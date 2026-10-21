#include "alchemy/darwin/corehaptics/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// CoreHaptics: the AHAP key strings and the pattern value classes
// (CHHapticEngine is not provided).
// String constants carry the values of Apple's public headers; values
// marked "(unverified)" follow the framework's naming convention and could
// not be checked against a header. Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::corehaptics {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view core_haptics = "/System/Library/Frameworks/CoreHaptics.framework/Versions/A/CoreHaptics";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(core_haptics));
    string(core_haptics, "_CHHapticEventTypeHapticTransient", "HapticTransient");
    string(core_haptics, "_CHHapticEventTypeHapticContinuous", "HapticContinuous");
    string(core_haptics, "_CHHapticEventParameterIDHapticIntensity", "HapticIntensity");
    string(core_haptics, "_CHHapticEventParameterIDHapticSharpness", "HapticSharpness");
    string(core_haptics, "_CHHapticDynamicParameterIDHapticIntensityControl", "HapticIntensityControl");
    string(core_haptics, "_CHHapticDynamicParameterIDHapticSharpnessControl", "HapticSharpnessControl");
    const auto event_parameter = cls(core_haptics, "CHHapticEventParameter", "NSObject");
    object_properties(event_parameter, {"parameterID"});
    real_properties(event_parameter, {"value"}, true);
    method(event_parameter, "initWithParameterID:value:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "parameterID", s.x[2]);
        const auto state = properties(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->reals["value"] = fill::real32(s, 0);
    }, false, "@@:@f");
    const auto dynamic_parameter = cls(core_haptics, "CHHapticDynamicParameter", "NSObject");
    object_properties(dynamic_parameter, {"parameterID"});
    real_properties(dynamic_parameter, {"value"}, true);
    real_properties(dynamic_parameter, {"relativeTime"});
    method(dynamic_parameter, "initWithParameterID:value:relativeTime:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "parameterID", s.x[2]);
        const auto state = properties(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->reals["value"] = fill::real32(s, 0);
        state->reals["relativeTime"] = fill::real(s, 1);
    }, false, "@@:@fd");
    const auto haptic_event = cls(core_haptics, "CHHapticEvent", "NSObject");
    object_properties(haptic_event, {"type", "eventParameters"});
    real_properties(haptic_event, {"relativeTime", "duration"});
    method(haptic_event, "initWithEventType:parameters:relativeTime:duration:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "type", s.x[2]);
        store(s.x[0], "eventParameters", s.x[3]);
        const auto state = properties(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->reals["relativeTime"] = fill::real(s, 0);
        state->reals["duration"] = fill::real(s, 1);
    }, false, "@@:@@dd");
    const auto pattern = cls(core_haptics, "CHHapticPattern", "NSObject");
    method(pattern, "initWithEvents:parameters:error:", [](CpuState&, GuestMemory&) {});   // returns self
    method(pattern, "initWithEvents:parameterCurves:error:", [](CpuState&, GuestMemory&) {});
}

} // namespace

void install_corehaptics(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::corehaptics
