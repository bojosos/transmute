#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/foundation/decimal.hpp"
#include "alchemy/darwin/foundation/scan.hpp"
#include "alchemy/darwin/kit/completion/data/text.hpp"

#include <limits>

// NSScanner over an immutable copy of its string (UTF-16 locations):
// +scannerWithString:, +localizedScannerWithString:, -initWithString:,
// -string, -scanLocation / -setScanLocation:, -charactersToBeSkipped (default:
// white space and newlines), -caseSensitive (default NO), -locale (its
// decimal separator for floating-point scans), -isAtEnd, the string and
// character-set scans, and the number scans (-scanInt:, -scanInteger:,
// -scanLongLong:, -scanUnsignedLongLong:, -scanHexInt:, -scanHexLongLong:,
// -scanFloat:, -scanDouble:, -scanDecimal: through the scan / decimal cores).
// Each scan first skips charactersToBeSkipped; a failed scan leaves the
// location where it was.
namespace alchemy::gui::completion_kit::data {
namespace {

struct ScannerState final : ObjectPayload {
    std::u16string value;
    Address string{};   // +1 copy
    std::size_t location{};
    Address skip{};     // +1, may be nil
    bool case_sensitive{};
    Address locale{};   // +1, may be nil
};

struct Scanning {
    Support& s;
    Guest g{s};

    std::shared_ptr<ScannerState> state(Address self) { return s.objc.host_state<ScannerState>(self); }
    bool member(Address set, char32_t c) {
        CallRegs r;
        r.x[2] = c;
        return (s.send(set, "longCharacterIsMember:", r).x0 & 0xff) != 0;
    }
    char32_t at(const std::u16string& v, std::size_t i, std::size_t& width) {
        width = 1;
        const char32_t u = v[i];
        if (u >= 0xd800 && u <= 0xdbff && i + 1 < v.size() && v[i + 1] >= 0xdc00 && v[i + 1] <= 0xdfff) {
            width = 2;
            return 0x10000 + ((u - 0xd800) << 10) + (static_cast<char32_t>(v[i + 1]) - 0xdc00);
        }
        return u;
    }
    // Run of characters in (or, with `invert`, not in) `set` from `from`.
    std::size_t run(const std::u16string& v, std::size_t from, Address set, bool invert) {
        auto i = from;
        while (i < v.size()) {
            std::size_t w;
            const auto c = at(v, i, w);
            if (member(set, c) == invert) break;
            i += w;
        }
        return i;
    }
    std::size_t skipped(ScannerState& st) { return st.skip ? run(st.value, st.location, st.skip, false) : st.location; }
    void configure(Address self, Address string) {
        auto st = state(self);
        st->value = g.text(string);
        st->string = string ? s.send_id(string, "copy") : g.string(u"");
        st->location = 0;
        st->skip = g.retain(s.send_id(s.objc.find_class("NSCharacterSet"), "whitespaceAndNewlineCharacterSet"));
    }
    char16_t separator(ScannerState& st) {
        if (!st.locale) return u'.';
        const auto value = s.send_id(st.locale, "decimalSeparator");
        const auto text = g.text(value);
        return text.size() == 1 ? text[0] : u'.';
    }
    // Stores `result` (+0) into a guest out parameter.
    void store_string(Address out, const std::u16string& v, std::size_t from, std::size_t to) {
        if (out) g.memory().write64(out, g.auto_string(std::u16string_view(v).substr(from, to - from)));
    }
};

} // namespace

void install_scanner(Support& support) {
    auto sc = std::make_shared<Scanning>(Scanning{support});
    auto* sp = &support;
    const auto scanner = support.declare_class("NSScanner", "NSObject", foundation_library);

    support.method(scanner, "scannerWithString:", [sc](CpuState& s, GuestMemory&) {
        const auto self = sc->s.objc.allocate(s.x[0]);
        sc->configure(self, s.x[2]);
        s.x[0] = sc->g.autorelease(self);
    }, "@@:@", true);
    support.method(scanner, "localizedScannerWithString:", [sc](CpuState& s, GuestMemory&) {
        const auto self = sc->s.objc.allocate(s.x[0]);
        sc->configure(self, s.x[2]);
        sc->state(self)->locale = sc->g.retain(sc->s.send_id(sc->s.objc.find_class("NSLocale"), "currentLocale"));
        s.x[0] = sc->g.autorelease(self);
    }, "@@:@", true);
    support.method(scanner, "initWithString:", [sc](CpuState& s, GuestMemory&) { sc->configure(s.x[0], s.x[2]); }, "@@:@");
    support.method(scanner, "string", [sc](CpuState& s, GuestMemory&) { s.x[0] = sc->state(s.x[0])->string; }, "@@:");
    support.method(scanner, "scanLocation", [sc](CpuState& s, GuestMemory&) { s.x[0] = sc->state(s.x[0])->location; }, "Q@:");
    support.method(scanner, "currentIndex", [sc](CpuState& s, GuestMemory&) { s.x[0] = sc->state(s.x[0])->location; }, "Q@:");
    support.method(scanner, "setScanLocation:", [sc](CpuState& s, GuestMemory&) {
        const auto st = sc->state(s.x[0]);
        if (s.x[2] > st->value.size()) fail("NSRangeException: -[NSScanner setScanLocation:]: index " + std::to_string(s.x[2]) + " beyond bounds; length " + std::to_string(st->value.size()));
        st->location = static_cast<std::size_t>(s.x[2]);
    }, "v@:Q");
    support.method(scanner, "charactersToBeSkipped", [sc](CpuState& s, GuestMemory&) { s.x[0] = sc->state(s.x[0])->skip; }, "@@:");
    support.method(scanner, "setCharactersToBeSkipped:", [sc](CpuState& s, GuestMemory&) {
        const auto st = sc->state(s.x[0]);
        const auto old = std::exchange(st->skip, s.x[2] ? sc->s.send_id(s.x[2], "copy") : 0);
        sc->g.release(old);
    }, "v@:@");
    support.method(scanner, "caseSensitive", [sc](CpuState& s, GuestMemory&) { s.x[0] = sc->state(s.x[0])->case_sensitive; }, "B@:");
    support.method(scanner, "setCaseSensitive:", [sc](CpuState& s, GuestMemory&) { sc->state(s.x[0])->case_sensitive = arg_bool(s.x[2]); }, "v@:B");
    support.method(scanner, "locale", [sc](CpuState& s, GuestMemory&) { s.x[0] = sc->state(s.x[0])->locale; }, "@@:");
    support.method(scanner, "setLocale:", [sc](CpuState& s, GuestMemory&) {
        const auto st = sc->state(s.x[0]);
        const auto old = std::exchange(st->locale, sc->g.retain(s.x[2]));
        sc->g.release(old);
    }, "v@:@");
    support.method(scanner, "isAtEnd", [sc](CpuState& s, GuestMemory&) {
        const auto st = sc->state(s.x[0]);
        return_bool(s, sc->skipped(*st) >= st->value.size());
    }, "B@:");

    // ---- strings and character sets ----
    support.method(scanner, "scanString:intoString:", [sc](CpuState& s, GuestMemory&) {
        const auto st = sc->state(s.x[0]);
        const auto from = sc->skipped(*st);
        const auto target = sc->g.text(s.x[2]);
        const auto options = st->case_sensitive ? 0 : text::case_insensitive_search;
        const auto f = target.empty() || from + target.size() > st->value.size() ? text::Found{}
                     : text::find(st->value, target, options | text::anchored_search, static_cast<std::int64_t>(from), static_cast<std::int64_t>(target.size()));
        if (f.location < 0) { return_bool(s, false); return; }
        sc->store_string(s.x[3], st->value, from, from + target.size());
        st->location = from + target.size();
        return_bool(s, true);
    }, "B@:@^@");
    support.method(scanner, "scanUpToString:intoString:", [sc](CpuState& s, GuestMemory&) {
        const auto st = sc->state(s.x[0]);
        const auto from = sc->skipped(*st);
        const auto target = sc->g.text(s.x[2]);
        const auto options = st->case_sensitive ? 0 : text::case_insensitive_search;
        const auto rest = static_cast<std::int64_t>(st->value.size() - from);
        const auto f = target.empty() ? text::Found{} : text::find(st->value, target, options, static_cast<std::int64_t>(from), rest);
        const auto end = f.location < 0 ? st->value.size() : static_cast<std::size_t>(f.location);
        if (end == from) { return_bool(s, false); return; }
        sc->store_string(s.x[3], st->value, from, end);
        st->location = end;
        return_bool(s, true);
    }, "B@:@^@");
    for (const auto invert : {false, true})
        support.method(scanner, invert ? "scanUpToCharactersFromSet:intoString:" : "scanCharactersFromSet:intoString:", [sc, invert](CpuState& s, GuestMemory&) {
            const auto st = sc->state(s.x[0]);
            const auto from = sc->skipped(*st);
            if (!s.x[2]) fail("NSInvalidArgumentException: -[NSScanner scanCharactersFromSet:intoString:]: nil character set");
            const auto end = sc->run(st->value, from, s.x[2], invert);
            if (end == from) { return_bool(s, false); return; }
            sc->store_string(s.x[3], st->value, from, end);
            st->location = end;
            return_bool(s, true);
        }, "B@:@^@");

    // ---- numbers ----
    struct IntegerScan { const char* name; std::int64_t min, max; unsigned bytes; };
    for (const auto& spec : {IntegerScan{"scanInt:", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(), 4},
                             IntegerScan{"scanInteger:", std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), 8},
                             IntegerScan{"scanLongLong:", std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), 8}}) {
        support.method(scanner, spec.name, [sc, spec](CpuState& s, GuestMemory& m) {
            const auto st = sc->state(s.x[0]);
            const auto from = sc->skipped(*st);
            const auto r = scan::integer(st->value, from, spec.min, spec.max);
            if (!r) { return_bool(s, false); return; }
            if (s.x[2]) { if (spec.bytes == 4) m.write32(s.x[2], static_cast<std::uint32_t>(r->first)); else m.write64(s.x[2], static_cast<std::uint64_t>(r->first)); }
            st->location = r->second;
            return_bool(s, true);
        }, spec.bytes == 4 ? "B@:^i" : "B@:^q");
    }
    support.method(scanner, "scanUnsignedLongLong:", [sc](CpuState& s, GuestMemory& m) {
        const auto st = sc->state(s.x[0]);
        const auto r = scan::unsigned_integer(st->value, sc->skipped(*st));
        if (!r) { return_bool(s, false); return; }
        if (s.x[2]) m.write64(s.x[2], r->first);
        st->location = r->second;
        return_bool(s, true);
    }, "B@:^Q");
    for (const auto wide : {false, true})
        support.method(scanner, wide ? "scanHexLongLong:" : "scanHexInt:", [sc, wide](CpuState& s, GuestMemory& m) {
            const auto st = sc->state(s.x[0]);
            const auto r = scan::hex(st->value, sc->skipped(*st), wide ? std::numeric_limits<std::uint64_t>::max() : std::numeric_limits<std::uint32_t>::max());
            if (!r) { return_bool(s, false); return; }
            if (s.x[2]) { if (wide) m.write64(s.x[2], r->first); else m.write32(s.x[2], static_cast<std::uint32_t>(r->first)); }
            st->location = r->second;
            return_bool(s, true);
        }, wide ? "B@:^Q" : "B@:^I");
    for (const auto single : {false, true})
        support.method(scanner, single ? "scanFloat:" : "scanDouble:", [sc, single](CpuState& s, GuestMemory& m) {
            const auto st = sc->state(s.x[0]);
            const auto r = scan::floating(st->value, sc->skipped(*st), sc->separator(*st));
            if (!r) { return_bool(s, false); return; }
            if (s.x[2]) {
                if (single) m.write32(s.x[2], std::bit_cast<std::uint32_t>(static_cast<float>(r->first)));
                else m.write64(s.x[2], std::bit_cast<std::uint64_t>(r->first));
            }
            st->location = r->second;
            return_bool(s, true);
        }, single ? "B@:^f" : "B@:^d");
    support.method(scanner, "scanDecimal:", [sc](CpuState& s, GuestMemory& m) {
        const auto st = sc->state(s.x[0]);
        const auto from = sc->skipped(*st);
        const char16_t separator[1] = {sc->separator(*st)};
        std::size_t used = 0;
        const auto parsed = decimal::parse(std::u16string_view(st->value).substr(from), std::u16string_view(separator, 1), &used);
        if (!parsed) { return_bool(s, false); return; }
        if (s.x[2]) { const auto bytes = decimal::pack(*parsed); m.write_bytes(s.x[2], bytes); }
        st->location = from + used;
        return_bool(s, true);
    }, "B@:^{NSDecimal=b8b4b1b1b18[8S]}");

    support.method(scanner, "copyWithZone:", [sc](CpuState& s, GuestMemory&) {
        const auto st = sc->state(s.x[0]);
        const auto copy = sc->s.objc.allocate(sc->s.objc.object_class(s.x[0]));
        sc->configure(copy, st->string);
        auto other = sc->state(copy);
        other->location = st->location;
        sc->g.release(std::exchange(other->skip, sc->g.retain(st->skip)));
        other->case_sensitive = st->case_sensitive;
        other->locale = sc->g.retain(st->locale);
        s.x[0] = copy;
    }, "@@:^v");
    support.method(scanner, "dealloc", [sp, scanner](CpuState& s, GuestMemory&) {
        if (const auto st = sp->objc.find_host_state<ScannerState>(s.x[0]))
            for (auto* held : {&st->string, &st->skip, &st->locale}) if (*held) sp->send_id(std::exchange(*held, 0), "release");
        static_cast<void>(sp->objc.send_super(s.x[0], scanner, sp->objc.selector("dealloc")));
    }, "v@:");
}

} // namespace alchemy::gui::completion_kit::data
