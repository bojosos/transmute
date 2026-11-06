// MetalFX (src/alchemy/darwin/metalfx/): the identity, descriptors that keep
// their properties, and the documented "unsupported device" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/MetalFX.framework/Versions/A/MetalFX"

const Case metalfx_case("metalfx", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "MetalFX identity");
    require(f.process.resolve(LIBRARY, "_OBJC_CLASS_$_MTLFXSpatialScalerDescriptor").has_value(), "spatial descriptor export");
    const Address device = f.send(f.cls("NSObject"), "new");   // any object: no device is supported
    for (const auto* name : {"MTLFXSpatialScalerDescriptor", "MTLFXTemporalScalerDescriptor",
                             "MTLFXTemporalDenoisedScalerDescriptor", "MTLFXFrameInterpolatorDescriptor"})
        require(f.send(f.cls(name), "supportsDevice:", {device}) == 0, std::string(name) + " supportsDevice: is NO");

    const auto spatial = f.send(f.send(f.cls("MTLFXSpatialScalerDescriptor"), "alloc"), "init");
    f.send(spatial, "setColorTextureFormat:", {80});   // MTLPixelFormatBGRA8Unorm
    f.send(spatial, "setInputWidth:", {640});
    f.send(spatial, "setOutputWidth:", {1280});
    require(f.send(spatial, "colorTextureFormat") == 80 && f.send(spatial, "inputWidth") == 640 &&
                f.send(spatial, "outputWidth") == 1280 && f.send(spatial, "outputHeight") == 0,
            "the spatial descriptor keeps its properties");
    require(f.send(spatial, "newSpatialScalerWithDevice:", {device}) == 0, "no spatial scaler");

    const auto temporal = f.send(f.send(f.cls("MTLFXTemporalScalerDescriptor"), "alloc"), "init");
    require(f.send(temporal, "isAutoExposureEnabled") == 0, "auto exposure is off by default");
    f.send(temporal, "setAutoExposureEnabled:", {1});
    require(f.send(temporal, "isAutoExposureEnabled") == 1 && f.send(temporal, "autoExposureEnabled") == 1,
            "the temporal descriptor keeps its flags");
    require(f.send(temporal, "newTemporalScalerWithDevice:", {device}) == 0, "no temporal scaler");
    alchemy::CallRegs registers{};
    registers.x[2] = device;
    const auto scale = f.objc.send(f.cls("MTLFXTemporalScalerDescriptor"), "supportedInputContentMaxScaleForDevice:", registers);
    require(scale.s(0) == 1.0F, "an unsupported device's scale range is 1.0");
    f.send(temporal, "release");
    f.send(spatial, "release");
    f.send(device, "release");
});

#undef LIBRARY
} // namespace
