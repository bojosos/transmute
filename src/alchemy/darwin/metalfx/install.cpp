#include "alchemy/darwin/metalfx/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <cctype>
#include <string>

// MetalFX: the scaler and frame interpolator descriptors, answering that no
// device supports them. MetalFX's spatial and temporal upscalers are Apple's
// own algorithms; Alchemy's Metal on Vulkan does not provide them, so
// +supportsDevice: is NO for every device and the factories return nil, the
// documented answers for an unsupported device. Clients (Godot 4.7's Metal
// renderer) check supportsDevice: and use their own upscaling instead.
// Godot first linked the framework (it did not load without it).
namespace alchemy::metalfx {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view metal_fx = "/System/Library/Frameworks/MetalFX.framework/Versions/A/MetalFX";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();

private:
    // A descriptor class: its integer (MTLPixelFormat, NSUInteger, enum, BOOL)
    // properties with `is<Name>` getters for the BOOL ones, single-precision
    // float properties, +supportsDevice: NO and the factory `factory` (and its
    // macOS 26 `...WithDevice:compiler:` form) returning nil.
    Address descriptor(std::string_view name, std::initializer_list<std::string_view> integers,
                       std::initializer_list<std::string_view> booleans, std::initializer_list<std::string_view> floats,
                       std::string_view factory);
};

Address Provider::descriptor(std::string_view name, std::initializer_list<std::string_view> integers,
                             std::initializer_list<std::string_view> booleans, std::initializer_list<std::string_view> floats,
                             std::string_view factory) {
    const auto target = cls(metal_fx, name, "NSObject");
    integer_properties(target, integers);
    integer_properties(target, booleans);
    for (const auto boolean : booleans) {
        std::string getter = "is" + std::string(boolean);
        getter[2] = static_cast<char>(std::toupper(static_cast<unsigned char>(getter[2])));
        method(target, getter, [this, key = std::string(boolean)](CpuState& s, GuestMemory&) {
            s.x[0] = stored_integer(s.x[0], key) != 0;
        }, false, "B@:");
    }
    if (floats.size()) real_properties(target, floats, true);
    cleanup_on_dealloc(target);
    method(target, "supportsDevice:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "B@:@");
    const auto nil = [](CpuState& s, GuestMemory&) { s.x[0] = 0; };
    method(target, std::string(factory) + ":", nil, false, "@@:@");
    method(target, std::string(factory) + ":compiler:", nil, false, "@@:@@");
    return target;
}

void Provider::install() {
    ProcessContext::add_provider_library(std::string(metal_fx));
    descriptor("MTLFXSpatialScalerDescriptor",
               {"colorTextureFormat", "outputTextureFormat", "inputWidth", "inputHeight", "outputWidth", "outputHeight",
                "colorProcessingMode"},
               {}, {}, "newSpatialScalerWithDevice");
    const auto temporal = descriptor("MTLFXTemporalScalerDescriptor",
        {"colorTextureFormat", "depthTextureFormat", "motionTextureFormat", "outputTextureFormat", "reactiveMaskTextureFormat",
         "inputWidth", "inputHeight", "outputWidth", "outputHeight"},
        {"autoExposureEnabled", "requiresSynchronousInitialization", "inputContentPropertiesEnabled",
         "reactiveMaskTextureEnabled"},
        {"inputContentMinScale", "inputContentMaxScale"}, "newTemporalScalerWithDevice");
    // The supported input scale range of a device MetalFX does not support:
    // 1.0 (no scaling) both ways.
    for (const auto selector : {"supportedInputContentMinScaleForDevice:", "supportedInputContentMaxScaleForDevice:"})
        method(temporal, selector, [](CpuState& s, GuestMemory&) { fill::return_real32(s, 1.0F); }, true, "f@:@");
    descriptor("MTLFXTemporalDenoisedScalerDescriptor",
               {"colorTextureFormat", "depthTextureFormat", "motionTextureFormat", "outputTextureFormat", "inputWidth",
                "inputHeight", "outputWidth", "outputHeight"},
               {"autoExposureEnabled", "requiresSynchronousInitialization"}, {}, "newTemporalDenoisedScalerWithDevice");
    descriptor("MTLFXFrameInterpolatorDescriptor",
               {"colorTextureFormat", "depthTextureFormat", "motionTextureFormat", "outputTextureFormat", "inputWidth",
                "inputHeight", "outputWidth", "outputHeight"},
               {}, {}, "newFrameInterpolatorWithDevice");
}

} // namespace

void install_metalfx(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::metalfx
