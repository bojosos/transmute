// Win32 host backend, area "ui": spell checking (include/alchemy/host/ui.hpp)
// over the Windows Spell Checking API (ISpellCheckerFactory, Windows 8 and
// later), on a dedicated COM (multithreaded apartment) host thread that owns
// the factory and one ISpellChecker per language.
#include "alchemy/host/ui.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <objbase.h>
#include <spellcheck.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#pragma comment(lib, "ole32.lib")

namespace alchemy::host::ui {
namespace {

std::wstring wide(std::u16string_view text) { return {text.begin(), text.end()}; }
std::u16string utf16(const wchar_t* text) {
    const std::wstring_view view(text);
    return {view.begin(), view.end()};
}

// The COM thread and the Windows spell checkers (process lifetime).
class Engine {
public:
    static Engine& instance() {
        static auto* engine = new Engine();   // never destroyed: the thread outlives static destruction
        return *engine;
    }

    std::vector<std::string> languages() {
        return run<std::vector<std::string>>([this] {
            std::vector<std::string> result;
            if (!factory) return result;
            IEnumString* list{};
            if (FAILED(factory->get_SupportedLanguages(&list)) || !list) return result;
            LPOLESTR item{};
            while (list->Next(1, &item, nullptr) == S_OK) {
                result.push_back(win32::narrow(item));
                CoTaskMemFree(item);
            }
            list->Release();
            return result;
        });
    }

    std::vector<Misspelling> check(const std::wstring& language, const std::wstring& text) {
        return run<std::vector<Misspelling>>([this, &language, &text] {
            std::vector<Misspelling> result;
            auto* checker = checker_for(language);
            if (!checker || text.empty()) return result;
            IEnumSpellingError* errors{};
            if (FAILED(checker->Check(text.c_str(), &errors)) || !errors) return result;
            ISpellingError* error{};
            while (errors->Next(&error) == S_OK) {
                ULONG start{}, length{};
                CORRECTIVE_ACTION action{};
                error->get_StartIndex(&start);
                error->get_Length(&length);
                error->get_CorrectiveAction(&action);
                if (action == CORRECTIVE_ACTION_GET_SUGGESTIONS || action == CORRECTIVE_ACTION_REPLACE) {
                    Misspelling entry{start, length, action == CORRECTIVE_ACTION_REPLACE, {}};
                    LPWSTR replacement{};
                    if (entry.replace && SUCCEEDED(error->get_Replacement(&replacement)) && replacement) {
                        entry.replacement = utf16(replacement);
                        CoTaskMemFree(replacement);
                    }
                    result.push_back(std::move(entry));
                }
                error->Release();
            }
            errors->Release();
            return result;
        });
    }

    std::vector<std::u16string> suggest(const std::wstring& language, const std::wstring& word) {
        return run<std::vector<std::u16string>>([this, &language, &word] {
            std::vector<std::u16string> result;
            auto* checker = checker_for(language);
            if (!checker || word.empty()) return result;
            IEnumString* list{};
            if (FAILED(checker->Suggest(word.c_str(), &list)) || !list) return result;
            LPOLESTR item{};
            while (list->Next(1, &item, nullptr) == S_OK) {
                result.push_back(utf16(item));
                CoTaskMemFree(item);
            }
            list->Release();
            return result;
        });
    }

private:
    Engine() {
        std::thread([this] { loop(); }).detach();
    }

    template <class Result>
    Result run(std::function<Result()> work) {
        std::packaged_task<Result()> task(std::move(work));
        auto future = task.get_future();
        {
            std::lock_guard lock(mutex);
            queue.emplace_back([&task] { task(); });
        }
        ready.notify_one();
        return future.get();
    }

    void loop() {
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(initialized) &&
            FAILED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            factory = nullptr;
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] { return !queue.empty(); });
                work = std::move(queue.front());
                queue.pop_front();
            }
            work();
        }
    }

    ISpellChecker* checker_for(const std::wstring& language) {
        if (!factory || language.empty()) return nullptr;
        if (const auto found = checkers.find(language); found != checkers.end()) return found->second;
        BOOL supported{};
        ISpellChecker* checker{};
        if (FAILED(factory->IsSupported(language.c_str(), &supported)) || !supported ||
            FAILED(factory->CreateSpellChecker(language.c_str(), &checker)))
            checker = nullptr;
        checkers[language] = checker;
        return checker;
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> queue;
    ISpellCheckerFactory* factory{};                  // COM thread only
    std::map<std::wstring, ISpellChecker*> checkers;   // COM thread only
};

} // namespace

Result<std::vector<std::string>> spelling_languages() { return Engine::instance().languages(); }

Result<std::vector<Misspelling>> check_spelling(std::string_view language, std::u16string_view text) {
    return Engine::instance().check(win32::widen(language), wide(text));
}

Result<std::vector<std::u16string>> spelling_suggestions(std::string_view language, std::u16string_view word) {
    return Engine::instance().suggest(win32::widen(language), wide(word));
}

} // namespace alchemy::host::ui
