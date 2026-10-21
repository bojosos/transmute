// Win32 host backend, area "gpu": system text rasterizing for AppKit's
// software text drawing (include/alchemy/host/gpu.hpp). GDI fonts and one
// memory DC, shared by every thread under one mutex: metrics, extents and
// grayscale coverage of a line drawn into a DIB section.
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

#pragma comment(lib, "gdi32.lib")

namespace alchemy::host::gpu {
namespace {

struct Gdi {
    std::mutex mutex;
    HDC dc{};
    std::map<std::tuple<std::wstring, int, bool, bool>, HFONT> fonts;
    Gdi() { dc = CreateCompatibleDC(nullptr); }
    ~Gdi() {
        for (auto& [key, font] : fonts) DeleteObject(font);
        if (dc) DeleteDC(dc);
    }
    HFONT font(const RasterFont& spec) {
        const auto quarters = std::max(4, spec.quarter_pixels);
        const auto key = std::make_tuple(host::win32::widen(spec.family), quarters, spec.bold, spec.italic);
        if (const auto found = fonts.find(key); found != fonts.end()) return found->second;
        LOGFONTW description{};
        description.lfHeight = -std::max(1, static_cast<int>(std::lround(quarters / 4.0)));
        description.lfWeight = spec.bold ? FW_BOLD : FW_NORMAL;
        description.lfItalic = spec.italic ? TRUE : FALSE;
        description.lfCharSet = DEFAULT_CHARSET;
        description.lfOutPrecision = OUT_TT_PRECIS;
        description.lfQuality = ANTIALIASED_QUALITY;
        description.lfPitchAndFamily = DEFAULT_PITCH;
        wcsncpy_s(description.lfFaceName, std::get<0>(key).c_str(), _TRUNCATE);
        const auto created = CreateFontIndirectW(&description);
        if (!created) return nullptr;
        fonts.emplace(key, created);
        return created;
    }
};
Gdi& gdi() {
    static Gdi instance;
    return instance;
}

// Selects `font` into the shared DC for the scope (the mutex is held by the caller).
struct Selected {
    HDC dc;
    HGDIOBJ old;
    Selected(HDC context, HFONT font) : dc(context), old(SelectObject(context, font)) {}
    ~Selected() { SelectObject(dc, old); }
};

} // namespace

Result<RasterMetrics> raster_metrics(const RasterFont& font) {
    auto& g = gdi();
    std::lock_guard lock(g.mutex);
    const auto handle = g.font(font);
    if (!g.dc || !handle) return Error::other;   // CreateCompatibleDC or CreateFontIndirect failed
    const Selected selected(g.dc, handle);
    TEXTMETRICW metrics{};
    GetTextMetricsW(g.dc, &metrics);
    OUTLINETEXTMETRICW outline{};
    outline.otmSize = sizeof(outline);
    const bool have_outline = GetOutlineTextMetricsW(g.dc, sizeof(outline), &outline) != 0;
    RasterMetrics result;
    result.ascent = metrics.tmAscent;
    result.descent = metrics.tmDescent;
    result.leading = metrics.tmExternalLeading;
    result.average_width = metrics.tmAveCharWidth;
    result.max_width = metrics.tmMaxCharWidth;
    if (have_outline && outline.otmsCapEmHeight) result.cap_height = outline.otmsCapEmHeight;
    if (have_outline && outline.otmsXHeight) result.x_height = outline.otmsXHeight;
    return result;
}

Result<double> raster_width(const RasterFont& font, std::u16string_view text) {
    if (text.empty()) return 0.0;
    auto& g = gdi();
    std::lock_guard lock(g.mutex);
    const auto handle = g.font(font);
    if (!g.dc || !handle) return Error::other;   // CreateCompatibleDC or CreateFontIndirect failed
    const Selected selected(g.dc, handle);
    SIZE size{};
    GetTextExtentPoint32W(g.dc, reinterpret_cast<const wchar_t*>(text.data()), static_cast<int>(text.size()), &size);
    return static_cast<double>(size.cx);
}

Result<std::vector<double>> raster_prefix_widths(const RasterFont& font, std::u16string_view text) {
    std::vector<double> result(text.size());
    if (text.empty()) return result;
    std::vector<INT> extents(text.size());
    auto& g = gdi();
    std::lock_guard lock(g.mutex);
    const auto handle = g.font(font);
    if (!g.dc || !handle) return Error::other;   // CreateCompatibleDC or CreateFontIndirect failed
    const Selected selected(g.dc, handle);
    SIZE size{};
    // No fit count: every partial extent is filled (the maximum extent is ignored).
    GetTextExtentExPointW(g.dc, reinterpret_cast<const wchar_t*>(text.data()), static_cast<int>(text.size()), 0, nullptr, extents.data(),
                          &size);
    for (std::size_t i = 0; i < text.size(); ++i) result[i] = extents[i];
    return result;
}

Result<std::vector<float>> raster_coverage(const RasterFont& font, std::u16string_view text, int width, int height, int x, int baseline) {
    if (width <= 0 || height <= 0) return Error::invalid_argument;
    std::vector<float> coverage(static_cast<std::size_t>(width) * height);
    auto& g = gdi();
    std::lock_guard lock(g.mutex);
    const auto handle = g.font(font);
    if (!g.dc || !handle) return Error::other;   // CreateCompatibleDC or CreateFontIndirect failed
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    const auto bitmap = CreateDIBSection(g.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        return Error::no_memory;
    }
    std::memset(bits, 0, static_cast<std::size_t>(width) * height * 4);
    const auto old_bitmap = SelectObject(g.dc, bitmap);
    const auto old_font = SelectObject(g.dc, handle);
    SetTextColor(g.dc, RGB(255, 255, 255));
    SetBkMode(g.dc, TRANSPARENT);
    SetTextAlign(g.dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
    TextOutW(g.dc, x, baseline, reinterpret_cast<const wchar_t*>(text.data()), static_cast<int>(text.size()));
    GdiFlush();
    const auto* source = static_cast<const std::uint8_t*>(bits);
    for (std::size_t i = 0; i < coverage.size(); ++i) {
        const auto b = source[i * 4], gch = source[i * 4 + 1], r = source[i * 4 + 2];
        coverage[i] = std::max({r, gch, b}) / 255.0f;
    }
    SelectObject(g.dc, old_font);
    SelectObject(g.dc, old_bitmap);
    DeleteObject(bitmap);
    return coverage;
}

} // namespace alchemy::host::gpu
