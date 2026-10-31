#include "alchemy/darwin/media/avaudio.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/host/speech.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// AVFAudio speech synthesis over the host's voices (host::speech):
// - AVSpeechSynthesisVoice: +speechVoices lists the host's voices (identifier, name, BCP 47
//   language, gender; quality default); +voiceWithLanguage: picks the first voice of the language
//   (then of its base language; nil: the current one's); +voiceWithIdentifier:,
//   +currentLanguageCode.
// - AVSpeechSynthesizer: -speakUtterance: queues an utterance, which is synthesized on a host thread
//   (its voice, rate, pitch multiplier and volume; the delays as silence) and played through the
//   default output device, resampled, to every channel. The delegate hears didStart,
//   willSpeakRangeOfSpeechString (as each word starts playing), didFinish, didPause, didContinue
//   and didCancel on the main queue. Pausing and stopping happen at once or at the next word
//   (AVSpeechBoundaryWord); stopping drops the queue. -writeUtterance:toBufferCallback: hands over
//   the speech as float32 mono AVAudioPCMBuffers at the voice's rate, then an empty one.
// - The utterance's rate (0 to 1, normal at 0.5) is half to three times the voice's speed.
// - Unavailable, as documented: without voices (or without an output device for speaking) an
//   utterance is cancelled (didCancel) instead of spoken, and a notice is printed once.
namespace alchemy::gui::completion_kit::media::avaudio {
namespace {

struct VoiceState final : ObjectPayload { host::speech::Voice voice; };

struct Utterance {
    Address object{};                  // retained AVSpeechUtterance
    std::u16string text;
    std::string voice;                 // host identifier; empty: the default
    host::speech::Options options;
    double before{}, after{};          // seconds of silence
};

struct Synth final : ObjectPayload {
    std::mutex mutex;
    Address delegate{};                // not retained
    std::deque<Utterance> queue;       // queue.front() is speaking (or being synthesized)
    bool speaking{}, paused{}, synthesizing{};
    std::int64_t pause_at_word{-1}, stop_at_word{-1};   // the boundary asked for, as a frame
    // The speech playing.
    std::vector<float> samples;
    std::uint32_t rate{};
    std::vector<host::speech::Word> words;
    std::size_t next_word{};
    double position{};                 // in speech frames
    std::shared_ptr<hal_io::DeviceIo> io;
    std::unique_ptr<Resampler> resampler;
    std::uint64_t generation{};        // bumped when the playing utterance ends
};

// AVSpeechUtterance rate (0..1, 0.5 normal) as a speed multiple.
double speed_of(double rate) { return rate <= 0.5 ? 0.5 + std::max(rate, 0.0) : 1 + (std::min(rate, 1.0) - 0.5) * 4; }

class Speech : public std::enable_shared_from_this<Speech> {
public:
    explicit Speech(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    const std::vector<host::speech::Voice>& voices();
    Address make_voice(const host::speech::Voice& voice);   // +1
    std::optional<host::speech::Voice> voice_for_language(std::string language);
    std::shared_ptr<Synth> synth(Address self) { return support_.objc.host_state<Synth>(self); }
    Utterance read_utterance(Address utterance);
    void speak_next(Address self, const std::shared_ptr<Synth>& s);
    void begin(Address self, const std::shared_ptr<Synth>& s, std::uint64_t generation, host::Result<host::speech::Speech> speech);
    void render(Address self, const std::shared_ptr<Synth>& s, float* mix, std::uint32_t frames, std::uint32_t channels, double device_rate);
    // Ends the current utterance: `callback` names the delegate method (didFinish / didCancel).
    void end(Address self, const std::shared_ptr<Synth>& s, const char* callback);
    void tell(Address self, Address delegate, const char* selector, Address utterance);
    void notice(const std::string& text) {
        std::lock_guard lock(mutex_);
        if (!noticed_) std::cerr << "alchemy: " << text << std::endl;
        noticed_ = true;
    }

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    std::mutex mutex_;
    std::optional<std::vector<host::speech::Voice>> voices_;
    bool noticed_{};
    Address voice_class_{};
};

const std::vector<host::speech::Voice>& Speech::voices() {
    std::lock_guard lock(mutex_);
    if (!voices_) {
        auto listed = host::speech::voices();
        voices_ = listed ? std::move(*listed) : std::vector<host::speech::Voice>{};
    }
    return *voices_;
}

Address Speech::make_voice(const host::speech::Voice& voice) {
    const auto object = support_.send_id(support_.send_id(voice_class_, "alloc"), "init");
    support_.objc.host_state<VoiceState>(object)->voice = voice;
    return object;
}

std::optional<host::speech::Voice> Speech::voice_for_language(std::string language) {
    std::replace(language.begin(), language.end(), '_', '-');
    const auto& all = voices();
    for (const auto& v : all)
        if (v.language == language) return v;
    const auto base = language.substr(0, language.find('-'));
    for (const auto& v : all)
        if (v.language.substr(0, v.language.find('-')) == base) return v;
    return std::nullopt;
}

Utterance Speech::read_utterance(Address utterance) {
    Utterance u;
    u.object = support_.retain(utterance);
    const auto text = support_.send_id(utterance, "speechString");
    if (text) {
        const auto units = support_.cf.units(text);
        u.text.assign(units.begin(), units.end());
    }
    if (const auto voice = support_.send_id(utterance, "voice"))
        if (const auto v = support_.objc.find_host_state<VoiceState>(voice)) u.voice = v->voice.identifier;
    const auto real32 = [&](const char* selector) { return static_cast<double>(support_.send(utterance, selector).s(0)); };
    const auto real64 = [&](const char* selector) { return support_.send(utterance, selector).d(0); };
    u.options.rate = speed_of(real32("rate"));
    u.options.pitch = real32("pitchMultiplier");
    u.options.volume = real32("volume");
    u.before = std::max(0.0, real64("preUtteranceDelay"));
    u.after = std::max(0.0, real64("postUtteranceDelay"));
    return u;
}

void Speech::tell(Address self, Address delegate, const char* selector, Address utterance) {
    if (!delegate) return;
    CallRegs r{};
    r.x[2] = self;
    r.x[3] = utterance;
    static_cast<void>(support_.objc.send_if_responds(delegate, support_.objc.selector(selector), r));
}

// Synthesizes the front of the queue on a host thread, then plays it from the main queue.
void Speech::speak_next(Address self, const std::shared_ptr<Synth>& s) {
    Utterance u;
    std::uint64_t generation{};
    {
        std::lock_guard lock(s->mutex);
        if (s->speaking || s->synthesizing || s->queue.empty()) return;
        s->synthesizing = true;
        u = s->queue.front();
        generation = s->generation;
    }
    const auto owner = shared_from_this();
    std::thread([owner, self, s, u, generation] {
        auto speech = u.text.empty() ? host::Result<host::speech::Speech>(host::speech::Speech{22050, {}, {}})
                                     : host::speech::synthesize(u.voice, u.text, u.options);
        if (speech) {
            // The delays as silence around the speech; word frames move with it.
            const auto before = static_cast<std::size_t>(u.before * speech->rate), after = static_cast<std::size_t>(u.after * speech->rate);
            speech->samples.insert(speech->samples.begin(), before, 0);
            speech->samples.insert(speech->samples.end(), after, 0);
            for (auto& w : speech->words) w.frame += before;
        }
        owner->bridge_->dispatch_async(owner->bridge_->main_queue(),
                                       [owner, self, s, generation, speech = std::move(speech)]() mutable { owner->begin(self, s, generation, std::move(speech)); });
    }).detach();
}

void Speech::begin(Address self, const std::shared_ptr<Synth>& s, std::uint64_t generation, host::Result<host::speech::Speech> speech) {
    Address utterance{}, delegate{};
    {
        std::lock_guard lock(s->mutex);
        s->synthesizing = false;
        if (generation != s->generation || s->queue.empty()) return;   // stopped meanwhile
        utterance = s->queue.front().object;
        delegate = s->delegate;
    }
    const auto io = speech ? hal_device_io(hal_default_output()) : nullptr;
    if (!speech || !io) {
        notice(!speech ? "no speech voice is available on this host: AVSpeechSynthesizer utterances are cancelled"
                       : "no audio output device: AVSpeechSynthesizer utterances are cancelled");
        return end(self, s, "speechSynthesizer:didCancelSpeechUtterance:");
    }
    {
        std::lock_guard lock(s->mutex);
        s->samples.assign(speech->samples.size(), 0.0f);
        for (std::size_t i = 0; i < s->samples.size(); ++i) s->samples[i] = speech->samples[i] / 32768.0f;
        s->rate = speech->rate;
        s->words = std::move(speech->words);
        s->next_word = 0;
        s->position = 0;
        s->speaking = true;
        s->paused = false;
        s->pause_at_word = s->stop_at_word = -1;
        s->io = io;
        s->resampler.reset();
    }
    tell(self, delegate, "speechSynthesizer:didStartSpeechUtterance:", utterance);
    const auto owner = shared_from_this();
    const auto channels = io->channels();
    const auto rate = io->rate();
    if (io->start(self, [owner, self, s, channels, rate](float* mix, std::uint32_t frames, const hal_io::Timing&) {
            owner->render(self, s, mix, frames, channels, rate);
        }, hal_buffer_frames(hal_default_output())) != 0) {
        notice("the audio output device did not start: AVSpeechSynthesizer utterances are cancelled");
        end(self, s, "speechSynthesizer:didCancelSpeechUtterance:");
    }
}

void Speech::render(Address self, const std::shared_ptr<Synth>& s, float* mix, std::uint32_t frames, std::uint32_t channels, double device_rate) {
    std::vector<std::pair<std::size_t, std::size_t>> started;   // words that began in this buffer
    Address utterance{}, delegate{};
    bool finished = false, paused_now = false, cancelled = false;
    std::uint64_t generation{};
    {
        std::lock_guard lock(s->mutex);
        if (!s->speaking || s->paused || s->queue.empty()) return;
        utterance = s->queue.front().object;
        delegate = s->delegate;
        generation = s->generation;
        if (!s->resampler) s->resampler = std::make_unique<Resampler>(s->rate, device_rate, 1);
        const double total = static_cast<double>(s->samples.size());
        // A boundary asked for: the frame where the next word starts.
        const auto boundary = s->next_word < s->words.size() ? static_cast<double>(s->words[s->next_word].frame) : total;
        bool hit_boundary = false;
        std::vector<double> out(frames, 0.0);
        s->resampler->render(out.data(), frames, [&](double* into, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                const bool waiting = (s->pause_at_word >= 0 || s->stop_at_word >= 0) && s->position >= boundary;
                if (waiting) hit_boundary = true;
                into[i] = !waiting && s->position < total ? s->samples[static_cast<std::size_t>(s->position)] : 0.0;
                if (!waiting && s->position < total) s->position += 1;
            }
        });
        for (std::uint32_t f = 0; f < frames; ++f)
            for (std::uint32_t c = 0; c < channels; ++c) mix[static_cast<std::size_t>(f) * channels + c] += static_cast<float>(out[f]);
        while (s->next_word < s->words.size() && s->words[s->next_word].frame <= s->position) {
            started.emplace_back(s->words[s->next_word].offset, s->words[s->next_word].length);
            ++s->next_word;
        }
        if (hit_boundary && s->stop_at_word >= 0) finished = cancelled = true;
        else if (hit_boundary && s->pause_at_word >= 0) {
            s->paused = true;
            s->pause_at_word = -1;
            paused_now = true;
        } else if (s->position >= total) {
            finished = true;
        }
    }
    const auto owner = shared_from_this();
    if (!started.empty() || paused_now || finished) {
        bridge_->dispatch_async(bridge_->main_queue(), [owner, self, s, utterance, delegate, started, paused_now, finished, cancelled, generation] {
            for (const auto& [offset, length] : started) {
                if (!delegate) break;
                CallRegs r{};
                r.x[2] = self;
                r.x[3] = offset;
                r.x[4] = length;
                r.x[5] = utterance;
                static_cast<void>(owner->support_.objc.send_if_responds(delegate, owner->support_.objc.selector("speechSynthesizer:willSpeakRangeOfSpeechString:utterance:"), r));
            }
            if (paused_now) owner->tell(self, delegate, "speechSynthesizer:didPauseSpeechUtterance:", utterance);
            if (finished) {
                {
                    std::lock_guard lock(s->mutex);
                    if (generation != s->generation) return;
                }
                owner->end(self, s, cancelled ? "speechSynthesizer:didCancelSpeechUtterance:" : "speechSynthesizer:didFinishSpeechUtterance:");
            }
        });
    }
}

void Speech::end(Address self, const std::shared_ptr<Synth>& s, const char* callback) {
    std::shared_ptr<hal_io::DeviceIo> io;
    Address utterance{}, delegate{};
    bool drop_queue{};
    {
        std::lock_guard lock(s->mutex);
        if (s->queue.empty()) return;
        utterance = s->queue.front().object;
        s->queue.pop_front();
        delegate = s->delegate;
        drop_queue = s->stop_at_word >= 0;
        s->speaking = s->paused = false;
        s->pause_at_word = s->stop_at_word = -1;
        s->samples.clear();
        s->words.clear();
        ++s->generation;
        io = std::exchange(s->io, nullptr);
    }
    if (io) io->stop(self);
    tell(self, delegate, callback, utterance);
    support_.release(utterance);
    if (drop_queue) {
        std::deque<Utterance> dropped;
        {
            std::lock_guard lock(s->mutex);
            dropped.swap(s->queue);
        }
        for (const auto& u : dropped) support_.release(u.object);
        return;
    }
    speak_next(self, s);
}

void Speech::install() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    voice_class_ = declare(support, "AVSpeechSynthesisVoice", "NSObject");
    const auto synth_class = declare(support, "AVSpeechSynthesizer", "NSObject");

    // ---- AVSpeechSynthesisVoice --------------------------------------------------------------
    const auto voice_of = [sp](Address object) {
        const auto v = sp->objc.find_host_state<VoiceState>(object);
        if (!v) unsupported("an AVSpeechSynthesisVoice Alchemy did not make");
        return v;
    };
    const auto text = [sp](const std::string& value) {
        const auto string = sp->cf.string(value);
        return sp->send_id(string, "autorelease");
    };
    support.method(voice_class_, "speechVoices", [self, sp](CpuState& s, GuestMemory&) {
        std::vector<Address> items;
        for (const auto& v : self->voices()) items.push_back(self->make_voice(v));
        const auto array = self->bridge_->array(items);
        for (const auto item : items) sp->release(item);
        s.x[0] = sp->send_id(array, "autorelease");
    }, "@@:", true);
    support.method(voice_class_, "currentLanguageCode", [self, sp, text](CpuState& s, GuestMemory&) {
        auto code = sp->cf.utf8(sp->send_id(sp->cf.current_locale(), "localeIdentifier"));
        std::replace(code.begin(), code.end(), '_', '-');
        s.x[0] = text(code.empty() ? "en-US" : code);
    }, "@@:", true);
    support.method(voice_class_, "voiceWithLanguage:", [self, sp](CpuState& s, GuestMemory&) {
        std::string language = s.x[2] ? sp->cf.utf8(s.x[2]) : sp->cf.utf8(sp->send_id(sp->cf.current_locale(), "localeIdentifier"));
        auto found = self->voice_for_language(language);
        if (!found && !s.x[2] && !self->voices().empty()) found = self->voices().front();
        s.x[0] = found ? sp->send_id(self->make_voice(*found), "autorelease") : 0;
    }, "@@:@", true);
    support.method(voice_class_, "voiceWithIdentifier:", [self, sp](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string{};
        s.x[0] = 0;
        for (const auto& v : self->voices())
            if (v.identifier == wanted) { s.x[0] = sp->send_id(self->make_voice(v), "autorelease"); break; }
    }, "@@:@", true);
    support.method(voice_class_, "identifier", [voice_of, text](CpuState& s, GuestMemory&) { s.x[0] = text(voice_of(s.x[0])->voice.identifier); }, "@@:");
    support.method(voice_class_, "name", [voice_of, text](CpuState& s, GuestMemory&) { s.x[0] = text(voice_of(s.x[0])->voice.name); }, "@@:");
    support.method(voice_class_, "language", [voice_of, text](CpuState& s, GuestMemory&) { s.x[0] = text(voice_of(s.x[0])->voice.language); }, "@@:");
    // AVSpeechSynthesisVoiceGender: unspecified 0, male 1, female 2; AVSpeechSynthesisVoiceQualityDefault 1.
    support.method(voice_class_, "gender", [voice_of](CpuState& s, GuestMemory&) {
        const auto g = voice_of(s.x[0])->voice.gender;
        s.x[0] = g == host::speech::Voice::Gender::male ? 1 : g == host::speech::Voice::Gender::female ? 2 : 0;
    }, "q@:");
    support.method(voice_class_, "quality", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "q@:");
    support.method(voice_class_, "audioFileSettings", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->send_id(sp->objc.find_class("NSDictionary"), "dictionary"), "self");
    }, "@@:");
    support.method(voice_class_, "isEqual:", [sp](CpuState& s, GuestMemory&) {
        const auto a = sp->objc.find_host_state<VoiceState>(s.x[0]), b = s.x[2] ? sp->objc.find_host_state<VoiceState>(s.x[2]) : nullptr;
        s.x[0] = a && b && a->voice.identifier == b->voice.identifier ? 1 : 0;
    }, "B@:@");

    // ---- AVSpeechSynthesizer -----------------------------------------------------------------
    support.method(synth_class, "delegate", [self](CpuState& s, GuestMemory&) {
        const auto st = self->synth(s.x[0]);
        std::lock_guard lock(st->mutex);
        s.x[0] = st->delegate;
    }, "@@:");
    support.method(synth_class, "setDelegate:", [self](CpuState& s, GuestMemory&) {
        const auto st = self->synth(s.x[0]);
        std::lock_guard lock(st->mutex);
        st->delegate = s.x[2];
    }, "v@:@");
    support.method(synth_class, "speakUtterance:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) unsupported("-[AVSpeechSynthesizer speakUtterance:] with nil (an exception on macOS)");
        const auto st = self->synth(s.x[0]);
        auto u = self->read_utterance(s.x[2]);
        {
            std::lock_guard lock(st->mutex);
            st->queue.push_back(std::move(u));
        }
        self->speak_next(s.x[0], st);
    }, "v@:@");
    support.method(synth_class, "isSpeaking", [self](CpuState& s, GuestMemory&) {
        const auto st = self->synth(s.x[0]);
        std::lock_guard lock(st->mutex);
        s.x[0] = st->speaking || st->synthesizing || !st->queue.empty() ? 1 : 0;
    }, "B@:");
    support.method(synth_class, "isPaused", [self](CpuState& s, GuestMemory&) {
        const auto st = self->synth(s.x[0]);
        std::lock_guard lock(st->mutex);
        s.x[0] = st->paused ? 1 : 0;
    }, "B@:");
    // AVSpeechBoundary: immediate 0, word 1.
    support.method(synth_class, "stopSpeakingAtBoundary:", [self](CpuState& s, GuestMemory&) {
        const auto st = self->synth(s.x[0]);
        bool now{}, any{};
        {
            std::lock_guard lock(st->mutex);
            any = !st->queue.empty();
            if (!any) { s.x[0] = 0; return; }
            st->stop_at_word = 0;
            now = s.x[2] == 0 || !st->speaking || st->paused;
            if (!st->speaking) ++st->generation;   // a synthesis in flight is dropped
        }
        if (now) self->end(s.x[0], st, "speechSynthesizer:didCancelSpeechUtterance:");
        s.x[0] = 1;
    }, "B@:q");
    support.method(synth_class, "pauseSpeakingAtBoundary:", [self](CpuState& s, GuestMemory&) {
        const auto st = self->synth(s.x[0]);
        Address utterance{}, delegate{};
        {
            std::lock_guard lock(st->mutex);
            if (!st->speaking || st->paused) { s.x[0] = 0; return; }
            if (s.x[2] != 0) { st->pause_at_word = 0; s.x[0] = 1; return; }
            st->paused = true;
            utterance = st->queue.front().object;
            delegate = st->delegate;
        }
        self->tell(s.x[0], delegate, "speechSynthesizer:didPauseSpeechUtterance:", utterance);
        s.x[0] = 1;
    }, "B@:q");
    support.method(synth_class, "continueSpeaking", [self](CpuState& s, GuestMemory&) {
        const auto st = self->synth(s.x[0]);
        Address utterance{}, delegate{};
        {
            std::lock_guard lock(st->mutex);
            if (!st->paused) { s.x[0] = 0; return; }
            st->paused = false;
            utterance = st->queue.front().object;
            delegate = st->delegate;
        }
        self->tell(s.x[0], delegate, "speechSynthesizer:didContinueSpeechUtterance:", utterance);
        s.x[0] = 1;
    }, "B@:");
    // -writeUtterance:toBufferCallback: (utterance x2, block x3): float32 mono buffers at the
    // speech's rate, then an empty buffer, delivered on the main queue.
    support.method(synth_class, "writeUtterance:toBufferCallback:", [self, sp](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) unsupported("-[AVSpeechSynthesizer writeUtterance:toBufferCallback:] with nil");
        auto u = self->read_utterance(s.x[2]);
        const auto block = self->bridge_->block_copy(s.x[3]);
        const auto owner = self;
        std::thread([owner, sp, u = std::move(u), block] {
            auto speech = host::speech::synthesize(u.voice, u.text, u.options);
            owner->bridge_->dispatch_async(owner->bridge_->main_queue(), [owner, sp, u, block, speech = std::move(speech)] {
                if (!speech) owner->notice("no speech voice is available on this host: AVSpeechSynthesizer writes only an empty buffer");
                const auto rate = speech ? speech->rate : 22050U;
                const auto format = standard_format(*sp, rate, 1);
                const std::size_t total = speech ? speech->samples.size() : 0;
                for (std::size_t at = 0;; at += 4096) {
                    const auto count = static_cast<std::uint32_t>(at < total ? std::min<std::size_t>(4096, total - at) : 0);
                    const auto buffer = make_buffer(*sp, format, std::max(count, 1U));
                    if (const auto b = sp->objc.find_host_state<PcmBuffer>(buffer)) {
                        std::vector<double> samples(count);
                        for (std::uint32_t i = 0; i < count; ++i) samples[i] = speech->samples[at + i] / 32768.0;
                        fill_buffer(*sp, *b, samples, 1, count);
                    }
                    static_cast<void>(owner->bridge_->invoke_block(block, {buffer}));
                    sp->release(buffer);
                    if (count == 0) break;
                }
                sp->release(format);
                owner->bridge_->block_release(block);
                sp->release(u.object);
            });
        }).detach();
    }, "v@:@@?");
}

} // namespace

void install_avaudio_speech(Support& support) { std::make_shared<Speech>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media::avaudio
