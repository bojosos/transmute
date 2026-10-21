// OpenGL and AGL (src/alchemy/darwin/opengl/): no OpenGL context can exist.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view gl = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL", agl = "/System/Library/Frameworks/AGL.framework/Versions/A/AGL";

const Case opengl_case("opengl", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(agl), "AGL identity");
    require(f.call(gl, "_glGetString", {0x1F00}) == 0, "glGetString without a context");
    require(f.call(gl, "_glGetError") == 0x0502, "glGetError: GL_INVALID_OPERATION");
    const auto attributes = f.block(8);
    require(f.send(f.send(f.cls("NSOpenGLPixelFormat"), "alloc"), "initWithAttributes:", {attributes}) == 0, "no pixel format");
});

} // namespace
