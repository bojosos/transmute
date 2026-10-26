#pragma once

// Quartz 2D contract (Q drawing layer <-> A4 AppKit, A7 QuartzCore, AppKit
// controls). Everything else of the layer is reached through the exported
// CoreGraphics / CoreText / ImageIO functions and Objective-C messages.
//
// Window compositing integration point (not wired by this layer):
// a window's backing store for drawRect:-based (non-layer-backed) views is a
// bitmap context of the content area in pixels:
//
//   const auto backing = create_bitmap_context(width_px, height_px, scale);
//   render_view_hierarchy(content_view, backing, dirty_rect_in_window_points);
//   const auto pixels = context_image(backing);   // straight RGBA8, top row first
//   window server: present `pixels` to the window's HWND (needs a present call
//   on include/alchemy/gui/window_server.hpp, owned by A3), then
//   CFRelease(backing) or keep it as the window's persistent backing store.
//
// render_view_hierarchy makes an NSGraphicsContext current for each view
// (+[NSGraphicsContext currentContext] answers inside -drawRect:), applies
// the frame / bounds / flipped transform and clips to the view, sends
// -drawRect: through ObjcRuntime::send (guest overrides), then draws the
// subviews in order. Only messages are used (frame, bounds, isFlipped,
// isHidden, subviews), so it works with any NSView implementation.
//
// Threading: main thread (AppKit). All functions require install_quartz2d.

#include "alchemy/gui/window_server.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace alchemy::gui {
struct InstallContext;
void install_quartz2d(InstallContext& context);
} // namespace alchemy::gui

namespace alchemy::gui::quartz2d {

// +1 CGContextRef over a new zero-filled bitmap of width x height pixels in
// premultiplied BGRA (kCGImageAlphaPremultipliedFirst |
// kCGBitmapByteOrder32Little, sRGB). The base CTM scales by `scale`
// (backing scale factor) so drawing happens in points with a bottom-left origin.
[[nodiscard]] std::uint64_t create_bitmap_context(std::uint32_t width, std::uint32_t height, double scale = 1.0);

// The current pixels of a bitmap context as straight RGBA8, top row first.
// nullopt when `context` is not a bitmap context.
[[nodiscard]] std::optional<Image> context_image(std::uint64_t context);

// Renders `view` and its subviews into `context` (whose user space is the
// window base space: bottom-left origin, points). `dirty` limits drawing
// (window base points); std::nullopt draws everything.
void render_view_hierarchy(std::uint64_t view, std::uint64_t context, std::optional<Rect> dirty = std::nullopt);

// +1 CGPath of the part of `path` from the fraction `start` to `end` of its
// length (CAShapeLayer strokeStart / strokeEnd; curves measured flattened),
// as open polylines in the path's order.
[[nodiscard]] std::uint64_t trimmed_path(std::uint64_t path, double start, double end);

// The CGContextRef of +[NSGraphicsContext currentContext] on this thread (0: none).
[[nodiscard]] std::uint64_t current_cg_context();

// Tests: the screens Quartz Display Services (CGMainDisplayID, CGDisplayBounds,
// ...) report, instead of the window server's. An empty function restores them.
void set_screens_override(std::function<std::vector<ScreenInfo>()> source);

} // namespace alchemy::gui::quartz2d
