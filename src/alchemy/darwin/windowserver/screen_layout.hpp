#pragma once

// Win32 physical-pixel desktop <-> AppKit point space (pure geometry).
//
// Per-monitor DPI v2 gives every monitor its own scale, so the virtual
// desktop's pixel rectangles cannot simply be divided by one factor. The
// point space is built like a mixed-DPI AppKit arrangement: the primary
// monitor's top-left pixel (Win32 0,0) is point 0,0 (top-down), each monitor's
// size in points is its pixel size divided by its own scale, and a monitor
// that shares an edge with an already placed monitor is placed against that
// edge in point space (its offset along the edge measured in the neighbour's
// points). Monitors touching nothing placed are positioned by their pixel
// origin divided by the primary scale. Within a monitor the mapping is
// linear, so pixel -> point -> pixel round-trips exactly (up to rounding to
// whole pixels by callers).
//
// AppKit flips y: AppKit y = primary height (points) - top-down y.

#include "alchemy/gui/window_server.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace alchemy::gui::win32 {

struct PixelRect {
    std::int32_t left{}, top{}, right{}, bottom{};
    [[nodiscard]] std::int32_t width() const noexcept { return right - left; }
    [[nodiscard]] std::int32_t height() const noexcept { return bottom - top; }
};

struct PixelPoint { double x{}, y{}; };

struct MonitorPixels {
    PixelRect bounds;
    PixelRect work;          // bounds without the taskbar / app bars
    double scale{1};         // dpi / 96
    std::uintptr_t handle{}; // HMONITOR, for lookups
};

struct MonitorPlacement {
    MonitorPixels pixels;
    double x{}, y{};         // top-down point origin
    double width{}, height{};
};

class ScreenLayout {
public:
    ScreenLayout() = default;
    // monitors[0] is the primary monitor. An empty list yields an empty
    // layout whose conversions throw.
    explicit ScreenLayout(std::vector<MonitorPixels> monitors);

    [[nodiscard]] bool empty() const noexcept { return monitors_.empty(); }
    [[nodiscard]] const std::vector<MonitorPlacement>& monitors() const noexcept { return monitors_; }
    [[nodiscard]] double primary_height() const;   // points

    // Monitor containing the pixel / point, else the nearest one.
    [[nodiscard]] std::size_t monitor_for_pixel(PixelPoint pixel) const;
    [[nodiscard]] std::size_t monitor_for_point(Point point) const;
    // Monitor with the largest overlap with an AppKit rect (else nearest to
    // its center).
    [[nodiscard]] std::size_t monitor_for_rect(Rect rect) const;
    // Index of the monitor with this handle, or monitors().size().
    [[nodiscard]] std::size_t monitor_for_handle(std::uintptr_t handle) const noexcept;

    // Screen pixels <-> AppKit screen points, through one monitor's mapping
    // (default: the monitor containing / nearest to the position).
    [[nodiscard]] Point to_points(PixelPoint pixel) const;
    [[nodiscard]] Point to_points(PixelPoint pixel, std::size_t monitor) const;
    [[nodiscard]] PixelPoint to_pixels(Point point) const;
    [[nodiscard]] PixelPoint to_pixels(Point point, std::size_t monitor) const;

    // A pixel rect -> AppKit rect: the top-left corner through `monitor`'s
    // mapping, the size divided by `scale` (the window's DPI scale).
    [[nodiscard]] Rect to_points(const PixelRect& rect, std::size_t monitor, double scale) const;
    // AppKit rect -> pixel rect (rounded to whole pixels), the inverse.
    [[nodiscard]] PixelRect to_pixels(const Rect& rect, std::size_t monitor, double scale) const;

    [[nodiscard]] Rect frame(std::size_t monitor) const;
    [[nodiscard]] Rect visible_frame(std::size_t monitor) const;

private:
    const MonitorPlacement& at(std::size_t monitor) const;
    std::vector<MonitorPlacement> monitors_;
};

} // namespace alchemy::gui::win32
