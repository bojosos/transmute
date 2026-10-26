// Foundation core-class additions over the full provider set (fill_fixture.hpp):
// strings_more.cpp, collections_more.cpp, foundation_more.cpp, the
// NSPropertyListSerialization stream forms and NSUserDefaults' volatile domains
// and URLs. NSRange results come back in x0/x1; this engine shows x0 only.
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <chrono>
#include <filesystem>
#include <vector>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr Address not_found = 0x7fffffffffffffffULL;
bool yes(Address value) { return (value & 0xff) != 0; }

Address str(Fixture& f, std::string_view value) { return f.cf.string(value); }
std::string text(Fixture& f, Address string) { return string ? f.cf.utf8(string) : std::string("(nil)"); }
Address u16(Fixture& f, std::initializer_list<std::uint16_t> units) { return f.cf.string_utf16(std::vector<std::uint16_t>(units)); }
Address array(Fixture& f, std::initializer_list<Address> values) {
    const auto result = f.call(cf, "_CFArrayCreateMutable", {0, 0, f.symbol(cf, "_kCFTypeArrayCallBacks")});
    for (const auto value : values) static_cast<void>(f.call(cf, "_CFArrayAppendValue", {result, value}));
    return result;
}
std::filesystem::path scratch_directory() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("alchemy-core-classes-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    return directory;
}

void strings(Fixture& f) {
    const auto ns_string = f.cls("NSString");
    const auto list = f.send(ns_string, "availableStringEncodings");
    bool utf8 = false, shift_jis = false;
    std::size_t count = 0;
    for (Address at = list; count < 1000; at += 8, ++count) {
        const auto encoding = f.engine.read64(at);
        if (!encoding) break;
        utf8 = utf8 || encoding == 4;
        shift_jis = shift_jis || encoding == 8;
    }
    require(count < 1000 && utf8 && shift_jis, "+availableStringEncodings (0-terminated NSStringEncodings)");
    require(f.send(ns_string, "defaultCStringEncoding") == 30, "+defaultCStringEncoding is Mac OS Roman");
    require(text(f, f.send(ns_string, "localizedNameOfStringEncoding:", {4})) == "Unicode (UTF-8)", "+localizedNameOfStringEncoding:");
    const auto cafe = u16(f, {'c', 'a', 'f', 0x00e9});
    require(!yes(f.send(cafe, "canBeConvertedToEncoding:", {1})) && yes(f.send(cafe, "canBeConvertedToEncoding:", {4})), "-canBeConvertedToEncoding:");
    require(f.send(str(f, "abc"), "fastestEncoding") == 1 && f.send(cafe, "smallestEncoding") == 30, "fastest and smallest encodings");
    const auto buffer = f.block(16);
    static_cast<void>(f.send(str(f, "hey"), "getCharacters:", {buffer}));
    require(f.memory().read8(buffer) == 'h' && f.memory().read8(buffer + 4) == 'y', "-getCharacters:");
    require(f.send(u16(f, {'x', 'e', 0x0301, 'y'}), "rangeOfComposedCharacterSequencesForRange:", {2, 1}) == 1, "composed sequences widen a range");
    const auto prefix = text(f, f.send(str(f, "Hello World"), "commonPrefixWithString:options:", {str(f, "Help"), 0}));
    require(prefix == "Hel", "-commonPrefixWithString:options: " + prefix);
    require(text(f, f.send(str(f, "HELlo"), "commonPrefixWithString:options:", {str(f, "hello"), 1})) == "HELlo", "case-insensitive common prefix");
    require(text(f, f.send(u16(f, {'C', 'a', 'f', 0x00e9}), "stringByFoldingWithOptions:locale:", {1 | 128, 0})) == "cafe",
            "-stringByFoldingWithOptions:locale:");
    const auto to_latin = f.engine.read64(f.symbol(cf, "_kCFStringTransformToLatin"));
    require(text(f, f.send(u16(f, {0x041f, 0x0440, 0x0438}), "stringByApplyingTransform:reverse:", {to_latin, 0})) == "Pri",
            "-stringByApplyingTransform:reverse:");
    const auto turkish = f.call(cf, "_CFLocaleCreate", {0, str(f, "tr_TR")});
    require(f.cf.units(f.send(str(f, "I"), "lowercaseStringWithLocale:", {turkish})) == std::vector<std::uint16_t>{0x0131}, "Turkish dotless i");
    require(text(f, f.send(u16(f, {0xfb01}), "decomposedStringWithCompatibilityMapping")) == "fi", "compatibility decomposition");
    const auto lines = u16(f, {'a', 0x0a, 'b', 'c', 0x0a, 'd'});
    require(f.send(lines, "lineRangeForRange:", {3, 0}) == 2, "-lineRangeForRange:");
    const auto bounds = f.block(24);
    static_cast<void>(f.send(lines, "getLineStart:end:contentsEnd:forRange:", {bounds, bounds + 8, bounds + 16, 3, 0}));
    require(f.engine.read64(bounds) == 2 && f.engine.read64(bounds + 8) == 5 && f.engine.read64(bounds + 16) == 4, "-getLineStart:end:contentsEnd:forRange:");
    require(text(f, f.send(str(f, "a%20b"), "stringByRemovingPercentEncoding")) == "a b", "-stringByRemovingPercentEncoding");
    require(text(f, f.send(str(f, "a b"), "stringByAddingPercentEscapesUsingEncoding:", {4})) == "a%20b", "-stringByAddingPercentEscapesUsingEncoding:");
    const auto home = text(f, f.call(foundation, "_NSHomeDirectory"));
    require(text(f, f.send(str(f, home + "/notes.txt"), "stringByAbbreviatingWithTildeInPath")) == "~/notes.txt", "-stringByAbbreviatingWithTildeInPath");
    const auto appended = f.send(str(f, "/tmp"), "stringsByAppendingPaths:", {array(f, {str(f, "a"), str(f, "b")})});
    require(f.send(appended, "count") == 2 && text(f, f.send(appended, "objectAtIndex:", {1})) == "/tmp/b", "-stringsByAppendingPaths:");
    require(yes(f.send(ns_string, "supportsSecureCoding")), "+supportsSecureCoding");
}

void collections(Fixture& f) {
    const auto a = str(f, "a"), b = str(f, "b"), c = str(f, "c");
    const auto list = array(f, {a, b, c, b});
    require(f.send(list, "indexOfObject:inRange:", {str(f, "b"), 2, 2}) == 3, "-indexOfObject:inRange:");
    require(text(f, f.send(list, "firstObjectCommonWithArray:", {array(f, {str(f, "z"), str(f, "c")})})) == "c", "-firstObjectCommonWithArray:");
    const auto paths = array(f, {str(f, "x.png"), str(f, "y.txt"), str(f, "z.png")});
    require(f.send(f.send(paths, "pathsMatchingExtensions:", {array(f, {str(f, "png")})}), "count") == 2, "-pathsMatchingExtensions:");
    const auto directory = scratch_directory();
    const auto file = (directory / "list.plist").generic_string();
    require(yes(f.send(list, "writeToFile:atomically:", {str(f, file), 1})), "-writeToFile:atomically:");
    const auto url = f.send(f.cls("NSURL"), "fileURLWithPath:", {str(f, file)});
    const auto read = f.send(f.cls("NSArray"), "arrayWithContentsOfURL:error:", {url, 0});
    require(read && f.send(read, "count") == 4 && text(f, f.send(read, "objectAtIndex:", {2})) == "c", "+arrayWithContentsOfURL:error:");
    const auto edited = array(f, {a, b, c, b});
    static_cast<void>(f.send(edited, "removeObjectsInArray:", {array(f, {str(f, "b")})}));
    require(f.send(edited, "count") == 2, "-removeObjectsInArray:");
    static_cast<void>(f.send(edited, "exchangeObjectAtIndex:withObjectAtIndex:", {0, 1}));
    require(text(f, f.send(edited, "objectAtIndex:", {0})) == "c", "-exchangeObjectAtIndex:withObjectAtIndex:");
    static_cast<void>(f.send(edited, "replaceObjectsInRange:withObjectsFromArray:", {0, 1, array(f, {str(f, "x"), str(f, "y")})}));
    require(f.send(edited, "count") == 3 && text(f, f.send(edited, "objectAtIndex:", {1})) == "y", "-replaceObjectsInRange:withObjectsFromArray:");

    const auto dictionary = f.call(cf, "_CFDictionaryCreateMutable",
        {0, 0, f.symbol(cf, "_kCFTypeDictionaryKeyCallBacks"), f.symbol(cf, "_kCFTypeDictionaryValueCallBacks")});
    static_cast<void>(f.send(dictionary, "setObject:forKey:", {str(f, "1"), str(f, "one")}));
    const auto marker = str(f, "?");
    const auto values = f.send(dictionary, "objectsForKeys:notFoundMarker:", {array(f, {str(f, "one"), str(f, "two")}), marker});
    require(text(f, f.send(values, "objectAtIndex:", {0})) == "1" && text(f, f.send(values, "objectAtIndex:", {1})) == "?", "-objectsForKeys:notFoundMarker:");
    require(text(f, f.send(dictionary, "descriptionInStringsFileFormat")).find("= \"1\";") != std::string::npos, "-descriptionInStringsFileFormat");
    const auto replacement = f.call(cf, "_CFDictionaryCreateMutable",
        {0, 0, f.symbol(cf, "_kCFTypeDictionaryKeyCallBacks"), f.symbol(cf, "_kCFTypeDictionaryValueCallBacks")});
    static_cast<void>(f.send(replacement, "setObject:forKey:", {str(f, "2"), str(f, "two")}));
    static_cast<void>(f.send(dictionary, "setDictionary:", {replacement}));
    require(f.send(dictionary, "count") == 1 && f.send(dictionary, "objectForKey:", {str(f, "one")}) == 0, "-setDictionary:");

    const auto counted = f.send(f.send(f.cls("NSCountedSet"), "alloc"), "init");
    for (const auto value : {str(f, "x"), str(f, "x"), str(f, "y")}) static_cast<void>(f.send(counted, "addObject:", {value}));
    require(f.send(counted, "count") == 2 && f.send(counted, "countForObject:", {str(f, "x")}) == 2, "NSCountedSet counts");
    static_cast<void>(f.send(counted, "removeObject:", {str(f, "x")}));
    require(f.send(counted, "countForObject:", {str(f, "x")}) == 1 && f.send(counted, "count") == 2, "one removal leaves the member");

    const auto data_class = f.cls("NSData");
    require(f.send(f.send(data_class, "data"), "length") == 0, "+[NSData data]");
    const auto haystack = f.send(str(f, "abcabc"), "dataUsingEncoding:", {4});
    const auto needle = f.send(str(f, "bc"), "dataUsingEncoding:", {4});
    require(f.send(haystack, "rangeOfData:options:range:", {needle, 0, 0, 6}) == 1 && f.send(haystack, "rangeOfData:options:range:", {needle, 1, 0, 6}) == 4 &&
                f.send(haystack, "rangeOfData:options:range:", {needle, 2, 0, 6}) == not_found, "-rangeOfData:options:range:");
    const auto bytes = f.send(haystack, "mutableCopy");
    static_cast<void>(f.send(bytes, "resetBytesInRange:", {1, 2}));
    require(f.memory().read8(f.send(bytes, "bytes") + 1) == 0 && f.memory().read8(f.send(bytes, "bytes") + 3) == 'a', "-resetBytesInRange:");
    static_cast<void>(f.send(bytes, "setData:", {needle}));
    require(f.send(bytes, "length") == 2, "-setData:");

    const auto indexes = f.send(f.send(f.cls("NSMutableIndexSet"), "alloc"), "init");
    for (const Address index : {2, 3, 4, 9}) static_cast<void>(f.send(indexes, "addIndex:", {index}));
    require(f.send(indexes, "indexGreaterThanOrEqualToIndex:", {5}) == 9 && f.send(indexes, "indexLessThanOrEqualToIndex:", {8}) == 4 &&
                yes(f.send(indexes, "containsIndexesInRange:", {2, 3})) && !yes(f.send(indexes, "containsIndexesInRange:", {2, 4})) &&
                yes(f.send(indexes, "intersectsIndexesInRange:", {5, 5})), "index set queries");
    static_cast<void>(f.send(indexes, "shiftIndexesStartingAtIndex:by:", {3, 2}));
    require(yes(f.send(indexes, "containsIndex:", {2})) && yes(f.send(indexes, "containsIndex:", {5})) && yes(f.send(indexes, "containsIndex:", {11})) &&
                !yes(f.send(indexes, "containsIndex:", {3})), "-shiftIndexesStartingAtIndex:by:");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void foundation_more(Fixture& f) {
    const auto info = f.send(f.cls("NSProcessInfo"), "processInfo");
    require(!text(f, f.send(info, "userName")).empty() && !yes(f.send(info, "isMacCatalystApp")) && f.send(info, "operatingSystem") == 5,
            "NSProcessInfo additions");
    const auto manager = f.send(f.cls("NSFileManager"), "defaultManager");
    const auto directory = scratch_directory();
    const auto source = (directory / "a.txt").generic_string(), target = (directory / "b.txt").generic_string();
    static_cast<void>(f.send(str(f, "data"), "writeToFile:atomically:encoding:error:", {str(f, source), 1, 4, 0}));
    const auto url_class = f.cls("NSURL");
    require(yes(f.send(manager, "copyItemAtURL:toURL:error:", {f.send(url_class, "fileURLWithPath:", {str(f, source)}),
                                                              f.send(url_class, "fileURLWithPath:", {str(f, target)}), 0})) &&
                std::filesystem::exists(directory / "b.txt"), "-copyItemAtURL:toURL:error:");
    const auto attributes = f.send(manager, "attributesOfFileSystemForPath:error:", {str(f, directory.generic_string()), 0});
    require(attributes && f.send(attributes, "objectForKey:", {str(f, "NSFileSystemSize")}) != 0, "-attributesOfFileSystemForPath:error:");
    const auto error = f.block(8);
    require(!yes(f.send(manager, "setUbiquitous:itemAtURL:destinationURL:error:", {1, 0, 0, error})) && f.engine.read64(error) != 0,
            "no iCloud: -setUbiquitous: fails with an error");
    // NSPropertyListSerialization through streams.
    const auto plist = f.cls("NSPropertyListSerialization");
    const auto output = f.send(f.cls("NSOutputStream"), "outputStreamToMemory");
    static_cast<void>(f.send(output, "open"));
    const auto value = array(f, {str(f, "p"), str(f, "q")});
    require(static_cast<std::int64_t>(f.send(plist, "writePropertyList:toStream:format:options:error:", {value, output, 100, 0, 0})) > 0,
            "+writePropertyList:toStream:...");
    const auto written = f.send(output, "propertyForKey:", {f.engine.read64(f.symbol(foundation, "_NSStreamDataWrittenToMemoryStreamKey"))});
    const auto input = f.send(f.cls("NSInputStream"), "inputStreamWithData:", {written});
    static_cast<void>(f.send(input, "open"));
    const auto back = f.send(plist, "propertyListWithStream:options:format:error:", {input, 0, 0, 0});
    require(back && f.send(back, "count") == 2 && text(f, f.send(back, "objectAtIndex:", {1})) == "q", "+propertyListWithStream:...");
    const auto description = f.block(8);
    require(f.send(plist, "dataFromPropertyList:format:errorDescription:", {value, 200, description}) != 0, "+dataFromPropertyList:format:errorDescription:");
    // Small ones.
    const auto note = f.send(f.send(f.cls("NSNotification"), "alloc"), "initWithName:object:userInfo:", {str(f, "N"), 0, 0});
    require(text(f, f.send(note, "name")) == "N", "-[NSNotification initWithName:object:userInfo:]");
    const auto uuid_class = f.cls("NSUUID");
    const auto low = f.send(f.send(uuid_class, "alloc"), "initWithUUIDString:", {str(f, "00000000-0000-0000-0000-000000000001")});
    const auto high = f.send(f.send(uuid_class, "alloc"), "initWithUUIDString:", {str(f, "00000000-0000-0000-0000-000000000002")});
    require(static_cast<std::int64_t>(f.send(low, "compare:", {high})) == -1 && f.send(high, "compare:", {high}) == 0, "-[NSUUID compare:]");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void user_defaults(Fixture& f) {
    const auto defaults = f.send(f.send(f.cls("NSUserDefaults"), "alloc"), "initWithSuiteName:", {str(f, "org.alchemy.test.coreclasses")});
    const auto domain = f.call(cf, "_CFDictionaryCreateMutable",
        {0, 0, f.symbol(cf, "_kCFTypeDictionaryKeyCallBacks"), f.symbol(cf, "_kCFTypeDictionaryValueCallBacks")});
    static_cast<void>(f.send(domain, "setObject:forKey:", {str(f, "v"), str(f, "k")}));
    static_cast<void>(f.send(defaults, "setVolatileDomain:forName:", {domain, str(f, "Test")}));
    const auto names = f.send(defaults, "volatileDomainNames");
    require(f.send(names, "count") == 3, "-volatileDomainNames");
    require(text(f, f.send(f.send(defaults, "volatileDomainForName:", {str(f, "Test")}), "objectForKey:", {str(f, "k")})) == "v", "-volatileDomainForName:");
    static_cast<void>(f.send(defaults, "removeVolatileDomainForName:", {str(f, "Test")}));
    require(f.send(defaults, "volatileDomainForName:", {str(f, "Test")}) == 0, "-removeVolatileDomainForName:");
    const auto url = f.send(f.cls("NSURL"), "URLWithString:", {str(f, "https://example.com/a")});
    static_cast<void>(f.send(defaults, "setURL:forKey:", {url, str(f, "link")}));
    require(text(f, f.send(f.send(defaults, "URLForKey:", {str(f, "link")}), "host")) == "example.com", "-setURL:forKey: and -URLForKey:");
    static_cast<void>(f.send(defaults, "setObject:forKey:", {str(f, "/Volumes/C/lib"), str(f, "path")}));
    require(text(f, f.send(f.send(defaults, "URLForKey:", {str(f, "path")}), "path")) == "/Volumes/C/lib", "a path reads as a file URL");
    static_cast<void>(f.send(defaults, "removeObjectForKey:", {str(f, "link")}));
    static_cast<void>(f.send(defaults, "removeObjectForKey:", {str(f, "path")}));
}

} // namespace

int main() {
    return run("foundation core classes", {{"strings", strings}, {"collections", collections}, {"foundation more", foundation_more},
        {"user defaults", user_defaults}});
}
