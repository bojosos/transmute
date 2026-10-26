#include "alchemy/darwin/quartzcore/layer_tree.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

// CALayer tree composition (layer_tree.hpp).
namespace alchemy::gui::quartzcore::tree {
namespace {

constexpr double epsilon = 1e-9;

struct Box { double x0, y0, x1, y1; };

// The target box of a rectangle of layer space (axis-aligned transforms only).
Box map_box(const Affine& m, const Rect& r) {
    const auto p = m.apply(r.origin);
    const auto q = m.apply({r.origin.x + r.size.width, r.origin.y + r.size.height});
    return {std::min(p.x, q.x), std::min(p.y, q.y), std::max(p.x, q.x), std::max(p.y, q.y)};
}

PixelRect pixels_of(const Box& box) {
    return {std::lround(box.x0), std::lround(box.y0), std::lround(box.x1), std::lround(box.y1)};
}

std::uint8_t channel(double value) { return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0), 0L, 255L)); }

// Premultiplied BGRA source-over (`alpha` scales the source).
void blend(std::uint8_t* dst, const std::uint8_t* src, unsigned alpha) {
    if (alpha == 255) {
        const unsigned inverse = 255 - src[3];
        if (inverse == 0) { std::memcpy(dst, src, 4); return; }
        for (int i = 0; i < 4; ++i) dst[i] = static_cast<std::uint8_t>(src[i] + (dst[i] * inverse + 127) / 255);
        return;
    }
    const unsigned sa = (src[3] * alpha + 127) / 255;
    const unsigned inverse = 255 - sa;
    for (int i = 0; i < 4; ++i) {
        const unsigned s = (src[i] * alpha + 127) / 255;
        dst[i] = static_cast<std::uint8_t>(std::min(255U, s + (dst[i] * inverse + 127) / 255));
    }
}

// A rounded rectangle in target pixels: per target corner (top-left,
// top-right, bottom-left, bottom-right) the x and y radii.
struct Rounding {
    Box box{};
    std::array<double, 4> rx{}, ry{};
    [[nodiscard]] bool any() const {
        for (unsigned i = 0; i < 4; ++i) if (rx[i] > 0 && ry[i] > 0) return true;
        return false;
    }
    // The part of pixel (x, y) the shape covers, 0..1.
    [[nodiscard]] double coverage(long x, long y) const {
        const double cover_x = std::clamp(std::min(static_cast<double>(x) + 1, box.x1) - std::max(static_cast<double>(x), box.x0), 0.0, 1.0);
        const double cover_y = std::clamp(std::min(static_cast<double>(y) + 1, box.y1) - std::max(static_cast<double>(y), box.y0), 0.0, 1.0);
        double cover = cover_x * cover_y;
        if (cover <= 0) return 0;
        const double px = static_cast<double>(x) + 0.5, py = static_cast<double>(y) + 0.5;
        const bool left = px < (box.x0 + box.x1) / 2, top = py < (box.y0 + box.y1) / 2;
        const unsigned corner = (top ? 0 : 2) + (left ? 0 : 1);
        const double a = rx[corner], b = ry[corner];
        if (a <= 0 || b <= 0) return cover;
        const double cx = left ? box.x0 + a : box.x1 - a, cy = top ? box.y0 + b : box.y1 - b;
        if ((left ? px >= cx : px <= cx) || (top ? py >= cy : py <= cy)) return cover;
        const double dx = (px - cx) / a, dy = (py - cy) / b;
        const double outside = (std::sqrt(dx * dx + dy * dy) - 1.0) * std::min(a, b);   // about the pixel distance
        return std::min(cover, std::clamp(0.5 - outside, 0.0, 1.0));
    }
    [[nodiscard]] Rounding inset(double dx, double dy) const {
        Rounding out = *this;
        out.box = {box.x0 + dx, box.y0 + dy, box.x1 - dx, box.y1 - dy};
        for (unsigned i = 0; i < 4; ++i) { out.rx[i] = std::max(0.0, rx[i] - dx); out.ry[i] = std::max(0.0, ry[i] - dy); }
        return out;
    }
};

Rounding rounding_of(const Layer& value, const Affine& m, const Box& box) {
    Rounding out{box, {}, {}};
    if (!(value.corner_radius > 0)) return out;
    const double half_w = (box.x1 - box.x0) / 2, half_h = (box.y1 - box.y0) / 2;
    const double rx = std::min(value.corner_radius * std::abs(m.a), half_w), ry = std::min(value.corner_radius * std::abs(m.d), half_h);
    // The layer's corners (kCALayerMinXMinYCorner ...) in target order: the
    // left edge is min x unless x is mirrored, the top edge min y where the
    // layer's space is flipped (m.d > 0).
    const bool left_is_min_x = m.a > 0, top_is_min_y = m.d > 0;
    for (unsigned corner = 0; corner < 4; ++corner) {
        const bool top = corner < 2, left = corner % 2 == 0;
        const bool min_x = left == left_is_min_x, min_y = top == top_is_min_y;
        const std::uint32_t bit = (min_x ? 1U : 2U) << (min_y ? 0 : 2);
        if (value.masked_corners & bit) { out.rx[corner] = rx; out.ry[corner] = ry; }
    }
    return out;
}

class Composer {
public:
    Composer(Target& target, Stats& stats) : target_(target), stats_(stats) {}

    void layer(const Layer& value, const Affine& m, PixelRect clip, float parent_alpha, bool self) {
        if (value.hidden) return;
        const float alpha = parent_alpha * std::clamp(value.opacity, 0.0F, 1.0F);
        if (alpha <= 0) return;
        if (std::abs(m.b) > epsilon || std::abs(m.c) > epsilon) { ++stats_.unsupported_transforms; return; }
        const auto box = map_box(m, value.bounds);
        const auto bounds_pixels = pixels_of(box);
        const auto round = rounding_of(value, m, box);
        const auto own_clip = value.masks_to_bounds ? clip.intersect(bounds_pixels) : clip;
        const bool has_contents = self && value.contents && value.contents->bgra && value.contents->width && value.contents->height;
        if (self) {
            ++stats_.layers_drawn;
            if (value.shadow) shadow(value, m, box, round, clip, alpha);
            if (value.background && (*value.background)[3] > 0) {
                if (round.any()) fill_shape(bounds_pixels.intersect(clip), *value.background, alpha, round);
                else fill(bounds_pixels.intersect(clip), *value.background, alpha);
            }
        }
        // Rounded clipping: what the contents and sublayers draw outside the
        // rounded bounds is taken back afterwards.
        std::vector<std::uint8_t> saved;
        if (value.masks_to_bounds && round.any() && !own_clip.empty() && (has_contents || !value.sublayers.empty())) save(own_clip, saved);
        if (has_contents) image(value, m, box, own_clip, alpha);
        if (!value.sublayers.empty() && !own_clip.empty()) sublayers(value, m, own_clip, alpha);
        if (!saved.empty()) restore_outside(own_clip, saved, round);
        if (self && value.border && value.border_width > 0 && (*value.border)[3] > 0) border(value, m, round, clip, alpha);
    }

    void sublayers(const Layer& value, const Affine& m, PixelRect own_clip, float alpha) {
        // The sublayers' space -> this layer's bounds space: the flip, then
        // sublayerTransform about the anchor point.
        const auto& b = value.bounds;
        const Point anchor{b.origin.x + value.anchor.x * b.size.width, b.origin.y + value.anchor.y * b.size.height};
        auto inner = m;
        if (value.geometry_flipped) inner = inner.after({1, 0, 0, -1, 0, 2 * b.origin.y + b.size.height});
        if (!value.sublayer_transform.identity())
            inner = inner.after(Affine::translation(anchor.x, anchor.y)
                                    .after(value.sublayer_transform)
                                    .after(Affine::translation(-anchor.x, -anchor.y)));
        std::vector<const Layer*> order;
        order.reserve(value.sublayers.size());
        for (const auto& child : value.sublayers) order.push_back(&child);
        std::stable_sort(order.begin(), order.end(), [](const Layer* l, const Layer* r) { return l->z_position < r->z_position; });
        for (const auto* child : order) layer(*child, inner.after(local_to_parent(*child)), own_clip, alpha, true);
    }

    void save(PixelRect area, std::vector<std::uint8_t>& saved) {
        const auto width = static_cast<std::size_t>(area.x1 - area.x0) * 4;
        saved.resize(width * static_cast<std::size_t>(area.y1 - area.y0));
        for (long y = area.y0; y < area.y1; ++y)
            std::memcpy(saved.data() + static_cast<std::size_t>(y - area.y0) * width, at(area.x0, y), width);
    }
    // Pixels partly or wholly outside `shape` go back towards what was there
    // before, by the part outside.
    void restore_outside(PixelRect area, const std::vector<std::uint8_t>& saved, const Rounding& shape) {
        const auto width = static_cast<std::size_t>(area.x1 - area.x0) * 4;
        for (long y = area.y0; y < area.y1; ++y) {
            const auto* before = saved.data() + static_cast<std::size_t>(y - area.y0) * width;
            auto* row = at(area.x0, y);
            for (long x = area.x0; x < area.x1; ++x, row += 4, before += 4) {
                const double c = shape.coverage(x, y);
                if (c >= 1) continue;
                for (int i = 0; i < 4; ++i)
                    row[i] = static_cast<std::uint8_t>(std::lround(before[i] * (1 - c) + row[i] * c));
            }
        }
    }
    // A colour over the pixels of `shape` by coverage.
    void fill_shape(PixelRect area, const std::array<float, 4>& rgba, float alpha, const Rounding& shape, const Rounding* hole = nullptr) {
        if (area.empty()) return;
        for (long y = area.y0; y < area.y1; ++y) {
            auto* row = at(area.x0, y);
            for (long x = area.x0; x < area.x1; ++x, row += 4) {
                double c = shape.coverage(x, y);
                if (hole) c = std::max(0.0, c - hole->coverage(x, y));
                if (c <= 0) continue;
                const double a = std::clamp(static_cast<double>(rgba[3]) * alpha * c, 0.0, 1.0);
                const std::uint8_t pixel[4] = {channel(rgba[2] * a), channel(rgba[1] * a), channel(rgba[0] * a), channel(a)};
                blend(row, pixel, 255);
            }
        }
    }
    // The border lies inside the rounded bounds.
    void border(const Layer& value, const Affine& m, const Rounding& round, PixelRect clip, float alpha) {
        const double wx = value.border_width * std::abs(m.a), wy = value.border_width * std::abs(m.d);
        const auto inner = round.inset(wx, wy);
        fill_shape(pixels_of(round.box).intersect(clip), *value.border, alpha, round, &inner);
    }

    // The shadow: the shape's coverage offset and blurred (three box blurs
    // approximating a Gaussian of half the radius), in the shadow colour.
    void shadow(const Layer& value, const Affine& m, const Box& box, const Rounding& round, PixelRect clip, float alpha) {
        const auto& s = *value.shadow;
        const double strength = std::clamp(static_cast<double>(s.color[3]) * s.opacity * alpha, 0.0, 1.0);
        if (strength <= 0) return;
        const bool from_path = s.path && s.path->pixels && s.path->width && s.path->height;
        const bool from_background = !from_path && value.background && (*value.background)[3] > 0;
        const bool from_contents = !from_path && !from_background && value.contents && value.contents->bgra;
        if (!from_path && !from_background && !from_contents) return;
        const double dx = m.a * s.offset.width, dy = m.d * s.offset.height;
        const double scale = std::max(std::abs(m.a), std::abs(m.d));
        const long reach = std::max(0L, std::lround(s.radius * scale / 2));   // box half-width per pass
        Box shape_box = from_path ? map_box(m, s.path->rect)
                        : from_contents ? (value.contents_frame ? map_box(m, *value.contents_frame) : place(value, m, box)) : box;
        const PixelRect area = PixelRect{static_cast<long>(std::floor(shape_box.x0 + dx)) - 3 * reach - 1,
                                         static_cast<long>(std::floor(shape_box.y0 + dy)) - 3 * reach - 1,
                                         static_cast<long>(std::ceil(shape_box.x1 + dx)) + 3 * reach + 1,
                                         static_cast<long>(std::ceil(shape_box.y1 + dy)) + 3 * reach + 1}.intersect(clip);
        if (area.empty()) return;
        // The blur reads beyond the visible area: work on the area grown by the blur's reach.
        const PixelRect work{area.x0 - 3 * reach, area.y0 - 3 * reach, area.x1 + 3 * reach, area.y1 + 3 * reach};
        const long w = work.x1 - work.x0, h = work.y1 - work.y0;
        std::vector<float> mask(static_cast<std::size_t>(w * h));
        const auto shape_at = [&](long x, long y) -> double {   // coverage of the unshifted shape at target pixel (x, y)
            if (from_background) return round.coverage(x, y);
            const double px = static_cast<double>(x) + 0.5, py = static_cast<double>(y) + 0.5;
            if (from_path) {
                const auto& mk = *s.path;
                const double lx = (px - m.tx) / m.a, ly = (py - m.ty) / m.d;   // layer space
                const double u = (lx - mk.rect.origin.x) / mk.rect.size.width;
                const double v = (mk.rect.origin.y + mk.rect.size.height - ly) / mk.rect.size.height;   // first row at max y
                if (u < 0 || v < 0 || u >= 1 || v >= 1) return 0;
                const auto col = static_cast<std::size_t>(u * mk.width), line = static_cast<std::size_t>(v * mk.height);
                return (*mk.pixels)[line * mk.width + col] / 255.0;
            }
            const auto& image = *value.contents;
            const double u = (px - shape_box.x0) / (shape_box.x1 - shape_box.x0), v = (py - shape_box.y0) / (shape_box.y1 - shape_box.y0);
            if (u < 0 || v < 0 || u >= 1 || v >= 1) return 0;
            const auto col = std::min<std::size_t>(static_cast<std::size_t>(u * image.width), image.width - 1);
            const auto line = std::min<std::size_t>(static_cast<std::size_t>(v * image.height), image.height - 1);
            return image.bgra[line * image.stride + col * 4 + 3] / 255.0;
        };
        const long sx = std::lround(dx), sy = std::lround(dy);
        for (long y = 0; y < h; ++y)
            for (long x = 0; x < w; ++x) mask[static_cast<std::size_t>(y * w + x)] = static_cast<float>(shape_at(work.x0 + x - sx, work.y0 + y - sy));
        if (reach > 0) blur(mask, w, h, reach);
        for (long y = area.y0; y < area.y1; ++y) {
            auto* row = at(area.x0, y);
            for (long x = area.x0; x < area.x1; ++x, row += 4) {
                const double a = strength * mask[static_cast<std::size_t>((y - work.y0) * w + (x - work.x0))];
                if (a <= 0) continue;
                const std::uint8_t pixel[4] = {channel(s.color[2] * a), channel(s.color[1] * a), channel(s.color[0] * a), channel(a)};
                blend(row, pixel, 255);
            }
        }
    }
    static void blur(std::vector<float>& values, long w, long h, long reach) {
        std::vector<float> line(static_cast<std::size_t>(std::max(w, h)));
        const auto pass = [&](long count, long length, auto index) {
            for (long n = 0; n < count; ++n) {
                for (int repeat = 0; repeat < 3; ++repeat) {
                    double sum = 0;
                    for (long i = -reach; i <= reach; ++i) sum += i >= 0 && i < length ? values[index(n, i)] : 0;
                    for (long i = 0; i < length; ++i) {
                        line[static_cast<std::size_t>(i)] = static_cast<float>(sum / static_cast<double>(2 * reach + 1));
                        const long out = i - reach, in = i + reach + 1;
                        if (out >= 0) sum -= values[index(n, out)];
                        if (in < length) sum += values[index(n, in)];
                    }
                    for (long i = 0; i < length; ++i) values[index(n, i)] = line[static_cast<std::size_t>(i)];
                }
            }
        };
        pass(h, w, [w](long row, long i) { return static_cast<std::size_t>(row * w + i); });
        pass(w, h, [w](long column, long i) { return static_cast<std::size_t>(i * w + column); });
    }

    static Affine local_to_parent(const Layer& value) {
        const auto& b = value.bounds;
        const Point anchor{b.origin.x + value.anchor.x * b.size.width, b.origin.y + value.anchor.y * b.size.height};
        return Affine::translation(value.position.x, value.position.y)
            .after(value.transform)
            .after(Affine::translation(-anchor.x, -anchor.y));
    }

private:
    Target& target_;
    Stats& stats_;

    std::uint8_t* at(long x, long y) { return target_.bgra + static_cast<std::size_t>(y) * target_.stride + static_cast<std::size_t>(x) * 4; }

    void fill(PixelRect area, const std::array<float, 4>& rgba, float alpha) {
        if (area.empty()) return;
        const double a = std::clamp(static_cast<double>(rgba[3]) * alpha, 0.0, 1.0);
        const std::uint8_t pixel[4] = {channel(rgba[2] * a), channel(rgba[1] * a), channel(rgba[0] * a), channel(a)};
        for (long y = area.y0; y < area.y1; ++y) {
            auto* row = at(area.x0, y);
            for (long x = area.x0; x < area.x1; ++x, row += 4) blend(row, pixel, 255);
        }
    }

    // The part of the image contentsRect selects, in image pixels (rows from
    // the top). Its y axis is the layer's: where the layer's space is flipped
    // (m.d > 0) y = 0 is the image's top, otherwise its bottom.
    struct Source {
        double x{}, y{}, width{}, height{};
        bool whole{};   // {0,0,1,1}
    };
    static Source source_of(const Layer& value, const Affine& m) {
        const auto& image = *value.contents;
        auto r = value.contents_rect;
        if (r.size.width < 0) { r.origin.x += r.size.width; r.size.width = -r.size.width; }
        if (r.size.height < 0) { r.origin.y += r.size.height; r.size.height = -r.size.height; }
        const bool whole = r.origin.x == 0 && r.origin.y == 0 && r.size.width == 1 && r.size.height == 1;
        const double top = m.d > 0 ? r.origin.y : 1.0 - r.origin.y - r.size.height;
        return {r.origin.x * image.width, top * image.height, r.size.width * image.width, r.size.height * image.height, whole};
    }

    // The contents' target box for the layer's gravity.
    static Box place(const Layer& value, const Affine& m, const Box& box) {
        const auto source = source_of(value, m);
        const double scale = value.contents_scale > 0 ? value.contents_scale : 1.0;
        const double width = source.width / scale * std::abs(m.a), height = source.height / scale * std::abs(m.d);
        const double bw = box.x1 - box.x0, bh = box.y1 - box.y0;
        // The vertical gravities name the layer's axis: "top" is its max-y
        // edge, the visual top unless the layer's space is flipped (m.d > 0).
        const bool flipped = m.d > 0;
        const auto at_top = [&](double h) { return flipped ? box.y1 - h : box.y0; };
        const auto at_bottom = [&](double h) { return flipped ? box.y0 : box.y1 - h; };
        const auto centered = [&](double w, double h) { return Box{box.x0 + (bw - w) / 2, box.y0 + (bh - h) / 2, 0, 0}; };
        Box out{};
        double w = width, h = height;
        switch (value.gravity) {
        case Gravity::resize: return box;
        case Gravity::resize_aspect:
        case Gravity::resize_aspect_fill: {
            const double kx = width > 0 ? bw / width : 0, ky = height > 0 ? bh / height : 0;
            const double k = value.gravity == Gravity::resize_aspect ? std::min(kx, ky) : std::max(kx, ky);
            w = width * k; h = height * k;
            out = centered(w, h);
            break;
        }
        case Gravity::center: out = centered(w, h); break;
        case Gravity::top: out = {box.x0 + (bw - w) / 2, at_top(h), 0, 0}; break;
        case Gravity::bottom: out = {box.x0 + (bw - w) / 2, at_bottom(h), 0, 0}; break;
        case Gravity::left: out = {box.x0, box.y0 + (bh - h) / 2, 0, 0}; break;
        case Gravity::right: out = {box.x1 - w, box.y0 + (bh - h) / 2, 0, 0}; break;
        case Gravity::top_left: out = {box.x0, at_top(h), 0, 0}; break;
        case Gravity::top_right: out = {box.x1 - w, at_top(h), 0, 0}; break;
        case Gravity::bottom_left: out = {box.x0, at_bottom(h), 0, 0}; break;
        case Gravity::bottom_right: out = {box.x1 - w, at_bottom(h), 0, 0}; break;
        }
        out.x1 = out.x0 + w;
        out.y1 = out.y0 + h;
        return out;
    }

    void image(const Layer& value, const Affine& m, const Box& box, PixelRect clip, float alpha) {
        const auto& image = *value.contents;
        const auto source = source_of(value, m);
        if (!(source.width > 0) || !(source.height > 0) || !std::isfinite(source.x) || !std::isfinite(source.y) ||
            !std::isfinite(source.width) || !std::isfinite(source.height))
            return;   // an empty (or invalid) contentsRect draws nothing
        const auto destination = value.contents_frame ? map_box(m, *value.contents_frame) : place(value, m, box);
        if (!source.whole) {
            sub_image(image, source, destination, clip, alpha);
            return;
        }
        const auto pixels = pixels_of(destination);
        const auto area = pixels.intersect(clip);
        if (area.empty()) return;
        ++stats_.images_drawn;
        const long dw = pixels.x1 - pixels.x0, dh = pixels.y1 - pixels.y0;
        const unsigned a = static_cast<unsigned>(std::lround(std::clamp(alpha, 0.0F, 1.0F) * 255.0F));
        const bool one_to_one = dw == static_cast<long>(image.width) && dh == static_cast<long>(image.height);
        const bool copy = image.opaque && a == 255;
        for (long y = area.y0; y < area.y1; ++y) {
            const long sy = one_to_one ? y - pixels.y0 : static_cast<long>(((y - pixels.y0) * 2 + 1) * static_cast<long long>(image.height) / (2 * dh));
            const auto* source = image.bgra + static_cast<std::size_t>(std::clamp(sy, 0L, static_cast<long>(image.height) - 1)) * image.stride;
            auto* row = at(area.x0, y);
            if (one_to_one && copy) {
                std::memcpy(row, source + static_cast<std::size_t>(area.x0 - pixels.x0) * 4, static_cast<std::size_t>(area.x1 - area.x0) * 4);
                continue;
            }
            for (long x = area.x0; x < area.x1; ++x, row += 4) {
                const long sx = one_to_one ? x - pixels.x0 : static_cast<long>(((x - pixels.x0) * 2 + 1) * static_cast<long long>(image.width) / (2 * dw));
                const auto* pixel = source + static_cast<std::size_t>(std::clamp(sx, 0L, static_cast<long>(image.width) - 1)) * 4;
                if (copy) std::memcpy(row, pixel, 4);
                else blend(row, pixel, a);
            }
        }
    }

    // A contentsRect other than the unit rectangle: the source part scaled
    // into `destination`, nearest neighbour, edge pixels repeated outside it.
    void sub_image(const Image& image, const Source& source, const Box& destination, PixelRect clip, float alpha) {
        const auto pixels = pixels_of(destination);
        const auto area = pixels.intersect(clip);
        if (area.empty()) return;
        ++stats_.images_drawn;
        const double dw = static_cast<double>(pixels.x1 - pixels.x0), dh = static_cast<double>(pixels.y1 - pixels.y0);
        const unsigned a = static_cast<unsigned>(std::lround(std::clamp(alpha, 0.0F, 1.0F) * 255.0F));
        const bool copy = image.opaque && a == 255;
        const long last_x = static_cast<long>(image.width) - 1, last_y = static_cast<long>(image.height) - 1;
        for (long y = area.y0; y < area.y1; ++y) {
            const long sy = std::clamp(static_cast<long>(std::floor(source.y + (static_cast<double>(y - pixels.y0) + 0.5) * source.height / dh)), 0L, last_y);
            const auto* row_source = image.bgra + static_cast<std::size_t>(sy) * image.stride;
            auto* row = at(area.x0, y);
            for (long x = area.x0; x < area.x1; ++x, row += 4) {
                const long sx = std::clamp(static_cast<long>(std::floor(source.x + (static_cast<double>(x - pixels.x0) + 0.5) * source.width / dw)), 0L, last_x);
                const auto* pixel = row_source + static_cast<std::size_t>(sx) * 4;
                if (copy) std::memcpy(row, pixel, 4);
                else blend(row, pixel, a);
            }
        }
    }
};

} // namespace

std::optional<Gravity> gravity_named(std::string_view name) {
    static constexpr std::pair<std::string_view, Gravity> table[] = {
        {"center", Gravity::center}, {"top", Gravity::top}, {"bottom", Gravity::bottom}, {"left", Gravity::left},
        {"right", Gravity::right}, {"topLeft", Gravity::top_left}, {"topRight", Gravity::top_right},
        {"bottomLeft", Gravity::bottom_left}, {"bottomRight", Gravity::bottom_right}, {"resize", Gravity::resize},
        {"resizeAspect", Gravity::resize_aspect}, {"resizeAspectFill", Gravity::resize_aspect_fill}};
    for (const auto& [key, value] : table)
        if (key == name) return value;
    return std::nullopt;
}

PixelRect PixelRect::intersect(const PixelRect& other) const {
    return {std::max(x0, other.x0), std::max(y0, other.y0), std::min(x1, other.x1), std::min(y1, other.y1)};
}

Affine root_transform(const Layer& root, const Placement& placement) {
    const auto& b = root.bounds;
    const auto& f = placement.frame;
    const double sx = b.size.width > 0 ? f.size.width / b.size.width : 1.0;
    const double sy = b.size.height > 0 ? f.size.height / b.size.height : 1.0;
    const double s = placement.scale;
    // x -> (f.x + (x - b.x) * sx) * s; y -> (H - (f.y + (y - b.y) * sy)) * s.
    return {s * sx, 0, 0, -s * sy, (f.origin.x - b.origin.x * sx) * s, (placement.content_height - f.origin.y + b.origin.y * sy) * s};
}

Stats compose(const Layer& root, const Placement& placement, Target& target, PixelRect clip, bool root_self) {
    Stats stats;
    if (!target.bgra || target.width == 0 || target.height == 0) return stats;
    const PixelRect whole{0, 0, static_cast<long>(target.width), static_cast<long>(target.height)};
    clip = clip.empty() ? whole : clip.intersect(whole);
    if (clip.empty()) return stats;
    Composer(target, stats).layer(root, root_transform(root, placement), clip, 1.0F, root_self);
    return stats;
}

} // namespace alchemy::gui::quartzcore::tree
