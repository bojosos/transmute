#include "alchemy/darwin/media/avaudio.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// AVAudioPlayer on the default output device's I/O (hal_io.hpp): the file or data (linear PCM
// WAVE, AIFF/AIFC, CAF) is decoded when the player is made, then played resampled to the device,
// mono to every channel. Volume (with fades), pan, rate (with enableRate), loops, seeking,
// playAtTime: on the deviceCurrentTime clock and metering are applied; the delegate's
// -audioPlayerDidFinishPlaying:successfully: runs on the main queue. Without an output device
// -prepareToPlay and -play answer NO. Compressed files decode through audio_stream.hpp (MP3,
// FLAC, Opus); other compressed formats fail to load.
namespace alchemy::gui::completion_kit::media::avaudio {
namespace {

namespace af = audio_file;

struct Player final : ObjectPayload {
    std::mutex mutex;
    Decoded audio;
    Address url{}, data{}, format{};   // retained
    Address delegate{};                // not retained
    float volume{1}, target_volume{1}, pan{}, rate{1};
    double fade_step{};                // volume change per device frame while fading
    bool enable_rate{}, metering{}, playing{};
    bool deallocating{};               // set by -dealloc: a finish posted earlier does nothing
    std::int64_t loops{}, loops_left{};
    double position{};                 // in source frames: where the resampler's input is
    double played{};                   // source frames played (currentTime), behind `position` by the resampler's look-ahead
    std::uint64_t delay_frames{};      // playAtTime: device frames of silence before playing
    std::shared_ptr<hal_io::DeviceIo> io;
    std::unique_ptr<Resampler> resampler;
    double resampler_ratio{};
    std::vector<float> peak, average;  // per channel, linear
    std::vector<float> meter_peak, meter_average;
};

float decibels(float level) { return level > 1e-8f ? std::max(-160.0f, 20.0f * std::log10(level)) : -160.0f; }

class Players : public std::enable_shared_from_this<Players> {
public:
    explicit Players(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    std::shared_ptr<Player> state(Address self) {
        const auto p = support_.objc.find_host_state<Player>(self);
        if (!p) unsupported("an AVAudioPlayer that was not initialized with a URL or data");
        return p;
    }
    void load(CpuState& s, Decoded decoded, std::int32_t status, Address url, Address data, Address error_out);
    bool start(Address self, const std::shared_ptr<Player>& p);
    void stop_io(Address self, Player& p);
    void render(Address self, const std::shared_ptr<Player>& p, float* mix, std::uint32_t frames, std::uint32_t channels, double device_rate);

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
};

void Players::load(CpuState& s, Decoded decoded, std::int32_t status, Address url, Address data, Address error_out) {
    if (status || decoded.channels == 0 || !(decoded.rate > 0)) {
        if (error_out)
            bridge_->store_error(error_out, bridge_->os_status_error(status ? status : af::error::invalid_file,
                status == af::error::unsupported_data_format ? "The audio format has no decoder under Alchemy" : "The audio could not be read"), true);
        static_cast<void>(support_.send_id(s.x[0], "release"));
        s.x[0] = 0;
        return;
    }
    auto p = support_.objc.host_state<Player>(s.x[0]);
    p->audio = std::move(decoded);
    p->url = url ? support_.send_id(url, "retain") : 0;
    p->data = data ? support_.send_id(data, "retain") : 0;
    af::Pcm pcm;
    pcm.rate = p->audio.rate;
    pcm.channels = p->audio.channels;
    pcm.is_float = true;
    pcm.bits = 32;
    pcm.container = 4;
    pcm.non_interleaved = true;
    p->format = make_format(support_, pcm);
    p->peak.assign(p->audio.channels, 0);
    p->average.assign(p->audio.channels, 0);
    p->meter_peak.assign(p->audio.channels, 0);
    p->meter_average.assign(p->audio.channels, 0);
}

bool Players::start(Address self, const std::shared_ptr<Player>& p) {
    std::shared_ptr<hal_io::DeviceIo> io;
    {
        std::lock_guard lock(p->mutex);
        if (!p->io) p->io = hal_device_io(hal_default_output());
        io = p->io;
    }
    if (!io) return false;
    if (io->running(self)) return true;
    const auto owner = shared_from_this();
    const auto channels = io->channels();
    const auto rate = io->rate();
    return io->start(self, [owner, self, p, channels, rate](float* mix, std::uint32_t frames, const hal_io::Timing&) {
        owner->render(self, p, mix, frames, channels, rate);
    }, hal_buffer_frames(hal_default_output())) == 0;
}

void Players::stop_io(Address self, Player& p) {
    std::shared_ptr<hal_io::DeviceIo> io;
    {
        std::lock_guard lock(p.mutex);
        io = p.io;
    }
    if (io) io->stop(self);
}

void Players::render(Address self, const std::shared_ptr<Player>& p, float* mix, std::uint32_t frames, std::uint32_t channels, double device_rate) {
    bool finished = false;
    {
        std::lock_guard lock(p->mutex);
        if (!p->playing) return;
        const auto source_channels = p->audio.channels;
        const auto total = static_cast<double>(p->audio.frames());
        std::uint32_t skip = static_cast<std::uint32_t>(std::min<std::uint64_t>(p->delay_frames, frames));
        p->delay_frames -= skip;
        const double ratio = p->audio.rate * (p->enable_rate ? p->rate : 1.0) / device_rate;
        if (!p->resampler || p->resampler_ratio != ratio) {
            p->resampler = std::make_unique<Resampler>(p->audio.rate * (p->enable_rate ? p->rate : 1.0), device_rate, source_channels);
            p->resampler_ratio = ratio;
        }
        bool ran_out = false;
        const auto pull = [&](double* out, std::size_t count) {
            for (std::size_t f = 0; f < count; ++f) {
                if (p->position >= total) {
                    if (p->loops_left != 0 && total > 0) {
                        if (p->loops_left > 0) --p->loops_left;
                        p->position = 0;
                    } else {
                        ran_out = true;
                        std::fill(out + f * source_channels, out + count * source_channels, 0.0);
                        return;
                    }
                }
                const auto at = static_cast<std::size_t>(p->position) * source_channels;
                for (std::uint32_t c = 0; c < source_channels; ++c) out[f * source_channels + c] = p->audio.samples[at + c];
                p->position += 1;
            }
        };
        const auto count = frames - skip;
        std::vector<double> converted(static_cast<std::size_t>(count) * source_channels);
        if (count) p->resampler->render(converted.data(), count, pull);
        p->played = std::fmod(p->played + count * ratio, std::max(total, 1.0));
        // Pan (stereo balance or mono placement), volume (and its fade), metering.
        const float left = std::min(1.0f, 1.0f - p->pan), right = std::min(1.0f, 1.0f + p->pan);
        std::fill(p->peak.begin(), p->peak.end(), 0.0f);
        std::vector<double> squares(source_channels, 0);
        for (std::uint32_t f = 0; f < count; ++f) {
            if (p->fade_step != 0) {
                p->volume += static_cast<float>(p->fade_step);
                if ((p->fade_step > 0 && p->volume >= p->target_volume) || (p->fade_step < 0 && p->volume <= p->target_volume)) {
                    p->volume = p->target_volume;
                    p->fade_step = 0;
                }
            }
            const double* frame = converted.data() + static_cast<std::size_t>(f) * source_channels;
            float* out = mix + static_cast<std::size_t>(skip + f) * channels;
            for (std::uint32_t c = 0; c < source_channels; ++c) {
                const auto level = static_cast<float>(std::abs(frame[c]) * p->volume);
                p->peak[c] = std::max(p->peak[c], level);
                squares[c] += level * level;
            }
            if (source_channels == 1) {
                const auto value = static_cast<float>(frame[0]) * p->volume;
                for (std::uint32_t c = 0; c < channels; ++c) out[c] += value * (channels >= 2 ? (c == 0 ? left : c == 1 ? right : 1.0f) : 1.0f);
            } else {
                for (std::uint32_t c = 0; c < channels && c < source_channels; ++c)
                    out[c] += static_cast<float>(frame[c]) * p->volume * (c == 0 && source_channels == 2 ? left : c == 1 && source_channels == 2 ? right : 1.0f);
            }
        }
        for (std::uint32_t c = 0; c < source_channels; ++c) p->average[c] = count ? static_cast<float>(std::sqrt(squares[c] / count)) : 0.0f;
        if (ran_out) {
            finished = true;
            p->playing = false;
            p->position = 0;
            p->played = 0;
            p->resampler.reset();
        }
    }
    if (!finished) return;
    // On the main queue (where -play usually runs): the device client goes unless the player was
    // played again meanwhile, and the delegate hears.
    auto* sp = &support_;
    const auto owner = shared_from_this();
    bridge_->dispatch_async(bridge_->main_queue(), [owner, sp, self, p] {
        Address delegate{};
        bool restarted{};
        {
            std::lock_guard lock(p->mutex);
            if (p->deallocating) return;
            restarted = p->playing;
            delegate = p->delegate;
        }
        if (!restarted) owner->stop_io(self, *p);
        if (!delegate) return;
        CallRegs r{};
        r.x[2] = self;
        r.x[3] = 1;
        static_cast<void>(sp->objc.send_if_responds(delegate, sp->objc.selector("audioPlayerDidFinishPlaying:successfully:"), r));
    });
}

void Players::install() {
    auto& support = support_;
    const auto self_ptr = shared_from_this();
    auto* sp = &support_;
    const auto player = declare(support, "AVAudioPlayer", "NSObject");
    const auto real = [](const CpuState& s, unsigned i) { return std::bit_cast<double>(s.v[i][0]); };
    const auto real32 = [](const CpuState& s, unsigned i) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[i][0])); };
    const auto return_real32 = [](CpuState& s, float value) { s.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; };
    const auto return_real = [](CpuState& s, double value) { s.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; };

    // ---- making a player -----------------------------------------------------------------------
    const auto from_url = [self_ptr, sp](CpuState& s, Address url, Address error_out) {
        Decoded decoded;
        std::int32_t status = af::error::file_not_found;
        const auto scheme = url ? sp->send_id(url, "scheme") : 0;
        if (url && scheme && sp->cf.utf8(scheme) == "file") {
            GuestAudio audio;
            status = open_guest_audio(*sp, sp->cf.url_path(url), audio);
            if (!status) {
                status = decode_file(*sp, audio, decoded);
                close_guest_audio(*sp, audio);
            }
        }
        self_ptr->load(s, std::move(decoded), status, url, 0, error_out);
    };
    const auto from_data = [self_ptr](CpuState& s, Address data, Address error_out) {
        Decoded decoded;
        std::int32_t status = af::error::invalid_file;
        if (data && self_ptr->bridge_->is_data(data)) {
            const auto bytes = self_ptr->bridge_->bytes(data);
            status = decode_bytes(bytes, decoded);
        }
        self_ptr->load(s, std::move(decoded), status, 0, data, error_out);
    };
    support.method(player, "initWithContentsOfURL:error:", [from_url](CpuState& s, GuestMemory&) { from_url(s, s.x[2], s.x[3]); }, "@@:@^@");
    support.method(player, "initWithContentsOfURL:fileTypeHint:error:", [from_url](CpuState& s, GuestMemory&) { from_url(s, s.x[2], s.x[4]); }, "@@:@@^@");
    support.method(player, "initWithData:error:", [from_data](CpuState& s, GuestMemory&) { from_data(s, s.x[2], s.x[3]); }, "@@:@^@");
    support.method(player, "initWithData:fileTypeHint:error:", [from_data](CpuState& s, GuestMemory&) { from_data(s, s.x[2], s.x[4]); }, "@@:@@^@");
    support.method(player, "dealloc", [self_ptr, sp, player](CpuState& s, GuestMemory&) {
        if (const auto p = sp->objc.find_host_state<Player>(s.x[0])) {
            {
                std::lock_guard lock(p->mutex);
                p->deallocating = true;
                p->playing = false;
            }
            self_ptr->stop_io(s.x[0], *p);   // waits for a render in progress
            for (auto* object : {&p->url, &p->data, &p->format})
                if (*object) static_cast<void>(sp->send_id(std::exchange(*object, 0), "release"));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], player, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- transport -----------------------------------------------------------------------------
    support.method(player, "prepareToPlay", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        if (!p->io) p->io = hal_device_io(hal_default_output());
        s.x[0] = p->io ? 1 : 0;
    }, "B@:");
    const auto play = [self_ptr](Address self, double at_device_time) -> bool {
        const auto p = self_ptr->state(self);
        {
            std::lock_guard lock(p->mutex);
            if (p->playing) return true;
            p->playing = true;
            p->loops_left = p->loops;
            p->delay_frames = 0;
        }
        if (!self_ptr->start(self, p)) {
            std::lock_guard lock(p->mutex);
            p->playing = false;
            return false;
        }
        if (at_device_time > 0) {
            std::lock_guard lock(p->mutex);
            const auto now = static_cast<double>(host_time_now()) / 1e9;
            if (at_device_time > now && p->io) p->delay_frames = static_cast<std::uint64_t>((at_device_time - now) * p->io->rate());
        }
        return true;
    };
    support.method(player, "play", [play](CpuState& s, GuestMemory&) { s.x[0] = play(s.x[0], 0) ? 1 : 0; }, "B@:");
    support.method(player, "playAtTime:", [play, real](CpuState& s, GuestMemory&) { s.x[0] = play(s.x[0], real(s, 0)) ? 1 : 0; }, "B@:d");
    const auto halt = [self_ptr](Address self) {
        const auto p = self_ptr->state(self);
        {
            std::lock_guard lock(p->mutex);
            p->playing = false;
            p->delay_frames = 0;
        }
        self_ptr->stop_io(self, *p);
    };
    support.method(player, "pause", [halt](CpuState& s, GuestMemory&) { halt(s.x[0]); }, "v@:");
    // -stop keeps currentTime (as AVAudioPlayer does).
    support.method(player, "stop", [halt](CpuState& s, GuestMemory&) { halt(s.x[0]); }, "v@:");
    support.method(player, "isPlaying", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        s.x[0] = p->playing ? 1 : 0;
    }, "B@:");

    // ---- time --------------------------------------------------------------------------------
    support.method(player, "duration", [self_ptr, return_real](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        return_real(s, static_cast<double>(p->audio.frames()) / p->audio.rate);
    }, "d@:");
    support.method(player, "currentTime", [self_ptr, return_real](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        return_real(s, p->played / p->audio.rate);
    }, "d@:");
    support.method(player, "setCurrentTime:", [self_ptr, real](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        const auto frame = std::clamp(real(s, 0) * p->audio.rate, 0.0, static_cast<double>(p->audio.frames()));
        p->position = std::floor(frame);
        p->played = p->position;
        if (p->resampler) p->resampler->reset();
    }, "v@:d");
    support.method(player, "deviceCurrentTime", [return_real](CpuState& s, GuestMemory&) {
        return_real(s, static_cast<double>(host_time_now()) / 1e9);
    }, "d@:");

    // ---- properties ------------------------------------------------------------------------------
    support.method(player, "url", [self_ptr](CpuState& s, GuestMemory&) { s.x[0] = self_ptr->state(s.x[0])->url; }, "@@:");
    support.method(player, "data", [self_ptr](CpuState& s, GuestMemory&) { s.x[0] = self_ptr->state(s.x[0])->data; }, "@@:");
    support.method(player, "format", [self_ptr](CpuState& s, GuestMemory&) { s.x[0] = self_ptr->state(s.x[0])->format; }, "@@:");
    support.method(player, "settings", [self_ptr, sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(self_ptr->state(s.x[0])->format, "settings"); }, "@@:");
    support.method(player, "numberOfChannels", [self_ptr](CpuState& s, GuestMemory&) { s.x[0] = self_ptr->state(s.x[0])->audio.channels; }, "Q@:");
    support.method(player, "delegate", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        s.x[0] = p->delegate;
    }, "@@:");
    support.method(player, "setDelegate:", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        p->delegate = s.x[2];
    }, "v@:@");
    support.method(player, "numberOfLoops", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        s.x[0] = static_cast<std::uint64_t>(p->loops);
    }, "q@:");
    support.method(player, "setNumberOfLoops:", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        p->loops = static_cast<std::int64_t>(s.x[2]);
        if (p->playing) p->loops_left = p->loops;
    }, "v@:q");
    const auto float_property = [&](const char* getter, const char* setter, float Player::*field, float low, float high) {
        support.method(player, getter, [self_ptr, field, return_real32](CpuState& s, GuestMemory&) {
            const auto p = self_ptr->state(s.x[0]);
            std::lock_guard lock(p->mutex);
            return_real32(s, (*p).*field);
        }, "f@:");
        support.method(player, setter, [self_ptr, field, real32, low, high](CpuState& s, GuestMemory&) {
            const auto p = self_ptr->state(s.x[0]);
            std::lock_guard lock(p->mutex);
            (*p).*field = std::clamp(real32(s, 0), low, high);
            if (field == &Player::volume) {
                p->target_volume = p->volume;
                p->fade_step = 0;
            }
        }, "v@:f");
    };
    float_property("volume", "setVolume:", &Player::volume, 0.0f, 1.0f);
    float_property("pan", "setPan:", &Player::pan, -1.0f, 1.0f);
    float_property("rate", "setRate:", &Player::rate, 0.5f, 2.0f);
    // -setVolume:fadeDuration: (float s0, NSTimeInterval d1): a linear ramp on the device's frames.
    support.method(player, "setVolume:fadeDuration:", [self_ptr, real32, real](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        p->target_volume = std::clamp(real32(s, 0), 0.0f, 1.0f);
        const auto duration = real(s, 1);
        const double rate = p->io ? p->io->rate() : 48000.0;
        if (!(duration > 0) || !p->playing) {
            p->volume = p->target_volume;
            p->fade_step = 0;
        } else {
            p->fade_step = (p->target_volume - p->volume) / (duration * rate);
        }
    }, "v@:fd");
    support.method(player, "enableRate", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        s.x[0] = p->enable_rate ? 1 : 0;
    }, "B@:");
    support.method(player, "setEnableRate:", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        p->enable_rate = (s.x[2] & 0xff) != 0;
    }, "v@:B");

    // ---- metering ------------------------------------------------------------------------------
    support.method(player, "isMeteringEnabled", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        s.x[0] = p->metering ? 1 : 0;
    }, "B@:");
    support.method(player, "setMeteringEnabled:", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        p->metering = (s.x[2] & 0xff) != 0;
    }, "v@:B");
    support.method(player, "updateMeters", [self_ptr](CpuState& s, GuestMemory&) {
        const auto p = self_ptr->state(s.x[0]);
        std::lock_guard lock(p->mutex);
        if (!p->metering) return;
        p->meter_peak = p->peak;
        p->meter_average = p->average;
    }, "v@:");
    for (const auto& [selector, peak] : std::initializer_list<std::pair<const char*, bool>>{{"peakPowerForChannel:", true}, {"averagePowerForChannel:", false}}) {
        const bool use_peak = peak;
        support.method(player, selector, [self_ptr, use_peak, return_real32](CpuState& s, GuestMemory&) {
            const auto p = self_ptr->state(s.x[0]);
            std::lock_guard lock(p->mutex);
            const auto& levels = use_peak ? p->meter_peak : p->meter_average;
            return_real32(s, p->metering && s.x[2] < levels.size() ? decibels(levels[s.x[2]]) : -160.0f);
        }, "f@:Q");
    }
    support.method(player, "channelAssignments", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
}

} // namespace

void install_avaudio_player(Support& support) { std::make_shared<Players>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media::avaudio
