#include "alchemy/darwin/opengl/opengl.hpp"

#include "alchemy/call_regs.hpp"
#include "alchemy/core_foundation.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/fill_provider.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/registration_audit.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// AppKit's OpenGL classes on CGL (cgl_contexts.cpp), as on macOS.
//
// - NSOpenGLPixelFormat holds a CGLPixelFormatObj (the NSOpenGLPFA values are CGL's); an
//   attribute list CGL cannot satisfy gives nil.
// - NSOpenGLContext holds a CGLContextObj; +currentContext is the context last made current on
//   the thread. -setView: gives the context a drawable: a framebuffer object of the view's size in
//   pixels (color, and depth/stencil or multisampling as the pixel format asks) that stands in for
//   framebuffer 0 (set_default_framebuffer). -flushBuffer reads it back into an IOSurface shown as
//   the contents of a sublayer of the view's layer, so the window compositor shows it like any
//   layer contents; -update follows the view's size.
// - NSOpenGLView makes its context on first use (+defaultPixelFormat unless given one), calls
//   -prepareOpenGL once, and draws through -displayLayer: (AppKit's display of a view whose layer
//   shows its own contents): the context is made current and -drawRect: runs, which flushes.
// - CAOpenGLLayer gets its pixel format and context from its own -copyCGLPixelFormatForDisplayMask:
//   and -copyCGLContextForPixelFormat: (subclasses override them), and draws into a drawable of
//   its bounds times contentsScale in pixels: -display makes the context current with the
//   drawable as framebuffer 0, asks -canDrawInCGLContext:..., runs -drawInCGLContext:... and
//   shows the result as the layer's own contents. -setNeedsDisplay displays on the main thread's
//   run loop; an asynchronous layer draws again every 1/60 s.
// - Unavailable: full-screen and off-screen contexts (-setFullScreen, -setOffScreen:...) and
//   -copyAttributesFromContext:withMask: stop with a message; -initWithData: answers nil.
namespace alchemy::opengl {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;
using Address = std::uint64_t;

// GL values.
constexpr std::uint32_t gl_framebuffer = 0x8D40, gl_read_framebuffer = 0x8CA8, gl_draw_framebuffer = 0x8CA9, gl_renderbuffer = 0x8D41,
    gl_rgba8 = 0x8058, gl_depth24_stencil8 = 0x88F0, gl_depth_component24 = 0x81A6, gl_color_attachment0 = 0x8CE0,
    gl_depth_attachment = 0x8D00, gl_depth_stencil_attachment = 0x821A, gl_framebuffer_binding = 0x8CA6,
    gl_read_framebuffer_binding = 0x8CAA, gl_color_buffer_bit = 0x4000, gl_nearest = 0x2600, gl_bgra = 0x80E1,
    gl_unsigned_int_8_8_8_8_rev = 0x8367, gl_pack_row_length = 0x0D02, gl_pack_skip_rows = 0x0D03, gl_pack_skip_pixels = 0x0D04,
    gl_pack_alignment = 0x0D05, gl_pixel_pack_buffer = 0x88EB, gl_pixel_pack_buffer_binding = 0x88ED, gl_read_buffer = 0x0C02;
// CGL values (CGLTypes.h).
constexpr std::uint32_t pfa_depth_size = 12, pfa_stencil_size = 13, pfa_samples = 56;
constexpr std::string_view iosurface_library = "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface";
// kIOSurfaceLock flags: 0 (read-write).
constexpr std::uint64_t bgra = 0x42475241;   // 'BGRA'

// A view drawable: the stand-in for framebuffer 0, and what shows it.
struct Drawable {
    Address view{};                    // not retained (the view owns its context's drawable)
    Address sublayer{};                // the layer showing the drawable (retained when owns_layer)
    bool owns_layer{true};             // a sublayer made for a view; a CAOpenGLLayer presents itself
    Address surface{};                 // retained IOSurface the sublayer shows
    bool shown{};                      // the sublayer has the surface as contents
    std::uint32_t framebuffer{}, color{}, depth{};
    std::uint32_t resolve_framebuffer{}, resolve_color{};   // multisampled drawables
    std::int32_t width{}, height{};
    std::uint32_t samples{};
    bool depth_stencil{}, depth_only{};
};

struct PixelFormatState final : ObjectPayload { Address cgl{}; };
struct ContextState final : ObjectPayload {
    Address cgl{};
    Address pixel_format{};            // NSOpenGLPixelFormat, retained
    std::unique_ptr<Drawable> drawable;
};
// CAOpenGLLayer, kept beside the layer (whose host state is QuartzCore's).
struct LayerState {
    ContextState gl;                   // the layer's context and drawable
    Address pixel_format{};            // CGLPixelFormatObj from -copyCGLPixelFormatForDisplayMask:
    bool asynchronous{};
    bool needs_display{};
    bool display_queued{};             // a -displayIfNeeded is queued on the main thread
    bool frame_scheduled{};            // the asynchronous layer's next frame is queued
};
struct ViewState final : ObjectPayload {
    Address pixel_format{};            // retained
    Address context{};                 // retained
    bool prepared{};
    bool best_resolution{};
};

class NsOpenGl : public std::enable_shared_from_this<NsOpenGl> {
public:
    explicit NsOpenGl(gui::InstallContext& context) : process_(context.process), objc_(context.objc), cf_(context.cf) {}
    void install();
    void install_layer(Address layer_class);

private:
    using Handler = std::function<void(CpuState&, GuestMemory&)>;
    void method(Address cls, std::string selector, Handler handler, bool class_method = false, std::string types = "@@:",
                const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(objc_.add_native_method(cls, std::move(selector),
            [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
            class_method, std::move(types)));
    }
    std::uint64_t call(std::string_view library, std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        const auto found = process_.resolve(library, name);
        if (!found) throw std::runtime_error("Alchemy OpenGL: missing " + std::string(name));
        return EngineAdapter(process_.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
    }
    std::int32_t cgl(std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        return static_cast<std::int32_t>(call(opengl_library, name, arguments));
    }
    Address send(Address receiver, std::string_view selector, CallRegs registers = {}) { return objc_.send(receiver, selector, registers).x0; }
    Address send1(Address receiver, std::string_view selector, Address argument) {
        CallRegs registers{};
        registers.x[2] = argument;
        return send(receiver, selector, registers);
    }
    std::int32_t describe(Address pixel_format_cgl, std::uint32_t attribute);
    // Runs `work` with `cgl_context` current on this thread, restoring the previous context.
    template <class F> void with_current(Address cgl_context, F work);
    void attach(Address context, Address view);
    // The framebuffer object of a drawable (its size, depth, stencil and samples set).
    void build(ContextState& state, Drawable& d, std::int32_t depth, std::int32_t stencil);
    // CAOpenGLLayer: its context and drawable (made or resized), then one frame.
    void display_layer(Address layer);
    void detach(Address context);
    void detach(ContextState& state);
    void resize(Drawable& drawable);
    std::pair<std::int32_t, std::int32_t> pixel_size(Address view);
    void flush(Address context);
    void flush(ContextState& state);
    // A layer's state (made on first use; `make` false: nullptr when there is none).
    std::shared_ptr<LayerState> layer(Address object, bool make = true);

    ProcessContext& process_;
    ObjcRuntime& objc_;
    CoreFoundation& cf_;
    Address pixel_format_class_{}, context_class_{}, view_class_{};
    std::mutex mutex_;
    std::map<Address, Address> by_cgl_;   // CGLContextObj -> NSOpenGLContext
    std::map<Address, std::shared_ptr<LayerState>> layers_;   // CAOpenGLLayer -> its state
};

std::shared_ptr<LayerState> NsOpenGl::layer(Address object, bool make) {
    std::lock_guard lock(mutex_);
    const auto found = layers_.find(object);
    if (found != layers_.end()) return found->second;
    if (!make) return nullptr;
    return layers_[object] = std::make_shared<LayerState>();
}

std::int32_t NsOpenGl::describe(Address pixel_format_cgl, std::uint32_t attribute) {
    const auto value = process_.allocate_guest(4, 4);
    process_.calling_engine().write32(value, 0);
    static_cast<void>(cgl("_CGLDescribePixelFormat", {pixel_format_cgl, 0, attribute, value}));
    const auto result = static_cast<std::int32_t>(process_.calling_engine().read32(value));
    process_.release_guest(value);
    return result;
}

template <class F>
void NsOpenGl::with_current(Address cgl_context, F work) {
    const auto previous = call(opengl_library, "_CGLGetCurrentContext", {});
    if (previous != cgl_context && cgl("_CGLSetCurrentContext", {cgl_context}) != 0) return;
    work();
    if (previous != cgl_context) static_cast<void>(cgl("_CGLSetCurrentContext", {previous}));
}

std::pair<std::int32_t, std::int32_t> NsOpenGl::pixel_size(Address view) {
    const auto bounds = objc_.send(view, "bounds");
    double width = bounds.d(2), height = bounds.d(3);
    bool best = false;
    if (const auto state = objc_.find_host_state<ViewState>(view)) best = state->best_resolution;
    if (best) {
        const auto window = send(view, "window");
        double scale = 1;
        if (window) scale = objc_.send(window, "backingScaleFactor").d(0);
        if (!(scale > 0)) scale = 1;
        width *= scale;
        height *= scale;
    }
    return {std::max(1, static_cast<std::int32_t>(std::lround(width))), std::max(1, static_cast<std::int32_t>(std::lround(height)))};
}

void NsOpenGl::resize(Drawable& d) {
    using RenderbufferStorage = void (*)(std::uint32_t, std::uint32_t, std::int32_t, std::int32_t);
    using RenderbufferStorageMultisample = void (*)(std::uint32_t, std::int32_t, std::uint32_t, std::int32_t, std::int32_t);
    using BindRenderbuffer = void (*)(std::uint32_t, std::uint32_t);
    const auto bind = gl_function<BindRenderbuffer>("glBindRenderbuffer");
    const auto storage = gl_function<RenderbufferStorage>("glRenderbufferStorage");
    const auto storage_multisample = gl_function<RenderbufferStorageMultisample>("glRenderbufferStorageMultisample");
    if (!bind || !storage) return;
    const auto store = [&](std::uint32_t renderbuffer, std::uint32_t format, std::uint32_t samples) {
        bind(gl_renderbuffer, renderbuffer);
        if (samples > 1 && storage_multisample) storage_multisample(gl_renderbuffer, static_cast<std::int32_t>(samples), format, d.width, d.height);
        else storage(gl_renderbuffer, format, d.width, d.height);
    };
    store(d.color, gl_rgba8, d.samples);
    if (d.depth) store(d.depth, d.depth_stencil ? gl_depth24_stencil8 : gl_depth_component24, d.samples);
    if (d.resolve_color) store(d.resolve_color, gl_rgba8, 0);
    bind(gl_renderbuffer, 0);
}

void NsOpenGl::build(ContextState& state, Drawable& d, std::int32_t depth, std::int32_t stencil) {
    auto* host_context = cgl_host_context(state.cgl);
    with_current(state.cgl, [&] {
        using Gen = void (*)(std::int32_t, std::uint32_t*);
        using BindFramebuffer = void (*)(std::uint32_t, std::uint32_t);
        using Attach = void (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
        using GetInteger = void (*)(std::uint32_t, std::int32_t*);
        using Viewport = void (*)(std::int32_t, std::int32_t, std::int32_t, std::int32_t);
        const auto gen_framebuffers = gl_function<Gen>("glGenFramebuffers");
        const auto gen_renderbuffers = gl_function<Gen>("glGenRenderbuffers");
        const auto bind_framebuffer = gl_function<BindFramebuffer>("glBindFramebuffer");
        const auto attach_renderbuffer = gl_function<Attach>("glFramebufferRenderbuffer");
        const auto get_integer = gl_function<GetInteger>("glGetIntegerv");
        const auto viewport = gl_function<Viewport>("glViewport");
        const auto scissor = gl_function<Viewport>("glScissor");
        if (!gen_framebuffers || !gen_renderbuffers || !bind_framebuffer || !attach_renderbuffer || !get_integer)
            throw std::runtime_error("unsupported: Alchemy: an NSOpenGLContext drawable needs framebuffer objects, which the host driver lacks");
        std::int32_t bound{};
        get_integer(gl_framebuffer_binding, &bound);
        gen_framebuffers(1, &d.framebuffer);
        gen_renderbuffers(1, &d.color);
        if (depth > 0 || stencil > 0) gen_renderbuffers(1, &d.depth);
        if (d.samples > 1) {
            gen_framebuffers(1, &d.resolve_framebuffer);
            gen_renderbuffers(1, &d.resolve_color);
        }
        resize(d);
        bind_framebuffer(gl_framebuffer, d.framebuffer);
        attach_renderbuffer(gl_framebuffer, gl_color_attachment0, gl_renderbuffer, d.color);
        if (d.depth)
            attach_renderbuffer(gl_framebuffer, d.depth_stencil ? gl_depth_stencil_attachment : gl_depth_attachment, gl_renderbuffer, d.depth);
        if (d.resolve_framebuffer) {
            bind_framebuffer(gl_framebuffer, d.resolve_framebuffer);
            attach_renderbuffer(gl_framebuffer, gl_color_attachment0, gl_renderbuffer, d.resolve_color);
        }
        // Framebuffer 0 was bound: the drawable is now framebuffer 0.
        bind_framebuffer(gl_framebuffer, bound == 0 ? d.framebuffer : static_cast<std::uint32_t>(bound));
        // A context's first drawable sets its viewport and scissor box to the drawable's size.
        if (viewport) viewport(0, 0, d.width, d.height);
        if (scissor) scissor(0, 0, d.width, d.height);
    });
    if (host_context) set_default_framebuffer(host_context, d.framebuffer);
}

void NsOpenGl::attach(Address context, Address view) {
    const auto state = objc_.find_host_state<ContextState>(context);
    if (!state || !state->cgl) return;
    if (state->drawable && state->drawable->view == view) return;
    detach(context);
    auto d = std::make_unique<Drawable>();
    d->view = view;
    const auto pixel_format = call(opengl_library, "_CGLGetPixelFormat", {state->cgl});
    const auto depth = describe(pixel_format, pfa_depth_size), stencil = describe(pixel_format, pfa_stencil_size);
    d->samples = static_cast<std::uint32_t>(std::max(0, describe(pixel_format, pfa_samples)));
    d->depth_stencil = stencil > 0;
    d->depth_only = depth > 0 && stencil == 0;
    std::tie(d->width, d->height) = pixel_size(view);
    build(*state, *d, depth, stencil);
    // The view shows the drawable through a sublayer of its layer.
    CallRegs yes{};
    yes.x[2] = 1;
    static_cast<void>(objc_.send(view, "setWantsLayer:", yes));
    const auto layer = send(view, "layer");
    if (const auto layer_class = objc_.find_class("CALayer"); layer && layer_class) {
        d->sublayer = send(send(layer_class, "layer"), "retain");
        const auto bounds = objc_.send(layer, "bounds");
        CallRegs frame{};
        for (unsigned i = 0; i < 4; ++i) frame.v[i] = bounds.v[i];
        static_cast<void>(objc_.send(d->sublayer, "setFrame:", frame));
        CallRegs mask{};
        mask.x[2] = 2 | 16;   // kCALayerWidthSizable | kCALayerHeightSizable
        static_cast<void>(objc_.send(d->sublayer, "setAutoresizingMask:", mask));
        static_cast<void>(send1(layer, "addSublayer:", d->sublayer));
    }
    state->drawable = std::move(d);
}

void NsOpenGl::display_layer(Address layer) {
    const auto layer_state = this->layer(layer);
    auto* state = &layer_state->gl;
    layer_state->needs_display = false;
    if (!state->cgl) {
        CallRegs mask{};
        mask.x[2] = 1;   // the main display's OpenGL display mask
        layer_state->pixel_format = objc_.send(layer, "copyCGLPixelFormatForDisplayMask:", mask).x0;
        if (!layer_state->pixel_format) return;
        state->cgl = send1(layer, "copyCGLContextForPixelFormat:", layer_state->pixel_format);
        if (!state->cgl) return;
    }
    // The drawable: the layer's bounds in pixels.
    const auto bounds = objc_.send(layer, "bounds");
    double scale = objc_.send(layer, "contentsScale").d(0);
    if (!(scale > 0)) scale = 1;
    const auto width = std::max(1, static_cast<std::int32_t>(std::lround(bounds.d(2) * scale)));
    const auto height = std::max(1, static_cast<std::int32_t>(std::lround(bounds.d(3) * scale)));
    if (!state->drawable) {
        auto d = std::make_unique<Drawable>();
        d->sublayer = layer;
        d->owns_layer = false;
        const auto pixel_format = call(opengl_library, "_CGLGetPixelFormat", {state->cgl});
        const auto depth = describe(pixel_format, pfa_depth_size), stencil = describe(pixel_format, pfa_stencil_size);
        d->samples = static_cast<std::uint32_t>(std::max(0, describe(pixel_format, pfa_samples)));
        d->depth_stencil = stencil > 0;
        d->depth_only = depth > 0 && stencil == 0;
        d->width = width;
        d->height = height;
        build(*state, *d, depth, stencil);
        state->drawable = std::move(d);
    } else if (state->drawable->width != width || state->drawable->height != height) {
        auto& d = *state->drawable;
        d.width = width;
        d.height = height;
        with_current(state->cgl, [&] {
            resize(d);
            if (const auto viewport = gl_function<void (*)(std::int32_t, std::int32_t, std::int32_t, std::int32_t)>("glViewport"))
                viewport(0, 0, width, height);
        });
    }
    // One frame: the drawable is framebuffer 0 while the layer draws.
    auto& memory = process_.calling_engine();
    const auto now = gui::monotonic_seconds();
    const auto stamp = process_.allocate_guest(80, 8);   // CVTimeStamp
    memory.fill_bytes(stamp, 0, 80);
    memory.write32(stamp + 4, 1'000'000'000);                              // videoTimeScale (nanoseconds)
    memory.write64(stamp + 16, static_cast<std::uint64_t>(now * 1e9));   // hostTime
    memory.write64(stamp + 24, std::bit_cast<std::uint64_t>(1.0));       // rateScalar
    memory.write64(stamp + 64, 2 | 4);                                    // hostTime and rateScalar are valid
    CallRegs frame{};
    frame.x[2] = state->cgl;
    frame.x[3] = layer_state->pixel_format;
    frame.x[4] = stamp;
    frame.set_d(0, now);
    bool drawn = false;
    with_current(state->cgl, [&] {
        if (const auto bind = gl_function<void (*)(std::uint32_t, std::uint32_t)>("glBindFramebuffer"))
            bind(gl_framebuffer, state->drawable->framebuffer);
        if ((objc_.send(layer, "canDrawInCGLContext:pixelFormat:forLayerTime:displayTime:", frame).x0 & 0xFF) == 0) return;
        static_cast<void>(objc_.send(layer, "drawInCGLContext:pixelFormat:forLayerTime:displayTime:", frame));
        drawn = true;
    });
    process_.release_guest(stamp);
    if (drawn) flush(*state);
    // An asynchronous layer draws its next frame in 1/60 s.
    if (layer_state->asynchronous && !layer_state->frame_scheduled) {
        layer_state->frame_scheduled = true;
        CallRegs later{};
        later.x[2] = objc_.selector("alchemyAsynchronousFrame");
        later.set_d(0, 1.0 / 60);
        static_cast<void>(objc_.send(layer, "performSelector:withObject:afterDelay:", later));
    }
}

void NsOpenGl::detach(Address context) {
    if (const auto state = objc_.find_host_state<ContextState>(context)) detach(*state);
}

void NsOpenGl::detach(ContextState& state) {
    if (!state.drawable) return;
    auto d = std::move(state.drawable);
    auto* host_context = cgl_host_context(state.cgl);
    with_current(state.cgl, [&] {
        using Delete = void (*)(std::int32_t, const std::uint32_t*);
        const auto delete_framebuffers = gl_function<Delete>("glDeleteFramebuffers");
        const auto delete_renderbuffers = gl_function<Delete>("glDeleteRenderbuffers");
        if (delete_framebuffers) {
            const std::uint32_t framebuffers[] = {d->framebuffer, d->resolve_framebuffer};
            delete_framebuffers(2, framebuffers);
        }
        if (delete_renderbuffers) {
            const std::uint32_t renderbuffers[] = {d->color, d->depth, d->resolve_color};
            delete_renderbuffers(3, renderbuffers);
        }
    });
    if (host_context) set_default_framebuffer(host_context, 0);
    if (d->sublayer && d->owns_layer) {
        static_cast<void>(send(d->sublayer, "removeFromSuperlayer"));
        static_cast<void>(send(d->sublayer, "release"));
    }
    if (d->surface) call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {d->surface});
}

void NsOpenGl::flush(Address context) {
    if (const auto state = objc_.find_host_state<ContextState>(context)) flush(*state);
}

void NsOpenGl::flush(ContextState& state_ref) {
    auto* state = &state_ref;
    if (!state->drawable) {
        with_current(state->cgl, [] {
            if (const auto flush = gl_function<void (*)()>("glFlush")) flush();
        });
        return;
    }
    auto& d = *state->drawable;
    std::vector<std::uint8_t> pixels;
    with_current(state->cgl, [&] {
        using GetInteger = void (*)(std::uint32_t, std::int32_t*);
        using PixelStore = void (*)(std::uint32_t, std::int32_t);
        using BindFramebuffer = void (*)(std::uint32_t, std::uint32_t);
        using BindBuffer = void (*)(std::uint32_t, std::uint32_t);
        using ReadBuffer = void (*)(std::uint32_t);
        using Blit = void (*)(std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                              std::uint32_t, std::uint32_t);
        using ReadPixels = void (*)(std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::uint32_t, std::uint32_t, void*);
        const auto get_integer = gl_function<GetInteger>("glGetIntegerv");
        const auto pixel_store = gl_function<PixelStore>("glPixelStorei");
        const auto bind_framebuffer = gl_function<BindFramebuffer>("glBindFramebuffer");
        const auto bind_buffer = gl_function<BindBuffer>("glBindBuffer");
        const auto read_buffer = gl_function<ReadBuffer>("glReadBuffer");
        const auto blit = gl_function<Blit>("glBlitFramebuffer");
        const auto read_pixels = gl_function<ReadPixels>("glReadPixels");
        if (!get_integer || !pixel_store || !bind_framebuffer || !read_pixels) return;
        std::int32_t read_binding{}, draw_binding{}, pack_buffer{}, read_mode{};
        get_integer(gl_read_framebuffer_binding, &read_binding);
        get_integer(gl_framebuffer_binding, &draw_binding);
        constexpr std::array<std::uint32_t, 4> pack{gl_pack_row_length, gl_pack_skip_rows, gl_pack_skip_pixels, gl_pack_alignment};
        std::array<std::int32_t, 4> saved{};
        for (std::size_t i = 0; i < pack.size(); ++i) get_integer(pack[i], &saved[i]);
        if (bind_buffer) {
            get_integer(gl_pixel_pack_buffer_binding, &pack_buffer);
            if (pack_buffer) bind_buffer(gl_pixel_pack_buffer, 0);
        }
        auto source = d.framebuffer;
        if (d.resolve_framebuffer && blit) {
            bind_framebuffer(gl_read_framebuffer, d.framebuffer);
            bind_framebuffer(gl_draw_framebuffer, d.resolve_framebuffer);
            blit(0, 0, d.width, d.height, 0, 0, d.width, d.height, gl_color_buffer_bit, gl_nearest);
            source = d.resolve_framebuffer;
        }
        bind_framebuffer(gl_read_framebuffer, source);
        if (read_buffer) {
            get_integer(gl_read_buffer, &read_mode);
            read_buffer(gl_color_attachment0);
        }
        pixel_store(gl_pack_row_length, 0);
        pixel_store(gl_pack_skip_rows, 0);
        pixel_store(gl_pack_skip_pixels, 0);
        pixel_store(gl_pack_alignment, 4);
        pixels.resize(static_cast<std::size_t>(d.width) * static_cast<std::size_t>(d.height) * 4);
        read_pixels(0, 0, d.width, d.height, gl_bgra, gl_unsigned_int_8_8_8_8_rev, pixels.data());
        for (std::size_t i = 0; i < pack.size(); ++i) pixel_store(pack[i], saved[i]);
        if (pack_buffer && bind_buffer) bind_buffer(gl_pixel_pack_buffer, static_cast<std::uint32_t>(pack_buffer));
        bind_framebuffer(gl_draw_framebuffer, static_cast<std::uint32_t>(draw_binding));
        bind_framebuffer(gl_read_framebuffer, static_cast<std::uint32_t>(read_binding));
        if (read_buffer && read_mode) read_buffer(static_cast<std::uint32_t>(read_mode));
    });
    if (pixels.empty()) return;
    // The surface the sublayer shows (rows top first, as GL's are bottom first).
    auto& memory = process_.calling_engine();
    const auto surface_width = d.surface ? call(iosurface_library, "_IOSurfaceGetWidth", {d.surface}) : 0;
    const auto surface_height = d.surface ? call(iosurface_library, "_IOSurfaceGetHeight", {d.surface}) : 0;
    if (!d.surface || surface_width != static_cast<std::uint64_t>(d.width) || surface_height != static_cast<std::uint64_t>(d.height)) {
        const auto number = [&](std::int64_t value) { return cf_.integer(value, 9 /* kCFNumberSInt64Type */); };
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 4> entries{{
            {cf_.string("IOSurfaceWidth"), number(d.width)}, {cf_.string("IOSurfaceHeight"), number(d.height)},
            {cf_.string("IOSurfaceBytesPerElement"), number(4)}, {cf_.string("IOSurfacePixelFormat"), number(static_cast<std::int64_t>(bgra))}}};
        const auto made = call(iosurface_library, "_IOSurfaceCreate", {cf_.dictionary(entries)});
        if (!made) return;
        if (d.surface) call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {d.surface});
        d.surface = made;
        d.shown = false;
    }
    const auto base = call(iosurface_library, "_IOSurfaceGetBaseAddress", {d.surface});
    const auto row = call(iosurface_library, "_IOSurfaceGetBytesPerRow", {d.surface});
    static_cast<void>(call(iosurface_library, "_IOSurfaceLock", {d.surface, 0, 0}));
    const auto line = static_cast<std::size_t>(d.width) * 4;
    for (std::int32_t y = 0; y < d.height; ++y)
        memory.write_bytes(base + static_cast<std::uint64_t>(y) * row,
                           std::span<const std::uint8_t>(pixels.data() + static_cast<std::size_t>(d.height - 1 - y) * line, line));
    static_cast<void>(call(iosurface_library, "_IOSurfaceUnlock", {d.surface, 0, 0}));
    if (!d.sublayer) return;
    if (!d.shown) {
        static_cast<void>(send1(d.sublayer, "setContents:", d.surface));
        d.shown = true;
    } else {
        static_cast<void>(send(d.sublayer, "setContentsChanged"));
    }
}

void NsOpenGl::install() {
    pixel_format_class_ = objc_.find_class("NSOpenGLPixelFormat");
    context_class_ = objc_.find_class("NSOpenGLContext");
    view_class_ = objc_.find_class("NSOpenGLView");
    if (!pixel_format_class_ || !context_class_ || !view_class_) return;   // AppKit's classes are not installed
    auto* self = this;
    const auto pixel_format_class = pixel_format_class_, context_class = context_class_, view_class = view_class_;

    // ---- NSOpenGLPixelFormat ----
    method(pixel_format_class, "initWithAttributes:", [self](CpuState& s, GuestMemory& m) {
        const auto out = self->process_.allocate_guest(16, 8);
        m.fill_bytes(out, 0, 16);
        const auto code = s.x[2] ? self->cgl("_CGLChoosePixelFormat", {s.x[2], out, out + 8}) : -1;
        const auto pix = code == 0 ? m.read64(out) : 0;
        self->process_.release_guest(out);
        if (!pix) {
            static_cast<void>(self->send(s.x[0], "release"));
            s.x[0] = 0;
            return;
        }
        self->objc_.host_state<PixelFormatState>(s.x[0])->cgl = pix;
    }, false, "@@:^I");
    method(pixel_format_class, "initWithCGLPixelFormatObj:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) {
            static_cast<void>(self->send(s.x[0], "release"));
            s.x[0] = 0;
            return;
        }
        static_cast<void>(self->call(opengl_library, "_CGLRetainPixelFormat", {s.x[2]}));
        self->objc_.host_state<PixelFormatState>(s.x[0])->cgl = s.x[2];
    }, false, "@@:^v");
    method(pixel_format_class, "initWithData:", [self](CpuState& s, GuestMemory&) {
        static_cast<void>(self->send(s.x[0], "release"));
        s.x[0] = 0;   // the serialized form is not CGL's to read
    });
    method(pixel_format_class, "CGLPixelFormatObj", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.find_host_state<PixelFormatState>(s.x[0]);
        s.x[0] = state ? state->cgl : 0;
    }, false, "^v@:");
    method(pixel_format_class, "numberOfVirtualScreens", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.find_host_state<PixelFormatState>(s.x[0]);
        s.x[0] = state && state->cgl ? 1 : 0;
    }, false, "i@:");
    method(pixel_format_class, "getValues:forAttribute:forVirtualScreen:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.find_host_state<PixelFormatState>(s.x[0]);
        if (state && state->cgl && s.x[2]) static_cast<void>(self->cgl("_CGLDescribePixelFormat", {state->cgl, s.x[4], s.x[3], s.x[2]}));
    }, false, "v@:^iIi");
    method(pixel_format_class, "dealloc", [self, pixel_format_class](CpuState& s, GuestMemory&) {
        if (const auto state = self->objc_.find_host_state<PixelFormatState>(s.x[0]); state && state->cgl)
            static_cast<void>(self->call(opengl_library, "_CGLReleasePixelFormat", {state->cgl}));
        static_cast<void>(self->objc_.send_super(s.x[0], pixel_format_class, self->objc_.selector("dealloc")));
    }, false, "v@:");

    // ---- NSOpenGLContext ----
    const auto adopt = [self](Address context, Address cgl_context) {
        const auto state = self->objc_.host_state<ContextState>(context);
        state->cgl = cgl_context;
        std::lock_guard lock(self->mutex_);
        self->by_cgl_[cgl_context] = context;
    };
    method(context_class, "initWithFormat:shareContext:", [self, adopt](CpuState& s, GuestMemory& m) {
        const auto format = s.x[2] ? self->objc_.find_host_state<PixelFormatState>(s.x[2]) : nullptr;
        const auto share = s.x[3] ? self->objc_.find_host_state<ContextState>(s.x[3]) : nullptr;
        Address made{};
        if (format && format->cgl) {
            const auto out = self->process_.allocate_guest(8, 8);
            m.write64(out, 0);
            if (self->cgl("_CGLCreateContext", {format->cgl, share ? share->cgl : 0, out}) == 0) made = m.read64(out);
            self->process_.release_guest(out);
        }
        if (!made) {
            static_cast<void>(self->send(s.x[0], "release"));
            s.x[0] = 0;
            return;
        }
        adopt(s.x[0], made);
        self->objc_.host_state<ContextState>(s.x[0])->pixel_format = self->send(s.x[2], "retain");
    }, false, "@@:@@");
    method(context_class, "initWithCGLContextObj:", [self, adopt, pixel_format_class](CpuState& s, GuestMemory&) {
        if (!s.x[2]) {
            static_cast<void>(self->send(s.x[0], "release"));
            s.x[0] = 0;
            return;
        }
        static_cast<void>(self->call(opengl_library, "_CGLRetainContext", {s.x[2]}));
        adopt(s.x[0], s.x[2]);
        const auto pix = self->call(opengl_library, "_CGLGetPixelFormat", {s.x[2]});
        self->objc_.host_state<ContextState>(s.x[0])->pixel_format =
            self->send1(self->objc_.allocate(pixel_format_class), "initWithCGLPixelFormatObj:", pix);
    }, false, "@@:^v");
    method(context_class, "CGLContextObj", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.find_host_state<ContextState>(s.x[0]);
        s.x[0] = state ? state->cgl : 0;
    }, false, "^v@:");
    method(context_class, "pixelFormat", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.find_host_state<ContextState>(s.x[0]);
        s.x[0] = state ? state->pixel_format : 0;
    });
    method(context_class, "makeCurrentContext", [self](CpuState& s, GuestMemory&) {
        if (const auto state = self->objc_.find_host_state<ContextState>(s.x[0]); state && state->cgl)
            static_cast<void>(self->cgl("_CGLSetCurrentContext", {state->cgl}));
    }, false, "v@:");
    method(context_class, "currentContext", [self](CpuState& s, GuestMemory&) {
        const auto cgl_context = self->call(opengl_library, "_CGLGetCurrentContext", {});
        std::lock_guard lock(self->mutex_);
        const auto found = self->by_cgl_.find(cgl_context);
        s.x[0] = found == self->by_cgl_.end() ? 0 : found->second;
    }, true);
    method(context_class, "clearCurrentContext", [self](CpuState&, GuestMemory&) {
        static_cast<void>(self->cgl("_CGLSetCurrentContext", {0}));
    }, true, "v@:");
    method(context_class, "flushBuffer", [self](CpuState& s, GuestMemory&) { self->flush(s.x[0]); }, false, "v@:");
    method(context_class, "update", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.find_host_state<ContextState>(s.x[0]);
        if (!state || !state->drawable) return;
        auto& d = *state->drawable;
        const auto [width, height] = self->pixel_size(d.view);
        if (width == d.width && height == d.height) return;
        d.width = width;
        d.height = height;
        self->with_current(state->cgl, [&] { self->resize(d); });
    }, false, "v@:");
    method(context_class, "setView:", [self](CpuState& s, GuestMemory&) {
        if (s.x[2]) self->attach(s.x[0], s.x[2]);
        else self->detach(s.x[0]);
    }, false, "v@:@");
    method(context_class, "view", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.find_host_state<ContextState>(s.x[0]);
        s.x[0] = state && state->drawable ? state->drawable->view : 0;
    });
    method(context_class, "clearDrawable", [self](CpuState& s, GuestMemory&) { self->detach(s.x[0]); }, false, "v@:");
    method(context_class, "setValues:forParameter:", [self](CpuState& s, GuestMemory&) {
        if (const auto state = self->objc_.find_host_state<ContextState>(s.x[0]); state && state->cgl)
            static_cast<void>(self->cgl("_CGLSetParameter", {state->cgl, s.x[3], s.x[2]}));
    }, false, "v@:^ii");
    method(context_class, "getValues:forParameter:", [self](CpuState& s, GuestMemory&) {
        if (const auto state = self->objc_.find_host_state<ContextState>(s.x[0]); state && state->cgl)
            static_cast<void>(self->cgl("_CGLGetParameter", {state->cgl, s.x[3], s.x[2]}));
    }, false, "v@:^ii");
    method(context_class, "currentVirtualScreen", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "i@:");
    method(context_class, "setCurrentVirtualScreen:", [](CpuState&, GuestMemory&) {}, false, "v@:i");
    for (const auto* selector : {"setFullScreen", "setOffScreen:width:height:rowbytes:", "copyAttributesFromContext:withMask:"}) {
        const std::string what = selector;
        method(context_class, selector, [what](CpuState&, GuestMemory&) {
            throw std::runtime_error("unsupported: Alchemy: -[NSOpenGLContext " + what +
                                     "] (full-screen and off-screen contexts and context copies are not provided)");
        }, false, "v@:");
    }
    method(context_class, "dealloc", [self, context_class](CpuState& s, GuestMemory&) {
        self->detach(s.x[0]);
        if (const auto state = self->objc_.find_host_state<ContextState>(s.x[0])) {
            if (state->cgl) {
                {
                    std::lock_guard lock(self->mutex_);
                    self->by_cgl_.erase(state->cgl);
                }
                static_cast<void>(self->call(opengl_library, "_CGLReleaseContext", {state->cgl}));
            }
            if (state->pixel_format) static_cast<void>(self->send(state->pixel_format, "release"));
        }
        static_cast<void>(self->objc_.send_super(s.x[0], context_class, self->objc_.selector("dealloc")));
    }, false, "v@:");

    // ---- NSOpenGLView ----
    method(view_class, "defaultPixelFormat", [self, pixel_format_class](CpuState& s, GuestMemory& m) {
        const auto attributes = self->process_.allocate_guest(4, 4);
        m.write32(attributes, 0);
        const auto format = self->send1(self->objc_.allocate(pixel_format_class), "initWithAttributes:", attributes);
        self->process_.release_guest(attributes);
        s.x[0] = format ? self->send(format, "autorelease") : 0;
    }, true);
    method(view_class, "initWithFrame:pixelFormat:", [self, view_class](CpuState& s, GuestMemory&) {
        CallRegs frame{};
        for (unsigned i = 0; i < 4; ++i) frame.v[i] = {s.v[i][0], s.v[i][1]};
        const auto format = s.x[2];
        const auto view = self->objc_.send_super(s.x[0], view_class, self->objc_.selector("initWithFrame:"), frame).x0;
        if (view) self->objc_.host_state<ViewState>(view)->pixel_format = format ? self->send(format, "retain") : 0;
        s.x[0] = view;
    }, false, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(view_class, "pixelFormat", [self, view_class](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.host_state<ViewState>(s.x[0]);
        if (!state->pixel_format) {
            state->pixel_format = self->send(self->send(view_class, "defaultPixelFormat"), "retain");
        }
        s.x[0] = state->pixel_format;
    });
    method(view_class, "setPixelFormat:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.host_state<ViewState>(s.x[0]);
        if (s.x[2]) static_cast<void>(self->send(s.x[2], "retain"));
        if (state->pixel_format) static_cast<void>(self->send(state->pixel_format, "release"));
        state->pixel_format = s.x[2];
    }, false, "v@:@");
    method(view_class, "openGLContext", [self, context_class](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto state = self->objc_.host_state<ViewState>(view);
        if (!state->context) {
            CallRegs r{};
            r.x[2] = self->send(view, "pixelFormat");
            r.x[3] = 0;
            const auto context = self->objc_.send(self->objc_.allocate(context_class), "initWithFormat:shareContext:", r).x0;
            if (!context) {
                s.x[0] = 0;
                return;
            }
            state->context = context;
            static_cast<void>(self->send1(context, "setView:", view));
        }
        if (!state->prepared) {
            state->prepared = true;
            static_cast<void>(self->send(state->context, "makeCurrentContext"));
            static_cast<void>(self->send(view, "prepareOpenGL"));
        }
        s.x[0] = state->context;
    });
    method(view_class, "setOpenGLContext:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.host_state<ViewState>(s.x[0]);
        if (s.x[2]) static_cast<void>(self->send(s.x[2], "retain"));
        if (state->context) {
            static_cast<void>(self->send(state->context, "clearDrawable"));
            static_cast<void>(self->send(state->context, "release"));
        }
        state->context = s.x[2];
        state->prepared = false;
        if (s.x[2]) static_cast<void>(self->send1(s.x[2], "setView:", s.x[0]));
    }, false, "v@:@");
    method(view_class, "clearGLContext", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.host_state<ViewState>(s.x[0]);
        if (!state->context) return;
        static_cast<void>(self->send(state->context, "clearDrawable"));
        static_cast<void>(self->send(state->context, "release"));
        state->context = 0;
        state->prepared = false;
    }, false, "v@:");
    method(view_class, "prepareOpenGL", [](CpuState&, GuestMemory&) {}, false, "v@:");
    method(view_class, "reshape", [](CpuState&, GuestMemory&) {}, false, "v@:");
    method(view_class, "update", [self](CpuState& s, GuestMemory&) {
        if (const auto state = self->objc_.find_host_state<ViewState>(s.x[0]); state && state->context)
            static_cast<void>(self->send(state->context, "update"));
    }, false, "v@:");
    method(view_class, "isOpaque", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "B@:");
    method(view_class, "wantsBestResolutionOpenGLSurface", [self](CpuState& s, GuestMemory&) {
        const auto state = self->objc_.find_host_state<ViewState>(s.x[0]);
        s.x[0] = state && state->best_resolution ? 1 : 0;
    }, false, "B@:");
    method(view_class, "setWantsBestResolutionOpenGLSurface:", [self](CpuState& s, GuestMemory&) {
        self->objc_.host_state<ViewState>(s.x[0])->best_resolution = (s.x[2] & 0xFF) != 0;
    }, false, "v@:B");
    // AppKit's display of the view: the context is current, then -drawRect: draws and flushes.
    method(view_class, "displayLayer:", [self](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto context = self->send(view, "openGLContext");
        if (!context) return;
        static_cast<void>(self->send(context, "makeCurrentContext"));
        static_cast<void>(self->send(context, "update"));
        const auto bounds = self->objc_.send(view, "bounds");
        CallRegs rect{};
        for (unsigned i = 0; i < 4; ++i) rect.v[i] = bounds.v[i];
        static_cast<void>(self->objc_.send(view, "drawRect:", rect));
    }, false, "v@:@");
    method(view_class, "dealloc", [self, view_class](CpuState& s, GuestMemory&) {
        if (const auto state = self->objc_.find_host_state<ViewState>(s.x[0])) {
            if (state->context) {
                static_cast<void>(self->send(state->context, "clearDrawable"));
                static_cast<void>(self->send(state->context, "release"));
            }
            if (state->pixel_format) static_cast<void>(self->send(state->pixel_format, "release"));
        }
        static_cast<void>(self->objc_.send_super(s.x[0], view_class, self->objc_.selector("dealloc")));
    }, false, "v@:");
}

void NsOpenGl::install_layer(Address layer_class) {
    auto* self = this;
    // CGLTypes.h: kCGLPFAColorSize, kCGLPFAAlphaSize, kCGLPFAAccelerated, kCGLPFADisplayMask.
    constexpr std::uint32_t pfa_color_size = 8, pfa_alpha_size = 11, pfa_accelerated = 73, pfa_display_mask = 84;
    method(layer_class, "copyCGLPixelFormatForDisplayMask:", [self](CpuState& s, GuestMemory& m) {
        const std::array<std::uint32_t, 8> attributes{pfa_display_mask, static_cast<std::uint32_t>(s.x[2]), pfa_accelerated,
                                                      pfa_color_size, 24, pfa_alpha_size, 8, 0};
        const auto list = self->process_.allocate_guest(attributes.size() * 4 + 16, 8);
        for (std::size_t i = 0; i < attributes.size(); ++i) m.write32(list + i * 4, attributes[i]);
        const auto out = list + attributes.size() * 4;
        m.fill_bytes(out, 0, 16);
        const auto code = self->cgl("_CGLChoosePixelFormat", {list, out, out + 8});
        s.x[0] = code == 0 ? m.read64(out) : 0;
        self->process_.release_guest(list);
    }, false, "^v@:I");
    method(layer_class, "copyCGLContextForPixelFormat:", [self](CpuState& s, GuestMemory& m) {
        const auto out = self->process_.allocate_guest(8, 8);
        m.write64(out, 0);
        const auto code = s.x[2] ? self->cgl("_CGLCreateContext", {s.x[2], 0, out}) : -1;
        s.x[0] = code == 0 ? m.read64(out) : 0;
        self->process_.release_guest(out);
    }, false, "^v@:^v");
    method(layer_class, "releaseCGLContext:", [self](CpuState& s, GuestMemory&) {
        if (s.x[2]) static_cast<void>(self->call(opengl_library, "_CGLReleaseContext", {s.x[2]}));
    }, false, "v@:^v");
    method(layer_class, "releaseCGLPixelFormat:", [self](CpuState& s, GuestMemory&) {
        if (s.x[2]) static_cast<void>(self->call(opengl_library, "_CGLReleasePixelFormat", {s.x[2]}));
    }, false, "v@:^v");
    method(layer_class, "canDrawInCGLContext:pixelFormat:forLayerTime:displayTime:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; },
           false, "B@:^v^vd^v");
    // The default drawing flushes the context.
    method(layer_class, "drawInCGLContext:pixelFormat:forLayerTime:displayTime:", [self](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        self->with_current(s.x[2], [] {
            if (const auto flush = gl_function<void (*)()>("glFlush")) flush();
        });
    }, false, "v@:^v^vd^v");
    method(layer_class, "isAsynchronous", [self](CpuState& s, GuestMemory&) {
        const auto state = self->layer(s.x[0], false);
        s.x[0] = state && state->asynchronous ? 1 : 0;
    }, false, "B@:");
    method(layer_class, "setAsynchronous:", [self](CpuState& s, GuestMemory&) {
        const auto state = self->layer(s.x[0]);
        const bool on = (s.x[2] & 0xFF) != 0;
        const bool start = on && !state->asynchronous;
        state->asynchronous = on;
        if (start) static_cast<void>(self->send(s.x[0], "setNeedsDisplay"));
    }, false, "v@:B");
    method(layer_class, "alchemyAsynchronousFrame", [self](CpuState& s, GuestMemory&) {
        const auto state = self->layer(s.x[0]);
        state->frame_scheduled = false;
        if (state->asynchronous) self->display_layer(s.x[0]);
    }, false, "v@:");
    method(layer_class, "display", [self](CpuState& s, GuestMemory&) { self->display_layer(s.x[0]); }, false, "v@:");
    // As at a Core Animation commit, a layer that needs display displays on the main thread's run loop.
    const auto needs_display = [self](CpuState& s, GuestMemory&) {
        const auto state = self->layer(s.x[0]);
        state->needs_display = true;
        if (std::exchange(state->display_queued, true)) return;
        CallRegs later{};
        later.x[2] = self->objc_.selector("displayIfNeeded");
        static_cast<void>(self->objc_.send(s.x[0], "performSelectorOnMainThread:withObject:waitUntilDone:", later));
    };
    method(layer_class, "setNeedsDisplay", needs_display, false, "v@:");
    method(layer_class, "setNeedsDisplayInRect:", needs_display, false, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(layer_class, "needsDisplay", [self](CpuState& s, GuestMemory&) {
        const auto state = self->layer(s.x[0], false);
        s.x[0] = state && state->needs_display ? 1 : 0;
    }, false, "B@:");
    method(layer_class, "displayIfNeeded", [self](CpuState& s, GuestMemory&) {
        const auto state = self->layer(s.x[0]);
        state->display_queued = false;
        if (state->needs_display) self->display_layer(s.x[0]);
    }, false, "v@:");
    method(layer_class, "dealloc", [self, layer_class](CpuState& s, GuestMemory&) {
        const auto layer = s.x[0];
        if (const auto layer_state = self->layer(layer, false)) {
            self->detach(layer_state->gl);
            if (layer_state->gl.cgl) static_cast<void>(self->send1(layer, "releaseCGLContext:", layer_state->gl.cgl));
            if (layer_state->pixel_format) static_cast<void>(self->send1(layer, "releaseCGLPixelFormat:", layer_state->pixel_format));
            std::lock_guard lock(self->mutex_);
            self->layers_.erase(layer);
        }
        static_cast<void>(self->objc_.send_super(layer, layer_class, self->objc_.selector("dealloc")));
    }, false, "v@:");
}

} // namespace

void install_nsopengl(gui::InstallContext& context) {
    const auto opengl = std::make_shared<NsOpenGl>(context);
    opengl->install();
    // CAOpenGLLayer is QuartzCore's; its drawing is OpenGL's.
    if (const auto layer = context.objc.find_class("CALayer")) {
        auto layer_class = context.objc.find_class("CAOpenGLLayer");
        if (!layer_class) layer_class = context.objc.register_native_class("CAOpenGLLayer", layer, 8);
        context.objc.publish_class("CAOpenGLLayer", layer_class, gui::fill::quartz_core);
        opengl->install_layer(layer_class);
    }
}

} // namespace alchemy::opengl
