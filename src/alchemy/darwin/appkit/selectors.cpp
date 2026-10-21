#include "alchemy/darwin/appkit/drawing.hpp"
#include "alchemy/host/locale.hpp"
#include "alchemy/host/ui.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <algorithm>
#include <array>
#include <cctype>

// Selectors Chrome references on classes other providers implement, for the
// classes its start-up plausibly reaches (docs/ALCHEMY_CHROME_GAPS.md,
// "Selectors with no implementation"). Each is added only when the class
// does not implement it yet; every other unimplemented selector keeps
// failing explicitly ("unsupported Objective-C message").
// - NSWorkspace display accessibility flags follow the host: reduce motion is
//   Windows' "show animations" setting (SPI_GETCLIENTAREAANIMATION) turned
//   off, increase contrast is high-contrast mode; colors are never inverted
//   and transparency is never reduced (Windows has no such switches).
//   Application lookups by bundle identifier find the running application's
//   own bundle, nothing else (nil / empty).
// - NSScreen: SDR displays (extended dynamic range headroom 1.0).
// - NSLocale: canonical identifiers (separator and case normalization) and
//   the character direction of the right-to-left scripts' languages.
// - NSString localized case mapping uses the host's locale-aware mapping.
// - NSData enumerates its one contiguous range; NSDictionary sorts its keys
//   by value with the given comparison selector.
namespace alchemy::gui::values {
namespace {

using fill::appkit;
using fill::core_foundation;
using fill::foundation;
using fill::libobjc;

bool host_reduce_motion() { return alchemy::host::ui::reduce_motion().value_or(false); }

bool host_high_contrast() { return alchemy::host::ui::high_contrast().value_or(false); }

// Localized case mapping in the user's locale (host::locale linguistic
// casing); ASCII only where the host has no locale support.
std::string map_case(const std::string& text, bool upper) {
    namespace locale = alchemy::host::locale;
    const auto mapped = locale::map("", alchemy::gui::appkit::drawing::utf16(text),
                                    upper ? locale::Mapping::linguistic_upper : locale::Mapping::linguistic_lower);
    if (mapped) return alchemy::gui::appkit::drawing::utf8(*mapped);
    if (mapped.error() != alchemy::host::Error::unsupported) return {};
    std::string result = text;
    for (auto& c : result) c = static_cast<char>(upper ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// "en_us" / "EN-US" -> "en-US", "zh_hant_tw" -> "zh-Hant-TW".
std::string canonical_language(std::string text) {
    std::replace(text.begin(), text.end(), '_', '-');
    std::string result;
    std::size_t start = 0, part = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find('-', start), text.size());
        auto piece = text.substr(start, end - start);
        if (part == 0) for (auto& c : piece) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (piece.size() == 4) {
            for (auto& c : piece) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!piece.empty()) piece[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(piece[0])));
        } else if (piece.size() == 2 || piece.size() == 3) {
            const bool digits = std::all_of(piece.begin(), piece.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
            if (!digits) for (auto& c : piece) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (!result.empty()) result += '-';
        result += piece;
        if (end == text.size()) break;
        start = end + 1;
        ++part;
    }
    return result;
}

} // namespace

void Provider::install_selectors() {
    const auto extend = [this](std::string_view name, std::string_view selector, transmute::GuestCallHandler handler,
                               bool class_method = false, std::string types = "@@:") {
        this->extend(name, selector, std::move(handler), class_method, std::move(types));
    };
    const auto yes_no = [](bool value) { return [value](CpuState& s, GuestMemory&) { s.x[0] = value; }; };

    // ---- NSWorkspace ----
    extend("NSWorkspace", "accessibilityDisplayShouldReduceMotion", [](CpuState& s, GuestMemory&) { s.x[0] = host_reduce_motion(); }, false, "B@:");
    extend("NSWorkspace", "accessibilityDisplayShouldIncreaseContrast", [](CpuState& s, GuestMemory&) { s.x[0] = host_high_contrast(); }, false, "B@:");
    extend("NSWorkspace", "accessibilityDisplayShouldInvertColors", yes_no(false), false, "B@:");
    extend("NSWorkspace", "accessibilityDisplayShouldReduceTransparency", yes_no(false), false, "B@:");
    extend("NSWorkspace", "accessibilityDisplayShouldDifferentiateWithoutColor", yes_no(false), false, "B@:");
    // The only application bundle LaunchServices knows is the running one:
    // its own identifier names its bundle, any other none (nil / empty).
    const auto own_bundle_url = [this](Address identifier) -> Address {
        if (!identifier || !bridge->is_string(identifier)) return 0;
        const auto bundle = call(core_foundation, "_CFBundleGetMainBundle", {});
        const auto mine = bundle ? call(core_foundation, "_CFBundleGetIdentifier", {bundle}) : 0;
        if (!mine || !cf.equal(identifier, mine)) return 0;
        return bridge->autorelease(call(core_foundation, "_CFBundleCopyBundleURL", {bundle}));
    };
    extend("NSWorkspace", "URLForApplicationWithBundleIdentifier:", [own_bundle_url](CpuState& s, GuestMemory&) { s.x[0] = own_bundle_url(s.x[2]); });
    extend("NSWorkspace", "URLsForApplicationsWithBundleIdentifier:", [this, own_bundle_url](CpuState& s, GuestMemory&) {
        const auto url = own_bundle_url(s.x[2]);
        s.x[0] = bridge->autorelease(url ? cf.array(std::array{url}) : cf.array({}));
    });
    for (const auto* selector : {"URLForApplicationToOpenContentType:", "URLForApplicationToOpenURL:"})
        extend("NSWorkspace", selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    extend("NSWorkspace", "noteFileSystemChanged:", [](CpuState&, GuestMemory&) {}, false, "v@:@");   // no Finder to tell
    // -isFilePackageAtPath: a directory whose extension is a package type.
    extend("NSWorkspace", "isFilePackageAtPath:", [this](CpuState& s, GuestMemory&) {
        const auto path = s.x[2] && bridge->is_string(s.x[2]) ? cf.utf8(s.x[2]) : std::string();
        auto extension = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.') + 1);
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static constexpr std::array packages{"app", "bundle", "framework", "plugin", "kext", "pkg", "mpkg", "xpc", "appex", "rtfd", "qlgenerator"};
        bool package = std::find(packages.begin(), packages.end(), extension) != packages.end();
        if (package) {
            const auto url = cf.file_url(path, true);
            package = cf.url_is_directory(url);
            release(url);
        }
        s.x[0] = package;
    }, false, "B@:@");

    // ---- NSScreen: standard dynamic range ----
    extend("NSScreen", "maximumExtendedDynamicRangeColorComponentValue", [](CpuState& s, GuestMemory&) { fill::return_real(s, 1.0); }, false, "d@:");
    extend("NSScreen", "maximumPotentialExtendedDynamicRangeColorComponentValue", [](CpuState& s, GuestMemory&) { fill::return_real(s, 1.0); }, false, "d@:");
    extend("NSScreen", "maximumReferenceExtendedDynamicRangeColorComponentValue", [](CpuState& s, GuestMemory&) { fill::return_real(s, 0.0); }, false, "d@:");

    // ---- NSLocale ----
    extend("NSLocale", "canonicalLanguageIdentifierFromString:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] ? bridge->autorelease(cf.string(canonical_language(cf.utf8(s.x[2])))) : 0;
    }, true);
    extend("NSLocale", "canonicalLocaleIdentifierFromString:", [this](CpuState& s, GuestMemory&) {
        auto value = s.x[2] ? canonical_language(cf.utf8(s.x[2])) : std::string();
        std::replace(value.begin(), value.end(), '-', '_');
        s.x[0] = s.x[2] ? bridge->autorelease(cf.string(value)) : 0;
    }, true);
    // NSLocaleLanguageDirection: LeftToRight 1, RightToLeft 2.
    extend("NSLocale", "characterDirectionForLanguage:", [this](CpuState& s, GuestMemory&) {
        auto language = s.x[2] ? canonical_language(cf.utf8(s.x[2])) : std::string();
        language = language.substr(0, language.find('-'));
        static constexpr std::array rtl{"ar", "he", "iw", "fa", "ur", "yi", "ji", "ps", "dv", "ug", "sd", "ckb", "syr", "arc"};
        s.x[0] = std::find(rtl.begin(), rtl.end(), language) != rtl.end() ? 2 : 1;
    }, true, "Q@:@");

    // ---- NSString ----
    extend("NSString", "localizedLowercaseString", [this](CpuState& s, GuestMemory&) {
        s.x[0] = bridge->autorelease(cf.string(map_case(cf.utf8(s.x[0]), false)));
    });
    extend("NSString", "localizedUppercaseString", [this](CpuState& s, GuestMemory&) {
        s.x[0] = bridge->autorelease(cf.string(map_case(cf.utf8(s.x[0]), true)));
    });

    // ---- NSData: one contiguous range ----
    extend("NSData", "enumerateByteRangesUsingBlock:", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return;
        const auto length = call(core_foundation, "_CFDataGetLength", {s.x[0]});
        const auto bytes = call(core_foundation, "_CFDataGetBytePtr", {s.x[0]});
        const auto stop = process.allocate_guest(8);
        m.write8(stop, 0);
        // block(const void *bytes, NSRange byteRange, BOOL *stop): the range takes x2, x3.
        static_cast<void>(bridge->invoke_block(s.x[2], {bytes, 0, length, stop}));
        process.release_guest(stop);
    }, false, "v@:@?");

    // ---- NSDictionary: keys sorted by value ----
    extend("NSDictionary", "keysSortedByValueUsingSelector:", [this](CpuState& s, GuestMemory&) {
        auto entries = cf.dictionary_entries(s.x[0]);
        const auto comparator = s.x[2];
        std::stable_sort(entries.begin(), entries.end(), [&](const auto& left, const auto& right) {
            CallRegs registers{};
            registers.x[2] = right.second;
            return static_cast<std::int64_t>(objc.send(left.second, comparator, registers).x0) < 0;   // NSOrderedAscending
        });
        std::vector<Address> keys;
        for (const auto& [key, value] : entries) keys.push_back(key);
        s.x[0] = bridge->autorelease(cf.array(keys));
    });

    // ---- NSBundle: a localization-specific resource ----
    extend("NSBundle", "pathForResource:ofType:inDirectory:forLocalization:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], name = s.x[2], type = s.x[3], directory = s.x[4], localization = s.x[5];
        CallRegs registers{};
        if (localization) {
            // <directory>/<localization>.lproj first, then the unlocalized lookup.
            auto sub = cf.utf8(localization) + ".lproj";
            if (directory) sub = cf.utf8(directory) + "/" + sub;
            const auto text = cf.string(sub);
            registers.x = {0, 0, name, type, text};
            const auto found = objc.send(self, "pathForResource:ofType:inDirectory:", registers).x0;
            release(text);
            if (found) { s.x[0] = found; return; }
        }
        registers.x = {0, 0, name, type, directory};
        s.x[0] = objc.send(self, "pathForResource:ofType:inDirectory:", registers).x0;
    });
}

} // namespace alchemy::gui::values
