#include "alchemy/darwin/kit/completion/data/providers.hpp"

#include <algorithm>

// More NSString / NSMutableString (added where missing), over CoreFoundation's
// string calls (darwin/corefoundation/cf_strings.cpp, cf_urls.cpp):
//   formats        +localizedStringWithFormat:, -initWithFormat:locale:[arguments:]
//                  (the format engine's; the locale does not change it)
//   encodings      +availableStringEncodings, +defaultCStringEncoding (Mac OS
//                  Roman), +localizedNameOfStringEncoding:, -canBeConvertedToEncoding:,
//                  -fastestEncoding, -smallestEncoding,
//                  +stringWithContentsOfFile/URL:usedEncoding:error: (a byte-order
//                  mark, else UTF-8, else Mac OS Roman)
//   characters     -getCharacters:, -initWithCharactersNoCopy:length:freeWhenDone:
//                  (copied, then freed), -rangeOfComposedCharacterSequencesForRange:,
//                  -commonPrefixWithString:options:, -rangeOfString:options:range:locale:
//   transforms     -stringByFoldingWithOptions:locale:, -stringByApplyingTransform:reverse:,
//                  -lowercase/uppercaseStringWithLocale:, the compatibility mappings,
//                  -[NSMutableString applyTransform:reverse:range:updatedRange:]
//   lines          -enumerateLinesUsingBlock:, -getLineStart:end:contentsEnd:forRange:,
//                  -lineRangeForRange:, and the paragraph versions
//   escapes        -stringByRemovingPercentEncoding, -stringByAddingPercentEscapesUsingEncoding:,
//                  -stringByReplacingPercentEscapesUsingEncoding:
//   paths          -stringByAbbreviatingWithTildeInPath, -stringsByAppendingPaths:
//   property lists -propertyList, -propertyListFromStringsFileFormat
//   other          -mutableCopyWithZone:, +supportsSecureCoding, -variantFittingPresentationWidth:
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::string_view cf_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::uint64_t ns_utf8 = 4, ns_mac_roman = 30;

struct More {
    Support& s;
    Guest g{s};

    Address cf(std::string_view symbol, std::initializer_list<Address> arguments) { return s.call(cf_library, symbol, arguments); }
    std::uint32_t cf_encoding(Address ns_encoding) { return static_cast<std::uint32_t>(cf("_CFStringConvertNSStringEncodingToEncoding", {ns_encoding})); }
    Address length(Address string) { return s.send_id(string, "length"); }
    // A +1 mutable copy, edited by `edit`, returned autoreleased (or nil when edit says so).
    template <class Edit>
    Address edited(Address string, Edit edit) {
        const auto copy = s.send_id(string, "mutableCopy");
        if (!edit(copy)) { g.release(copy); return 0; }
        return g.autorelease(copy);
    }
    Address read_file(Address path_or_url, bool url, Address encoding_out, Address error_out) {
        const auto data_class = s.objc.find_class("NSData");
        const auto data = url ? s.send_id(data_class, "dataWithContentsOfURL:options:error:", {path_or_url, 0, error_out})
                              : s.send_id(data_class, "dataWithContentsOfFile:options:error:", {path_or_url, 0, error_out});
        if (!data) return 0;
        const auto bytes = s.send_id(data, "bytes");
        const auto count = s.send_id(data, "length");
        std::array<std::uint8_t, 4> head{};
        if (count) s.memory().read_bytes(bytes, std::span(head.data(), std::min<std::size_t>(4, static_cast<std::size_t>(count))));
        // A byte-order mark names the encoding; otherwise UTF-8, then Mac OS Roman.
        std::vector<std::uint64_t> candidates;
        if (count >= 4 && head[0] == 0xff && head[1] == 0xfe && head[2] == 0 && head[3] == 0) candidates = {0x9c000100};   // UTF-32LE
        else if (count >= 4 && head[0] == 0 && head[1] == 0 && head[2] == 0xfe && head[3] == 0xff) candidates = {0x98000100};
        else if (count >= 2 && ((head[0] == 0xff && head[1] == 0xfe) || (head[0] == 0xfe && head[1] == 0xff))) candidates = {10};   // NSUnicodeStringEncoding
        else candidates = {ns_utf8, ns_mac_roman};
        for (const auto encoding : candidates) {
            const auto text = s.send_id(s.send_id(s.objc.find_class("NSString"), "alloc"), "initWithData:encoding:", {data, encoding});
            if (!text) continue;
            if (encoding_out) s.memory().write64(encoding_out, encoding);
            if (error_out) s.memory().write64(error_out, 0);
            return g.autorelease(text);
        }
        g.set_error(error_out, g.error("NSCocoaErrorDomain", 264, "The file couldn't be opened because the text encoding of its contents can't be determined."));
        return 0;
    }
    // CFStringGetLineBounds / GetParagraphBounds of [location, location + length).
    std::array<Address, 3> bounds(Address string, Address location, Address count, bool lines) {
        const auto out = s.process.allocate_guest(24);
        static_cast<void>(cf(lines ? "_CFStringGetLineBounds" : "_CFStringGetParagraphBounds", {string, location, count, out, out + 8, out + 16}));
        const std::array<Address, 3> result{s.memory().read64(out), s.memory().read64(out + 8), s.memory().read64(out + 16)};
        s.process.release_guest(out);
        return result;
    }

    void install();
};

void More::install() {
    const auto string = s.objc.find_class("NSString"), mutable_string = s.objc.find_class("NSMutableString");
    if (!string || !mutable_string) fail("NSString is not registered");

    // ---- formats ----
    fill_method(s, string, "localizedStringWithFormat:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = g.autorelease(s.send_id(s.send_id(state.x[0], "alloc"), "initWithFormat:arguments:", {state.x[2], state.sp}));
    }, "@@:@", true);
    fill_method(s, string, "initWithFormat:locale:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = s.send_id(state.x[0], "initWithFormat:arguments:", {state.x[2], state.sp});
    }, "@@:@@");
    fill_method(s, string, "initWithFormat:locale:arguments:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = s.send_id(state.x[0], "initWithFormat:arguments:", {state.x[2], state.x[4]});
    }, "@@:@@^v");

    // ---- encodings ----
    fill_method(s, string, "availableStringEncodings", [this](CpuState& state, GuestMemory&) {
        static Address list = 0;
        if (!list) {
            std::vector<std::uint64_t> encodings;
            for (Address at = cf("_CFStringGetListOfAvailableEncodings", {});; at += 4) {
                const auto encoding = s.memory().read32(at);
                if (encoding == 0xffffffffu) break;
                encodings.push_back(cf("_CFStringConvertEncodingToNSStringEncoding", {encoding}));
            }
            encodings.push_back(0);
            list = s.process.allocate_guest(encodings.size() * 8);
            for (std::size_t index = 0; index < encodings.size(); ++index) s.memory().write64(list + index * 8, encodings[index]);
        }
        state.x[0] = list;
    }, "r^Q@:", true);
    fill_method(s, string, "defaultCStringEncoding", [](CpuState& state, GuestMemory&) { state.x[0] = ns_mac_roman; }, "Q@:", true);
    fill_method(s, string, "localizedNameOfStringEncoding:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = cf("_CFStringGetNameOfEncoding", {cf_encoding(state.x[2])});
    }, "@@:Q", true);
    fill_method(s, string, "canBeConvertedToEncoding:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = s.send_id(state.x[0], "dataUsingEncoding:allowLossyConversion:", {state.x[2], 0}) != 0;
    }, "B@:Q");
    fill_method(s, string, "fastestEncoding", [this](CpuState& state, GuestMemory&) {
        state.x[0] = cf("_CFStringConvertEncodingToNSStringEncoding", {cf("_CFStringGetFastestEncoding", {state.x[0]})});
    }, "Q@:");
    fill_method(s, string, "smallestEncoding", [this](CpuState& state, GuestMemory&) {
        state.x[0] = cf("_CFStringConvertEncodingToNSStringEncoding", {cf("_CFStringGetSmallestEncoding", {state.x[0]})});
    }, "Q@:");
    fill_method(s, string, "stringWithContentsOfFile:usedEncoding:error:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = read_file(state.x[2], false, state.x[3], state.x[4]);
    }, "@@:@^Q^@", true);
    fill_method(s, string, "stringWithContentsOfURL:usedEncoding:error:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = read_file(state.x[2], true, state.x[3], state.x[4]);
    }, "@@:@^Q^@", true);

    // ---- characters ----
    fill_method(s, string, "getCharacters:", [this](CpuState& state, GuestMemory&) {
        static_cast<void>(s.send_id(state.x[0], "getCharacters:range:", {state.x[2], 0, length(state.x[0])}));
    }, "v@:^S");
    fill_method(s, string, "initWithCharactersNoCopy:length:freeWhenDone:", [this](CpuState& state, GuestMemory&) {
        const auto characters = state.x[2];
        const bool free_it = (state.x[4] & 0xff) != 0;
        state.x[0] = s.send_id(state.x[0], "initWithCharacters:length:", {characters, state.x[3]});
        if (free_it && characters) static_cast<void>(s.call("/usr/lib/libSystem.B.dylib", "_free", {characters}));
    }, "@@:^SQB");
    // NSRange -rangeOfComposedCharacterSequencesForRange:: both ends widened to whole sequences.
    fill_method(s, string, "rangeOfComposedCharacterSequencesForRange:", [this](CpuState& state, GuestMemory&) {
        const auto total = length(state.x[0]);
        auto start = state.x[2], end = state.x[2] + state.x[3];
        if (start > total || end > total) fail("NSRangeException: -rangeOfComposedCharacterSequencesForRange: beyond the string");
        const auto composed = [&](Address index) {
            CallRegs r;
            r.x[0] = state.x[0];
            r.x[1] = index;
            const auto result = s.call(cf_library, "_CFStringGetRangeOfComposedCharactersAtIndex", r);
            return std::pair{result.x0, result.x1};
        };
        if (start < total) start = composed(start).first;
        if (end > 0 && end <= total && end > start) { const auto last = composed(end - 1); end = last.first + last.second; }
        return_range(state, static_cast<std::int64_t>(start), static_cast<std::int64_t>(end - start));
    }, "{_NSRange=QQ}@:{_NSRange=QQ}");
    // -commonPrefixWithString:options: (NSCaseInsensitiveSearch and NSLiteralSearch).
    fill_method(s, string, "commonPrefixWithString:options:", [this](CpuState& state, GuestMemory&) {
        const auto a = g.text(state.x[0]), b = state.x[2] ? g.text(state.x[2]) : std::u16string();
        const bool insensitive = state.x[3] & 1;
        const auto fold = [](char16_t c) -> char16_t { return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + 32) : c; };
        std::size_t common = 0;
        while (common < a.size() && common < b.size() && (insensitive ? fold(a[common]) == fold(b[common]) : a[common] == b[common])) ++common;
        state.x[0] = g.auto_string(std::u16string_view(a).substr(0, common));
    }, "@@:@Q");
    fill_method(s, string, "rangeOfString:options:range:locale:", [this](CpuState& state, GuestMemory&) {
        const auto result = s.process.allocate_guest(16);
        const bool found = cf("_CFStringFindWithOptionsAndLocale", {state.x[0], state.x[2], state.x[4], state.x[5], state.x[3], state.x[6], result}) & 0xff;
        const auto location = s.memory().read64(result), count = s.memory().read64(result + 8);
        s.process.release_guest(result);
        if (found) return_range(state, static_cast<std::int64_t>(location), static_cast<std::int64_t>(count));
        else return_range(state, static_cast<std::int64_t>(not_found), 0);
    }, "{_NSRange=QQ}@:@Q{_NSRange=QQ}@");

    // ---- transforms ----
    fill_method(s, string, "stringByFoldingWithOptions:locale:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = edited(state.x[0], [&](Address copy) { static_cast<void>(cf("_CFStringFold", {copy, state.x[2], state.x[3]})); return true; });
    }, "@@:Q@");
    fill_method(s, string, "stringByApplyingTransform:reverse:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = edited(state.x[0], [&](Address copy) { return (cf("_CFStringTransform", {copy, 0, state.x[2], state.x[3] & 0xff}) & 0xff) != 0; });
    }, "@@:@B");
    fill_method(s, mutable_string, "applyTransform:reverse:range:updatedRange:", [this](CpuState& state, GuestMemory& m) {
        const auto range = s.process.allocate_guest(16);
        m.write64(range, state.x[4]);
        m.write64(range + 8, state.x[5]);
        const bool ok = cf("_CFStringTransform", {state.x[0], range, state.x[2], state.x[3] & 0xff}) & 0xff;
        if (ok && state.x[6]) { m.write64(state.x[6], m.read64(range)); m.write64(state.x[6] + 8, m.read64(range + 8)); }
        s.process.release_guest(range);
        state.x[0] = ok;
    }, "B@:@B{_NSRange=QQ}^{_NSRange=QQ}");
    fill_method(s, string, "lowercaseStringWithLocale:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = edited(state.x[0], [&](Address copy) { static_cast<void>(cf("_CFStringLowercase", {copy, state.x[2]})); return true; });
    }, "@@:@");
    fill_method(s, string, "uppercaseStringWithLocale:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = edited(state.x[0], [&](Address copy) { static_cast<void>(cf("_CFStringUppercase", {copy, state.x[2]})); return true; });
    }, "@@:@");
    for (const auto& [selector, form] : std::array<std::pair<const char*, Address>, 2>{{
             {"decomposedStringWithCompatibilityMapping", 1}, {"precomposedStringWithCompatibilityMapping", 3}}})
        fill_method(s, string, selector, [this, form](CpuState& state, GuestMemory&) {
            state.x[0] = edited(state.x[0], [&](Address copy) { static_cast<void>(cf("_CFStringNormalize", {copy, form})); return true; });
        }, "@@:");

    // ---- lines and paragraphs ----
    for (const bool lines : {true, false}) {
        fill_method(s, string, lines ? "getLineStart:end:contentsEnd:forRange:" : "getParagraphStart:end:contentsEnd:forRange:",
            [this, lines](CpuState& state, GuestMemory& m) {
                const auto found = bounds(state.x[0], state.x[5], state.x[6], lines);
                if (state.x[2]) m.write64(state.x[2], found[0]);
                if (state.x[3]) m.write64(state.x[3], found[1]);
                if (state.x[4]) m.write64(state.x[4], found[2]);
            }, "v@:^Q^Q^Q{_NSRange=QQ}");
        fill_method(s, string, lines ? "lineRangeForRange:" : "paragraphRangeForRange:", [this, lines](CpuState& state, GuestMemory&) {
            const auto found = bounds(state.x[0], state.x[2], state.x[3], lines);
            return_range(state, static_cast<std::int64_t>(found[0]), static_cast<std::int64_t>(found[1] - found[0]));
        }, "{_NSRange=QQ}@:{_NSRange=QQ}");
    }
    // -enumerateLinesUsingBlock: block(NSString* line, BOOL* stop), without the line terminators.
    fill_method(s, string, "enumerateLinesUsingBlock:", [this](CpuState& state, GuestMemory& m) {
        const auto self = state.x[0], block = state.x[2];
        const auto total = length(self);
        const auto stop = s.process.allocate_guest(8);
        m.write64(stop, 0);
        for (Address at = 0; at < total;) {
            const auto found = bounds(self, at, 0, true);
            const auto line = s.send_id(self, "substringWithRange:", {found[0], found[2] - found[0]});
            static_cast<void>(g.invoke_block(block, {line, stop}));
            if (m.read8(stop) || found[1] <= at) break;
            at = found[1];
        }
        s.process.release_guest(stop);
    }, "v@:@?");

    // ---- escapes ----
    fill_method(s, string, "stringByRemovingPercentEncoding", [this](CpuState& state, GuestMemory&) {
        state.x[0] = g.autorelease(cf("_CFURLCreateStringByReplacingPercentEscapes", {0, state.x[0], g.auto_string(std::string_view(""))}));
    }, "@@:");
    fill_method(s, string, "stringByAddingPercentEscapesUsingEncoding:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = g.autorelease(cf("_CFURLCreateStringByAddingPercentEscapes", {0, state.x[0], 0, 0, cf_encoding(state.x[2])}));
    }, "@@:Q");
    fill_method(s, string, "stringByReplacingPercentEscapesUsingEncoding:", [this](CpuState& state, GuestMemory&) {
        state.x[0] = g.autorelease(cf("_CFURLCreateStringByReplacingPercentEscapesUsingEncoding",
            {0, state.x[0], g.auto_string(std::string_view("")), cf_encoding(state.x[2])}));
    }, "@@:Q");

    // ---- paths ----
    fill_method(s, string, "stringByAbbreviatingWithTildeInPath", [this](CpuState& state, GuestMemory&) {
        const auto path = g.utf8(state.x[0]);
        const auto home = g.utf8(s.call("/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation", "_NSHomeDirectory", {}));
        if (!home.empty() && (path == home || path.starts_with(home + "/"))) state.x[0] = g.auto_string(std::string_view("~" + path.substr(home.size())));
    }, "@@:");
    fill_method(s, string, "stringsByAppendingPaths:", [this](CpuState& state, GuestMemory&) {
        std::vector<Address> paths;
        for (const auto part : state.x[2] ? g.elements(state.x[2]) : std::vector<Address>{})
            paths.push_back(s.send_id(state.x[0], "stringByAppendingPathComponent:", {part}));
        state.x[0] = g.auto_array(paths);
    }, "@@:@");

    // ---- property lists ----
    const auto property_list = [this](Address self) {
        const auto data = s.send_id(self, "dataUsingEncoding:", {ns_utf8});
        return s.send_id(s.objc.find_class("NSPropertyListSerialization"), "propertyListWithData:options:format:error:", {data, 0, 0, 0});
    };
    fill_method(s, string, "propertyList", [property_list](CpuState& state, GuestMemory&) {
        const auto value = property_list(state.x[0]);
        if (!value) fail("NSParseErrorException: -[NSString propertyList] of text that is not a property list");
        state.x[0] = value;
    }, "@@:");
    fill_method(s, string, "propertyListFromStringsFileFormat", [property_list](CpuState& state, GuestMemory&) {
        state.x[0] = property_list(state.x[0]);
    }, "@@:");

    // ---- other ----
    fill_method(s, string, "mutableCopyWithZone:", [this](CpuState& state, GuestMemory&) { state.x[0] = s.send_id(state.x[0], "mutableCopy"); }, "@@:^v");
    fill_method(s, string, "supportsSecureCoding", [](CpuState& state, GuestMemory&) { state.x[0] = 1; }, "B@:", true);
    fill_method(s, string, "variantFittingPresentationWidth:", [](CpuState&, GuestMemory&) {}, "@@:q");   // no width variants
}

} // namespace

void install_strings_more(Support& support) {
    auto more = std::make_shared<More>(More{support});
    more->install();
    static std::vector<std::shared_ptr<More>> keep;
    keep.push_back(more);
}

} // namespace alchemy::gui::completion_kit::data
