#include "alchemy/darwin/coretext/ct_internal.hpp"
#include "alchemy/darwin/coregraphics/context.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <numbers>
#include <set>

// CTFont / NSFont (one class, toll-free bridged), CTFontDescriptor /
// NSFontDescriptor, CTFontManager, CGFont and a minimal NSFontManager.
namespace alchemy::gui::quartz2d {
namespace {

struct DescriptorState final : ObjectPayload {
    Address attributes{};                     // +1 CFDictionary
};

// The font files registered by URL. Like the host's registered fonts they
// stay for the life of the process.
struct RegisteredFiles {
    std::mutex mutex;
    std::set<std::string> paths;
};
RegisteredFiles& registered_files() {
    static RegisteredFiles files;
    return files;
}
bool is_registered_file(const std::string& path) {
    auto& files = registered_files();
    std::lock_guard lock(files.mutex);
    return files.paths.contains(path);
}
enum class Registration { added, already, unreadable, invalid };
Registration register_file(Quartz& q, const std::string& path, std::string* error) {
    auto& files = registered_files();
    std::lock_guard lock(files.mutex);
    if (files.paths.contains(path)) return Registration::already;
    const auto bytes = path.empty() ? std::nullopt : q.read_file(path);
    if (!bytes) return Registration::unreadable;
    if (!register_font_file(*bytes, error)) return Registration::invalid;
    files.paths.insert(path);
    return Registration::added;
}

constexpr std::string_view descriptor_keys[] = {
    "kCTFontNameAttribute|NSFontNameAttribute", "kCTFontFamilyNameAttribute|NSFontFamilyAttribute",
    "kCTFontDisplayNameAttribute|NSFontVisibleNameAttribute", "kCTFontStyleNameAttribute|NSFontFaceAttribute",
    "kCTFontSizeAttribute|NSFontSizeAttribute", "kCTFontMatrixAttribute|NSFontMatrixAttribute",
    "kCTFontTraitsAttribute|NSCTFontTraitsAttribute", "kCTFontSymbolicTrait|NSCTFontSymbolicTrait",
    "kCTFontWeightTrait|NSCTFontWeightTrait", "kCTFontWidthTrait|NSCTFontProportionTrait", "kCTFontSlantTrait|NSCTFontSlantTrait",
    "kCTFontURLAttribute|NSCTFontFileURLAttribute", "kCTFontFeatureSettingsAttribute|NSCTFontFeatureSettingsAttribute",
    "kCTFontFixedAdvanceAttribute|NSCTFontFixedAdvanceAttribute", "kCTFontCascadeListAttribute|NSCTFontCascadeListAttribute",
    "kCTFontUIUsageAttribute|NSCTFontUIUsageAttribute", "kCTFontOrientationAttribute|NSCTFontOrientationAttribute",
    "kCTFontFamilyNameKey|CTFontFamilyName", "kCTFontSubFamilyNameKey|CTFontSubFamilyName", "kCTFontStyleNameKey|CTFontSubFamilyName",
    "kCTFontFullNameKey|CTFontFullName", "kCTFontPostScriptNameKey|CTFontPostScriptName", "kCTFontUniqueNameKey|CTFontUniqueName",
    "kCTFontVersionNameKey|CTFontVersionName", "kCTFontCopyrightNameKey|CTFontCopyrightName",
    "kCTFontManagerRegisteredFontsChangedNotification|CTFontManagerFontChangedNotification",
    "kCTFontManagerErrorDomain|com.apple.CoreText.CTFontManagerErrorDomain",
};
// NSFontWeight values (CGFloat) <-> host font weights (host::gpu::font_weight).
double weight_trait(int weight) {
    const auto w = weight;
    if (w <= 100) return -0.8;
    if (w <= 200) return -0.6;
    if (w <= 300) return -0.4;
    if (w <= 400) return 0.0;
    if (w <= 500) return 0.23;
    if (w <= 600) return 0.3;
    if (w <= 700) return 0.4;
    if (w <= 800) return 0.56;
    return 0.62;
}
// CFEqual / -isEqual: of two fonts, as CoreText: the same face, size and
// matrix (and the same reported name: a mapped Apple name differs from the
// host face it resolves to).
bool same_matrix(const Affine& a, const Affine& b) {
    return a.a == b.a && a.b == b.b && a.c == b.c && a.d == b.d && a.tx == b.tx && a.ty == b.ty;
}
bool same_font(const FontState& a, const FontState& b) {
    return a.face == b.face && a.size == b.size && a.postscript == b.postscript && same_matrix(a.matrix, b.matrix);
}
std::uint64_t font_hash(const FontState& font) {
    return std::hash<std::string>{}(font.postscript) ^ static_cast<std::uint64_t>(font.size * 64);
}

int weight_from_trait(double value) {
    namespace fw = host::gpu::font_weight;
    if (value <= -0.7) return fw::thin;
    if (value <= -0.5) return fw::extra_light;
    if (value <= -0.2) return fw::light;
    if (value < 0.1) return fw::normal;
    if (value < 0.27) return fw::medium;
    if (value < 0.35) return fw::semi_bold;
    if (value < 0.5) return fw::bold;
    if (value < 0.6) return fw::extra_bold;
    return fw::black;
}

} // namespace

Address make_font(Quartz& q, const ResolvedFont& font, double size, const Affine& matrix) {
    auto state = std::make_shared<FontState>();
    state->face = font.face;
    state->size = size > 0 ? size : 12.0;
    state->matrix = matrix;
    state->postscript = font.postscript.empty() ? font.face->postscript : font.postscript;
    state->family = font.family.empty() ? font.face->family : font.family;
    state->display = font.display.empty() ? font.face->full_name : font.display;
    return q.create(coretext().font_class, state);
}

Address default_font(Quartz& q) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    auto& ct = coretext();
    if (!ct.default_font) {
        const auto helvetica = resolve_font("Helvetica");
        if (!helvetica) fail("no default font (Arial) is installed");
        ct.default_font = make_font(q, *helvetica, 12);
        if (const auto record = q.process.objects().find(ct.default_font)) record->immortal = true;
    }
    return ct.default_font;
}

std::shared_ptr<FontState> font_state(Quartz& q, Address font) { return font ? q.find<FontState>(font) : nullptr; }

void install_font_exports(Quartz& q) {
    auto& ct = coretext();
    ct.font_class = q.declare_class("NSFont", "NSObject", appkit_library);
    ct.descriptor_class = q.declare_class("NSFontDescriptor", "NSObject", appkit_library);
    q.font_class = q.declare_class("_AlchemyCGFont");
    Quartz* const quartz = &q;
    const auto lib = core_text_library;
    const auto cg = core_graphics_library;
    for (const auto entry : descriptor_keys) {
        const auto bar = entry.find('|');
        q.string_constant(lib, "_" + std::string(entry.substr(0, bar)), entry.substr(bar + 1));
    }
    // AppKit attribute keys (NSAttributedString drawing).
    for (const auto* entry : {"NSFontAttributeName|NSFont", "NSForegroundColorAttributeName|NSColor",
             "NSBackgroundColorAttributeName|NSBackgroundColor", "NSParagraphStyleAttributeName|NSParagraphStyle",
             "NSKernAttributeName|NSKern", "NSUnderlineStyleAttributeName|NSUnderline", "NSUnderlineColorAttributeName|NSUnderlineColor",
             "NSStrikethroughStyleAttributeName|NSStrikethrough", "NSStrikethroughColorAttributeName|NSStrikethroughColor",
             "NSStrokeWidthAttributeName|NSStrokeWidth", "NSStrokeColorAttributeName|NSStrokeColor",
             "NSBaselineOffsetAttributeName|NSBaselineOffset", "NSLigatureAttributeName|NSLigature", "NSShadowAttributeName|NSShadow",
             "NSLinkAttributeName|NSLink", "NSToolTipAttributeName|NSToolTip", "NSCursorAttributeName|NSCursor",
             "NSSuperscriptAttributeName|NSSuperScript", "NSObliquenessAttributeName|NSObliqueness",
             "NSExpansionAttributeName|NSExpansion", "NSWritingDirectionAttributeName|NSWritingDirection",
             "NSAttachmentAttributeName|NSAttachment", "NSFontFamilyAttribute|NSFontFamilyAttribute",
             "NSFontNameAttribute|NSFontNameAttribute", "NSFontSizeAttribute|NSFontSizeAttribute",
             "NSFontFaceAttribute|NSFontFaceAttribute", "NSFontTraitsAttribute|NSCTFontTraitsAttribute",
             "NSFontSymbolicTrait|NSCTFontSymbolicTrait", "NSFontWeightTrait|NSCTFontWeightTrait",
             "NSFontWidthTrait|NSCTFontProportionTrait", "NSFontSlantTrait|NSCTFontSlantTrait",
             "NSFontVisibleNameAttribute|NSFontVisibleNameAttribute", "NSFontMatrixAttribute|NSFontMatrixAttribute"}) {
        const std::string_view text = entry;
        const auto bar = text.find('|');
        q.string_constant(appkit_library, "_" + std::string(text.substr(0, bar)), text.substr(bar + 1));
    }
    // Cocoa re-exports AppKit: the attribute keys Cocoa-linked images bind.
    constexpr std::string_view cocoa_library = "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa";
    for (const auto* entry : {"NSFontAttributeName|NSFont", "NSForegroundColorAttributeName|NSColor",
             "NSBackgroundColorAttributeName|NSBackgroundColor", "NSParagraphStyleAttributeName|NSParagraphStyle",
             "NSUnderlineStyleAttributeName|NSUnderline", "NSKernAttributeName|NSKern", "NSLinkAttributeName|NSLink",
             "NSStrikethroughStyleAttributeName|NSStrikethrough", "NSBaselineOffsetAttributeName|NSBaselineOffset",
             "NSShadowAttributeName|NSShadow", "NSToolTipAttributeName|NSToolTip"}) {
        const std::string_view text = entry;
        const auto bar = text.find('|');
        q.string_constant(cocoa_library, "_" + std::string(text.substr(0, bar)), text.substr(bar + 1));
    }
    // NSFontWeight constants (CGFloat data).
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, double>>{
             {"_NSFontWeightUltraLight", -0.8}, {"_NSFontWeightThin", -0.6}, {"_NSFontWeightLight", -0.4}, {"_NSFontWeightRegular", 0.0},
             {"_NSFontWeightMedium", 0.23}, {"_NSFontWeightSemibold", 0.3}, {"_NSFontWeightBold", 0.4}, {"_NSFontWeightHeavy", 0.56},
             {"_NSFontWeightBlack", 0.62}}) {
        const auto bits = std::bit_cast<std::array<std::uint8_t, 8>>(value);
        q.data_constant(appkit_library, name, bits);
    }

    const auto fn = [&q, lib](const char* name, Handler handler) { q.function(lib, name, std::move(handler)); };
    const auto font = [quartz](Address object) { return quartz->require<FontState>(object, "CTFont"); };
    const auto matrix_arg = [quartz](Address pointer) { return pointer ? quartz->read_affine(pointer) : Affine{}; };
    const auto by_name = [quartz](Address name, double size, const Affine& matrix) {
        const auto text = name ? quartz->cf.utf8(name) : std::string();
        auto resolved = resolve_font(text);
        if (!resolved) resolved = resolve_font("Helvetica");
        if (!resolved) fail("no font for " + text);
        return make_font(*quartz, *resolved, size, matrix);
    };

    // ---- creation ----
    fn("_CTFontCreateWithName", [by_name, matrix_arg](CpuState& s, GuestMemory&) { s.x[0] = by_name(s.x[0], d(s, 0), matrix_arg(s.x[1])); });
    fn("_CTFontCreateWithNameAndOptions", [by_name, matrix_arg](CpuState& s, GuestMemory&) { s.x[0] = by_name(s.x[0], d(s, 0), matrix_arg(s.x[1])); });
    fn("_CTFontCreateUIFontForLanguage", [quartz](CpuState& s, GuestMemory&) {
        // (CTFontUIFontType, size d0, language)
        const auto entry = ui_font(static_cast<std::int32_t>(s.x[0]));
        if (!entry) { s.x[0] = 0; return; }
        s.x[0] = make_font(*quartz, entry->first, d(s, 0) > 0 ? d(s, 0) : entry->second);
    });
    const auto descriptor_attributes = [quartz](Address descriptor) {
        return descriptor ? quartz->require<DescriptorState>(descriptor, "CTFontDescriptor")->attributes : 0;
    };
    // A font from descriptor attributes: name, else family + traits.
    const auto from_attributes = [quartz](Address attributes, double size, const Affine& matrix) -> Address {
        const auto value = [&](std::string_view key) { return quartz->dictionary_get(attributes, key); };
        if (size <= 0) size = quartz->number(value("NSFontSizeAttribute")).value_or(12);
        std::optional<ResolvedFont> resolved;
        // A font file's descriptor (CTFontManagerCreateFontDescriptorsFromURL): that
        // file's first face, even when an installed font has the same name; the
        // file is registered now.
        if (const auto url = value("NSCTFontFileURLAttribute")) {
            const auto path = quartz->cf.url_path(url);
            const auto bytes = register_file(*quartz, path, nullptr) == Registration::unreadable ? std::nullopt : quartz->read_file(path);
            if (const auto file_font = bytes ? host::gpu::font_from_data(*bytes) : host::Result<host::gpu::Font>(host::Error::not_found)) {
                auto face = face_from(*file_font, host::gpu::registered_fonts());
                resolved = ResolvedFont{face, face->postscript, face->family, face->full_name};
            }
        }
        if (!resolved)
            if (const auto name = value("NSFontNameAttribute")) resolved = resolve_font(quartz->cf.utf8(name));
        if (!resolved)
            if (const auto family = value("NSFontFamilyAttribute")) resolved = resolve_font(quartz->cf.utf8(family));
        if (!resolved) resolved = resolve_font(".AppleSystemUIFont");
        if (!resolved) return 0;
        const auto created = make_font(*quartz, *resolved, size, matrix);
        if (const auto traits = value("NSCTFontTraitsAttribute")) {
            const auto symbolic = quartz->number(quartz->dictionary_get(traits, "NSCTFontSymbolicTrait"));
            const auto weight = quartz->number(quartz->dictionary_get(traits, "NSCTFontWeightTrait"));
            const auto state = quartz->find<FontState>(created);
            std::optional<ResolvedFont> styled;
            if (weight)
                if (auto face = family_match(*state->face, weight_from_trait(*weight)))
                    styled = ResolvedFont{std::move(face), state->postscript, state->family, state->display};
            if (symbolic) {
                const auto mask = static_cast<std::uint32_t>(*symbolic) & (font_trait::bold | font_trait::italic);
                if (mask) if (auto changed = with_traits(*state, static_cast<std::uint32_t>(*symbolic), mask)) styled = changed;
            }
            if (styled) {
                quartz->release(created);
                return make_font(*quartz, *styled, size, matrix);
            }
        }
        return created;
    };
    fn("_CTFontCreateWithFontDescriptor", [descriptor_attributes, from_attributes, matrix_arg](CpuState& s, GuestMemory&) {
        s.x[0] = from_attributes(descriptor_attributes(s.x[0]), d(s, 0), matrix_arg(s.x[1]));
    });
    fn("_CTFontCreateWithFontDescriptorAndOptions", [descriptor_attributes, from_attributes, matrix_arg](CpuState& s, GuestMemory&) {
        s.x[0] = from_attributes(descriptor_attributes(s.x[0]), d(s, 0), matrix_arg(s.x[1]));
    });
    const auto copy_font = [quartz](const FontState& source, double size, const Affine& matrix) {
        return make_font(*quartz, {source.face, source.postscript, source.family, source.display}, size > 0 ? size : source.size, matrix);
    };
    fn("_CTFontCreateCopyWithAttributes", [font, copy_font, matrix_arg](CpuState& s, GuestMemory&) {
        const auto source = font(s.x[0]);
        s.x[0] = copy_font(*source, d(s, 0), s.x[1] ? matrix_arg(s.x[1]) : source->matrix);
    });
    fn("_CTFontCreateCopyWithSymbolicTraits", [quartz, font, matrix_arg](CpuState& s, GuestMemory&) {
        // (font, size d0, matrix, traitValue, traitMask)
        const auto source = font(s.x[0]);
        const auto resolved = with_traits(*source, static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]));
        s.x[0] = resolved ? make_font(*quartz, *resolved, d(s, 0) > 0 ? d(s, 0) : source->size, s.x[1] ? matrix_arg(s.x[1]) : source->matrix) : 0;
    });
    fn("_CTFontCreateCopyWithFamily", [quartz, font, matrix_arg](CpuState& s, GuestMemory&) {
        // (font, size d0, matrix, family)
        const auto source = font(s.x[0]);
        const auto resolved = s.x[2] ? with_family(*source, quartz->cf.utf8(s.x[2])) : std::nullopt;
        s.x[0] = resolved ? make_font(*quartz, *resolved, d(s, 0) > 0 ? d(s, 0) : source->size, s.x[1] ? matrix_arg(s.x[1]) : source->matrix) : 0;
    });
    fn("_CTFontCreateForString", [quartz, font](CpuState& s, GuestMemory&) {
        // (currentFont, string, CFRange x2 x3): a font that covers the range.
        const auto source = font(s.x[0]);
        const auto units = s.x[1] ? quartz->cf.units(s.x[1]) : std::vector<std::uint16_t>{};
        const auto first = std::min<std::size_t>(s.x[2], units.size());
        const auto last = std::min<std::size_t>(first + s.x[3], units.size());
        const std::span<const std::uint16_t> part(units.data() + first, last - first);
        const auto glyphs = glyphs_for(*source->face, part);
        bool covered = true;
        for (std::size_t index = 0; index < part.size(); ++index) {
            const bool trailing = index > 0 && part[index] >= 0xdc00 && part[index] <= 0xdfff;
            if (!trailing && glyphs[index] == 0) covered = false;
        }
        if (covered || part.empty()) { s.x[0] = quartz->retain(s.x[0]); return; }
        // The host's fallback for the first uncovered character run.
        const auto fallback = host::gpu::fallback_font(std::u16string_view(reinterpret_cast<const char16_t*>(part.data()), part.size()),
                                                       source->face->family, source->face->weight, source->face->style,
                                                       source->face->stretch);
        if (!fallback) {
            s.x[0] = quartz->retain(s.x[0]);
            return;
        }
        const auto resolved = face_from(fallback->font);
        s.x[0] = make_font(*quartz, {resolved, resolved->postscript, resolved->family, resolved->full_name}, source->size, source->matrix);
    });
    fn("_CTFontCreateWithGraphicsFont", [quartz, matrix_arg](CpuState& s, GuestMemory&) {
        // (graphicsFont, size d0, matrix, attributes)
        if (!s.x[0]) return;
        const auto graphics = quartz->require<GraphicsFontState>(s.x[0], "CGFont");
        s.x[0] = make_font(*quartz, {graphics->face, graphics->postscript, graphics->family, graphics->face->full_name}, d(s, 0), matrix_arg(s.x[1]));
    });
    const auto make_graphics_font = [quartz](std::shared_ptr<FontFace> face, std::string postscript, std::string family) {
        auto state = std::make_shared<GraphicsFontState>();
        state->face = std::move(face);
        state->postscript = std::move(postscript);
        state->family = std::move(family);
        return quartz->create(quartz->font_class, state);
    };
    fn("_CTFontCopyGraphicsFont", [font, make_graphics_font](CpuState& s, GuestMemory&) {
        const auto source = font(s.x[0]);
        s.x[0] = make_graphics_font(source->face, source->postscript, source->family);
    });

    // ---- names and attributes ----
    fn("_CTFontCopyPostScriptName", [quartz, font](CpuState& s, GuestMemory&) { s.x[0] = quartz->cf.string(font(s.x[0])->postscript); });
    fn("_CTFontCopyFamilyName", [quartz, font](CpuState& s, GuestMemory&) { s.x[0] = quartz->cf.string(font(s.x[0])->family); });
    fn("_CTFontCopyFullName", [quartz, font](CpuState& s, GuestMemory&) { s.x[0] = quartz->cf.string(font(s.x[0])->display); });
    fn("_CTFontCopyDisplayName", [quartz, font](CpuState& s, GuestMemory&) { s.x[0] = quartz->cf.string(font(s.x[0])->display); });
    const auto name_for = [quartz](const FontState& state, Address key) -> Address {
        const auto name = key ? quartz->cf.utf8(key) : std::string();
        if (name == "CTFontFamilyName") return quartz->cf.string(state.family);
        if (name == "CTFontFullName") return quartz->cf.string(state.display);
        if (name == "CTFontPostScriptName" || name == "CTFontUniqueName") return quartz->cf.string(state.postscript);
        if (name == "CTFontSubFamilyName") return quartz->cf.string(state.face->style_name.empty() ? "Regular" : state.face->style_name);
        return 0;
    };
    fn("_CTFontCopyName", [font, name_for](CpuState& s, GuestMemory&) { s.x[0] = name_for(*font(s.x[0]), s.x[1]); });
    fn("_CTFontCopyLocalizedName", [quartz, font, name_for](CpuState& s, GuestMemory& m) {
        s.x[0] = name_for(*font(s.x[0]), s.x[1]);
        if (s.x[2]) m.write64(s.x[2], s.x[0] ? quartz->cf.string("en") : 0);
    });
    const auto traits_dictionary = [quartz](const FontState& state) {
        std::vector<std::pair<Address, Address>> pairs{
            {quartz->cf.string("NSCTFontSymbolicTrait"), quartz->cf.integer(state.traits())},
            {quartz->cf.string("NSCTFontWeightTrait"), quartz->cf.real(weight_trait(state.face->weight))},
            {quartz->cf.string("NSCTFontProportionTrait"), quartz->cf.real(0)},
            {quartz->cf.string("NSCTFontSlantTrait"), quartz->cf.real(state.face->style == host::gpu::font_style::normal ? 0.0 : 0.07)}};
        const auto dictionary = quartz->cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { quartz->release(key); quartz->release(value); }
        return dictionary;
    };
    fn("_CTFontCopyTraits", [font, traits_dictionary](CpuState& s, GuestMemory&) { s.x[0] = traits_dictionary(*font(s.x[0])); });
    const auto font_descriptor = [quartz, traits_dictionary](const FontState& state) {
        std::vector<std::pair<Address, Address>> pairs{
            {quartz->cf.string("NSFontNameAttribute"), quartz->cf.string(state.postscript)},
            {quartz->cf.string("NSFontFamilyAttribute"), quartz->cf.string(state.family)},
            {quartz->cf.string("NSFontSizeAttribute"), quartz->cf.real(state.size)},
            {quartz->cf.string("NSCTFontTraitsAttribute"), traits_dictionary(state)}};
        const auto dictionary = quartz->cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { quartz->release(key); quartz->release(value); }
        auto descriptor = std::make_shared<DescriptorState>();
        descriptor->attributes = dictionary;
        return quartz->create(coretext().descriptor_class, descriptor, [quartz, descriptor](Address) { quartz->release(std::exchange(descriptor->attributes, 0)); });
    };
    fn("_CTFontCopyFontDescriptor", [font, font_descriptor](CpuState& s, GuestMemory&) { s.x[0] = font_descriptor(*font(s.x[0])); });
    fn("_CTFontCopyAttribute", [quartz, font, traits_dictionary](CpuState& s, GuestMemory&) {
        const auto state = font(s.x[0]);
        const auto key = s.x[1] ? quartz->cf.utf8(s.x[1]) : std::string();
        if (key == "NSFontNameAttribute") s.x[0] = quartz->cf.string(state->postscript);
        else if (key == "NSFontFamilyAttribute") s.x[0] = quartz->cf.string(state->family);
        else if (key == "NSFontVisibleNameAttribute") s.x[0] = quartz->cf.string(state->display);
        else if (key == "NSFontSizeAttribute") s.x[0] = quartz->cf.real(state->size);
        else if (key == "NSCTFontTraitsAttribute") s.x[0] = traits_dictionary(*state);
        else if (key == "NSFontFaceAttribute") s.x[0] = quartz->cf.string(state->face->style_name.empty() ? "Regular" : state->face->style_name);
        else s.x[0] = 0;
    });
    fn("_CTFontCopySupportedLanguages", [quartz](CpuState& s, GuestMemory&) {
        const auto en = quartz->cf.string("en");
        const std::array<Address, 1> values{en};
        s.x[0] = quartz->cf.array(values);
        quartz->release(en);
    });
    fn("_CTFontCopyDefaultCascadeListForLanguages", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->cf.array({}); });
    fn("_CTFontGetStringEncoding", [](CpuState& s, GuestMemory&) { s.x[0] = 0x0100; });   // kCFStringEncodingUnicode
    fn("_CTFontHasTable", [font](CpuState& s, GuestMemory&) {
        return_bool(s, static_cast<bool>(font(s.x[0])->face->font.table(static_cast<std::uint32_t>(s.x[1]))));
    });
    fn("_CTFontGetGlyphWithName", [quartz, font](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[1] ? glyph_with_name(*font(s.x[0])->face, quartz->cf.utf8(s.x[1])) : 0;
    });
    fn("_CTFontCopyNameForGlyph", [quartz, font](CpuState& s, GuestMemory&) {
        const auto name = glyph_name(*font(s.x[0])->face, static_cast<std::uint16_t>(s.x[1]));
        s.x[0] = name ? quartz->cf.string(*name) : 0;
    });
    fn("_CTFontCopyTable", [quartz, font](CpuState& s, GuestMemory&) {
        // (font, tag (FourCharCode), options)
        const auto state = font(s.x[0]);
        const auto table = state->face->font.table(static_cast<std::uint32_t>(s.x[1]));
        s.x[0] = table ? quartz->data_object(*table) : 0;
    });

    // ---- metrics ----
    const auto metric = [font](const char* name, double (*value)(const FontState&)) {
        return std::pair{name, value};
    };
    for (const auto& [name, value] : {
             metric("_CTFontGetSize", [](const FontState& f) { return f.size; }),
             metric("_CTFontGetAscent", [](const FontState& f) { return f.face->metrics.ascent * f.scale(); }),
             metric("_CTFontGetDescent", [](const FontState& f) { return f.face->metrics.descent * f.scale(); }),
             metric("_CTFontGetLeading", [](const FontState& f) { return f.face->metrics.leading * f.scale(); }),
             metric("_CTFontGetCapHeight", [](const FontState& f) { return f.face->metrics.cap_height * f.scale(); }),
             metric("_CTFontGetXHeight", [](const FontState& f) { return f.face->metrics.x_height * f.scale(); }),
             metric("_CTFontGetUnderlinePosition", [](const FontState& f) { return f.face->metrics.underline_position * f.scale(); }),
             metric("_CTFontGetUnderlineThickness", [](const FontState& f) { return f.face->metrics.underline_thickness * f.scale(); }),
             metric("_CTFontGetSlantAngle", [](const FontState& f) { return f.face->metrics.slant; })}) {
        const auto compute = value;
        fn(name, [font, compute](CpuState& s, GuestMemory&) { set_d(s, 0, compute(*font(s.x[0]))); });
    }
    fn("_CTFontGetUnitsPerEm", [font](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint32_t>(font(s.x[0])->face->metrics.units_per_em); });
    fn("_CTFontGetGlyphCount", [font](CpuState& s, GuestMemory&) { s.x[0] = font(s.x[0])->face->font.glyph_count(); });
    fn("_CTFontGetBoundingBox", [font](CpuState& s, GuestMemory&) {
        const auto state = font(s.x[0]);
        const auto box = state->face->metrics.bounding_box;
        const auto scale = state->scale();
        return_rect(s, {{box.origin.x * scale, box.origin.y * scale}, {box.size.width * scale, box.size.height * scale}});
    });
    fn("_CTFontGetMatrix", [quartz, font](CpuState& s, GuestMemory&) { quartz->write_affine(s.x[8], font(s.x[0])->matrix); });
    fn("_CTFontGetSymbolicTraits", [font](CpuState& s, GuestMemory&) { s.x[0] = font(s.x[0])->traits(); });
    fn("_CTFontGetGlyphsForCharacters", [quartz, font](CpuState& s, GuestMemory&) {
        // (font, characters, glyphs, count) -> all mapped
        const auto state = font(s.x[0]);
        const auto units = quartz->read_units(s.x[1], static_cast<std::size_t>(std::min<Address>(s.x[3], 1 << 20)));
        const auto glyphs = glyphs_for(*state->face, units);
        if (s.x[2]) quartz->write_units(s.x[2], glyphs);
        bool all = true;
        for (std::size_t index = 0; index < units.size(); ++index) {
            const bool trailing = index > 0 && units[index] >= 0xdc00 && units[index] <= 0xdfff && units[index - 1] >= 0xd800 && units[index - 1] <= 0xdbff;
            if (!trailing && glyphs[index] == 0) all = false;
        }
        return_bool(s, all);
    });
    fn("_CTFontGetAdvancesForGlyphs", [quartz, font](CpuState& s, GuestMemory&) {
        // (font, orientation, glyphs, advances (CGSize), count) -> total
        const auto state = font(s.x[0]);
        const auto glyphs = quartz->read_units(s.x[2], static_cast<std::size_t>(std::min<Address>(s.x[4], 1 << 20)));
        const auto advances = advances_for(*state->face, state->size, glyphs);
        double total{};
        const bool vertical = s.x[1] == 2;   // kCTFontOrientationVertical
        for (std::size_t index = 0; index < advances.size(); ++index) {
            const auto advance = vertical ? state->size : advances[index];
            total += advance;
            if (s.x[3]) {
                quartz->write_double(s.x[3] + index * 16, vertical ? 0 : advance);
                quartz->write_double(s.x[3] + index * 16 + 8, vertical ? advance : 0);
            }
        }
        set_d(s, 0, total);
    });
    const auto bounding = [quartz, font](CpuState& s) {
        // (font, orientation, glyphs, rects, count) -> overall rect
        const auto state = font(s.x[0]);
        const auto glyphs = quartz->read_units(s.x[2], static_cast<std::size_t>(std::min<Address>(s.x[4], 1 << 20)));
        const auto rects = glyph_bounds(*state->face, state->size, glyphs);
        Rect total = null_rect();
        for (std::size_t index = 0; index < rects.size(); ++index) {
            const auto& r = rects[index];
            if (s.x[3]) {
                quartz->write_double(s.x[3] + index * 32, r.origin.x); quartz->write_double(s.x[3] + index * 32 + 8, r.origin.y);
                quartz->write_double(s.x[3] + index * 32 + 16, r.size.width); quartz->write_double(s.x[3] + index * 32 + 24, r.size.height);
            }
            if (r.size.width > 0 && r.size.height > 0) total = rect_union(total, r);
        }
        return_rect(s, rect_is_null(total) ? Rect{} : total);
    };
    fn("_CTFontGetBoundingRectsForGlyphs", [bounding](CpuState& s, GuestMemory&) { bounding(s); });
    fn("_CTFontGetOpticalBoundsForGlyphs", [quartz, font](CpuState& s, GuestMemory&) {
        // (font, glyphs, rects, count, options): advance boxes
        const auto state = font(s.x[0]);
        const auto glyphs = quartz->read_units(s.x[1], static_cast<std::size_t>(std::min<Address>(s.x[3], 1 << 20)));
        const auto advances = advances_for(*state->face, state->size, glyphs);
        const auto scale = state->scale();
        const auto descent = state->face->metrics.descent * scale, ascent = state->face->metrics.ascent * scale;
        Rect total = null_rect();
        for (std::size_t index = 0; index < advances.size(); ++index) {
            const Rect r{{0, -descent}, {advances[index], ascent + descent}};
            if (s.x[2]) {
                quartz->write_double(s.x[2] + index * 32, r.origin.x); quartz->write_double(s.x[2] + index * 32 + 8, r.origin.y);
                quartz->write_double(s.x[2] + index * 32 + 16, r.size.width); quartz->write_double(s.x[2] + index * 32 + 24, r.size.height);
            }
            total = rect_union(total, r);
        }
        return_rect(s, rect_is_null(total) ? Rect{} : total);
    });
    fn("_CTFontCreatePathForGlyph", [quartz, font, matrix_arg](CpuState& s, GuestMemory&) {
        // (font, glyph, matrix)
        const auto state = font(s.x[0]);
        GlyphRunDraw run;
        run.face = state->face;
        run.size = state->size;
        run.matrix = state->matrix;
        run.glyphs = {static_cast<std::uint16_t>(s.x[1])};
        run.positions = {{0, 0}};
        auto path = glyph_outlines(run);
        if (path.empty()) { s.x[0] = 0; return; }
        s.x[0] = quartz->make_path(path.transformed(matrix_arg(s.x[2])), false);
    });
    fn("_CTFontDrawGlyphs", [quartz, font](CpuState& s, GuestMemory&) {
        // (font, glyphs, positions, count, context): fill color of the context
        if (!s.x[4]) return;
        const auto state = font(s.x[0]);
        GlyphRunDraw run;
        run.face = state->face;
        run.size = state->size;
        run.matrix = state->matrix;
        run.glyphs = quartz->read_units(s.x[1], static_cast<std::size_t>(std::min<Address>(s.x[3], 1 << 20)));
        for (Address index = 0; index < run.glyphs.size(); ++index) run.positions.push_back(quartz->read_point(s.x[2] + index * 16));
        const auto c = context_state(*quartz, s.x[4]);
        std::lock_guard lock(c->mutex);
        draw_glyphs(*quartz, *c, run, {}, std::nullopt);
    });
    fn("_CTFontGetLigatureCaretPositions", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    fn("_CTFontGetVerticalTranslationsForGlyphs", [quartz, font](CpuState& s, GuestMemory&) {
        // (font, glyphs, translations (CGSize), count)
        const auto state = font(s.x[0]);
        const auto glyphs = quartz->read_units(s.x[1], static_cast<std::size_t>(std::min<Address>(s.x[3], 1 << 20)));
        const auto advances = advances_for(*state->face, state->size, glyphs);
        for (std::size_t index = 0; index < advances.size() && s.x[2]; ++index) {
            quartz->write_double(s.x[2] + index * 16, -advances[index] / 2);
            quartz->write_double(s.x[2] + index * 16 + 8, -state->face->metrics.ascent * state->scale());
        }
    });
    fn("_CTFontGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CTFontDescriptor ----
    const auto make_descriptor = [quartz](Address attributes) {
        auto state = std::make_shared<DescriptorState>();
        state->attributes = attributes ? quartz->cf.copy(attributes) : quartz->cf.dictionary({});
        return quartz->create(coretext().descriptor_class, state, [quartz, state](Address) { quartz->release(std::exchange(state->attributes, 0)); });
    };
    const auto merged = [quartz](Address base, std::vector<std::pair<Address, Address>> extra) {
        auto entries = base ? quartz->cf.dictionary_entries(base) : std::vector<std::pair<Address, Address>>{};
        for (const auto& [key, value] : extra) {
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& e) { return quartz->cf.equal(e.first, key); }), entries.end());
            entries.emplace_back(key, value);
        }
        return quartz->cf.dictionary(entries);
    };
    fn("_CTFontDescriptorCreateWithNameAndSize", [quartz, make_descriptor](CpuState& s, GuestMemory&) {
        const auto key = quartz->cf.string("NSFontNameAttribute"), size_key = quartz->cf.string("NSFontSizeAttribute");
        const auto size = quartz->cf.real(d(s, 0));
        std::vector<std::pair<Address, Address>> pairs{{key, s.x[0]}, {size_key, size}};
        const auto dictionary = quartz->cf.dictionary(pairs);
        quartz->release(key); quartz->release(size_key); quartz->release(size);
        s.x[0] = make_descriptor(dictionary);
        quartz->release(dictionary);
    });
    fn("_CTFontDescriptorCreateWithAttributes", [make_descriptor](CpuState& s, GuestMemory&) { s.x[0] = make_descriptor(s.x[0]); });
    fn("_CTFontDescriptorCreateCopyWithAttributes", [quartz, descriptor_attributes, make_descriptor, merged](CpuState& s, GuestMemory&) {
        const auto combined = merged(descriptor_attributes(s.x[0]), quartz->cf.dictionary_entries(s.x[1]));
        s.x[0] = make_descriptor(combined);
        quartz->release(combined);
    });
    fn("_CTFontDescriptorCreateCopyWithFamily", [quartz, descriptor_attributes, make_descriptor, merged](CpuState& s, GuestMemory&) {
        const auto key = quartz->cf.string("NSFontFamilyAttribute");
        auto combined = merged(descriptor_attributes(s.x[0]), {{key, s.x[1]}});
        quartz->release(key);
        s.x[0] = make_descriptor(combined);
        quartz->release(combined);
    });
    fn("_CTFontDescriptorCreateCopyWithSymbolicTraits", [quartz, descriptor_attributes, make_descriptor, merged](CpuState& s, GuestMemory&) {
        const auto key = quartz->cf.string("NSCTFontTraitsAttribute"), trait = quartz->cf.string("NSCTFontSymbolicTrait");
        const auto value = quartz->cf.integer(static_cast<std::int64_t>(s.x[1] & s.x[2]));
        std::vector<std::pair<Address, Address>> traits{{trait, value}};
        const auto traits_dictionary = quartz->cf.dictionary(traits);
        auto combined = merged(descriptor_attributes(s.x[0]), {{key, traits_dictionary}});
        for (const auto object : {key, trait, value, traits_dictionary}) quartz->release(object);
        s.x[0] = make_descriptor(combined);
        quartz->release(combined);
    });
    fn("_CTFontDescriptorCreateCopyWithVariation", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->retain(s.x[0]); });
    fn("_CTFontDescriptorCreateMatchingFontDescriptor", [quartz, descriptor_attributes, from_attributes, font_descriptor](CpuState& s, GuestMemory&) {
        const auto created = from_attributes(descriptor_attributes(s.x[0]), 0, {});
        if (!created) { s.x[0] = 0; return; }
        s.x[0] = font_descriptor(*quartz->find<FontState>(created));
        quartz->release(created);
    });
    fn("_CTFontDescriptorCreateMatchingFontDescriptors", [quartz, descriptor_attributes, from_attributes, font_descriptor](CpuState& s, GuestMemory&) {
        const auto created = from_attributes(descriptor_attributes(s.x[0]), 0, {});
        if (!created) { s.x[0] = 0; return; }
        const auto descriptor = font_descriptor(*quartz->find<FontState>(created));
        quartz->release(created);
        const std::array<Address, 1> values{descriptor};
        s.x[0] = quartz->cf.array(values);
        quartz->release(descriptor);
    });
    fn("_CTFontDescriptorCopyAttributes", [quartz, descriptor_attributes](CpuState& s, GuestMemory&) { s.x[0] = quartz->retain(descriptor_attributes(s.x[0])); });
    fn("_CTFontDescriptorCopyAttribute", [quartz, descriptor_attributes](CpuState& s, GuestMemory&) {
        Address found{};
        for (const auto& [key, value] : quartz->cf.dictionary_entries(descriptor_attributes(s.x[0])))
            if (quartz->cf.equal(key, s.x[1])) found = value;
        s.x[0] = quartz->retain(found);
    });
    fn("_CTFontDescriptorCopyLocalizedAttribute", [quartz, descriptor_attributes](CpuState& s, GuestMemory& m) {
        Address found{};
        for (const auto& [key, value] : quartz->cf.dictionary_entries(descriptor_attributes(s.x[0])))
            if (quartz->cf.equal(key, s.x[1])) found = value;
        if (s.x[2]) m.write64(s.x[2], 0);
        s.x[0] = quartz->retain(found);
    });
    fn("_CTFontDescriptorGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CTFontManager ----
    // kCTFontManagerError* codes.
    constexpr std::int64_t file_not_found = 101, invalid_font_data = 104, already_registered = 105, asset_not_found = 107,
                           not_registered = 201, in_use = 202, insufficient_info = 303;
    // A CFError (+1, 0 when CFErrorCreate is missing) of kCTFontManagerErrorDomain about `url`.
    const auto manager_error = [quartz](std::int64_t code, Address url, const std::string& description) -> Address {
        const auto create = quartz->process.resolve(core_foundation_library, "_CFErrorCreate");
        if (!create) return 0;
        std::vector<std::pair<Address, Address>> pairs{{quartz->cf.string("NSLocalizedDescription"), quartz->cf.string(description)}};
        if (url) {
            const std::array<Address, 1> urls{url};
            pairs.emplace_back(quartz->cf.string("CTFontManagerErrorFontURLs"), quartz->cf.array(urls));
        }
        const auto info = quartz->cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { quartz->release(key); quartz->release(value); }
        const auto domain = quartz->cf.string("com.apple.CoreText.CTFontManagerErrorDomain");
        const auto error = quartz->call_guest(create->address, {0, domain, static_cast<Address>(code), info});
        quartz->release(domain);
        quartz->release(info);
        return error;
    };
    // The operations below answer nullopt on success, else the error (+1, may be 0).
    using Outcome = std::optional<Address>;
    const auto register_url = [quartz, manager_error](Address url) -> Outcome {
        const auto path = url ? quartz->cf.url_path(url) : std::string();
        std::string error = "unknown error";
        switch (register_file(*quartz, path, &error)) {
        case Registration::added: return std::nullopt;
        case Registration::already: return manager_error(already_registered, url, "The font file is already registered: " + path);
        case Registration::unreadable: return manager_error(file_not_found, url, "The font file could not be read: " + path);
        case Registration::invalid: break;
        }
        diagnostic("CTFontManagerRegisterFontsForURL: " + path + ": " + error);
        return manager_error(invalid_font_data, url, "The font file could not be registered: " + path + ": " + error);
    };
    const auto unregister_url = [quartz, manager_error](Address url) -> Outcome {
        const auto path = url ? quartz->cf.url_path(url) : std::string();
        if (!is_registered_file(path)) return manager_error(not_registered, url, "The font file is not registered: " + path);
        return manager_error(in_use, url, "CTFontManagerUnregisterFontsForURL: registered fonts stay for the life of the process: " + path);
    };
    // An error out-parameter (the caller releases what it gets).
    const auto put_error = [quartz](Address out, const Outcome& outcome) {
        const auto error = outcome.value_or(0);
        if (out) quartz->memory().write64(out, error);
        else quartz->release(error);
    };
    // `each` over an array's values: nullopt, or an array (+1) of the errors.
    const auto for_each_value = [quartz](Address array, const std::function<Outcome(Address)>& each) -> Outcome {
        std::vector<Address> errors;
        bool failed{};
        for (const auto value : array ? quartz->cf.array_values(array) : std::vector<Address>{})
            if (const auto outcome = each(value)) {
                failed = true;
                if (*outcome) errors.push_back(*outcome);
            }
        if (!failed) return std::nullopt;
        const auto list = quartz->cf.array(errors);
        for (const auto error : errors) quartz->release(error);
        return list;
    };
    // A registrationHandler(errors, done) -> bool block, called once with every error.
    const auto report = [quartz](Address block, const Outcome& outcome) {
        const auto errors = outcome.value_or(0);
        if (!block) return quartz->release(errors);
        const auto list = errors ? errors : quartz->cf.array({});
        static_cast<void>(quartz->call_guest(quartz->memory().read64(block + 16), {block, list, 1}));
        quartz->release(list);
    };
    fn("_CTFontManagerRegisterFontsForURL", [register_url, put_error](CpuState& s, GuestMemory&) {
        // (url, scope, error*)
        const auto outcome = register_url(s.x[0]);
        put_error(s.x[2], outcome);
        return_bool(s, !outcome);
    });
    fn("_CTFontManagerRegisterFontsForURLs", [register_url, for_each_value, put_error](CpuState& s, GuestMemory&) {
        // (urls, scope, errors*)
        const auto outcome = for_each_value(s.x[0], register_url);
        put_error(s.x[2], outcome);
        return_bool(s, !outcome);
    });
    fn("_CTFontManagerRegisterFontURLs", [register_url, for_each_value, report](CpuState& s, GuestMemory&) {
        // (urls, scope, enabled, registrationHandler)
        report(s.x[3], for_each_value(s.x[0], register_url));
    });
    fn("_CTFontManagerUnregisterFontsForURL", [unregister_url, put_error](CpuState& s, GuestMemory&) {
        const auto outcome = unregister_url(s.x[0]);
        put_error(s.x[2], outcome);
        return_bool(s, !outcome);
    });
    fn("_CTFontManagerUnregisterFontURLs", [unregister_url, for_each_value, report](CpuState& s, GuestMemory&) {
        // (urls, scope, registrationHandler)
        report(s.x[2], for_each_value(s.x[0], unregister_url));
    });
    // A descriptor's font file (kCTFontURLAttribute).
    const auto descriptor_file = [quartz, descriptor_attributes, manager_error](Address descriptor, const auto& operation) -> Outcome {
        const auto url = quartz->dictionary_get(descriptor_attributes(descriptor), "NSCTFontFileURLAttribute");
        if (!url) return manager_error(insufficient_info, 0, "The font descriptor names no font file");
        return operation(url);
    };
    fn("_CTFontManagerRegisterFontDescriptors", [register_url, descriptor_file, for_each_value, report](CpuState& s, GuestMemory&) {
        // (descriptors, scope, enabled, registrationHandler)
        report(s.x[3], for_each_value(s.x[0], [&](Address descriptor) { return descriptor_file(descriptor, register_url); }));
    });
    fn("_CTFontManagerUnregisterFontDescriptors", [unregister_url, descriptor_file, for_each_value, report](CpuState& s, GuestMemory&) {
        // (descriptors, scope, registrationHandler)
        report(s.x[2], for_each_value(s.x[0], [&](Address descriptor) { return descriptor_file(descriptor, unregister_url); }));
    });
    fn("_CTFontManagerRegisterFontsWithAssetNames", [quartz, manager_error, for_each_value, report](CpuState& s, GuestMemory&) {
        // (names, bundle, scope, enabled, registrationHandler): no font assets exist here.
        report(s.x[4], for_each_value(s.x[0], [&](Address name) -> Outcome {
            return manager_error(asset_not_found, 0, "No font asset named " + quartz->cf.utf8(name));
        }));
    });
    // CGFonts made from data are registered when they are made (CGFontCreateWithDataProvider).
    fn("_CTFontManagerRegisterGraphicsFont", [](CpuState& s, GuestMemory&) { return_bool(s, true); });
    fn("_CTFontManagerUnregisterGraphicsFont", [manager_error, put_error](CpuState& s, GuestMemory&) {
        put_error(s.x[1], manager_error(in_use, 0, "CTFontManagerUnregisterGraphicsFont: registered fonts stay for the life of the process"));
        return_bool(s, false);
    });
    fn("_CTFontManagerGetScopeForURL", [quartz](CpuState& s, GuestMemory&) {
        // kCTFontManagerScopeProcess for a file registered here, else kCTFontManagerScopeNone.
        s.x[0] = s.x[0] && is_registered_file(quartz->cf.url_path(s.x[0])) ? 1 : 0;
    });
    fn("_CTFontManagerIsSupportedFont", [quartz](CpuState& s, GuestMemory&) {
        const auto path = s.x[0] ? quartz->cf.url_path(s.x[0]) : std::string();
        const auto bytes = path.empty() ? std::nullopt : quartz->read_file(path);
        return_bool(s, bytes && static_cast<bool>(host::gpu::font_from_data(*bytes)));
    });
    fn("_CTFontManagerCopyAvailableFontFamilyNames", [quartz](CpuState& s, GuestMemory&) {
        std::vector<Address> names;
        for (const auto& name : available_families()) names.push_back(quartz->cf.string(name));
        s.x[0] = quartz->cf.array(names);
        for (const auto name : names) quartz->release(name);
    });
    fn("_CTFontManagerCopyAvailablePostScriptNames", [quartz](CpuState& s, GuestMemory&) {
        std::vector<std::string> names;
        for (const auto& family : available_families())
            for (const auto& face : family_members(family))
                if (!face->postscript.empty()) names.push_back(face->postscript);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        std::vector<Address> values;
        for (const auto& name : names) values.push_back(quartz->cf.string(name));
        s.x[0] = quartz->cf.array(values);
        for (const auto value : values) quartz->release(value);
    });
    // The descriptor of a font file's first face: its names and URL.
    const auto file_descriptor = [quartz, make_descriptor](Address url, std::span<const std::uint8_t> bytes) -> Address {
        const auto font = host::gpu::font_from_data(bytes);
        if (!font) return 0;
        const auto info = font->info();
        if (!info) return 0;
        std::vector<std::pair<Address, Address>> pairs{
            {quartz->cf.string("NSFontNameAttribute"), quartz->cf.string(info->postscript)},
            {quartz->cf.string("NSFontFamilyAttribute"), quartz->cf.string(info->family)},
            {quartz->cf.string("NSFontFaceAttribute"), quartz->cf.string(info->style_name)},
            {quartz->cf.string("NSCTFontFileURLAttribute"), quartz->retain(url)}};
        const auto attributes = quartz->cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { quartz->release(key); quartz->release(value); }
        const auto descriptor = make_descriptor(attributes);
        quartz->release(attributes);
        return descriptor;
    };
    fn("_CTFontManagerCreateFontDescriptorsFromURL", [quartz, file_descriptor](CpuState& s, GuestMemory&) {
        const auto path = s.x[0] ? quartz->cf.url_path(s.x[0]) : std::string();
        const auto bytes = path.empty() ? std::nullopt : quartz->read_file(path);
        const auto descriptor = bytes ? file_descriptor(s.x[0], *bytes) : 0;
        if (!descriptor) { s.x[0] = 0; return; }
        const std::array<Address, 1> values{descriptor};
        s.x[0] = quartz->cf.array(values);
        quartz->release(descriptor);
    });
    // The descriptors no installed or registered font matches.
    fn("_CTFontManagerRequestFonts", [quartz, descriptor_attributes](CpuState& s, GuestMemory& m) {
        // (descriptors, completionHandler(unresolvedFontDescriptors))
        std::vector<Address> unresolved;
        for (const auto descriptor : s.x[0] ? quartz->cf.array_values(s.x[0]) : std::vector<Address>{}) {
            const auto attributes = descriptor_attributes(descriptor);
            const auto name = quartz->dictionary_get(attributes, "NSFontNameAttribute");
            const auto family = quartz->dictionary_get(attributes, "NSFontFamilyAttribute");
            const auto named = name ? name : family;
            if (!named || !resolve_font(quartz->cf.utf8(named))) unresolved.push_back(descriptor);
        }
        const auto list = quartz->cf.array(unresolved);
        if (s.x[1]) static_cast<void>(quartz->call_guest(m.read64(s.x[1] + 16), {s.x[1], list}));
        quartz->release(list);
    });
    // Auto-activation settings by bundle identifier, kept for this process (no
    // font activates on demand here).
    struct Activation {
        std::mutex mutex;
        std::map<std::string, std::int64_t> settings;
    };
    const auto activation = std::make_shared<Activation>();
    fn("_CTFontManagerSetAutoActivationSetting", [quartz, activation](CpuState& s, GuestMemory&) {
        std::lock_guard lock(activation->mutex);
        activation->settings[s.x[0] ? quartz->cf.utf8(s.x[0]) : std::string()] = static_cast<std::int64_t>(s.x[1]);
    });
    fn("_CTFontManagerGetAutoActivationSetting", [quartz, activation](CpuState& s, GuestMemory&) {
        std::lock_guard lock(activation->mutex);
        const auto found = activation->settings.find(s.x[0] ? quartz->cf.utf8(s.x[0]) : std::string());
        s.x[0] = found == activation->settings.end() ? 0 : static_cast<Address>(found->second);   // kCTFontManagerAutoActivationDefault
    });

    // ---- CGFont ----
    const auto graphics = [quartz](Address object) { return quartz->require<GraphicsFontState>(object, "CGFont"); };
    q.function(cg, "_CGFontCreateWithFontName", [quartz, make_graphics_font](CpuState& s, GuestMemory&) {
        const auto resolved = s.x[0] ? resolve_font(quartz->cf.utf8(s.x[0])) : std::nullopt;
        s.x[0] = resolved ? make_graphics_font(resolved->face, resolved->postscript, resolved->family) : 0;
    });
    q.function(cg, "_CGFontCreateWithDataProvider", [quartz, make_graphics_font](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return;
        const auto bytes = quartz->require<DataProviderInfo>(s.x[0], "CGDataProvider")->snapshot(m);
        // The font joins the registered fonts; its first face is the CGFont.
        const auto font = host::gpu::font_from_data(*bytes);
        if (!font) { s.x[0] = 0; return; }
        static_cast<void>(register_font_file(*bytes, nullptr));
        const auto resolved = face_from(*font);
        s.x[0] = make_graphics_font(resolved, resolved->postscript, resolved->family);
    });
    q.function(cg, "_CGFontRetain", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->retain(s.x[0]); });
    q.function(cg, "_CGFontRelease", [quartz](CpuState& s, GuestMemory&) { quartz->release(s.x[0]); });
    q.function(cg, "_CGFontGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
    q.function(cg, "_CGFontCopyPostScriptName", [quartz, graphics](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? quartz->cf.string(graphics(s.x[0])->postscript) : 0; });
    q.function(cg, "_CGFontCopyFullName", [quartz, graphics](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? quartz->cf.string(graphics(s.x[0])->face->full_name) : 0; });
    const auto design = [quartz, graphics](const char* name, double (*value)(const FontMetrics&)) {
        quartz->function(core_graphics_library, name, [graphics, value](CpuState& s, GuestMemory&) {
            s.x[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(value(graphics(s.x[0])->face->metrics))));
        });
    };
    design("_CGFontGetUnitsPerEm", [](const FontMetrics& m) { return m.units_per_em; });
    design("_CGFontGetAscent", [](const FontMetrics& m) { return m.ascent; });
    design("_CGFontGetDescent", [](const FontMetrics& m) { return -m.descent; });
    design("_CGFontGetLeading", [](const FontMetrics& m) { return m.leading; });
    design("_CGFontGetCapHeight", [](const FontMetrics& m) { return m.cap_height; });
    design("_CGFontGetXHeight", [](const FontMetrics& m) { return m.x_height; });
    q.function(cg, "_CGFontGetItalicAngle", [graphics](CpuState& s, GuestMemory&) { set_d(s, 0, graphics(s.x[0])->face->metrics.slant); });
    q.function(cg, "_CGFontGetStemV", [](CpuState& s, GuestMemory&) { set_d(s, 0, 0); });
    q.function(cg, "_CGFontGetFontBBox", [graphics](CpuState& s, GuestMemory&) { return_rect(s, graphics(s.x[0])->face->metrics.bounding_box); });
    q.function(cg, "_CGFontGetNumberOfGlyphs", [graphics](CpuState& s, GuestMemory&) { s.x[0] = graphics(s.x[0])->face->font.glyph_count(); });
    q.function(cg, "_CGFontGetGlyphAdvances", [quartz, graphics](CpuState& s, GuestMemory& m) {
        // (font, glyphs, count, int advances[]) in design units
        const auto state = graphics(s.x[0]);
        const auto glyphs = quartz->read_units(s.x[1], static_cast<std::size_t>(std::min<Address>(s.x[2], 1 << 20)));
        const auto advances = advances_for(*state->face, state->face->metrics.units_per_em, glyphs);
        for (std::size_t index = 0; index < advances.size() && s.x[3]; ++index)
            m.write32(s.x[3] + index * 4, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(advances[index]))));
        return_bool(s, true);
    });
    q.function(cg, "_CGFontGetGlyphBBoxes", [quartz, graphics](CpuState& s, GuestMemory&) {
        // (font, glyphs, count, CGRect bboxes[]) in design units
        const auto state = graphics(s.x[0]);
        const auto glyphs = quartz->read_units(s.x[1], static_cast<std::size_t>(std::min<Address>(s.x[2], 1 << 20)));
        const auto rects = glyph_bounds(*state->face, state->face->metrics.units_per_em, glyphs);
        for (std::size_t index = 0; index < rects.size() && s.x[3]; ++index) {
            quartz->write_double(s.x[3] + index * 32, rects[index].origin.x); quartz->write_double(s.x[3] + index * 32 + 8, rects[index].origin.y);
            quartz->write_double(s.x[3] + index * 32 + 16, rects[index].size.width); quartz->write_double(s.x[3] + index * 32 + 24, rects[index].size.height);
        }
        return_bool(s, true);
    });
    q.function(cg, "_CGFontGetGlyphWithGlyphName", [quartz, graphics](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] && s.x[1] ? glyph_with_name(*graphics(s.x[0])->face, quartz->cf.utf8(s.x[1])) : 0;
    });
    q.function(cg, "_CGFontCopyGlyphNameForGlyph", [quartz, graphics](CpuState& s, GuestMemory&) {
        const auto name = s.x[0] ? glyph_name(*graphics(s.x[0])->face, static_cast<std::uint16_t>(s.x[1])) : std::nullopt;
        s.x[0] = name ? quartz->cf.string(*name) : 0;
    });
    q.function(cg, "_CGFontCopyTableForTag", [quartz, graphics](CpuState& s, GuestMemory&) {
        const auto state = graphics(s.x[0]);
        const auto table = state->face->font.table(static_cast<std::uint32_t>(s.x[1]));
        s.x[0] = table ? quartz->data_object(*table) : 0;
    });

    // ---- NSFont (the CTFont class) ----
    const auto f = ct.font_class;
    const auto method = [quartz](Address cls, const char* name, Handler handler, const char* types = "@@:",
                                   const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(quartz->objc.add_native_method(cls, name, scoped(std::move(handler)), false, types));
    };
    const auto class_method = [quartz](Address cls, const char* name, Handler handler, const char* types = "@@:",
                                   const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(quartz->objc.add_native_method(cls, name, scoped(std::move(handler)), true, types));
    };
    const auto ui = [quartz](std::int64_t type, double size) {
        const auto entry = ui_font(type);
        if (!entry) fail("no system font");
        return quartz->autorelease(make_font(*quartz, entry->first, size > 0 ? size : entry->second));
    };
    const auto system_weighted = [quartz](double size, double weight, bool monospaced) {
        auto resolved = resolve_font(monospaced ? ".AppleSystemUIFontMonospaced" : ".AppleSystemUIFont");
        if (!resolved) fail("no system font");
        const auto target = weight_from_trait(weight);
        if (target != host::gpu::font_weight::normal)
            if (auto face = family_match(*resolved->face, target)) resolved->face = std::move(face);
        return quartz->autorelease(make_font(*quartz, *resolved, size > 0 ? size : 13));
    };
    class_method(f, "systemFontOfSize:", [ui](CpuState& s, GuestMemory&) { s.x[0] = ui(2, d(s, 0)); }, "@@:d");
    class_method(f, "boldSystemFontOfSize:", [ui](CpuState& s, GuestMemory&) { s.x[0] = ui(3, d(s, 0)); }, "@@:d");
    class_method(f, "systemFontOfSize:weight:", [system_weighted](CpuState& s, GuestMemory&) { s.x[0] = system_weighted(d(s, 0), d(s, 1), false); }, "@@:dd");
    class_method(f, "monospacedSystemFontOfSize:weight:", [system_weighted](CpuState& s, GuestMemory&) { s.x[0] = system_weighted(d(s, 0), d(s, 1), true); }, "@@:dd");
    class_method(f, "monospacedDigitSystemFontOfSize:weight:", [system_weighted](CpuState& s, GuestMemory&) { s.x[0] = system_weighted(d(s, 0), d(s, 1), false); }, "@@:dd");
    class_method(f, "userFontOfSize:", [ui](CpuState& s, GuestMemory&) { s.x[0] = ui(0, d(s, 0)); }, "@@:d");
    class_method(f, "userFixedPitchFontOfSize:", [ui](CpuState& s, GuestMemory&) { s.x[0] = ui(1, d(s, 0)); }, "@@:d");
    for (const auto& [name, type] : std::initializer_list<std::pair<const char*, std::int64_t>>{
             {"labelFontOfSize:", 10}, {"messageFontOfSize:", 23}, {"menuFontOfSize:", 12}, {"menuBarFontOfSize:", 11},
             {"titleBarFontOfSize:", 15}, {"paletteFontOfSize:", 24}, {"toolTipsFontOfSize:", 25}, {"controlContentFontOfSize:", 26}}) {
        const auto value = type;
        class_method(f, name, [ui, value](CpuState& s, GuestMemory&) { s.x[0] = ui(value, d(s, 0)); }, "@@:d");
    }
    class_method(f, "fontWithName:size:", [quartz](CpuState& s, GuestMemory&) {
        const auto resolved = s.x[2] ? resolve_font(quartz->cf.utf8(s.x[2])) : std::nullopt;
        s.x[0] = resolved ? quartz->autorelease(make_font(*quartz, *resolved, d(s, 0) > 0 ? d(s, 0) : 12)) : 0;   // nil for unknown names
    }, "@@:@d");
    class_method(f, "fontWithDescriptor:size:", [quartz, descriptor_attributes, from_attributes](CpuState& s, GuestMemory&) {
        s.x[0] = quartz->autorelease(from_attributes(descriptor_attributes(s.x[2]), d(s, 0), {}));
    }, "@@:@d");
    class_method(f, "systemFontSize", [](CpuState& s, GuestMemory&) { set_d(s, 0, 13); }, "d@:");
    class_method(f, "smallSystemFontSize", [](CpuState& s, GuestMemory&) { set_d(s, 0, 11); }, "d@:");
    class_method(f, "labelFontSize", [](CpuState& s, GuestMemory&) { set_d(s, 0, 10); }, "d@:");
    class_method(f, "systemFontSizeForControlSize:", [](CpuState& s, GuestMemory&) {
        const auto size = s.x[2];   // regular 0, small 1, mini 2, large 3
        set_d(s, 0, size == 1 ? 11 : size == 2 ? 9 : 13);
    }, "d@:Q");
    class_method(f, "preferredFontForTextStyle:options:", [ui](CpuState& s, GuestMemory&) { s.x[0] = ui(2, 13); }, "@@:@@");
    method(f, "pointSize", [font](CpuState& s, GuestMemory&) { set_d(s, 0, font(s.x[0])->size); }, "d@:");
    method(f, "fontName", [quartz, font](CpuState& s, GuestMemory&) { s.x[0] = quartz->autorelease(quartz->cf.string(font(s.x[0])->postscript)); });
    method(f, "familyName", [quartz, font](CpuState& s, GuestMemory&) { s.x[0] = quartz->autorelease(quartz->cf.string(font(s.x[0])->family)); });
    method(f, "displayName", [quartz, font](CpuState& s, GuestMemory&) { s.x[0] = quartz->autorelease(quartz->cf.string(font(s.x[0])->display)); });
    method(f, "ascender", [font](CpuState& s, GuestMemory&) { const auto st = font(s.x[0]); set_d(s, 0, st->face->metrics.ascent * st->scale()); }, "d@:");
    method(f, "descender", [font](CpuState& s, GuestMemory&) { const auto st = font(s.x[0]); set_d(s, 0, -st->face->metrics.descent * st->scale()); }, "d@:");
    method(f, "leading", [font](CpuState& s, GuestMemory&) { const auto st = font(s.x[0]); set_d(s, 0, st->face->metrics.leading * st->scale()); }, "d@:");
    method(f, "capHeight", [font](CpuState& s, GuestMemory&) { const auto st = font(s.x[0]); set_d(s, 0, st->face->metrics.cap_height * st->scale()); }, "d@:");
    method(f, "xHeight", [font](CpuState& s, GuestMemory&) { const auto st = font(s.x[0]); set_d(s, 0, st->face->metrics.x_height * st->scale()); }, "d@:");
    method(f, "underlinePosition", [font](CpuState& s, GuestMemory&) { const auto st = font(s.x[0]); set_d(s, 0, st->face->metrics.underline_position * st->scale()); }, "d@:");
    method(f, "underlineThickness", [font](CpuState& s, GuestMemory&) { const auto st = font(s.x[0]); set_d(s, 0, st->face->metrics.underline_thickness * st->scale()); }, "d@:");
    method(f, "italicAngle", [font](CpuState& s, GuestMemory&) { set_d(s, 0, font(s.x[0])->face->metrics.slant); }, "d@:");
    method(f, "isFixedPitch", [font](CpuState& s, GuestMemory&) { return_bool(s, font(s.x[0])->face->monospace); }, "B@:");
    method(f, "numberOfGlyphs", [font](CpuState& s, GuestMemory&) { s.x[0] = font(s.x[0])->face->font.glyph_count(); }, "Q@:");
    method(f, "boundingRectForFont", [font](CpuState& s, GuestMemory&) {
        const auto st = font(s.x[0]);
        const auto box = st->face->metrics.bounding_box;
        const auto scale = st->scale();
        return_rect(s, {{box.origin.x * scale, box.origin.y * scale}, {box.size.width * scale, box.size.height * scale}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(f, "maximumAdvancement", [font](CpuState& s, GuestMemory&) {
        const auto st = font(s.x[0]);
        return_size(s, {st->face->metrics.bounding_box.size.width * st->scale(), 0});
    }, "{CGSize=dd}@:");
    method(f, "advancementForGlyph:", [font](CpuState& s, GuestMemory&) {
        const auto st = font(s.x[0]);
        const std::array<std::uint16_t, 1> glyph{static_cast<std::uint16_t>(s.x[2])};
        return_size(s, {advances_for(*st->face, st->size, glyph)[0], 0});
    }, "{CGSize=dd}@:I");
    method(f, "boundingRectForGlyph:", [font](CpuState& s, GuestMemory&) {
        const auto st = font(s.x[0]);
        const std::array<std::uint16_t, 1> glyph{static_cast<std::uint16_t>(s.x[2])};
        return_rect(s, glyph_bounds(*st->face, st->size, glyph)[0]);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:I");
    method(f, "fontDescriptor", [quartz, font, font_descriptor](CpuState& s, GuestMemory&) { s.x[0] = quartz->autorelease(font_descriptor(*font(s.x[0]))); });
    method(f, "fontWithSize:", [quartz, font, copy_font](CpuState& s, GuestMemory&) {
        const auto st = font(s.x[0]);
        s.x[0] = quartz->autorelease(copy_font(*st, d(s, 0), st->matrix));
    }, "@@:d");
    for (const auto* name : {"screenFont", "printerFont", "verticalFont"})
        method(f, name, [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->autorelease(quartz->retain(s.x[0])); });
    method(f, "screenFontWithRenderingMode:", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->autorelease(quartz->retain(s.x[0])); }, "@@:Q");
    method(f, "isVertical", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(f, "renderingMode", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:");
    method(f, "mostCompatibleStringEncoding", [](CpuState& s, GuestMemory&) { s.x[0] = 4; }, "Q@:");   // NSUTF8StringEncoding... unicode capable
    method(f, "copyWithZone:", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->retain(s.x[0]); }, "@@:^v");
    method(f, "isEqual:", [quartz, font](CpuState& s, GuestMemory&) {
        const auto other = quartz->find<FontState>(s.x[2]);
        return_bool(s, s.x[0] == s.x[2] || (other && same_font(*font(s.x[0]), *other)));
    }, "B@:@");
    method(f, "hash", [font](CpuState& s, GuestMemory&) { s.x[0] = font_hash(*font(s.x[0])); }, "Q@:");
    method(f, "description", [quartz, font](CpuState& s, GuestMemory&) {
        const auto st = font(s.x[0]);
        s.x[0] = quartz->autorelease(quartz->cf.string("\"" + st->postscript + " " + std::to_string(st->size) + "pt\""));
    });
    method(f, "set", [](CpuState&, GuestMemory&) {}, "v@:");            // AppKit text drawing passes fonts as attributes
    method(f, "setInContext:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(f, "glyphWithName:", [quartz, font](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? glyph_with_name(*font(s.x[0])->face, quartz->cf.utf8(s.x[2])) : 0;
    }, "I@:@");

    // NSFontDescriptor (the CTFontDescriptor class).
    const auto fd = ct.descriptor_class;
    class_method(fd, "fontDescriptorWithName:size:", [quartz, make_descriptor](CpuState& s, GuestMemory&) {
        const auto key = quartz->cf.string("NSFontNameAttribute"), size_key = quartz->cf.string("NSFontSizeAttribute");
        const auto size = quartz->cf.real(d(s, 0));
        std::vector<std::pair<Address, Address>> pairs{{key, s.x[2]}, {size_key, size}};
        const auto dictionary = quartz->cf.dictionary(pairs);
        quartz->release(key); quartz->release(size_key); quartz->release(size);
        s.x[0] = quartz->autorelease(make_descriptor(dictionary));
        quartz->release(dictionary);
    }, "@@:@d");
    class_method(fd, "fontDescriptorWithFontAttributes:", [quartz, make_descriptor](CpuState& s, GuestMemory&) {
        s.x[0] = quartz->autorelease(make_descriptor(s.x[2]));
    });
    class_method(fd, "preferredFontDescriptorForTextStyle:options:", [quartz, make_descriptor](CpuState& s, GuestMemory&) {
        const auto key = quartz->cf.string("NSFontNameAttribute"), name = quartz->cf.string(".AppleSystemUIFont");
        std::vector<std::pair<Address, Address>> pairs{{key, name}};
        const auto dictionary = quartz->cf.dictionary(pairs);
        quartz->release(key); quartz->release(name);
        s.x[0] = quartz->autorelease(make_descriptor(dictionary));
        quartz->release(dictionary);
    }, "@@:@@");
    method(fd, "fontAttributes", [descriptor_attributes](CpuState& s, GuestMemory&) { s.x[0] = descriptor_attributes(s.x[0]); });
    // Descriptors are equal when their attributes are (CFEqual, -isEqual:).
    method(fd, "isEqual:", [quartz](CpuState& s, GuestMemory&) {
        const auto self = quartz->find<DescriptorState>(s.x[0]), other = quartz->find<DescriptorState>(s.x[2]);
        return_bool(s, s.x[0] == s.x[2] || (self && other && quartz->cf.equal(self->attributes, other->attributes)));
    }, "B@:@");
    method(fd, "hash", [quartz, descriptor_attributes](CpuState& s, GuestMemory&) { s.x[0] = quartz->cf.hash(descriptor_attributes(s.x[0])); }, "Q@:");
    method(fd, "objectForKey:", [quartz, descriptor_attributes](CpuState& s, GuestMemory&) {
        Address found{};
        for (const auto& [key, value] : quartz->cf.dictionary_entries(descriptor_attributes(s.x[0]))) if (quartz->cf.equal(key, s.x[2])) found = value;
        s.x[0] = found;
    });
    method(fd, "pointSize", [quartz, descriptor_attributes](CpuState& s, GuestMemory&) {
        set_d(s, 0, quartz->number(quartz->dictionary_get(descriptor_attributes(s.x[0]), "NSFontSizeAttribute")).value_or(0));
    }, "d@:");
    method(fd, "postscriptName", [quartz, descriptor_attributes](CpuState& s, GuestMemory&) {
        s.x[0] = quartz->dictionary_get(descriptor_attributes(s.x[0]), "NSFontNameAttribute");
    });
    method(fd, "symbolicTraits", [quartz, descriptor_attributes](CpuState& s, GuestMemory&) {
        const auto traits = quartz->dictionary_get(descriptor_attributes(s.x[0]), "NSCTFontTraitsAttribute");
        s.x[0] = static_cast<Address>(quartz->number(quartz->dictionary_get(traits, "NSCTFontSymbolicTrait")).value_or(0));
    }, "I@:");
    method(fd, "fontDescriptorWithSymbolicTraits:", [quartz](CpuState& s, GuestMemory&) {
        const auto found = quartz->process.resolve(core_text_library, "_CTFontDescriptorCreateCopyWithSymbolicTraits");
        if (!found) fail("missing CTFontDescriptorCreateCopyWithSymbolicTraits");
        const std::array<Address, 3> arguments{s.x[0], s.x[2], 0xffffffff};
        s.x[0] = quartz->autorelease(quartz->process.calling_engine().invoke_callback(found->address, arguments));
    }, "@@:I");
    method(fd, "fontDescriptorWithSize:", [quartz, descriptor_attributes, make_descriptor, merged](CpuState& s, GuestMemory&) {
        const auto key = quartz->cf.string("NSFontSizeAttribute");
        const auto size = quartz->cf.real(d(s, 0));
        const auto combined = merged(descriptor_attributes(s.x[0]), {{key, size}});
        quartz->release(key); quartz->release(size);
        s.x[0] = quartz->autorelease(make_descriptor(combined));
        quartz->release(combined);
    }, "@@:d");
    method(fd, "copyWithZone:", [quartz](CpuState& s, GuestMemory&) { s.x[0] = quartz->retain(s.x[0]); }, "@@:^v");

    // NSFontManager: trait conversion and font lists.
    const auto fm = q.declare_class("NSFontManager", "NSObject", appkit_library);
    class_method(fm, "sharedFontManager", [quartz, fm](CpuState& s, GuestMemory&) {
        auto& manager = coretext().font_manager;
        if (!manager) {
            manager = quartz->objc.allocate(fm);
            if (const auto record = quartz->process.objects().find(manager)) record->immortal = true;
        }
        s.x[0] = manager;
    });
    // NSFontTraitMask: italic 1, bold 2, unbold 4, nonstandard 8, narrow 0x10,
    // expanded 0x20, condensed 0x40, smallcaps 0x80, poster 0x100,
    // compressed 0x200, fixed pitch 0x400, unitalic 0x1000000.
    const auto convert = [quartz, font](Address source, std::uint64_t trait, bool add) -> Address {
        const auto st = font(source);
        std::uint32_t traits{}, mask{};
        if (trait & 2) { mask |= font_trait::bold; if (add) traits |= font_trait::bold; }
        if (trait & 4) { mask |= font_trait::bold; if (!add) traits |= font_trait::bold; }
        if (trait & 1) { mask |= font_trait::italic; if (add) traits |= font_trait::italic; }
        if (trait & 0x1000000) { mask |= font_trait::italic; if (!add) traits |= font_trait::italic; }
        const auto resolved = mask ? with_traits(*st, traits, mask) : std::nullopt;
        return resolved ? quartz->autorelease(make_font(*quartz, *resolved, st->size, st->matrix)) : source;
    };
    method(fm, "convertFont:toHaveTrait:", [convert](CpuState& s, GuestMemory&) { s.x[0] = convert(s.x[2], s.x[3], true); }, "@@:@Q");
    method(fm, "convertFont:toNotHaveTrait:", [convert](CpuState& s, GuestMemory&) { s.x[0] = convert(s.x[2], s.x[3], false); }, "@@:@Q");
    method(fm, "convertFont:toSize:", [quartz, font, copy_font](CpuState& s, GuestMemory&) {
        const auto st = font(s.x[2]);
        s.x[0] = quartz->autorelease(copy_font(*st, d(s, 0), st->matrix));
    }, "@@:@d");
    method(fm, "convertFont:toFamily:", [quartz, font](CpuState& s, GuestMemory&) {
        const auto st = font(s.x[2]);
        const auto resolved = s.x[3] ? with_family(*st, quartz->cf.utf8(s.x[3])) : std::nullopt;
        s.x[0] = resolved ? quartz->autorelease(make_font(*quartz, *resolved, st->size, st->matrix)) : s.x[2];
    }, "@@:@@");
    method(fm, "traitsOfFont:", [font](CpuState& s, GuestMemory&) {
        const auto traits = font(s.x[2])->traits();
        s.x[0] = ((traits & font_trait::bold) ? 2 : 0) | ((traits & font_trait::italic) ? 1 : 0) | ((traits & font_trait::monospace) ? 0x400 : 0);
    }, "Q@:@");
    method(fm, "weightOfFont:", [font](CpuState& s, GuestMemory&) {
        // AppKit weights 0..15 (5 regular, 9 bold).
        s.x[0] = static_cast<Address>(std::clamp(static_cast<int>(font(s.x[2])->face->weight) / 100 + 1, 1, 14));
    }, "q@:@");
    method(fm, "fontWithFamily:traits:weight:size:", [quartz](CpuState& s, GuestMemory&) {
        // (family, traits, weight (0..15), size d0)
        const auto resolved = s.x[2] ? resolve_font(quartz->cf.utf8(s.x[2])) : std::nullopt;
        if (!resolved) { s.x[0] = 0; return; }
        const auto base = make_font(*quartz, *resolved, d(s, 0));
        auto state = quartz->find<FontState>(base);
        std::uint32_t traits{}, mask = font_trait::bold | font_trait::italic;
        if ((s.x[3] & 2) || static_cast<std::int64_t>(s.x[4]) >= 9) traits |= font_trait::bold;
        if (s.x[3] & 1) traits |= font_trait::italic;
        const auto styled = with_traits(*state, traits, mask);
        if (styled) { quartz->release(base); s.x[0] = quartz->autorelease(make_font(*quartz, *styled, d(s, 0))); }
        else s.x[0] = quartz->autorelease(base);
    }, "@@:@Qqd");
    method(fm, "availableFontFamilies", [quartz](CpuState& s, GuestMemory&) {
        std::vector<Address> names;
        for (const auto& name : available_families()) names.push_back(quartz->cf.string(name));
        s.x[0] = quartz->autorelease(quartz->cf.array(names));
        for (const auto name : names) quartz->release(name);
    });
    // Each member: [PostScript name, face name, weight (AppKit's 0..15, as
    // -weightOfFont:), traits (NSFontTraitMask)]; nil for an unknown family.
    method(fm, "availableMembersOfFontFamily:", [quartz](CpuState& s, GuestMemory&) {
        const auto members = s.x[2] ? family_members(quartz->cf.utf8(s.x[2])) : std::vector<std::shared_ptr<FontFace>>{};
        if (members.empty()) { s.x[0] = 0; return; }
        std::vector<Address> rows;
        for (const auto& face : members) {
            const auto weight = std::clamp(static_cast<int>(face->weight) / 100 + 1, 1, 14);
            std::uint64_t traits = 0;
            if (face->weight >= host::gpu::font_weight::semi_bold) traits |= 2;
            if (face->style != host::gpu::font_style::normal) traits |= 1;
            if (face->monospace) traits |= 0x400;
            const std::vector<Address> row{quartz->cf.string(face->postscript), quartz->cf.string(face->style_name),
                                           quartz->cf.integer(weight), quartz->cf.integer(static_cast<std::int64_t>(traits))};
            rows.push_back(quartz->cf.array(row));
            for (const auto value : row) quartz->release(value);
        }
        s.x[0] = quartz->autorelease(quartz->cf.array(rows));
        for (const auto row : rows) quartz->release(row);
    });
    method(fm, "availableFonts", [quartz](CpuState& s, GuestMemory&) {
        std::vector<Address> names;
        for (const auto& name : available_families()) names.push_back(quartz->cf.string(name));
        s.x[0] = quartz->autorelease(quartz->cf.array(names));
        for (const auto name : names) quartz->release(name);
    });
    static_cast<void>(std::numbers::pi);

    // CFEqual / CFHash of CTFonts and CTFontDescriptors (CoreFoundation's
    // extension point; the objects are CoreText's): the same answers as
    // -isEqual: / -hash. A font equals only a font, a descriptor only a
    // descriptor. Skia's typeface cache finds its CTFonts by CFEqual.
    q.cf.extend("CFEqual", "CoreText fonts and font descriptors", [quartz](CpuState& s, GuestMemory&) -> std::optional<std::uint64_t> {
        const auto left_font = quartz->find<FontState>(s.x[0]), right_font = quartz->find<FontState>(s.x[1]);
        if (left_font || right_font) return left_font && right_font && (s.x[0] == s.x[1] || same_font(*left_font, *right_font)) ? 1 : 0;
        const auto left = quartz->find<DescriptorState>(s.x[0]), right = quartz->find<DescriptorState>(s.x[1]);
        if (left || right)
            return left && right && (s.x[0] == s.x[1] || quartz->cf.equal(left->attributes, right->attributes)) ? 1 : 0;
        return std::nullopt;
    });
    q.cf.extend("CFHash", "CoreText fonts and font descriptors", [quartz](CpuState& s, GuestMemory&) -> std::optional<std::uint64_t> {
        if (const auto value = quartz->find<FontState>(s.x[0])) return font_hash(*value);
        if (const auto value = quartz->find<DescriptorState>(s.x[0])) return quartz->cf.hash(value->attributes);
        return std::nullopt;
    });
}

} // namespace alchemy::gui::quartz2d
