#include "alchemy/darwin/media/media_support.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// AudioToolbox parameter and event listeners (<AudioToolbox/AudioUnitUtilities.h>).
//
// Listeners are real objects that record the (object, parameter) and
// (object, event) registrations they are given. AUParameterSet sets the value
// through AudioUnitSetParameter and AUParameterListenerNotify reads it through
// AudioUnitGetParameter, as Apple's implementation does; their status is
// returned unchanged (the output units have one parameter, kHALOutputParam_Volume:
// audio_units.cpp). Property-change events register an AudioUnit property
// listener. Successful notifications call the listeners on the notifying thread; listeners made with
// a dispatch queue get their block called there, with copies of the parameter or event.
// AUEventListenerNotify delivers any parameter event (value change, gesture begin and end) to the
// listeners registered for it.
//
// AUParameterValueToLinear / FromLinear map a value to a slider position in [0, 1] and back over the
// parameter's range (kAudioUnitProperty_ParameterInfo), shaped by its display flags (square root,
// squared, cubed, cube root, exponential, logarithmic). AUParameterFormatValue writes the value with
// `digits` significant digits.
namespace alchemy::gui::completion_kit::media {
namespace {

constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view audio_unit = "/System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit";
constexpr std::int32_t param_err = -50;
constexpr std::int32_t bad_component_instance = static_cast<std::int32_t>(0x80008001U);

// AudioUnitParameter {AudioUnit mAudioUnit; AudioUnitParameterID mParameterID; AudioUnitScope mScope; AudioUnitElement mElement}
struct Parameter {
    Address unit{};
    std::uint32_t id{}, scope{}, element{};
    bool operator==(const Parameter&) const = default;
};
Parameter read_parameter(GuestMemory& m, Address at) { return {m.read64(at), m.read32(at + 8), m.read32(at + 12), m.read32(at + 16)}; }

struct Registration { Address object{}; std::uint32_t event{}; Parameter parameter; };

struct Listener {
    bool events{};                 // an AUEventListener (proc signature differs)
    Address proc{}, user_data{};
    Address block{}, queue{};      // ...CreateWithDispatchQueue: the block (copied) and its queue (retained)
    float interval{}, granularity{};
    std::vector<Registration> registrations;
};

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

// kAudioUnitParameterFlag_Display* (the shape of a parameter's slider).
constexpr std::uint32_t display_mask = (7U << 16) | (1U << 22), display_square_root = 1U << 16, display_squared = 2U << 16,
    display_cubed = 3U << 16, display_cube_root = 4U << 16, display_exponential = 5U << 16, display_logarithmic = 1U << 22;

struct Range { double minimum{0}, maximum{1}; std::uint32_t flags{}; };

// The parameter's AudioUnitParameterInfo (104 bytes: minValue at 84, maxValue at 88, flags at 96); [0, 1]
// when the unit has none.
Range parameter_range(Support& support, GuestMemory& m, const Parameter& p) {
    Range range;
    const auto info = support.process.allocate_guest(112, 8), size = support.process.allocate_guest(8, 8);
    m.write32(size, 104);
    const auto result = static_cast<std::int32_t>(support.call("/System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit",
        "_AudioUnitGetProperty", {p.unit, 4, p.scope, p.id, info, size}));   // kAudioUnitProperty_ParameterInfo
    if (!result && m.read32(size) >= 100) {
        range.minimum = std::bit_cast<float>(m.read32(info + 84));
        range.maximum = std::bit_cast<float>(m.read32(info + 88));
        range.flags = m.read32(info + 96);
    }
    support.process.release_guest(info);
    support.process.release_guest(size);
    return range;
}

double to_linear(double value, const Range& r) {
    if (!(r.maximum > r.minimum)) return 0;
    value = std::clamp(value, r.minimum, r.maximum);
    if ((r.flags & display_logarithmic) && r.minimum > 0) return std::log(value / r.minimum) / std::log(r.maximum / r.minimum);
    const double n = (value - r.minimum) / (r.maximum - r.minimum);
    switch (r.flags & display_mask & ~display_logarithmic) {
    case display_square_root: return std::sqrt(n);
    case display_squared: return n * n;
    case display_cubed: return n * n * n;
    case display_cube_root: return std::cbrt(n);
    case display_exponential: return (std::exp(n) - 1) / (std::exp(1.0) - 1);
    default: return n;
    }
}

double from_linear(double linear, const Range& r) {
    if (!(r.maximum > r.minimum)) return r.minimum;
    linear = std::clamp(linear, 0.0, 1.0);
    if ((r.flags & display_logarithmic) && r.minimum > 0) return r.minimum * std::pow(r.maximum / r.minimum, linear);
    double n = linear;
    switch (r.flags & display_mask & ~display_logarithmic) {
    case display_square_root: n = linear * linear; break;
    case display_squared: n = std::sqrt(linear); break;
    case display_cubed: n = std::cbrt(linear); break;
    case display_cube_root: n = linear * linear * linear; break;
    case display_exponential: n = std::log(linear * (std::exp(1.0) - 1) + 1); break;
    default: break;
    }
    return r.minimum + n * (r.maximum - r.minimum);
}

} // namespace

void install_au_listeners(Support& support) {
    struct State { std::mutex mutex; std::map<Address, Listener> listeners; };
    auto state = std::make_shared<State>();
    auto* sp = &support;
    const auto lib = audio_toolbox;

    // AUListenerCreate(AUParameterListenerProc, void *userData, CFRunLoopRef, CFStringRef mode,
    //     Float32 notificationInterval [s0], AUParameterListenerRef *out)
    support.function(lib, "_AUListenerCreate", [state, sp](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[4]) return status(s, param_err);
        Listener listener;
        listener.proc = s.x[0];
        listener.user_data = s.x[1];
        listener.interval = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        const auto handle = sp->process.allocate_guest(16, 16);
        { std::lock_guard lock(state->mutex); state->listeners[handle] = std::move(listener); }
        m.write64(s.x[4], handle);
        status(s, 0);
    });
    // AUEventListenerCreate(AUEventListenerProc, void *userData, CFRunLoopRef, CFStringRef mode,
    //     Float32 notificationInterval [s0], Float32 valueChangeGranularity [s1], AUEventListenerRef *out)
    support.function(lib, "_AUEventListenerCreate", [state, sp](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[4]) return status(s, param_err);
        Listener listener;
        listener.events = true;
        listener.proc = s.x[0];
        listener.user_data = s.x[1];
        listener.interval = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        listener.granularity = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[1][0]));
        const auto handle = sp->process.allocate_guest(16, 16);
        { std::lock_guard lock(state->mutex); state->listeners[handle] = std::move(listener); }
        m.write64(s.x[4], handle);
        status(s, 0);
    });
    // AUListenerCreateWithDispatchQueue(AUParameterListenerRef *out, Float32 interval [s0], dispatch_queue_t,
    //     void (^)(void *object, const AudioUnitParameter *, AudioUnitParameterValue))
    // AUEventListenerCreateWithDispatchQueue(AUEventListenerRef *out, Float32 interval [s0], Float32 granularity [s1],
    //     dispatch_queue_t, void (^)(void *object, const AudioUnitEvent *, UInt64 hostTime, AudioUnitParameterValue))
    for (const bool events : {false, true})
        support.function(lib, events ? "_AUEventListenerCreateWithDispatchQueue" : "_AUListenerCreateWithDispatchQueue",
                         [state, sp, events](CpuState& s, GuestMemory& m) {
            if (!s.x[0] || !s.x[1] || !s.x[2]) return status(s, param_err);
            const auto bridge = security::bridge();
            Listener listener;
            listener.events = events;
            listener.interval = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
            if (events) listener.granularity = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[1][0]));
            listener.queue = bridge->retain(s.x[1]);
            listener.block = bridge->block_copy(s.x[2]);
            const auto handle = sp->process.allocate_guest(16, 16);
            { std::lock_guard lock(state->mutex); state->listeners[handle] = std::move(listener); }
            m.write64(s.x[0], handle);
            status(s, 0);
        });
    // AUListenerDispose (parameter and event listeners alike)
    for (const auto* name : {"_AUListenerDispose"})
        support.function(lib, name, [state, sp](CpuState& s, GuestMemory&) {
            Listener gone;
            {
                std::lock_guard lock(state->mutex);
                const auto found = state->listeners.find(s.x[0]);
                if (found == state->listeners.end()) return status(s, param_err);
                gone = std::move(found->second);
                state->listeners.erase(found);
            }
            sp->process.release_guest(s.x[0]);
            if (gone.block) {
                const auto bridge = security::bridge();
                bridge->block_release(gone.block);
                bridge->release(gone.queue);
            }
            status(s, 0);
        });
    // AUListenerAddParameter(listener, void *object, const AudioUnitParameter *)
    support.function(lib, "_AUListenerAddParameter", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, param_err);
        const auto parameter = read_parameter(m, s.x[2]);
        std::lock_guard lock(state->mutex);
        const auto found = state->listeners.find(s.x[0]);
        if (found == state->listeners.end()) return status(s, param_err);
        found->second.registrations.push_back({s.x[1], 0, parameter});
        status(s, 0);
    });
    support.function(lib, "_AUListenerRemoveParameter", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, param_err);
        const auto parameter = read_parameter(m, s.x[2]);
        std::lock_guard lock(state->mutex);
        const auto found = state->listeners.find(s.x[0]);
        if (found == state->listeners.end()) return status(s, param_err);
        std::erase_if(found->second.registrations, [&](const Registration& r) { return r.object == s.x[1] && r.parameter == parameter; });
        status(s, 0);
    });
    // AudioUnitEvent {AudioUnitEventType mEventType; union {AudioUnitParameter; AudioUnitProperty} mArgument (at 8)}.
    // Event types 0-2 name a parameter; 3 (kAudioUnitEvent_PropertyChange) a property of a unit.
    support.function(lib, "_AUEventListenerAddEventType", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, param_err);
        const auto type = m.read32(s.x[2]);
        const auto argument = read_parameter(m, s.x[2] + 8);
        if (type > 3) return status(s, param_err);
        std::lock_guard lock(state->mutex);
        const auto found = state->listeners.find(s.x[0]);
        if (found == state->listeners.end() || !found->second.events) return status(s, param_err);
        // A property listener is installed on the unit itself: no unit instance can exist.
        if (type == 3) return status(s, argument.unit ? bad_component_instance : param_err);
        found->second.registrations.push_back({s.x[1], type, argument});
        status(s, 0);
    });
    support.function(lib, "_AUEventListenerRemoveEventType", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, param_err);
        const auto type = m.read32(s.x[2]);
        const auto argument = read_parameter(m, s.x[2] + 8);
        std::lock_guard lock(state->mutex);
        const auto found = state->listeners.find(s.x[0]);
        if (found == state->listeners.end() || !found->second.events) return status(s, param_err);
        std::erase_if(found->second.registrations, [&](const Registration& r) { return r.object == s.x[1] && r.event == type && r.parameter == argument; });
        status(s, 0);
    });

    // Calls the listeners registered for event `type` (0: value change, 1-2: gesture begin, end) of
    // `parameter` (except `sender`) with `value`: procedures on this thread, blocks on their queue.
    // Parameter listeners hear value changes only.
    const auto deliver = [state, sp](Address sender, std::uint32_t type, const Parameter& parameter, Address parameter_at, float value) {
        std::vector<std::pair<Listener, Address>> targets;
        {
            std::lock_guard lock(state->mutex);
            for (const auto& [handle, listener] : state->listeners) {
                if (handle == sender) continue;
                for (const auto& r : listener.registrations)
                    if (r.event == type && r.parameter == parameter && (listener.events || type == 0)) targets.emplace_back(listener, r.object);
            }
        }
        auto& m = sp->memory();
        std::vector<std::uint8_t> parameter_bytes(24);
        m.read_bytes(parameter_at, parameter_bytes);
        const auto bridge = security::bridge();
        for (const auto& [listener, object] : targets) {
            // The argument: an AudioUnitEvent {type; AudioUnitParameter at 8} or the AudioUnitParameter.
            const auto argument = sp->process.allocate_guest(32, 8);
            m.fill_bytes(argument, 0, 32);
            if (listener.events) {
                m.write32(argument, type);
                m.write_bytes(argument + 8, parameter_bytes);
            } else {
                m.write_bytes(argument, parameter_bytes);
            }
            const auto host_time = host_time_now();
            const auto call = [sp, listener, object, argument, host_time, value] {
                CallRegs registers{};
                std::size_t next = 0;
                Address target = listener.proc;
                if (listener.block) {
                    target = sp->memory().read64(listener.block + 16);   // the block's invoke function
                    registers.x[next++] = listener.block;
                } else {
                    registers.x[next++] = listener.user_data;
                }
                // Procedures: (userData, object, argument[, hostTime], value [s0]); blocks: (block, object, argument[, hostTime], value [s0]).
                registers.x[next++] = object;
                registers.x[next++] = argument;
                if (listener.events) registers.x[next++] = host_time;
                registers.set_s(0, value);
                static_cast<void>(EngineAdapter(sp->process.calling_engine()).call_regs(target, registers));
                sp->process.release_guest(argument);
            };
            if (listener.block) bridge->dispatch_async(listener.queue, call);
            else call();
        }
    };

    // AUParameterSet(sendingListener, sendingObject, const AudioUnitParameter *, AudioUnitParameterValue [s0],
    //     UInt32 bufferOffsetInFrames): AudioUnitSetParameter, then the listeners.
    support.function(lib, "_AUParameterSet", [sp, deliver](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, param_err);
        const auto parameter = read_parameter(m, s.x[2]);
        CallRegs registers{};
        registers.x[0] = parameter.unit;
        registers.x[1] = parameter.id;
        registers.x[2] = parameter.scope;
        registers.x[3] = parameter.element;
        registers.v[0] = s.v[0];
        registers.x[4] = static_cast<std::uint32_t>(s.x[3]);
        const auto result = static_cast<std::int32_t>(sp->call(audio_unit, "_AudioUnitSetParameter", registers).x0);
        if (!result) deliver(s.x[0], 0, parameter, s.x[2], std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])));
        status(s, result);
    });
    // AUParameterListenerNotify(sendingListener, sendingObject, const AudioUnitParameter *): the value
    // comes from AudioUnitGetParameter.
    support.function(lib, "_AUParameterListenerNotify", [sp, deliver](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, param_err);
        const auto parameter = read_parameter(m, s.x[2]);
        const auto value = sp->process.allocate_guest(8, 8);
        const auto result = static_cast<std::int32_t>(sp->call(audio_unit, "_AudioUnitGetParameter",
            {parameter.unit, parameter.id, parameter.scope, parameter.element, value}));
        const auto read = std::bit_cast<float>(m.read32(value));
        sp->process.release_guest(value);
        if (!result) deliver(s.x[0], 0, parameter, s.x[2], read);
        status(s, result);
    });
    // AUEventListenerNotify(sendingListener, sendingObject, const AudioUnitEvent *): a parameter event goes to the
    // listeners registered for it (a value change with the current value); property changes have no listeners here.
    support.function(lib, "_AUEventListenerNotify", [sp, deliver](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, param_err);
        const auto type = m.read32(s.x[2]);
        if (type > 3) return status(s, param_err);
        if (type == 3) return status(s, 0);
        const auto parameter = read_parameter(m, s.x[2] + 8);
        float value = 0;
        if (type == 0) {
            const auto out = sp->process.allocate_guest(8, 8);
            const auto result = static_cast<std::int32_t>(sp->call(audio_unit, "_AudioUnitGetParameter",
                {parameter.unit, parameter.id, parameter.scope, parameter.element, out}));
            value = std::bit_cast<float>(m.read32(out));
            sp->process.release_guest(out);
            if (result) return status(s, result);
        }
        deliver(s.x[0], type, parameter, s.x[2] + 8, value);
        status(s, 0);
    });

    // AUParameterValueToLinear(AudioUnitParameterValue [s0], const AudioUnitParameter *) -> Float32 [s0]
    support.function(lib, "_AUParameterValueToLinear", [sp](CpuState& s, GuestMemory& m) {
        const double value = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        const auto range = s.x[0] ? parameter_range(*sp, m, read_parameter(m, s.x[0])) : Range{};
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(to_linear(value, range))), 0};
    });
    // AUParameterValueFromLinear(Float32 linear [s0], const AudioUnitParameter *) -> AudioUnitParameterValue [s0]
    support.function(lib, "_AUParameterValueFromLinear", [sp](CpuState& s, GuestMemory& m) {
        const double linear = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        const auto range = s.x[0] ? parameter_range(*sp, m, read_parameter(m, s.x[0])) : Range{};
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(from_linear(linear, range))), 0};
    });
    // AUParameterFormatValue(Float64 value [d0], const AudioUnitParameter *, char *textBuffer, UInt32 digits) -> textBuffer:
    // `digits` significant digits (at least the integer part), no more than 31 characters.
    support.function(lib, "_AUParameterFormatValue", [](CpuState& s, GuestMemory& m) {
        const auto value = std::bit_cast<double>(s.v[0][0]);
        const auto buffer = s.x[1];
        const auto digits = static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint32_t>(s.x[2]), 15));
        if (!buffer) { s.x[0] = 0; return; }
        const int whole = std::fabs(value) >= 1 ? static_cast<int>(std::floor(std::log10(std::fabs(value)))) + 1 : 1;
        char text[64];
        std::snprintf(text, sizeof text, "%.*f", std::max(0, digits - whole), value);
        std::string out(text);
        out.resize(std::min<std::size_t>(out.size(), 31));
        std::vector<std::uint8_t> bytes(out.begin(), out.end());
        bytes.push_back(0);
        m.write_bytes(buffer, bytes);
        s.x[0] = buffer;
    });
}

} // namespace alchemy::gui::completion_kit::media
