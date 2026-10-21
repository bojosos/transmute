#include "alchemy/host/win32/gui/window_server.hpp"

#include <algorithm>
#include <cmath>
#include <string>

// Software presentation of AppKit window backing stores (non-layer-backed
// NSView drawing): a GDI blit of the BGRA pixels into the content HWND. The
// window class paints nothing itself; WM_PAINT becomes an expose
// notification, AppKit redraws the dirty views and presents again, so no copy
// of the pixels is kept here.
namespace alchemy::gui::win32 {

void Win32WindowServer::present_bitmap(WindowId window, const BitmapView& pixels, std::optional<Rect> content) {
    require_main_thread("present_bitmap");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto target = record(window, "present_bitmap");
    if (!pixels.bgra || pixels.width == 0 || pixels.height == 0) return;
    if (pixels.stride < static_cast<std::size_t>(pixels.width) * 4 || pixels.stride % 4 != 0)
        fail("present_bitmap: the row stride must be a multiple of 4 covering the width");
    if (IsIconic(target->hwnd)) return;
    RECT client{};
    if (!GetClientRect(target->hwnd, &client)) fail_win32("present_bitmap: GetClientRect");
    if (client.right <= 0 || client.bottom <= 0) return;
    const auto scale = window_scale(target->hwnd);

    // The update area in client pixels.
    RECT area = client;
    if (content) {
        const RECT dirty{static_cast<LONG>(std::floor(content->origin.x * scale)),
                         static_cast<LONG>(std::floor(client.bottom - (content->origin.y + content->size.height) * scale)),
                         static_cast<LONG>(std::ceil((content->origin.x + content->size.width) * scale)),
                         static_cast<LONG>(std::ceil(client.bottom - content->origin.y * scale))};
        if (!IntersectRect(&area, &client, &dirty)) return;
    }

    const auto dc = GetDC(target->hwnd);
    if (!dc) fail_win32("present_bitmap: GetDC");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(pixels.stride / 4);
    info.bmiHeader.biHeight = -static_cast<LONG>(pixels.height);   // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    // The whole bitmap over the whole content area (1:1 unless the window is
    // being resized), clipped to the update area: whole-image source
    // rectangles avoid the bottom-up source origin rules of GDI.
    const auto saved = SaveDC(dc);
    IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
    const bool same_size = static_cast<LONG>(pixels.width) == client.right && static_cast<LONG>(pixels.height) == client.bottom;
    SetStretchBltMode(dc, same_size ? COLORONCOLOR : HALFTONE);
    if (!same_size) SetBrushOrgEx(dc, 0, 0, nullptr);
    const int copied = StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, static_cast<int>(pixels.width),
                                     static_cast<int>(pixels.height), pixels.bgra, &info, DIB_RGB_COLORS, SRCCOPY);
    RestoreDC(dc, saved);
    ReleaseDC(target->hwnd, dc);
    if (copied == 0) fail_win32("present_bitmap: GDI blit");
    trace("present_bitmap %llu %ux%u -> [%ld,%ld,%ld,%ld]", static_cast<unsigned long long>(window), pixels.width,
          pixels.height, area.left, area.top, area.right, area.bottom);
}

} // namespace alchemy::gui::win32
