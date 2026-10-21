#pragma once

// Host area "gpu" (owner: r3). The rules are in include/alchemy/host/host.hpp. The Vulkan instance
// and window surfaces (Vulkan itself is portable), and the host 2D, text and imaging stacks under
// CoreGraphics, CoreText and ImageIO. GUI-only: the backends are src/alchemy/host/<backend>/gui/,
// compiled into alchemy-fx.

#include "alchemy/host/host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::host::gpu {

// ---- 2D values ----
struct Point { double x{}, y{}; };
// x' = a x + c y + tx, y' = b x + d y + ty.
struct Matrix { double a{1}, b{}, c{}, d{1}, tx{}, ty{}; };
enum class PathOp : std::uint32_t { move = 0, line = 1, quad = 2, cubic = 3, close = 4 };
struct PathElement {
    PathOp op{};
    std::array<Point, 3> points{};     // move/line: 1, quad: 2, cubic: 3
};

// ---- Vulkan (Metal, CAMetalLayer) ----
// Vulkan's own types stay with the callers (vulkan.h without a platform
// define): handles and function pointers cross this API untyped.

// The system Vulkan loader's vkGetInstanceProcAddr (never a loader from the
// guest's directory). Error::not_found: no loader is installed;
// Error::unsupported: the loader lacks the entry point.
[[nodiscard]] Result<void*> vulkan_get_instance_proc_addr();
// The instance extension a window surface needs on this host
// ("VK_KHR_win32_surface"), besides VK_KHR_surface.
[[nodiscard]] std::string_view vulkan_surface_extension() noexcept;
// Creates a VkSurfaceKHR for a native window (gui::WindowServer::native_handle)
// on `instance`, resolving the host's surface function through
// `get_instance_proc_addr`. Returns the VkResult; VK_ERROR_EXTENSION_NOT_PRESENT
// (-7) when the instance lacks vulkan_surface_extension().
[[nodiscard]] std::int32_t create_vulkan_surface(void* instance, void* get_instance_proc_addr, std::uintptr_t window,
                                                 std::uint64_t& surface);

// ---- image files (NSImage, NSBitmapImageRep, NSPasteboard) ----
struct Pixels {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;    // straight RGBA8, width * height * 4 bytes
};
// The first frame of an image file the host can read (PNG, TIFF, JPEG, BMP,
// GIF, ICO on win32). Error::unsupported: not a format the host reads.
[[nodiscard]] Result<Pixels> decode_image(std::span<const std::uint8_t> bytes);
enum class ImageFormat : std::uint8_t { tiff, bmp, gif, jpeg, png };
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_image(const Pixels& pixels, ImageFormat format);

// ---- image files with frames and metadata (ImageIO, CGImageSource) ----
struct ImageFrame {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;    // straight RGBA8, top row first (empty when only described)
    bool has_alpha{};
    double dpi_x{72}, dpi_y{72};
    double delay{};                    // GIF frame delay, seconds
    std::uint32_t orientation{1};      // EXIF orientation (1: up)
    std::uint32_t bits_per_component{8};   // of the file
};
// The frames of an image file the host reads. Error::unsupported: not one.
[[nodiscard]] Result<std::size_t> image_frame_count(std::span<const std::uint8_t> bytes);
// Frame `index`: its header facts, and with `pixels` its RGBA (opaque
// formats get alpha 0xff).
[[nodiscard]] Result<ImageFrame> image_frame(std::span<const std::uint8_t> bytes, std::size_t index, bool pixels);
// Encodes RGBA frames; TIFF and GIF keep every frame, the others the first.
// `quality` in 0..1 for JPEG.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_image_frames(ImageFormat format, std::span<const Pixels> frames, double quality);

// ---- fonts (CoreText, CGFont) ----
// Weight, style and stretch are the OpenType scales DirectWrite uses:
// weight 1..999 (400 regular, 700 bold), style 0 upright / 1 oblique /
// 2 italic, stretch 1..9 (5 normal, 0 undefined).
namespace font_weight {
inline constexpr int thin = 100, extra_light = 200, light = 300, normal = 400, medium = 500, semi_bold = 600, bold = 700,
    extra_bold = 800, black = 900;
}
namespace font_style { inline constexpr int normal = 0, oblique = 1, italic = 2; }
namespace font_stretch { inline constexpr int undefined = 0, normal = 5; }

// Design-unit metrics of a face.
struct FontMetrics {
    double units_per_em{}, ascent{}, descent{}, leading{}, cap_height{}, x_height{}, underline_position{}, underline_thickness{};
    double box_left{}, box_bottom{}, box_width{}, box_height{};   // glyph bounding box, y up
    double slant{};                    // the italic angle of the post table, degrees
};
// The names (English where localized) and facts of a face.
struct FontInfo {
    std::string family, full_name, postscript, style_name;
    int weight{font_weight::normal}, style{font_style::normal}, stretch{font_stretch::normal};
    FontMetrics metrics;
    bool monospace{}, symbol{}, color{};
};
// Design-unit metrics of one glyph.
struct GlyphMetrics {
    std::int32_t left_bearing{}, right_bearing{}, top_bearing{}, bottom_bearing{}, vertical_origin_y{};
    std::uint32_t advance_width{}, advance_height{};
};

// A set of fonts to look families and names up in: the system's, or the
// fonts this process registered. Copyable; equal handles are the same set.
class FontCollection {
public:
    struct Impl;
    FontCollection() = default;
    explicit FontCollection(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(impl_); }
    friend bool operator==(const FontCollection& a, const FontCollection& b) noexcept { return a.impl_ == b.impl_; }
    [[nodiscard]] Impl* impl() const noexcept { return impl_.get(); }

private:
    std::shared_ptr<Impl> impl_;
};
// A font face. Copyable.
class Font {
public:
    struct Impl;
    Font() = default;
    explicit Font(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(impl_); }
    // The same face of the same font file (not merely an equal handle).
    [[nodiscard]] bool same_face(const Font& other) const;
    [[nodiscard]] Result<FontInfo> info() const;
    [[nodiscard]] std::uint32_t glyph_count() const;
    // Glyphs of Unicode code points (0 where the face has none).
    [[nodiscard]] Result<std::vector<std::uint16_t>> glyphs(std::span<const std::uint32_t> code_points) const;
    [[nodiscard]] Result<std::vector<GlyphMetrics>> glyph_metrics(std::span<const std::uint16_t> glyphs) const;
    // The outlines of `glyphs` at `size`, each at its `offsets` entry (x along
    // the baseline, y up from it), as moves, lines, cubics and closes in glyph
    // space (y down), mapped through `transform`.
    [[nodiscard]] Result<std::vector<PathElement>> outlines(double size, std::span<const std::uint16_t> glyphs,
                                                            std::span<const Point> offsets, const Matrix& transform) const;
    // An OpenType table by tag ('p','o','s','t' is 0x706f7374); Error::not_found when the face has none.
    [[nodiscard]] Result<std::vector<std::uint8_t>> table(std::uint32_t tag) const;
    [[nodiscard]] Impl* impl() const noexcept { return impl_.get(); }

private:
    std::shared_ptr<Impl> impl_;
};

[[nodiscard]] Result<FontCollection> system_fonts();
// The fonts registered with register_font_file; empty before the first.
[[nodiscard]] FontCollection registered_fonts();
// The face of `family` closest to weight / style / stretch.
[[nodiscard]] Result<Font> family_font(const FontCollection& collection, std::string_view family, int weight, int style, int stretch);
enum class FontName : std::uint8_t { postscript, full };
[[nodiscard]] Result<Font> named_font(const FontCollection& collection, FontName kind, std::string_view name);
// The faces of `family`, without simulated (synthesized) styles.
[[nodiscard]] Result<std::vector<Font>> family_faces(const FontCollection& collection, std::string_view family);
[[nodiscard]] Result<std::vector<std::string>> family_names(const FontCollection& collection);
// Adds a font file's faces to registered_fonts(). Error::invalid_argument:
// empty data; Error::io: it cannot be loaded; Error::unsupported: not a font
// file the host reads; Error::other: it could not be added.
[[nodiscard]] Status register_font_file(std::span<const std::uint8_t> bytes);
// The first face of a font file, outside every collection (CGFontCreateWithDataProvider).
[[nodiscard]] Result<Font> font_from_data(std::span<const std::uint8_t> bytes);
// The host's fallback for the start of `text` that a font of `family` lacks:
// how many UTF-16 units it covers, the face and its scale.
struct Fallback {
    std::size_t length{};
    Font font;
    double scale{1};
};
[[nodiscard]] Result<Fallback> fallback_font(std::u16string_view text, std::string_view family, int weight, int style, int stretch);

// ---- text layout (CoreText lines) ----
// Shaping, bidi, font fallback and line breaking of UTF-16 text.
struct TextSpan {
    std::size_t start{}, length{};     // UTF-16 units
    FontCollection collection;
    std::string family;
    int weight{font_weight::normal}, style{font_style::normal}, stretch{font_stretch::normal};
    double size{12};
    double character_spacing{};        // added after each character (kern), 0: none
    std::size_t tag{};                 // reported back on the runs it produces
};
enum class Wrapping : std::uint8_t { none, character, word };
struct TextLayoutRequest {
    std::u16string_view text;
    TextSpan base;                     // the whole text's format (start and length unused)
    std::span<const TextSpan> spans;
    Wrapping wrapping{Wrapping::word};
    bool rtl{}, justified{};
    double tab_interval{28};
    double width{}, height{};
};
struct LaidOutRun {
    Font font;
    double size{};
    std::vector<std::uint16_t> glyphs;
    std::vector<double> advances;
    std::vector<Point> offsets;        // x: advance offset, y: ascender offset
    std::vector<std::uint16_t> clusters;   // per UTF-16 unit of the run: its first glyph
    std::uint32_t text_position{}, text_length{};
    bool rtl{};
    double x{}, y{};                   // the run's pen origin
    std::size_t tag{};
};
struct LaidOutLine { std::uint32_t length{}, newline_length{}; };
struct TextLayout {
    std::vector<LaidOutRun> runs;      // in drawing order
    std::vector<LaidOutLine> lines;
};
[[nodiscard]] Result<TextLayout> layout_text(const TextLayoutRequest& request);

// ---- 2D drawing (the Quartz 2D pipeline) ----
// Integer pixel rectangle [x0, x1) x [y0, y1).
struct PixelRect {
    std::int32_t x0{}, y0{}, x1{}, y1{};
    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] std::uint32_t width() const { return empty() ? 0 : static_cast<std::uint32_t>(x1 - x0); }
    [[nodiscard]] std::uint32_t height() const { return empty() ? 0 : static_cast<std::uint32_t>(y1 - y0); }
};
struct Box { double left{}, top{}, right{}, bottom{}; };
struct Color { double r{}, g{}, b{}, a{1}; };   // straight components
// Premultiplied BGRA rows, `width` * 4 bytes each, top row first.
struct BgraView {
    std::uint32_t width{}, height{};
    const std::uint8_t* pixels{};
};
struct GradientStop {
    double position{};                 // 0..1
    Color color;
};
struct Brush {
    enum class Kind : std::uint8_t { solid, tiles, linear_gradient };
    Kind kind{Kind::solid};
    Color color{1, 1, 1, 1};           // solid
    double opacity{1};                 // tiles and gradients
    BgraView tiles;                    // repeated both ways, linear filtering
    Matrix tiles_transform;            // tile pixel space -> the surface
    std::span<const GradientStop> stops;   // gamma 2.2, clamped at the ends
    Point start, end;
};
struct StrokeStyle {
    enum class Cap : std::uint8_t { flat, round, square };
    enum class Join : std::uint8_t { miter_or_bevel, round, bevel };
    Cap cap{Cap::flat};
    Join join{Join::miter_or_bevel};
    double miter_limit{10};            // the host's limit (Direct2D: miter length per half stroke width)
    std::vector<double> dashes;        // in multiples of the stroke width; empty: solid
    double dash_offset{};              // in multiples of the stroke width
};

// An immutable path geometry. Copyable.
class Geometry {
public:
    struct Impl;
    Geometry() = default;
    explicit Geometry(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(impl_); }
    [[nodiscard]] Impl* impl() const noexcept { return impl_.get(); }
    // A path of `elements`; a line or curve without a current figure starts
    // one at the last point. `filled` false makes hollow figures (strokes).
    [[nodiscard]] static Result<Geometry> path(std::span<const PathElement> elements, bool even_odd, bool filled = true);
    // The outline of its stroke, read back as moves, lines, cubics and closes
    // mapped through `transform`.
    [[nodiscard]] Result<std::vector<PathElement>> widened(double width, const StrokeStyle& style, const Matrix& transform) const;
    // Whether `point` lies on its stroke of `width` (default stroke style).
    [[nodiscard]] Result<bool> stroke_contains(Point point, double width) const;

private:
    std::shared_ptr<Impl> impl_;
};

enum class Blend : std::uint8_t { source_over, copy, add };
enum class Interpolation : std::uint8_t { nearest, linear, high_quality };

// A software drawing surface of premultiplied BGRA pixels (Direct2D over a
// WIC bitmap on win32). Between begin and end it draws; otherwise its pixels
// can be locked, read and written.
class Surface {
public:
    struct Impl;
    [[nodiscard]] static Result<std::unique_ptr<Surface>> create(std::uint32_t width, std::uint32_t height);
    explicit Surface(std::unique_ptr<Impl> impl) noexcept;
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    // Whether set_blend can select copy and add (a device context).
    [[nodiscard]] bool blend_modes() const noexcept;

    [[nodiscard]] Status begin();
    [[nodiscard]] Status end();
    // Pixels between draws.
    struct Lock {
        std::shared_ptr<void> hold;
        std::uint8_t* data{};
        std::uint32_t stride{};
        [[nodiscard]] std::uint8_t* at(std::int32_t x, std::int32_t y, const PixelRect& origin) const {
            return data + static_cast<std::size_t>(y - origin.y0) * stride + static_cast<std::size_t>(x - origin.x0) * 4;
        }
    };
    [[nodiscard]] Result<Lock> lock(const PixelRect& rect, bool write);

    // Drawing (between begin and end).
    void set_transform(const Matrix& transform);
    void set_antialias(bool antialias);
    void set_text_antialias(bool grayscale);   // false: aliased glyphs
    void set_blend(Blend blend);               // only when blend_modes()
    void push_axis_clip(const Box& box, bool antialias);
    void pop_axis_clip();
    [[nodiscard]] Status push_clip(const Geometry& geometry, bool antialias);
    void pop_clip();
    [[nodiscard]] Status fill_rect(const Box& box, const Brush& brush);
    [[nodiscard]] Status fill(const Geometry& geometry, const Brush& brush);
    [[nodiscard]] Status stroke(const Geometry& geometry, const Brush& brush, double width, const StrokeStyle& style);
    [[nodiscard]] Status draw_bitmap(const BgraView& pixels, const Box& destination, double opacity, Interpolation interpolation);
    // Glyphs at `size`, each at its `offsets` entry (x along the baseline, y
    // up from it) from the origin of the current transform.
    [[nodiscard]] Status draw_glyphs(const Font& font, double size, std::span<const std::uint16_t> glyphs, std::span<const Point> offsets,
                                     const Brush& brush);

private:
    std::unique_ptr<Impl> impl_;
};

// ---- system text rasterizing (AppKit's software text; GDI on win32) ----
// A host font family at an em height in quarter pixels.
struct RasterFont {
    std::string family;
    int quarter_pixels{};
    bool bold{}, italic{};
};
// In pixels at the font's size. Cap and x heights when the font reports them.
struct RasterMetrics {
    double ascent{}, descent{}, leading{}, average_width{}, max_width{};
    std::optional<double> cap_height, x_height;
};
[[nodiscard]] Result<RasterMetrics> raster_metrics(const RasterFont& font);
// The advance of a line of text, and the advance after each UTF-16 unit, in pixels.
[[nodiscard]] Result<double> raster_width(const RasterFont& font, std::u16string_view text);
[[nodiscard]] Result<std::vector<double>> raster_prefix_widths(const RasterFont& font, std::u16string_view text);
// The coverage (0..1) of `text` drawn into a `width` x `height` plane, its
// baseline starting at (x, baseline), top row first.
[[nodiscard]] Result<std::vector<float>> raster_coverage(const RasterFont& font, std::u16string_view text, int width, int height, int x,
                                                         int baseline);

} // namespace alchemy::host::gpu
