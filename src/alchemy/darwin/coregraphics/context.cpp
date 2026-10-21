#include "alchemy/darwin/coregraphics/context.hpp"
#include "alchemy/darwin/coretext/ct_internal.hpp"

#include <algorithm>
#include <cstring>
#include <numbers>

// The CGContext drawing pipeline over a host drawing surface (host::gpu; Direct2D on win32).
//
// Flush semantics: CoreGraphics draws synchronously into the bitmap a
// bitmap context was created with, so the guest may read (or write) its
// pixels at any time. Each drawing operation therefore copies the affected
// device rectangle from guest memory into the surface ("pull"), draws it
// on the host surface, and copies it back ("push"). Transparency layers draw into
// host-only surfaces and reach guest memory when they end.
//
// Simple operations (normal, copy, clear and plus-lighter blending, no
// shadow, no mask clip) draw straight into the surface with the host's clip
// layers. Everything else renders into a scratch surface and is composited
// on the CPU: shadows (Gaussian blur of the source alpha), every CGBlendMode
// (W3C compositing formulas) and mask clips.
namespace alchemy::gui::quartz2d {

ObjectRef ObjectRef::retain(Address object) {
    ObjectRef result;
    if (!object) return result;
    auto* q = provider();
    if (!q) fail("Quartz provider is not installed");
    result.holder_ = std::make_shared<Holder>();
    result.holder_->object = q->retain(object);
    return result;
}
ObjectRef ObjectRef::adopt(Address object) {
    ObjectRef result;
    if (!object) return result;
    result.holder_ = std::make_shared<Holder>();
    result.holder_->object = object;
    return result;
}
namespace {
thread_local unsigned provider_scope_depth = 0;
} // namespace
ProviderScope::ProviderScope() noexcept { ++provider_scope_depth; }
ProviderScope::~ProviderScope() { --provider_scope_depth; }
bool ProviderScope::active() noexcept { return provider_scope_depth != 0; }
Handler scoped(Handler handler) {
    return [handler = std::move(handler)](CpuState& s, GuestMemory& m) {
        const ProviderScope scope;
        handler(s, m);
    };
}

ObjectRef::Holder::~Holder() {
    if (!object || !ProviderScope::active()) return;
    if (auto* q = provider()) {
        try { q->release(object); } catch (...) {}
    }
}

namespace {

Affine flip(const ContextState& c) { return {1, 0, 0, -1, 0, static_cast<double>(c.layout.height)}; }
Surface& target(ContextState& c) { return c.layers.empty() ? *c.surface : *c.layers.back().surface; }
bool guest_backed(const ContextState& c) { return c.layers.empty() && c.data; }
PixelRect full(const ContextState& c) { return {0, 0, static_cast<std::int32_t>(c.layout.width), static_cast<std::int32_t>(c.layout.height)}; }

void pull(Quartz& q, ContextState& c, const PixelRect& r) {
    if (!guest_backed(c) || r.empty()) return;
    const auto locked = c.surface->lock(r, true);
    const auto bpp = pixel_bytes(c.layout);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(r.width()) * bpp);
    auto& memory = q.memory();
    for (auto y = r.y0; y < r.y1; ++y) {
        memory.read_bytes(c.data + static_cast<Address>(y) * c.layout.bytes_per_row + static_cast<Address>(r.x0) * bpp, row);
        row_to_pbgra(c.layout, row.data(), r.width(), locked.at(r.x0, y, r));
    }
}
void push(Quartz& q, ContextState& c, const PixelRect& r) {
    if (!guest_backed(c) || r.empty()) return;
    const auto locked = c.surface->lock(r, false);
    const auto bpp = pixel_bytes(c.layout);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(r.width()) * bpp);
    auto& memory = q.memory();
    for (auto y = r.y0; y < r.y1; ++y) {
        pbgra_to_row(c.layout, locked.at(r.x0, y, r), r.width(), row.data());
        memory.write_bytes(c.data + static_cast<Address>(y) * c.layout.bytes_per_row + static_cast<Address>(r.x0) * bpp, row);
    }
}

Rect clip_device_bounds(const ContextState& c) {
    Rect bounds{{0, 0}, {static_cast<double>(c.layout.width), static_cast<double>(c.layout.height)}};
    for (const auto& clip : c.g.clips) bounds = rect_intersection(bounds, clip->device);
    return bounds;
}
bool antialiasing(const GState& g) { return g.antialias && g.allows_antialias; }
bool has_mask_clip(const GState& g) {
    return std::any_of(g.clips.begin(), g.clips.end(), [](const auto& clip) { return clip->kind == ClipItem::Kind::mask; });
}

// Pushes the non-mask clips; returns how many were pushed (pop with pop_clips).
struct PushedClip { bool axis{}; };
std::vector<PushedClip> push_clips(host::gpu::Surface& rt, const GState& g) {
    std::vector<PushedClip> pushed;
    for (const auto& clip : g.clips) {
        if (clip->kind == ClipItem::Kind::rect) {
            rt.set_transform({});
            rt.push_axis_clip(to_box(clip->device), clip->antialias);
            pushed.push_back({true});
        } else if (clip->kind == ClipItem::Kind::geometry) {
            rt.set_transform({});
            check(rt.push_clip(clip->geometry, clip->antialias), "creating a clip layer");
            pushed.push_back({false});
        }
    }
    return pushed;
}
void pop_clips(host::gpu::Surface& rt, const std::vector<PushedClip>& pushed) {
    for (auto it = pushed.rbegin(); it != pushed.rend(); ++it) {
        if (it->axis) rt.pop_axis_clip();
        else rt.pop_clip();
    }
}

std::shared_ptr<Surface> scratch_surface(std::shared_ptr<Surface>& slot, const Surface& like) {
    if (!slot || slot->width() != like.width() || slot->height() != like.height())
        slot = std::make_shared<Surface>(like.width(), like.height());
    return slot;
}

// ---- compositing ------------------------------------------------------------------------------
struct Px { float r, g, b, a; };
Px load(const std::uint8_t* p) { return {p[2] / 255.0F, p[1] / 255.0F, p[0] / 255.0F, p[3] / 255.0F}; }
void store(std::uint8_t* p, Px v) {
    const auto byte = [](float x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0F, 1.0F) * 255.0F)); };
    const auto a = byte(v.a);
    p[3] = a;
    p[2] = std::min(byte(v.r), a); p[1] = std::min(byte(v.g), a); p[0] = std::min(byte(v.b), a);
}
Px lerp(Px d, Px r, float t) { return {d.r + (r.r - d.r) * t, d.g + (r.g - d.g) * t, d.b + (r.b - d.b) * t, d.a + (r.a - d.a) * t}; }

float separable(std::int32_t mode, float cb, float cs) {
    switch (mode) {
    case blend::multiply: return cb * cs;
    case blend::screen: return cb + cs - cb * cs;
    case blend::overlay: return cb <= 0.5F ? cs * 2 * cb : cs + (2 * cb - 1) - cs * (2 * cb - 1);
    case blend::darken: return std::min(cb, cs);
    case blend::lighten: return std::max(cb, cs);
    case blend::color_dodge: return cb == 0 ? 0 : cs >= 1 ? 1 : std::min(1.0F, cb / (1 - cs));
    case blend::color_burn: return cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - std::min(1.0F, (1 - cb) / cs);
    case blend::hard_light: return cs <= 0.5F ? cb * 2 * cs : cb + (2 * cs - 1) - cb * (2 * cs - 1);
    case blend::soft_light: {
        if (cs <= 0.5F) return cb - (1 - 2 * cs) * cb * (1 - cb);
        const auto dcb = cb <= 0.25F ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
        return cb + (2 * cs - 1) * (dcb - cb);
    }
    case blend::difference: return std::abs(cb - cs);
    case blend::exclusion: return cb + cs - 2 * cb * cs;
    default: return cs;
    }
}
struct Rgb { float r, g, b; };
float lum(Rgb c) { return 0.3F * c.r + 0.59F * c.g + 0.11F * c.b; }
Rgb clip_color(Rgb c) {
    const auto l = lum(c);
    const auto n = std::min({c.r, c.g, c.b}), x = std::max({c.r, c.g, c.b});
    if (n < 0) c = {l + (c.r - l) * l / (l - n), l + (c.g - l) * l / (l - n), l + (c.b - l) * l / (l - n)};
    if (x > 1) c = {l + (c.r - l) * (1 - l) / (x - l), l + (c.g - l) * (1 - l) / (x - l), l + (c.b - l) * (1 - l) / (x - l)};
    return c;
}
Rgb set_lum(Rgb c, float l) {
    const auto d = l - lum(c);
    return clip_color({c.r + d, c.g + d, c.b + d});
}
float sat(Rgb c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }
Rgb set_sat(Rgb c, float s) {
    std::array<float*, 3> v{&c.r, &c.g, &c.b};
    std::sort(v.begin(), v.end(), [](float* a, float* b) { return *a < *b; });
    if (*v[2] > *v[0]) {
        *v[1] = (*v[1] - *v[0]) * s / (*v[2] - *v[0]);
        *v[2] = s;
    } else {
        *v[1] = *v[2] = 0;
    }
    *v[0] = 0;
    return c;
}

bool bounded_mode(std::int32_t mode) {
    return mode == blend::clear || mode == blend::copy || mode == blend::source_in || mode == blend::source_out ||
           mode == blend::destination_in || mode == blend::destination_atop;
}

Px blend_pixel(std::int32_t mode, Px s, Px d) {
    switch (mode) {
    case blend::normal: return {s.r + d.r * (1 - s.a), s.g + d.g * (1 - s.a), s.b + d.b * (1 - s.a), s.a + d.a * (1 - s.a)};
    case blend::clear: return {0, 0, 0, 0};
    case blend::copy: return s;
    case blend::source_in: return {s.r * d.a, s.g * d.a, s.b * d.a, s.a * d.a};
    case blend::source_out: return {s.r * (1 - d.a), s.g * (1 - d.a), s.b * (1 - d.a), s.a * (1 - d.a)};
    case blend::source_atop: return {s.r * d.a + d.r * (1 - s.a), s.g * d.a + d.g * (1 - s.a), s.b * d.a + d.b * (1 - s.a), d.a};
    case blend::destination_over: return {d.r + s.r * (1 - d.a), d.g + s.g * (1 - d.a), d.b + s.b * (1 - d.a), d.a + s.a * (1 - d.a)};
    case blend::destination_in: return {d.r * s.a, d.g * s.a, d.b * s.a, d.a * s.a};
    case blend::destination_out: return {d.r * (1 - s.a), d.g * (1 - s.a), d.b * (1 - s.a), d.a * (1 - s.a)};
    case blend::destination_atop: return {d.r * s.a + s.r * (1 - d.a), d.g * s.a + s.g * (1 - d.a), d.b * s.a + s.b * (1 - d.a), s.a};
    case blend::exclusive_or:
        return {s.r * (1 - d.a) + d.r * (1 - s.a), s.g * (1 - d.a) + d.g * (1 - s.a), s.b * (1 - d.a) + d.b * (1 - s.a),
                s.a * (1 - d.a) + d.a * (1 - s.a)};
    case blend::plus_lighter: return {std::min(1.0F, s.r + d.r), std::min(1.0F, s.g + d.g), std::min(1.0F, s.b + d.b), std::min(1.0F, s.a + d.a)};
    case blend::plus_darker: {
        const auto a = std::min(1.0F, s.a + d.a);
        const auto channel = [&](float sc, float dc) { return std::max(0.0F, a - ((d.a - dc) + (s.a - sc))); };
        return {channel(s.r, d.r), channel(s.g, d.g), channel(s.b, d.b), a};
    }
    default: break;
    }
    // Separable and non-separable blend modes (W3C Compositing and Blending).
    const auto un = [](float c, float a) { return a > 0 ? c / a : 0.0F; };
    const Rgb cs{un(s.r, s.a), un(s.g, s.a), un(s.b, s.a)};
    const Rgb cb{un(d.r, d.a), un(d.g, d.a), un(d.b, d.a)};
    Rgb mixed{};
    switch (mode) {
    case blend::hue: mixed = set_lum(set_sat(cs, sat(cb)), lum(cb)); break;
    case blend::saturation: mixed = set_lum(set_sat(cb, sat(cs)), lum(cb)); break;
    case blend::color: mixed = set_lum(cs, lum(cb)); break;
    case blend::luminosity: mixed = set_lum(cb, lum(cs)); break;
    default: mixed = {separable(mode, cb.r, cs.r), separable(mode, cb.g, cs.g), separable(mode, cb.b, cs.b)}; break;
    }
    const auto both = s.a * d.a;
    return {s.r * (1 - d.a) + d.r * (1 - s.a) + both * mixed.r, s.g * (1 - d.a) + d.g * (1 - s.a) + both * mixed.g,
            s.b * (1 - d.a) + d.b * (1 - s.a) + both * mixed.b, s.a + d.a * (1 - s.a)};
}

// Separable Gaussian blur of an alpha plane (sigma in pixels).
void blur(std::vector<float>& plane, std::uint32_t width, std::uint32_t height, double sigma) {
    if (sigma <= 0.05) return;
    const auto radius = static_cast<int>(std::ceil(sigma * 3));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    double total{};
    for (int index = -radius; index <= radius; ++index) {
        const auto value = std::exp(-(index * index) / (2 * sigma * sigma));
        kernel[static_cast<std::size_t>(index + radius)] = static_cast<float>(value);
        total += value;
    }
    for (auto& value : kernel) value = static_cast<float>(value / total);
    std::vector<float> temporary(plane.size());
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            float sum{};
            for (int k = -radius; k <= radius; ++k) {
                const auto sx = static_cast<std::int64_t>(x) + k;
                if (sx < 0 || sx >= width) continue;
                sum += plane[y * width + static_cast<std::uint32_t>(sx)] * kernel[static_cast<std::size_t>(k + radius)];
            }
            temporary[y * width + x] = sum;
        }
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            float sum{};
            for (int k = -radius; k <= radius; ++k) {
                const auto sy = static_cast<std::int64_t>(y) + k;
                if (sy < 0 || sy >= height) continue;
                sum += temporary[static_cast<std::uint32_t>(sy) * width + x] * kernel[static_cast<std::size_t>(k + radius)];
            }
            plane[y * width + x] = sum;
        }
}

} // namespace

// ---- prepared paints ------------------------------------------------------------------------------
namespace {
struct PreparedPaint {
    Rgba color;
    double opacity{1};
    std::shared_ptr<const Pbgra> pattern;   // cell pixels
    Affine pattern_to_device;                // cell pixel space -> device
};

// Renders one pattern cell through the guest callback.
PreparedPaint prepare_paint(Quartz& q, ContextState& c, const Paint& paint) {
    PreparedPaint result;
    result.color = paint.color;
    result.opacity = c.g.alpha;
    if (!paint.pattern) return result;
    const auto info = q.require<PatternInfo>(paint.pattern.get(), "CGPattern");
    const auto x_step = info->x_step != 0 ? std::abs(info->x_step) : std::abs(info->bounds.size.width);
    const auto y_step = info->y_step != 0 ? std::abs(info->y_step) : std::abs(info->bounds.size.height);
    if (x_step <= 0 || y_step <= 0) { result.color.a = 0; return result; }
    // The pattern matrix maps pattern space to the context's default user space.
    const auto pattern_to_base = info->matrix.then(Affine::translation(c.g.pattern_phase.width, c.g.pattern_phase.height));
    const auto pattern_to_device = pattern_to_base.then(c.base_ctm).then(flip(c));
    const auto scale = std::max(0.01, pattern_to_device.scale());
    const auto width = static_cast<std::uint32_t>(std::clamp(std::ceil(x_step * scale), 1.0, 2048.0));
    const auto height = static_cast<std::uint32_t>(std::clamp(std::ceil(y_step * scale), 1.0, 2048.0));
    const auto sx = width / x_step, sy = height / y_step;
    PixelLayout layout;
    layout.width = width;
    layout.height = height;
    layout.bytes_per_row = static_cast<std::uint64_t>(width) * 4;
    layout.bitmap_info = alpha_info::premultiplied_first | bitmap_info::order_32_little;
    const auto cell = create_context(q, layout, 0, q.device_rgb());
    if (!cell) fail("creating a pattern cell context");
    try {
        const auto cell_state = context_state(q, cell);
        cell_state->g.ctm = Affine::translation(-info->bounds.origin.x, -info->bounds.origin.y).then(Affine::scaling(sx, sy));
        PathData bounds;
        bounds.add_rect(cell_state->g.ctm, info->bounds);
        clip_to_path(q, *cell_state, bounds, false);
        static_cast<void>(q.call_guest(info->draw, {info->info, cell}));
        synchronize(q, *cell_state);
        auto pixels = std::make_shared<Pbgra>(cell_state->surface->read(full(*cell_state)));
        if (!info->colored) {
            // Uncolored pattern: the cell's coverage tinted with the paint's color.
            const auto tint = paint.color;
            for (std::size_t index = 0; index < pixels->pixels.size(); index += 4) {
                const auto a = pixels->pixels[index + 3] / 255.0 * tint.a;
                pixels->pixels[index + 0] = static_cast<std::uint8_t>(std::lround(tint.b * a * 255));
                pixels->pixels[index + 1] = static_cast<std::uint8_t>(std::lround(tint.g * a * 255));
                pixels->pixels[index + 2] = static_cast<std::uint8_t>(std::lround(tint.r * a * 255));
                pixels->pixels[index + 3] = static_cast<std::uint8_t>(std::lround(a * 255));
            }
        }
        result.pattern = std::move(pixels);
        const Affine cell_to_pattern{1 / sx, 0, 0, -1 / sy, info->bounds.origin.x, height / sy + info->bounds.origin.y};
        result.pattern_to_device = cell_to_pattern.then(pattern_to_device);
    } catch (...) { q.release(cell); throw; }
    q.release(cell);
    return result;
}

host::gpu::Brush make_brush(const PreparedPaint& paint, bool coverage) {
    host::gpu::Brush brush;   // solid opaque white: coverage
    if (coverage) return brush;
    if (paint.pattern) {
        brush.kind = host::gpu::Brush::Kind::tiles;
        brush.tiles = view(*paint.pattern);
        brush.tiles_transform = to_host(paint.pattern_to_device);
        brush.opacity = paint.opacity;
        return brush;
    }
    brush.color = to_color(paint.color, paint.opacity);
    return brush;
}

host::gpu::StrokeStyle stroke_style(const GState& g, double width) {
    return quartz2d::stroke_style(g.cap, g.join, g.miter_limit, g.dashes, g.dash_phase, width);
}

} // namespace

// ---- the operation runner ---------------------------------------------------------------------------
namespace {
struct Operation {
    Rect device_bounds;      // conservative, device space
    std::function<void(host::gpu::Surface&, bool coverage)> paint;
};

host::gpu::Blend primitive_blend(std::int32_t mode, bool& supported) {
    supported = true;
    switch (mode) {
    case blend::normal: return host::gpu::Blend::source_over;
    case blend::copy: return host::gpu::Blend::copy;
    case blend::plus_lighter: return host::gpu::Blend::add;
    default: supported = false; return host::gpu::Blend::source_over;
    }
}

void run(Quartz& q, ContextState& c, const Operation& op) {
    auto& surface = target(c);
    const auto surface_rect = full(c);
    const auto clip_rect = pixel_bounds(clip_device_bounds(c), c.layout.width, c.layout.height);
    if (clip_rect.empty()) return;
    const auto& g = c.g;
    const auto aa = antialiasing(g);
    auto mode = g.blend_mode;
    bool primitive_ok{};
    const auto primitive = primitive_blend(mode, primitive_ok);
    const bool needs_context = mode != blend::normal;
    const bool simple = primitive_ok && !g.shadow.enabled && !has_mask_clip(g) && (!needs_context || surface.blend_modes());
    if (simple) {
        const auto dirty = intersect(intersect(pixel_bounds(op.device_bounds, c.layout.width, c.layout.height), clip_rect), surface_rect);
        if (dirty.empty()) return;
        pull(q, c, dirty);
        auto& rt = surface.canvas();
        surface.begin();
        try {
            rt.set_antialias(aa);
            rt.set_text_antialias(antialiasing(g));
            if (needs_context) rt.set_blend(primitive);
            const auto pushed = push_clips(rt, g);
            // Limit drawing to the pulled rectangle (estimates are conservative).
            rt.set_transform({});
            rt.push_axis_clip(to_box(dirty), false);
            op.paint(rt, false);
            rt.set_transform({});
            rt.pop_axis_clip();
            pop_clips(rt, pushed);
            if (needs_context) rt.set_blend(host::gpu::Blend::source_over);
        } catch (...) {
            try { surface.end(); } catch (...) {}
            throw;
        }
        surface.end();
        push(q, c, dirty);
        return;
    }

    // ---- composited path ----
    const auto source_rect = intersect(pixel_bounds(op.device_bounds, c.layout.width, c.layout.height), surface_rect);
    if (source_rect.empty()) return;
    auto scratch = scratch_surface(c.scratch, surface);
    scratch->clear(source_rect);
    scratch->begin();
    try {
        scratch->canvas().set_antialias(aa);
        scratch->canvas().set_text_antialias(antialiasing(g));
        op.paint(scratch->canvas(), false);
    } catch (...) { try { scratch->end(); } catch (...) {} throw; }
    scratch->end();
    const auto source = scratch->read(source_rect);

    // Shadow: blurred source alpha, offset (base space, y up -> device y down).
    PixelRect shadow_rect{};
    std::vector<float> shadow_plane;
    std::int32_t shadow_dx{}, shadow_dy{}, margin{};
    PixelRect plane_rect{};
    // Shadow offsets and blur are in default user space.
    const auto base_scale = c.base_ctm.scale();
    if (g.shadow.enabled && g.shadow.color.a > 0) {
        const auto sigma = g.shadow.blur * base_scale / 2;
        margin = static_cast<std::int32_t>(std::ceil(sigma * 3)) + 1;
        shadow_dx = static_cast<std::int32_t>(std::lround(g.shadow.offset.width * base_scale));
        shadow_dy = static_cast<std::int32_t>(std::lround(-g.shadow.offset.height * base_scale));
        plane_rect = {source_rect.x0 - margin, source_rect.y0 - margin, source_rect.x1 + margin, source_rect.y1 + margin};
        const auto pw = plane_rect.width(), ph = plane_rect.height();
        shadow_plane.assign(static_cast<std::size_t>(pw) * ph, 0.0F);
        for (std::uint32_t y = 0; y < source.height; ++y)
            for (std::uint32_t x = 0; x < source.width; ++x)
                shadow_plane[(y + static_cast<std::uint32_t>(margin)) * pw + x + static_cast<std::uint32_t>(margin)] = source.row(y)[x * 4 + 3] / 255.0F;
        blur(shadow_plane, pw, ph, sigma);
        shadow_rect = intersect({plane_rect.x0 + shadow_dx, plane_rect.y0 + shadow_dy, plane_rect.x1 + shadow_dx, plane_rect.y1 + shadow_dy},
                                surface_rect);
    }
    const auto dirty = intersect(unite(source_rect, shadow_rect), clip_rect);
    if (dirty.empty()) return;

    // Clip coverage over the dirty rectangle.
    std::vector<float> coverage(static_cast<std::size_t>(dirty.width()) * dirty.height(), 1.0F);
    if (!g.clips.empty()) {
        const bool geometric = std::any_of(g.clips.begin(), g.clips.end(), [](const auto& clip) { return clip->kind != ClipItem::Kind::mask; });
        if (geometric) {
            auto cover = scratch_surface(c.coverage, surface);
            cover->clear(dirty);
            cover->begin();
            try {
                auto& rt = cover->canvas();
                const auto pushed = push_clips(rt, g);
                rt.set_transform({});
                check(rt.fill_rect(to_box(dirty), host::gpu::Brush{}), "filling the clip coverage");
                pop_clips(rt, pushed);
            } catch (...) { try { cover->end(); } catch (...) {} throw; }
            cover->end();
            const auto alpha = cover->read(dirty);
            for (std::size_t index = 0; index < coverage.size(); ++index) coverage[index] = alpha.pixels[index * 4 + 3] / 255.0F;
        }
        for (const auto& clip : g.clips) {
            if (clip->kind != ClipItem::Kind::mask) continue;
            for (auto y = dirty.y0; y < dirty.y1; ++y)
                for (auto x = dirty.x0; x < dirty.x1; ++x)
                    coverage[static_cast<std::size_t>(y - dirty.y0) * dirty.width() + static_cast<std::size_t>(x - dirty.x0)] *=
                        (*clip->mask)[static_cast<std::size_t>(y) * c.layout.width + static_cast<std::size_t>(x)] / 255.0F;
        }
    }

    // Shape coverage for bounded Porter-Duff modes.
    std::optional<Pbgra> shape;
    if (bounded_mode(mode)) {
        scratch->clear(source_rect);
        scratch->begin();
        try {
            scratch->canvas().set_antialias(aa);
            op.paint(scratch->canvas(), true);
        } catch (...) { try { scratch->end(); } catch (...) {} throw; }
        scratch->end();
        shape = scratch->read(source_rect);
    }

    pull(q, c, dirty);
    {
        const auto locked = surface.lock(dirty, true);
        const auto shadow_color = g.shadow.color;
        for (auto y = dirty.y0; y < dirty.y1; ++y) {
            for (auto x = dirty.x0; x < dirty.x1; ++x) {
                const auto cov = coverage[static_cast<std::size_t>(y - dirty.y0) * dirty.width() + static_cast<std::size_t>(x - dirty.x0)];
                if (cov <= 0) continue;
                auto* pixel = locked.at(x, y, dirty);
                auto d = load(pixel);
                if (!shadow_plane.empty()) {
                    const auto px = x - shadow_dx - plane_rect.x0, py = y - shadow_dy - plane_rect.y0;
                    if (px >= 0 && py >= 0 && px < static_cast<std::int32_t>(plane_rect.width()) && py < static_cast<std::int32_t>(plane_rect.height())) {
                        const auto a = shadow_plane[static_cast<std::size_t>(py) * plane_rect.width() + static_cast<std::size_t>(px)] *
                                       static_cast<float>(shadow_color.a);
                        if (a > 0) {
                            const Px sp{static_cast<float>(shadow_color.r) * a, static_cast<float>(shadow_color.g) * a,
                                        static_cast<float>(shadow_color.b) * a, a};
                            d = lerp(d, blend_pixel(blend::normal, sp, d), cov);
                        }
                    }
                }
                Px s{0, 0, 0, 0};
                float m = 1;
                const bool inside = x >= source_rect.x0 && x < source_rect.x1 && y >= source_rect.y0 && y < source_rect.y1;
                if (inside) {
                    s = load(source.row(static_cast<std::uint32_t>(y - source_rect.y0)) + static_cast<std::size_t>(x - source_rect.x0) * 4);
                    if (shape) m = shape->row(static_cast<std::uint32_t>(y - source_rect.y0))[static_cast<std::size_t>(x - source_rect.x0) * 4 + 3] / 255.0F;
                } else if (shape) {
                    m = 0;
                }
                if (shape) {
                    if (m <= 0) { store(pixel, d); continue; }
                    s = {s.r / m, s.g / m, s.b / m, s.a / m};
                    d = lerp(d, blend_pixel(mode, s, d), cov * m);
                } else if (inside) {
                    d = lerp(d, blend_pixel(mode, s, d), cov);
                }
                store(pixel, d);
            }
        }
    }
    push(q, c, dirty);
}

Rect device_bounds_of(const ContextState& c, const Rect& base_rect) { return transform_rect(flip(c), base_rect); }
Rect expand(const Rect& r, double amount) {
    if (rect_is_null(r)) return r;
    return {{r.origin.x - amount, r.origin.y - amount}, {r.size.width + 2 * amount, r.size.height + 2 * amount}};
}
Rect everything(const ContextState& c) {
    return {{0, 0}, {static_cast<double>(c.layout.width), static_cast<double>(c.layout.height)}};
}
} // namespace

// ---- public drawing entry points ----------------------------------------------------------------------
std::shared_ptr<ContextState> context_state(Quartz& q, Address context) {
    const auto state = q.require<ContextState>(context, "CGContext");
    if (state->destroyed) fail("use of a released CGContext");
    return state;
}

void fill_path(Quartz& q, ContextState& c, const PathData& base_path, bool even_odd) {
    if (base_path.empty()) return;
    const auto paint = prepare_paint(q, c, c.g.fill);
    const auto device_flip = flip(c);
    const auto geometry = make_geometry(base_path, device_flip, even_odd);
    Operation op;
    op.device_bounds = expand(device_bounds_of(c, base_path.control_bounds()), 2);
    op.paint = [&](host::gpu::Surface& rt, bool coverage) {
        rt.set_transform({});
        check(rt.fill(geometry, make_brush(paint, coverage)), "filling a path");
    };
    run(q, c, op);
}

void stroke_path(Quartz& q, ContextState& c, const PathData& base_path) {
    if (base_path.empty()) return;
    const auto paint = prepare_paint(q, c, c.g.stroke);
    const auto& g = c.g;
    const auto inverse = g.ctm.inverse();
    const bool hairline = g.line_width <= 0 || !inverse;
    // Strokes are built in user space so the CTM scales and skews the pen.
    const auto user_to_device = g.ctm.then(flip(c));
    const auto geometry = hairline ? make_geometry(base_path, flip(c), false, false)
                                   : make_geometry(base_path, *inverse, false, false);
    const auto width = hairline ? 1.0 : g.line_width;
    const auto style = stroke_style(g, width);
    Operation op;
    const auto pen = hairline ? 1.0 : g.line_width * std::max({std::hypot(g.ctm.a, g.ctm.b), std::hypot(g.ctm.c, g.ctm.d), 1e-9});
    op.device_bounds = expand(device_bounds_of(c, base_path.control_bounds()), pen * std::max(1.0, g.miter_limit) / 2 + pen + 2);
    op.paint = [&](host::gpu::Surface& rt, bool coverage) {
        rt.set_transform(hairline ? host::gpu::Matrix{} : to_host(user_to_device));
        check(rt.stroke(geometry, make_brush(paint, coverage), width, style), "stroking a path");
    };
    run(q, c, op);
}

void clip_to_path(Quartz& q, ContextState& c, const PathData& base_path, bool even_odd) {
    static_cast<void>(q);
    auto item = std::make_shared<ClipItem>();
    item->antialias = antialiasing(c.g);
    if (base_path.empty()) {
        item->kind = ClipItem::Kind::rect;
        item->device = {{0, 0}, {0, 0}};
    } else if (const auto rect = base_path.as_rect()) {
        item->kind = ClipItem::Kind::rect;
        item->device = device_bounds_of(c, *rect);
    } else {
        item->kind = ClipItem::Kind::geometry;
        const auto geometry = make_geometry(base_path, flip(c), even_odd);
        item->geometry = geometry;
        item->device = device_bounds_of(c, base_path.control_bounds());
    }
    c.g.clips.push_back(std::move(item));
}

void draw_image(Quartz& q, ContextState& c, Address image, const Rect& rect) {
    if (!image || rect_is_empty(rect)) return;
    const auto info = q.require<ImageInfo>(image, "CGImage");
    auto pixels = q.image_pixels(image);
    if (!pixels || pixels->width == 0 || pixels->height == 0) return;
    if (info->desc.mask) {
        // Image masks paint the fill color through the mask.
        auto tinted = std::make_shared<Pbgra>(*pixels);
        const auto color = c.g.fill.color;
        for (std::size_t index = 0; index < tinted->pixels.size(); index += 4) {
            const auto a = tinted->pixels[index + 3] / 255.0 * color.a;
            tinted->pixels[index] = static_cast<std::uint8_t>(std::lround(color.b * a * 255));
            tinted->pixels[index + 1] = static_cast<std::uint8_t>(std::lround(color.g * a * 255));
            tinted->pixels[index + 2] = static_cast<std::uint8_t>(std::lround(color.r * a * 255));
            tinted->pixels[index + 3] = static_cast<std::uint8_t>(std::lround(a * 255));
        }
        pixels = tinted;
    }
    const auto r = standardize(rect);
    // Image pixel space (top row first) -> user space -> device.
    const Affine image_to_user{r.size.width / pixels->width, 0, 0, -r.size.height / pixels->height, r.origin.x, r.origin.y + r.size.height};
    const auto image_to_device = image_to_user.then(c.g.ctm).then(flip(c));
    const auto quality = c.g.interpolation;
    const bool nearest = quality == 1 /* kCGInterpolationNone */ || !info->should_interpolate;
    const auto opacity = static_cast<float>(c.g.alpha);
    Operation op;
    op.device_bounds = expand(transform_rect(image_to_device, {{0, 0}, {static_cast<double>(pixels->width), static_cast<double>(pixels->height)}}), 1);
    op.paint = [&](host::gpu::Surface& rt, bool coverage) {
        rt.set_transform(to_host(image_to_device));
        const host::gpu::Box destination{0, 0, static_cast<double>(pixels->width), static_cast<double>(pixels->height)};
        if (coverage) {
            check(rt.fill_rect(destination, host::gpu::Brush{}), "filling an image's coverage");
            return;
        }
        const auto interpolation = nearest ? host::gpu::Interpolation::nearest
                                 : quality == 3 /* kCGInterpolationHigh */ ? host::gpu::Interpolation::high_quality
                                                                           : host::gpu::Interpolation::linear;
        check(rt.draw_bitmap(view(*pixels), destination, opacity, interpolation), "drawing an image");
    };
    run(q, c, op);
}

void draw_glyphs(Quartz& q, ContextState& c, const GlyphRunDraw& run_draw, Point origin, const std::optional<Rgba>& color) {
    if (run_draw.glyphs.empty() || !run_draw.face) return;
    const auto mode = c.g.text_mode;
    const auto text_to_user = Affine::translation(origin.x, origin.y).then(c.text_matrix);
    const auto text_to_base = text_to_user.then(c.g.ctm);
    const bool fills = mode == text_mode::fill || mode == text_mode::fill_stroke || mode == text_mode::fill_clip ||
                       mode == text_mode::fill_stroke_clip;
    const bool strokes = mode == text_mode::stroke || mode == text_mode::fill_stroke || mode == text_mode::stroke_clip ||
                         mode == text_mode::fill_stroke_clip;
    const bool clips = mode >= text_mode::fill_clip;
    std::optional<PathData> outlines;
    const auto outline_base = [&]() -> const PathData& {
        if (!outlines) outlines = glyph_outlines(run_draw).transformed(text_to_base);
        return *outlines;
    };
    if (fills) {
        if (!run_draw.matrix.is_identity()) {
            // Font matrices are drawn as outlines.
            auto saved_fill = c.g.fill;
            if (color) { c.g.fill.color = *color; c.g.fill.pattern.reset(); }
            try { fill_path(q, c, outline_base(), false); } catch (...) { c.g.fill = saved_fill; throw; }
            c.g.fill = saved_fill;
        } else {
            auto paint_source = c.g.fill;
            if (color) { paint_source.color = *color; paint_source.pattern.reset(); }
            const auto paint = prepare_paint(q, c, paint_source);
            const auto glyph_to_device = Affine::scaling(1, -1).then(text_to_base).then(flip(c));
            std::vector<host::gpu::Point> offsets(run_draw.glyphs.size());
            for (std::size_t index = 0; index < offsets.size(); ++index)
                if (index < run_draw.positions.size()) offsets[index] = to_host(run_draw.positions[index]);
            const auto metrics = font_metrics(*run_draw.face);
            double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
            for (const auto& p : run_draw.positions) { min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x); min_y = std::min(min_y, p.y); max_y = std::max(max_y, p.y); }
            const auto em = run_draw.size;
            const auto box = metrics.bounding_box;
            const auto scale = em / std::max(1.0, metrics.units_per_em);
            const Rect text_bounds{{min_x + box.origin.x * scale - em, min_y + box.origin.y * scale - em},
                                   {max_x - min_x + box.size.width * scale + 2 * em, max_y - min_y + box.size.height * scale + 2 * em}};
            Operation op;
            op.device_bounds = expand(transform_rect(text_to_base.then(flip(c)), text_bounds), 2);
            const auto smooth = c.g.smooth_fonts && c.g.allows_smoothing && antialiasing(c.g);
            op.paint = [&](host::gpu::Surface& rt, bool coverage) {
                rt.set_transform(to_host(glyph_to_device));
                rt.set_text_antialias(smooth || antialiasing(c.g));
                check(rt.draw_glyphs(run_draw.face->font, run_draw.size, run_draw.glyphs, offsets, make_brush(paint, coverage)),
                      "drawing glyphs");
            };
            run(q, c, op);
        }
    }
    if (strokes) {
        // Text strokes use the stroke paint, as CoreGraphics does.
        stroke_path(q, c, outline_base());
    }
    if (clips) clip_to_path(q, c, outline_base(), false);
}

void fill_rect_color(Quartz& q, ContextState& c, const Rect& user_rect, const Rgba& color) {
    PathData path;
    path.add_rect(c.g.ctm, user_rect);
    auto saved = c.g.fill;
    c.g.fill.color = color;
    c.g.fill.pattern.reset();
    try { fill_path(q, c, path, false); } catch (...) { c.g.fill = saved; throw; }
    c.g.fill = saved;
}

// ---- gradients ------------------------------------------------------------------------------------
namespace {
// Device-space rectangle of everything drawing may touch (clip bounds).
Rect clip_area(const ContextState& c) { return clip_device_bounds(c); }
// User-space polygon covering `device` (for extended gradient fills).
double user_extent(const ContextState& c, const Affine& user_to_device) {
    const auto inverse = user_to_device.inverse();
    if (!inverse) return 0;
    const auto area = transform_rect(*inverse, clip_area(c));
    return std::hypot(area.size.width, area.size.height) + std::hypot(area.origin.x, area.origin.y);
}
} // namespace

void draw_gradient(Quartz& q, ContextState& c, const GradientInfo& gradient, bool radial, Point start, double start_radius, Point end,
                   double end_radius, bool extend_start, bool extend_end) {
    if (gradient.stops.empty()) return;
    const auto user_to_device = c.g.ctm.then(flip(c));
    const auto opacity = c.g.alpha;
    if (!radial) {
        const Point direction{end.x - start.x, end.y - start.y};
        const auto length = std::hypot(direction.x, direction.y);
        if (length == 0) return;
        const Point u{direction.x / length, direction.y / length}, n{-u.y, u.x};
        const auto big = user_extent(c, user_to_device) * 2 + length + 1;
        const Point p0{start.x - (extend_start ? u.x * big : 0), start.y - (extend_start ? u.y * big : 0)};
        const Point p1{end.x + (extend_end ? u.x * big : 0), end.y + (extend_end ? u.y * big : 0)};
        PathData band;
        band.move_to({p0.x + n.x * big, p0.y + n.y * big});
        band.line_to({p0.x - n.x * big, p0.y - n.y * big});
        band.line_to({p1.x - n.x * big, p1.y - n.y * big});
        band.line_to({p1.x + n.x * big, p1.y + n.y * big});
        band.close();
        const auto geometry = make_geometry(band, Affine{}, false);
        Operation op;
        op.device_bounds = rect_intersection(transform_rect(user_to_device, band.control_bounds()), everything(c));
        std::vector<host::gpu::GradientStop> stops;
        for (const auto& stop : gradient.stops) stops.push_back({std::clamp(stop.location, 0.0, 1.0), to_color(stop.color)});
        op.paint = [&](host::gpu::Surface& rt, bool coverage) {
            rt.set_transform(to_host(user_to_device));
            host::gpu::Brush brush;
            if (!coverage) {
                brush.kind = host::gpu::Brush::Kind::linear_gradient;
                brush.stops = stops;
                brush.start = to_host(start);
                brush.end = to_host(end);
                brush.opacity = opacity;
            }
            check(rt.fill(geometry, brush), "filling a linear gradient");
        };
        run(q, c, op);
        return;
    }
    // Two-circle radial gradient, rendered per device pixel.
    const auto inverse = user_to_device.inverse();
    if (!inverse) return;
    auto area = clip_area(c);
    if (!extend_start || !extend_end) {
        const auto r = std::max(std::abs(start_radius), std::abs(end_radius));
        const Rect circles = rect_union({{start.x - std::abs(start_radius), start.y - std::abs(start_radius)}, {2 * std::abs(start_radius), 2 * std::abs(start_radius)}},
                                        {{end.x - std::abs(end_radius), end.y - std::abs(end_radius)}, {2 * std::abs(end_radius), 2 * std::abs(end_radius)}});
        static_cast<void>(r);
        if (!extend_end) area = rect_intersection(area, expand(transform_rect(user_to_device, circles), 1));
    }
    const auto region = pixel_bounds(area, c.layout.width, c.layout.height);
    if (region.empty()) return;
    auto pixels = std::make_shared<Pbgra>();
    pixels->width = region.width();
    pixels->height = region.height();
    pixels->pixels.assign(static_cast<std::size_t>(pixels->width) * pixels->height * 4, 0);
    const Point cd{end.x - start.x, end.y - start.y};
    const auto dr = end_radius - start_radius;
    const auto a = cd.x * cd.x + cd.y * cd.y - dr * dr;
    for (std::uint32_t y = 0; y < pixels->height; ++y) {
        for (std::uint32_t x = 0; x < pixels->width; ++x) {
            const auto p = inverse->apply({region.x0 + x + 0.5, region.y0 + y + 0.5});
            const Point pd{p.x - start.x, p.y - start.y};
            const auto b = pd.x * cd.x + pd.y * cd.y + start_radius * dr;
            const auto cc = pd.x * pd.x + pd.y * pd.y - start_radius * start_radius;
            std::optional<double> t;
            const auto accept = [&](double candidate) {
                if (start_radius + candidate * dr < 0) return false;
                if (candidate < 0 && !extend_start) return false;
                if (candidate > 1 && !extend_end) return false;
                t = candidate;
                return true;
            };
            if (std::abs(a) < 1e-12) {
                if (std::abs(b) > 1e-12) static_cast<void>(accept(cc / (2 * b)));
            } else {
                const auto discriminant = b * b - a * cc;
                if (discriminant >= 0) {
                    const auto root = std::sqrt(discriminant);
                    const auto t1 = (b + root) / a, t2 = (b - root) / a;
                    if (!accept(std::max(t1, t2))) static_cast<void>(accept(std::min(t1, t2)));
                }
            }
            if (!t) continue;
            const auto color = gradient.at(std::clamp(*t, 0.0, 1.0));
            const auto alpha = color.a * opacity;
            auto* out = pixels->row(y) + x * 4;
            out[0] = static_cast<std::uint8_t>(std::lround(std::clamp(color.b, 0.0, 1.0) * alpha * 255));
            out[1] = static_cast<std::uint8_t>(std::lround(std::clamp(color.g, 0.0, 1.0) * alpha * 255));
            out[2] = static_cast<std::uint8_t>(std::lround(std::clamp(color.r, 0.0, 1.0) * alpha * 255));
            out[3] = static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255));
        }
    }
    Operation op;
    op.device_bounds = {{static_cast<double>(region.x0), static_cast<double>(region.y0)},
                        {static_cast<double>(region.width()), static_cast<double>(region.height())}};
    op.paint = [&](host::gpu::Surface& rt, bool) {
        rt.set_transform({});
        check(rt.draw_bitmap(view(*pixels), to_box(region), 1.0, host::gpu::Interpolation::nearest), "drawing a radial gradient");
    };
    run(q, c, op);
}

void clip_to_mask(Quartz& q, ContextState& c, const Rect& rect, Address mask) {
    const auto info = q.require<ImageInfo>(mask, "CGImage");
    const auto pixels = q.image_pixels(mask);
    // Image masks clip where they would paint; grayscale images clip with
    // their samples as alpha (white shows); images with alpha use it.
    const bool gray = !info->desc.mask && info->desc.layout.model == ColorModel::monochrome && !info->desc.layout.has_alpha();
    Pbgra alpha = *pixels;
    for (std::size_t index = 0; index < alpha.pixels.size(); index += 4) {
        const auto a = gray ? alpha.pixels[index + 2] : alpha.pixels[index + 3];
        alpha.pixels[index] = alpha.pixels[index + 1] = alpha.pixels[index + 2] = alpha.pixels[index + 3] = a;
    }
    const auto r = standardize(rect);
    const Affine image_to_user{r.size.width / alpha.width, 0, 0, -r.size.height / alpha.height, r.origin.x, r.origin.y + r.size.height};
    const auto image_to_device = image_to_user.then(c.g.ctm).then(flip(c));
    auto surface = std::make_shared<Surface>(c.layout.width, c.layout.height);
    surface->begin();
    try {
        auto& rt = surface->canvas();
        rt.set_transform(to_host(image_to_device));
        check(rt.draw_bitmap(view(alpha), {0, 0, static_cast<double>(alpha.width), static_cast<double>(alpha.height)}, 1.0,
                             host::gpu::Interpolation::linear),
              "drawing a clip mask");
    } catch (...) { try { surface->end(); } catch (...) {} throw; }
    surface->end();
    const auto rendered = surface->read(full(c));
    auto buffer = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(c.layout.width) * c.layout.height);
    for (std::size_t index = 0; index < buffer->size(); ++index) (*buffer)[index] = rendered.pixels[index * 4 + 3];
    auto item = std::make_shared<ClipItem>();
    item->kind = ClipItem::Kind::mask;
    item->mask = std::move(buffer);
    item->device = transform_rect(image_to_device, {{0, 0}, {static_cast<double>(alpha.width), static_cast<double>(alpha.height)}});
    c.g.clips.push_back(std::move(item));
}

void clear_rect(Quartz& q, ContextState& c, const Rect& rect) {
    PathData path;
    path.add_rect(c.g.ctm, rect);
    const auto saved = c.g;
    c.g.blend_mode = blend::clear;
    c.g.shadow.enabled = false;
    c.g.alpha = 1;
    c.g.fill.pattern.reset();
    c.g.fill.color = {0, 0, 0, 1};
    if (target(c).blend_modes()) {
        c.g.blend_mode = blend::copy;
        c.g.fill.color = {0, 0, 0, 0};
    }
    try { fill_path(q, c, path, false); } catch (...) { c.g = saved; throw; }
    c.g = saved;
}

void draw_tiled_image(Quartz& q, ContextState& c, Address image, const Rect& rect) {
    if (!image || rect_is_empty(rect)) return;
    const auto pixels = q.image_pixels(image);
    if (!pixels || pixels->width == 0 || pixels->height == 0) return;
    const auto r = standardize(rect);
    const Affine image_to_user{r.size.width / pixels->width, 0, 0, -r.size.height / pixels->height, r.origin.x, r.origin.y + r.size.height};
    const auto image_to_device = image_to_user.then(c.g.ctm).then(flip(c));
    const auto area = clip_area(c);
    const auto opacity = static_cast<float>(c.g.alpha);
    Operation op;
    op.device_bounds = area;
    op.paint = [&](host::gpu::Surface& rt, bool coverage) {
        rt.set_transform({});
        host::gpu::Brush brush;
        if (!coverage) {
            brush.kind = host::gpu::Brush::Kind::tiles;
            brush.tiles = view(*pixels);
            brush.tiles_transform = to_host(image_to_device);
            brush.opacity = opacity;
        }
        check(rt.fill_rect(to_box(area), brush), "tiling an image");
    };
    run(q, c, op);
}

// ---- transparency layers -------------------------------------------------------------------------------
namespace {
void composite_layer(Quartz& q, ContextState& c, ContextState::Layer layer) {
    c.g = layer.state;
    const auto pixels = layer.surface->read(full(c));
    Operation op;
    op.device_bounds = everything(c);
    op.paint = [&](host::gpu::Surface& rt, bool coverage) {
        rt.set_transform({});
        const host::gpu::Box whole{0, 0, static_cast<double>(pixels.width), static_cast<double>(pixels.height)};
        if (coverage) {
            check(rt.fill_rect(whole, host::gpu::Brush{}), "filling a layer's coverage");
            return;
        }
        check(rt.draw_bitmap(view(pixels), whole, static_cast<float>(c.g.alpha), host::gpu::Interpolation::nearest), "compositing a layer");
    };
    run(q, c, op);
}
} // namespace

void begin_transparency_layer(Quartz& q, ContextState& c, const std::optional<Rect>& rect) {
    static_cast<void>(q);
    static_cast<void>(rect);   // the whole surface is the layer; drawing stays clipped
    ContextState::Layer layer;
    layer.surface = std::make_shared<Surface>(c.layout.width, c.layout.height);
    layer.state = c.g;
    layer.depth = c.saved.size();
    c.layers.push_back(std::move(layer));
    c.g.alpha = 1;
    c.g.blend_mode = blend::normal;
    c.g.shadow.enabled = false;
}

void end_transparency_layer(Quartz& q, ContextState& c) {
    if (c.layers.empty()) return;   // unbalanced: ignored like CoreGraphics
    auto layer = std::move(c.layers.back());
    c.layers.pop_back();
    composite_layer(q, c, std::move(layer));
}

void save_state(ContextState& c) { c.saved.push_back(c.g); }

void restore_state(Quartz& q, ContextState& c) {
    if (c.saved.empty()) return;   // CoreGraphics logs an unbalanced restore and ignores it
    // A transparency layer begun after the matching save ends here too.
    while (!c.layers.empty() && c.layers.back().depth >= c.saved.size()) {
        auto layer = std::move(c.layers.back());
        c.layers.pop_back();
        composite_layer(q, c, std::move(layer));
    }
    c.g = std::move(c.saved.back());
    c.saved.pop_back();
}

void synchronize(Quartz& q, ContextState& c) { static_cast<void>(q); static_cast<void>(c); }

Image snapshot(Quartz& q, ContextState& c) {
    static_cast<void>(q);
    Image image;
    image.width = c.layout.width;
    image.height = c.layout.height;
    const auto pixels = c.surface->read(full(c));
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    // The surface mirrors guest memory after every operation; read the guest
    // copy so pixels the guest wrote directly are included.
    PixelLayout straight;
    straight.width = c.layout.width;
    straight.height = 1;
    straight.bytes_per_row = static_cast<std::uint64_t>(c.layout.width) * 4;
    straight.bitmap_info = alpha_info::last;
    straight.straight_alpha = true;
    if (c.data && c.layers.empty()) {
        const auto bpp = pixel_bytes(c.layout);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(c.layout.width) * bpp), converted(static_cast<std::size_t>(c.layout.width) * 4);
        for (std::uint32_t y = 0; y < c.layout.height; ++y) {
            q.memory().read_bytes(c.data + static_cast<Address>(y) * c.layout.bytes_per_row, row);
            row_to_pbgra(c.layout, row.data(), c.layout.width, converted.data());
            pbgra_to_row(straight, converted.data(), c.layout.width, image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4);
        }
        return image;
    }
    for (std::uint32_t y = 0; y < c.layout.height; ++y)
        pbgra_to_row(straight, pixels.row(y), c.layout.width, image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4);
    return image;
}

Address create_context(Quartz& q, const PixelLayout& layout, Address data, Address space, Address release_callback, Address release_info) {
    if (const auto problem = check_context_layout(layout)) {
        diagnostic("CGBitmapContextCreate: unsupported parameter combination: " + *problem);
        return 0;
    }
    auto state = std::make_shared<ContextState>();
    state->layout = layout;
    state->surface = std::make_shared<Surface>(layout.width, layout.height);
    state->space = ObjectRef::retain(space);
    if (data) {
        state->data = data;
        state->release_callback = release_callback;
        state->release_info = release_info;
    } else {
        const auto block = q.allocate(static_cast<std::size_t>(layout.byte_size()));
        state->data = block.address;
        state->data_heap = block.heap;
        state->owns_data = true;
        q.memory().fill_bytes(state->data, 0, layout.byte_size());
    }
    // Initial surface content: the guest's pixels.
    pull(q, *state, full(*state));
    state->g.fill.space = ObjectRef::retain(q.device_rgb());
    state->g.stroke.space = state->g.fill.space;
    return q.create(q.context_class, state, [&q, state](Address) {
        std::lock_guard lock(state->mutex);
        state->destroyed = true;
        state->g = {};
        state->saved.clear();
        state->layers.clear();
        state->space.reset();
        if (state->owns_data) q.free({std::exchange(state->data, 0), state->data_heap});
        else if (state->release_callback) static_cast<void>(q.call_guest(state->release_callback, {state->release_info, state->data}));
        state->surface.reset();
        state->scratch.reset();
        state->coverage.reset();
    });
}

} // namespace alchemy::gui::quartz2d
