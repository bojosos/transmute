#pragma once

// Drawn-text notes for diagnostics (owner: AppKit autopress,
// src/alchemy/darwin/appkit/autopress.cpp).
//
// Text drawing primitives (CoreText CTLineDraw/CTFrameDraw/CTRunDraw, the
// AppKit host rasterizer's draw_text) report the text they draw. While the
// AppKit display pass sends -drawRect: to a view, AppKit records the reported
// strings as that view's drawn labels, so the diagnostic
// ALCHEMY_GUI_AUTOPRESS hook can find custom-drawn controls (wxWidgets
// wxNSView buttons and checkboxes) by their visible label. When no sink is
// installed (the hook is off) reporting costs one relaxed atomic load.
//
// Threading: any thread may report; the sink ignores reports made outside a
// display pass on the main thread.

#include <string_view>

namespace alchemy::gui {

using DrawnTextSink = void (*)(std::u16string_view text);

void set_drawn_text_sink(DrawnTextSink sink) noexcept;
void report_drawn_text(std::u16string_view text) noexcept;

} // namespace alchemy::gui
