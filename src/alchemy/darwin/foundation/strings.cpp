#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/foundation/regex.hpp"
#include "alchemy/darwin/kit/completion/data/text.hpp"

#include <algorithm>

// NSString / NSMutableString additions (added only where missing):
//   comparison      -compare:options:[range:], -localizedCaseInsensitiveCompare:,
//                   -localizedStandardCompare:, -localizedCaseInsensitiveContainsString:
//                   (CFString order after case/diacritic/width folding and
//                   numeric runs; no locale collation tables)
//   searching       -rangeOfString:options:[range:] (NSRegularExpressionSearch
//                   through the regex engine), -localizedStandardRangeOfString:,
//                   -rangeOfCharacterFromSet:[options:[range:]]
//   transforming    -capitalizedString[WithLocale:], -stringByTrimmingCharactersInSet:,
//                   -stringByPaddingToLength:withString:startingAtIndex:,
//                   -stringByReplacingCharactersInRange:withString:,
//                   -stringByReplacingOccurrencesOfString:withString:options:range:,
//                   -componentsSeparatedByCharactersInSet:,
//                   -stringByAddingPercentEncodingWithAllowedCharacters: (UTF-8),
//                   -stringByAppendingFormat:, -[NSMutableString appendFormat:]
//   enumeration     -enumerateSubstringsInRange:options:usingBlock: (lines,
//                   paragraphs, composed characters, words, sentences)
//   paths           +pathWithComponents:, -stringByResolvingSymlinksInPath (realpath)
//   files           +stringWithContentsOfFile/URL:encoding:error:, -initWith...,
//                   -writeToFile/URL:atomically:encoding:error: (through NSData)
//   creation        -initWithBytesNoCopy:length:encoding:freeWhenDone: (copies, then frees)
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::uint64_t regular_expression_search = 1024;
constexpr std::int64_t file_read_inapplicable_encoding = 261, file_write_inapplicable_encoding = 517;

struct Strings {
    Support& s;
    Guest g{s};

    bool member(Address set, char32_t c) {
        CallRegs r;
        r.x[2] = c;
        return (s.send(set, "longCharacterIsMember:", r).x0 & 0xff) != 0;
    }
    std::pair<std::int64_t, std::int64_t> range_in(const std::u16string& value, Address location, Address length, const char* what) {
        if (location > value.size() || length > value.size() - location)
            fail(std::string("NSRangeException: -[NSString ") + what + "]: range {" + std::to_string(location) + ", " + std::to_string(length) +
                 "} out of bounds; string length " + std::to_string(value.size()));
        return {static_cast<std::int64_t>(location), static_cast<std::int64_t>(length)};
    }
    text::Found find(Address self, Address target, std::uint64_t options, std::int64_t location, std::int64_t length) {
        const auto haystack = g.text(self), needle = g.text(target);
        if (options & regular_expression_search) {
            regex::Status status;
            std::string message;
            const auto pattern = regex::Pattern::compile(needle, (options & text::case_insensitive_search) ? regex::case_insensitive : 0, status, message);
            if (!pattern) {
                if (status == regex::Status::unsupported) unsupported("NSRegularExpressionSearch pattern: " + message);
                return {};
            }
            const auto anchored = (options & text::anchored_search) ? regex::anchored : 0;
            if (options & text::backwards_search) {
                const auto all = pattern->all(haystack, {location, length}, anchored);
                if (all.empty()) return {};
                return {all.back().groups[0].location, all.back().groups[0].length};
            }
            const auto m = pattern->search(haystack, {location, length}, location, anchored);
            if (!m) return {};
            return {m->groups[0].location, m->groups[0].length};
        }
        return text::find(haystack, needle, options, location, length);
    }
    std::vector<std::pair<std::size_t, std::size_t>> pieces(const std::u16string& value, std::size_t lo, std::size_t hi, std::uint64_t kind,
                                                            std::vector<std::pair<std::size_t, std::size_t>>& enclosing);
};

bool line_end(char16_t c, bool paragraphs) {
    if (c == u'\n' || c == u'\r' || c == 0x85 || c == 0x2029) return true;
    return !paragraphs && c == 0x2028;
}

// Substring ranges for -enumerateSubstringsInRange:options:usingBlock:.
std::vector<std::pair<std::size_t, std::size_t>> Strings::pieces(const std::u16string& value, std::size_t lo, std::size_t hi, std::uint64_t kind,
                                                                   std::vector<std::pair<std::size_t, std::size_t>>& enclosing) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (kind == 0 || kind == 1) {
        // Lines / paragraphs: the substring excludes, the enclosing range includes, the terminator.
        std::size_t at = lo;
        while (at < hi) {
            auto end = at;
            while (end < hi && !line_end(value[end], kind == 1)) ++end;
            auto next = end;
            if (next < hi) next += (value[next] == u'\r' && next + 1 < hi && value[next + 1] == u'\n') ? 2 : 1;
            out.emplace_back(at, end - at);
            enclosing.emplace_back(at, next - at);
            at = next;
        }
        return out;
    }
    if (kind == 2) {
        // Composed character sequences: a base character, its combining marks, surrogate pairs.
        std::size_t at = lo;
        while (at < hi) {
            auto end = at + 1;
            if (value[at] >= 0xd800 && value[at] <= 0xdbff && end < hi && value[end] >= 0xdc00 && value[end] <= 0xdfff) ++end;
            if (value[at] == u'\r' && end < hi && value[end] == u'\n') ++end;
            while (end < hi && (text::is_mark(value[end]) || value[end] == 0x200d || (value[end] >= 0xfe00 && value[end] <= 0xfe0f))) ++end;
            out.emplace_back(at, end - at);
            enclosing.emplace_back(at, end - at);
            at = end;
        }
        return out;
    }
    if (kind == 3) {
        // Words: runs of word characters (apostrophes and periods between letters stay inside);
        // the enclosing range extends over the following non-word characters.
        const auto word = [&](std::size_t i) { return text::is_word(value[i]); };
        std::size_t at = lo;
        while (at < hi && !word(at)) ++at;
        while (at < hi) {
            auto end = at;
            while (end < hi && (word(end) || ((value[end] == u'\'' || value[end] == 0x2019 || value[end] == u'.') && end + 1 < hi && word(end + 1) && end > at))) ++end;
            auto next = end;
            while (next < hi && !word(next)) ++next;
            out.emplace_back(at, end - at);
            enclosing.emplace_back(enclosing.empty() ? lo : at, next - (enclosing.empty() ? lo : at));
            at = next;
        }
        return out;
    }
    if (kind == 4) {
        // Sentences: up to and including terminal punctuation and the white space after it.
        std::size_t at = lo;
        while (at < hi) {
            auto end = at;
            while (end < hi && value[end] != u'.' && value[end] != u'!' && value[end] != u'?' && !line_end(value[end], true)) ++end;
            while (end < hi && (value[end] == u'.' || value[end] == u'!' || value[end] == u'?' || value[end] == u'"' || value[end] == u')')) ++end;
            auto next = end;
            while (next < hi && text::is_space(value[next])) ++next;
            out.emplace_back(at, next - at);
            enclosing.emplace_back(at, next - at);
            at = next;
        }
        return out;
    }
    unsupported("-[NSString enumerateSubstringsInRange:options:usingBlock:] enumeration kind " + std::to_string(kind));
}

} // namespace

void install_strings(Support& support) {
    auto st = std::make_shared<Strings>(Strings{support});
    auto* sp = &support;
    const auto string_class = support.objc.find_class("NSString");
    const auto mutable_class = support.objc.find_class("NSMutableString");
    if (!string_class || !mutable_class) fail("NSString / NSMutableString are not registered");
    auto& g = st->g;
    const auto ns = string_class;

    // ---- comparison ----
    const auto compare_range = [st](Address self, Address other, std::uint64_t options, std::optional<std::pair<Address, Address>> range) {
        if (!other) fail("NSInvalidArgumentException: -[NSString compare:]: nil argument");
        auto left = st->g.text(self);
        if (range) {
            const auto [lo, n] = st->range_in(left, range->first, range->second, "compare:options:range:");
            left = left.substr(static_cast<std::size_t>(lo), static_cast<std::size_t>(n));
        }
        return static_cast<Address>(static_cast<std::int64_t>(text::compare(left, st->g.text(other), options)));
    };
    fill_method(support, ns, "compare:options:", [compare_range](CpuState& s, GuestMemory&) { s.x[0] = compare_range(s.x[0], s.x[2], s.x[3], std::nullopt); }, "q@:@Q");
    fill_method(support, ns, "compare:options:range:", [compare_range](CpuState& s, GuestMemory&) {
        s.x[0] = compare_range(s.x[0], s.x[2], s.x[3], std::pair{s.x[4], s.x[5]});
    }, "q@:@Q{_NSRange=QQ}");
    fill_method(support, ns, "localizedCaseInsensitiveCompare:", [compare_range](CpuState& s, GuestMemory&) {
        s.x[0] = compare_range(s.x[0], s.x[2], text::case_insensitive_search, std::nullopt);
    }, "q@:@");
    fill_method(support, ns, "localizedCompare:", [compare_range](CpuState& s, GuestMemory&) { s.x[0] = compare_range(s.x[0], s.x[2], 0, std::nullopt); }, "q@:@");
    fill_method(support, ns, "localizedStandardCompare:", [compare_range](CpuState& s, GuestMemory&) {
        s.x[0] = compare_range(s.x[0], s.x[2], text::case_insensitive_search | text::numeric_search | text::width_insensitive_search | text::forced_ordering_search, std::nullopt);
    }, "q@:@");
    fill_method(support, ns, "localizedCaseInsensitiveContainsString:", [st](CpuState& s, GuestMemory&) {
        const auto n = static_cast<std::int64_t>(st->g.text(s.x[0]).size());
        return_bool(s, st->find(s.x[0], s.x[2], text::case_insensitive_search, 0, n).location >= 0);
    }, "B@:@");
    fill_method(support, ns, "localizedStandardContainsString:", [st](CpuState& s, GuestMemory&) {
        const auto n = static_cast<std::int64_t>(st->g.text(s.x[0]).size());
        return_bool(s, st->find(s.x[0], s.x[2], text::case_insensitive_search | text::diacritic_insensitive_search, 0, n).location >= 0);
    }, "B@:@");

    // ---- searching ----
    fill_method(support, ns, "rangeOfString:options:", [st](CpuState& s, GuestMemory&) {
        const auto n = static_cast<std::int64_t>(st->g.text(s.x[0]).size());
        const auto f = st->find(s.x[0], s.x[2], s.x[3], 0, n);
        return_range(s, f.location, f.length);
    }, "{_NSRange=QQ}@:@Q");
    fill_method(support, ns, "rangeOfString:options:range:", [st](CpuState& s, GuestMemory&) {
        const auto [lo, n] = st->range_in(st->g.text(s.x[0]), s.x[4], s.x[5], "rangeOfString:options:range:");
        const auto f = st->find(s.x[0], s.x[2], s.x[3], lo, n);
        return_range(s, f.location, f.length);
    }, "{_NSRange=QQ}@:@Q{_NSRange=QQ}");
    fill_method(support, ns, "localizedStandardRangeOfString:", [st](CpuState& s, GuestMemory&) {
        const auto n = static_cast<std::int64_t>(st->g.text(s.x[0]).size());
        const auto f = st->find(s.x[0], s.x[2], text::case_insensitive_search | text::diacritic_insensitive_search | text::width_insensitive_search, 0, n);
        return_range(s, f.location, f.length);
    }, "{_NSRange=QQ}@:@");
    const auto character_range = [st](CpuState& s, Address set, std::uint64_t options, std::int64_t lo, std::int64_t n) {
        if (!set) fail("NSInvalidArgumentException: -[NSString rangeOfCharacterFromSet:]: nil argument");
        const auto value = st->g.text(s.x[0]);
        const auto test = [&](std::int64_t i) -> std::int64_t {
            // A surrogate pair is one character of length 2.
            const auto u = value[static_cast<std::size_t>(i)];
            if (u >= 0xd800 && u <= 0xdbff && i + 1 < lo + n && value[static_cast<std::size_t>(i + 1)] >= 0xdc00 && value[static_cast<std::size_t>(i + 1)] <= 0xdfff) {
                const char32_t c = 0x10000 + ((static_cast<char32_t>(u) - 0xd800) << 10) + (static_cast<char32_t>(value[static_cast<std::size_t>(i + 1)]) - 0xdc00);
                return st->member(set, c) ? 2 : 0;
            }
            return st->member(set, u) ? 1 : 0;
        };
        const bool backwards = options & text::backwards_search, anchored = options & text::anchored_search;
        if (!backwards) {
            for (auto i = lo; i < lo + n; ++i) {
                if (const auto w = test(i)) { return_range(s, i, w); return; }
                if (anchored) break;
            }
        } else {
            for (auto i = lo + n - 1; i >= lo; --i) {
                if (const auto w = test(i)) { return_range(s, i, w); return; }
                if (anchored) break;
            }
        }
        return_range(s, -1, 0);
    };
    fill_method(support, ns, "rangeOfCharacterFromSet:", [st, character_range](CpuState& s, GuestMemory&) {
        character_range(s, s.x[2], 0, 0, static_cast<std::int64_t>(st->g.text(s.x[0]).size()));
    }, "{_NSRange=QQ}@:@");
    fill_method(support, ns, "rangeOfCharacterFromSet:options:", [st, character_range](CpuState& s, GuestMemory&) {
        character_range(s, s.x[2], s.x[3], 0, static_cast<std::int64_t>(st->g.text(s.x[0]).size()));
    }, "{_NSRange=QQ}@:@Q");
    fill_method(support, ns, "rangeOfCharacterFromSet:options:range:", [st, character_range](CpuState& s, GuestMemory&) {
        const auto [lo, n] = st->range_in(st->g.text(s.x[0]), s.x[4], s.x[5], "rangeOfCharacterFromSet:options:range:");
        character_range(s, s.x[2], s.x[3], lo, n);
    }, "{_NSRange=QQ}@:@Q{_NSRange=QQ}");

    // ---- transforming ----
    for (const auto* name : {"capitalizedString", "localizedCapitalizedString"})
        fill_method(support, ns, name, [st](CpuState& s, GuestMemory&) { s.x[0] = st->g.auto_string(text::capitalized(st->g.text(s.x[0]))); }, "@@:");
    fill_method(support, ns, "capitalizedStringWithLocale:", [st](CpuState& s, GuestMemory&) { s.x[0] = st->g.auto_string(text::capitalized(st->g.text(s.x[0]))); }, "@@:@");
    fill_method(support, ns, "stringByTrimmingCharactersInSet:", [st](CpuState& s, GuestMemory&) {
        const auto value = st->g.text(s.x[0]);
        const auto points = text::code_points(value);
        std::size_t lo = 0, hi = points.size();
        while (lo < hi && st->member(s.x[2], points[lo])) ++lo;
        while (hi > lo && st->member(s.x[2], points[hi - 1])) --hi;
        s.x[0] = st->g.auto_string(text::from_code_points(std::u32string_view(points).substr(lo, hi - lo)));
    }, "@@:@");
    fill_method(support, ns, "stringByPaddingToLength:withString:startingAtIndex:", [st](CpuState& s, GuestMemory&) {
        const auto value = st->g.text(s.x[0]), padding = st->g.text(s.x[3]);
        if (s.x[2] > value.size() && padding.empty()) fail("NSInvalidArgumentException: -[NSString stringByPaddingToLength:withString:startingAtIndex:]: empty padding string");
        if (s.x[2] > value.size() && s.x[4] >= padding.size()) fail("NSRangeException: -[NSString stringByPaddingToLength:withString:startingAtIndex:]: index " + std::to_string(s.x[4]) + " out of bounds");
        s.x[0] = st->g.auto_string(text::pad(value, static_cast<std::size_t>(s.x[2]), padding, static_cast<std::size_t>(s.x[4])));
    }, "@@:Q@Q");
    fill_method(support, ns, "stringByReplacingCharactersInRange:withString:", [st](CpuState& s, GuestMemory&) {
        auto value = st->g.text(s.x[0]);
        const auto [lo, n] = st->range_in(value, s.x[2], s.x[3], "stringByReplacingCharactersInRange:withString:");
        if (!s.x[4]) fail("NSInvalidArgumentException: -[NSString stringByReplacingCharactersInRange:withString:]: nil argument");
        value.replace(static_cast<std::size_t>(lo), static_cast<std::size_t>(n), st->g.text(s.x[4]));
        s.x[0] = st->g.auto_string(value);
    }, "@@:{_NSRange=QQ}@");
    fill_method(support, ns, "stringByReplacingOccurrencesOfString:withString:options:range:", [st](CpuState& s, GuestMemory&) {
        auto value = st->g.text(s.x[0]);
        const auto target = st->g.text(s.x[2]), replacement = st->g.text(s.x[3]);
        const auto options = s.x[4];
        const auto [lo, n] = st->range_in(value, s.x[5], s.x[6], "stringByReplacingOccurrencesOfString:withString:options:range:");
        std::u16string out;
        if (options & regular_expression_search) {
            regex::Status status;
            std::string message;
            const auto pattern = regex::Pattern::compile(target, (options & text::case_insensitive_search) ? regex::case_insensitive : 0, status, message);
            if (!pattern) {
                if (status == regex::Status::unsupported) unsupported("NSRegularExpressionSearch pattern: " + message);
                s.x[0] = s.x[0];
                return;
            }
            const auto matches = pattern->all(value, {lo, n}, (options & text::anchored_search) ? regex::anchored : 0);
            std::int64_t at = 0;
            for (const auto& m : matches) {
                out += value.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(m.groups[0].location - at));
                out += regex::expand_template(*pattern, value, m, replacement);
                at = m.groups[0].location + m.groups[0].length;
            }
            out += value.substr(static_cast<std::size_t>(at));
        } else {
            if (target.empty()) { s.x[0] = st->g.autorelease(st->g.string(value)); return; }
            std::vector<text::Found> found;
            if (options & text::backwards_search) {
                auto hi = lo + n;
                for (;;) {
                    const auto f = text::find(value, target, options, lo, hi - lo);
                    if (f.location < 0) break;
                    found.push_back(f);
                    if (options & text::anchored_search) break;
                    hi = f.location;
                }
                std::reverse(found.begin(), found.end());
            } else {
                auto at = lo;
                for (;;) {
                    const auto f = text::find(value, target, options, at, lo + n - at);
                    if (f.location < 0) break;
                    found.push_back(f);
                    if (options & text::anchored_search) break;
                    at = f.location + f.length;
                }
            }
            std::int64_t at = 0;
            for (const auto& f : found) {
                out += value.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(f.location - at));
                out += replacement;
                at = f.location + f.length;
            }
            out += value.substr(static_cast<std::size_t>(at));
        }
        s.x[0] = st->g.auto_string(out);
    }, "@@:@@Q{_NSRange=QQ}");
    fill_method(support, ns, "componentsSeparatedByCharactersInSet:", [st](CpuState& s, GuestMemory&) {
        const auto points = text::code_points(st->g.text(s.x[0]));
        std::vector<Address> parts;
        std::u32string current;
        try {
            for (const auto c : points) {
                if (st->member(s.x[2], c)) { parts.push_back(st->g.string(text::from_code_points(current))); current.clear(); }
                else current.push_back(c);
            }
            parts.push_back(st->g.string(text::from_code_points(current)));
        } catch (...) { for (const auto p : parts) st->g.release(p); throw; }
        const auto list = st->g.array(parts);
        for (const auto p : parts) st->g.release(p);
        s.x[0] = st->g.autorelease(list);
    }, "@@:@");
    fill_method(support, ns, "stringByAddingPercentEncodingWithAllowedCharacters:", [st](CpuState& s, GuestMemory&) {
        const auto set = s.x[2];
        s.x[0] = st->g.auto_string(text::percent_encode(st->g.text(s.x[0]), [&](char32_t c) { return set && st->member(set, c); }));
    }, "@@:@");
    // Variadic formats: the arguments are at the caller's stack (a Darwin va_list).
    fill_method(support, ns, "stringByAppendingFormat:", [st](CpuState& s, GuestMemory&) {
        CallRegs r;
        r.x[2] = s.x[2];
        r.x[3] = s.sp;
        const auto formatted = st->s.send(st->s.send_id(st->s.objc.find_class("NSString"), "alloc"), "initWithFormat:arguments:", r).x0;
        auto value = st->g.text(s.x[0]);
        value += st->g.text(formatted);
        st->g.release(formatted);
        s.x[0] = st->g.auto_string(value);
    }, "@@:@");
    fill_method(support, mutable_class, "appendFormat:", [st](CpuState& s, GuestMemory&) {
        CallRegs r;
        r.x[2] = s.x[2];
        r.x[3] = s.sp;
        const auto formatted = st->s.send(st->s.send_id(st->s.objc.find_class("NSString"), "alloc"), "initWithFormat:arguments:", r).x0;
        try { st->s.send_id(s.x[0], "appendString:", {formatted}); } catch (...) { st->g.release(formatted); throw; }
        st->g.release(formatted);
    }, "v@:@");

    // ---- enumeration ----
    fill_method(support, ns, "enumerateSubstringsInRange:options:usingBlock:", [st](CpuState& s, GuestMemory&) {
        const auto value = st->g.text(s.x[0]);
        const auto [lo, n] = st->range_in(value, s.x[2], s.x[3], "enumerateSubstringsInRange:options:usingBlock:");
        const auto options = s.x[4];
        const auto block = s.x[5];
        const auto kind = options & 0xff;
        const bool reverse = options & (1u << 8), no_substring = options & (1u << 9);
        std::vector<std::pair<std::size_t, std::size_t>> enclosing;
        auto ranges = st->pieces(value, static_cast<std::size_t>(lo), static_cast<std::size_t>(lo + n), kind, enclosing);
        std::vector<std::size_t> order(ranges.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = reverse ? order.size() - 1 - i : i;
        Guest::Scratch stop(st->g, 8);
        st->g.memory().write64(stop.address, 0);
        for (const auto i : order) {
            const auto [at, length] = ranges[i];
            const auto substring = no_substring ? 0 : st->g.string(value.substr(at, length));
            try {
                static_cast<void>(st->g.invoke_block(block, {substring, at, length, enclosing[i].first, enclosing[i].second, stop.address}));
            } catch (...) { st->g.release(substring); throw; }
            st->g.release(substring);
            if (st->g.memory().read8(stop.address)) break;
        }
    }, "v@:{_NSRange=QQ}Q@?");

    // ---- paths ----
    fill_method(support, ns, "pathWithComponents:", [st](CpuState& s, GuestMemory&) {
        std::u16string path;
        for (const auto component : st->g.elements(s.x[2])) {
            const auto part = st->g.text(component);
            if (part.empty()) continue;
            if (!path.empty() && path.back() != u'/') path.push_back(u'/');
            path += part;
        }
        while (path.size() > 1 && path.back() == u'/') path.pop_back();
        s.x[0] = st->g.auto_string(path);
    }, "@@:@", true);
    fill_method(support, ns, "stringByResolvingSymlinksInPath", [st](CpuState& s, GuestMemory&) {
        // ~ expansion and standardization first (Foundation's), then realpath for absolute paths.
        const auto standardized = st->s.send_id(s.x[0], "stringByStandardizingPath");
        const auto path = st->g.utf8(standardized);
        if (path.empty() || path[0] != '/') { s.x[0] = standardized; return; }
        Guest::Scratch input(st->g, path.size() + 1), output(st->g, 1025);
        st->g.memory().write_bytes(input.address, std::span(reinterpret_cast<const std::uint8_t*>(path.c_str()), path.size() + 1));
        const auto resolved = st->s.call(libsystem_library, "_realpath", {input.address, output.address});
        if (!resolved) { s.x[0] = standardized; return; }
        std::string out;
        for (auto at = output.address; out.size() < 1024; ++at) { const auto c = st->g.memory().read8(at); if (!c) break; out.push_back(static_cast<char>(c)); }
        // Foundation removes a leading /private when the rest still names the same item.
        if (out.rfind("/private/", 0) == 0 && path.rfind("/private/", 0) != 0) out.erase(0, 8);
        s.x[0] = st->g.auto_string(std::string_view(out));
    }, "@@:");

    // ---- files ----
    const auto read_string = [st](Address cls_or_self, Address source, Address encoding, Address error_out, bool url) -> Address {
        st->g.set_error(error_out, 0);
        CallRegs r;
        r.x[2] = source;
        r.x[3] = 0;
        r.x[4] = error_out;
        const auto data = st->s.send(st->s.objc.find_class("NSData"), url ? "dataWithContentsOfURL:options:error:" : "dataWithContentsOfFile:options:error:", r).x0;
        if (!data) return 0;
        CallRegs d;
        d.x[2] = data;
        d.x[3] = encoding;
        const auto made = st->s.send(st->s.send_id(cls_or_self, "alloc"), "initWithData:encoding:", d).x0;
        if (!made) {
            const auto path = url ? st->s.send_id(source, "path") : source;
            st->g.set_error(error_out, st->g.error("NSCocoaErrorDomain", file_read_inapplicable_encoding,
                                                   "The file couldn't be opened using the specified text encoding: " + st->g.utf8(path)));
        }
        return made;   // +1
    };
    fill_method(support, ns, "stringWithContentsOfFile:encoding:error:", [st, read_string](CpuState& s, GuestMemory&) {
        s.x[0] = st->g.autorelease(read_string(s.x[0], s.x[2], s.x[3], s.x[4], false));
    }, "@@:@Q^@", true);
    fill_method(support, ns, "stringWithContentsOfURL:encoding:error:", [st, read_string](CpuState& s, GuestMemory&) {
        s.x[0] = st->g.autorelease(read_string(s.x[0], s.x[2], s.x[3], s.x[4], true));
    }, "@@:@Q^@", true);
    fill_method(support, ns, "initWithContentsOfFile:encoding:error:", [st, read_string](CpuState& s, GuestMemory&) {
        const auto cls = st->s.objc.object_class(s.x[0]);
        st->g.release(s.x[0]);
        s.x[0] = read_string(cls, s.x[2], s.x[3], s.x[4], false);
    }, "@@:@Q^@");
    fill_method(support, ns, "initWithContentsOfURL:encoding:error:", [st, read_string](CpuState& s, GuestMemory&) {
        const auto cls = st->s.objc.object_class(s.x[0]);
        st->g.release(s.x[0]);
        s.x[0] = read_string(cls, s.x[2], s.x[3], s.x[4], true);
    }, "@@:@Q^@");
    const auto write_string = [st](Address self, Address destination, bool atomic, Address encoding, Address error_out, bool url) {
        st->g.set_error(error_out, 0);
        CallRegs e;
        e.x[2] = encoding;
        const auto data = st->s.send(self, "dataUsingEncoding:", e).x0;
        if (!data) {
            st->g.set_error(error_out, st->g.error("NSCocoaErrorDomain", file_write_inapplicable_encoding, "The string cannot be represented in the specified text encoding"));
            return false;
        }
        CallRegs w;
        w.x[2] = destination;
        w.x[3] = atomic ? 1 : 0;
        w.x[4] = error_out;
        return (st->s.send(data, url ? "writeToURL:options:error:" : "writeToFile:options:error:", w).x0 & 0xff) != 0;
    };
    fill_method(support, ns, "writeToFile:atomically:encoding:error:", [write_string](CpuState& s, GuestMemory&) {
        return_bool(s, write_string(s.x[0], s.x[2], arg_bool(s.x[3]), s.x[4], s.x[5], false));
    }, "B@:@BQ^@");
    fill_method(support, ns, "writeToURL:atomically:encoding:error:", [write_string](CpuState& s, GuestMemory&) {
        return_bool(s, write_string(s.x[0], s.x[2], arg_bool(s.x[3]), s.x[4], s.x[5], true));
    }, "B@:@BQ^@");

    // ---- creation ----
    fill_method(support, ns, "initWithBytesNoCopy:length:encoding:freeWhenDone:", [sp](CpuState& s, GuestMemory&) {
        // Strings own their characters here: copy, then free the caller's buffer as promised.
        CallRegs r;
        r.x[2] = s.x[2];
        r.x[3] = s.x[3];
        r.x[4] = s.x[4];
        const auto bytes = s.x[2];
        const bool free_when_done = arg_bool(s.x[5]);
        const auto made = sp->send(s.x[0], "initWithBytes:length:encoding:", r).x0;
        if (made && free_when_done && bytes) static_cast<void>(sp->call(libsystem_library, "_free", {bytes}));
        s.x[0] = made;
    }, "@@:^vQQB");

    static_cast<void>(g);
}

} // namespace alchemy::gui::completion_kit::data
