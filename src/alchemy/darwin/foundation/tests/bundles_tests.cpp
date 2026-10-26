// Bundles over the full provider set (fill_fixture.hpp): NSBundle's
// localizations, .strings tables, localized resources, binary and OpenStep
// property lists, and the CFBundle calls over them (foundation_extra.cpp,
// bundles.cpp, corefoundation/cf_extra.cpp), on bundles written to a scratch
// directory. Loading executables is alchemy_gui_foundation_bundle_fixture's.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/corefoundation/cf_plist.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace fill_test;
namespace fs = std::filesystem;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

std::string text(Fixture& f, Address string) { return string ? f.cf.utf8(string) : std::string("(nil)"); }
Address str(Fixture& f, std::string_view value) { return f.cf.string(value); }
bool yes(Address value) { return (value & 0xff) != 0; }

void write(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
void write(const fs::path& path, std::string_view value) { write(path, std::vector<std::uint8_t>(value.begin(), value.end())); }
// UTF-16LE with a byte-order mark, as Xcode writes .strings files.
std::vector<std::uint8_t> utf16(std::string_view ascii) {
    std::vector<std::uint8_t> bytes{0xff, 0xfe};
    for (const auto c : ascii) { bytes.push_back(static_cast<std::uint8_t>(c)); bytes.push_back(0); }
    return bytes;
}
std::string info_plist(std::string_view identifier) {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>") +
           "<key>CFBundleIdentifier</key><string>" + std::string(identifier) + "</string>" +
           "<key>CFBundleDevelopmentRegion</key><string>en</string><key>CFBundleName</key><string>Raw</string></dict></plist>";
}
fs::path scratch() {
    const auto directory = fs::temp_directory_path() / ("alchemy-bundles-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    return directory;
}
std::vector<std::string> names(Fixture& f, Address array) {
    std::vector<std::string> values;
    if (array) for (const auto value : f.cf.array_values(array)) values.push_back(f.cf.utf8(value));
    return values;
}

void openstep(Fixture&) {
    using alchemy::export_fill::plist_parse;
    const std::string text_plist = "/* comment */ { list = (1, \"two\", ); data = <0a ff>; nested = { k = v; }; // line\n }";
    std::uint64_t format{};
    const auto value = plist_parse(std::span(reinterpret_cast<const std::uint8_t*>(text_plist.data()), text_plist.size()), &format);
    require(value && format == alchemy::export_fill::plist_openstep_format && value->kind == alchemy::export_fill::Plist::Kind::dictionary,
            "an OpenStep dictionary");
    const auto* list = value->find("list");
    require(list && list->items.size() == 2 && list->items[1].text == "two", "an OpenStep array with a trailing comma");
    const auto* data = value->find("data");
    require(data && data->bytes == std::vector<std::uint8_t>{0x0a, 0xff}, "OpenStep data");
    // A strings file: no braces, \U escapes (UTF-16 units), octal, "key"; meaning key = key.
    const std::string strings = "\"a\" = \"x\\U00e9\\n\";\n\"b\" = \"\\101\";\n\"same\";\n";
    const auto table = plist_parse(std::span(reinterpret_cast<const std::uint8_t*>(strings.data()), strings.size()));
    require(table && table->find("a") && table->find("a")->text == "x\xc3\xa9\n", "a strings file with a \\U escape");
    require(table->find("b") && table->find("b")->text == "A" && table->find("same") && table->find("same")->text == "same", "octal and key-only entries");
    const auto wide = utf16("\"k\" = \"v\";");
    const auto from_utf16 = plist_parse(wide);
    require(from_utf16 && from_utf16->find("k") && from_utf16->find("k")->text == "v", "a UTF-16LE strings file");
    require(!plist_parse(std::span(reinterpret_cast<const std::uint8_t*>("{ a = ; }"), 9)), "malformed OpenStep text");
}

void localization(Fixture& f) {
    const auto root = scratch();
    // Two localizations; Localizable.strings in UTF-16, InfoPlist.strings in UTF-8.
    const auto app = root / "Two.app";
    write(app / "Contents/Info.plist", info_plist("org.alchemy.two"));
    write(app / "Contents/Resources/en.lproj/Localizable.strings", utf16("\"greeting\" = \"Hello\";"));
    write(app / "Contents/Resources/de.lproj/Localizable.strings", utf16("\"greeting\" = \"Hallo\";"));
    write(app / "Contents/Resources/de.lproj/only.txt", "de");
    write(app / "Contents/Resources/image.png", "png");
    write(app / "Contents/Resources/sub/data.json", "{}");
    write(app / "Contents/Resources/sub/more.json", "{}");
    // One localization (the development region), for answers that do not depend on the user's languages.
    const auto solo = root / "Solo.app";
    write(solo / "Contents/Info.plist", info_plist("org.alchemy.solo"));
    write(solo / "Contents/Resources/en.lproj/Localizable.strings", "/* UTF-8 */ \"greeting\" = \"Hello\"; \"empty\" = \"\";");
    write(solo / "Contents/Resources/en.lproj/Menu.strings", "\"open\" = \"Open...\";");
    write(solo / "Contents/Resources/en.lproj/InfoPlist.strings", "CFBundleName = \"Localized\";");
    write(solo / "Contents/Resources/en.lproj/note.txt", "en");

    const auto bundle_class = f.cls("NSBundle");
    const auto two = f.send(bundle_class, "bundleWithPath:", {str(f, app.generic_string())});
    require(two && f.send(bundle_class, "bundleWithPath:", {str(f, app.generic_string())}) == two, "one NSBundle per path");
    require(names(f, f.send(two, "localizations")) == std::vector<std::string>{"de", "en"}, "-localizations");
    require(text(f, f.send(two, "developmentLocalization")) == "en", "-developmentLocalization");
    const auto available = f.cf.array(std::vector<Address>{str(f, "en"), str(f, "de")});
    const auto preferences = [&](std::initializer_list<const char*> values) {
        std::vector<Address> items;
        for (const auto* value : values) items.push_back(str(f, value));
        return f.cf.array(items);
    };
    const auto choose = [&](Address from, Address prefs) { return names(f, f.send(bundle_class, "preferredLocalizationsFromArray:forPreferences:", {from, prefs})); };
    require(choose(available, preferences({"de-AT", "en"})) == std::vector<std::string>{"de"}, "de-AT prefers de");
    require(choose(available, preferences({"fr"})) == std::vector<std::string>{"en"}, "no match: the development region");
    const auto legacy = f.cf.array(std::vector<Address>{str(f, "English"), str(f, "German")});
    require(choose(legacy, preferences({"de"})) == std::vector<std::string>{"German"}, "legacy localization names");
    // Localized resources by localization.
    const auto only = f.send(two, "URLForResource:withExtension:subdirectory:localization:", {str(f, "only"), str(f, "txt"), 0, str(f, "de")});
    require(only && text(f, f.send(only, "path")).ends_with("de.lproj/only.txt"), "-URLForResource:...localization:");
    require(f.send(two, "URLForResource:withExtension:subdirectory:localization:", {str(f, "only"), str(f, "txt"), 0, str(f, "en")}) == 0,
            "not in another localization");
    require(text(f, f.send(two, "pathForResource:ofType:", {str(f, "image"), str(f, "png")})).ends_with("Resources/image.png"), "unlocalized resource");
    require(f.cf.length(f.send(two, "URLsForResourcesWithExtension:subdirectory:", {str(f, "json"), str(f, "sub")})) == 2,
            "-URLsForResourcesWithExtension:subdirectory:");
    require(f.cf.length(f.send(two, "pathsForResourcesOfType:inDirectory:", {str(f, "png"), 0})) == 1, "-pathsForResourcesOfType:inDirectory:");

    // Strings: the development region's tables.
    const auto one = f.send(bundle_class, "bundleWithPath:", {str(f, solo.generic_string())});
    require(names(f, f.send(one, "preferredLocalizations")) == std::vector<std::string>{"en"}, "the only localization is preferred");
    require(text(f, f.send(one, "localizedStringForKey:value:table:", {str(f, "greeting"), 0, 0})) == "Hello", "Localizable.strings");
    require(text(f, f.send(one, "localizedStringForKey:value:table:", {str(f, "open"), 0, str(f, "Menu")})) == "Open...", "a named table");
    require(text(f, f.send(one, "localizedStringForKey:value:table:", {str(f, "missing"), str(f, "Fallback"), 0})) == "Fallback", "the value");
    require(text(f, f.send(one, "localizedStringForKey:value:table:", {str(f, "missing"), 0, 0})) == "missing", "the key");
    require(text(f, f.send(one, "localizedStringForKey:value:table:", {str(f, "empty"), str(f, "x"), 0})).empty(), "an empty translation");
    require(text(f, f.send(one, "objectForInfoDictionaryKey:", {str(f, "CFBundleName")})) == "Localized", "InfoPlist.strings overrides");
    require(text(f, f.call(cf, "_CFDictionaryGetValue", {f.send(one, "infoDictionary"), str(f, "CFBundleName")})) == "Raw", "the raw Info.plist");
    require(text(f, f.send(one, "pathForResource:ofType:", {str(f, "note"), str(f, "txt")})).ends_with("en.lproj/note.txt"), "a localized resource");

    // The CFBundle calls.
    require(names(f, f.call(cf, "_CFBundleCopyBundleLocalizations", {two})) == std::vector<std::string>{"de", "en"}, "CFBundleCopyBundleLocalizations");
    require(text(f, f.call(cf, "_CFBundleGetDevelopmentRegion", {two})) == "en", "CFBundleGetDevelopmentRegion");
    require(text(f, f.call(cf, "_CFBundleCopyLocalizedString", {one, str(f, "greeting"), 0, 0})) == "Hello", "CFBundleCopyLocalizedString");
    const auto local = f.call(cf, "_CFBundleGetLocalInfoDictionary", {one});
    require(local && text(f, f.call(cf, "_CFDictionaryGetValue", {local, str(f, "CFBundleName")})) == "Localized", "CFBundleGetLocalInfoDictionary");
    const auto de_url = f.call(cf, "_CFBundleCopyResourceURLForLocalization", {two, str(f, "only"), str(f, "txt"), 0, str(f, "de")});
    require(de_url != 0, "CFBundleCopyResourceURLForLocalization");
    require(f.cf.length(f.call(cf, "_CFBundleCopyResourceURLsOfType", {two, str(f, "json"), str(f, "sub")})) == 2, "CFBundleCopyResourceURLsOfType");
    const auto apps = f.call(cf, "_CFBundleCreateBundlesFromDirectory", {0, f.cf.file_url(root.generic_string(), true), str(f, "app")});
    require(f.cf.length(apps) == 2, "CFBundleCreateBundlesFromDirectory");
    require(names(f, f.call(cf, "_CFBundleCopyLocalizationsForPreferences", {available, preferences({"de"})})) == std::vector<std::string>{"de"},
            "CFBundleCopyLocalizationsForPreferences");
    // No executable: nothing loads, and the error says why.
    require(!yes(f.send(one, "load")) && !yes(f.send(one, "isLoaded")), "no executable to load");
    const auto error = f.block(8);
    require(!yes(f.send(one, "loadAndReturnError:", {error})) && f.memory().read64(error) &&
                f.send(f.memory().read64(error), "code") == 4, "-loadAndReturnError: NSFileNoSuchFileError");
    require(f.send(one, "executableArchitectures") == 0, "no architectures without an executable");
    fs::remove_all(root);
}

// This process has no guest executable: the main bundle has no path, and so no resources or localizations.
void pathless_main_bundle(Fixture& f) {
    const auto main = f.send(f.cls("NSBundle"), "mainBundle");
    require(main != 0, "+mainBundle");
    if (!text(f, f.send(main, "bundlePath")).empty()) return;   // a fixture with an executable: nothing to check
    require(f.send(main, "pathForResource:ofType:", {str(f, "Basso"), str(f, "aiff")}) == 0, "no resource");
    require(f.cf.length(f.send(main, "localizations")) == 0, "no localizations");
    require(f.cf.length(f.send(main, "pathsForResourcesOfType:inDirectory:", {str(f, "png"), 0})) == 0, "no resource list");
    require(text(f, f.send(main, "localizedStringForKey:value:table:", {str(f, "key"), 0, 0})) == "key", "the key as its own string");
}

void binary_info(Fixture& f) {
    const auto root = scratch();
    alchemy::export_fill::Plist info;
    info.kind = alchemy::export_fill::Plist::Kind::dictionary;
    alchemy::export_fill::Plist identifier;
    identifier.text = "org.alchemy.binary";
    info.set("CFBundleIdentifier", identifier);
    write(root / "Binary.app/Contents/Info.plist", alchemy::export_fill::plist_to_binary(info));
    const auto bundle = f.send(f.cls("NSBundle"), "bundleWithPath:", {str(f, (root / "Binary.app").generic_string())});
    require(text(f, f.send(bundle, "bundleIdentifier")) == "org.alchemy.binary", "a binary Info.plist");
    fs::remove_all(root);
}

} // namespace

int main() {
    return run("foundation bundles", {{"OpenStep property lists", openstep}, {"localizations and strings", localization}, {"binary Info.plist", binary_info},
                                        {"a main bundle without a path", pathless_main_bundle}});
}
