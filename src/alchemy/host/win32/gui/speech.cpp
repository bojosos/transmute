// Speech synthesis through SAPI 5 (the installed desktop voices): a voice speaks into a memory
// stream of 16-bit mono PCM at 22.05 kHz, with word-boundary events for the word times. The rate
// maps to SAPI's -10..10 (3x per 10 steps), the pitch to an XML <pitch absmiddle> around the
// escaped text (a factor of 2 per 10 steps), the volume to 0..100. COM runs on one host thread.

#include "alchemy/host/speech.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <objbase.h>
#include <sapi.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")

namespace alchemy::host::speech {
namespace {

constexpr std::uint32_t sample_rate = 22050;

template <class T> struct Com {
    T* p{};
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string string_value(ISpDataKey* key, const wchar_t* name) {
    wchar_t* value{};
    if (!key || FAILED(key->GetStringValue(name, &value)) || !value) return {};
    std::string out = win32::narrow(value);
    CoTaskMemFree(value);
    return out;
}

// A token's attributes: its language (hexadecimal LCIDs separated by ';', the first one) as BCP 47.
std::string language_of(ISpObjectToken* token) {
    Com<ISpDataKey> attributes;
    if (FAILED(token->OpenKey(L"Attributes", attributes.put()))) return {};
    const auto lcids = string_value(attributes.p, L"Language");
    if (lcids.empty()) return {};
    const auto lcid = static_cast<LCID>(std::strtoul(lcids.substr(0, lcids.find(';')).c_str(), nullptr, 16));
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (!LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0)) return {};
    return win32::narrow(name);
}

Voice::Gender gender_of(ISpObjectToken* token) {
    Com<ISpDataKey> attributes;
    if (FAILED(token->OpenKey(L"Attributes", attributes.put()))) return Voice::Gender::unspecified;
    const auto gender = string_value(attributes.p, L"Gender");
    if (gender == "Female") return Voice::Gender::female;
    if (gender == "Male") return Voice::Gender::male;
    return Voice::Gender::unspecified;
}

std::wstring escaped(std::u16string_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const auto c : text) {
        switch (c) {
        case u'&': out += L"&amp;"; break;
        case u'<': out += L"&lt;"; break;
        case u'>': out += L"&gt;"; break;
        default: out += static_cast<wchar_t>(c);
        }
    }
    return out;
}

class Engine {
public:
    static Engine& instance() {
        static Engine* engine = new Engine();   // lives for the process (its thread never ends)
        return *engine;
    }

    Result<std::vector<Voice>> list() {
        return run<Result<std::vector<Voice>>>([this]() -> Result<std::vector<Voice>> {
            if (!com_) return Error::unsupported;
            std::vector<Voice> out;
            for (auto* token : tokens()) {
                wchar_t* id{};
                Voice voice;
                if (SUCCEEDED(token->GetId(&id)) && id) {
                    voice.identifier = win32::narrow(id);
                    CoTaskMemFree(id);
                }
                voice.name = string_value(token, nullptr);
                voice.language = language_of(token);
                voice.gender = gender_of(token);
                token->Release();
                out.push_back(std::move(voice));
            }
            return out;
        });
    }

    Result<Speech> speak(std::string voice, std::u16string text, Options options) {
        return run<Result<Speech>>([&]() -> Result<Speech> {
            if (!com_) return Error::unsupported;
            Com<ISpVoice> speaker;
            if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(speaker.put())))) return Error::unsupported;
            if (!voice.empty()) {
                ISpObjectToken* chosen{};
                for (auto* token : tokens()) {
                    wchar_t* id{};
                    const bool match = SUCCEEDED(token->GetId(&id)) && id && win32::narrow(id) == voice;
                    if (id) CoTaskMemFree(id);
                    if (match && !chosen) chosen = token;
                    else token->Release();
                }
                if (!chosen) return Error::not_found;
                const auto set = speaker->SetVoice(chosen);
                chosen->Release();
                if (FAILED(set)) return Error::other;
            }
            // Output: a memory stream of 16-bit mono PCM.
            Com<IStream> memory;
            if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, memory.put()))) return Error::no_memory;
            Com<ISpStream> stream;
            if (FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(stream.put())))) return Error::unsupported;
            WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, sample_rate, sample_rate * 2, 2, 16, 0};
            if (FAILED(stream->SetBaseStream(memory.p, SPDFID_WaveFormatEx, &format))) return Error::other;
            if (FAILED(speaker->SetOutput(stream.p, FALSE))) return Error::other;   // keep this format
            const auto rate = static_cast<long>(std::lround(std::clamp(10 * std::log(std::clamp(options.rate, 1.0 / 3, 3.0)) / std::log(3.0), -10.0, 10.0)));
            speaker->SetRate(rate);
            speaker->SetVolume(static_cast<USHORT>(std::lround(std::clamp(options.volume, 0.0, 1.0) * 100)));
            const ULONGLONG interest = SPFEI(SPEI_WORD_BOUNDARY);
            speaker->SetInterest(interest, interest);
            const auto pitch = static_cast<int>(std::lround(std::clamp(10 * std::log2(std::clamp(options.pitch, 0.5, 2.0)), -10.0, 10.0)));
            const std::wstring prefix = L"<pitch absmiddle=\"" + std::to_wstring(pitch) + L"\">";
            const std::wstring xml = prefix + escaped(text) + L"</pitch>";
            if (FAILED(speaker->Speak(xml.c_str(), SPF_IS_XML, nullptr))) return Error::other;
            Speech out;
            out.rate = sample_rate;
            // Word boundaries: lParam is the position in the spoken string, wParam the length;
            // positions in the escaped text map back to the original by counting escapes.
            std::vector<std::size_t> original_at;   // escaped position -> original position
            for (std::size_t i = 0; i < text.size(); ++i) {
                const auto c = text[i];
                const std::size_t width = c == u'&' ? 5 : c == u'<' || c == u'>' ? 4 : 1;
                for (std::size_t k = 0; k < width; ++k) original_at.push_back(i);
            }
            original_at.push_back(text.size());
            SPEVENT event{};
            ULONG fetched{};
            while (SUCCEEDED(speaker->GetEvents(1, &event, &fetched)) && fetched == 1) {
                if (event.eEventId == SPEI_WORD_BOUNDARY) {
                    const auto at = static_cast<std::size_t>(event.lParam), length = static_cast<std::size_t>(event.wParam);
                    if (at >= prefix.size()) {
                        const auto start = std::min(at - prefix.size(), original_at.size() - 1);
                        const auto end = std::min(start + length, original_at.size() - 1);
                        out.words.push_back({original_at[start], original_at[end] - original_at[start], event.ullAudioStreamOffset / 2});
                    }
                }
                if (event.elParamType == SPET_LPARAM_IS_POINTER || event.elParamType == SPET_LPARAM_IS_STRING) CoTaskMemFree(reinterpret_cast<void*>(event.lParam));
                if (event.elParamType == SPET_LPARAM_IS_TOKEN || event.elParamType == SPET_LPARAM_IS_OBJECT) reinterpret_cast<IUnknown*>(event.lParam)->Release();
            }
            speaker->SetOutput(nullptr, TRUE);
            HGLOBAL global{};
            if (FAILED(GetHGlobalFromStream(memory.p, &global))) return Error::other;
            STATSTG stat{};
            if (FAILED(memory->Stat(&stat, STATFLAG_NONAME))) return Error::other;
            const auto bytes = static_cast<std::size_t>(stat.cbSize.QuadPart);
            if (const auto* data = static_cast<const std::int16_t*>(GlobalLock(global))) {
                out.samples.assign(data, data + bytes / 2);
                GlobalUnlock(global);
            }
            return out;
        });
    }

private:
    Engine() { std::thread([this] { loop(); }).detach(); }

    // The voice tokens (each +1).
    std::vector<ISpObjectToken*> tokens() {
        std::vector<ISpObjectToken*> out;
        Com<ISpObjectTokenCategory> category;
        if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(category.put()))) ||
            FAILED(category->SetId(SPCAT_VOICES, FALSE)))
            return out;
        Com<IEnumSpObjectTokens> list;
        if (FAILED(category->EnumTokens(nullptr, nullptr, list.put()))) return out;
        ISpObjectToken* token{};
        ULONG fetched{};
        while (SUCCEEDED(list->Next(1, &token, &fetched)) && fetched == 1) out.push_back(token);
        return out;
    }

    template <class R>
    R run(std::function<R()> work) {
        std::packaged_task<R()> task(std::move(work));
        auto future = task.get_future();
        {
            std::lock_guard lock(mutex_);
            queue_.emplace_back([&task] { task(); });
        }
        ready_.notify_one();
        return future.get();
    }

    void loop() {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return !queue_.empty(); });
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            work();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    bool com_{};
};

} // namespace

Result<std::vector<Voice>> voices() { return Engine::instance().list(); }

Result<Speech> synthesize(std::string_view voice, std::u16string_view text, const Options& options) {
    return Engine::instance().speak(std::string(voice), std::u16string(text), options);
}

} // namespace alchemy::host::speech
