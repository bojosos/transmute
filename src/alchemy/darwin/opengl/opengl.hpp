#pragma once

// OpenGL.framework over the host's OpenGL driver (host::gl). Group-internal header.
//   gl_exports.cpp    the gl* entry points (gl_functions.inc), marshalled from the Apple arm64
//                     calling convention to the host's and called on the driver
//   cgl_contexts.cpp  CGL: pixel formats, contexts, the current context, parameters, renderer
//                     info, IOSurface textures
//   nsopengl.cpp      AppKit's NSOpenGLPixelFormat, NSOpenGLContext and NSOpenGLView on CGL, with
//                     view drawables (a framebuffer object shown through an IOSurface sublayer)
//   install.cpp       the installer row

#include "alchemy/gui/install.hpp"
#include "alchemy/host/gl.hpp"
#include "alchemy/process_context.hpp"

#include <cstdint>
#include <string_view>

namespace alchemy::opengl {

inline constexpr std::string_view opengl_library = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";

void install_gl_exports(ProcessContext& process);
void install_cgl(gui::InstallContext& context);
void install_nsopengl(gui::InstallContext& context);

// The host context of a CGLContextObj (nullptr: not one).
[[nodiscard]] host::gl::Context* cgl_host_context(std::uint64_t cgl_context);

// A context drawing to a view has no framebuffer 0 of its own here: a framebuffer object stands
// in for it. The gl* entry points then answer for framebuffer 0 with that object (binding it,
// reporting it as 0, and its color attachment as GL_BACK). 0: none (framebuffer 0 is the
// context's hidden drawable).
void set_default_framebuffer(host::gl::Context* context, std::uint32_t framebuffer);
[[nodiscard]] std::uint32_t default_framebuffer(host::gl::Context* context);

// The host driver's entry point `name` (or its alias), looked up through the current context and
// cached; nullptr when no context is current or the driver lacks it.
[[nodiscard]] void* gl_proc(std::string_view name);

// gl_proc as a host function of type F (nullptr as gl_proc).
template <class F>
F gl_function(std::string_view name) {
    return reinterpret_cast<F>(gl_proc(name));
}

} // namespace alchemy::opengl
