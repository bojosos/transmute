#include "alchemy/darwin/appkit/controls/g3.hpp"
#include "alchemy/darwin/appkit/rtf.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <string>
#include <vector>

// NSText's and NSTextView's actions and the rest of their API over the
// TextKit objects (text_system.cpp) and the view's own text (text.cpp):
// * Attribute actions (alignment, -changeFont:, -changeAttributes:,
//   -changeColor:, underline, super- and subscript, outline, the font and
//   ruler pasteboards) change the selected text of a rich text view through
//   its storage (the selected paragraphs for paragraph attributes), the
//   typing attributes when nothing is selected, and the whole text of a
//   plain text view; each change asks -shouldChangeTextInRange:... first and
//   ends with -didChangeText. The view draws in its first attributes, so a
//   change that reaches the first character or paragraph shows at once.
// * RTF: -RTFFromRange:, -replaceCharactersInRange:withRTF: and the font and
//   ruler pasteboards exchange rtf.hpp's subset (font family, size, bold,
//   italic, underline, colour, alignment and indents); RTFD data (flattened
//   file wrappers) stops explicitly, and -writeRTFDToFile:atomically: writes
//   an RTFD directory holding TXT.rtf (text with attachments stops).
// * Find: -performFindPanelAction: / -performTextFinderAction: are
//   NSTextFinder's actions (text_finder.cpp) with the view as the client.
// * Spelling: -checkSpelling: selects the next misspelled word;
//   -checkTextInDocument: / -checkTextInSelection: mark misspellings with the
//   layout manager's NSSpellingState temporary attribute. No Spelling and
//   Grammar panel exists (-showGuessPanel: stops explicitly).
// * Kept settings: glyph info, input source locales, document background
//   colour changes, adaptive colour mapping, Writing Tools behaviour (Writing
//   Tools is never active), selection affinity and granularity.
// * Undo is not provided (text.cpp), so typing is never being coalesced.
// * Explicit stops: vertical layout orientation, the link / list / table
//   panels, dragging the selection, TextKit 2 text views.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

using Range = std::pair<std::size_t, std::size_t>;
constexpr std::uint64_t not_found = static_cast<std::uint64_t>(INT64_MAX);
constexpr std::string_view rtf_type = "public.rtf", string_type = "public.utf8-plain-text";
constexpr std::string_view font_type = "com.apple.cocoa.pasteboard.character-formatting", ruler_type = "com.apple.cocoa.pasteboard.paragraph-formatting";
constexpr std::string_view font_pasteboard = "Apple CFPasteboard font", ruler_pasteboard = "Apple CFPasteboard ruler";

Range selected(AppKit& k, Address view) {
    const auto r = k.send(view, "selectedRange");
    return {r.x0, r.x1};
}
std::u16string text_of(AppKit& k, Address view) { return k.string16(k.send_id(view, "string")); }
bool may_change(AppKit& k, Address view, Range r, Address replacement) {
    if (!k.responds(view, "shouldChangeTextInRange:replacementString:")) return k.send_bool(view, "isEditable");
    return k.send_bool(view, "shouldChangeTextInRange:replacementString:", {r.first, r.second, replacement});
}
Address attribute_at(AppKit& k, Address storage, std::size_t index, std::string_view key) {
    return k.send_id(storage, "attribute:atIndex:effectiveRange:", {k.auto_string(key), index, 0});
}
// The runs of one attribute (its value, 0 where absent) within r.
std::vector<std::pair<Range, Address>> runs_of(AppKit& k, Address storage, std::string_view key, Range r) {
    std::vector<std::pair<Range, Address>> runs;
    const auto effective = k.process.allocate_guest(16, 8);
    for (auto at = r.first; at < r.first + r.second;) {
        const auto value = k.send_id(storage, "attribute:atIndex:effectiveRange:", {k.auto_string(key), at, effective});
        const auto end = std::clamp<std::size_t>(k.memory().read64(effective) + k.memory().read64(effective + 8), at + 1, r.first + r.second);
        runs.push_back({{at, end - at}, value});
        at = end;
    }
    k.process.release_guest(effective);
    return runs;
}
// Every attribute run (the whole dictionary) within r.
std::vector<std::pair<Range, Address>> attribute_runs(AppKit& k, Address storage, Range r) {
    std::vector<std::pair<Range, Address>> runs;
    const auto effective = k.process.allocate_guest(16, 8);
    for (auto at = r.first; at < r.first + r.second;) {
        const auto value = k.send_id(storage, "attributesAtIndex:effectiveRange:", {at, effective});
        const auto end = std::clamp<std::size_t>(k.memory().read64(effective) + k.memory().read64(effective + 8), at + 1, r.first + r.second);
        runs.push_back({{at, end - at}, value});
        at = end;
    }
    k.process.release_guest(effective);
    return runs;
}
// A copy of `base` (may be nil) with keys set (value 0: removed); autoreleased.
Address dictionary_with(AppKit& k, Address base, const std::vector<std::pair<std::string, Address>>& changes) {
    auto entries = base ? k.cf.dictionary_entries(base) : std::vector<std::pair<Address, Address>>{};
    for (const auto& [key, value] : changes) {
        std::erase_if(entries, [&](const auto& entry) { return k.text(entry.first) == key; });
        if (value) entries.emplace_back(k.auto_string(key), value);
    }
    return k.autorelease(k.cf.dictionary(entries));
}
std::u16string paragraph_separators() { return {u'\n', u'\r', char16_t{0x2029}, char16_t{0x0085}}; }
// The paragraphs a range touches (with their separators).
Range paragraph_range(const std::u16string& text, Range r) {
    const auto separators = paragraph_separators();
    const auto is_separator = [&](char16_t c) { return separators.find(c) != std::u16string::npos; };
    auto start = std::min(r.first, text.size()), end = std::min(r.first + r.second, text.size());
    while (start > 0 && !is_separator(text[start - 1])) --start;
    if (end == start || (end > 0 && !is_separator(text[end - 1]))) {
        while (end < text.size() && !is_separator(text[end])) ++end;
        if (end < text.size()) ++end;
    }
    return {start, end - start};
}
Address shared_font_manager(AppKit& k) { return k.send_id(k.objc.find_class("NSFontManager"), "sharedFontManager"); }
Address default_paragraph(AppKit& k) { return k.send_id(k.objc.find_class("NSParagraphStyle"), "defaultParagraphStyle"); }
void fill(AppKit& k, Address color, const Rect& rect) {
    static_cast<void>(k.send(color, "set"));
    const auto found = k.process.resolve(appkit_library, "_NSRectFillUsingOperation");
    if (!found) unsupported("NSRectFillUsingOperation (the AppKit C functions are not installed)");
    CallRegs registers;
    put_rect(registers, 0, rect);
    registers.x[0] = 2;   // NSCompositingOperationSourceOver
    static_cast<void>(EngineAdapter(k.process.calling_engine()).call_regs(found->address, registers));
}

// Applies `apply(storage, range)` to the selection of a rich text view (the
// selected paragraphs when `paragraphs`), or to the whole text of a plain one;
// with nothing selected in a rich view, `typing` changes the typing attributes.
void change(AppKit& k, Address view, const std::function<void(Address, Range)>& apply, const std::function<Address(Address)>& typing,
            bool paragraphs = false) {
    const bool rich = k.send_bool(view, "isRichText");
    const auto text = text_of(k, view);
    Range r = rich ? selected(k, view) : Range{0, text.size()};
    if (rich && paragraphs) r = paragraph_range(text, r);
    if (r.second == 0) {
        if (typing) static_cast<void>(k.send_id(view, "setTypingAttributes:", {typing(k.send_id(view, "typingAttributes"))}));
        return;
    }
    if (!may_change(k, view, r, 0)) return;
    const auto storage = k.send_id(view, "textStorage");
    static_cast<void>(k.send_id(storage, "beginEditing"));
    try {
        apply(storage, r);
    } catch (...) {
        static_cast<void>(k.send_id(storage, "endEditing"));
        throw;
    }
    static_cast<void>(k.send_id(storage, "endEditing"));
    static_cast<void>(k.send_id(view, "didChangeText"));
}
// Sets one character attribute (computed from its current value) over the change's range or typing attributes.
void change_attribute(AppKit& k, Address view, std::string_view key, const std::function<Address(Address)>& value_for) {
    const std::string name(key);
    change(k, view, [&k, name, value_for](Address storage, Range r) {
        for (const auto& [run, old] : runs_of(k, storage, name, r)) {
            const auto value = value_for(old);
            if (value) static_cast<void>(k.send_id(storage, "addAttribute:value:range:", {k.auto_string(name), value, run.first, run.second}));
            else static_cast<void>(k.send_id(storage, "removeAttribute:range:", {k.auto_string(name), run.first, run.second}));
        }
    }, [&k, name, value_for](Address typing) {
        return dictionary_with(k, typing, {{name, value_for(typing ? dictionary_get(k, typing, name) : 0)}});
    });
}
// The view's font where no attribute gives one.
Address view_font(AppKit& k, Address view) {
    if (const auto font = k.send_id(view, "font")) return font;
    return k.send(k.objc.find_class("NSFont"), "userFontOfSize:", [] { CallRegs r; r.set_d(0, 12); return r; }()).x0;
}
// Paragraph alignment over the selected paragraphs; the view's own alignment follows a change to its first paragraph.
void align(AppKit& k, Address view, std::int64_t alignment) {
    const auto with_alignment = [&k, alignment](Address style) {
        const auto copy = k.autorelease(k.send(style ? style : default_paragraph(k), "mutableCopy").x0);
        static_cast<void>(k.send_id(copy, "setAlignment:", {static_cast<Address>(alignment)}));
        return copy;
    };
    change(k, view, [&k, with_alignment](Address storage, Range r) {
        for (const auto& [run, style] : runs_of(k, storage, "NSParagraphStyle", r))
            static_cast<void>(k.send_id(storage, "addAttribute:value:range:", {k.auto_string("NSParagraphStyle"), with_alignment(style), run.first, run.second}));
    }, [&k, with_alignment](Address typing) {
        return dictionary_with(k, typing, {{"NSParagraphStyle", with_alignment(typing ? dictionary_get(k, typing, "NSParagraphStyle") : 0)}});
    }, true);
    const auto text = text_of(k, view);
    if (!k.send_bool(view, "isRichText") || paragraph_range(text, selected(k, view)).first == 0)
        static_cast<void>(k.send_id(view, "setAlignment:", {static_cast<Address>(alignment)}));
}

// ---- RTF <-> attributed text ----------------------------------------------------------------
rtf::Format format_of(AppKit& k, Address attributes) {
    rtf::Format f;
    if (!attributes) return f;
    if (const auto font = dictionary_get(k, attributes, "NSFont")) {
        f.font = k.text(k.send_id(font, "familyName"));
        f.size = k.send(font, "pointSize").d(0);
        const auto traits = k.send_id(shared_font_manager(k), "traitsOfFont:", {font});
        f.bold = (traits & 2) != 0;     // NSBoldFontMask
        f.italic = (traits & 1) != 0;   // NSItalicFontMask
    }
    if (const auto color = dictionary_get(k, attributes, "NSColor")) {
        const auto rgba = k.process.allocate_guest(32, 8);
        static_cast<void>(k.send_id(color, "getRed:green:blue:alpha:", {rgba, rgba + 8, rgba + 16, rgba + 24}));
        f.color = std::array<double, 3>{std::bit_cast<double>(k.memory().read64(rgba)), std::bit_cast<double>(k.memory().read64(rgba + 8)),
                                        std::bit_cast<double>(k.memory().read64(rgba + 16))};
        k.process.release_guest(rgba);
    }
    if (const auto underline = dictionary_get(k, attributes, "NSUnderline")) f.underline = k.cf.number_value(underline).value_or(0) != 0;
    if (const auto paragraph = dictionary_get(k, attributes, "NSParagraphStyle")) {
        f.alignment = static_cast<int>(static_cast<std::int64_t>(k.send_id(paragraph, "alignment")));
        const double head = k.send(paragraph, "headIndent").d(0), first = k.send(paragraph, "firstLineHeadIndent").d(0), tail = k.send(paragraph, "tailIndent").d(0);
        f.left_indent = head;
        f.first_indent = first - head;
        f.right_indent = tail < 0 ? -tail : 0;
    }
    return f;
}
// Attribute entries of a format: the character attributes and / or the paragraph style.
std::vector<std::pair<std::string, Address>> attributes_of(AppKit& k, const rtf::Format& f, bool characters, bool paragraph) {
    std::vector<std::pair<std::string, Address>> entries;
    if (characters) {
        Address font{};
        if (!f.font.empty()) {
            CallRegs make;
            make.x[2] = k.auto_string(f.font);
            make.x[3] = (f.bold ? 2 : 0) | (f.italic ? 1 : 0);
            make.x[4] = 5;   // regular weight
            make.set_d(0, f.size);
            font = k.send(shared_font_manager(k), "fontWithFamily:traits:weight:size:", make).x0;
        }
        if (!font) font = k.send(k.objc.find_class("NSFont"), "userFontOfSize:", [&] { CallRegs r; r.set_d(0, f.size); return r; }()).x0;
        entries.emplace_back("NSFont", font);
        Address color{};
        if (f.color) {
            CallRegs make;
            for (unsigned i = 0; i < 3; ++i) make.set_d(i, (*f.color)[i]);
            make.set_d(3, 1);
            color = k.send(k.objc.find_class("NSColor"), "colorWithSRGBRed:green:blue:alpha:", make).x0;
        }
        entries.emplace_back("NSColor", color);
        entries.emplace_back("NSUnderline", f.underline ? k.autorelease(k.cf.integer(1)) : 0);
    }
    if (paragraph) {
        Address style{};
        if (f.alignment != 4 || f.left_indent != 0 || f.first_indent != 0 || f.right_indent != 0) {
            style = k.autorelease(k.alloc_init(k.objc.find_class("NSMutableParagraphStyle")));
            static_cast<void>(k.send_id(style, "setAlignment:", {static_cast<Address>(f.alignment)}));
            for (const auto& [setter, value] : {std::pair{"setHeadIndent:", f.left_indent}, std::pair{"setFirstLineHeadIndent:", f.left_indent + f.first_indent},
                                                std::pair{"setTailIndent:", -f.right_indent}}) {
                CallRegs r;
                r.set_d(0, value);
                static_cast<void>(k.send(style, setter, r));
            }
        }
        entries.emplace_back("NSParagraphStyle", style);
    }
    return entries;
}
std::vector<rtf::Run> runs_from(AppKit& k, Address view, Range r) {
    std::vector<rtf::Run> runs;
    const auto text = text_of(k, view);
    if (k.responds(view, "textStorage")) {
        const auto storage = k.send_id(view, "textStorage");
        for (const auto& [run, attributes] : attribute_runs(k, storage, r)) runs.push_back({text.substr(run.first, run.second), format_of(k, attributes)});
        return runs;
    }
    // A plain NSText: its own font and colour.
    const auto attributes = dictionary_with(k, 0, {{"NSFont", view_font(k, view)}, {"NSColor", k.send_id(view, "textColor")}});
    runs.push_back({text.substr(r.first, r.second), format_of(k, attributes)});
    return runs;
}
Address attributed_from(AppKit& k, const std::vector<rtf::Run>& runs) {
    const auto result = k.autorelease(k.alloc_init(k.objc.find_class("NSMutableAttributedString")));
    for (const auto& run : runs) {
        const auto string = k.cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(run.text.data()), run.text.size()));
        const auto attributes = dictionary_with(k, 0, attributes_of(k, run.format, true, true));
        const auto piece = k.send_id(k.send(k.objc.find_class("NSAttributedString"), "alloc").x0, "initWithString:attributes:", {string, attributes});
        static_cast<void>(k.send_id(result, "appendAttributedString:", {piece}));
        k.release(piece);
        k.release(string);
    }
    return result;
}
Address data_of(AppKit& k, const std::string& bytes) {
    const auto buffer = k.process.allocate_guest(std::max<std::size_t>(1, bytes.size()), 8);
    for (std::size_t i = 0; i < bytes.size(); ++i) k.memory().write8(buffer + i, static_cast<std::uint8_t>(bytes[i]));
    const auto data = k.send_id(k.objc.find_class("NSData"), "dataWithBytes:length:", {buffer, bytes.size()});
    k.process.release_guest(buffer);
    return data;
}
std::vector<rtf::Run> runs_of_data(AppKit& k, Address data) {
    if (!data) return {};
    const auto bytes = k.data_bytes(data);
    if (!rtf::looks_like_rtf(bytes)) fail("pasteboard formatting data is not RTF");
    return rtf::runs(bytes);
}
void write_pasteboard(AppKit& k, std::string_view name, std::string_view type, Address data) {
    const auto board = k.pasteboard_named(name);
    static_cast<void>(k.send_id(board, "clearContents"));
    static_cast<void>(k.send_id(board, "setData:forType:", {data, k.auto_string(type)}));
}
// The attributes at the start of the selection (the typing attributes in empty text).
Address attributes_at_selection(AppKit& k, Address view) {
    const auto text = text_of(k, view);
    if (text.empty() || !k.responds(view, "textStorage")) return k.send_id(view, "typingAttributes");
    const auto index = std::min(selected(k, view).first, text.size() - 1);
    return k.send_id(k.send_id(view, "textStorage"), "attributesAtIndex:effectiveRange:", {index, 0});
}
void apply_format(AppKit& k, Address view, const rtf::Format& f, bool characters) {
    const auto entries = attributes_of(k, f, characters, !characters);
    change(k, view, [&k, entries](Address storage, Range r) {
        for (const auto& [key, value] : entries) {
            if (value) static_cast<void>(k.send_id(storage, "addAttribute:value:range:", {k.auto_string(key), value, r.first, r.second}));
            else static_cast<void>(k.send_id(storage, "removeAttribute:range:", {k.auto_string(key), r.first, r.second}));
        }
    }, [&k, entries](Address typing) { return dictionary_with(k, typing, entries); }, !characters);
}
bool contains_attachment(AppKit& k, Address view) {
    if (!k.responds(view, "textStorage")) return false;
    const auto storage = k.send_id(view, "textStorage");
    const auto length = static_cast<std::size_t>(k.send(storage, "length").x0);
    for (const auto& [run, value] : runs_of(k, storage, "NSAttachment", {0, length}))
        if (value) return true;
    return false;
}

} // namespace

void install_text_actions(AppKit& kit) {
    auto* const k = &kit;
    const auto t = kit.objc.find_class("NSText");
    const auto tv = kit.text_view;
    if (!t || !tv) fail("NSText's actions need NSText and NSTextView installed first");
    for (const auto& [name, value] : {std::pair{"_NSPasteboardNameFont", font_pasteboard}, std::pair{"_NSPasteboardNameRuler", ruler_pasteboard},
                                      std::pair{"_NSFontPboard", font_pasteboard}, std::pair{"_NSRulerPboard", ruler_pasteboard}})
        if (!kit.process.resolve(appkit_library, name)) kit.string_constant(name, std::string(value));

    // ---- NSText: attributes ----
    for (const auto& [name, alignment] : {std::pair{"alignLeft:", 0}, std::pair{"alignCenter:", 1}, std::pair{"alignRight:", 2}}) {
        const auto value = alignment;
        kit.method(t, name, [k, value](CpuState& s, GuestMemory&) { align(*k, s.x[0], value); }, "v@:@");
    }
    kit.method(tv, "alignJustified:", [k](CpuState& s, GuestMemory&) { align(*k, s.x[0], 3); }, "v@:@");
    // The sender (the font manager) converts each run's font.
    kit.method(t, "changeFont:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto sender = s.x[2] ? s.x[2] : shared_font_manager(*k);
        change_attribute(*k, view, "NSFont", [k, view, sender](Address old) { return k->send_id(sender, "convertFont:", {old ? old : view_font(*k, view)}); });
    }, "v@:@");
    kit.method(tv, "changeAttributes:", [k](CpuState& s, GuestMemory&) {
        const auto sender = s.x[2];
        if (!sender) return;
        change(*k, s.x[0], [k, sender](Address storage, Range r) {
            for (const auto& [run, attributes] : attribute_runs(*k, storage, r)) {
                const auto converted = k->send_id(sender, "convertAttributes:", {attributes});
                if (converted) static_cast<void>(k->send_id(storage, "setAttributes:range:", {converted, run.first, run.second}));
            }
        }, [k, sender](Address typing) { return k->send_id(sender, "convertAttributes:", {typing ? typing : k->autorelease(k->cf.dictionary({}))}); });
    }, "v@:@");
    kit.method(tv, "changeColor:", [k](CpuState& s, GuestMemory&) {
        const auto color = s.x[2] ? k->send_id(s.x[2], "color") : 0;
        change_attribute(*k, s.x[0], "NSColor", [color](Address) { return color; });
    }, "v@:@");
    kit.method(t, "underline:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        // On or off for the whole range, from its first character (or the typing attributes).
        const auto attributes = attributes_at_selection(*k, view);
        const auto first = attributes ? dictionary_get(*k, attributes, "NSUnderline") : 0;
        const bool on = !first || k->cf.number_value(first).value_or(0) == 0;
        change_attribute(*k, view, "NSUnderline", [k, on](Address) { return on ? k->autorelease(k->cf.integer(1)) : Address{}; });
    }, "v@:@");
    for (const auto& [name, step] : {std::pair{"superscript:", 1}, std::pair{"subscript:", -1}, std::pair{"unscript:", 0}}) {
        const auto delta = step;
        kit.method(t, name, [k, delta](CpuState& s, GuestMemory&) {
            change_attribute(*k, s.x[0], "NSSuperScript", [k, delta](Address old) -> Address {
                if (delta == 0) return 0;
                const auto level = static_cast<std::int64_t>(old ? k->cf.number_value(old).value_or(0) : 0) + delta;
                return level == 0 ? Address{} : k->autorelease(k->cf.integer(level));
            });
        }, "v@:@");
    }
    kit.method(tv, "outline:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto attributes = attributes_at_selection(*k, view);
        const auto first = attributes ? dictionary_get(*k, attributes, "NSStrokeWidth") : 0;
        const bool on = !first || k->cf.number_value(first).value_or(0) == 0;
        change_attribute(*k, view, "NSStrokeWidth", [k, on](Address) { return on ? k->autorelease(k->cf.real(3)) : Address{}; });
    }, "v@:@");
    const auto set_range_attribute = [k](Address view, std::string_view key, Address value, Range r) {
        const auto storage = k->send_id(view, "textStorage");
        const auto length = static_cast<std::size_t>(k->send(storage, "length").x0);
        if (r.first > length || r.second > length - r.first) fail("NSText attribute range beyond the text (NSRangeException)");
        if (!r.second || !may_change(*k, view, r, 0)) return;
        if (value) static_cast<void>(k->send_id(storage, "addAttribute:value:range:", {k->auto_string(key), value, r.first, r.second}));
        else static_cast<void>(k->send_id(storage, "removeAttribute:range:", {k->auto_string(key), r.first, r.second}));
        static_cast<void>(k->send_id(view, "didChangeText"));
    };
    kit.method(t, "setFont:range:", [set_range_attribute](CpuState& s, GuestMemory&) { set_range_attribute(s.x[0], "NSFont", s.x[2], {s.x[3], s.x[4]}); },
               "v@:@{_NSRange=QQ}");
    kit.method(t, "setTextColor:range:", [set_range_attribute](CpuState& s, GuestMemory&) { set_range_attribute(s.x[0], "NSColor", s.x[2], {s.x[3], s.x[4]}); },
               "v@:@{_NSRange=QQ}");
    // The font and ruler pasteboards (RTF of the formatting).
    kit.method(t, "copyFont:", [k](CpuState& s, GuestMemory&) {
        const auto format = format_of(*k, attributes_at_selection(*k, s.x[0]));
        write_pasteboard(*k, font_pasteboard, font_type, data_of(*k, rtf::write({{u" ", format}})));
    }, "v@:@");
    kit.method(t, "pasteFont:", [k](CpuState& s, GuestMemory&) {
        const auto runs = runs_of_data(*k, k->send_id(k->pasteboard_named(font_pasteboard), "dataForType:", {k->auto_string(font_type)}));
        if (!runs.empty()) apply_format(*k, s.x[0], runs.front().format, true);
    }, "v@:@");
    kit.method(t, "copyRuler:", [k](CpuState& s, GuestMemory&) {
        const auto format = format_of(*k, attributes_at_selection(*k, s.x[0]));
        write_pasteboard(*k, ruler_pasteboard, ruler_type, data_of(*k, rtf::write({{u" ", format}})));
    }, "v@:@");
    kit.method(t, "pasteRuler:", [k](CpuState& s, GuestMemory&) {
        const auto runs = runs_of_data(*k, k->send_id(k->pasteboard_named(ruler_pasteboard), "dataForType:", {k->auto_string(ruler_type)}));
        if (!runs.empty()) apply_format(*k, s.x[0], runs.front().format, false);
    }, "v@:@");

    // ---- NSText: RTF ----
    kit.method(t, "RTFFromRange:", [k](CpuState& s, GuestMemory&) {
        const auto text = text_of(*k, s.x[0]);
        const auto location = std::min<std::size_t>(s.x[2], text.size());
        const auto length = std::min<std::size_t>(s.x[3], text.size() - location);
        s.x[0] = data_of(*k, rtf::write(runs_from(*k, s.x[0], {location, length})));
    }, "@@:{_NSRange=QQ}");
    kit.method(t, "replaceCharactersInRange:withRTF:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const Range r{s.x[2], s.x[3]};
        const auto runs = runs_of_data(*k, s.x[4]);
        if (!may_change(*k, view, r, 0)) return;
        if (k->send_bool(view, "isRichText") && k->responds(view, "textStorage")) {
            static_cast<void>(k->send_id(k->send_id(view, "textStorage"), "replaceCharactersInRange:withAttributedString:", {r.first, r.second, attributed_from(*k, runs)}));
        } else {
            std::u16string plain;
            for (const auto& run : runs) plain += run.text;
            const auto string = k->cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(plain.data()), plain.size()));
            static_cast<void>(k->send_id(view, "replaceCharactersInRange:withString:", {r.first, r.second, string}));
            k->release(string);
        }
        static_cast<void>(k->send_id(view, "didChangeText"));
    }, "v@:{_NSRange=QQ}@");
    for (const auto* name : {"RTFDFromRange:", "replaceCharactersInRange:withRTFD:"}) {
        const std::string selector = name;
        kit.method(t, selector, [selector](CpuState&, GuestMemory&) {
            unsupported("-[NSText " + selector + "] (flattened RTFD data is not read or written)");
        });
    }
    // An RTFD directory with TXT.rtf (attachments are not written).
    kit.method(t, "writeRTFDToFile:atomically:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0], path = s.x[2];
        if (!path) { return_bool(s, false); return; }
        if (contains_attachment(*k, view)) unsupported("-[NSText writeRTFDToFile:atomically:] with attachments (their files are not written)");
        const auto manager = k->send_id(k->objc.find_class("NSFileManager"), "defaultManager");
        if (!k->send_bool(manager, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", {path, 1, 0, 0})) { return_bool(s, false); return; }
        const auto text = text_of(*k, view);
        const auto data = data_of(*k, rtf::write(runs_from(*k, view, {0, text.size()})));
        const auto file = k->send_id(path, "stringByAppendingPathComponent:", {k->auto_string("TXT.rtf")});
        return_bool(s, k->send_bool(data, "writeToFile:atomically:", {file, s.x[3] & 0xff}));
    }, "B@:@B");

    // ---- NSText: spelling, rulers, kept settings ----
    const auto checker = [k]() { return k->send_id(k->objc.find_class("NSSpellChecker"), "sharedSpellChecker"); };
    kit.method(t, "checkSpelling:", [k, checker](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto [location, length] = selected(*k, view);
        const auto string = k->send_id(view, "string");
        const auto found = k->send(checker(), "checkSpellingOfString:startingAt:", [&] { CallRegs r; r.x[2] = string; r.x[3] = location + length; return r; }());
        if (found.x1 == 0 || found.x0 == not_found) return;
        static_cast<void>(k->send_id(view, "setSelectedRange:", {found.x0, found.x1}));
        static_cast<void>(k->send_id(view, "scrollRangeToVisible:", {found.x0, found.x1}));
        const auto word = k->send_id(string, "substringWithRange:", {found.x0, found.x1});
        static_cast<void>(k->send_id(checker(), "updateSpellingPanelWithMisspelledWord:", {word}));
    }, "v@:@");
    kit.method(t, "showGuessPanel:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSText showGuessPanel:] (no Spelling and Grammar panel is provided)");
    }, "v@:@");
    kit.method(t, "importsGraphics", [k](CpuState& s, GuestMemory&) { return_bool(s, text_state(*k, s.x[0])->imports_graphics); }, "B@:");
    kit.method(t, "setImportsGraphics:", [k](CpuState& s, GuestMemory&) { text_state(*k, s.x[0])->imports_graphics = bool_arg(s, 2); }, "v@:B");
    kit.method(t, "usesFontPanel", [k](CpuState& s, GuestMemory&) { return_bool(s, text_state(*k, s.x[0])->uses_font_panel); }, "B@:");
    kit.method(t, "setUsesFontPanel:", [k](CpuState& s, GuestMemory&) { text_state(*k, s.x[0])->uses_font_panel = bool_arg(s, 2); }, "v@:B");
    // The ruler is the enclosing scroll view's.
    const auto ruler_visible = [k](Address view) {
        const auto scroll = k->send_id(view, "enclosingScrollView");
        return scroll && k->responds(scroll, "rulersVisible") && k->send_bool(scroll, "rulersVisible");
    };
    kit.method(t, "isRulerVisible", [ruler_visible](CpuState& s, GuestMemory&) { return_bool(s, ruler_visible(s.x[0])); }, "B@:");
    kit.method(t, "toggleRuler:", [k, ruler_visible](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const bool visible = k->responds(view, "isRulerVisible") ? k->send_bool(view, "isRulerVisible") : ruler_visible(view);
        if (k->responds(view, "setRulerVisible:")) { static_cast<void>(k->send_id(view, "setRulerVisible:", {visible ? 0u : 1u})); return; }
        if (const auto scroll = k->send_id(view, "enclosingScrollView")) static_cast<void>(k->send_id(scroll, "setRulersVisible:", {visible ? 0u : 1u}));
    }, "v@:@");

    // ---- NSTextView: pasteboard types and dragging ----
    const auto readable = [k](Address view) {
        std::vector<Address> types;
        if (k->send_bool(view, "isRichText")) types.push_back(k->auto_string(rtf_type));
        types.push_back(k->auto_string(string_type));
        return types;
    };
    kit.method(tv, "readablePasteboardTypes", [k, readable](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(readable(s.x[0])); });
    kit.method(tv, "writablePasteboardTypes", [k, readable](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(readable(s.x[0])); });
    kit.method(tv, "acceptableDragTypes", [k, readable](CpuState& s, GuestMemory&) {
        s.x[0] = k->auto_array(k->send_bool(s.x[0], "isEditable") ? readable(s.x[0]) : std::vector<Address>{});
    });
    kit.method(tv, "updateDragTypeRegistration", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto types = k->send_id(view, "acceptableDragTypes");
        if (!k->items(types).empty()) static_cast<void>(k->send_id(view, "registerForDraggedTypes:", {types}));
        else static_cast<void>(k->send_id(view, "unregisterDraggedTypes"));
    }, "v@:");
    kit.method(tv, "preferredPasteboardTypeFromArray:restrictedToTypesInArray:", [k, readable](CpuState& s, GuestMemory&) {
        const auto names = [k](Address array) {
            std::vector<std::string> list;
            for (const auto item : k->items(array)) list.push_back(k->text(item));
            return list;
        };
        const auto available = names(s.x[2]);
        const auto allowed = s.x[3] ? names(s.x[3]) : std::vector<std::string>{};
        for (const auto type : readable(s.x[0])) {
            const auto name = k->text(type);
            if (std::find(available.begin(), available.end(), name) == available.end()) continue;
            if (s.x[3] && std::find(allowed.begin(), allowed.end(), name) == allowed.end()) continue;
            s.x[0] = type;
            return;
        }
        s.x[0] = 0;
    });
    kit.method(tv, "cleanUpAfterDragOperation", [](CpuState&, GuestMemory&) {}, "v@:");
    kit.method(tv, "dragSelectionWithEvent:offset:slideBack:", [](CpuState&, GuestMemory&) {
        unsupported("-[NSTextView dragSelectionWithEvent:offset:slideBack:] (dragging a text selection)");
    }, "B@:@{CGSize=dd}B");
    // Pasting with or without the formatting.
    kit.method(tv, "pasteAsPlainText:", [k](CpuState& s, GuestMemory&) {
        const auto string = k->send_id(k->general_pasteboard(), "stringForType:", {k->auto_string(string_type)});
        if (string) static_cast<void>(k->send_id(s.x[0], "insertText:", {string}));
    }, "v@:@");
    kit.method(tv, "pasteAsRichText:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto data = k->send_id(k->general_pasteboard(), "dataForType:", {k->auto_string(rtf_type)});
        if (!data) {
            const auto string = k->send_id(k->general_pasteboard(), "stringForType:", {k->auto_string(string_type)});
            if (string) static_cast<void>(k->send_id(view, "insertText:", {string}));
            return;
        }
        const auto [location, length] = selected(*k, view);
        static_cast<void>(k->send_id(view, "replaceCharactersInRange:withRTF:", {location, length, data}));
    }, "v@:@");

    // ---- NSTextView: editing hooks ----
    kit.method(tv, "didChangeText", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        static_cast<void>(k->post("NSTextDidChangeNotification", view));
        if (const auto delegate = text_state(*k, view)->delegate)
            static_cast<void>(k->send_optional(delegate, "textDidChange:", [&] { CallRegs r; r.x[2] = k->notification("NSTextDidChangeNotification", view); return r; }()));
    }, "v@:");
    kit.method(tv, "shouldChangeTextInRange:replacementString:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        if (!k->send_bool(view, "isEditable")) { return_bool(s, false); return; }
        const auto delegate = text_state(*k, view)->delegate;
        if (delegate && k->responds(delegate, "textView:shouldChangeTextInRange:replacementString:")) {
            return_bool(s, k->send_bool(delegate, "textView:shouldChangeTextInRange:replacementString:", {view, s.x[2], s.x[3], s.x[4]}));
            return;
        }
        return_bool(s, true);
    }, "B@:{_NSRange=QQ}@");
    kit.method(tv, "shouldChangeTextInRanges:replacementStrings:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        if (!k->send_bool(view, "isEditable")) { return_bool(s, false); return; }
        const auto delegate = text_state(*k, view)->delegate;
        if (delegate && k->responds(delegate, "textView:shouldChangeTextInRanges:replacementStrings:")) {
            return_bool(s, k->send_bool(delegate, "textView:shouldChangeTextInRanges:replacementStrings:", {view, s.x[2], s.x[3]}));
            return;
        }
        const auto ranges = k->items(s.x[2]);
        const auto strings = s.x[3] ? k->items(s.x[3]) : std::vector<Address>{};
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            const auto r = k->send(ranges[i], "rangeValue");
            if (!k->send_bool(view, "shouldChangeTextInRange:replacementString:", {r.x0, r.x1, i < strings.size() ? strings[i] : 0})) { return_bool(s, false); return; }
        }
        return_bool(s, true);
    }, "B@:@@");
    kit.method(tv, "rangeForUserTextChange", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        if (!k->send_bool(view, "isEditable")) { s.x[0] = not_found; s.x[1] = 0; return; }
        const auto [location, length] = selected(*k, view);
        s.x[0] = location;
        s.x[1] = length;
    }, "{_NSRange=QQ}@:");
    kit.method(tv, "rangesForUserTextChange", [k](CpuState& s, GuestMemory&) {
        s.x[0] = k->send_bool(s.x[0], "isEditable") ? k->send_id(s.x[0], "selectedRanges") : 0;
    });
    // Typing is never coalesced into undo groups: undo is not provided.
    kit.method(tv, "breakUndoCoalescing", [](CpuState&, GuestMemory&) {}, "v@:");
    kit.method(tv, "isCoalescingUndo", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    kit.method(tv, "clickedOnLink:atIndex:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0], link = s.x[2];
        const auto delegate = text_state(*k, view)->delegate;
        if (delegate && k->responds(delegate, "textView:clickedOnLink:atIndex:") &&
            k->send_bool(delegate, "textView:clickedOnLink:atIndex:", {view, link, s.x[3]}))
            return;
        if (!link) return;
        const auto url = k->responds(link, "absoluteString") ? link : k->send_id(k->objc.find_class("NSURL"), "URLWithString:", {link});
        if (url) static_cast<void>(k->send_id(k->send_id(k->objc.find_class("NSWorkspace"), "sharedWorkspace"), "openURL:", {url}));
    }, "v@:@Q");

    // ---- NSTextView: drawing ----
    kit.method(tv, "drawInsertionPointInRect:color:turnedOn:", [k](CpuState& s, GuestMemory&) {
        const auto rect = rect_arg(s);
        if (s.x[3] & 0xff) { fill(*k, s.x[2] ? s.x[2] : k->send(k->objc.find_class("NSColor"), "textColor").x0, rect); return; }
        CallRegs registers;
        put_rect(registers, 0, rect);
        static_cast<void>(k->send(s.x[0], "setNeedsDisplayInRect:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@B");
    kit.method(tv, "drawViewBackgroundInRect:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        if (!k->send_bool(view, "drawsBackground")) return;
        if (const auto color = k->send_id(view, "backgroundColor")) fill(*k, color, rect_arg(s));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(tv, "setNeedsDisplayInRect:avoidAdditionalLayout:", [k](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        static_cast<void>(k->send(s.x[0], "setNeedsDisplayInRect:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}B");
    kit.method(tv, "updateInsertionPointStateAndRestartTimer:", [k](CpuState& s, GuestMemory&) {
        static_cast<void>(k->send_id(s.x[0], "setNeedsDisplay:", {1}));
    }, "v@:B");
    // The container origin follows the inset, so there is nothing to invalidate.
    kit.method(tv, "invalidateTextContainerOrigin", [](CpuState&, GuestMemory&) {}, "v@:");
    kit.method(tv, "characterIndexForInsertionAtPoint:", [k](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_point(registers, 0, {d(s, 0), d(s, 1)});
        s.x[0] = k->send(s.x[0], "_alchemyIndexAtPoint:", registers).x0;
    }, "Q@:{CGPoint=dd}");
    kit.method(tv, "changeDocumentBackgroundColor:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        if (!text_state(*k, view)->allows_background_change || !s.x[2]) return;
        if (const auto color = k->send_id(s.x[2], "color")) static_cast<void>(k->send_id(view, "setBackgroundColor:", {color}));
    }, "v@:@");
    kit.method(tv, "toggleBaseWritingDirection:", [k](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto direction = static_cast<std::int64_t>(k->send_id(view, "baseWritingDirection"));
        static_cast<void>(k->send_id(view, "setBaseWritingDirection:", {direction == 1 ? 0u : 1u}));   // right to left <-> left to right
    }, "v@:@");

    // ---- NSTextView: find and spelling ----
    for (const auto* name : {"performFindPanelAction:", "performTextFinderAction:"})
        kit.method(tv, name, [k](CpuState& s, GuestMemory&) {
            perform_find_action(*k, s.x[0], s.x[2] ? static_cast<std::int64_t>(k->send_id(s.x[2], "tag")) : 1);
        }, "v@:@");
    // No find indicator is drawn: the range is scrolled into view.
    kit.method(tv, "showFindIndicatorForRange:", [k](CpuState& s, GuestMemory&) {
        static_cast<void>(k->send_id(s.x[0], "scrollRangeToVisible:", {s.x[2], s.x[3]}));
    }, "v@:{_NSRange=QQ}");
    kit.method(tv, "spellCheckerDocumentTag", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        if (state->spell_tag < 0) state->spell_tag = static_cast<std::int64_t>(k->send_id(k->objc.find_class("NSSpellChecker"), "uniqueSpellDocumentTag"));
        s.x[0] = static_cast<Address>(state->spell_tag);
    }, "q@:");
    kit.method(tv, "setSpellingState:range:", [k](CpuState& s, GuestMemory&) {
        const auto manager = k->send_id(s.x[0], "layoutManager");
        if (!manager) return;
        if (s.x[2]) static_cast<void>(k->send_id(manager, "addTemporaryAttribute:value:forCharacterRange:",
                                                 {k->auto_string("NSSpellingState"), k->autorelease(k->cf.integer(static_cast<std::int64_t>(s.x[2]))), s.x[3], s.x[4]}));
        else static_cast<void>(k->send_id(manager, "removeTemporaryAttribute:forCharacterRange:", {k->auto_string("NSSpellingState"), s.x[3], s.x[4]}));
    }, "v@:q{_NSRange=QQ}");
    // Misspelled words in the range are marked (NSSpellingStateSpellingFlag), the rest unmarked.
    const auto check_text = [k, checker](Address view, Range r) {
        if (r.second == 0) return;
        static_cast<void>(k->send_id(view, "setSpellingState:range:", {0, r.first, r.second}));
        const auto string = k->send_id(view, "string");
        for (auto start = r.first; start < r.first + r.second;) {
            const auto found = k->send(checker(), "checkSpellingOfString:startingAt:", [&] { CallRegs q; q.x[2] = string; q.x[3] = start; return q; }());
            if (found.x1 == 0 || found.x0 == not_found || found.x0 < start || found.x0 + found.x1 > r.first + r.second) break;
            static_cast<void>(k->send_id(view, "setSpellingState:range:", {1, found.x0, found.x1}));
            start = found.x0 + found.x1;
        }
    };
    kit.method(tv, "checkTextInDocument:", [k, check_text](CpuState& s, GuestMemory&) {
        check_text(s.x[0], {0, text_of(*k, s.x[0]).size()});
    }, "v@:@");
    kit.method(tv, "checkTextInSelection:", [k, check_text](CpuState& s, GuestMemory&) { check_text(s.x[0], selected(*k, s.x[0])); }, "v@:@");

    // ---- NSTextView: kept settings and TextKit 1 answers ----
    const auto flag = [&kit, k, tv](const char* getter, const char* setter, bool TextState::*member) {
        kit.method(tv, getter, [k, member](CpuState& s, GuestMemory&) { return_bool(s, (*text_state(*k, s.x[0])).*member); }, "B@:");
        kit.method(tv, setter, [k, member](CpuState& s, GuestMemory&) { (*text_state(*k, s.x[0])).*member = bool_arg(s, 2); }, "v@:B");
    };
    flag("acceptsGlyphInfo", "setAcceptsGlyphInfo:", &TextState::accepts_glyph_info);
    flag("allowsDocumentBackgroundColorChange", "setAllowsDocumentBackgroundColorChange:", &TextState::allows_background_change);
    flag("usesAdaptiveColorMappingForDarkAppearance", "setUsesAdaptiveColorMappingForDarkAppearance:", &TextState::adaptive_colors);
    kit.method(tv, "allowedInputSourceLocales", [k](CpuState& s, GuestMemory&) { s.x[0] = text_state(*k, s.x[0])->allowed_locales; });
    kit.method(tv, "setAllowedInputSourceLocales:", [k](CpuState& s, GuestMemory&) {
        const auto state = text_state(*k, s.x[0]);
        k->release(std::exchange(state->allowed_locales, s.x[2] ? k->send(s.x[2], "copy").x0 : 0));
    }, "v@:@");
    kit.method(tv, "writingToolsBehavior", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(text_state(*k, s.x[0])->writing_tools_behavior); }, "q@:");
    kit.method(tv, "setWritingToolsBehavior:", [k](CpuState& s, GuestMemory&) { text_state(*k, s.x[0])->writing_tools_behavior = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    kit.method(tv, "isWritingToolsActive", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    kit.method(tv, "writingToolsActive", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    kit.method(tv, "usesRolloverButtonForSelection", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    kit.method(tv, "smartInsertDeleteEnabled", [k](CpuState& s, GuestMemory&) { return_bool(s, k->send_bool(s.x[0], "isSmartInsertDeleteEnabled")); }, "B@:");
    kit.method(tv, "selectionAffinity", [k](CpuState& s, GuestMemory&) { s.x[0] = text_state(*k, s.x[0])->selection_affinity; }, "Q@:");
    kit.method(tv, "setSelectionGranularity:", [k](CpuState& s, GuestMemory&) { text_state(*k, s.x[0])->selection_granularity = s.x[2]; }, "v@:Q");
    kit.method(tv, "setSelectedRange:affinity:stillSelecting:", [k](CpuState& s, GuestMemory&) {
        text_state(*k, s.x[0])->selection_affinity = s.x[4];
        static_cast<void>(k->send_id(s.x[0], "setSelectedRange:", {s.x[2], s.x[3]}));
    }, "v@:{_NSRange=QQ}QB");
    kit.method(tv, "layoutOrientation", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");   // horizontal
    kit.method(tv, "setLayoutOrientation:", [](CpuState& s, GuestMemory&) {
        if (s.x[2] != 0) unsupported("-[NSTextView setLayoutOrientation:] vertical (vertical text layout)");
    }, "v@:q");
    for (const auto* name : {"textContentStorage", "textLayoutManager"})
        kit.method(tv, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; });   // TextKit 1 views
    for (const auto* name : {"orderFrontLinkPanel:", "orderFrontListPanel:", "orderFrontTablePanel:"}) {
        const std::string selector = name;
        kit.method(tv, selector, [selector](CpuState&, GuestMemory&) { unsupported("-[NSTextView " + selector + "] (no such panel is provided)"); }, "v@:@");
    }

    // ---- NSTextView class methods ----
    kit.class_method(tv, "fieldEditor", [k](CpuState& s, GuestMemory&) {
        CallRegs frame;
        put_rect(frame, 0, {});
        const auto view = k->send(k->send(s.x[0], "alloc").x0, "initWithFrame:", frame).x0;
        static_cast<void>(k->send_id(view, "setFieldEditor:", {1}));
        s.x[0] = k->autorelease(view);
    });
    // A scroll view showing a text view that tracks its width and grows down.
    kit.class_method(tv, "scrollableTextView", [k](CpuState& s, GuestMemory&) {
        CallRegs frame;
        put_rect(frame, 0, {{0, 0}, {240, 120}});
        const auto scroll = k->send(k->send(k->objc.find_class("NSScrollView"), "alloc").x0, "initWithFrame:", frame).x0;
        static_cast<void>(k->send_id(scroll, "setHasVerticalScroller:", {1}));
        const auto size = k->send(scroll, "contentSize");
        CallRegs text_frame;
        put_rect(text_frame, 0, {{0, 0}, {size.d(0), size.d(1)}});
        const auto view = k->send(k->send(s.x[0], "alloc").x0, "initWithFrame:", text_frame).x0;
        CallRegs max;
        put_size(max, 0, {1e7, 1e7});
        static_cast<void>(k->send(view, "setMaxSize:", max));
        static_cast<void>(k->send_id(view, "setVerticallyResizable:", {1}));
        static_cast<void>(k->send_id(view, "setHorizontallyResizable:", {0}));
        static_cast<void>(k->send_id(view, "setAutoresizingMask:", {2}));   // NSViewWidthSizable
        static_cast<void>(k->send_id(k->send_id(view, "textContainer"), "setWidthTracksTextView:", {1}));
        static_cast<void>(k->send_id(scroll, "setDocumentView:", {view}));
        k->release(view);
        s.x[0] = k->autorelease(scroll);
    });
    kit.class_method(tv, "textViewUsingTextLayoutManager:", [k](CpuState& s, GuestMemory&) {
        if (s.x[2] & 0xff) unsupported("+[NSTextView textViewUsingTextLayoutManager:YES] (TextKit 2 is not provided)");
        CallRegs frame;
        put_rect(frame, 0, {});
        s.x[0] = k->autorelease(k->send(k->send(s.x[0], "alloc").x0, "initWithFrame:", frame).x0);
    }, "@@:B");
}

} // namespace alchemy::gui::completion_kit::controls
