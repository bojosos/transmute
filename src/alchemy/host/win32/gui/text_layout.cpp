// Win32 host backend, area "gpu": text layout over DirectWrite
// (include/alchemy/host/gpu.hpp). An IDWriteTextLayout does the shaping,
// font fallback, bidi and line breaking; a recording renderer collects the
// glyph runs it would draw, each tagged with the span that styled it.
#include "alchemy/host/win32/gui/graphics.hpp"

#include <algorithm>

namespace alchemy::host::gpu {
namespace {

using win32::ComPtr;

// The drawing effect that carries a span's tag through the layout.
class SpanTag final : public IUnknown {
public:
    explicit SpanTag(std::size_t tag) : tag_(tag) {}
    STDMETHOD_(ULONG, AddRef)() override { return ++references_; }
    STDMETHOD_(ULONG, Release)() override {
        const auto value = --references_;
        if (value == 0) delete this;
        return value;
    }
    STDMETHOD(QueryInterface)(REFIID id, void** object) override {
        if (id == __uuidof(IUnknown)) { *object = this; AddRef(); return S_OK; }
        if (id == effect_id) { *object = this; AddRef(); return S_OK; }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    [[nodiscard]] std::size_t tag() const { return tag_; }
    static constexpr GUID effect_id{0x6b3c5c1e, 0x1d7a, 0x4f43, {0x9a, 0x2d, 0x51, 0x0e, 0x6a, 0x0d, 0x22, 0x7f}};

private:
    std::size_t tag_;
    ULONG references_{1};
};

class Collector final : public IDWriteTextRenderer {
public:
    std::vector<LaidOutRun> runs;
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(QueryInterface)(REFIID id, void** object) override {
        if (id == __uuidof(IDWriteTextRenderer) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IUnknown)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD(IsPixelSnappingDisabled)(void*, BOOL* disabled) override { *disabled = TRUE; return S_OK; }
    STDMETHOD(GetCurrentTransform)(void*, DWRITE_MATRIX* transform) override { *transform = {1, 0, 0, 1, 0, 0}; return S_OK; }
    STDMETHOD(GetPixelsPerDip)(void*, FLOAT* pixels) override { *pixels = 1; return S_OK; }
    STDMETHOD(DrawGlyphRun)(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                            const DWRITE_GLYPH_RUN_DESCRIPTION* description, IUnknown* effect) override {
        LaidOutRun recorded;
        ComPtr<IDWriteFontFace3> face;
        if (run->fontFace) static_cast<void>(run->fontFace->QueryInterface(IID_PPV_ARGS(&face)));
        recorded.font = win32::wrap(std::move(face));
        recorded.size = run->fontEmSize;
        recorded.glyphs.assign(run->glyphIndices, run->glyphIndices + run->glyphCount);
        recorded.advances.assign(run->glyphCount, 0.0);
        if (run->glyphAdvances)
            for (UINT32 index = 0; index < run->glyphCount; ++index) recorded.advances[index] = run->glyphAdvances[index];
        recorded.offsets.assign(run->glyphCount, Point{});
        if (run->glyphOffsets)
            for (UINT32 index = 0; index < run->glyphCount; ++index)
                recorded.offsets[index] = {run->glyphOffsets[index].advanceOffset, run->glyphOffsets[index].ascenderOffset};
        recorded.rtl = (run->bidiLevel & 1) != 0;
        recorded.x = x;
        recorded.y = y;
        if (description) {
            recorded.text_position = description->textPosition;
            recorded.text_length = description->stringLength;
            if (description->clusterMap) recorded.clusters.assign(description->clusterMap, description->clusterMap + description->stringLength);
        }
        ComPtr<IUnknown> token;
        if (effect && SUCCEEDED(effect->QueryInterface(SpanTag::effect_id, reinterpret_cast<void**>(token.GetAddressOf()))))
            recorded.tag = static_cast<SpanTag*>(token.Get())->tag();
        runs.push_back(std::move(recorded));
        return S_OK;
    }
    STDMETHOD(DrawUnderline)(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    STDMETHOD(DrawStrikethrough)(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    STDMETHOD(DrawInlineObject)(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return E_NOTIMPL; }
};

} // namespace

Result<TextLayout> layout_text(const TextLayoutRequest& request) {
    auto* factory = win32::dwrite_factory();
    if (!factory) return Error::unsupported;
    const auto& base = request.base;
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(host::win32::widen(base.family).c_str(), win32::native(base.collection),
                                         static_cast<DWRITE_FONT_WEIGHT>(base.weight), static_cast<DWRITE_FONT_STYLE>(base.style),
                                         static_cast<DWRITE_FONT_STRETCH>(base.stretch), static_cast<FLOAT>(base.size), L"en-us", &format)))
        return Error::invalid_argument;
    format->SetWordWrapping(request.wrapping == Wrapping::none        ? DWRITE_WORD_WRAPPING_NO_WRAP
                            : request.wrapping == Wrapping::character ? DWRITE_WORD_WRAPPING_CHARACTER
                                                                      : DWRITE_WORD_WRAPPING_WRAP);
    format->SetReadingDirection(request.rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
    format->SetTextAlignment(request.justified ? DWRITE_TEXT_ALIGNMENT_JUSTIFIED : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetIncrementalTabStop(static_cast<FLOAT>(request.tab_interval));
    const std::wstring text(request.text.begin(), request.text.end());
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), static_cast<FLOAT>(request.width),
                                         static_cast<FLOAT>(request.height), &layout)))
        return Error::invalid_argument;
    ComPtr<IDWriteTextLayout1> layout1;
    static_cast<void>(layout.As(&layout1));
    for (const auto& span : request.spans) {
        const DWRITE_TEXT_RANGE target{static_cast<UINT32>(span.start), static_cast<UINT32>(span.length)};
        layout->SetFontCollection(win32::native(span.collection), target);
        layout->SetFontFamilyName(host::win32::widen(span.family).c_str(), target);
        layout->SetFontWeight(static_cast<DWRITE_FONT_WEIGHT>(span.weight), target);
        layout->SetFontStyle(static_cast<DWRITE_FONT_STYLE>(span.style), target);
        layout->SetFontStretch(static_cast<DWRITE_FONT_STRETCH>(span.stretch), target);
        layout->SetFontSize(static_cast<FLOAT>(span.size), target);
        if (span.character_spacing != 0 && layout1) layout1->SetCharacterSpacing(0, static_cast<FLOAT>(span.character_spacing), 0, target);
        auto* effect = new SpanTag(span.tag);
        layout->SetDrawingEffect(effect, target);
        effect->Release();
    }
    Collector collector;
    if (FAILED(layout->Draw(nullptr, &collector, 0, 0))) return Error::io;
    UINT32 line_count{};
    static_cast<void>(layout->GetLineMetrics(nullptr, 0, &line_count));
    std::vector<DWRITE_LINE_METRICS> metrics(line_count);
    if (line_count && FAILED(layout->GetLineMetrics(metrics.data(), line_count, &line_count))) return Error::io;
    TextLayout result;
    result.runs = std::move(collector.runs);
    for (std::uint32_t index = 0; index < line_count; ++index) result.lines.push_back({metrics[index].length, metrics[index].newlineLength});
    return result;
}

} // namespace alchemy::host::gpu
