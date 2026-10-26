// OpenGL and AGL (src/alchemy/darwin/opengl/): the gl* entry points and CGL over the host's
// OpenGL driver (host::gl). Without a host driver (the null backend, a machine without OpenGL)
// only the answers without a context are checked.
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/host/gl.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using namespace fill_test;

constexpr std::string_view gl = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL", agl = "/System/Library/Frameworks/AGL.framework/Versions/A/AGL";
constexpr std::string_view iosurface = "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface";

// CGLTypes.h
constexpr Address pfa_double_buffer = 5, pfa_color_size = 8, pfa_alpha_size = 11, pfa_depth_size = 12, pfa_accelerated = 73,
    pfa_opengl_profile = 99, pfa_off_screen = 53;
constexpr Address profile_legacy = 0x1000, profile_4_1_core = 0x4100;
// OpenGL values.
constexpr Address gl_version = 0x1F02, gl_extensions = 0x1F03, gl_framebuffer = 0x8D40, gl_renderbuffer = 0x8D41, gl_rgba8 = 0x8058,
    gl_color_attachment0 = 0x8CE0, gl_framebuffer_complete = 0x8CD5, gl_color_buffer_bit = 0x4000, gl_rgba = 0x1908,
    gl_unsigned_byte = 0x1401, gl_read_framebuffer = 0x8CA8, gl_draw_framebuffer = 0x8CA9, gl_nearest = 0x2600,
    gl_vertex_shader = 0x8B31, gl_compile_status = 0x8B81, gl_array_buffer = 0x8892, gl_static_draw = 0x88E4,
    gl_texture_rectangle = 0x84F5, gl_bgra = 0x80E1, gl_unsigned_int_8_8_8_8_rev = 0x8367, gl_quads = 0x0007,
    gl_invalid_operation = 0x0502;

Address attributes(Fixture& f, std::initializer_list<std::uint32_t> values) {
    const auto block = f.block((values.size() + 1) * 4);
    std::size_t i = 0;
    for (const auto value : values) f.engine.write32(block + 4 * i++, value);
    return block;
}

std::array<std::uint8_t, 4> pixel(Fixture& f, Address buffer, std::size_t index = 0) {
    const auto word = f.engine.read32(buffer + index * 4);
    return {static_cast<std::uint8_t>(word), static_cast<std::uint8_t>(word >> 8), static_cast<std::uint8_t>(word >> 16),
            static_cast<std::uint8_t>(word >> 24)};
}
bool near(std::array<std::uint8_t, 4> got, std::array<int, 4> want) {
    for (std::size_t i = 0; i < 4; ++i)
        if (std::abs(static_cast<int>(got[i]) - want[i]) > 1) return false;
    return true;
}

void call_floats(Fixture& f, std::string_view name, std::initializer_list<float> values) {
    alchemy::CallRegs registers{};
    unsigned index = 0;
    for (const auto value : values) registers.set_s(index++, value);
    static_cast<void>(f.regs(gl, name, registers));
}

// A framebuffer with one RGBA8 renderbuffer of 4x4 pixels, bound for drawing and reading.
Address framebuffer(Fixture& f) {
    const auto names = f.block(8);
    f.call(gl, "_glGenFramebuffers", {1, names});
    f.call(gl, "_glGenRenderbuffers", {1, names + 4});
    const Address fb = f.engine.read32(names), rb = f.engine.read32(names + 4);
    f.call(gl, "_glBindFramebuffer", {gl_framebuffer, fb});
    f.call(gl, "_glBindRenderbuffer", {gl_renderbuffer, rb});
    f.call(gl, "_glRenderbufferStorage", {gl_renderbuffer, gl_rgba8, 4, 4});
    f.call(gl, "_glFramebufferRenderbuffer", {gl_framebuffer, gl_color_attachment0, gl_renderbuffer, rb});
    require(static_cast<std::uint32_t>(f.call(gl, "_glCheckFramebufferStatus", {gl_framebuffer})) == gl_framebuffer_complete,
            "framebuffer complete");
    return fb;
}

// One context of `profile`, current.
Address make_current(Fixture& f, std::initializer_list<std::uint32_t> list) {
    const auto out = f.block(16);
    require(f.status(gl, "_CGLChoosePixelFormat", {attributes(f, list), out, out + 8}) == 0, "CGLChoosePixelFormat");
    const auto pix = f.engine.read64(out);
    require(pix != 0 && f.engine.read32(out + 8) == 1, "one virtual screen");
    const auto ctx_out = f.block(8);
    require(f.status(gl, "_CGLCreateContext", {pix, 0, ctx_out}) == 0, "CGLCreateContext");
    const auto ctx = f.engine.read64(ctx_out);
    require(f.status(gl, "_CGLSetCurrentContext", {ctx}) == 0 && f.call(gl, "_CGLGetCurrentContext") == ctx, "current context");
    f.call(gl, "_CGLReleasePixelFormat", {pix});   // the context keeps it
    require(f.call(gl, "_CGLGetPixelFormat", {ctx}) == pix, "the context keeps its pixel format");
    return ctx;
}

void core_profile(Fixture& f) {
    const auto ctx = make_current(f, {pfa_opengl_profile, profile_4_1_core, pfa_color_size, 24, pfa_alpha_size, 8, pfa_double_buffer,
                                      pfa_accelerated, 0});
    const auto version = f.read_text(f.call(gl, "_glGetString", {gl_version}));
    require(!version.empty() && version[0] >= '4', "a core profile reports OpenGL 4: " + version);
    require(f.call(gl, "_glGetString", {gl_extensions}) == 0 && f.call(gl, "_glGetError") == 0x0500, "no extension string in a core profile");
    // Float arguments (v registers) and a readback.
    const auto fb = framebuffer(f);
    call_floats(f, "_glClearColor", {0.25f, 0.5f, 0.75f, 1.0f});
    f.call(gl, "_glClear", {gl_color_buffer_bit});
    const auto pixels = f.block(64);
    f.call(gl, "_glReadPixels", {0, 0, 4, 4, gl_rgba, gl_unsigned_byte, pixels});
    require(near(pixel(f, pixels, 5), {64, 128, 191, 255}), "glClearColor and glReadPixels");
    // Stack arguments: glBlitFramebuffer's mask and filter are the 9th and 10th integers.
    const auto second = framebuffer(f);
    f.call(gl, "_glBindFramebuffer", {gl_read_framebuffer, fb});
    alchemy::CallRegs blit{};
    for (unsigned i = 0; i < 8; ++i) blit.x[i] = (i % 4 < 2) ? 0 : 4;
    blit.push_stack(static_cast<std::uint32_t>(gl_color_buffer_bit));
    blit.push_stack(static_cast<std::uint32_t>(gl_nearest));
    static_cast<void>(f.regs(gl, "_glBlitFramebuffer", blit));
    f.call(gl, "_glBindFramebuffer", {gl_framebuffer, second});
    f.engine.fill_bytes(pixels, 0, 64);
    f.call(gl, "_glReadPixels", {0, 0, 4, 4, gl_rgba, gl_unsigned_byte, pixels});
    require(near(pixel(f, pixels, 15), {64, 128, 191, 255}), "glBlitFramebuffer");
    // Pointer arrays: a shader from guest strings.
    const auto source = f.text("#version 410\nvoid main() { gl_Position = vec4(0.0); }\n");
    const auto strings = f.block(8);
    f.engine.write64(strings, source);
    const auto shader = f.call(gl, "_glCreateShader", {gl_vertex_shader});
    f.call(gl, "_glShaderSource", {shader, 1, strings, 0});
    f.call(gl, "_glCompileShader", {shader});
    const auto value = f.block(4);
    f.call(gl, "_glGetShaderiv", {shader, gl_compile_status, value});
    require(f.engine.read32(value) == 1, "a GLSL 4.10 shader compiles");
    // Buffers: data in and out through guest memory.
    const auto names = f.block(4);
    f.call(gl, "_glGenBuffers", {1, names});
    f.call(gl, "_glBindBuffer", {gl_array_buffer, f.engine.read32(names)});
    const auto data = f.text("buffer contents");
    f.call(gl, "_glBufferData", {gl_array_buffer, 16, data, gl_static_draw});
    const auto back = f.block(16);
    f.call(gl, "_glGetBufferSubData", {gl_array_buffer, 0, 16, back});
    require(f.read_text(back) == "buffer contents", "glBufferData / glGetBufferSubData");
    // An IOSurface as a rectangle texture (CGLTexImageIOSurface2D), read back through a framebuffer.
    const auto number = [&](std::int64_t n) { return f.cf.integer(n, 9 /* kCFNumberSInt64Type */); };
    const std::array<std::pair<Address, Address>, 4> entries{{
        {f.cf.string("IOSurfaceWidth"), number(4)}, {f.cf.string("IOSurfaceHeight"), number(4)},
        {f.cf.string("IOSurfaceBytesPerElement"), number(4)}, {f.cf.string("IOSurfacePixelFormat"), number(0x42475241)}}};
    const auto surface = f.call(iosurface, "_IOSurfaceCreate", {f.cf.dictionary(entries)});
    require(surface != 0, "IOSurfaceCreate");
    const auto base = f.call(iosurface, "_IOSurfaceGetBaseAddress", {surface});
    const auto row = f.call(iosurface, "_IOSurfaceGetBytesPerRow", {surface});
    for (Address y = 0; y < 4; ++y)
        for (Address x = 0; x < 4; ++x) f.engine.write32(base + y * row + x * 4, 0xFF102030);   // BGRA bytes 30 20 10 FF
    f.call(gl, "_glGenTextures", {1, names});
    const auto texture = f.engine.read32(names);
    f.call(gl, "_glBindTexture", {gl_texture_rectangle, texture});
    alchemy::CallRegs upload{};
    upload.x = {ctx, gl_texture_rectangle, gl_rgba8, 4, 4, gl_bgra, gl_unsigned_int_8_8_8_8_rev, surface};
    upload.push_stack(static_cast<std::uint32_t>(0));
    require(static_cast<std::int32_t>(f.regs(gl, "_CGLTexImageIOSurface2D", upload).x0) == 0, "CGLTexImageIOSurface2D");
    f.call(gl, "_glFramebufferTexture2D", {gl_framebuffer, gl_color_attachment0, gl_texture_rectangle, texture, 0});
    f.call(gl, "_glReadPixels", {0, 0, 4, 4, gl_rgba, gl_unsigned_byte, pixels});
    require(near(pixel(f, pixels, 0), {0x10, 0x20, 0x30, 0xFF}), "the surface's pixels in the texture");
    require(f.call(gl, "_glGetError") == 0, "no GL error");
    require(f.status(gl, "_CGLSetCurrentContext", {0}) == 0 && f.call(gl, "_CGLGetCurrentContext") == 0, "no current context");
    require(f.status(gl, "_CGLDestroyContext", {ctx}) == 0, "CGLDestroyContext");
}

void legacy_profile(Fixture& f) {
    const auto ctx = make_current(f, {pfa_opengl_profile, profile_legacy, pfa_depth_size, 24, 0});
    require(f.call(gl, "_glGetString", {gl_extensions}) != 0, "a legacy profile has an extension string");
    // Immediate mode with float arguments, through the EXT framebuffer names.
    const auto names = f.block(8);
    f.call(gl, "_glGenFramebuffersEXT", {1, names});
    f.call(gl, "_glGenRenderbuffersEXT", {1, names + 4});
    f.call(gl, "_glBindFramebufferEXT", {gl_framebuffer, f.engine.read32(names)});
    f.call(gl, "_glBindRenderbufferEXT", {gl_renderbuffer, f.engine.read32(names + 4)});
    f.call(gl, "_glRenderbufferStorageEXT", {gl_renderbuffer, gl_rgba8, 4, 4});
    f.call(gl, "_glFramebufferRenderbufferEXT", {gl_framebuffer, gl_color_attachment0, gl_renderbuffer, f.engine.read32(names + 4)});
    f.call(gl, "_glViewport", {0, 0, 4, 4});
    call_floats(f, "_glClearColor", {0, 0, 0, 0});
    f.call(gl, "_glClear", {gl_color_buffer_bit});
    f.call(gl, "_glBegin", {gl_quads});
    call_floats(f, "_glColor4f", {1.0f, 0.5f, 0.0f, 1.0f});
    call_floats(f, "_glVertex2f", {-1, -1});
    call_floats(f, "_glVertex2f", {1, -1});
    call_floats(f, "_glVertex2f", {1, 1});
    call_floats(f, "_glVertex2f", {-1, 1});
    f.call(gl, "_glEnd");
    const auto pixels = f.block(64);
    f.call(gl, "_glReadPixels", {0, 0, 4, 4, gl_rgba, gl_unsigned_byte, pixels});
    require(near(pixel(f, pixels, 6), {255, 128, 0, 255}), "an immediate-mode quad");
    f.call(gl, "_CGLSetCurrentContext", {0});
    f.call(gl, "_CGLReleaseContext", {ctx});
}

// NSOpenGLPixelFormat / NSOpenGLContext with a view drawable: framebuffer 0 is the drawable,
// and -flushBuffer shows it as an IOSurface on a sublayer of the view's layer.
void appkit_contexts(Fixture& f) {
    const auto format = f.send(f.send(f.cls("NSOpenGLPixelFormat"), "alloc"), "initWithAttributes:",
                               {attributes(f, {pfa_opengl_profile, profile_4_1_core, pfa_double_buffer, pfa_depth_size, 24, 0})});
    require(format != 0 && f.send(format, "CGLPixelFormatObj") != 0, "NSOpenGLPixelFormat");
    const auto context = f.send(f.send(f.cls("NSOpenGLContext"), "alloc"), "initWithFormat:shareContext:", {format, 0});
    require(context != 0 && f.send(context, "CGLContextObj") != 0, "NSOpenGLContext");
    f.send(context, "makeCurrentContext");
    require(f.send(f.cls("NSOpenGLContext"), "currentContext") == context, "+currentContext");
    alchemy::CallRegs frame{};
    frame.set_d(0, 0);
    frame.set_d(1, 0);
    frame.set_d(2, 8);
    frame.set_d(3, 6);
    const auto view = f.objc.send(f.send(f.cls("NSView"), "alloc"), "initWithFrame:", frame).x0;
    require(view != 0, "an NSView");
    f.send(context, "setView:", {view});
    require(f.send(context, "view") == view, "-view");
    const auto binding = f.block(4);
    f.call(gl, "_glGetIntegerv", {0x8CA6 /* GL_FRAMEBUFFER_BINDING */, binding});
    require(f.engine.read32(binding) == 0, "the drawable is framebuffer 0");
    call_floats(f, "_glClearColor", {1.0f, 0.0f, 0.0f, 1.0f});
    f.call(gl, "_glClear", {gl_color_buffer_bit});
    f.send(context, "flushBuffer");
    const auto sublayers = f.send(f.send(view, "layer"), "sublayers");
    require(sublayers != 0 && f.send(sublayers, "count") == 1, "the drawable's sublayer");
    const auto surface = f.send(f.send(sublayers, "firstObject"), "contents");
    require(surface != 0 && f.call(iosurface, "_IOSurfaceGetWidth", {surface}) == 8 && f.call(iosurface, "_IOSurfaceGetHeight", {surface}) == 6,
            "an IOSurface of the view's size");
    const auto base = f.call(iosurface, "_IOSurfaceGetBaseAddress", {surface});
    require(near(pixel(f, base), {0, 0, 255, 255}), "the flushed pixels (BGRA red)");
    // Framebuffer 0 rebinds the drawable.
    const auto other = framebuffer(f);
    require(other != 0, "another framebuffer");
    f.call(gl, "_glBindFramebuffer", {gl_framebuffer, 0});
    f.call(gl, "_glGetIntegerv", {0x8CA6, binding});
    require(f.engine.read32(binding) == 0, "glBindFramebuffer(0) binds the drawable");
    f.send(context, "clearDrawable");
    require(f.send(context, "view") == 0 && f.send(f.send(f.send(view, "layer"), "sublayers"), "count") == 0, "-clearDrawable");
    f.send(f.cls("NSOpenGLContext"), "clearCurrentContext");
    require(f.send(f.cls("NSOpenGLContext"), "currentContext") == 0, "+clearCurrentContext");
    f.send(context, "release");
    f.send(format, "release");
}

// CAOpenGLLayer: a subclass's -drawInCGLContext:... draws into a drawable of bounds times
// contentsScale, framebuffer 0 while it draws, and the frame becomes the layer's contents.
void opengl_layer(Fixture& f) {
    const auto subclass = f.objc.register_native_class("AlchemyTestOpenGLLayer", f.cls("CAOpenGLLayer"), 8);
    struct Seen {
        int frames{};
        Address context{}, pixel_format{}, stamp{};
        std::uint64_t host_time{};
        double time{};
        std::uint32_t binding{1};
    };
    auto seen = std::make_shared<Seen>();
    static_cast<void>(f.objc.add_native_method(subclass, "drawInCGLContext:pixelFormat:forLayerTime:displayTime:",
        [&f, seen, subclass](transmute::CpuState& s, transmute::GuestMemory&) {
            ++seen->frames;
            seen->context = s.x[2];
            seen->pixel_format = s.x[3];
            seen->stamp = s.x[4];
            seen->host_time = s.x[4] ? f.engine.read64(s.x[4] + 16) : 0;   // the stamp lives for the call only
            seen->time = std::bit_cast<double>(s.v[0][0]);
            const auto binding = f.block(4);
            f.call(gl, "_glGetIntegerv", {0x8CA6 /* GL_FRAMEBUFFER_BINDING */, binding});
            seen->binding = f.engine.read32(binding);
            call_floats(f, "_glClearColor", {0.0f, 1.0f, 0.0f, 1.0f});
            f.call(gl, "_glClear", {gl_color_buffer_bit});
            alchemy::CallRegs up{};
            up.x[2] = s.x[2];
            up.x[3] = s.x[3];
            up.x[4] = s.x[4];
            up.v[0] = {s.v[0][0], s.v[0][1]};
            static_cast<void>(f.objc.send_super(s.x[0], subclass, f.objc.selector("drawInCGLContext:pixelFormat:forLayerTime:displayTime:"), up));
        }, false, "v@:^v^vd^v"));
    const auto layer = f.send(f.send(subclass, "alloc"), "init");
    require(layer != 0, "a CAOpenGLLayer subclass");
    alchemy::CallRegs bounds{};
    bounds.set_d(2, 5);
    bounds.set_d(3, 3);
    static_cast<void>(f.objc.send(layer, "setBounds:", bounds));
    alchemy::CallRegs scale{};
    scale.set_d(0, 2);
    static_cast<void>(f.objc.send(layer, "setContentsScale:", scale));
    require(f.send(layer, "isAsynchronous") == 0, "not asynchronous by default");
    f.send(layer, "display");
    require(seen->frames == 1 && seen->context != 0 && seen->pixel_format != 0 && seen->stamp != 0 && seen->time > 0,
            "-drawInCGLContext:pixelFormat:forLayerTime:displayTime: with the layer's context");
    require(seen->binding == 0, "the drawable is framebuffer 0 while the layer draws");
    require(seen->host_time != 0, "the display time's hostTime");
    const auto surface = f.send(layer, "contents");
    require(surface != 0 && f.call(iosurface, "_IOSurfaceGetWidth", {surface}) == 10 && f.call(iosurface, "_IOSurfaceGetHeight", {surface}) == 6,
            "an IOSurface of the layer's size in pixels");
    require(near(pixel(f, f.call(iosurface, "_IOSurfaceGetBaseAddress", {surface})), {0, 255, 0, 255}), "the drawn pixels (BGRA green)");
    require(f.call(gl, "_CGLGetCurrentContext") == 0, "the previous context is current again");
    // -setNeedsDisplay queues a display; -displayIfNeeded draws once.
    f.send(layer, "setNeedsDisplay");
    require(f.send(layer, "needsDisplay") == 1, "-needsDisplay");
    f.send(layer, "displayIfNeeded");
    f.send(layer, "displayIfNeeded");
    require(seen->frames == 2 && f.send(layer, "needsDisplay") == 0, "-displayIfNeeded draws once");
    f.send(layer, "release");
}

const Case opengl_case("opengl", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(agl), "AGL identity");
    for (const auto* name : {"_CGLChoosePixelFormat", "_CGLTexImageIOSurface2D", "_glGenFramebuffersEXT", "_glBindVertexArrayAPPLE",
                             "_glUniformMatrix4x3dv", "_glTexImage3D", "_glMultiTexCoord4fvARB", "_glFlushRenderAPPLE", "_glVertexAttribL4dv"})
        static_cast<void>(f.symbol(gl, name));
    // No current context: calls answer nothing, glGetError GL_INVALID_OPERATION.
    require(f.call(gl, "_glGetString", {gl_version}) == 0, "glGetString without a context");
    require(f.call(gl, "_glGetError") == gl_invalid_operation, "glGetError: GL_INVALID_OPERATION");
    require(f.call(gl, "_CGLGetCurrentContext") == 0, "no current context");
    require(f.read_text(f.call(gl, "_CGLErrorString", {10004})) == "invalid context", "CGLErrorString");
    const auto attributes_out = f.block(16);
    require(f.status(gl, "_CGLChoosePixelFormat", {attributes(f, {pfa_off_screen, 0}), attributes_out, attributes_out + 8}) == 0 &&
                f.engine.read64(attributes_out) == 0 && f.engine.read32(attributes_out + 8) == 0,
            "no off-screen renderer");
    require(f.status(gl, "_CGLChoosePixelFormat", {attributes(f, {12345, 0}), attributes_out, attributes_out + 8}) == 10000,
            "an unknown attribute: kCGLBadAttribute");
    if (!alchemy::host::gl::renderer()) {
        require(f.send(f.send(f.cls("NSOpenGLPixelFormat"), "alloc"), "initWithAttributes:", {f.block(8)}) == 0,
                "no NSOpenGL pixel format without a host driver");
        std::cout << "opengl: no host OpenGL driver; contexts not checked\n";
        return;
    }
    const auto renderer = alchemy::host::gl::renderer();
    std::cout << "opengl: host " << renderer->renderer << ", " << renderer->version << ", core " << renderer->major << "." << renderer->minor << "\n";
    core_profile(f);
    legacy_profile(f);
    appkit_contexts(f);
    opengl_layer(f);
});

} // namespace
