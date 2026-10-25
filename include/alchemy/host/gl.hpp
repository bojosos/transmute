#pragma once

// Host area "gl" (owner: fw-media). The rules are in include/alchemy/host/host.hpp. OpenGL
// contexts on the host's own OpenGL driver (WGL on win32), for the OpenGL framework
// (src/alchemy/darwin/opengl/). GUI-only: the backends are src/alchemy/host/<backend>/gui/gl.cpp,
// compiled into alchemy-fx.
//
// OpenGL itself is portable: its entry points cross this API untyped (proc), and the OpenGL
// framework calls them with the host's C calling convention. Only creating, sharing and binding
// contexts is host-specific.

#include "alchemy/host/host.hpp"

#include <cstdint>
#include <string>

namespace alchemy::host::gl {

// What a context's default framebuffer has, and its API profile. Bits are minimums (0: none
// needed); the backend picks the closest format it has.
struct Format {
    std::uint32_t color_bits{24}, alpha_bits{8}, depth_bits{}, stencil_bits{}, samples{};
    bool double_buffer{true};
    // A core profile context (OpenGL 3.2 or later, forward-compatible), else a compatibility
    // profile one (all of OpenGL 2.1 and the fixed-function API).
    bool core{};
};

// A context and its hidden drawable (a small default framebuffer that is never shown: the
// OpenGL framework draws into framebuffer objects of its own).
struct Context;

// A context sharing objects (textures, buffers, programs, ...) with `share` when given.
// Error::unsupported: the host has no OpenGL driver, or none with a core profile when one is
// asked for. Error::invalid_argument: `share` has the other profile.
[[nodiscard]] Result<Context*> create_context(const Format& format, Context* share);
// The context must not be current on any thread.
void destroy_context(Context* context) noexcept;
// Makes `context` current on the calling thread (nullptr: none).
[[nodiscard]] Status make_current(Context* context);
// The calling thread's current context (nullptr: none).
[[nodiscard]] Context* current() noexcept;
// The format the backend picked for `context`.
[[nodiscard]] Format format(const Context* context) noexcept;
// Presents the drawable's back buffer (a no-op for a single-buffered format). The drawable is
// hidden, so nothing shows: the caller reads pixels back.
void swap_buffers(Context* context) noexcept;

// The driver's entry point `name` ("glGenBuffers"), or nullptr when the driver lacks it. Entry
// points past OpenGL 1.1 are looked up through the current context, so call this with one
// current; the result then holds for every context of the process.
[[nodiscard]] void* proc(const char* name) noexcept;

// The host driver's strings, from a probe context made once (Error::unsupported: no driver).
struct Renderer {
    std::string vendor, renderer, version;
    std::uint32_t major{}, minor{};    // the highest core profile version
};
[[nodiscard]] Result<Renderer> renderer();

} // namespace alchemy::host::gl
