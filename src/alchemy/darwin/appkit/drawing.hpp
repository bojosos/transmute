#pragma once

// AppKit's software drawing layer (owner: AppKit). Window backing stores of
// non-layer-backed views, NSGraphicsContext state, and the host-side
// rasterizer used by AppKit's own drawing (standard controls, NSRectFill,
// NSBezierPath, NSString / NSImage drawing).
//
// Pixels are BGRA8 with premultiplied alpha, rows top to bottom, the layout
// of a CGBitmapContext created with kCGImageAlphaPremultipliedFirst |
// kCGBitmapByteOrder32Little and of a Win32 32-bit DIB; window backing
// stores live in guest memory so a guest CGContext (CoreGraphics provider)
// can draw into the same pixels.
//
// Not a contract header: internal to src/alchemy/appkit. The CoreGraphics /
// CoreText providers are separate (q-quartz); AppKit reaches a guest-visible
// CGContextRef only through the public CG C API (see graphics.cpp).
//
// Text: GDI renders grayscale-antialiased glyph coverage (Segoe UI stands in
// for the system font, Consolas for monospaced ones); CoreText metrics are
// approximated from the host font's.

#include "alchemy/gui/window_server.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace transmute { class GuestMemory; }

namespace alchemy::gui::appkit::drawing {

using Address = std::uint64_t;

struct Color {
    double r{}, g{}, b{}, a{1};   // straight (non-premultiplied) sRGB, 0..1
    [[nodiscard]] Color with_alpha(double value) const { return {r, g, b, a * value}; }
    [[nodiscard]] static Color gray(double white, double alpha = 1) { return {white, white, white, alpha}; }
};

// User space -> device pixels: device = (a*x + c*y + tx, b*x + d*y + ty).
struct Affine {
    double a{1}, b{0}, c{0}, d{1}, tx{0}, ty{0};
    [[nodiscard]] Point apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    // The composition "this after other": other applies first.
    [[nodiscard]] Affine then_after(const Affine& other) const {
        return {a * other.a + c * other.b, b * other.a + d * other.b, a * other.c + c * other.d, b * other.c + d * other.d,
                a * other.tx + c * other.ty + tx, b * other.tx + d * other.ty + ty};
    }
    [[nodiscard]] static Affine translation(double x, double y) { return {1, 0, 0, 1, x, y}; }
    [[nodiscard]] static Affine scaling(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    [[nodiscard]] bool axis_aligned() const { return b == 0 && c == 0; }
    [[nodiscard]] std::optional<Affine> inverted() const;
};

// Integer device rectangle [x0, x1) x [y0, y1).
struct PixelRect {
    int x0{}, y0{}, x1{}, y1{};
    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] PixelRect intersect(PixelRect other) const;
    [[nodiscard]] PixelRect unite(PixelRect other) const;
};
// The device pixels a user-space rectangle touches (bounding box).
[[nodiscard]] PixelRect device_bounds(const Affine& ctm, Rect rect);
// The transformed rectangle's bounding box.
[[nodiscard]] Rect bounding_rect(const Affine& transform, Rect rect);
[[nodiscard]] Rect intersect(Rect a, Rect b);
[[nodiscard]] Rect unite(Rect a, Rect b);   // an empty rect is the identity

// ---- pixel storage --------------------------------------------------------------
class PixelBuffer {
public:
    virtual ~PixelBuffer() = default;
    [[nodiscard]] virtual std::uint32_t width() const = 0;
    [[nodiscard]] virtual std::uint32_t height() const = 0;
    // `out.size()` pixels of row `y` from column `x` (premultiplied BGRA as
    // little-endian 0xAARRGGBB words).
    virtual void read(std::uint32_t x, std::uint32_t y, std::span<std::uint32_t> out) const = 0;
    virtual void write(std::uint32_t x, std::uint32_t y, std::span<const std::uint32_t> in) = 0;
};
class HostBuffer final : public PixelBuffer {
public:
    HostBuffer(std::uint32_t width, std::uint32_t height, std::uint32_t fill = 0);
    [[nodiscard]] std::uint32_t width() const override { return width_; }
    [[nodiscard]] std::uint32_t height() const override { return height_; }
    void read(std::uint32_t x, std::uint32_t y, std::span<std::uint32_t> out) const override;
    void write(std::uint32_t x, std::uint32_t y, std::span<const std::uint32_t> in) override;
    [[nodiscard]] std::uint32_t at(std::uint32_t x, std::uint32_t y) const { return pixels_[y * width_ + x]; }
    [[nodiscard]] std::span<const std::uint32_t> pixels() const { return pixels_; }
private:
    std::uint32_t width_, height_;
    std::vector<std::uint32_t> pixels_;
};
// Rows of `stride` bytes at a guest address (window backing stores; other
// CGBitmapContexts). Pixels are BGRA in memory, or RGBA when `rgba` (a
// kCGImageAlphaPremultipliedLast bitmap context).
class GuestBuffer final : public PixelBuffer {
public:
    GuestBuffer(transmute::GuestMemory& memory, Address pixels, std::uint32_t width, std::uint32_t height, std::size_t stride,
                bool rgba = false)
        : memory_(memory), pixels_(pixels), width_(width), height_(height), stride_(stride), rgba_(rgba) {}
    [[nodiscard]] std::uint32_t width() const override { return width_; }
    [[nodiscard]] std::uint32_t height() const override { return height_; }
    void read(std::uint32_t x, std::uint32_t y, std::span<std::uint32_t> out) const override;
    void write(std::uint32_t x, std::uint32_t y, std::span<const std::uint32_t> in) override;
private:
    transmute::GuestMemory& memory_;
    Address pixels_;
    std::uint32_t width_, height_;
    std::size_t stride_;
    bool rgba_;
};

// ---- paths --------------------------------------------------------------------------
class Path {
public:
    enum class Op : std::uint8_t { move, line, curve, close };
    struct Element { Op op; std::array<Point, 3> points; };
    void move_to(Point p);
    void line_to(Point p);
    void curve_to(Point c1, Point c2, Point end);
    void close();
    void add_rect(Rect r);
    void add_rounded_rect(Rect r, double rx, double ry);
    void add_oval(Rect r);
    // Circular arc around `center` from `start` to `end` degrees
    // (counterclockwise in a y-up space unless `clockwise`).
    void add_arc(Point center, double radius, double start, double end, bool clockwise);
    void append(const Path& other);
    [[nodiscard]] bool empty() const { return elements_.empty(); }
    [[nodiscard]] const std::vector<Element>& elements() const { return elements_; }
    [[nodiscard]] std::optional<Point> current_point() const { return current_; }
    [[nodiscard]] Rect bounds() const;   // control-point bounds
    // Polylines in device space (curves flattened); `closed` per contour.
    struct Contour { std::vector<Point> points; bool closed{}; };
    [[nodiscard]] std::vector<Contour> flatten(const Affine& ctm) const;
    void transform(const Affine& t);
private:
    std::vector<Element> elements_;
    std::optional<Point> current_, start_;
};

enum class Blend : std::uint8_t { source_over, copy, clear };
enum class Winding : std::uint8_t { non_zero, even_odd };

// ---- fonts and text ---------------------------------------------------------------
struct FontSpec {
    std::string name{".AppleSystemUIFont"};   // PostScript or family name as the guest gave it
    double size{13};                          // points (em size)
    bool bold{}, italic{}, monospace{};
};
struct FontMetrics {
    double ascent{}, descent{}, leading{}, cap_height{}, x_height{}, average_width{}, max_width{};   // points, descent > 0
    [[nodiscard]] double line_height() const { return ascent + descent + leading; }
};
[[nodiscard]] FontMetrics font_metrics(const FontSpec& font);
// Advance width in points of UTF-16 text on one line.
[[nodiscard]] double text_width(const FontSpec& font, std::u16string_view text);
// Cumulative advances: result[i] is the width of text[0..i] (i + 1 units).
[[nodiscard]] std::vector<double> prefix_widths(const FontSpec& font, std::u16string_view text);
// The host font family standing in for `font` (for diagnostics and tests).
[[nodiscard]] std::wstring host_family(const FontSpec& font);
// Lines of `text` broken at '\n' and, when `width` > 0, wrapped at spaces
// (words longer than the width are broken by character). Offsets into text.
struct TextLine { std::size_t start{}, length{}; double width{}; };
[[nodiscard]] std::vector<TextLine> layout_lines(const FontSpec& font, std::u16string_view text, double width);

// UTF-8 <-> UTF-16.
[[nodiscard]] std::u16string utf16(std::string_view utf8);
[[nodiscard]] std::string utf8(std::u16string_view text);

// ---- the rasterizer -------------------------------------------------------------
struct GraphicsState {
    Affine ctm;                     // user space -> device pixels
    PixelRect clip;                 // device clip (intersection of every clip)
    Color fill{0, 0, 0, 1}, stroke{0, 0, 0, 1};
    double line_width{1};
    double alpha{1};
    Blend blend{Blend::source_over};
    bool antialias{true};
    FontSpec font;
};

class Canvas {
public:
    explicit Canvas(PixelBuffer& target);
    [[nodiscard]] PixelBuffer& target() const { return *target_; }
    GraphicsState state;
    void save() { stack_.push_back(state); }
    void restore();
    [[nodiscard]] std::size_t depth() const { return stack_.size(); }

    void clip_to_rect(Rect user);   // axis-aligned in device space (bounding box otherwise)
    void fill_rect(Rect user, Color color);
    void fill_path(const Path& path, Color color, Winding winding = Winding::non_zero);
    void stroke_path(const Path& path, Color color, double width);
    void stroke_rect(Rect user, Color color, double width);
    void stroke_line(Point a, Point b, Color color, double width);
    // A straight RGBA image (rows top to bottom) into a user-space rect;
    // `flip` draws it upside down (the image's top row at the rect's minimum y).
    void draw_image(const Image& image, Rect user, double alpha, bool flip = false);
    // One line of text with its baseline starting at user-space `origin`;
    // glyphs stay upright on the device whatever the ctm's y direction.
    // Returns the advance in points.
    double draw_text(std::u16string_view text, const FontSpec& font, Point origin, Color color);
    // Fills device coverage (0..1 per pixel of `area`) with `color`.
    void blend_coverage(PixelRect area, std::span<const float> coverage, Color color);

private:
    void blend_span(int y, int x0, std::span<const float> coverage, Color color);
    PixelBuffer* target_;
    std::vector<GraphicsState> stack_;
};

// Coverage rasterization of device polylines (exposed for tests).
void rasterize(const std::vector<Path::Contour>& contours, Winding winding, PixelRect clip,
               const std::function<void(int y, int x0, std::span<const float>)>& span);

} // namespace alchemy::gui::appkit::drawing
