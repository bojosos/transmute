#pragma once

// CGContext model (context.cpp): graphics state, current path, bitmap
// backing and the drawing pipeline. Shared with CoreText (glyph drawing) and
// the AppKit drawing classes (NSGraphicsContext).

#include "alchemy/darwin/coregraphics/surface.hpp"

namespace alchemy::gui::quartz2d {

// CGTextDrawingMode
namespace text_mode {
inline constexpr std::int32_t fill = 0, stroke = 1, fill_stroke = 2, invisible = 3, fill_clip = 4, stroke_clip = 5,
    fill_stroke_clip = 6, clip = 7;
}
// CGBlendMode
namespace blend {
inline constexpr std::int32_t normal = 0, multiply = 1, screen = 2, overlay = 3, darken = 4, lighten = 5, color_dodge = 6,
    color_burn = 7, soft_light = 8, hard_light = 9, difference = 10, exclusion = 11, hue = 12, saturation = 13, color = 14,
    luminosity = 15, clear = 16, copy = 17, source_in = 18, source_out = 19, source_atop = 20, destination_over = 21,
    destination_in = 22, destination_out = 23, destination_atop = 24, exclusive_or = 25, plus_darker = 26, plus_lighter = 27;
}

struct Paint {
    Rgba color{0, 0, 0, 1};
    ObjectRef space;                      // the color space of SetFill/StrokeColor components
    ObjectRef pattern;                    // CGPatternRef (pattern paint)
    std::vector<double> tint;             // uncolored pattern tint components (base space) + alpha
};

struct Shadow {
    bool enabled{};
    Size offset{};                        // base space (not affected by the CTM)
    double blur{};
    Rgba color{0, 0, 0, 1.0 / 3.0};
};

struct ClipItem {
    enum class Kind { rect, geometry, mask } kind{Kind::rect};
    Rect device{};                        // device-space bounds (the rectangle itself for Kind::rect)
    host::gpu::Geometry geometry;         // device space
    bool antialias{true};
    std::shared_ptr<const std::vector<std::uint8_t>> mask;   // surface-sized alpha (Kind::mask)
};

struct GState {
    Affine ctm;
    std::vector<std::shared_ptr<const ClipItem>> clips;
    Paint fill, stroke;
    double line_width{1}, miter_limit{10}, flatness{0.5};
    std::uint32_t cap{}, join{};
    std::vector<double> dashes;
    double dash_phase{};
    double alpha{1};
    std::int32_t blend_mode{blend::normal};
    bool antialias{true}, allows_antialias{true}, smooth_fonts{true}, allows_smoothing{true};
    std::int32_t interpolation{};         // CGInterpolationQuality
    std::int32_t intent{};
    Shadow shadow;
    ObjectRef font;                       // CGFontRef
    double font_size{12};
    double character_spacing{};
    std::int32_t text_mode{text_mode::fill};
    Size pattern_phase{};
};

struct ContextState final : ObjectPayload {
    std::recursive_mutex mutex;
    PixelLayout layout;
    Address data{};                       // guest pixels
    bool owns_data{}, data_heap{};
    Address release_callback{}, release_info{};   // CGBitmapContextReleaseDataCallback(info, data)
    ObjectRef space;
    std::shared_ptr<Surface> surface;
    Affine base_ctm;                      // the CTM of the default user space (backing scale)
    GState g;
    std::vector<GState> saved;
    PathData path;                        // base space
    Affine text_matrix;
    struct Layer {
        std::shared_ptr<Surface> surface;
        GState state;                     // the state at Begin (composites the layer)
        std::size_t depth{};              // saved.size() at Begin
    };
    std::vector<Layer> layers;
    std::shared_ptr<Surface> scratch, coverage;   // complex compositing (lazily created)
    bool destroyed{};
};

[[nodiscard]] std::shared_ptr<ContextState> context_state(Quartz& q, Address context);
// +1 bitmap context over guest memory `data` (0: allocated, zero-filled).
// Returns 0 (and logs like CoreGraphics) for an unsupported layout.
Address create_context(Quartz& q, const PixelLayout& layout, Address data, Address space, Address release_callback = 0,
                       Address release_info = 0);

// Drawing used by CoreText and AppKit (the context's mutex must not be held).
void fill_path(Quartz& q, ContextState& c, const PathData& base_path, bool even_odd);
void stroke_path(Quartz& q, ContextState& c, const PathData& base_path);
void clip_to_path(Quartz& q, ContextState& c, const PathData& base_path, bool even_odd);
void draw_image(Quartz& q, ContextState& c, Address image, const Rect& rect);
// Draws `run` at text-space offset `origin` (relative to the text position)
// through the text matrix and CTM, honouring the text drawing mode; `color`
// replaces the fill color when given.
void draw_glyphs(Quartz& q, ContextState& c, const GlyphRunDraw& run, Point origin, const std::optional<Rgba>& color);
// Fills a base-space rectangle with an explicit color (CoreText backgrounds, underlines).
void fill_rect_color(Quartz& q, ContextState& c, const Rect& user_rect, const Rgba& color);
// Gradients and shadings (user-space geometry, CGGradientDrawingOptions as flags).
void draw_gradient(Quartz& q, ContextState& c, const GradientInfo& gradient, bool radial, Point start, double start_radius, Point end,
                   double end_radius, bool extend_start, bool extend_end);
void clip_to_mask(Quartz& q, ContextState& c, const Rect& rect, Address mask);
void clear_rect(Quartz& q, ContextState& c, const Rect& rect);
void draw_tiled_image(Quartz& q, ContextState& c, Address image, const Rect& rect);
void begin_transparency_layer(Quartz& q, ContextState& c, const std::optional<Rect>& rect);
void end_transparency_layer(Quartz& q, ContextState& c);
void save_state(ContextState& c);
void restore_state(Quartz& q, ContextState& c);
// Writes guest-visible pixels; a no-op for contexts without transparency layers (always in sync).
void synchronize(Quartz& q, ContextState& c);
// Straight RGBA8, top row first.
[[nodiscard]] Image snapshot(Quartz& q, ContextState& c);

} // namespace alchemy::gui::quartz2d
