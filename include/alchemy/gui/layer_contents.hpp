#pragma once

// CALayer tree compositing (owner: w3 compositing, src/alchemy/darwin/
// quartzcore/layer_contents.cpp and layer_tree.hpp) <-> AppKit.
//
// The tree of a view's backing layer is composited into the view's window
// when it shows contents somewhere: an IOSurface or a CGImage set as
// -[CALayer contents] on the backing layer itself (Qt 6's QNSView, which
// draws through -updateLayer / the CALayerDelegate -displayLayer:) or on any
// sublayer (Chrome's DisplayCALayerTree puts its software compositor's
// IOSurface on a sublayer). The composition (sublayers, frames,
// contentsScale, contentsGravity, geometryFlipped, opacity, backgroundColor,
// hidden, masksToBounds) is shown with WindowServer::present_bitmap when the
// outermost CATransaction commits on the main thread, or at the next main
// run-loop step for changes outside an explicit transaction and changes made
// on other threads, like the commits of Core Animation's explicit and
// implicit transactions. Contents changed in place (an IOSurface drawn into
// again, then -[CALayer setContentsChanged]) are shown again the same way.
//
// AppKit treats a view whose -wantsUpdateLayer answers YES, or that
// implements -displayLayer:, like a layer-hosting one: it leaves the view's
// rectangle out of the window backing store's presentation and, in its
// display pass, calls update_view_layer (which sends -updateLayer, or
// -displayLayer: with the view's layer) instead of -drawRect:. AppKit calls
// view_layer_displayed after -updateLayer so contents that did not change
// during the update are shown again after an expose.
//
// Threading: main thread, except as noted.
//
// ALCHEMY_CONTENTS_CAPTURE=<directory>: every window that shows composited
// layer trees is written as <directory>/window-<windowNumber>.bmp (top-down
// 32-bit BMP of the window's composited content area, premultiplied BGRA,
// rewritten atomically for the first three frames, then at most every 250 ms
// (a frame held back is written when its 250 ms are up), and at exit)
// together with window-<windowNumber>.txt ("frames <n> width <w> height
// <h>"). For harnesses: pixels come from the composition, never from the
// screen, so cloaked windows are captured too.
// ALCHEMY_TRACE_PRESENT=1 also logs each window's first compositions and
// every 300th as "[alchemy layer-tree] window <n> frame <k>: ...".

#include "alchemy/objc_runtime.hpp"

#include <cstdint>

namespace alchemy::gui {
class WindowServer;
}

namespace alchemy::gui::layer_contents {

// True when the view's layer contents are shown by this compositor:
// wantsLayer is YES and -wantsUpdateLayer answers YES, the view responds to
// -displayLayer:, or its layer tree is composited now (it shows contents on
// a sublayer). AppKit then neither draws the view with -drawRect: (on macOS
// that drawing would be the backing layer's own contents, under the
// sublayers; it is not shown) nor presents its window backing store over
// the view's rectangle.
[[nodiscard]] bool view_updates_layer(ObjcRuntime& objc, std::uint64_t view);

// AppKit's display of such a view: -updateLayer when -wantsUpdateLayer is
// YES, else -displayLayer: with the view's (created) layer when it responds;
// then view_layer_displayed.
void update_view_layer(ObjcRuntime& objc, std::uint64_t view);

// After AppKit sent -updateLayer to `view`: composites and shows the view's
// layer tree now.
void view_layer_displayed(ObjcRuntime& objc, std::uint64_t view);

// The window server the compositor presents to and CVDisplayLink takes its
// displays from: the override, else the process's window_server(), else
// nullptr. Any thread.
[[nodiscard]] WindowServer* presentation_server() noexcept;
// Tests: replaces the process's window server for presentation_server()
// (nullptr: no override). Any thread.
void set_window_server_override(WindowServer* server) noexcept;
// Tests and diagnostics: the number of window compositions so far.
[[nodiscard]] std::uint64_t compositions() noexcept;

} // namespace alchemy::gui::layer_contents
