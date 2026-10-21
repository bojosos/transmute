#include "alchemy/darwin/kit/completion/data/providers.hpp"

#include <bit>

// Keyed coding of the Foundation value classes nibs carry (added where
// missing), in Foundation's keys:
//   NSDate  NS.time (seconds since the reference date, a double)
//   NSURL   NS.base (NSURL or nil) and NS.relative (NSString)
// -initWithCoder: replaces the receiver (a class-cluster placeholder) with
// the decoded CF object, as the other initializers of these classes do.
// NSMutableAttributedString attribute fixing:
//   -fixParagraphStyleAttributeInRange: gives each paragraph the paragraph
//   style of its first character; -setAlignment:range: sets the alignment of
//   the paragraphs in the range (copies of their paragraph styles);
//   -fixFontAttributeInRange: keeps the fonts: Alchemy's text layout falls
//   back per glyph when a font lacks one, so no attribute substitution is
//   needed; -fixAttributesInRange: does both (attachments need no fixing:
//   attachment characters keep their attachment attribute).
namespace alchemy::gui::completion_kit::data {
namespace {

bool paragraph_end(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2029 || c == 0x85; }

// [start, end) paragraph ranges intersecting [location, location + length).
std::vector<std::pair<std::size_t, std::size_t>> paragraphs(const std::u16string& text, std::size_t location, std::size_t length) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    auto start = location;
    while (start > 0 && !paragraph_end(text[start - 1])) --start;
    while (start < location + length || (out.empty() && start <= text.size())) {
        auto end = start;
        while (end < text.size() && !paragraph_end(text[end])) ++end;
        if (end < text.size()) {
            if (text[end] == u'\r' && end + 1 < text.size() && text[end + 1] == u'\n') ++end;
            ++end;
        }
        out.emplace_back(start, end);
        if (end == start) break;
        start = end;
    }
    return out;
}

} // namespace

void install_coding(Support& support) {
    auto g = std::make_shared<Guest>(Guest{support});
    const auto key = [g](const char* k) { return g->s.cf.constant_string(k); };

    // ---- NSDate ----
    if (const auto date = support.objc.find_class("NSDate")) {
        fill_method(support, date, "initWithCoder:", [g, key](CpuState& s, GuestMemory&) {
            CallRegs r;
            r.x[2] = key("NS.time");
            const auto time = std::bit_cast<double>(g->s.send(s.x[2], "decodeDoubleForKey:", r).v[0][0]);
            g->release(s.x[0]);
            s.x[0] = g->s.cf.date(time);
        }, "@@:@");
        fill_method(support, date, "encodeWithCoder:", [g, key](CpuState& s, GuestMemory&) {
            CallRegs r;
            r.v[0] = {std::bit_cast<std::uint64_t>(g->s.cf.date_time(s.x[0])), 0};
            r.x[2] = key("NS.time");
            static_cast<void>(g->s.send(s.x[2], "encodeDouble:forKey:", r));
        }, "v@:@");
        fill_method(support, date, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    }

    // ---- NSURL ----
    if (const auto url = support.objc.find_class("NSURL")) {
        fill_method(support, url, "initWithCoder:", [g, key](CpuState& s, GuestMemory&) {
            const auto base = g->s.send_id(s.x[2], "decodeObjectForKey:", {key("NS.base")});
            const auto relative = g->s.send_id(s.x[2], "decodeObjectForKey:", {key("NS.relative")});
            g->release(s.x[0]);
            if (!relative) { s.x[0] = 0; return; }
            s.x[0] = g->retain(g->s.send_id(g->s.objc.find_class("NSURL"), "URLWithString:relativeToURL:", {relative, base}));
        }, "@@:@");
        fill_method(support, url, "encodeWithCoder:", [g, key](CpuState& s, GuestMemory&) {
            const auto base = g->s.send_id(s.x[0], "baseURL");
            const auto relative = g->s.send_id(s.x[0], "relativeString");
            g->s.send_id(s.x[2], "encodeObject:forKey:", {base, key("NS.base")});
            g->s.send_id(s.x[2], "encodeObject:forKey:", {relative, key("NS.relative")});
        }, "v@:@");
        fill_method(support, url, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    }

    // ---- NSMutableAttributedString ----
    const auto mas = support.objc.find_class("NSMutableAttributedString");
    if (!mas) return;
    const auto paragraph_key = [g]() { return g->s.cf.constant_string("NSParagraphStyle"); };
    const auto attribute_at = [g](Address self, Address name, std::size_t index) {
        CallRegs r;
        r.x[2] = name;
        r.x[3] = index;
        r.x[4] = 0;
        return g->s.send(self, "attribute:atIndex:effectiveRange:", r).x0;
    };
    const auto set_attribute = [g](Address self, Address name, Address value, std::size_t at, std::size_t length) {
        CallRegs r;
        if (value) {
            r.x[2] = name;
            r.x[3] = value;
            r.x[4] = at;
            r.x[5] = length;
            static_cast<void>(g->s.send(self, "addAttribute:value:range:", r));
        } else {
            r.x[2] = name;
            r.x[3] = at;
            r.x[4] = length;
            static_cast<void>(g->s.send(self, "removeAttribute:range:", r));
        }
    };
    const auto checked = [g](Address self, Address location, Address length, const char* what) {
        const auto text = g->text(g->s.send_id(self, "string"));
        if (location > text.size() || length > text.size() - location)
            fail(std::string("NSRangeException: -[NSMutableAttributedString ") + what + "]: range out of bounds");
        return text;
    };
    const auto fix_paragraphs = [g, paragraph_key, attribute_at, set_attribute, checked](Address self, Address location, Address length) {
        const auto text = checked(self, location, length, "fixParagraphStyleAttributeInRange:");
        if (text.empty()) return;
        for (const auto& [start, end] : paragraphs(text, static_cast<std::size_t>(location), static_cast<std::size_t>(length))) {
            if (start >= text.size() || end <= start) continue;
            const auto style = attribute_at(self, paragraph_key(), start);
            set_attribute(self, paragraph_key(), style, start, end - start);
        }
    };
    fill_method(support, mas, "fixParagraphStyleAttributeInRange:", [fix_paragraphs](CpuState& s, GuestMemory&) { fix_paragraphs(s.x[0], s.x[2], s.x[3]); }, "v@:{_NSRange=QQ}");
    fill_method(support, mas, "fixFontAttributeInRange:", [checked](CpuState& s, GuestMemory&) {
        static_cast<void>(checked(s.x[0], s.x[2], s.x[3], "fixFontAttributeInRange:"));
    }, "v@:{_NSRange=QQ}");
    fill_method(support, mas, "fixAttachmentAttributeInRange:", [checked](CpuState& s, GuestMemory&) {
        static_cast<void>(checked(s.x[0], s.x[2], s.x[3], "fixAttachmentAttributeInRange:"));
    }, "v@:{_NSRange=QQ}");
    fill_method(support, mas, "fixAttributesInRange:", [fix_paragraphs, checked](CpuState& s, GuestMemory&) {
        static_cast<void>(checked(s.x[0], s.x[2], s.x[3], "fixAttributesInRange:"));
        fix_paragraphs(s.x[0], s.x[2], s.x[3]);
    }, "v@:{_NSRange=QQ}");
    fill_method(support, mas, "setAlignment:range:", [g, paragraph_key, attribute_at, set_attribute, checked](CpuState& s, GuestMemory&) {
        const auto text = checked(s.x[0], s.x[3], s.x[4], "setAlignment:range:");
        const auto alignment = s.x[2];
        const auto style_class = g->s.objc.find_class("NSParagraphStyle");
        if (!style_class) unsupported("-[NSMutableAttributedString setAlignment:range:] without NSParagraphStyle");
        for (const auto& [start, end] : paragraphs(text, static_cast<std::size_t>(s.x[3]), static_cast<std::size_t>(s.x[4]))) {
            if (end <= start) continue;
            auto style = attribute_at(s.x[0], paragraph_key(), start);
            if (!style) style = g->s.send_id(style_class, "defaultParagraphStyle");
            const auto copy = g->s.send_id(style, "mutableCopy");
            CallRegs a;
            a.x[2] = alignment;
            static_cast<void>(g->s.send(copy, "setAlignment:", a));
            set_attribute(s.x[0], paragraph_key(), copy, start, end - start);
            g->release(copy);
        }
    }, "v@:q{_NSRange=QQ}");
}

} // namespace alchemy::gui::completion_kit::data
