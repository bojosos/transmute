#include "alchemy/darwin/windowserver/screen_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace alchemy::gui::win32 {
namespace {

bool overlaps(std::int32_t a_begin, std::int32_t a_end, std::int32_t b_begin, std::int32_t b_end) {
    return a_begin < b_end && a_end > b_begin;
}

// 0 inside the half-open rect [left, right) x [top, bottom), else the squared
// distance to it (at least a tiny positive value, so a containing rect always
// wins over one the point merely touches).
double distance_squared(double x, double y, double left, double top, double right, double bottom) {
    if (x >= left && x < right && y >= top && y < bottom) return 0;
    const auto dx = x < left ? left - x : (x >= right ? x - right : 0.0);
    const auto dy = y < top ? top - y : (y >= bottom ? y - bottom : 0.0);
    return std::max(dx * dx + dy * dy, 1e-300);
}

std::int32_t round_pixel(double value) {
    const auto rounded = std::floor(value + 0.5);
    if (rounded < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        rounded > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
        throw std::runtime_error("Alchemy window server: coordinate is outside the Win32 range");
    return static_cast<std::int32_t>(rounded);
}

} // namespace

ScreenLayout::ScreenLayout(std::vector<MonitorPixels> monitors) {
    monitors_.reserve(monitors.size());
    for (auto& monitor : monitors) {
        if (!(monitor.scale > 0)) monitor.scale = 1;
        MonitorPlacement placement;
        placement.pixels = monitor;
        placement.width = monitor.bounds.width() / monitor.scale;
        placement.height = monitor.bounds.height() / monitor.scale;
        monitors_.push_back(placement);
    }
    if (monitors_.empty()) return;
    std::vector<bool> placed(monitors_.size(), false);
    auto& primary = monitors_[0];
    primary.x = primary.pixels.bounds.left / primary.pixels.scale;
    primary.y = primary.pixels.bounds.top / primary.pixels.scale;
    placed[0] = true;
    for (bool progress = true; progress;) {
        progress = false;
        for (std::size_t index = 1; index < monitors_.size(); ++index) {
            if (placed[index]) continue;
            auto& monitor = monitors_[index];
            const auto& bounds = monitor.pixels.bounds;
            for (std::size_t other = 0; other < monitors_.size() && !placed[index]; ++other) {
                if (!placed[other]) continue;
                const auto& neighbour = monitors_[other];
                const auto& near = neighbour.pixels.bounds;
                const auto scale = neighbour.pixels.scale;
                if (overlaps(bounds.top, bounds.bottom, near.top, near.bottom) &&
                    (bounds.left == near.right || bounds.right == near.left)) {
                    monitor.x = bounds.left == near.right ? neighbour.x + neighbour.width : neighbour.x - monitor.width;
                    monitor.y = neighbour.y + (bounds.top - near.top) / scale;
                    placed[index] = true;
                } else if (overlaps(bounds.left, bounds.right, near.left, near.right) &&
                           (bounds.top == near.bottom || bounds.bottom == near.top)) {
                    monitor.y = bounds.top == near.bottom ? neighbour.y + neighbour.height : neighbour.y - monitor.height;
                    monitor.x = neighbour.x + (bounds.left - near.left) / scale;
                    placed[index] = true;
                }
            }
            progress = progress || placed[index];
        }
    }
    for (std::size_t index = 1; index < monitors_.size(); ++index) {
        if (placed[index]) continue;
        monitors_[index].x = monitors_[index].pixels.bounds.left / primary.pixels.scale;
        monitors_[index].y = monitors_[index].pixels.bounds.top / primary.pixels.scale;
    }
}

const MonitorPlacement& ScreenLayout::at(std::size_t monitor) const {
    if (monitors_.empty()) throw std::runtime_error("Alchemy window server: no display is attached");
    if (monitor >= monitors_.size()) throw std::runtime_error("Alchemy window server: unknown display index");
    return monitors_[monitor];
}

double ScreenLayout::primary_height() const { return at(0).y + at(0).height; }

std::size_t ScreenLayout::monitor_for_pixel(PixelPoint pixel) const {
    static_cast<void>(at(0));
    std::size_t best = 0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < monitors_.size(); ++index) {
        const auto& bounds = monitors_[index].pixels.bounds;
        const auto distance = distance_squared(pixel.x, pixel.y, bounds.left, bounds.top, bounds.right, bounds.bottom);
        if (distance < best_distance) {
            best = index;
            best_distance = distance;
        }
    }
    return best;
}

std::size_t ScreenLayout::monitor_for_point(Point point) const {
    const auto top_down = primary_height() - point.y;
    std::size_t best = 0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < monitors_.size(); ++index) {
        const auto& monitor = monitors_[index];
        const auto distance = distance_squared(point.x, top_down, monitor.x, monitor.y, monitor.x + monitor.width,
                                               monitor.y + monitor.height);
        if (distance < best_distance) {
            best = index;
            best_distance = distance;
        }
    }
    return best;
}

std::size_t ScreenLayout::monitor_for_rect(Rect rect) const {
    const auto height = primary_height();
    const auto top = height - (rect.origin.y + rect.size.height);
    const auto bottom = height - rect.origin.y;
    std::size_t best = monitors_.size();
    double best_area = 0;
    for (std::size_t index = 0; index < monitors_.size(); ++index) {
        const auto& monitor = monitors_[index];
        const auto width = std::min(rect.origin.x + rect.size.width, monitor.x + monitor.width) -
                           std::max(rect.origin.x, monitor.x);
        const auto overlap = std::min(bottom, monitor.y + monitor.height) - std::max(top, monitor.y);
        if (width > 0 && overlap > 0 && width * overlap > best_area) {
            best = index;
            best_area = width * overlap;
        }
    }
    if (best < monitors_.size()) return best;
    return monitor_for_point({rect.origin.x + rect.size.width / 2, rect.origin.y + rect.size.height / 2});
}

std::size_t ScreenLayout::monitor_for_handle(std::uintptr_t handle) const noexcept {
    for (std::size_t index = 0; index < monitors_.size(); ++index)
        if (monitors_[index].pixels.handle == handle) return index;
    return monitors_.size();
}

Point ScreenLayout::to_points(PixelPoint pixel) const { return to_points(pixel, monitor_for_pixel(pixel)); }

Point ScreenLayout::to_points(PixelPoint pixel, std::size_t index) const {
    const auto& monitor = at(index);
    const auto scale = monitor.pixels.scale;
    const auto x = monitor.x + (pixel.x - monitor.pixels.bounds.left) / scale;
    const auto y = monitor.y + (pixel.y - monitor.pixels.bounds.top) / scale;
    return {x, primary_height() - y};
}

PixelPoint ScreenLayout::to_pixels(Point point) const { return to_pixels(point, monitor_for_point(point)); }

PixelPoint ScreenLayout::to_pixels(Point point, std::size_t index) const {
    const auto& monitor = at(index);
    const auto scale = monitor.pixels.scale;
    const auto top_down = primary_height() - point.y;
    return {monitor.pixels.bounds.left + (point.x - monitor.x) * scale,
            monitor.pixels.bounds.top + (top_down - monitor.y) * scale};
}

Rect ScreenLayout::to_points(const PixelRect& rect, std::size_t monitor, double scale) const {
    if (!(scale > 0)) scale = at(monitor).pixels.scale;
    const auto top_left = to_points(PixelPoint{static_cast<double>(rect.left), static_cast<double>(rect.top)}, monitor);
    const Size size{rect.width() / scale, rect.height() / scale};
    return {{top_left.x, top_left.y - size.height}, size};
}

PixelRect ScreenLayout::to_pixels(const Rect& rect, std::size_t monitor, double scale) const {
    if (!(scale > 0)) scale = at(monitor).pixels.scale;
    const auto top_left = to_pixels(Point{rect.origin.x, rect.origin.y + rect.size.height}, monitor);
    PixelRect pixels;
    pixels.left = round_pixel(top_left.x);
    pixels.top = round_pixel(top_left.y);
    pixels.right = pixels.left + round_pixel(rect.size.width * scale);
    pixels.bottom = pixels.top + round_pixel(rect.size.height * scale);
    return pixels;
}

Rect ScreenLayout::frame(std::size_t index) const {
    const auto& monitor = at(index);
    return {{monitor.x, primary_height() - (monitor.y + monitor.height)}, {monitor.width, monitor.height}};
}

Rect ScreenLayout::visible_frame(std::size_t index) const {
    const auto& monitor = at(index);
    return to_points(monitor.pixels.work, index, monitor.pixels.scale);
}

} // namespace alchemy::gui::win32
