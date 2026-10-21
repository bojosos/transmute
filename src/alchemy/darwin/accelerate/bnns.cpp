#include "alchemy/darwin/accelerate/install.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include <cstdint>
#include <stdexcept>

// BNNS: no filter can be created. A renderer's denoiser library first imported it
// (moved from darwin/identities/framework_gaps.cpp).
namespace alchemy::accelerate {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view accelerate = "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate";

} // namespace

void install_accelerate_bnns(gui::InstallContext& context) {
    export_fill::Provider p(context.process);
    // No BNNS filter can be created: the BNNSFilterCreateLayer* functions
    // a renderer's denoiser library uses return NULL, the
    // documented failure result, so the caller sees the filter as unavailable.
    // Applying a filter fails (-1) and only NULL may be destroyed.
    for (const auto* name : {"_BNNSFilterCreateLayerConvolution", "_BNNSFilterCreateLayerPooling",
                             "_BNNSFilterCreateLayerFullyConnected", "_BNNSFilterCreateVectorActivation"})
        p.function(accelerate, name, [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    p.function(accelerate, "_BNNSFilterApply", [](CpuState&, GuestMemory&) -> std::uint64_t { return UINT64_MAX; });
    p.function(accelerate, "_BNNSFilterDestroy", [](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (s.x[0]) throw std::runtime_error("Alchemy Accelerate: BNNSFilterDestroy of an unknown filter");
        return 0;
    });
}

} // namespace alchemy::accelerate
