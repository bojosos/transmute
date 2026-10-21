#include "alchemy/darwin/coreml/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// CoreML: no model can be loaded or compiled (nil plus an NSError in the
// com.apple.CoreML domain, or the completion handler gets the error).
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::coreml {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view core_ml = "/System/Library/Frameworks/CoreML.framework/Versions/A/CoreML";
constexpr std::int64_t ml_model_error_generic = 0;

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(core_ml));
    for (const auto* name : {"MLFeatureValue", "MLMultiArray", "MLPredictionOptions", "MLCPUComputeDevice",
                             "MLGPUComputeDevice", "MLNeuralEngineComputeDevice", "MLComputePlan"})
        static_cast<void>(cls(core_ml, name, "NSObject"));
    const auto configuration = cls(core_ml, "MLModelConfiguration", "NSObject");
    integer_properties(configuration, {"computeUnits", "allowLowPrecisionAccumulationOnGPU"});
    object_properties(configuration, {"modelDisplayName", "preferredMetalDevice"});
    const auto model = cls(core_ml, "MLModel", "NSObject");
    const auto model_error = [this](Address out) {
        bridge->store_error(out, bridge->error("com.apple.CoreML", ml_model_error_generic, "Core ML is not available"), true);
    };
    method(model, "modelWithContentsOfURL:error:", [model_error](CpuState& s, GuestMemory&) { model_error(s.x[3]); s.x[0] = 0; }, true);
    method(model, "modelWithContentsOfURL:configuration:error:", [model_error](CpuState& s, GuestMemory&) { model_error(s.x[4]); s.x[0] = 0; }, true);
    method(model, "compileModelAtURL:error:", [model_error](CpuState& s, GuestMemory&) { model_error(s.x[3]); s.x[0] = 0; }, true);
    method(model, "loadContentsOfURL:configuration:completionHandler:", [this](CpuState& s, GuestMemory&) {
        complete_with_error(s.x[4], "com.apple.CoreML", ml_model_error_generic, "Core ML is not available");
    }, true, "v@:@@@?");
    method(model, "compileModelAtURL:completionHandler:", [this](CpuState& s, GuestMemory&) {
        complete_with_error(s.x[3], "com.apple.CoreML", ml_model_error_generic, "Core ML is not available");
    }, true, "v@:@@?");
}

} // namespace

void install_coreml(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::coreml
