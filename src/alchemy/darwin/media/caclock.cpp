#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <map>
#include <vector>

// AudioToolbox CAClock: a software media clock (QLab's MIDI timecode and
// timeline clocks). Time advances with the host clock at the play rate while
// the clock runs; it is converted between host time, seconds, beats (a
// constant tempo, 120 BPM unless kCAClockProperty_TempoMap sets one) and
// samples. External sync sources (MIDI timecode/clock input) have no devices
// under Alchemy, so internal sync is the only working mode; listeners hear
// the started/stopped/armed/disarmed/property-changed messages. (The CoreAudio
// HAL is media/hal.cpp's.)
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";

constexpr std::uint32_t four(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

constexpr std::int32_t no_err = 0;
constexpr std::int32_t param_err = -50;
constexpr std::int32_t clock_invalid_property = -66816;   // kCAClock_InvalidPropertyError
constexpr std::int32_t clock_invalid_size = -66815;       // kCAClock_InvalidPropertySizeError
constexpr std::int32_t clock_invalid_time_format = -66812; // kCAClock_InvalidTimeFormatError

// Time formats.
constexpr auto format_host = four("host"), format_samples = four("samp"), format_beats = four("beat"),
               format_seconds = four("secs"), format_smpte_seconds = four("smps"), format_absolute = four("asec");
// Messages.
constexpr auto message_started = four("strt"), message_stopped = four("stop"), message_armed = four("arm "),
               message_disarmed = four("darm"), message_property_changed = four("pchg");

struct Listener { Address proc{}, user_data{}; };

struct Clock {
    std::mutex mutex;
    std::map<std::uint32_t, std::vector<std::uint8_t>> properties;
    std::vector<Listener> listeners;
    double play_rate{1.0};
    double start_seconds{};       // media seconds at the anchor
    double anchor{};              // host seconds when started
    double stopped_seconds{};     // media seconds while stopped
    double start_time{};          // CAClockGetStartTime
    bool running{}, armed{};
    double tempo{120.0};
    double sample_rate{44100.0};
};

// Host time is mach_absolute_time's (host::time::monotonic_ns).
double host_seconds() { return static_cast<double>(host::time::monotonic_ns()) * 1e-9; }

struct Clocks {
    std::mutex mutex;
    std::map<Address, std::shared_ptr<Clock>> objects;
    std::shared_ptr<Clock> find(Address handle) {
        std::lock_guard lock(mutex);
        const auto found = objects.find(handle);
        return found == objects.end() ? nullptr : found->second;
    }
};

double media_seconds(Clock& clock) {
    return clock.running ? clock.start_seconds + (host_seconds() - clock.anchor) * clock.play_rate : clock.stopped_seconds;
}

void return_status(CpuState& s, std::int32_t status) { s.x[0] = static_cast<std::uint32_t>(status); }

} // namespace

void install_audio(Support& support) {
    auto clocks = std::make_shared<Clocks>();
    const auto notify = [&support](Clock& clock, std::uint32_t message, Address parameter) {
        std::vector<Listener> listeners;
        { std::lock_guard lock(clock.mutex); listeners = clock.listeners; }
        for (const auto& listener : listeners) {
            const std::array<Address, 3> arguments{listener.user_data, message, parameter};
            static_cast<void>(support.process.calling_engine().invoke_callback(listener.proc, arguments));
        }
    };
    support.function(audio_toolbox, "_CAClockNew", [&support, clocks](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) return return_status(s, param_err);
        const auto handle = support.process.allocate_guest(16, 16);
        { std::lock_guard lock(clocks->mutex); clocks->objects.emplace(handle, std::make_shared<Clock>()); }
        m.write64(s.x[1], handle);
        return_status(s, no_err);
    });
    support.function(audio_toolbox, "_CAClockDispose", [&support, clocks](CpuState& s, GuestMemory&) {
        bool found{};
        { std::lock_guard lock(clocks->mutex); found = clocks->objects.erase(s.x[0]) != 0; }
        if (found) support.process.release_guest(s.x[0]);
        return_status(s, found ? no_err : param_err);
    });
    support.function(audio_toolbox, "_CAClockAddListener", [clocks](CpuState& s, GuestMemory&) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock || !s.x[1]) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        clock->listeners.push_back({s.x[1], s.x[2]});
        return_status(s, no_err);
    });
    support.function(audio_toolbox, "_CAClockRemoveListener", [clocks](CpuState& s, GuestMemory&) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        auto& list = clock->listeners;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Listener& entry) { return entry.proc == s.x[1] && entry.user_data == s.x[2]; }), list.end());
        return_status(s, no_err);
    });
    // Properties are stored as given (sync mode, SMPTE format, tempo map,
    // MIDI endpoints, ...); a property never set reports its size as 0.
    support.function(audio_toolbox, "_CAClockGetPropertyInfo", [clocks](CpuState& s, GuestMemory& m) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        const auto found = clock->properties.find(static_cast<std::uint32_t>(s.x[1]));
        if (s.x[2]) m.write32(s.x[2], found == clock->properties.end() ? 0 : static_cast<std::uint32_t>(found->second.size()));
        if (s.x[3]) m.write8(s.x[3], 1);
        return_status(s, no_err);
    });
    support.function(audio_toolbox, "_CAClockGetProperty", [clocks](CpuState& s, GuestMemory& m) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock || !s.x[2] || !s.x[3]) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        const auto found = clock->properties.find(static_cast<std::uint32_t>(s.x[1]));
        if (found == clock->properties.end()) return return_status(s, clock_invalid_property);
        const auto room = m.read32(s.x[2]);
        if (room < found->second.size()) return return_status(s, clock_invalid_size);
        m.write_bytes(s.x[3], found->second);
        m.write32(s.x[2], static_cast<std::uint32_t>(found->second.size()));
        return_status(s, no_err);
    });
    support.function(audio_toolbox, "_CAClockSetProperty", [clocks, notify](CpuState& s, GuestMemory& m) {
        const auto clock = clocks->find(s.x[0]);
        const auto size = static_cast<std::uint32_t>(s.x[2]);
        if (!clock || (size && !s.x[3]) || size > 65536) return return_status(s, param_err);
        std::vector<std::uint8_t> bytes(size);
        if (size) m.read_bytes(s.x[3], bytes);
        { std::lock_guard lock(clock->mutex); clock->properties[static_cast<std::uint32_t>(s.x[1])] = std::move(bytes); }
        notify(*clock, message_property_changed, s.x[1] & 0xffffffffU);
        return_status(s, no_err);
    });
    // CAClockTime: {UInt32 format; UInt32 reserved; union {UInt64 hostTime; Float64 samples, beats, seconds; SMPTETime smpte;}}.
    const auto to_seconds = [](Clock& clock, GuestMemory& m, Address time, double& seconds) {
        const auto format = m.read32(time);
        const auto value = m.read64(time + 8);
        const auto real = std::bit_cast<double>(value);
        if (format == format_seconds || format == format_smpte_seconds || format == format_absolute) seconds = real;
        else if (format == format_beats) seconds = real * 60.0 / clock.tempo;
        else if (format == format_samples) seconds = real / clock.sample_rate;
        else if (format == format_host) seconds = clock.start_seconds + (static_cast<double>(value) / 1e9 - clock.anchor) * clock.play_rate;
        else return false;
        return true;
    };
    const auto from_seconds = [](Clock& clock, GuestMemory& m, Address time, std::uint32_t format, double seconds) {
        m.fill_bytes(time, 0, 32);
        m.write32(time, format);
        if (format == format_seconds || format == format_smpte_seconds || format == format_absolute) m.write64(time + 8, std::bit_cast<std::uint64_t>(seconds));
        else if (format == format_beats) m.write64(time + 8, std::bit_cast<std::uint64_t>(seconds * clock.tempo / 60.0));
        else if (format == format_samples) m.write64(time + 8, std::bit_cast<std::uint64_t>(seconds * clock.sample_rate));
        else if (format == format_host) {
            const auto host = clock.play_rate != 0 ? clock.anchor + (seconds - clock.start_seconds) / clock.play_rate : clock.anchor;
            m.write64(time + 8, static_cast<std::uint64_t>(std::max(0.0, host) * 1e9));
        } else return false;
        return true;
    };
    support.function(audio_toolbox, "_CAClockGetCurrentTime", [clocks, from_seconds](CpuState& s, GuestMemory& m) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock || !s.x[2]) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        return_status(s, from_seconds(*clock, m, s.x[2], static_cast<std::uint32_t>(s.x[1]), media_seconds(*clock)) ? no_err : clock_invalid_time_format);
    });
    support.function(audio_toolbox, "_CAClockGetStartTime", [clocks, from_seconds](CpuState& s, GuestMemory& m) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock || !s.x[2]) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        return_status(s, from_seconds(*clock, m, s.x[2], static_cast<std::uint32_t>(s.x[1]), clock->start_time) ? no_err : clock_invalid_time_format);
    });
    support.function(audio_toolbox, "_CAClockSetCurrentTime", [clocks, to_seconds](CpuState& s, GuestMemory& m) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock || !s.x[1]) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        double seconds{};
        if (!to_seconds(*clock, m, s.x[1], seconds)) return return_status(s, clock_invalid_time_format);
        clock->start_time = seconds;
        if (clock->running) { clock->start_seconds = seconds; clock->anchor = host_seconds(); }
        else clock->stopped_seconds = seconds;
        return_status(s, no_err);
    });
    support.function(audio_toolbox, "_CAClockTranslateTime", [clocks, to_seconds, from_seconds](CpuState& s, GuestMemory& m) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock || !s.x[1] || !s.x[3]) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        double seconds{};
        if (!to_seconds(*clock, m, s.x[1], seconds)) return return_status(s, clock_invalid_time_format);
        return_status(s, from_seconds(*clock, m, s.x[3], static_cast<std::uint32_t>(s.x[2]), seconds) ? no_err : clock_invalid_time_format);
    });
    support.function(audio_toolbox, "_CAClockSetPlayRate", [clocks](CpuState& s, GuestMemory&) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock) return return_status(s, param_err);
        std::lock_guard lock(clock->mutex);
        const auto now = media_seconds(*clock);
        clock->play_rate = std::bit_cast<double>(s.v[0][0]);
        if (clock->running) { clock->start_seconds = now; clock->anchor = host_seconds(); }
        return_status(s, no_err);
    });
    const auto transition = [clocks, notify](CpuState& s, bool start) {
        const auto clock = clocks->find(s.x[0]);
        if (!clock) return return_status(s, param_err);
        {
            std::lock_guard lock(clock->mutex);
            if (clock->running == start) return return_status(s, no_err);
            if (start) { clock->start_seconds = clock->stopped_seconds; clock->anchor = host_seconds(); }
            else clock->stopped_seconds = media_seconds(*clock);
            clock->running = start;
        }
        notify(*clock, start ? message_started : message_stopped, 0);
        return_status(s, no_err);
    };
    support.function(audio_toolbox, "_CAClockStart", [transition](CpuState& s, GuestMemory&) { transition(s, true); });
    support.function(audio_toolbox, "_CAClockStop", [transition](CpuState& s, GuestMemory&) { transition(s, false); });
    // Arming waits for an external sync source; the internal clock has none,
    // so arming only records the state.
    for (const bool arm : {true, false})
        support.function(audio_toolbox, arm ? "_CAClockArm" : "_CAClockDisarm", [clocks, notify, arm](CpuState& s, GuestMemory&) {
            const auto clock = clocks->find(s.x[0]);
            if (!clock) return return_status(s, param_err);
            { std::lock_guard lock(clock->mutex); clock->armed = arm; }
            notify(*clock, arm ? message_armed : message_disarmed, 0);
            return_status(s, no_err);
        });
}

} // namespace alchemy::gui::completion_kit
