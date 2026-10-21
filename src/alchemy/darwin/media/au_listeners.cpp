#include "alchemy/darwin/media/media_support.hpp"

#include <algorithm>
#include <bit>
#include "alchemy/engine_adapter.hpp"
#include <map>
#include <mutex>
#include <vector>

// AudioToolbox parameter and event listeners (<AudioToolbox/AudioUnitUtilities.h>).
//
// Listeners are real objects that record the (object, parameter) and
// (object, event) registrations they are given. AUParameterSet sets the value
// through AudioUnitSetParameter and AUParameterListenerNotify reads it through
// AudioUnitGetParameter, as Apple's implementation does; their status is
// returned unchanged. Under Alchemy no audio unit instance can exist
// (AudioComponentFindNext finds no component), so those calls answer the
// AudioUnit layer's badComponentInstance and no listener is ever called.
// Property-change events register an AudioUnit property listener, which no
// unit can accept either. Successful notifications (when audio units exist)
// call the listeners on the notifying thread.
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
    float interval{}, granularity{};
    std::vector<Registration> registrations;
};

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

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
    support.function(lib, "_AUListenerDispose", [state, sp](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        if (!state->listeners.erase(s.x[0])) return status(s, param_err);
        sp->process.release_guest(s.x[0]);
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

    // Calls the listeners registered for `parameter` (except `sender`) with `value`, on this thread.
    const auto deliver = [state, sp](Address sender, const Parameter& parameter, Address parameter_at, float value) {
        std::vector<std::pair<Listener, Address>> targets;
        {
            std::lock_guard lock(state->mutex);
            for (const auto& [handle, listener] : state->listeners) {
                if (handle == sender) continue;
                for (const auto& r : listener.registrations)
                    if (r.event == 0 && r.parameter == parameter) targets.emplace_back(listener, r.object);
            }
        }
        auto& m = sp->memory();
        for (const auto& [listener, object] : targets) {
            CallRegs registers{};
            registers.x[0] = listener.user_data;
            registers.x[1] = object;
            registers.set_s(0, value);
            Address event{};
            if (listener.events) {
                // AUEventListenerProc(userData, object, const AudioUnitEvent *, UInt64 hostTime, Float32 value)
                event = sp->process.allocate_guest(32, 8);
                m.fill_bytes(event, 0, 32);   // kAudioUnitEvent_ParameterValueChange
                std::vector<std::uint8_t> bytes(24);
                m.read_bytes(parameter_at, bytes);
                m.write_bytes(event + 8, bytes);
                registers.x[2] = event;
                registers.x[3] = host_time_now();
            } else {
                // AUParameterListenerProc(userData, object, const AudioUnitParameter *, AudioUnitParameterValue)
                registers.x[2] = parameter_at;
            }
            static_cast<void>(EngineAdapter(sp->process.calling_engine()).call_regs(listener.proc, registers));
            if (event) sp->process.release_guest(event);
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
        if (!result) deliver(s.x[0], parameter, s.x[2], std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])));
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
        if (!result) deliver(s.x[0], parameter, s.x[2], read);
        status(s, result);
    });
}

} // namespace alchemy::gui::completion_kit::media
