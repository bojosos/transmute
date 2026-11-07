#pragma once

// What this process presented in its own windows: a copy of the pixels each
// WindowServer::present_bitmap call showed, kept per window so that screen
// capture of the process's own content (ScreenCaptureKit, CGWindowList
// images: darwin/screencapturekit/) reads pixels the process drew itself and
// nothing else. The host never gives pixels back: no other window, no other
// process and no host desktop can reach this store.
// * AppKit's software drawing (appkit/graphics.cpp) and the CALayer tree
//   composition (quartzcore/layer_contents.cpp) record each present right
//   after making it, with the same arguments.
// * Content that presents itself (CAMetalLayer views through Vulkan) is not
//   recorded: those areas keep what was last presented there in software
//   (transparent if nothing was).
// * The title bar and frame are the host's and are never recorded: the
//   pixels cover the content area only.

#include "alchemy/gui/window_server.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace alchemy::gui::presented {

// Records `pixels` (covering the whole content area of `window`) in the part
// `content_rect` (content points, bottom-left origin; nullopt: all of it) as
// WindowServer::present_bitmap shows them. Main thread.
void record(const WindowServer& server, WindowId window, const WindowServer::BitmapView& pixels, std::optional<Rect> content_rect);

// The content area as last presented (premultiplied BGRA rows, top first), or
// nullopt when nothing was presented in `window` (or AppKit destroyed it).
struct Pixels {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> bgra;
};
[[nodiscard]] std::optional<Pixels> snapshot(WindowId window);

// Drops a destroyed window's copy (AppKit, when it destroys the window-server window).
void forget(WindowId window);

} // namespace alchemy::gui::presented
