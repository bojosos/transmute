#include "alchemy/darwin/appkit/c_functions.hpp"
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/controls_internal.hpp"
#include "alchemy/gui/layer_contents.hpp"   // layer-backed views with -updateLayer (qt-vfb)
#include "alchemy/darwin/appkit/asset_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

// Software drawing of non-layer-backed views: window backing stores, the
// display pass (dirty rectangles, -drawRect: through the view hierarchy with
// flipped coordinates and clipping, presentation), NSGraphicsContext,
// -[NSView lockFocus], NSFont, the AppKit drawing functions (NSRectFill, ...),
// NSStringDrawing and NSImage drawing into the current context.
//
// A window "uses its backing store" when one of its visible views draws
// (overrides -drawRect: or is an AppKit control) and is not layer-hosting.
// Such windows are drawn into a per-window BGRA backing store in guest
// memory and presented with WindowServer::present_bitmap. Windows whose
// views are all layer-hosting (Blender: one CocoaMetalView with a
// CAMetalLayer that presents through Vulkan) keep the plain -drawRect: pass
// without any backing store, exactly as before. In a mixed window the
// CAMetalLayer views' rectangles are left out of the presentation.
//
// CoreGraphics: -[NSGraphicsContext CGContext] wraps the backing store with
// CGBitmapContextCreate (BGRA premultiplied, the CoreGraphics provider) and
// sets its CTM and clip for the view being drawn; AppKit's own drawing uses
// the host rasterizer of drawing.hpp on the same pixels.
namespace alchemy::gui::appkit {
namespace {
using drawing::Affine;
using drawing::Color;

constexpr std::uint32_t cg_bitmap_info = 2 /* kCGImageAlphaPremultipliedFirst */ | (2u << 12) /* kCGBitmapByteOrder32Little */;

Rect rect_from_points(Point a, Point b) {
    return {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::abs(b.x - a.x), std::abs(b.y - a.y)}};
}
bool rect_is_empty(Rect r) { return !(r.size.width > 0) || !(r.size.height > 0); }

// Rectangle difference: the parts of `r` outside `hole` (at most four).
std::vector<Rect> subtract(Rect r, Rect hole) {
    const auto overlap = drawing::intersect(r, hole);
    if (rect_is_empty(overlap)) return {r};
    std::vector<Rect> parts;
    const auto rx1 = r.origin.x + r.size.width, ry1 = r.origin.y + r.size.height;
    const auto ox1 = overlap.origin.x + overlap.size.width, oy1 = overlap.origin.y + overlap.size.height;
    if (overlap.origin.y > r.origin.y) parts.push_back({r.origin, {r.size.width, overlap.origin.y - r.origin.y}});
    if (oy1 < ry1) parts.push_back({{r.origin.x, oy1}, {r.size.width, ry1 - oy1}});
    if (overlap.origin.x > r.origin.x) parts.push_back({{r.origin.x, overlap.origin.y}, {overlap.origin.x - r.origin.x, overlap.size.height}});
    if (ox1 < rx1) parts.push_back({{ox1, overlap.origin.y}, {rx1 - ox1, overlap.size.height}});
    return parts;
}

// 8-bit meshed RGB(A) NSBitmapImageRep pixels as a premultiplied BGRA buffer
// (+[NSGraphicsContext graphicsContextWithBitmapImageRep:]).
class BitmapRepBuffer final : public drawing::PixelBuffer {
public:
    BitmapRepBuffer(GuestMemory& memory, const BitmapState& rep) : memory_(memory), rep_(rep) {}
    [[nodiscard]] std::uint32_t width() const override { return static_cast<std::uint32_t>(rep_.width); }
    [[nodiscard]] std::uint32_t height() const override { return static_cast<std::uint32_t>(rep_.height); }
    void read(std::uint32_t x, std::uint32_t y, std::span<std::uint32_t> out) const override {
        const auto bytes = static_cast<std::size_t>(rep_.bits_per_pixel / 8);
        std::vector<std::uint8_t> raw(out.size() * bytes);
        memory_.read_bytes(rep_.data + y * static_cast<Address>(rep_.bytes_per_row) + x * bytes, raw);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto* p = raw.data() + i * bytes;
            const bool first = rep_.has_alpha && (rep_.format & 1);
            const unsigned color = first ? 1 : 0;
            double r = p[color] / 255.0, g = p[color + 1] / 255.0, b = p[color + 2] / 255.0, a = 1;
            if (rep_.has_alpha) a = p[first ? 0 : 3] / 255.0;
            if (rep_.has_alpha && (rep_.format & 2)) { r *= a; g *= a; b *= a; }   // non-premultiplied storage
            const auto byte = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
            out[i] = (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
        }
    }
    void write(std::uint32_t x, std::uint32_t y, std::span<const std::uint32_t> in) override {
        const auto bytes = static_cast<std::size_t>(rep_.bits_per_pixel / 8);
        std::vector<std::uint8_t> raw(in.size() * bytes);
        memory_.read_bytes(rep_.data + y * static_cast<Address>(rep_.bytes_per_row) + x * bytes, raw);
        for (std::size_t i = 0; i < in.size(); ++i) {
            auto* p = raw.data() + i * bytes;
            const auto v = in[i];
            double a = (v >> 24) / 255.0, r = ((v >> 16) & 0xff) / 255.0, g = ((v >> 8) & 0xff) / 255.0, b = (v & 0xff) / 255.0;
            if (rep_.has_alpha && (rep_.format & 2) && a > 0) { r /= a; g /= a; b /= a; }
            const bool first = rep_.has_alpha && (rep_.format & 1);
            const unsigned color = first ? 1 : 0;
            const auto byte = [](double value) { return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255)); };
            p[color] = byte(r); p[color + 1] = byte(g); p[color + 2] = byte(b);
            if (rep_.has_alpha) p[first ? 0 : 3] = byte(a);
        }
        memory_.write_bytes(rep_.data + y * static_cast<Address>(rep_.bytes_per_row) + x * bytes, raw);
    }
private:
    GuestMemory& memory_;
    const BitmapState& rep_;
};
} // namespace

const Theme& theme(bool dark) {
    static const Theme light{
        false, Color::gray(0.925), Color::gray(1), Color::gray(1), Color::gray(0.84), Color::gray(0.70),
        Color::gray(0.1), Color::gray(0.45), Color::gray(0.62), {0, 0.478, 1, 1}, Color::gray(1), {0, 0.478, 1, 1},
        Color::gray(0.86), Color::gray(1), Color::gray(0.84), Color::gray(0.98), Color::gray(0.62), Color::gray(0.965),
        Color::gray(0.89), Color::gray(0.955), {0.23, 0.6, 1, 0.55}};
    static const Theme dark_theme{
        true, Color::gray(0.196), Color::gray(0.118), Color::gray(0.36), Color::gray(0.46), Color::gray(0.40),
        Color::gray(0.9), Color::gray(0.6), Color::gray(0.4), {0.04, 0.52, 1, 1}, Color::gray(1), {0.04, 0.42, 0.85, 1},
        Color::gray(0.27), Color::gray(0.118), Color::gray(0.30), Color::gray(0.17), Color::gray(0.45), Color::gray(0.2),
        Color::gray(0.25), Color::gray(0.165), {0.2, 0.55, 1, 0.6}};
    return dark ? dark_theme : light;
}

Address AppKit::method_imp(Address cls, std::string name, Handler handler, std::string types) {
    return objc.add_native_method(cls, std::move(name),
        [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
        false, std::move(types));
}

// ---- geometry ------------------------------------------------------------------------
Affine AppKit::view_transform(Address value) {
    const auto o = view_to_window(value, {0, 0});
    const auto x = view_to_window(value, {1, 0});
    const auto y = view_to_window(value, {0, 1});
    return {x.x - o.x, x.y - o.y, y.x - o.x, y.y - o.y, o.x, o.y};
}
Rect AppKit::view_rect_in_window(Address value, Rect rect) { return drawing::bounding_rect(view_transform(value), rect); }
Rect AppKit::window_rect_in_view(Address value, Rect rect) {
    const auto inverse = view_transform(value).inverted();
    if (!inverse) return {};
    return drawing::bounding_rect(*inverse, rect);
}
Rect AppKit::visible_rect(Address value) {
    const auto state = view_state(value);
    auto visible = view_rect_in_window(value, view_bounds(*state));
    for (auto current = state->superview; current; current = view_state(current)->superview)
        visible = drawing::intersect(visible, view_rect_in_window(current, view_bounds(*view_state(current))));
    if (rect_is_empty(visible)) return {window_rect_in_view(value, {visible.origin, {0, 0}}).origin, {0, 0}};
    return drawing::intersect(window_rect_in_view(value, visible), view_bounds(*state));
}

// ---- which views draw ------------------------------------------------------------------
bool AppKit::layer_hosting(Address value) {
    const auto layer = view_state(value)->layer;
    if (!layer) return false;
    const auto metal_layer = objc.find_class("CAMetalLayer");
    if (metal_layer && is_kind(layer, metal_layer)) return true;
    // A layer-backed view that updates its layer (-wantsUpdateLayer YES, Qt's
    // QNSView) shows its layer contents itself (layer_contents.hpp, qt-vfb).
    return layer_contents::view_updates_layer(objc, value);
}
bool AppKit::view_draws(Address value) {
    return objc.lookup_imp(value, sel("drawRect:")) != view_draw_rect_imp;
}
bool AppKit::window_uses_backing(Address target) {
    const auto state = window_state(target);
    if (state->software) return true;
    if (!state->content_view) return false;
    std::vector<Address> pending{state->content_view};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        const auto info = view_state(current);
        if (info->hidden) continue;
        if (!layer_hosting(current) && view_draws(current)) { state->software = true; return true; }
        pending.insert(pending.end(), info->subviews.begin(), info->subviews.end());
    }
    return false;
}

void AppKit::invalidate_view(Address value, std::optional<Rect> rect) {
    const auto state = view_state(value);
    const auto area = rect ? drawing::intersect(*rect, view_bounds(*state)) : view_bounds(*state);
    state->needs_display = true;
    state->dirty = drawing::unite(state->dirty, area);
}

// ---- backing stores --------------------------------------------------------------------------
std::shared_ptr<BackingStore> AppKit::ensure_backing(Address target) {
    const auto state = window_state(target);
    auto& server = require_window_server("window drawing");
    if (!state->id) fail("drawing into a window that has no window-server window");
    const auto points = server.content_rect(state->id).size;
    const auto scale = server.backing_scale(state->id);
    const auto width = static_cast<std::uint32_t>(std::max(1.0, std::ceil(points.width * scale - 1e-6)));
    const auto height = static_cast<std::uint32_t>(std::max(1.0, std::ceil(points.height * scale - 1e-6)));
    if (state->backing && state->backing->width == width && state->backing->height == height && state->backing->scale == scale)
        return state->backing;
    if (width > 16384 || height > 16384) fail("window backing store larger than 16384 pixels");
    release_backing(*state);
    auto backing = std::make_shared<BackingStore>();
    backing->width = width;
    backing->height = height;
    backing->stride = static_cast<std::size_t>(width) * 4;
    backing->scale = scale;
    backing->points = points;
    const auto bytes = static_cast<Address>(backing->stride) * height;
    backing->pixels = process.allocate_guest(bytes, 64);
    memory().fill_bytes(backing->pixels, 0, bytes);
    backing->mirror.assign(static_cast<std::size_t>(bytes), 0);
    // The window's NSGraphicsContext: window base points -> device pixels.
    auto buffer = std::make_shared<drawing::GuestBuffer>(memory(), backing->pixels, width, height, backing->stride);
    backing->graphics_context = make_context(buffer, false);
    const auto context = context_state(backing->graphics_context);
    context->backing = backing;
    context->window = target;
    context->base = {scale, 0, 0, -scale, 0, static_cast<double>(height)};
    context->canvas->state.ctm = context->base;
    state->backing = backing;
    state->dirty = {{0, 0}, points};
    return backing;
}

void AppKit::release_backing(WindowState& state) {
    const auto backing = std::exchange(state.backing, nullptr);
    if (!backing) return;
    release(std::exchange(backing->foreign_context, 0));
    if (backing->cg_context) {
        std::erase_if(cg_canvases, [&](const auto& entry) { return entry->cg == backing->cg_context; });
        const auto found = process.resolve(core_graphics_library, "_CGContextRelease");
        if (found) static_cast<void>(EngineAdapter(process.calling_engine()).call(found->address, std::array<Address, 1>{backing->cg_context}));
        backing->cg_context = 0;
    }
    if (backing->graphics_context) {
        if (const auto context = objc.find_host_state<GraphicsContextState>(backing->graphics_context)) {
            context->backing.reset();
            context->cg_context = 0;
            context->canvas.reset();
            context->buffer.reset();
        }
        if (current_context == backing->graphics_context) set_current_context(0);
        release(std::exchange(backing->graphics_context, 0));
    }
    if (backing->pixels) process.release_guest(std::exchange(backing->pixels, 0));
}

void AppKit::present_window(Address target, Rect window_rect) {
    if (screen_updates_disabled()) return;   // presented again when updates resume (c_functions.cpp)
    const auto state = window_state(target);
    const auto backing = state->backing;
    const auto server = window_server_or_null();
    if (!backing || !server || !state->id) return;
    const Rect content{{0, 0}, backing->points};
    window_rect = drawing::intersect(window_rect, content);
    if (rect_is_empty(window_rect)) return;
    // Refresh the host mirror rows the update covers.
    const auto& base = context_state(backing->graphics_context)->base;
    const auto rows = drawing::device_bounds(base, window_rect)
                          .intersect({0, 0, static_cast<int>(backing->width), static_cast<int>(backing->height)});
    if (rows.empty()) return;
    const auto offset = static_cast<std::size_t>(rows.y0) * backing->stride;
    const auto length = static_cast<std::size_t>(rows.y1 - rows.y0) * backing->stride;
    memory().read_bytes(backing->pixels + offset, std::span(backing->mirror.data() + offset, length));
    // CAMetalLayer views present themselves: leave their rectangles out.
    std::vector<Rect> parts{window_rect};
    if (state->content_view)
        for (const auto value : all_views(state->content_view)) {
            if (!layer_hosting(value) || view_state(value)->hidden) continue;
            const auto hole = view_rect_in_window(value, view_bounds(*view_state(value)));
            std::vector<Rect> next;
            for (const auto& part : parts) for (const auto& piece : subtract(part, hole)) next.push_back(piece);
            parts.swap(next);
        }
    const WindowServer::BitmapView view{backing->width, backing->height, backing->stride, backing->mirror.data()};
    for (const auto& part : parts) if (!rect_is_empty(part)) server->present_bitmap(state->id, view, part);
    capture_presented(target);
}

// ---- the display pass --------------------------------------------------------------------------
void AppKit::display_window(Address target, bool force) {
    const auto state = window_state(target);
    const auto content = state->content_view;
    if (!content) { state->needs_display = false; return; }
    if (draw_depth) return;   // -display from inside -drawRect: draws nothing new
    // The content view's tree, then the top strip's (toolbar, tab bar: window_tabs.cpp).
    std::vector<Address> roots{content};
    for (const auto strip_view : top_strip_views(target)) roots.push_back(strip_view);
    if (!window_uses_backing(target)) {
        bool pending = force || std::exchange(state->needs_display, false);
        for (const auto root : roots)
            for (const auto value : all_views(root)) pending = pending || view_state(value)->needs_display;
        if (pending) for (const auto root : roots) display_view(root);
        return;
    }
    if (!state->id || !state->visible || state->closed) return;   // dirty state waits for the window to show
    const auto backing = ensure_backing(target);
    const Rect whole{{0, 0}, backing->points};
    auto dirty = state->dirty;
    if (force || state->needs_display) dirty = whole;
    for (const auto root : roots)
    for (const auto value : all_views(root)) {
        const auto info = view_state(value);
        if (!info->needs_display) continue;
        const auto area = rect_is_empty(info->dirty) ? view_bounds(*info) : info->dirty;
        dirty = drawing::unite(dirty, view_rect_in_window(value, area));
        info->needs_display = false;
        info->dirty = {};
    }
    state->needs_display = false;
    state->dirty = {};
    dirty = drawing::intersect(dirty, whole);
    if (rect_is_empty(dirty)) return;
    // Whole device pixels.
    {
        const auto pixels = drawing::device_bounds(context_state(backing->graphics_context)->base, dirty);
        const auto s = backing->scale;
        dirty = drawing::intersect({{pixels.x0 / s, (backing->height - pixels.y1) / s},
                                    {(pixels.x1 - pixels.x0) / s, (pixels.y1 - pixels.y0) / s}}, whole);
    }
    ++draw_depth;
    struct Leave { unsigned& depth; ~Leave() { --depth; } } leave{draw_depth};
    // Window background.
    {
        const auto context = context_state(backing->graphics_context);
        auto& canvas = *context->canvas;
        canvas.state = {};
        canvas.state.ctm = context->base;
        canvas.state.clip = drawing::device_bounds(context->base, dirty)
                                .intersect({0, 0, static_cast<int>(backing->width), static_cast<int>(backing->height)});
        canvas.state.blend = drawing::Blend::copy;
        const auto background = state->background_color ? color_value(state->background_color, theme_for(target).window_background)
                                                         : theme_for(target).window_background;
        canvas.fill_rect(dirty, state->opaque ? background.with_alpha(1) : background);
    }
    // Views back to front, each clipped to its visible part of the dirty area.
    const std::function<void(Address, Rect)> draw_tree = [&](Address value, Rect clip) {
        const auto info = view_state(value);
        if (info->hidden) return;
        const auto frame = view_rect_in_window(value, view_bounds(*info));
        const auto visible = drawing::intersect(clip, frame);
        if (rect_is_empty(visible)) return;
        static_cast<void>(send_optional(value, "viewWillDraw"));
        if (layer_contents::view_updates_layer(objc, value)) {
            layer_contents::update_view_layer(objc, value);
        } else if (!layer_hosting(value) || view_draws(value)) {
            const auto previous = begin_view_drawing(value, visible);
            // Drawn-text notes for the autopress diagnostic (drawn_text.hpp).
            const auto previous_drawing = std::exchange(drawing_view, autopress ? value : 0);
            if (autopress) drawn_labels.erase(value);
            try {
                CallRegs registers;
                put_rect(registers, 0, drawing::intersect(window_rect_in_view(value, visible), view_bounds(*info)));
                static_cast<void>(send(value, "drawRect:", registers));
            } catch (...) { drawing_view = previous_drawing; end_view_drawing(previous); throw; }
            drawing_view = previous_drawing;
            end_view_drawing(previous);
        }
        const auto children = info->subviews;
        for (const auto child : children) draw_tree(child, visible);
    };
    for (const auto root : roots) draw_tree(root, dirty);
    present_window(target, dirty);
}

// ---- NSGraphicsContext ------------------------------------------------------------------------
std::shared_ptr<GraphicsContextState> AppKit::context_state(Address context) {
    if (!context) fail("nil NSGraphicsContext");
    return objc.host_state<GraphicsContextState>(context);
}
Address AppKit::make_context(std::shared_ptr<drawing::PixelBuffer> buffer, bool flipped) {
    const auto context = objc.allocate(graphics_context);
    const auto state = context_state(context);
    state->buffer = std::move(buffer);
    if (state->buffer) {
        state->canvas = std::make_unique<drawing::Canvas>(*state->buffer);
        // Unflipped user space: y up from the bottom row.
        state->base = {1, 0, 0, -1, 0, static_cast<double>(state->buffer->height())};
        if (flipped) state->base = {};
        state->canvas->state.ctm = state->base;
    }
    state->flipped = flipped;
    return context;
}
Address AppKit::bitmap_context(Address rep) {
    const auto info = objc.host_state<BitmapState>(rep);
    if (!info->data || info->bits_per_sample != 8 || info->planar || (info->format & 4) ||
        (info->samples_per_pixel != 3 && info->samples_per_pixel != 4) || info->bits_per_pixel < info->samples_per_pixel * 8)
        unsupported("NSGraphicsContext bitmap format (8-bit meshed RGB/RGBA only)");
    const auto context = make_context(std::make_shared<BitmapRepBuffer>(memory(), *info), false);
    const auto state = context_state(context);
    state->bitmap = retain(rep);
    // Points of the rep's size -> its pixels.
    const auto sx = info->width / std::max(1e-9, info->size.width), sy = info->height / std::max(1e-9, info->size.height);
    state->base = {sx, 0, 0, -sy, 0, static_cast<double>(info->height)};
    state->canvas->state.ctm = state->base;
    return context;
}
Address AppKit::window_context(Address target) {
    return ensure_backing(target)->graphics_context;
}
void AppKit::set_current_context(Address context) { assign(current_context, context); }
drawing::Canvas* AppKit::current_canvas() {
    if (current_context) {
        const auto state = context_state(current_context);
        if (state->canvas) return state->canvas.get();
        // An AppKit context wrapping a guest CGContextRef: its pixels.
        return state->cg_context ? canvas_over_cg(state->cg_context) : nullptr;
    }
    // The Quartz 2D layer's current context (a view drawn outside AppKit's
    // display pass, e.g. -cacheDisplayInRect:toBitmapImageRep:).
    if (!foreign_graphics()) return nullptr;
    const auto context = send(graphics_context, "currentContext").x0;
    if (!context) return nullptr;
    const auto cg = send(context, "CGContext").x0;
    return cg ? canvas_over_cg(cg) : nullptr;
}
drawing::Canvas& AppKit::require_canvas(std::string_view operation) {
    if (const auto canvas = current_canvas()) return *canvas;
    if (!current_context && !(foreign_graphics() && send(graphics_context, "currentContext").x0))
        fail(std::string(operation) + " without a current NSGraphicsContext (outside -drawRect: / -lockFocus)");
    unsupported(std::string(operation) + " into a graphics context without 32-bit BGRA/RGBA bitmap pixels (no AppKit pixel access)");
}

bool AppKit::current_flipped() {
    if (current_context) return context_state(current_context)->flipped;
    if (!foreign_graphics()) return false;
    const auto context = send(graphics_context, "currentContext").x0;
    return context && send_bool(context, "isFlipped");
}
// NSGraphicsContext is the Quartz 2D layer's (darwin/coregraphics/appkit_drawing.cpp),
// installed after AppKit: its contexts are reachable once it is.
bool AppKit::foreign_graphics() { return objc.responds(graphics_context, sel("currentContext")); }
// NSFont is CoreText's (darwin/coretext/font_exports.cpp), installed after
// AppKit: fonts are made through it once it is.
bool AppKit::foreign_fonts() { return objc.responds(font, sel("fontWithName:size:")); }

Address AppKit::begin_view_drawing(Address value, Rect window_clip) {
    const auto owner = view_state(value)->window;
    if (!owner) fail("drawing a view that is not in a window");
    const auto context = window_context(owner);
    const auto state = context_state(context);
    auto& canvas = *state->canvas;
    canvas.state = {};
    canvas.state.ctm = state->base.then_after(view_transform(value));
    canvas.state.clip = drawing::device_bounds(state->base, window_clip)
                            .intersect({0, 0, static_cast<int>(state->buffer->width()), static_cast<int>(state->buffer->height())});
    state->flipped = send_bool(value, "isFlipped");
    const auto previous = retain(current_context);
    set_current_context(context);
    state->in_view = true;
    ForeignFocus focus;
    try {
        apply_cg_state(*state);
        if (foreign_graphics()) {
            // The guest's +[NSGraphicsContext currentContext]: the Quartz
            // layer's context over the backing store's CGContext, which
            // context_cg sets up with this view's CTM and clip.
            const auto cg = context_cg(context);
            CallRegs make;
            make.x[2] = cg;
            make.x[3] = state->flipped ? 1 : 0;
            const auto wrapper = send(graphics_context, "graphicsContextWithCGContext:flipped:", make).x0;
            if (!wrapper) fail("the Quartz 2D layer refused an NSGraphicsContext over the window backing store");
            focus.previous = retain(send(graphics_context, "currentContext").x0);
            focus.active = true;
            static_cast<void>(send_id(graphics_context, "setCurrentContext:", {wrapper}));
        }
    } catch (...) {
        if (focus.active) release(focus.previous);
        restore_cg_state(*state);
        state->in_view = false;
        set_current_context(previous);
        release(previous);
        throw;
    }
    foreign_focus.push_back(focus);
    return previous;
}
void AppKit::end_view_drawing(Address previous) {
    if (!foreign_focus.empty()) {
        const auto focus = foreign_focus.back();
        foreign_focus.pop_back();
        if (focus.active) {
            static_cast<void>(send_id(graphics_context, "setCurrentContext:", {focus.previous}));
            release(focus.previous);
        }
    }
    if (current_context) if (const auto state = objc.find_host_state<GraphicsContextState>(current_context)) {
        state->in_view = false;
        restore_cg_state(*state);
    }
    set_current_context(previous);
    release(previous);
}

namespace {
CallResult cg_call(AppKit& kit, std::string_view symbol, const CallRegs& registers) {
    const auto found = kit.process.resolve(core_graphics_library, symbol);
    if (!found) unsupported("CoreGraphics " + std::string(symbol.substr(1)) + " (the CoreGraphics provider is not installed)");
    return EngineAdapter(kit.process.calling_engine()).call_regs(found->address, registers);
}
} // namespace

void AppKit::apply_cg_state(GraphicsContextState& state) {
    if (!state.cg_context || !state.canvas) return;
    const auto height = static_cast<double>(state.buffer->height());
    CallRegs save;
    save.x[0] = state.cg_context;
    static_cast<void>(cg_call(*this, "_CGContextSaveGState", save));
    ++state.cg_saved;
    // The clip first, in the identity (device, y up) space.
    const auto& clip = state.canvas->state.clip;
    CallRegs clip_call;
    clip_call.x[0] = state.cg_context;
    put_rect(clip_call, 0, {{static_cast<double>(clip.x0), height - clip.y1},
                            {static_cast<double>(std::max(0, clip.x1 - clip.x0)), static_cast<double>(std::max(0, clip.y1 - clip.y0))}});
    static_cast<void>(cg_call(*this, "_CGContextClipToRect", clip_call));
    // CTM: CG's y-up device space from the canvas's y-down one.
    const Affine flip{1, 0, 0, -1, 0, height};
    const auto t = flip.then_after(state.canvas->state.ctm);
    const std::array<double, 6> values{t.a, t.b, t.c, t.d, t.tx, t.ty};
    const auto block = process.allocate_guest(sizeof(values));
    try {
        memory().write_bytes(block, std::span(reinterpret_cast<const std::uint8_t*>(values.data()), sizeof(values)));
        CallRegs concat;
        concat.x[0] = state.cg_context;
        concat.x[1] = block;   // CGAffineTransform (48 bytes, not an HFA): passed by reference
        static_cast<void>(cg_call(*this, "_CGContextConcatCTM", concat));
    } catch (...) { process.release_guest(block); throw; }
    process.release_guest(block);
}
void AppKit::restore_cg_state(GraphicsContextState& state) {
    while (state.cg_saved) {
        --state.cg_saved;
        CallRegs restore;
        restore.x[0] = state.cg_context;
        static_cast<void>(cg_call(*this, "_CGContextRestoreGState", restore));
    }
}

Address AppKit::context_cg(Address context) {
    const auto state = context_state(context);
    if (state->cg_context) {
        if (state->in_view && !state->cg_saved && state->canvas) apply_cg_state(*state);
        return state->cg_context;
    }
    if (!state->buffer) return 0;
    // A CGBitmapContext over the same pixels (backing stores; bitmap reps
    // are drawn by the host rasterizer only).
    if (!state->backing) unsupported("-[NSGraphicsContext CGContext] for NSBitmapImageRep contexts");
    if (!state->backing->cg_context) {
        CallRegs space_call;
        const auto space = cg_call(*this, "_CGColorSpaceCreateDeviceRGB", space_call).x0;
        CallRegs create;
        create.x[0] = state->backing->pixels; create.x[1] = state->backing->width; create.x[2] = state->backing->height;
        create.x[3] = 8; create.x[4] = state->backing->stride; create.x[5] = space; create.x[6] = cg_bitmap_info;
        const auto created = cg_call(*this, "_CGBitmapContextCreate", create).x0;
        CallRegs release_space;
        release_space.x[0] = space;
        static_cast<void>(cg_call(*this, "_CGColorSpaceRelease", release_space));
        if (!created) fail("CGBitmapContextCreate refused the window backing store");
        state->backing->cg_context = created;
    }
    state->cg_context = state->backing->cg_context;
    // Inside a view's drawing: its CTM and clip.
    if (state->in_view && !state->cg_saved) apply_cg_state(*state);
    return state->cg_context;
}

drawing::Canvas* AppKit::canvas_over_cg(Address cg) {
    if (!cg) return nullptr;
    const auto value = [&](std::string_view symbol) {
        CallRegs call;
        call.x[0] = cg;
        return cg_call(*this, symbol, call).x0;
    };
    const auto data = value("_CGBitmapContextGetData");
    if (!data) return nullptr;   // not a bitmap context
    const auto width = static_cast<std::uint32_t>(value("_CGBitmapContextGetWidth"));
    const auto height = static_cast<std::uint32_t>(value("_CGBitmapContextGetHeight"));
    const auto stride = static_cast<std::size_t>(value("_CGBitmapContextGetBytesPerRow"));
    const auto bits = value("_CGBitmapContextGetBitsPerPixel");
    const auto info = static_cast<std::uint32_t>(value("_CGBitmapContextGetBitmapInfo"));
    const auto alpha = info & 0x1f, order = info & 0x7000;
    // 32-bit premultiplied (or skip) alpha, BGRA (little-endian, alpha first) or RGBA (alpha last).
    bool rgba{};
    if (bits != 32 || !width || !height) return nullptr;
    if (order == 0x2000 && (alpha == 2 || alpha == 6)) rgba = false;
    else if ((order == 0 || order == 0x4000) && (alpha == 1 || alpha == 5)) rgba = true;
    else return nullptr;
    CgCanvas* entry{};
    for (const auto& candidate : cg_canvases) if (candidate->cg == cg) entry = candidate.get();
    if (entry && (entry->data != data || entry->width != width || entry->height != height || entry->stride != stride || entry->rgba != rgba)) {
        std::erase_if(cg_canvases, [entry](const auto& candidate) { return candidate.get() == entry; });
        entry = nullptr;
    }
    if (!entry) {
        // Contexts come and go; keep a few (a canvas handed out stays valid
        // while it is in use: only idle entries are dropped).
        if (cg_canvases.size() >= 16)
            std::erase_if(cg_canvases, [](const auto& candidate) { return candidate->canvas->depth() == 0; });
        auto made = std::make_unique<CgCanvas>();
        made->cg = cg; made->data = data; made->width = width; made->height = height; made->stride = stride; made->rgba = rgba;
        made->buffer = std::make_shared<drawing::GuestBuffer>(memory(), data, width, height, stride, rgba);
        made->canvas = std::make_unique<drawing::Canvas>(*made->buffer);
        entry = made.get();
        cg_canvases.push_back(std::move(made));
    }
    auto& canvas = *entry->canvas;
    if (canvas.depth() == 0) {
        // The context's current CTM (user -> CG device, y up) and clip.
        const auto block = process.allocate_guest(48);
        Affine ctm;
        try {
            CallRegs get;
            get.x[0] = cg;
            get.indirect_result = block;   // CGAffineTransform (48 bytes) returns through x8
            static_cast<void>(cg_call(*this, "_CGContextGetCTM", get));
            std::array<double, 6> m{};
            memory().read_bytes(block, std::span(reinterpret_cast<std::uint8_t*>(m.data()), sizeof(m)));
            ctm = {m[0], m[1], m[2], m[3], m[4], m[5]};
        } catch (...) { process.release_guest(block); throw; }
        process.release_guest(block);
        CallRegs clip_call;
        clip_call.x[0] = cg;
        const auto clip = cg_call(*this, "_CGContextGetClipBoundingBox", clip_call);
        const Affine flip{1, 0, 0, -1, 0, static_cast<double>(height)};
        canvas.state = {};
        canvas.state.ctm = flip.then_after(ctm);
        canvas.state.clip = drawing::device_bounds(canvas.state.ctm, {{clip.d(0), clip.d(1)}, {clip.d(2), clip.d(3)}})
                                .intersect({0, 0, static_cast<int>(width), static_cast<int>(height)});
    }
    return &canvas;
}

// ---- values --------------------------------------------------------------------------------------
Color AppKit::color_value(Address value, Color fallback) {
    if (!value) return fallback;
    if (!is_kind(value, color)) {
        // Another color class (a guest NSColor subclass): its components.
        if (!responds(value, "getRed:green:blue:alpha:")) return fallback;
        const auto block = process.allocate_guest(32);
        CallRegs registers;
        registers.x[2] = block; registers.x[3] = block + 8; registers.x[4] = block + 16; registers.x[5] = block + 24;
        try { static_cast<void>(send(value, "getRed:green:blue:alpha:", registers)); }
        catch (...) { process.release_guest(block); throw; }
        Color result{std::bit_cast<double>(memory().read64(block)), std::bit_cast<double>(memory().read64(block + 8)),
                     std::bit_cast<double>(memory().read64(block + 16)), std::bit_cast<double>(memory().read64(block + 24))};
        process.release_guest(block);
        return result;
    }
    const auto state = objc.host_state<ColorState>(value);
    return {state->red, state->green, state->blue, state->alpha};
}

drawing::FontSpec AppKit::font_spec(Address value) {
    if (!value) return {};
    if (const auto state = objc.find_host_state<FontState>(value)) return state->spec;
    drawing::FontSpec spec;
    if (responds(value, "pointSize")) spec.size = send(value, "pointSize").d(0);
    if (responds(value, "fontName")) spec.name = text(send(value, "fontName").x0);
    return spec;
}
Address AppKit::font_object(const drawing::FontSpec& spec) {
    if (foreign_fonts()) {
        // NSFont belongs to CoreText (toll-free bridged CTFont): ask it.
        CallRegs make;
        make.set_d(0, spec.size);
        if (!spec.name.empty() && spec.name[0] == '.') {
            if (spec.monospace) {
                make.set_d(1, spec.bold ? 0.4 : 0.0);
                return send(font, "monospacedSystemFontOfSize:weight:", make).x0;
            }
            return send(font, spec.bold ? "boldSystemFontOfSize:" : "systemFontOfSize:", make).x0;
        }
        make.x[2] = cf.string(spec.name);
        const auto made = send(font, "fontWithName:size:", make).x0;
        release(make.x[2]);
        if (made) return made;
        return send(font, spec.monospace ? "userFixedPitchFontOfSize:" : "systemFontOfSize:", make).x0;
    }
    const auto value = objc.allocate(font);
    objc.host_state<FontState>(value)->spec = spec;
    return autorelease(value);
}

bool AppKit::dark_appearance(Address object) {
    const auto named_dark = [this](Address appearance_object) {
        if (!appearance_object) return std::optional<bool>{};
        const auto state = objc.find_host_state<AppearanceState>(appearance_object);
        if (!state) return std::optional<bool>{};
        return std::optional<bool>{text(state->name).find("Dark") != std::string::npos};
    };
    Address target = object;
    if (object && is_kind(object, view)) target = view_state(object)->window;
    if (target && is_kind(target, window)) if (const auto answer = named_dark(window_state(target)->appearance)) return *answer;
    if (app) if (const auto answer = named_dark(app_state()->appearance)) return *answer;
    return system_appearance_name().find("Dark") != std::string::npos;
}
const Theme& AppKit::theme_for(Address object) { return theme(dark_appearance(object)); }

std::u16string AppKit::string16(Address value) {
    if (!value) return {};
    if (cf.kind(value) == ObjectKind::string) {
        const auto units = cf.units(value);
        return {units.begin(), units.end()};
    }
    if (responds(value, "string")) return string16(send(value, "string").x0);   // NSAttributedString
    return drawing::utf16(text(value));
}

TextStyle AppKit::text_style(Address attributes, const TextStyle& defaults) {
    auto style = defaults;
    if (!attributes) return style;
    const auto lookup = [&](std::string_view key) -> Address {
        const auto name = string(key);
        Address value{};
        try { value = send_id(attributes, "objectForKey:", {name}); } catch (...) { release(name); throw; }
        release(name);
        return value;
    };
    if (const auto value = lookup("NSFont")) style.font = font_spec(value);
    if (const auto value = lookup("NSColor")) style.color = color_value(value, style.color);
    if (const auto value = lookup("NSBackgroundColor")) style.background = color_value(value);
    if (const auto value = lookup("NSUnderline")) {
        if (const auto number = cf.number_value(value)) style.underline = *number != 0;
    }
    if (const auto value = lookup("NSParagraphStyle")) {
        // -[NSParagraphStyle alignment] answers the guest's (arm64) NSTextAlignment:
        // left 0, center 1, right 2, justified 3, natural 4. TextStyle keeps
        // AppKit's internal numbering (center 2, right 1): swap the two.
        if (responds(value, "alignment")) {
            const auto alignment = static_cast<std::int64_t>(send(value, "alignment").x0);
            style.alignment = alignment == 1 ? 2 : alignment == 2 ? 1 : alignment;
        }
        if (responds(value, "lineBreakMode")) style.line_break = static_cast<std::int64_t>(send(value, "lineBreakMode").x0);
    }
    return style;
}

Size AppKit::measure_string(std::u16string_view value, const TextStyle& style, double width) {
    const auto metrics = drawing::font_metrics(style.font);
    const auto lines = drawing::layout_lines(style.font, value, width);
    double widest{};
    for (const auto& line : lines) widest = std::max(widest, line.width);
    return {std::ceil(widest), std::ceil(metrics.line_height() * static_cast<double>(lines.size()))};
}

Size AppKit::draw_string(std::u16string_view value, const TextStyle& style, Rect rect, bool wrap) {
    auto& canvas = require_canvas("NSString drawing");
    const bool flipped = current_flipped();
    const auto metrics = drawing::font_metrics(style.font);
    const auto lines = drawing::layout_lines(style.font, value, wrap ? rect.size.width : 0);
    const auto line_height = metrics.line_height();
    double widest{};
    for (const auto& line : lines) widest = std::max(widest, line.width);
    canvas.save();
    if (wrap) canvas.clip_to_rect(rect);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto& line = lines[index];
        const auto top_offset = line_height * static_cast<double>(index);
        // Stop below the rectangle (drawInRect: shows whole lines only).
        if (wrap && top_offset + line_height > rect.size.height + 0.5 && index > 0) break;
        double x = rect.origin.x;
        const auto room = rect.size.width - line.width;
        if (rect.size.width > 0) {
            if (style.alignment == 1) x += room;
            else if (style.alignment == 2) x += room / 2;
        }
        const double baseline = flipped ? rect.origin.y + top_offset + metrics.ascent
                                        : rect.origin.y + rect.size.height - top_offset - metrics.ascent;
        auto shown = value.substr(line.start, line.length);
        while (!shown.empty() && (shown.back() == u' ' || shown.back() == u'\r')) shown.remove_suffix(1);
        if (style.background) {
            const double top = flipped ? baseline - metrics.ascent : baseline - metrics.descent;
            canvas.fill_rect({{x, top}, {line.width, line_height}}, *style.background);
        }
        canvas.draw_text(shown, style.font, {x, baseline}, style.color);
        if (style.underline) {
            const auto y = flipped ? baseline + 1.5 : baseline - 1.5;
            canvas.fill_rect({{x, y - 0.5}, {line.width, 1}}, style.color);
        }
    }
    canvas.restore();
    return {std::ceil(widest), std::ceil(line_height * static_cast<double>(lines.size()))};
}

void AppKit::draw_image_in(Address image_object, Rect rect, double alpha, bool respect_flipped) {
    auto& canvas = require_canvas("NSImage drawing");
    const auto pixels = image_pixels(image_object);
    if (!pixels) return;   // an image without bitmap representations draws nothing
    const bool flipped = current_flipped();
    // The image's top row goes to the top of the rect on screen.
    canvas.draw_image(*pixels, rect, alpha, flipped && respect_flipped);
}

// ---- installation -------------------------------------------------------------------------------
void AppKit::install_graphics() {
    graphics_context = declare_class("NSGraphicsContext", "NSObject");
    font = declare_class("NSFont", "NSObject");
    font_manager = declare_class("NSFontManager", "NSObject");

    // ---- NSView display ----
    const auto v = view;
    view_draw_rect_imp = method_imp(v, "drawRect:", [](CpuState&, GuestMemory&) {}, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(v, "visibleRect", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        return_rect(s, view_state(self)->window ? visible_rect(self) : view_bounds(*view_state(self)));
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(v, "setNeedsDisplay:", [this](CpuState& s, GuestMemory&) {
        const auto state = view_state(s.x[0]);
        if (!bool_arg(s, 2)) { state->needs_display = false; state->dirty = {}; return; }
        invalidate_view(s.x[0]);
        if (state->layer) static_cast<void>(send_optional(state->layer, "setNeedsDisplay"));
    }, "v@:B");
    method(v, "setNeedsDisplayInRect:", [this](CpuState& s, GuestMemory&) { invalidate_view(s.x[0], rect_arg(s)); },
           "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto display_now = [this](Address self, std::optional<Rect> rect) {
        const auto owner = view_state(self)->window;
        if (!owner) return;
        if (window_uses_backing(owner)) {
            invalidate_view(self, rect);
            display_window(owner);
        } else {
            display_view(self);
        }
    };
    method(v, "display", [display_now](CpuState& s, GuestMemory&) { display_now(s.x[0], std::nullopt); }, "v@:");
    for (const auto* name : {"displayRect:", "displayRectIgnoringOpacity:"})
        method(v, name, [display_now](CpuState& s, GuestMemory&) { display_now(s.x[0], rect_arg(s)); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    for (const auto* name : {"displayIfNeeded", "displayIfNeededIgnoringOpacity"})
        method(v, name, [this](CpuState& s, GuestMemory&) {
            const auto owner = view_state(s.x[0])->window;
            if (!owner) return;
            bool pending{};
            for (const auto value : all_views(s.x[0])) pending = pending || view_state(value)->needs_display;
            if (!pending) return;
            if (window_uses_backing(owner)) display_window(owner);
            else display_view(s.x[0]);
        }, "v@:");
    method(v, "canDraw", [this](CpuState& s, GuestMemory&) {
        const auto owner = view_state(s.x[0])->window;
        return_bool(s, owner && window_state(owner)->id && !view_state(s.x[0])->hidden);
    }, "B@:");
    method(v, "lockFocus", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = view_state(self);
        const auto owner = state->window;
        if (!owner || !window_state(owner)->id) fail("-[NSView lockFocus] on a view outside a shown window");
        if (state->focus_locked) fail("nested -[NSView lockFocus]");
        window_state(owner)->software = true;
        state->focus_previous = begin_view_drawing(self, view_rect_in_window(self, visible_rect(self)));
        state->focus_locked = true;
    }, "v@:");
    method(v, "lockFocusIfCanDraw", [this](CpuState& s, GuestMemory&) {
        const auto owner = view_state(s.x[0])->window;
        if (!owner || !window_state(owner)->id || view_state(s.x[0])->hidden || view_state(s.x[0])->focus_locked) { s.x[0] = 0; return; }
        static_cast<void>(send(s.x[0], "lockFocus"));
        s.x[0] = 1;
    }, "B@:");
    method(v, "unlockFocus", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = view_state(self);
        if (!state->focus_locked) fail("-[NSView unlockFocus] without -lockFocus");
        state->focus_locked = false;
        end_view_drawing(std::exchange(state->focus_previous, 0));
        if (state->window) present_window(state->window, view_rect_in_window(self, visible_rect(self)));
    }, "v@:");
    method(v, "isOpaque", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(v, "opaqueAncestor", [this](CpuState& s, GuestMemory&) {
        auto current = s.x[0];
        while (current && !send_bool(current, "isOpaque") && view_state(current)->superview) current = view_state(current)->superview;
        s.x[0] = current;
    });
    method(v, "tag", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(view_state(s.x[0])->tag); }, "q@:");
    method(v, "viewWithTag:", [this](CpuState& s, GuestMemory&) {
        const auto wanted = static_cast<std::int64_t>(s.x[2]);
        for (const auto value : all_views(s.x[0]))
            if (static_cast<std::int64_t>(send(value, "tag").x0) == wanted) { s.x[0] = value; return; }
        s.x[0] = 0;
    }, "@@:q");

    // ---- NSWindow display ----
    const auto w = window;
    // Explicit display runs the layout engine's pass first (set_layout_engine).
    method(w, "display", [this](CpuState& s, GuestMemory&) {
        if (layout_engine) layout_engine->second.layout_window(s.x[0]);
        display_window(s.x[0], true);
    }, "v@:");
    method(w, "displayIfNeeded", [this](CpuState& s, GuestMemory&) {
        if (layout_engine) layout_engine->second.layout_window(s.x[0]);
        display_window(s.x[0]);
    }, "v@:");
    method(w, "flushWindow", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (state->backing) present_window(s.x[0], {{0, 0}, state->backing->points});
    }, "v@:");
    method(w, "flushWindowIfNeeded", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (state->backing) present_window(s.x[0], {{0, 0}, state->backing->points});
    }, "v@:");
    method(w, "graphicsContext", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (!state->id) { s.x[0] = 0; return; }
        const auto context = window_context(s.x[0]);
        if (!foreign_graphics()) { s.x[0] = context; return; }
        // The guest talks to the Quartz layer's NSGraphicsContext class.
        const auto backing = state->backing;
        if (!backing->foreign_context) {
            CallRegs make;
            make.x[2] = context_cg(context);
            make.x[3] = 0;
            backing->foreign_context = retain(send(graphics_context, "graphicsContextWithCGContext:flipped:", make).x0);
        }
        s.x[0] = backing->foreign_context;
    });

    // ---- NSGraphicsContext ----
    // The context classes, current context, graphics state, CGContext
    // bridge and drawing properties are Quartz 2D's (darwin/coregraphics/appkit_drawing.cpp).
    const auto g = graphics_context;
    class_method(g, "graphicsContextWithWindow:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !is_kind(s.x[2], window)) fail("+[NSGraphicsContext graphicsContextWithWindow:] needs an NSWindow");
        window_state(s.x[2])->software = true;
        s.x[0] = window_context(s.x[2]);
    });
    method(g, "focusStack", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });

    // ---- NSFont ----
    // NSFont and NSFontManager are CoreText's (darwin/coretext/font_exports.cpp);
    // -copy is the receiver, as for every immutable font.
    const auto f = font;
    method(f, "copy", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });

    // ---- AppKit drawing functions ----
    const auto both = [this](std::string symbol, Handler handler) {
        function(appkit_library, symbol, handler);
        function(cocoa_library, std::move(symbol), std::move(handler));
    };
    // NSRectFill, NSRectFillUsingOperation, NSRectFillList, NSFrameRect and
    // NSRectClip are Quartz 2D's; these draw on the current AppKit canvas.
    both("_NSEraseRect", [this](CpuState& s, GuestMemory&) {
        auto& canvas = require_canvas("NSEraseRect");
        const auto saved = canvas.state.blend;
        canvas.state.blend = drawing::Blend::copy;
        canvas.fill_rect(rect_arg(s), Color::gray(1));
        canvas.state.blend = saved;
    });
    const auto frame = [this](Rect r, double width) {
        auto& canvas = require_canvas("NSFrameRect");
        const auto color = canvas.state.fill;
        width = std::min({width, r.size.width / 2, r.size.height / 2});
        if (width <= 0) return;
        canvas.fill_rect({r.origin, {r.size.width, width}}, color);
        canvas.fill_rect({{r.origin.x, r.origin.y + r.size.height - width}, {r.size.width, width}}, color);
        canvas.fill_rect({{r.origin.x, r.origin.y + width}, {width, r.size.height - 2 * width}}, color);
        canvas.fill_rect({{r.origin.x + r.size.width - width, r.origin.y + width}, {width, r.size.height - 2 * width}}, color);
    };
    both("_NSFrameRectWithWidth", [frame](CpuState& s, GuestMemory&) { frame(rect_arg(s), d(s, 4)); });
    both("_NSFrameRectWithWidthUsingOperation", [frame](CpuState& s, GuestMemory&) { frame(rect_arg(s), d(s, 4)); });

    // NSStringDrawing, NSAttributedString drawing and NSImage drawing into
    // the current context are Quartz 2D's (darwin/coregraphics/appkit_drawing.cpp).
    install_bezier_paths();
    install_system_colors();
}

// ---- NSBezierPath construction and stroking -----------------------------------------------
void AppKit::install_bezier_paths() {
    // NSBezierPath construction, geometry, stroking and filling are Quartz
    // 2D's (darwin/coregraphics/appkit_drawing.cpp), over CGPath.
    const auto bp = bezier_path;
    const auto path_of = [this](Address value) { return objc.host_state<BezierState>(value); };
    method(bp, "init", [path_of](CpuState& s, GuestMemory&) { path_of(s.x[0])->general = true; });
}

// ---- NSColor system (semantic) colors: the current appearance's values ----------------------
void AppKit::install_system_colors() {
    // Named system colours with fixed light / dark values. `shared` ones are one
    // immortal object for the process (shared_color), taken in the appearance
    // of their first use; the dynamic ones are controls/appearance.cpp's.
    struct Named { const char* name; Color light, dark; bool shared; };
    static const Named colors[] = {
        {"alternateSelectedControlTextColor", Color::gray(1), Color::gray(1), true},
        {"selectedMenuItemTextColor", Color::gray(1), Color::gray(1), true},
        {"alternateSelectedControlColor", {0, 0.39, 0.88, 1}, {0, 0.35, 0.82, 1}, false},
        {"selectedContentBackgroundColor", {0, 0.39, 0.88, 1}, {0, 0.35, 0.82, 1}, true},
        {"keyboardFocusIndicatorColor", {0.23, 0.6, 1, 0.5}, {0.2, 0.55, 1, 0.5}, true},
        {"controlShadowColor", Color::gray(0.5), Color::gray(0), true},
        {"controlDarkShadowColor", Color::gray(0), Color::gray(0), true},
        {"controlLightHighlightColor", Color::gray(1), Color::gray(1), true},
        {"knobColor", Color::gray(0.5), Color::gray(0.5), true},
        {"shadowColor", Color::gray(0), Color::gray(0), true},
        {"findHighlightColor", {1, 1, 0, 1}, {1, 1, 0, 1}, true},
    };
    for (const auto& entry : colors) {
        const auto light = entry.light, dark_value = entry.dark;
        const bool shared = entry.shared;
        const std::string key = entry.name;
        class_method(color, entry.name, [this, light, dark_value, shared, key](CpuState& s, GuestMemory&) {
            const auto make = [&] {
                const auto value = dark_appearance(0) ? dark_value : light;
                return color_object(value.r, value.g, value.b, value.a);
            };
            // A shared colour keeps the appearance of its first answer.
            s.x[0] = shared ? shared_color(key, make) : make();
        });
    }
    // The dynamic system colours (controlTextColor, labelColor, ...: one
    // object each, resolved for the appearance being drawn).
    install_appearance(*this);
    // Named colors of a bundle's compiled asset catalog (Contents/Resources/
    // Assets.car), resolved for the current appearance when asked, like the
    // system colors above. A name the catalog lacks gives nil, as on macOS.
    struct Catalogs { std::mutex mutex; std::map<std::string, std::shared_ptr<const car::AssetCatalog>> by_path; };
    auto catalogs = std::make_shared<Catalogs>();
    const auto catalog_of = [this, catalogs](Address bundle) -> std::shared_ptr<const car::AssetCatalog> {
        if (!bundle) bundle = send(objc.find_class("NSBundle"), "mainBundle").x0;
        if (!bundle) return nullptr;
        const auto path_object = send_id(bundle, "pathForResource:ofType:", {auto_string("Assets"), auto_string("car")});
        if (!path_object) return nullptr;
        const auto path = text(path_object);
        std::lock_guard lock(catalogs->mutex);
        if (const auto found = catalogs->by_path.find(path); found != catalogs->by_path.end()) return found->second;
        const auto bytes = read_file(path);
        if (!bytes) fail("cannot read the asset catalog " + path);
        try {
            auto parsed = std::make_shared<const car::AssetCatalog>(car::AssetCatalog::parse(*bytes));
            catalogs->by_path.emplace(path, parsed);
            return parsed;
        } catch (const std::exception& error) {
            fail(path + ": " + error.what());
        }
    };
    const auto named_color = [this, catalog_of](Address name_object, Address bundle) -> Address {
        if (!name_object) return 0;
        const auto catalog = catalog_of(bundle);
        if (!catalog) return 0;
        const auto name = text(name_object);
        const auto value = catalog->color(name, dark_appearance(0) ? "NSAppearanceNameDarkAqua" : "NSAppearanceNameAqua");
        if (!value) return 0;
        const auto& c = value->components;
        switch (static_cast<car::ColorSpace>(value->color_space)) {
        case car::ColorSpace::srgb:
        case car::ColorSpace::wide_rgb:   // components kept as given: drawing is sRGB only (docs/ALCHEMY_QLAB_AUDIT.md)
            if (c.size() != 4) break;
            return color_object(c[0], c[1], c[2], c[3]);
        case car::ColorSpace::gray: {
            if (c.size() != 2) break;
            const auto result = color_object(c[0], c[0], c[0], c[1]);
            objc.host_state<ColorState>(result)->white = true;
            return result;
        }
        }
        unsupported("named color \"" + name + "\" in asset color space " + std::to_string(value->color_space) + " with "
                    + std::to_string(c.size()) + " components");
    };
    class_method(color, "colorNamed:", [named_color](CpuState& s, GuestMemory&) { s.x[0] = named_color(s.x[2], 0); }, "@@:@");
    class_method(color, "colorNamed:bundle:", [named_color](CpuState& s, GuestMemory&) { s.x[0] = named_color(s.x[2], s.x[3]); }, "@@:@@");
    // Nib colors name their catalog entry ("System", "controlColor").
    class_method(color, "colorWithCatalogName:colorName:", [this](CpuState& s, GuestMemory&) {
        const auto name = text(s.x[3]);
        if (!name.empty() && responds(color, name)) { s.x[0] = send(color, name).x0; return; }
        s.x[0] = 0;
    });
}

} // namespace alchemy::gui::appkit
