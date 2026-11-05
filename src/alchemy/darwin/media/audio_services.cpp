#include "alchemy/darwin/media/audio_file.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// AudioToolbox AudioServices system sounds. AudioServicesCreateSystemSoundID decodes a sound file
// (anything AudioFile reads: linear PCM WAVE, AIFF and CAF, and the compressed streams of
// audio_stream.hpp) of up to 30 seconds. Each AudioServicesPlaySystemSound / PlayAlertSound plays
// it once on the default output device through the HAL's output (hal_io.hpp; silent in cloaked
// runs), resampled to the device and overlapping earlier plays. When a play ends, its block
// (...WithCompletion) and the sound's AudioServicesAddSystemSoundCompletion procedure run: on the
// main queue for the main run loop (or none), otherwise on a thread of the sounds' own.
// kSystemSoundID_UserPreferredAlert is the system alert (NSBeep, where AppKit is loaded);
// kSystemSoundID_FlashScreen flashes nothing here; both complete at once. A sound disposed while
// it plays stops without completing. kAudioServicesPropertyIsUISound and
// kAudioServicesPropertyCompletePlaybackIfAppDies are kept per sound (UI sounds always play).
namespace alchemy::gui::completion_kit::media {
namespace {

constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";

constexpr std::uint32_t four(const char (&code)[5]) { return audio_file::four(code); }
// <AudioServices.h>
constexpr std::int32_t unsupported_property = static_cast<std::int32_t>(four("pty?")), bad_property_size = static_cast<std::int32_t>(four("!siz")),
    bad_specifier_size = static_cast<std::int32_t>(four("spc?")), unspecified = -1500, exceeded_maximum_duration = -1502, param = -50;
constexpr std::uint32_t user_preferred_alert = 0x00001000, flash_screen = 0x00000FFE;
constexpr std::uint32_t prop_is_ui_sound = four("isui"), prop_complete_if_app_dies = four("ifdi");
constexpr double maximum_seconds = 30;

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

struct Sound {
    std::uint32_t id{};
    std::shared_ptr<const std::vector<float>> samples;   // interleaved
    double rate{};
    std::uint32_t channels{};
    std::uint64_t frames{};
    Address proc{}, client{};                            // AudioServicesAddSystemSoundCompletion
    bool main_loop{true};
    std::uint32_t ui_sound{1}, complete_if_app_dies{};
};

struct Play {
    std::shared_ptr<Sound> sound;
    std::uint64_t key{};                                 // its client key on the device (a guest address of its own)
    std::shared_ptr<hal_io::DeviceIo> io;
    std::unique_ptr<Resampler> resampler;
    std::uint64_t position{};
    Address block{};                                     // ...WithCompletion (copied)
    std::atomic<bool> done{}, cancelled{};
};

class SystemSounds : public std::enable_shared_from_this<SystemSounds> {
public:
    explicit SystemSounds(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    std::uint64_t call(Address function, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(support_.process.calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
    }
    std::int32_t create(const std::string& path, std::uint32_t& id);
    void play(std::uint32_t id, Address block);
    void render(const std::shared_ptr<Play>& p, float* mix, std::uint32_t frames, std::uint32_t channels, const hal_io::Timing& timing);
    // The play's completion (block, then the sound's procedure) where the sound's completions run.
    void complete(const std::shared_ptr<Play>& p);
    void complete_now(std::uint32_t id, Address block);   // a sound that does not play (the alert, the flash)

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    std::mutex mutex_;
    std::map<std::uint32_t, std::shared_ptr<Sound>> sounds_;
    std::map<std::uint64_t, std::shared_ptr<Play>> plays_;
    std::uint32_t next_id_{0x2001};
    std::shared_ptr<CompletionThread> thread_;
};

std::int32_t SystemSounds::create(const std::string& path, std::uint32_t& id) {
    GuestAudio audio;
    if (const auto result = open_guest_audio(support_, path, audio)) return result;
    const auto& format = audio.container.format;
    const auto frames = audio.frames();
    if (!(format.rate > 0) || static_cast<double>(frames) / format.rate > maximum_seconds) {
        close_guest_audio(support_, audio);
        return exceeded_maximum_duration;
    }
    std::vector<double> decoded;
    const auto got = read_audio_frames(support_, audio, 0, static_cast<std::size_t>(frames), decoded);
    close_guest_audio(support_, audio);
    auto sound = std::make_shared<Sound>();
    sound->samples = std::make_shared<const std::vector<float>>(decoded.begin(), decoded.end());
    sound->rate = format.rate;
    sound->channels = std::max(format.channels, 1U);
    sound->frames = got;
    std::lock_guard lock(mutex_);
    sound->id = next_id_++;
    id = sound->id;
    sounds_[id] = std::move(sound);
    return 0;
}

void SystemSounds::play(std::uint32_t id, Address block) {
    std::shared_ptr<Sound> sound;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = sounds_.find(id); found != sounds_.end()) sound = found->second;
    }
    if (!sound) {
        if (id == user_preferred_alert) {
            if (const auto beep = support_.process.resolve(appkit, "_NSBeep")) static_cast<void>(call(beep->address, {}));
        }
        if (id == user_preferred_alert || id == flash_screen) complete_now(id, block);
        return;   // an unknown sound plays nothing
    }
    auto p = std::make_shared<Play>();
    p->sound = sound;
    p->block = block ? bridge_->block_copy(block) : 0;
    const auto device = hal_default_output();
    p->io = hal_device_io(device);
    if (!p->io) {   // no output device: the play ends at once
        complete(p);
        return;
    }
    p->key = support_.process.allocate_guest(16, 16);
    {
        std::lock_guard lock(mutex_);
        plays_[p->key] = p;
    }
    auto self = shared_from_this();
    const auto channels = p->io->channels();
    const auto result = p->io->start(p->key, [self, p, channels](float* mix, std::uint32_t frames, const hal_io::Timing& timing) {
        self->render(p, mix, frames, channels, timing);
    }, hal_buffer_frames(device));
    if (result) {
        {
            std::lock_guard lock(mutex_);
            plays_.erase(p->key);
        }
        complete(p);
    }
}

void SystemSounds::render(const std::shared_ptr<Play>& p, float* mix, std::uint32_t frames, std::uint32_t channels, const hal_io::Timing& timing) {
    if (p->done) return;
    const auto& sound = *p->sound;
    if (!p->resampler) p->resampler = std::make_unique<Resampler>(sound.rate, timing.rate > 0 ? timing.rate : sound.rate, channels);
    const auto& samples = *sound.samples;
    // A little silence after the sound lets the resampler give out its last frames.
    const auto end = sound.frames + 64;
    const auto pull = [&](double* out, std::size_t count) {
        for (std::size_t f = 0; f < count; ++f, ++p->position)
            for (std::uint32_t c = 0; c < channels; ++c) {
                const auto source = sound.channels == 1 ? 0U : c;
                out[f * channels + c] = p->position < sound.frames && source < sound.channels
                    ? samples[static_cast<std::size_t>(p->position) * sound.channels + source] : 0.0;
            }
    };
    std::vector<double> out(static_cast<std::size_t>(frames) * channels);
    p->resampler->render(out.data(), frames, pull);
    for (std::size_t i = 0; i < out.size(); ++i) mix[i] += static_cast<float>(out[i]);
    if (p->position < end) return;
    p->done = true;
    p->io->stop(p->key);   // from the I/O thread's render, as the device allows
    bool mine{};   // a dispose may have taken it first
    {
        std::lock_guard lock(mutex_);
        mine = plays_.erase(p->key) > 0;
    }
    if (mine) complete(p);
}

void SystemSounds::complete(const std::shared_ptr<Play>& p) {
    auto self = shared_from_this();
    const auto run = [self, p] {
        if (p->key) self->support_.process.release_guest(std::exchange(p->key, 0));
        const auto block = std::exchange(p->block, 0);
        if (p->cancelled) {
            if (block) self->bridge_->block_release(block);
            return;
        }
        if (block) {
            static_cast<void>(self->bridge_->invoke_block(block, {}));
            self->bridge_->block_release(block);
        }
        Address proc{}, client{};
        {
            std::lock_guard lock(self->mutex_);
            proc = p->sound->proc;
            client = p->sound->client;
        }
        // AudioServicesSystemSoundCompletionProc(SystemSoundID, void *clientData)
        if (proc) static_cast<void>(self->call(proc, {p->sound->id, client}));
    };
    bool main_loop{};
    {
        std::lock_guard lock(mutex_);
        main_loop = p->sound->main_loop;
        if (!main_loop && !thread_) thread_ = start_completion_thread(support_.process, "com.apple.audio.SystemSounds");
    }
    if (main_loop) bridge_->dispatch_async(bridge_->main_queue(), run);
    else thread_->post(run);
}

void SystemSounds::complete_now(std::uint32_t id, Address block) {
    if (!block) return;
    auto self = shared_from_this();
    const auto copy = bridge_->block_copy(block);
    bridge_->dispatch_async(bridge_->main_queue(), [self, copy, id] {
        static_cast<void>(id);
        static_cast<void>(self->bridge_->invoke_block(copy, {}));
        self->bridge_->block_release(copy);
    });
}

void SystemSounds::install() {
    auto self = shared_from_this();
    // AudioServicesCreateSystemSoundID(CFURLRef, SystemSoundID *out)
    support_.function(audio_toolbox, "_AudioServicesCreateSystemSoundID", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) return status(s, param);
        std::uint32_t id{};
        const auto result = self->create(self->support_.cf.url_path(s.x[0]), id);
        if (!result) m.write32(s.x[1], id);
        status(s, result);
    });
    // AudioServicesDisposeSystemSoundID(SystemSoundID): its plays stop without completing.
    support_.function(audio_toolbox, "_AudioServicesDisposeSystemSoundID", [self](CpuState& s, GuestMemory&) {
        const auto id = static_cast<std::uint32_t>(s.x[0]);
        std::vector<std::shared_ptr<Play>> stopping;
        {
            std::lock_guard lock(self->mutex_);
            if (!self->sounds_.erase(id)) return status(s, unspecified);
            for (auto it = self->plays_.begin(); it != self->plays_.end();) {
                if (it->second->sound->id == id) {
                    stopping.push_back(it->second);
                    it = self->plays_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& p : stopping) {
            p->cancelled = true;
            p->done = true;
            p->io->stop(p->key);
            self->complete(p);   // releases its key and block
        }
        status(s, 0);
    });
    for (const auto* name : {"_AudioServicesPlaySystemSound", "_AudioServicesPlayAlertSound"})
        support_.function(audio_toolbox, name, [self](CpuState& s, GuestMemory&) { self->play(static_cast<std::uint32_t>(s.x[0]), 0); });
    // ...WithCompletion(SystemSoundID, void (^)(void))
    for (const auto* name : {"_AudioServicesPlaySystemSoundWithCompletion", "_AudioServicesPlayAlertSoundWithCompletion"})
        support_.function(audio_toolbox, name, [self](CpuState& s, GuestMemory&) { self->play(static_cast<std::uint32_t>(s.x[0]), s.x[1]); });
    // AudioServicesAddSystemSoundCompletion(SystemSoundID, CFRunLoopRef, CFStringRef mode, proc, void *clientData)
    support_.function(audio_toolbox, "_AudioServicesAddSystemSoundCompletion", [self](CpuState& s, GuestMemory&) {
        if (!s.x[3]) return status(s, param);
        const bool main_loop = !s.x[1] || s.x[1] == self->support_.call(core_foundation, "_CFRunLoopGetMain", {});
        std::lock_guard lock(self->mutex_);
        const auto found = self->sounds_.find(static_cast<std::uint32_t>(s.x[0]));
        if (found == self->sounds_.end()) return status(s, unspecified);
        found->second->proc = s.x[3];
        found->second->client = s.x[4];
        found->second->main_loop = main_loop;
        status(s, 0);
    });
    support_.function(audio_toolbox, "_AudioServicesRemoveSystemSoundCompletion", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        if (const auto found = self->sounds_.find(static_cast<std::uint32_t>(s.x[0])); found != self->sounds_.end()) {
            found->second->proc = 0;
            found->second->client = 0;
        }
    });
    // The per-sound properties: (propertyID, specifierSize, specifier = SystemSoundID *, ...).
    const auto sound_property = [self](std::uint32_t id, std::uint32_t specifier_size, Address specifier, GuestMemory& m,
                                       std::uint32_t*& value) -> std::int32_t {
        if (id != prop_is_ui_sound && id != prop_complete_if_app_dies) return unsupported_property;
        if (specifier_size < 4 || !specifier) return bad_specifier_size;
        const auto found = self->sounds_.find(m.read32(specifier));
        if (found == self->sounds_.end()) return unspecified;
        value = id == prop_is_ui_sound ? &found->second->ui_sound : &found->second->complete_if_app_dies;
        return 0;
    };
    // AudioServicesGetPropertyInfo(id, specifierSize, specifier, UInt32 *outSize, Boolean *outWritable)
    support_.function(audio_toolbox, "_AudioServicesGetPropertyInfo", [self, sound_property](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        std::uint32_t* value{};
        if (const auto result = sound_property(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), s.x[2], m, value))
            return status(s, result);
        if (s.x[3]) m.write32(s.x[3], 4);
        if (s.x[4]) m.write8(s.x[4], 1);
        status(s, 0);
    });
    // AudioServicesGetProperty(id, specifierSize, specifier, UInt32 *ioSize, void *out)
    support_.function(audio_toolbox, "_AudioServicesGetProperty", [self, sound_property](CpuState& s, GuestMemory& m) {
        if (!s.x[3] || !s.x[4]) return status(s, param);
        std::lock_guard lock(self->mutex_);
        std::uint32_t* value{};
        if (const auto result = sound_property(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), s.x[2], m, value))
            return status(s, result);
        if (m.read32(s.x[3]) < 4) return status(s, bad_property_size);
        m.write32(s.x[4], *value);
        m.write32(s.x[3], 4);
        status(s, 0);
    });
    // AudioServicesSetProperty(id, specifierSize, specifier, UInt32 size, const void *data)
    support_.function(audio_toolbox, "_AudioServicesSetProperty", [self, sound_property](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        std::uint32_t* value{};
        if (const auto result = sound_property(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), s.x[2], m, value))
            return status(s, result);
        if (static_cast<std::uint32_t>(s.x[3]) < 4 || !s.x[4]) return status(s, bad_property_size);
        *value = m.read32(s.x[4]);
        status(s, 0);
    });
}

} // namespace

void install_audio_services(Support& support) { std::make_shared<SystemSounds>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
