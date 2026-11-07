#include "alchemy/darwin/windowserver/presented_pixels.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace alchemy::gui::presented {
namespace {

std::mutex& mutex() {
    static std::mutex value;
    return value;
}
std::map<WindowId, Pixels>& store() {
    static std::map<WindowId, Pixels> value;
    return value;
}

} // namespace

void record(const WindowServer& server, WindowId window, const WindowServer::BitmapView& pixels, std::optional<Rect> content_rect) {
    if (!window || !pixels.bgra || !pixels.width || !pixels.height || pixels.stride < static_cast<std::size_t>(pixels.width) * 4) return;
    // The part of the bitmap that was shown, in its pixels (rows top first).
    long x0 = 0, y0 = 0, x1 = pixels.width, y1 = pixels.height;
    if (content_rect) {
        const auto content = server.content_rect(window).size;
        if (!(content.width > 0) || !(content.height > 0)) return;
        const double sx = pixels.width / content.width, sy = pixels.height / content.height;
        x0 = std::max(0L, static_cast<long>(std::floor(content_rect->origin.x * sx)));
        x1 = std::min<long>(pixels.width, static_cast<long>(std::ceil((content_rect->origin.x + content_rect->size.width) * sx)));
        y0 = std::max(0L, static_cast<long>(std::floor((content.height - content_rect->origin.y - content_rect->size.height) * sy)));
        y1 = std::min<long>(pixels.height, static_cast<long>(std::ceil((content.height - content_rect->origin.y) * sy)));
        if (x1 <= x0 || y1 <= y0) return;
    }
    std::lock_guard lock(mutex());
    auto& kept = store()[window];
    if (kept.width != pixels.width || kept.height != pixels.height) {
        kept.width = pixels.width;
        kept.height = pixels.height;
        kept.bgra.assign(static_cast<std::size_t>(pixels.width) * pixels.height * 4, 0);
    }
    const auto row_bytes = static_cast<std::size_t>(x1 - x0) * 4;
    for (long y = y0; y < y1; ++y)
        std::memcpy(kept.bgra.data() + (static_cast<std::size_t>(y) * pixels.width + x0) * 4,
                    pixels.bgra + static_cast<std::size_t>(y) * pixels.stride + static_cast<std::size_t>(x0) * 4, row_bytes);
}

std::optional<Pixels> snapshot(WindowId window) {
    std::lock_guard lock(mutex());
    const auto found = store().find(window);
    if (found == store().end()) return std::nullopt;
    return found->second;
}

void forget(WindowId window) {
    std::lock_guard lock(mutex());
    store().erase(window);
}

} // namespace alchemy::gui::presented
