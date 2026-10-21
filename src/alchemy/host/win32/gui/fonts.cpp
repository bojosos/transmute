// Win32 host backend, area "gpu": fonts over DirectWrite (include/alchemy/host/gpu.hpp).
// System and registered font collections and font sets, family matching,
// name lookups, face names and metrics, glyphs, glyph metrics and outlines,
// OpenType tables, in-memory font files and the system font fallback.
#include "alchemy/host/win32/gui/graphics.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace alchemy::host::gpu {

struct FontCollection::Impl {
    win32::ComPtr<IDWriteFontCollection> collection;
    win32::ComPtr<IDWriteFontSet> set;
};
struct Font::Impl {
    win32::ComPtr<IDWriteFontFace3> face;
};

namespace win32 {

IDWriteFontFace3* native(const Font& font) noexcept { return font ? font.impl()->face.Get() : nullptr; }
Font wrap(ComPtr<IDWriteFontFace3> face) {
    if (!face) return {};
    auto impl = std::make_shared<Font::Impl>();
    impl->face = std::move(face);
    return Font(std::move(impl));
}
IDWriteFontCollection* native(const FontCollection& collection) noexcept {
    return collection ? collection.impl()->collection.Get() : nullptr;
}

} // namespace win32

namespace {

using win32::ComPtr;

std::string localized(IDWriteLocalizedStrings* strings) {
    if (!strings || strings->GetCount() == 0) return {};
    UINT32 index{};
    BOOL exists{};
    if (FAILED(strings->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
    UINT32 length{};
    if (FAILED(strings->GetStringLength(index, &length))) return {};
    std::wstring text(length + 1, L' ');
    if (FAILED(strings->GetString(index, text.data(), length + 1))) return {};
    text.resize(length);
    return host::win32::narrow(text);
}

Font face_of(IDWriteFont* font) {
    ComPtr<IDWriteFontFace> face;
    ComPtr<IDWriteFontFace3> face3;
    if (!font || FAILED(font->CreateFontFace(&face)) || FAILED(face.As(&face3))) return {};
    return win32::wrap(std::move(face3));
}

// Registered fonts (register_font_file): one collection rebuilt per registration.
std::mutex registry_mutex;
ComPtr<IDWriteInMemoryFontFileLoader> memory_loader;
ComPtr<IDWriteFontSetBuilder1> registered_builder;
FontCollection registered;

ComPtr<IDWriteFactory5> factory5() {
    ComPtr<IDWriteFactory5> factory;
    auto* base = win32::dwrite_factory();
    if (!base || FAILED(base->QueryInterface(IID_PPV_ARGS(&factory)))) return nullptr;
    return factory;
}

} // namespace

bool Font::same_face(const Font& other) const {
    auto* a = win32::native(*this);
    auto* b = win32::native(other);
    if (!a || !b) return false;
    if (a == b) return true;
    ComPtr<IDWriteFontFaceReference> ra, rb;
    return SUCCEEDED(a->GetFontFaceReference(&ra)) && SUCCEEDED(b->GetFontFaceReference(&rb)) && ra->Equals(rb.Get());
}

Result<FontInfo> Font::info() const {
    auto* face = win32::native(*this);
    if (!face) return Error::bad_handle;
    FontInfo result;
    ComPtr<IDWriteLocalizedStrings> strings;
    if (SUCCEEDED(face->GetFamilyNames(&strings))) result.family = localized(strings.Get());
    strings.Reset();
    if (SUCCEEDED(face->GetFaceNames(&strings))) result.style_name = localized(strings.Get());
    BOOL exists{};
    strings.Reset();
    if (SUCCEEDED(face->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &strings, &exists)) && exists)
        result.postscript = localized(strings.Get());
    strings.Reset();
    if (SUCCEEDED(face->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_FULL_NAME, &strings, &exists)) && exists)
        result.full_name = localized(strings.Get());
    result.weight = face->GetWeight();
    result.style = face->GetStyle();
    result.stretch = face->GetStretch();
    DWRITE_FONT_METRICS1 metrics{};
    face->GetMetrics(&metrics);
    auto& m = result.metrics;
    m.units_per_em = metrics.designUnitsPerEm;
    m.ascent = metrics.ascent;
    m.descent = metrics.descent;
    m.leading = metrics.lineGap;
    m.cap_height = metrics.capHeight;
    m.x_height = metrics.xHeight;
    m.underline_position = metrics.underlinePosition;
    m.underline_thickness = metrics.underlineThickness;
    m.box_left = metrics.glyphBoxLeft;
    m.box_bottom = metrics.glyphBoxBottom;
    m.box_width = static_cast<double>(metrics.glyphBoxRight - metrics.glyphBoxLeft);
    m.box_height = static_cast<double>(metrics.glyphBoxTop - metrics.glyphBoxBottom);
    // 'post' table italic angle (16.16 fixed, offset 4).
    const void* data{};
    UINT32 size{};
    void* context{};
    if (SUCCEEDED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('p', 'o', 's', 't'), &data, &size, &context, &exists)) && exists) {
        if (size >= 8) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            const auto fixed = static_cast<std::int32_t>(static_cast<std::uint32_t>(bytes[4]) << 24 | static_cast<std::uint32_t>(bytes[5]) << 16 |
                                                         static_cast<std::uint32_t>(bytes[6]) << 8 | bytes[7]);
            m.slant = fixed / 65536.0;
        }
        face->ReleaseFontTable(context);
    }
    result.monospace = face->IsMonospacedFont() != FALSE;
    result.symbol = face->IsSymbolFont() != FALSE;
    result.color = face->IsColorFont() != FALSE;
    return result;
}

std::uint32_t Font::glyph_count() const {
    auto* face = win32::native(*this);
    return face ? face->GetGlyphCount() : 0;
}

Result<std::vector<std::uint16_t>> Font::glyphs(std::span<const std::uint32_t> code_points) const {
    auto* face = win32::native(*this);
    if (!face) return Error::bad_handle;
    std::vector<std::uint16_t> indices(code_points.size());
    if (!code_points.empty() &&
        FAILED(face->GetGlyphIndices(code_points.data(), static_cast<UINT32>(code_points.size()), indices.data())))
        return Error::invalid_argument;
    return indices;
}

Result<std::vector<GlyphMetrics>> Font::glyph_metrics(std::span<const std::uint16_t> glyphs) const {
    auto* face = win32::native(*this);
    if (!face) return Error::bad_handle;
    std::vector<DWRITE_GLYPH_METRICS> metrics(glyphs.size());
    if (!glyphs.empty() && FAILED(face->GetDesignGlyphMetrics(glyphs.data(), static_cast<UINT32>(glyphs.size()), metrics.data(), FALSE)))
        return Error::invalid_argument;
    std::vector<GlyphMetrics> result(glyphs.size());
    for (std::size_t index = 0; index < glyphs.size(); ++index) {
        const auto& m = metrics[index];
        result[index] = {m.leftSideBearing, m.rightSideBearing, m.topSideBearing, m.bottomSideBearing, m.verticalOriginY,
                         m.advanceWidth, m.advanceHeight};
    }
    return result;
}

Result<std::vector<PathElement>> Font::outlines(double size, std::span<const std::uint16_t> glyphs, std::span<const Point> offsets,
                                                 const Matrix& transform) const {
    auto* face = win32::native(*this);
    if (!face) return Error::bad_handle;
    std::vector<PathElement> path;
    if (glyphs.empty()) return path;
    std::vector<FLOAT> advances(glyphs.size(), 0.0F);
    std::vector<DWRITE_GLYPH_OFFSET> placed(glyphs.size());
    for (std::size_t index = 0; index < placed.size(); ++index) {
        const auto p = index < offsets.size() ? offsets[index] : Point{};
        placed[index] = {static_cast<FLOAT>(p.x), static_cast<FLOAT>(p.y)};
    }
    win32::PathSink sink(path, transform);
    if (FAILED(face->GetGlyphRunOutline(static_cast<FLOAT>(size), glyphs.data(), advances.data(), placed.data(),
                                        static_cast<UINT32>(glyphs.size()), FALSE, FALSE, &sink)))
        return Error::io;
    return path;
}

Result<std::vector<std::uint8_t>> Font::table(std::uint32_t tag) const {
    auto* face = win32::native(*this);
    if (!face) return Error::bad_handle;
    const void* data{};
    UINT32 size{};
    void* context{};
    BOOL exists{};
    const auto dwrite_tag = DWRITE_MAKE_OPENTYPE_TAG(tag >> 24, (tag >> 16) & 0xff, (tag >> 8) & 0xff, tag & 0xff);
    if (FAILED(face->TryGetFontTable(dwrite_tag, &data, &size, &context, &exists)) || !exists) return Error::not_found;
    std::vector<std::uint8_t> bytes(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
    face->ReleaseFontTable(context);
    return bytes;
}

Result<FontCollection> system_fonts() {
    static std::once_flag once;
    static FontCollection system;
    std::call_once(once, [] {
        auto* factory = win32::dwrite_factory();
        if (!factory) return;
        auto impl = std::make_shared<FontCollection::Impl>();
        if (FAILED(factory->GetSystemFontCollection(&impl->collection, FALSE)) || FAILED(factory->GetSystemFontSet(&impl->set))) return;
        system = FontCollection(std::move(impl));
    });
    if (!system) return Error::unsupported;
    return system;
}

FontCollection registered_fonts() {
    std::lock_guard lock(registry_mutex);
    return registered;
}

Result<Font> family_font(const FontCollection& collection, std::string_view family, int weight, int style, int stretch) {
    auto* fonts = win32::native(collection);
    if (!fonts) return Error::bad_handle;
    UINT32 index{};
    BOOL exists{};
    if (FAILED(fonts->FindFamilyName(host::win32::widen(family).c_str(), &index, &exists)) || !exists) return Error::not_found;
    ComPtr<IDWriteFontFamily> font_family;
    ComPtr<IDWriteFont> font;
    if (FAILED(fonts->GetFontFamily(index, &font_family)) ||
        FAILED(font_family->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(weight), static_cast<DWRITE_FONT_STRETCH>(stretch),
                                                 static_cast<DWRITE_FONT_STYLE>(style), &font)))
        return Error::not_found;
    auto result = face_of(font.Get());
    if (!result) return Error::io;
    return result;
}

Result<Font> named_font(const FontCollection& collection, FontName kind, std::string_view name) {
    if (!collection || !collection.impl()->set) return Error::bad_handle;
    const auto text = host::win32::widen(name);
    const DWRITE_FONT_PROPERTY query{kind == FontName::postscript ? DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME : DWRITE_FONT_PROPERTY_ID_FULL_NAME,
                                     text.c_str(), L""};
    ComPtr<IDWriteFontSet> matches;
    if (FAILED(collection.impl()->set->GetMatchingFonts(&query, 1, &matches)) || matches->GetFontCount() == 0) return Error::not_found;
    ComPtr<IDWriteFontFaceReference> reference;
    ComPtr<IDWriteFontFace3> face;
    if (FAILED(matches->GetFontFaceReference(0, &reference)) || FAILED(reference->CreateFontFace(&face))) return Error::io;
    return win32::wrap(std::move(face));
}

Result<std::vector<Font>> family_faces(const FontCollection& collection, std::string_view family) {
    auto* fonts = win32::native(collection);
    if (!fonts) return Error::bad_handle;
    UINT32 index{};
    BOOL exists{};
    if (FAILED(fonts->FindFamilyName(host::win32::widen(family).c_str(), &index, &exists)) || !exists) return Error::not_found;
    ComPtr<IDWriteFontFamily> font_family;
    if (FAILED(fonts->GetFontFamily(index, &font_family))) return Error::not_found;
    std::vector<Font> faces;
    for (UINT32 i = 0; i < font_family->GetFontCount(); ++i) {
        ComPtr<IDWriteFont> font;
        if (FAILED(font_family->GetFont(i, &font)) || font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) continue;
        if (auto face = face_of(font.Get())) faces.push_back(std::move(face));
    }
    return faces;
}

Result<std::vector<std::string>> family_names(const FontCollection& collection) {
    auto* fonts = win32::native(collection);
    if (!fonts) return Error::bad_handle;
    std::vector<std::string> names;
    for (UINT32 index = 0; index < fonts->GetFontFamilyCount(); ++index) {
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteLocalizedStrings> strings;
        if (SUCCEEDED(fonts->GetFontFamily(index, &family)) && SUCCEEDED(family->GetFamilyNames(&strings)))
            names.push_back(localized(strings.Get()));
    }
    return names;
}

Status register_font_file(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return Error::invalid_argument;
    auto factory = factory5();
    if (!factory) return Error::unsupported;
    std::lock_guard lock(registry_mutex);
    if (!memory_loader) {
        if (FAILED(factory->CreateInMemoryFontFileLoader(&memory_loader)) || FAILED(factory->RegisterFontFileLoader(memory_loader.Get())) ||
            FAILED(factory->CreateFontSetBuilder(&registered_builder))) {
            memory_loader.Reset();
            return Error::other;
        }
    }
    ComPtr<IDWriteFontFile> file;
    if (FAILED(memory_loader->CreateInMemoryFontFileReference(factory.Get(), bytes.data(), static_cast<UINT32>(bytes.size()), nullptr, &file)))
        return Error::io;
    BOOL supported{};
    DWRITE_FONT_FILE_TYPE file_type{};
    DWRITE_FONT_FACE_TYPE face_type{};
    UINT32 faces{};
    if (FAILED(file->Analyze(&supported, &file_type, &face_type, &faces)) || !supported || faces == 0) return Error::unsupported;
    if (FAILED(registered_builder->AddFontFile(file.Get()))) return Error::other;
    auto impl = std::make_shared<FontCollection::Impl>();
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(registered_builder->CreateFontSet(&impl->set)) || FAILED(factory->CreateFontCollectionFromFontSet(impl->set.Get(), &collection)))
        return Error::other;
    impl->collection = collection;
    registered = FontCollection(std::move(impl));
    return {};
}

Result<Font> font_from_data(std::span<const std::uint8_t> bytes) {
    auto factory = factory5();
    if (!factory) return Error::unsupported;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(factory->CreateInMemoryFontFileLoader(&loader)) || FAILED(factory->RegisterFontFileLoader(loader.Get()))) return Error::other;
    ComPtr<IDWriteFontFile> file;
    ComPtr<IDWriteFontFace> face;
    if (FAILED(loader->CreateInMemoryFontFileReference(factory.Get(), bytes.data(), static_cast<UINT32>(bytes.size()), nullptr, &file)))
        return Error::io;
    BOOL supported{};
    DWRITE_FONT_FILE_TYPE file_type{};
    DWRITE_FONT_FACE_TYPE face_type{};
    UINT32 faces{};
    if (FAILED(file->Analyze(&supported, &file_type, &face_type, &faces)) || !supported) return Error::unsupported;
    IDWriteFontFile* files[] = {file.Get()};
    if (FAILED(factory->CreateFontFace(face_type, 1, files, 0, DWRITE_FONT_SIMULATIONS_NONE, &face))) return Error::unsupported;
    ComPtr<IDWriteFontFace3> face3;
    if (FAILED(face.As(&face3))) return Error::unsupported;
    return win32::wrap(std::move(face3));
}

Result<Fallback> fallback_font(std::u16string_view text, std::string_view family, int weight, int style, int stretch) {
    ComPtr<IDWriteFactory2> factory2;
    ComPtr<IDWriteFontFallback> fallback;
    auto* base = win32::dwrite_factory();
    if (!base || FAILED(base->QueryInterface(IID_PPV_ARGS(&factory2))) || FAILED(factory2->GetSystemFontFallback(&fallback)))
        return Error::unsupported;
    struct Source final : IDWriteTextAnalysisSource {
        std::wstring text;
        STDMETHOD_(ULONG, AddRef)() override { return 1; }
        STDMETHOD_(ULONG, Release)() override { return 1; }
        STDMETHOD(QueryInterface)(REFIID id, void** object) override {
            if (id == __uuidof(IDWriteTextAnalysisSource) || id == __uuidof(IUnknown)) { *object = this; return S_OK; }
            *object = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHOD(GetTextAtPosition)(UINT32 position, const WCHAR** out, UINT32* length) override {
            if (position >= text.size()) { *out = nullptr; *length = 0; } else { *out = text.c_str() + position; *length = static_cast<UINT32>(text.size() - position); }
            return S_OK;
        }
        STDMETHOD(GetTextBeforePosition)(UINT32 position, const WCHAR** out, UINT32* length) override {
            if (position == 0 || position > text.size()) { *out = nullptr; *length = 0; } else { *out = text.c_str(); *length = position; }
            return S_OK;
        }
        STDMETHOD_(DWRITE_READING_DIRECTION, GetParagraphReadingDirection)() override { return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
        STDMETHOD(GetLocaleName)(UINT32, UINT32* length, const WCHAR** name) override { *name = L"en-us"; *length = static_cast<UINT32>(text.size()); return S_OK; }
        STDMETHOD(GetNumberSubstitution)(UINT32, UINT32* length, IDWriteNumberSubstitution** substitution) override {
            *substitution = nullptr; *length = static_cast<UINT32>(text.size()); return S_OK;
        }
    } analysis;
    analysis.text.assign(text.begin(), text.end());
    UINT32 mapped{};
    ComPtr<IDWriteFont> mapped_font;
    FLOAT scale{};
    if (FAILED(fallback->MapCharacters(&analysis, 0, static_cast<UINT32>(analysis.text.size()), nullptr, host::win32::widen(family).c_str(),
                                       static_cast<DWRITE_FONT_WEIGHT>(weight), static_cast<DWRITE_FONT_STYLE>(style),
                                       static_cast<DWRITE_FONT_STRETCH>(stretch), &mapped, &mapped_font, &scale)) ||
        !mapped_font)
        return Error::not_found;
    auto font = face_of(mapped_font.Get());
    if (!font) return Error::io;
    return Fallback{mapped, std::move(font), scale};
}

} // namespace alchemy::host::gpu
