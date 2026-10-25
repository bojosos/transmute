// CoreFoundation additions (cf_plist.cpp, cf_extra.cpp, cf_preferences.cpp,
// cf_locale.cpp): the property list core against reference plists, and the
// guest-visible exports as a renderer plug-in's images call them. Split out of the
// export-fill tests (the fixture is export_fill_test_kit.hpp: the gap-fill rows, installed as run_program does).
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"
#include "alchemy/darwin/corefoundation/cf_plist.hpp"

namespace {

void test_plist() {
    vd::Plist root{vd::Plist::Kind::dictionary};
    vd::Plist text; text.text = "Scene <&> \xc3\xa9\xe2\x82\xac";
    vd::Plist number{vd::Plist::Kind::integer}; number.integer = -42;
    vd::Plist big{vd::Plist::Kind::integer}; big.integer = 1LL << 40;
    vd::Plist real{vd::Plist::Kind::real}; real.real = 2.5;
    vd::Plist flag{vd::Plist::Kind::boolean}; flag.boolean = true;
    vd::Plist when{vd::Plist::Kind::date}; when.real = 812345678.0;
    vd::Plist blob{vd::Plist::Kind::data}; blob.bytes = {0, 1, 2, 250, 251, 252, 253};
    vd::Plist list{vd::Plist::Kind::array}; list.items = {text, number, flag};
    root.set("text", text); root.set("number", number); root.set("big", big); root.set("real", real);
    root.set("flag", flag); root.set("when", when); root.set("blob", blob); root.set("list", list);
    const auto same = [](const vd::Plist& a, const vd::Plist& b, auto& self) -> bool {
        if (a.kind != b.kind) return false;
        switch (a.kind) {
        case vd::Plist::Kind::string: return a.text == b.text;
        case vd::Plist::Kind::integer: return a.integer == b.integer;
        case vd::Plist::Kind::real: case vd::Plist::Kind::date: return a.real == b.real;
        case vd::Plist::Kind::boolean: return a.boolean == b.boolean;
        case vd::Plist::Kind::data: return a.bytes == b.bytes;
        case vd::Plist::Kind::array:
            if (a.items.size() != b.items.size()) return false;
            for (std::size_t i = 0; i < a.items.size(); ++i) if (!self(a.items[i], b.items[i], self)) return false;
            return true;
        case vd::Plist::Kind::dictionary:
            if (a.entries.size() != b.entries.size()) return false;
            for (const auto& [key, value] : a.entries) { const auto* other = b.find(key); if (!other || !self(value, *other, self)) return false; }
            return true;
        }
        return false;
    };
    const auto xml = vd::plist_to_xml(root);
    require(xml.find("<string>Scene &lt;&amp;&gt; ") != std::string::npos, "XML string escaping");
    require(xml.find("<date>2026-09-29T03:34:38Z</date>") != std::string::npos, "XML date");
    std::uint64_t format{};
    const auto from_xml = vd::plist_parse(std::span(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()), &format);
    require(from_xml && format == vd::plist_xml_format && same(root, *from_xml, same), "XML round trip");
    const auto binary = vd::plist_to_binary(root);
    const auto from_binary = vd::plist_parse(binary, &format);
    require(from_binary && format == vd::plist_binary_format && same(root, *from_binary, same), "binary round trip");
    // plistlib.dumps({'a': [1, 'x'], 'b': True}, fmt=plistlib.FMT_BINARY)
    const std::uint8_t reference[] = {0x62,0x70,0x6c,0x69,0x73,0x74,0x30,0x30,0xd2,0x01,0x02,0x03,0x06,0x51,0x61,0x51,0x62,0xa2,
        0x04,0x05,0x10,0x01,0x51,0x78,0x09,0x08,0x0d,0x0f,0x11,0x14,0x16,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x19};
    const auto parsed = vd::plist_parse(reference);
    require(parsed && parsed->find("b") && parsed->find("b")->boolean && parsed->find("a") && parsed->find("a")->items.size() == 2 &&
            parsed->find("a")->items[0].integer == 1 && parsed->find("a")->items[1].text == "x", "reference binary plist");
    const std::string broken = "<plist><dict><key>a</key></plist>";
    require(!vd::plist_parse(std::span(reinterpret_cast<const std::uint8_t*>(broken.data()), broken.size())), "malformed XML accepted");
}

void test_guest_cf_extra(Fixture& f) {
    const auto cf = alchemy::core_foundation_library;
    const auto system = "/usr/lib/libSystem.B.dylib";
    // Constants.
    {
        const auto since1970 = f.engine.read64(f.symbol(cf, "_kCFAbsoluteTimeIntervalSince1970"));
        require(std::bit_cast<double>(since1970) == 978307200.0, "kCFAbsoluteTimeIntervalSince1970");
        require(f.cf.utf8(f.engine.read64(f.symbol(cf, "_kCFPreferencesCurrentUser"))) == "kCFPreferencesCurrentUser", "kCFPreferencesCurrentUser");
        require(f.cf.utf8(f.engine.read64(f.symbol("/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices", "_kUTTypeTIFF"))) ==
                "public.tiff", "kUTTypeTIFF");
    }
    // NoCopy: the default deallocator frees the caller's malloc'd bytes.
    {
        const auto bytes = f.sys("_malloc", {5});
        f.engine.write_bytes(bytes, std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'});
        const auto data = f.call(cf, "_CFDataCreateWithBytesNoCopy", {0, bytes, 5, 0});
        require(f.call(cf, "_CFDataGetLength", {data}) == 5 && f.read(f.call(cf, "_CFDataGetBytePtr", {data})).substr(0, 5) == "hello", "CFDataCreateWithBytesNoCopy");
        const auto null_allocator = f.engine.read64(f.symbol(cf, "_kCFAllocatorNull"));
        const auto kept = f.text("kept");
        const auto string = f.call(cf, "_CFStringCreateWithCStringNoCopy", {0, kept, 0x08000100, null_allocator});
        require(f.cf.utf8(string) == "kept" && f.read(kept) == "kept", "CFStringCreateWithCStringNoCopy with kCFAllocatorNull");
        require(f.cf.utf8(f.call(cf, "_CFStringCreateWithFileSystemRepresentation", {0, f.text("/tmp/a b")})) == "/tmp/a b", "file system representation");
    }
    // CFArrayAppendArray with a range.
    {
        const std::array<std::uint64_t, 3> values{f.cf.string("a"), f.cf.string("b"), f.cf.string("c")};
        const auto source = f.cf.array(values);
        const auto target = f.cf.array({}, true);
        f.call(cf, "_CFArrayAppendArray", {target, source, 1, 2});
        require(f.cf.length(target) == 2 && f.cf.utf8(f.cf.element(target, 0)) == "b" && f.cf.utf8(f.cf.element(target, 1)) == "c", "CFArrayAppendArray");
    }
    // CFUUID bytes round trip (16-byte struct in x0/x1 and as arguments x1/x2).
    {
        alchemy::CallRegs make;
        make.x[1] = 0x0706050403020100ULL;
        make.x[2] = 0x0f0e0d0c0b0a0908ULL;
        const auto uuid = alchemy::engine_call_regs(f.engine, f.symbol(cf, "_CFUUIDCreateFromUUIDBytes"), make).x0;
        require(f.cf.utf8(f.call(cf, "_CFUUIDCreateString", {0, uuid})) == "00010203-0405-0607-0809-0A0B0C0D0E0F", "CFUUIDCreateFromUUIDBytes");
        alchemy::CallRegs get;
        get.x[0] = uuid;
        const auto bytes = alchemy::engine_call_regs(f.engine, f.symbol(cf, "_CFUUIDGetUUIDBytes"), get);
        require(!bytes.complete || (bytes.x0 == 0x0706050403020100ULL && bytes.x1 == 0x0f0e0d0c0b0a0908ULL), "CFUUIDGetUUIDBytes");
    }
    // CFBundle over a bundle directory.
    {
        const auto root = std::filesystem::temp_directory_path() / "alchemy_export_fill_bundle.app";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root / "Contents" / "Resources" / "sub");
        {
            std::ofstream info(root / "Contents" / "Info.plist", std::ios::binary);
            info << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
                    "<key>CFBundleIdentifier</key><string>com.example.alchemytest</string>"
                    "<key>CFBundleExecutable</key><string>tool</string>"
                    "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>\n";
            std::ofstream resource(root / "Contents" / "Resources" / "sub" / "data.json", std::ios::binary);
            resource << "{}";
        }
        const auto url = f.cf.file_url(alchemy::guest_path_string(root), true);
        const auto bundle = f.call(cf, "_CFBundleCreate", {0, url});
        require(bundle != 0 && f.cf.utf8(f.call(cf, "_CFBundleGetIdentifier", {bundle})) == "com.example.alchemytest", "CFBundleCreate/GetIdentifier");
        const auto resource = f.call(cf, "_CFBundleCopyResourceURL", {bundle, f.cf.string("data"), f.cf.string("json"), f.cf.string("sub")});
        require(resource && f.cf.url_path(resource).ends_with("/Contents/Resources/sub/data.json"), "CFBundleCopyResourceURL");
        require(f.cf.url_path(f.call(cf, "_CFBundleCopyExecutableURL", {bundle})).ends_with("/Contents/MacOS/tool"), "CFBundleCopyExecutableURL");
        const auto info = f.call(cf, "_CFBundleCopyInfoDictionaryForURL", {url});
        require(info && f.cf.utf8(f.cf.dictionary_value(info, f.cf.string("CFBundleExecutable"))) == "tool", "CFBundleCopyInfoDictionaryForURL");
        const auto type = f.process.allocate_guest(4), creator = f.process.allocate_guest(4);
        require(f.call(cf, "_CFBundleGetPackageInfoInDirectory", {url, type, creator}) == 1 && f.engine.read32(type) == 0x4150504cU &&
                f.engine.read32(creator) == 0x3f3f3f3fU, "CFBundleGetPackageInfoInDirectory from Info.plist");
        require(f.call(cf, "_CFBundleGetMainBundle") != 0, "CFBundleGetMainBundle");
        // Volume capacities through the resource properties.
        const std::array<std::uint64_t, 2> keys{f.engine.read64(f.symbol(cf, "_kCFURLVolumeTotalCapacityKey")),
                                                f.engine.read64(f.symbol(cf, "_kCFURLVolumeAvailableCapacityKey"))};
        const auto properties = f.call(cf, "_CFURLCopyResourcePropertiesForKeys", {url, f.cf.array(keys), 0});
        const auto total = f.cf.number_value(f.cf.dictionary_value(properties, keys[0]));
        const auto available = f.cf.number_value(f.cf.dictionary_value(properties, keys[1]));
        require(total && available && *total > 0 && *available <= *total, "volume capacities");
        require(f.call(cf, "_CFURLCreateBookmarkDataFromFile", {0, url, 0}) == 0, "no bookmark data on the host");
        std::filesystem::remove_all(root, ignored);
    }
    static_cast<void>(system);
}

// cf_system.cpp: the system version dictionary and NSGetSizeAndAlignment.
void test_guest_cf_system(Fixture& f) {
    const auto cf = alchemy::core_foundation_library;
    const auto key = f.engine.read64(f.symbol(cf, "__kCFSystemVersionProductVersionKey"));
    require(f.cf.utf8(key) == "ProductVersion" &&
            f.cf.utf8(f.engine.read64(f.symbol(cf, "__kCFSystemVersionBuildVersionKey"))) == "ProductBuildVersion", "kCFSystemVersion keys");
    const auto versions = f.call(cf, "__CFCopySystemVersionDictionary", {});
    require(f.cf.utf8(f.cf.dictionary_value(versions, key)) == "14.0" &&
            f.cf.utf8(f.cf.dictionary_value(versions, f.cf.string("ProductName"))) == "macOS", "_CFCopySystemVersionDictionary");
    const auto size = f.sys("_malloc", {16});
    const auto layout = [&](std::string_view type) {
        const auto text = f.text(std::string(type));
        const auto end = f.call(cf, "_NSGetSizeAndAlignment", {text, size, size + 8});
        return std::tuple{f.engine.read64(size), f.engine.read64(size + 8), end - text};
    };
    require(layout("q") == std::tuple{8ULL, 8ULL, 1ULL} && layout("c") == std::tuple{1ULL, 1ULL, 1ULL}, "scalars");
    require(layout("{CGRect={CGPoint=dd}{CGSize=dd}}") == std::tuple{32ULL, 8ULL, 32ULL}, "nested structure");
    require(layout("{?=cid}") == std::tuple{16ULL, 8ULL, 7ULL} && layout("(?=ciq)") == std::tuple{8ULL, 8ULL, 7ULL}, "padding and union");
    require(layout("[3s]") == std::tuple{6ULL, 2ULL, 4ULL} && layout("^{Opaque}") == std::tuple{8ULL, 8ULL, 9ULL}, "array and pointer");
    require(layout("r*16@0:8") == std::tuple{8ULL, 8ULL, 4ULL} && layout("@\"NSString\"") == std::tuple{8ULL, 8ULL, 11ULL} &&
            layout("@?") == std::tuple{8ULL, 8ULL, 2ULL}, "qualifier, frame offset, class name, block");
}

void test_guest_preferences(Fixture& f) {
    const auto cf = alchemy::core_foundation_library;
    // Property list data through the guest ABI.
    {
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> entries{{{f.cf.string("name"), f.cf.string("Scene")},
                                                                               {f.cf.string("count"), f.cf.integer(7)}}};
        const auto dictionary = f.cf.dictionary(entries);
        for (const std::uint64_t format : {100ULL, 200ULL}) {
            const auto data = f.call(cf, "_CFPropertyListCreateData", {0, dictionary, format, 0, 0});
            require(data != 0, "CFPropertyListCreateData");
            const auto format_out = f.process.allocate_guest(8);
            const auto copy = f.call(cf, "_CFPropertyListCreateWithData", {0, data, 0, format_out, 0});
            require(copy && f.engine.read64(format_out) == format && f.cf.utf8(f.cf.dictionary_value(copy, f.cf.string("name"))) == "Scene" &&
                    f.cf.number_value(f.cf.dictionary_value(copy, f.cf.string("count"))) == 7.0, "CFPropertyListCreateWithData");
        }
    }
    // CFPreferences persisted under the guest home.
    const auto home = std::filesystem::temp_directory_path() / "alchemy_export_fill_home";
    std::error_code ignored;
    std::filesystem::remove_all(home, ignored);
    std::filesystem::create_directories(home);
    f.process.environment.push_back("HOME=" + alchemy::guest_path_string(home));
    const auto app = f.cf.string("com.example.alchemytest");
    const auto user = f.engine.read64(f.symbol(cf, "_kCFPreferencesCurrentUser"));
    const auto any_host = f.engine.read64(f.symbol(cf, "_kCFPreferencesAnyHost"));
    f.call(cf, "_CFPreferencesSetValue", {f.cf.string("RenderMode"), f.cf.string("GPU"), app, user, any_host});
    require(f.call(cf, "_CFPreferencesSynchronize", {app, user, any_host}) == 1, "CFPreferencesSynchronize");
    require(std::filesystem::exists(home / "Library" / "Preferences" / "com.example.alchemytest.plist"), "preferences file");
    require(f.cf.utf8(f.call(cf, "_CFPreferencesCopyAppValue", {f.cf.string("RenderMode"), app})) == "GPU", "CFPreferencesCopyAppValue");
    const auto keys = f.call(cf, "_CFPreferencesCopyKeyList", {app, user, any_host});
    require(keys && f.cf.length(keys) == 1, "CFPreferencesCopyKeyList");
    const auto exists = f.process.allocate_guest(1);
    f.call(cf, "_CFPreferencesSetValue", {f.cf.string("Enabled"), f.cf.string("YES"), app, user, any_host});
    require(f.call(cf, "_CFPreferencesGetAppBooleanValue", {f.cf.string("Enabled"), app, exists}) == 1 && f.byte(exists) == 1,
            "CFPreferencesGetAppBooleanValue");
    f.call(cf, "_CFPreferencesSetValue", {f.cf.string("Enabled"), 0, app, user, any_host});
    require(f.call(cf, "_CFPreferencesGetAppBooleanValue", {f.cf.string("Enabled"), app, exists}) == 0 && f.byte(exists) == 0, "removed key");
    // NSUserDefaults over the same store.
    {
        const auto defaults_class = f.objc.find_class("NSUserDefaults");
        require(defaults_class != 0, "NSUserDefaults class");
        const auto send = [&](std::uint64_t receiver, std::string_view selector, std::initializer_list<std::uint64_t> arguments = {}) {
            alchemy::CallRegs registers;
            std::size_t index = 2;
            for (const auto argument : arguments) registers.x[index++] = argument;
            return f.objc.send(receiver, selector, registers);
        };
        const auto suite = send(f.objc.allocate(defaults_class), "initWithSuiteName:", {app}).x0;
        require(f.cf.utf8(send(suite, "objectForKey:", {f.cf.string("RenderMode")}).x0) == "GPU", "NSUserDefaults reads CFPreferences");
        send(suite, "setInteger:forKey:", {12, f.cf.string("Threads")});
        send(suite, "setBool:forKey:", {1, f.cf.string("Denoise")});
        require(send(suite, "integerForKey:", {f.cf.string("Threads")}).x0 == 12 && send(suite, "boolForKey:", {f.cf.string("Denoise")}).x0 == 1,
                "NSUserDefaults typed values");
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> registered{{{f.cf.string("Fallback"), f.cf.integer(5)}}};
        send(suite, "registerDefaults:", {f.cf.dictionary(registered)});
        require(send(suite, "integerForKey:", {f.cf.string("Fallback")}).x0 == 5, "registration domain");
        require(send(suite, "synchronize").x0 == 1, "NSUserDefaults synchronize");
        const auto fresh = send(f.objc.allocate(defaults_class), "initWithSuiteName:", {app}).x0;
        require(send(fresh, "integerForKey:", {f.cf.string("Threads")}).x0 == 12 && send(fresh, "integerForKey:", {f.cf.string("Fallback")}).x0 == 0,
                "persisted defaults");
        require(send(fresh, "persistentDomainForName:", {f.cf.string("com.example.absent")}).x0 == 0, "an absent domain is nil");
        const auto dock = send(fresh, "persistentDomainForName:", {f.cf.string("com.apple.dock")}).x0;
        require(dock != 0 && f.call(cf, "_CFDictionaryGetCount", {dock}) == 0, "the Dock's domain reads as empty");
    }
    std::filesystem::remove_all(home, ignored);
}

void test_guest_locale(Fixture& f) {
    const auto cf = alchemy::core_foundation_library;
    const auto us = f.call(cf, "_CFLocaleCreate", {0, f.cf.string("en_US")});
    const auto key = [&](const char* name) { return f.engine.read64(f.symbol(cf, name)); };
    // Date formatter patterns and symbols.
    const auto short_date = f.call(cf, "_CFDateFormatterCreate", {0, us, 1, 0});
    require(f.cf.utf8(f.call(cf, "_CFDateFormatterGetFormat", {short_date})) == "M/d/y", "en_US short date pattern");
    const auto months = f.call(cf, "_CFDateFormatterCopyProperty", {short_date, key("_kCFDateFormatterMonthSymbols")});
    require(months && f.cf.length(months) == 12 && f.cf.utf8(f.cf.element(months, 0)) == "January", "month symbols");
    const auto days = f.call(cf, "_CFDateFormatterCopyProperty", {short_date, key("_kCFDateFormatterShortWeekdaySymbols")});
    require(days && f.cf.length(days) == 7 && f.cf.utf8(f.cf.element(days, 0)) == "Sun", "short weekday symbols start on Sunday");
    require(f.cf.utf8(f.call(cf, "_CFDateFormatterCopyProperty", {short_date, key("_kCFDateFormatterPMSymbol")})) == "PM", "PM symbol");
    f.call(cf, "_CFDateFormatterSetFormat", {short_date, f.cf.string("y MMMM 'at' HH")});
    const auto text = f.cf.utf8(f.call(cf, "_CFDateFormatterCreateStringWithDate", {0, short_date, f.cf.date(15552000.0)}));
    require(text.starts_with("2001 June at "), "formatted date: " + text);
    // Number formatter.
    const auto decimal = f.call(cf, "_CFNumberFormatterCreate", {0, us, 1});
    require(f.cf.utf8(f.call(cf, "_CFNumberFormatterCreateStringWithNumber", {0, decimal, f.cf.real(1234.5)})) == "1,234.5", "decimal style");
    const auto currency = f.call(cf, "_CFNumberFormatterCreate", {0, us, 2});
    f.call(cf, "_CFNumberFormatterSetProperty", {currency, key("_kCFNumberFormatterCurrencySymbol"), f.cf.string("EUR ")});
    const auto value = f.process.allocate_guest(8);
    f.engine.write64(value, std::bit_cast<std::uint64_t>(1234.5));
    require(f.cf.utf8(f.call(cf, "_CFNumberFormatterCreateStringWithValue", {0, currency, 6, value})) == "EUR 1,234.50", "currency style");
    const auto percent = f.call(cf, "_CFNumberFormatterCreate", {0, us, 3});
    require(f.cf.utf8(f.call(cf, "_CFNumberFormatterCreateStringWithNumber", {0, percent, f.cf.real(0.25)})) == "25%", "percent style");
    // Calendar and display names.
    const auto calendar = f.call(cf, "_CFCalendarCopyCurrent");
    const auto weekday = f.call(cf, "_CFCalendarGetFirstWeekday", {calendar});
    require(weekday >= 1 && weekday <= 7, "first weekday");
    const auto german = f.call(cf, "_CFLocaleCopyDisplayNameForPropertyValue", {us, key("_kCFLocaleIdentifier"), f.cf.string("de_DE")});
    require(german && f.cf.utf8(german) == "German (Germany)", "display name of de_DE in English");
    // Locale lists (from the host's locales).
    const auto contains = [&f](std::uint64_t array, std::string_view value) {
        for (std::uint64_t index = 0; index < f.cf.length(array); ++index) if (f.cf.utf8(f.cf.element(array, index)) == value) return true;
        return false;
    };
    const auto sorted_unique = [&f](std::uint64_t array) {
        for (std::uint64_t index = 1; index < f.cf.length(array); ++index)
            if (!(f.cf.utf8(f.cf.element(array, index - 1)) < f.cf.utf8(f.cf.element(array, index)))) return false;
        return true;
    };
    const auto identifiers = f.call(cf, "_CFLocaleCopyAvailableLocaleIdentifiers");
    require(identifiers && contains(identifiers, "en_US") && contains(identifiers, "zh_Hans") && contains(identifiers, "de") &&
            !contains(identifiers, "en-US"), "CFLocaleCopyAvailableLocaleIdentifiers: CF identifiers");
    const auto languages = f.call(cf, "_CFLocaleCopyISOLanguageCodes");
    require(languages && sorted_unique(languages) && contains(languages, "en") && contains(languages, "ja") && contains(languages, "haw") &&
            !contains(languages, "en_US") && !contains(languages, "Hans"), "CFLocaleCopyISOLanguageCodes: ISO 639 codes");
    const auto countries = f.call(cf, "_CFLocaleCopyISOCountryCodes");
    require(countries && sorted_unique(countries) && contains(countries, "US") && contains(countries, "JP") && contains(countries, "CN") &&
            !contains(countries, "419") && !contains(countries, "Hans"), "CFLocaleCopyISOCountryCodes: ISO 3166 codes");
    for (const auto* name : {"_CFLocaleCopyISOCurrencyCodes", "_CFLocaleCopyCommonISOCurrencyCodes"}) {
        const auto currencies = f.call(cf, name);
        require(currencies && sorted_unique(currencies) && contains(currencies, "USD") && contains(currencies, "EUR") && contains(currencies, "JPY"),
                std::string(name) + ": ISO 4217 codes");
    }
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("plist"); test_plist();
        stage("fixture"); Fixture f;
        stage("guest cf extra"); test_guest_cf_extra(f);
        stage("guest cf system"); test_guest_cf_system(f);
        stage("guest preferences"); test_guest_preferences(f);
        stage("guest locale"); test_guest_locale(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "corefoundation additions tests passed\n";
    return 0;
}
