#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/foundation/regex.hpp"
#include "alchemy/darwin/kit/completion/data/text.hpp"

// NSRegularExpression and NSTextCheckingResult over the host regex engine
// (regex.cpp). Invalid patterns give nil and an NSCocoaErrorDomain 2048
// (NSFormattingError) error, as ICU's syntax errors do; constructs the
// engine does not provide (Unicode properties, set operations) and runaway
// backtracking fail explicitly. Matching options: NSMatchingAnchored,
// NSMatchingWithTransparentBounds, NSMatchingWithoutAnchoringBounds;
// NSMatchingReportProgress / ReportCompletion only add block calls, which
// -enumerateMatches... makes for completion (NSMatchingCompleted) when asked.
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr Address text_checking_type_regular_expression = 1ull << 10;
constexpr std::int64_t formatting_error = 2048;

struct RegexState final : ObjectPayload {
    std::shared_ptr<regex::Pattern> pattern;
    Address source{};   // +1 pattern string
    std::uint64_t options{};
};
struct ResultState final : ObjectPayload {
    std::vector<regex::Range> ranges;
    Address expression{};   // +1
};

struct Regexes {
    Support& s;
    Guest g{s};
    Address regex_class{}, result_class{};

    // false: the pattern is invalid (error stored when asked).
    bool configure(Address self, Address pattern, std::uint64_t options, Address error_out) {
        g.set_error(error_out, 0);
        if (!pattern) fail("NSInvalidArgumentException: -[NSRegularExpression initWithPattern:options:error:]: nil pattern");
        regex::Status status;
        std::string message;
        auto compiled = regex::Pattern::compile(g.text(pattern), options, status, message);
        if (!compiled) {
            if (status == regex::Status::unsupported) unsupported("NSRegularExpression pattern \"" + g.utf8(pattern) + "\": " + message);
            std::vector<std::pair<Address, Address>> info{{s.cf.constant_string("NSInvalidValue"), pattern}};
            const auto extra = g.autorelease(s.cf.dictionary(info));
            g.set_error(error_out, g.error("NSCocoaErrorDomain", formatting_error, "The value \"" + g.utf8(pattern) + "\" is invalid: " + message, extra));
            return false;
        }
        auto st = s.objc.host_state<RegexState>(self);
        st->pattern = std::make_shared<regex::Pattern>(std::move(*compiled));
        st->source = s.send_id(pattern, "copy");
        st->options = options;
        return true;
    }
    std::shared_ptr<RegexState> state(Address self) {
        auto st = s.objc.find_host_state<RegexState>(self);
        if (!st || !st->pattern) fail("NSRegularExpression used before initialization");
        return st;
    }
    regex::Range checked_range(const std::u16string& value, Address location, Address length) {
        if (location > value.size() || length > value.size() - location)
            fail("NSRangeException: NSRegularExpression range {" + std::to_string(location) + ", " + std::to_string(length) + "} out of bounds; string length " + std::to_string(value.size()));
        return {static_cast<std::int64_t>(location), static_cast<std::int64_t>(length)};
    }
    std::vector<regex::Match> matches(const RegexState& st, const std::u16string& value, regex::Range range, std::uint64_t options) {
        try { return st.pattern->all(value, range, options); }
        catch (const regex::Exhausted& e) { unsupported(std::string("NSRegularExpression: ") + e.what()); }
    }
    std::optional<regex::Match> first(const RegexState& st, const std::u16string& value, regex::Range range, std::uint64_t options) {
        try { return st.pattern->search(value, range, range.location, options); }
        catch (const regex::Exhausted& e) { unsupported(std::string("NSRegularExpression: ") + e.what()); }
    }
    Address result(Address expression, const regex::Match& m) {
        const auto made = s.objc.allocate(result_class);
        auto st = s.objc.host_state<ResultState>(made);
        st->ranges = m.groups;
        st->expression = g.retain(expression);
        return made;   // +1
    }
    std::u16string replaced(Address self, const std::u16string& value, regex::Range range, std::uint64_t options, const std::u16string& replacement, std::size_t& count) {
        const auto st = state(self);
        const auto all = matches(*st, value, range, options);
        std::u16string out;
        std::int64_t at = 0;
        for (const auto& m : all) {
            out += value.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(m.groups[0].location - at));
            out += regex::expand_template(*st->pattern, value, m, replacement);
            at = m.groups[0].location + m.groups[0].length;
        }
        out += value.substr(static_cast<std::size_t>(at));
        count = all.size();
        return out;
    }
};

} // namespace

void install_regex(Support& support) {
    auto rx = std::make_shared<Regexes>(Regexes{support});
    auto* sp = &support;
    const auto re = support.declare_class("NSRegularExpression", "NSObject", foundation_library);
    const auto tcr = support.declare_class("NSTextCheckingResult", "NSObject", foundation_library);
    rx->regex_class = re;
    rx->result_class = tcr;

    // ---- NSRegularExpression ----
    support.method(re, "regularExpressionWithPattern:options:error:", [rx](CpuState& s, GuestMemory&) {
        const auto self = rx->s.objc.allocate(s.x[0]);
        if (!rx->configure(self, s.x[2], s.x[3], s.x[4])) { rx->g.release(self); s.x[0] = 0; return; }
        s.x[0] = rx->g.autorelease(self);
    }, "@@:@Q^@", true);
    support.method(re, "initWithPattern:options:error:", [rx](CpuState& s, GuestMemory&) {
        if (!rx->configure(s.x[0], s.x[2], s.x[3], s.x[4])) { rx->g.release(s.x[0]); s.x[0] = 0; }
    }, "@@:@Q^@");
    support.method(re, "escapedPatternForString:", [rx](CpuState& s, GuestMemory&) { s.x[0] = rx->g.auto_string(regex::escape_pattern(rx->g.text(s.x[2]))); }, "@@:@", true);
    support.method(re, "escapedTemplateForString:", [rx](CpuState& s, GuestMemory&) { s.x[0] = rx->g.auto_string(regex::escape_template(rx->g.text(s.x[2]))); }, "@@:@", true);
    support.method(re, "pattern", [rx](CpuState& s, GuestMemory&) { s.x[0] = rx->state(s.x[0])->source; }, "@@:");
    support.method(re, "options", [rx](CpuState& s, GuestMemory&) { s.x[0] = rx->state(s.x[0])->options; }, "Q@:");
    support.method(re, "numberOfCaptureGroups", [rx](CpuState& s, GuestMemory&) { s.x[0] = rx->state(s.x[0])->pattern->capture_count(); }, "Q@:");
    support.method(re, "firstMatchInString:options:range:", [rx](CpuState& s, GuestMemory&) {
        const auto value = rx->g.text(s.x[2]);
        const auto m = rx->first(*rx->state(s.x[0]), value, rx->checked_range(value, s.x[4], s.x[5]), s.x[3]);
        s.x[0] = m ? rx->g.autorelease(rx->result(s.x[0], *m)) : 0;
    }, "@@:@Q{_NSRange=QQ}");
    support.method(re, "rangeOfFirstMatchInString:options:range:", [rx](CpuState& s, GuestMemory&) {
        const auto value = rx->g.text(s.x[2]);
        const auto m = rx->first(*rx->state(s.x[0]), value, rx->checked_range(value, s.x[4], s.x[5]), s.x[3]);
        if (m) return_range(s, m->groups[0].location, m->groups[0].length); else return_range(s, -1, 0);
    }, "{_NSRange=QQ}@:@Q{_NSRange=QQ}");
    support.method(re, "matchesInString:options:range:", [rx](CpuState& s, GuestMemory&) {
        const auto value = rx->g.text(s.x[2]);
        const auto all = rx->matches(*rx->state(s.x[0]), value, rx->checked_range(value, s.x[4], s.x[5]), s.x[3]);
        std::vector<Address> results;
        for (const auto& m : all) results.push_back(rx->result(s.x[0], m));
        const auto list = rx->g.array(results);
        for (const auto r : results) rx->g.release(r);
        s.x[0] = rx->g.autorelease(list);
    }, "@@:@Q{_NSRange=QQ}");
    support.method(re, "numberOfMatchesInString:options:range:", [rx](CpuState& s, GuestMemory&) {
        const auto value = rx->g.text(s.x[2]);
        s.x[0] = rx->matches(*rx->state(s.x[0]), value, rx->checked_range(value, s.x[4], s.x[5]), s.x[3]).size();
    }, "Q@:@Q{_NSRange=QQ}");
    support.method(re, "enumerateMatchesInString:options:range:usingBlock:", [rx](CpuState& s, GuestMemory&) {
        const auto value = rx->g.text(s.x[2]);
        const auto options = s.x[3];
        const auto block = s.x[6];
        const auto all = rx->matches(*rx->state(s.x[0]), value, rx->checked_range(value, s.x[4], s.x[5]), options);
        Guest::Scratch stop(rx->g, 8);
        rx->g.memory().write64(stop.address, 0);
        bool stopped = false;
        for (const auto& m : all) {
            const auto r = rx->result(s.x[0], m);
            try { static_cast<void>(rx->g.invoke_block(block, {r, 0, stop.address})); } catch (...) { rx->g.release(r); throw; }
            rx->g.release(r);
            if (rx->g.memory().read8(stop.address)) { stopped = true; break; }
        }
        // NSMatchingReportCompletion: one last call with NSMatchingCompleted (2) or NSMatchingHitEnd (4).
        if ((options & regex::report_completion) && !stopped) static_cast<void>(rx->g.invoke_block(block, {0, 2 | 4, stop.address}));
    }, "v@:@Q{_NSRange=QQ}@?");
    support.method(re, "stringByReplacingMatchesInString:options:range:withTemplate:", [rx](CpuState& s, GuestMemory&) {
        const auto value = rx->g.text(s.x[2]);
        std::size_t count = 0;
        s.x[0] = rx->g.auto_string(rx->replaced(s.x[0], value, rx->checked_range(value, s.x[4], s.x[5]), s.x[3], rx->g.text(s.x[6]), count));
    }, "@@:@Q{_NSRange=QQ}@");
    support.method(re, "replaceMatchesInString:options:range:withTemplate:", [rx](CpuState& s, GuestMemory&) {
        const auto value = rx->g.text(s.x[2]);
        std::size_t count = 0;
        const auto replaced = rx->replaced(s.x[0], value, rx->checked_range(value, s.x[4], s.x[5]), s.x[3], rx->g.text(s.x[6]), count);
        if (count) rx->s.send_id(s.x[2], "setString:", {rx->g.auto_string(replaced)});
        s.x[0] = count;
    }, "Q@:@Q{_NSRange=QQ}@");
    support.method(re, "replacementStringForResult:inString:offset:template:", [rx](CpuState& s, GuestMemory&) {
        const auto st = rx->state(s.x[0]);
        const auto result = rx->s.objc.find_host_state<ResultState>(s.x[2]);
        if (!result) unsupported("-[NSRegularExpression replacementStringForResult:...] with a result of another class");
        regex::Match m;
        const auto offset = static_cast<std::int64_t>(s.x[4]);
        for (auto r : result->ranges) { if (r.location >= 0) r.location += offset; m.groups.push_back(r); }
        s.x[0] = rx->g.auto_string(regex::expand_template(*st->pattern, rx->g.text(s.x[3]), m, rx->g.text(s.x[5])));
    }, "@@:@@q@");
    support.method(re, "copyWithZone:", [rx](CpuState& s, GuestMemory&) { s.x[0] = rx->g.retain(s.x[0]); }, "@@:^v");
    support.method(re, "isEqual:", [rx](CpuState& s, GuestMemory&) {
        if (s.x[2] == s.x[0]) { s.x[0] = 1; return; }
        const auto a = rx->s.objc.find_host_state<RegexState>(s.x[0]);
        const auto b = s.x[2] ? rx->s.objc.find_host_state<RegexState>(s.x[2]) : nullptr;
        return_bool(s, a && b && a->options == b->options && rx->g.is_equal(a->source, b->source));
    }, "B@:@");
    support.method(re, "hash", [rx](CpuState& s, GuestMemory&) {
        const auto st = rx->state(s.x[0]);
        s.x[0] = rx->g.hash(st->source) ^ st->options;
    }, "Q@:");
    support.method(re, "description", [rx](CpuState& s, GuestMemory&) {
        const auto st = rx->state(s.x[0]);
        s.x[0] = rx->g.auto_string(std::string_view("<NSRegularExpression> " + rx->g.utf8(st->source) + " 0x" + std::to_string(st->options)));
    }, "@@:");
    support.method(re, "dealloc", [sp, re](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<RegexState>(s.x[0])) if (st->source) sp->send_id(std::exchange(st->source, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], re, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- NSTextCheckingResult ----
    const auto result_of = [sp](Address self) {
        auto st = sp->objc.find_host_state<ResultState>(self);
        if (!st) fail("NSTextCheckingResult without a match");
        return st;
    };
    support.method(tcr, "resultType", [](CpuState& s, GuestMemory&) { s.x[0] = text_checking_type_regular_expression; }, "Q@:");
    support.method(tcr, "range", [result_of](CpuState& s, GuestMemory&) {
        const auto r = result_of(s.x[0])->ranges[0];
        return_range(s, r.location, r.length);
    }, "{_NSRange=QQ}@:");
    support.method(tcr, "numberOfRanges", [result_of](CpuState& s, GuestMemory&) { s.x[0] = result_of(s.x[0])->ranges.size(); }, "Q@:");
    support.method(tcr, "rangeAtIndex:", [result_of](CpuState& s, GuestMemory&) {
        const auto st = result_of(s.x[0]);
        if (s.x[2] >= st->ranges.size()) fail("NSRangeException: -[NSTextCheckingResult rangeAtIndex:]: index " + std::to_string(s.x[2]) + " out of bounds");
        const auto r = st->ranges[static_cast<std::size_t>(s.x[2])];
        return_range(s, r.location, r.length);
    }, "{_NSRange=QQ}@:Q");
    support.method(tcr, "rangeWithName:", [rx, result_of](CpuState& s, GuestMemory&) {
        const auto st = result_of(s.x[0]);
        const auto expression = rx->s.objc.find_host_state<RegexState>(st->expression);
        const auto index = expression ? expression->pattern->group_named(rx->g.text(s.x[2])) : std::nullopt;
        if (!index) fail("NSInvalidArgumentException: -[NSTextCheckingResult rangeWithName:]: no group named \"" + rx->g.utf8(s.x[2]) + "\"");
        const auto r = st->ranges[*index];
        return_range(s, r.location, r.length);
    }, "{_NSRange=QQ}@:@");
    support.method(tcr, "regularExpression", [result_of](CpuState& s, GuestMemory&) { s.x[0] = result_of(s.x[0])->expression; }, "@@:");
    support.method(tcr, "resultByAdjustingRangesWithOffset:", [rx, result_of](CpuState& s, GuestMemory&) {
        const auto st = result_of(s.x[0]);
        regex::Match m;
        for (auto r : st->ranges) { if (r.location >= 0) r.location += static_cast<std::int64_t>(s.x[2]); m.groups.push_back(r); }
        s.x[0] = rx->g.autorelease(rx->result(st->expression, m));
    }, "@@:q");
    support.method(tcr, "copyWithZone:", [rx](CpuState& s, GuestMemory&) { s.x[0] = rx->g.retain(s.x[0]); }, "@@:^v");
    support.method(tcr, "dealloc", [sp, tcr](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<ResultState>(s.x[0])) if (st->expression) sp->send_id(std::exchange(st->expression, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], tcr, sp->objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit::data
