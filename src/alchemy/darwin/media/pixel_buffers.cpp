#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// CoreVideo: pixel buffers here are IOSurface-backed GPU interop surfaces,
// which a headless process cannot provide (kCVReturnUnsupported); the
// attribute keys are real.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

constexpr std::string_view core_video = darwin_framework_libraries[9];

// CVReturn.h
constexpr std::int32_t cv_return_unsupported = -6663;

class PixelBuffers final : public Provider {
public:
    using Provider::Provider;
    void install() {
        // CoreVideo: pixel buffers here are IOSurface-backed GPU interop
        // surfaces, which a headless process cannot provide.
        function(core_video, "_CVPixelBufferCreate", [](CpuState& s, GuestMemory& m) {
            if (s.x[5]) m.write64(s.x[5], 0);
            s.x[0] = status(cv_return_unsupported);
        });
        function(core_video, "_CVPixelBufferGetIOSurface", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        function(core_video, "_CVPixelBufferRelease", [this](CpuState& s, GuestMemory&) {
            if (s.x[0]) context.objects().release(s.x[0]);
            s.x[0] = 0;
        });
        string_symbol(core_video, "_kCVPixelBufferMetalCompatibilityKey", "MetalCompatibility");
        string_symbol(core_video, "_kCVPixelBufferOpenGLCompatibilityKey", "OpenGLCompatibility");
        string_symbol(core_video, "_kCVPixelBufferIOSurfacePropertiesKey", "IOSurfaceProperties");
        string_symbol(core_video, "_kCVPixelBufferPixelFormatTypeKey", "PixelFormatType");
        string_symbol(core_video, "_kCVPixelBufferWidthKey", "Width");
        string_symbol(core_video, "_kCVPixelBufferHeightKey", "Height");
    }
};

} // namespace

void install_media_pixel_buffers(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<PixelBuffers>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
