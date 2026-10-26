#include "alchemy/darwin/appkit/controls_internal.hpp"
#include "alchemy/darwin/appkit/rtf.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

// NSText / NSTextView: plain-text editing and display (an installer's
// license text, multi-line fields) and the window's field editor, which
// edits NSTextFields: the field editor becomes the window's first responder
// and an NSTextInputClient (IME composition and committed text arrive through
// -setMarkedText:... / -insertText:replacementRange:), key bindings arrive as
// -doCommandBySelector: (asking the text field's delegate first,
// control:textView:doCommandBySelector:), and the edited text field hears
// textDidBeginEditing: / textDidChange: / textDidEndEditing: (and posts the
// NSControlTextDid... notifications to its delegate).
//
// Text is one attribute run (the view's font and color); ranges are UTF-16
// units as in Cocoa. A view with an NSTextStorage (the TextKit objects of
// src/alchemy/darwin/kit/completion/controls/text_system.cpp) edits the storage too and
// shows its text in the attributes of its first character. Undo is not
// provided.
namespace alchemy::gui::appkit {
namespace {
using drawing::Color;

constexpr char16_t bullet = char16_t{0x2022};

constexpr std::size_t not_found_index = static_cast<std::size_t>(INT64_MAX);

TextStyle style_of(AppKit& kit, Address value, const TextState& state) {
    const auto& th = kit.theme_for(value);
    TextStyle style;
    style.font = kit.font_spec(state.font);
    if (!state.font) style.font.size = state.field_editor ? 13 : 12;
    style.color = kit.color_value(state.text_color, th.text);
    style.alignment = state.alignment;
    return style;
}
std::u16string shown_text(const TextState& state) {
    if (state.secure) return std::u16string(state.value.size(), bullet);
    // Line and paragraph separators (-insertLineBreak:) break lines like newlines.
    auto shown = state.value;
    std::replace_if(shown.begin(), shown.end(), [](char16_t c) { return c == 0x2028 || c == 0x2029; }, char16_t{0x0a});
    return shown;
}
// The horizontal padding of each line: the container's line fragment
// padding (AppKit's 5 points; a field editor's 2), inside the container inset.
double line_padding(AppKit& kit, const TextState& state) {
    if (state.field_editor) return 2;
    if (state.container)
        if (const auto container = kit.objc.find_host_state<ContainerState>(state.container)) return container->padding;
    return 5;
}
// Lines of the text in the view (UTF-16 offsets).
std::vector<drawing::TextLine> lines_of(AppKit& kit, Address value, const TextState& state) {
    const auto style = style_of(kit, value, state);
    const auto bounds = AppKit::view_bounds(*kit.view_state(value));
    bool unwrapped = state.field_editor || state.horizontally_resizable;
    if (state.container)
        if (const auto container = kit.objc.find_host_state<ContainerState>(state.container); container && !container->tracks_width)
            unwrapped = unwrapped || container->size.width >= bounds.size.width * 4;
    const double width = unwrapped ? 0 : std::max(1.0, bounds.size.width - 2 * state.inset.width - 2 * line_padding(kit, state));
    return drawing::layout_lines(style.font, shown_text(state), width);
}
// The x offset of a caret position within its line.
double caret_x(const drawing::FontSpec& font, std::u16string_view line, std::size_t offset) {
    if (offset == 0 || line.empty()) return 0;
    return drawing::prefix_widths(font, line)[std::min(offset, line.size()) - 1];
}
} // namespace

Address AppKit::editing_field_editor(Address field) {
    const auto state = find_extension<ControlState>(field);
    return state ? state->editor : 0;
}

Address AppKit::field_editor_for(Address target) {
    const auto state = window_state(target);
    if (!state->field_editor) {
        CallRegs registers;
        put_rect(registers, 0, {{0, 0}, {100, 20}});
        state->field_editor = send(send(text_view, "alloc").x0, "initWithFrame:", registers).x0;
        static_cast<void>(send_id(state->field_editor, "setFieldEditor:", {1}));
    }
    return state->field_editor;
}

void AppKit::begin_field_editing(Address field) {
    const auto owner = view_state(field)->window;
    if (!owner) return;
    const auto control_state = extension<ControlState>(field);
    if (control_state->editor) return;
    const auto editor = field_editor_for(owner);
    const auto editing = text_state(*this, editor);
    if (editing->client && editing->client != field) end_field_editing(editing->client, false);
    const auto target_cell = control_cell(*this, field);
    if (!target_cell) return;
    const auto cell_info = cell_state(*this, target_cell);
    editing->value = cell_text(*this, target_cell);
    editing->selection = 0;
    editing->selection_length = editing->value.size();
    editing->anchor = 0;
    editing->marked.reset();
    editing->client = field;
    editing->delegate = field;
    editing->editable = cell_info->editable;
    editing->selectable = cell_info->selectable || cell_info->editable;
    editing->secure = cell_info->secure;
    editing->alignment = cell_info->alignment;
    editing->scroll_x = 0;
    editing->draws_background = false;
    assign(editing->font, cell_info->font);
    assign(editing->text_color, cell_info->text_color);
    const auto bounds = view_bounds(*view_state(field));
    const auto area = cell_info->bezeled || cell_info->bordered
                          ? Rect{{bounds.origin.x + 4, bounds.origin.y + 2}, {std::max(0.0, bounds.size.width - 8), std::max(0.0, bounds.size.height - 4)}}
                          : Rect{{bounds.origin.x + 2, bounds.origin.y}, {std::max(0.0, bounds.size.width - 4), bounds.size.height}};
    CallRegs frame;
    put_rect(frame, 0, {{area.origin.x + cell_info->leading_inset, area.origin.y},
                        {std::max(0.0, area.size.width - cell_info->leading_inset - cell_info->trailing_inset), area.size.height}});
    static_cast<void>(send(editor, "setFrame:", frame));
    control_state->editor = editor;
    static_cast<void>(send_id(field, "addSubview:", {editor}));
    invalidate_view(field);
    if (editing->editable) {
        const auto note_value = notification("NSTextDidBeginEditingNotification", editor);
        static_cast<void>(send_id(field, "textDidBeginEditing:", {note_value}));
    }
}

void AppKit::end_field_editing(Address field, bool send_action_too) {
    const auto control_state = extension<ControlState>(field);
    const auto editor = control_state->editor;
    if (!editor) return;
    const auto editing = text_state(*this, editor);
    if (const auto target_cell = control_cell(*this, field))
        release(std::exchange(cell_state(*this, target_cell)->contents, string(drawing::utf8(editing->value))));
    control_state->editor = 0;
    editing->client = 0;
    editing->delegate = 0;
    editing->marked.reset();
    const auto owner = view_state(field)->window;
    if (owner && window_state(owner)->first_responder == editor) window_state(owner)->first_responder = field;
    static_cast<void>(send(editor, "removeFromSuperview"));
    invalidate_view(field);
    if (editing->editable) {
        const auto note_value = notification("NSTextDidEndEditingNotification", editor);
        static_cast<void>(send_id(field, "textDidEndEditing:", {note_value}));
    }
    // Return sends the action; so does any other end of editing (Tab, a
    // click elsewhere) when the cell sends its action on end editing, but
    // not a cancellation (Escape).
    const bool aborted = std::exchange(editing->aborting, false);
    if (const auto target_cell = control_cell(*this, field)) {
        const auto info = cell_state(*this, target_cell);
        if (send_action_too || (!aborted && info->sends_action_on_end_editing))
            static_cast<void>(send_id(field, "sendAction:to:", {info->action, info->target}));
    }
}

void AppKit::install_text() {
    text_class = declare_class("NSText", "NSView");
    text_view = declare_class("NSTextView", "NSText");
    const auto ts = [this](Address value) { return text_state(*this, value); };
    string_constant("_NSTextDidChangeNotification", "NSTextDidChangeNotification");
    string_constant("_NSTextDidBeginEditingNotification", "NSTextDidBeginEditingNotification");
    string_constant("_NSTextDidEndEditingNotification", "NSTextDidEndEditingNotification");

    const auto changed = [this, ts](Address self) {
        invalidate_view(self);
        const auto state = ts(self);
        const auto note_value = notification("NSTextDidChangeNotification", self);
        if (state->delegate) {
            CallRegs registers;
            registers.x[2] = note_value;
            static_cast<void>(send_optional(state->delegate, "textDidChange:", registers));
        }
        // Vertically resizable text views grow with their text.
        if (!state->field_editor && state->vertically_resizable) static_cast<void>(send(self, "sizeToFit"));
    };
    // The view's NSTextStorage (when it has one) follows its edits: the
    // replaced characters, then the typing attributes on the new ones.
    const auto edit_storage = [this, ts](Address self, std::size_t location, std::size_t length, std::u16string_view with) {
        const auto state = ts(self);
        if (!state->storage || state->syncing) return;
        state->syncing = true;
        try {
            const auto replacement = cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(with.data()), with.size()));
            static_cast<void>(send_id(state->storage, "replaceCharactersInRange:withString:", {location, length, replacement}));
            release(replacement);
            if (state->typing_attributes && !with.empty())
                static_cast<void>(send_id(state->storage, "setAttributes:range:", {state->typing_attributes, location, with.size()}));
        } catch (...) {
            state->syncing = false;
            throw;
        }
        state->syncing = false;
    };
    // Replaces [location, location + length) and puts the caret after the new text.
    const auto replace = [ts, changed, edit_storage](Address self, std::size_t location, std::size_t length, std::u16string_view with) {
        const auto state = ts(self);
        location = std::min(location, state->value.size());
        length = std::min(length, state->value.size() - location);
        edit_storage(self, location, length, with);
        state->value.replace(location, length, with);
        state->selection = location + with.size();
        state->selection_length = 0;
        state->anchor = state->selection;
        changed(self);
    };

    const auto t = text_class;
    method(t, "initWithFrame:", [this, ts](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = objc.send_super(s.x[0], text_class, sel("initWithFrame:"), registers).x0;
        static_cast<void>(ts(s.x[0]));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(t, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = find_extension<TextState>(s.x[0]))
        {
            for (auto* slot : {&state->font, &state->text_color, &state->background_color, &state->typing_attributes,
                     &state->selected_attributes, &state->marked_attributes, &state->link_attributes, &state->default_paragraph,
                     &state->insertion_color, &state->layout_manager, &state->storage})
                release(std::exchange(*slot, 0));
            if (const auto container = std::exchange(state->container, 0)) {
                if (const auto info = objc.find_host_state<ContainerState>(container)) info->text_view = 0;
                release(container);
            }
        }
        static_cast<void>(objc.send_super(s.x[0], text_class, sel("dealloc")));
    }, "v@:");
    method(t, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(t, "string", [this, ts](CpuState& s, GuestMemory&) { s.x[0] = auto_string(drawing::utf8(ts(s.x[0])->value)); });
    // An RTF file, or an RTFD package's TXT.rtf, as the text (rtf.hpp: the
    // document's text; its formatting is not kept). NO when unreadable.
    method(t, "readRTFDFromFile:", [this](CpuState& s, GuestMemory&) {
        auto path = text(s.x[2]);
        if (path.ends_with(".rtfd") || path.ends_with(".rtfd/")) path = (path.ends_with("/") ? path : path + "/") + "TXT.rtf";
        const auto bytes = path.empty() ? std::nullopt : read_file(path);
        if (!bytes || !rtf::looks_like_rtf(*bytes)) { s.x[0] = 0; return; }
        std::u16string value;
        try { value = rtf::plain_text(*bytes); }
        catch (const std::exception& error) { fail(path + ": " + error.what()); }
        const auto string_value = cf.string_utf16(std::span(reinterpret_cast<const std::uint16_t*>(value.data()), value.size()));
        static_cast<void>(send_id(s.x[0], "setString:", {string_value}));
        release(string_value);
        s.x[0] = 1;
    }, "B@:@");
    method(t, "setString:", [this, ts, edit_storage](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        const auto value = string16(s.x[2]);
        edit_storage(s.x[0], 0, state->value.size(), value);
        state->value = value;
        state->selection = state->value.size();
        state->selection_length = 0;
        state->anchor = state->selection;
        state->marked.reset();
        invalidate_view(s.x[0]);
        if (!state->field_editor && state->vertically_resizable) static_cast<void>(send(s.x[0], "sizeToFit"));
    }, "v@:@");
    method(t, "length", [ts](CpuState& s, GuestMemory&) { s.x[0] = ts(s.x[0])->value.size(); }, "Q@:");
    method(t, "selectedRange", [ts](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        s.x[0] = state->selection;
        s.x[1] = state->selection_length;
    }, "{_NSRange=QQ}@:");
    method(t, "setSelectedRange:", [this, ts](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        const auto location = std::min<std::size_t>(s.x[2], state->value.size());
        state->selection = location;
        state->selection_length = std::min<std::size_t>(s.x[3], state->value.size() - location);
        state->anchor = location;
        invalidate_view(s.x[0]);
    }, "v@:{_NSRange=QQ}");
    method(t, "selectAll:", [this, ts](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        state->selection = 0;
        state->selection_length = state->value.size();
        state->anchor = 0;
        invalidate_view(s.x[0]);
    }, "v@:@");
    method(t, "replaceCharactersInRange:withString:", [this, replace](CpuState& s, GuestMemory&) {
        replace(s.x[0], s.x[2], s.x[3], string16(s.x[4]));
    }, "v@:{_NSRange=QQ}@");
    for (const auto* name : {"insertText:", "insertText:replacementRange:"}) {
        const bool ranged = std::string_view(name).ends_with("Range:");
        method(t, name, [this, ts, replace, ranged](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto state = ts(self);
            if (!state->editable) { if (const auto server = window_server_or_null()) server->beep(); return; }
            std::size_t location = state->selection, length = state->selection_length;
            if (state->marked) { location = state->marked->first; length = state->marked->second; }
            if (ranged && s.x[3] != not_found) { location = s.x[3]; length = s.x[4]; }
            state->marked.reset();
            auto inserted = string16(s.x[2]);
            if (state->field_editor)
                inserted.erase(std::remove_if(inserted.begin(), inserted.end(), [](char16_t c) { return c == u'\n' || c == u'\r'; }), inserted.end());
            replace(self, location, length, inserted);
        }, ranged ? "v@:@{_NSRange=QQ}" : "v@:@");
    }
    // NSTextInputClient.
    method(t, "hasMarkedText", [ts](CpuState& s, GuestMemory&) { return_bool(s, ts(s.x[0])->marked.has_value()); }, "B@:");
    method(t, "markedRange", [ts](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        s.x[0] = state->marked ? state->marked->first : not_found_index;
        s.x[1] = state->marked ? state->marked->second : 0;
    }, "{_NSRange=QQ}@:");
    method(t, "setMarkedText:selectedRange:replacementRange:", [this, ts](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ts(self);
        if (!state->editable) return;
        const auto value = string16(s.x[2]);
        std::size_t location = state->selection, length = state->selection_length;
        if (state->marked) { location = state->marked->first; length = state->marked->second; }
        if (s.x[5] != not_found) { location = s.x[5]; length = s.x[6]; }
        location = std::min(location, state->value.size());
        length = std::min(length, state->value.size() - location);
        state->value.replace(location, length, value);
        if (value.empty()) state->marked.reset();
        else state->marked = std::make_pair(location, value.size());
        state->selection = location + std::min<std::size_t>(s.x[3], value.size());
        state->selection_length = std::min<std::size_t>(s.x[4], value.size());
        invalidate_view(self);
    }, "v@:@{_NSRange=QQ}{_NSRange=QQ}");
    method(t, "unmarkText", [this, ts](CpuState& s, GuestMemory&) { ts(s.x[0])->marked.reset(); invalidate_view(s.x[0]); }, "v@:");
    method(t, "validAttributesForMarkedText", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array({}); });
    method(t, "attributedSubstringForProposedRange:actualRange:", [this, ts](CpuState& s, GuestMemory& m) {
        const auto state = ts(s.x[0]);
        const auto location = std::min<std::size_t>(s.x[2], state->value.size());
        const auto length = std::min<std::size_t>(s.x[3], state->value.size() - location);
        if (s.x[4]) { m.write64(s.x[4], location); m.write64(s.x[4] + 8, length); }
        const auto attributed = objc.find_class("NSAttributedString");
        const auto value = auto_string(drawing::utf8(std::u16string_view(state->value).substr(location, length)));
        s.x[0] = autorelease(send_id(send(attributed, "alloc").x0, "initWithString:", {value}));
    }, "@@:{_NSRange=QQ}^{_NSRange=QQ}");
    method(t, "characterIndexForPoint:", [this](CpuState& s, GuestMemory&) {
        // Screen point -> character index.
        const auto self = s.x[0];
        const auto owner = view_state(self)->window;
        if (!owner) { s.x[0] = not_found_index; return; }
        const auto origin = content_rect(owner).origin;
        const auto screen = point_arg(s);
        const auto local = window_to_view(self, {screen.x - origin.x, screen.y - origin.y});
        CallRegs registers;
        put_point(registers, 0, local);
        s.x[0] = send(self, "_alchemyIndexAtPoint:", registers).x0;
    }, "Q@:{CGPoint=dd}");
    method(t, "firstRectForCharacterRange:actualRange:", [this, ts](CpuState& s, GuestMemory& m) {
        const auto self = s.x[0];
        const auto state = ts(self);
        const auto location = std::min<std::size_t>(s.x[2], state->value.size());
        if (s.x[4]) { m.write64(s.x[4], location); m.write64(s.x[4] + 8, std::min<std::size_t>(s.x[3], state->value.size() - location)); }
        CallRegs registers;
        registers.x[2] = location;
        const auto caret = result_rect(send(self, "_alchemyCaretRect:", registers));
        const auto owner = view_state(self)->window;
        const auto in_window = view_rect_in_window(self, caret);
        const auto origin = owner ? content_rect(owner).origin : Point{};
        return_rect(s, {{in_window.origin.x + origin.x, in_window.origin.y + origin.y}, in_window.size});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^{_NSRange=QQ}");
    // Layout queries (view coordinates, flipped).
    method(t, "_alchemyCaretRect:", [this, ts](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ts(self);
        const auto style = style_of(*this, self, *state);
        const auto metrics = drawing::font_metrics(style.font);
        const auto lines = lines_of(*this, self, *state);
        const auto shown = shown_text(*state);
        const auto index = std::min<std::size_t>(s.x[2], shown.size());
        std::size_t row = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) if (lines[i].start <= index) row = i;
        const auto& line = lines[row];
        const auto x = caret_x(style.font, std::u16string_view(shown).substr(line.start, line.length), index - line.start);
        const auto top = state->inset.height + metrics.line_height() * static_cast<double>(row);
        return_rect(s, {{state->inset.width + line_padding(*this, *state) + x - state->scroll_x, top}, {1, metrics.ascent + metrics.descent}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:Q");
    method(t, "_alchemyIndexAtPoint:", [this, ts](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ts(self);
        const auto style = style_of(*this, self, *state);
        const auto metrics = drawing::font_metrics(style.font);
        const auto lines = lines_of(*this, self, *state);
        const auto shown = shown_text(*state);
        const auto point = point_arg(s);
        const auto row = static_cast<std::size_t>(std::clamp((point.y - state->inset.height) / metrics.line_height(), 0.0,
                                                             static_cast<double>(lines.size() - 1)));
        const auto& line = lines[row];
        const auto x = point.x - state->inset.width - line_padding(*this, *state) + state->scroll_x;
        const auto widths = drawing::prefix_widths(style.font, std::u16string_view(shown).substr(line.start, line.length));
        std::size_t offset = 0;
        while (offset < widths.size()) {
            const auto left = offset ? widths[offset - 1] : 0.0;
            if (x < (left + widths[offset]) / 2) break;
            ++offset;
        }
        s.x[0] = line.start + offset;
    }, "Q@:{CGPoint=dd}");
    // Properties.
    const auto boolean = [this, ts](const char* getter, const char* setter, bool TextState::*member) {
        method(text_class, getter, [ts, member](CpuState& s, GuestMemory&) { return_bool(s, (*ts(s.x[0])).*member); }, "B@:");
        method(text_class, setter, [this, ts, member](CpuState& s, GuestMemory&) {
            (*ts(s.x[0])).*member = bool_arg(s, 2);
            invalidate_view(s.x[0]);
        }, "v@:B");
    };
    boolean("isEditable", "setEditable:", &TextState::editable);
    boolean("isSelectable", "setSelectable:", &TextState::selectable);
    boolean("isRichText", "setRichText:", &TextState::rich_text);
    boolean("isFieldEditor", "setFieldEditor:", &TextState::field_editor);
    boolean("drawsBackground", "setDrawsBackground:", &TextState::draws_background);
    boolean("isHorizontallyResizable", "setHorizontallyResizable:", &TextState::horizontally_resizable);
    boolean("isVerticallyResizable", "setVerticallyResizable:", &TextState::vertically_resizable);
    const auto object = [this, ts](const char* getter, const char* setter, Address TextState::*member) {
        method(text_class, getter, [ts, member](CpuState& s, GuestMemory&) { s.x[0] = (*ts(s.x[0])).*member; });
        method(text_class, setter, [this, ts, member](CpuState& s, GuestMemory&) {
            assign((*ts(s.x[0])).*member, s.x[2]);
            invalidate_view(s.x[0]);
        }, "v@:@");
    };
    object("font", "setFont:", &TextState::font);
    object("textColor", "setTextColor:", &TextState::text_color);
    object("backgroundColor", "setBackgroundColor:", &TextState::background_color);
    method(t, "delegate", [ts](CpuState& s, GuestMemory&) { s.x[0] = ts(s.x[0])->delegate; });
    method(t, "setDelegate:", [ts](CpuState& s, GuestMemory&) { ts(s.x[0])->delegate = s.x[2]; }, "v@:@");
    method(t, "alignment", [ts](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(alignment_to_guest(ts(s.x[0])->alignment)); }, "q@:");
    method(t, "setAlignment:", [this, ts](CpuState& s, GuestMemory&) {
        ts(s.x[0])->alignment = alignment_from_guest(static_cast<std::int64_t>(s.x[2]));
        invalidate_view(s.x[0]);
    }, "v@:q");
    method(text_view, "textContainerInset", [ts](CpuState& s, GuestMemory&) { return_size(s, ts(s.x[0])->inset); }, "{CGSize=dd}@:");
    method(text_view, "setTextContainerInset:", [this, ts](CpuState& s, GuestMemory&) {
        ts(s.x[0])->inset = size_arg(s);
        invalidate_view(s.x[0]);
    }, "v@:{CGSize=dd}");
    method(t, "minSize", [ts](CpuState& s, GuestMemory&) { return_size(s, ts(s.x[0])->min_size); }, "{CGSize=dd}@:");
    method(t, "setMinSize:", [ts](CpuState& s, GuestMemory&) { ts(s.x[0])->min_size = size_arg(s); }, "v@:{CGSize=dd}");
    method(t, "maxSize", [ts](CpuState& s, GuestMemory&) { return_size(s, ts(s.x[0])->max_size); }, "{CGSize=dd}@:");
    method(t, "setMaxSize:", [ts](CpuState& s, GuestMemory&) { ts(s.x[0])->max_size = size_arg(s); }, "v@:{CGSize=dd}");
    // Editing conveniences with no effect on this plain-text view (no spell
    // checking, substitutions, font panel or undo manager are provided); the
    // requested value is kept for the getter.
    for (const auto* base : {"UsesFontPanel", "AllowsUndo", "ContinuousSpellCheckingEnabled", "GrammarCheckingEnabled",
             "AutomaticQuoteSubstitutionEnabled", "AutomaticDashSubstitutionEnabled", "AutomaticTextReplacementEnabled",
             "AutomaticSpellingCorrectionEnabled", "AutomaticLinkDetectionEnabled", "AutomaticDataDetectionEnabled",
             "SmartInsertDeleteEnabled", "UsesFindBar", "UsesRuler", "ImportsGraphics", "AllowsImageEditing",
             "DisplaysLinkToolTips", "AutomaticTextCompletionEnabled", "UsesInspectorBar", "IncrementalSearchingEnabled", "UsesFindPanel"}) {
        const std::string name = base;
        auto stored = std::make_shared<std::map<Address, bool>>();
        method(text_view, "set" + name + ":", [stored](CpuState& s, GuestMemory&) { (*stored)[s.x[0]] = bool_arg(s, 2); }, "v@:B");
        std::string getter = name;
        getter[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(getter[0])));
        const bool plain = name.starts_with("Uses") || name.starts_with("Allows") || name.starts_with("Displays") || name.starts_with("Imports");
        method(text_view, plain ? getter : "is" + name, [stored](CpuState& s, GuestMemory&) {
            const auto found = stored->find(s.x[0]);
            return_bool(s, found != stored->end() && found->second);
        }, "B@:");
    }
    // NSTextContainer.
    const auto container_class = declare_class("NSTextContainer", "NSObject");
    const auto cs = [this](Address value) { return objc.host_state<ContainerState>(value); };
    const auto relayout = [this, cs](Address container) {
        if (const auto owner = cs(container)->text_view) invalidate_view(owner);
    };
    method(container_class, "init", [this, cs](CpuState& s, GuestMemory&) {
        s.x[0] = objc.send_super(s.x[0], objc.find_class("NSTextContainer"), sel("init")).x0;
        static_cast<void>(cs(s.x[0]));
    });
    for (const auto* name : {"initWithSize:", "initWithContainerSize:"})
        method(container_class, name, [this, cs](CpuState& s, GuestMemory&) {
            const auto size = size_arg(s);
            s.x[0] = objc.send_super(s.x[0], objc.find_class("NSTextContainer"), sel("init")).x0;
            cs(s.x[0])->size = size;
        }, "@@:{CGSize=dd}");
    for (const auto* name : {"size", "containerSize"})
        method(container_class, name, [cs](CpuState& s, GuestMemory&) { return_size(s, cs(s.x[0])->size); }, "{CGSize=dd}@:");
    for (const auto* name : {"setSize:", "setContainerSize:"})
        method(container_class, name, [cs, relayout](CpuState& s, GuestMemory&) { cs(s.x[0])->size = size_arg(s); relayout(s.x[0]); }, "v@:{CGSize=dd}");
    method(container_class, "widthTracksTextView", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->tracks_width); }, "B@:");
    method(container_class, "setWidthTracksTextView:", [cs, relayout](CpuState& s, GuestMemory&) { cs(s.x[0])->tracks_width = bool_arg(s, 2); relayout(s.x[0]); }, "v@:B");
    method(container_class, "heightTracksTextView", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->tracks_height); }, "B@:");
    method(container_class, "setHeightTracksTextView:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->tracks_height = bool_arg(s, 2); }, "v@:B");
    method(container_class, "lineFragmentPadding", [cs](CpuState& s, GuestMemory&) { set_d(s, 0, cs(s.x[0])->padding); }, "d@:");
    method(container_class, "setLineFragmentPadding:", [cs, relayout](CpuState& s, GuestMemory&) { cs(s.x[0])->padding = d(s, 0); relayout(s.x[0]); }, "v@:d");
    method(container_class, "lineBreakMode", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->line_break); }, "Q@:");
    // Word wrapping is the only line breaking the text layout does.
    method(container_class, "setLineBreakMode:", [cs](CpuState& s, GuestMemory&) {
        if (s.x[2] != 0) unsupported("-[NSTextContainer setLineBreakMode:] other than NSLineBreakByWordWrapping");
        cs(s.x[0])->line_break = 0;
    }, "v@:Q");
    method(container_class, "textView", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->text_view; });
    method(text_view, "textContainer", [this, ts, cs, container_class](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        if (!state->container) {
            state->container = alloc_init(container_class);
            cs(state->container)->text_view = s.x[0];
        }
        s.x[0] = state->container;
    });
    method(text_view, "textContainerOrigin", [ts](CpuState& s, GuestMemory&) {
        const auto inset = ts(s.x[0])->inset;
        return_point(s, {inset.width, inset.height});
    }, "{CGPoint=dd}@:");
    method(t, "sizeToFit", [this, ts](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ts(self);
        if (state->field_editor) return;
        const auto style = style_of(*this, self, *state);
        const auto lines = lines_of(*this, self, *state);
        const auto height = std::ceil(drawing::font_metrics(style.font).line_height() * static_cast<double>(lines.size()) +
                                      2 * state->inset.height + 2);
        auto frame = view_state(self)->frame.size;
        if (state->vertically_resizable) frame.height = std::clamp(std::max(height, state->min_size.height), 0.0, state->max_size.height);
        // At least the enclosing clip view's height (scroll views' document views).
        if (const auto parent = view_state(self)->superview) frame.height = std::max(frame.height, view_bounds(*view_state(parent)).size.height);
        CallRegs registers;
        put_size(registers, 0, frame);
        static_cast<void>(send(self, "setFrameSize:", registers));
        // The enclosing scroll view's scrollers follow the new document size
        // (tiling again shows an autohidden scroller the text now needs).
        if (const auto parent = view_state(self)->superview; parent && is_kind(parent, clip_view))
            if (const auto owner = view_state(parent)->superview; owner && is_kind(owner, scroll_view))
                static_cast<void>(send(owner, "tile"));
    }, "v@:");
    method(t, "scrollRangeToVisible:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto caret = result_rect(send(s.x[0], "_alchemyCaretRect:", registers));
        CallRegs scroll;
        put_rect(scroll, 0, caret);
        static_cast<void>(send(s.x[0], "scrollRectToVisible:", scroll));
    }, "v@:{_NSRange=QQ}");

    // ---- responder ----
    method(t, "acceptsFirstResponder", [ts](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        return_bool(s, state->editable || state->selectable);
    }, "B@:");
    method(t, "becomeFirstResponder", [this](CpuState& s, GuestMemory&) { invalidate_view(s.x[0]); s.x[0] = 1; }, "B@:");
    method(t, "resignFirstResponder", [this, ts](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ts(self);
        invalidate_view(self);
        if (state->field_editor && state->client) {
            const auto client = state->client;
            if (!send_bool(client, "textShouldEndEditing:", {self})) { s.x[0] = 0; return; }
            end_field_editing(client, false);
        }
        s.x[0] = 1;
    }, "B@:");
    method(t, "keyDown:", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(s.x[0], "interpretKeyEvents:", {auto_array({s.x[2]})}));
    }, "v@:@");
    method(t, "doCommandBySelector:", [this, ts](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], command = s.x[2];
        const auto state = ts(self);
        // The edited text field (and its delegate) may take the command.
        if (state->field_editor && state->client && send_bool(state->client, "textView:doCommandBySelector:", {self, command})) return;
        if (objc.responds(self, command)) {
            CallRegs registers;
            registers.x[2] = self;
            static_cast<void>(objc.send(self, command, registers));
            return;
        }
        if (const auto server = window_server_or_null()) server->beep();
    }, "v@::");
    const auto move_to = [this, ts](Address self, std::size_t position, bool extend) {
        const auto state = ts(self);
        position = std::min(position, state->value.size());
        if (extend) {
            const auto a = std::min(state->anchor, position), b = std::max(state->anchor, position);
            state->selection = a;
            state->selection_length = b - a;
        } else {
            state->selection = position;
            state->selection_length = 0;
            state->anchor = position;
        }
        state->marked.reset();
        invalidate_view(self);
    };
    const auto caret = [ts](Address self, bool extend) {
        const auto state = ts(self);
        if (extend) return state->selection == state->anchor ? state->selection + state->selection_length : state->selection;
        return state->selection;
    };
    const auto command = [this](const char* name, std::function<void(Address)> body) {
        method(text_class, name, [body](CpuState& s, GuestMemory&) { body(s.x[0]); }, "v@:@");
    };
    command("moveLeft:", [ts, move_to](Address self) {
        const auto state = ts(self);
        move_to(self, state->selection_length ? state->selection : (state->selection ? state->selection - 1 : 0), false);
    });
    command("moveRight:", [ts, move_to](Address self) {
        const auto state = ts(self);
        move_to(self, state->selection_length ? state->selection + state->selection_length : state->selection + 1, false);
    });
    command("moveLeftAndModifySelection:", [move_to, caret](Address self) {
        const auto at = caret(self, true);
        move_to(self, at ? at - 1 : 0, true);
    });
    command("moveRightAndModifySelection:", [move_to, caret](Address self) { move_to(self, caret(self, true) + 1, true); });
    for (const auto* name : {"moveToBeginningOfLine:", "moveToBeginningOfDocument:", "moveToBeginningOfParagraph:", "moveUp:",
             "scrollToBeginningOfDocument:"})
        command(name, [move_to](Address self) { move_to(self, 0, false); });
    for (const auto* name : {"moveToEndOfLine:", "moveToEndOfDocument:", "moveToEndOfParagraph:", "moveDown:", "scrollToEndOfDocument:"})
        command(name, [ts, move_to](Address self) { move_to(self, ts(self)->value.size(), false); });
    command("moveToBeginningOfLineAndModifySelection:", [move_to](Address self) { move_to(self, 0, true); });
    command("moveToEndOfLineAndModifySelection:", [ts, move_to](Address self) { move_to(self, ts(self)->value.size(), true); });
    command("deleteBackward:", [ts, replace](Address self) {
        const auto state = ts(self);
        if (!state->editable) return;
        if (state->selection_length) { replace(self, state->selection, state->selection_length, u""); return; }
        if (!state->selection) return;
        auto from = state->selection - 1;
        if (from && state->value[from] >= 0xdc00 && state->value[from] < 0xe000) --from;   // a surrogate pair
        replace(self, from, state->selection - from, u"");
    });
    command("deleteForward:", [ts, replace](Address self) {
        const auto state = ts(self);
        if (!state->editable) return;
        if (state->selection_length) { replace(self, state->selection, state->selection_length, u""); return; }
        if (state->selection >= state->value.size()) return;
        replace(self, state->selection, 1, u"");
    });
    command("insertNewline:", [this, ts, replace](Address self) {
        const auto state = ts(self);
        if (state->field_editor && state->client) {
            // Return in a text field: end editing, send the action and keep
            // editing with the text selected (AppKit's field editor).
            const auto client = state->client;
            end_field_editing(client, true);
            if (const auto owner = view_state(client)->window)
                if (window_state(owner)->first_responder == client) {
                    begin_field_editing(client);
                    if (const auto editor = extension<ControlState>(client)->editor) window_state(owner)->first_responder = editor;
                }
            return;
        }
        if (state->editable) replace(self, state->selection, state->selection_length, u"\n");
    });
    for (const auto* name : {"insertTab:", "insertBacktab:"})
        command(name, [this, ts, replace](Address self) {
            const auto state = ts(self);
            if (state->field_editor && state->client) {
                const auto client = state->client;
                const auto owner = view_state(client)->window;
                end_field_editing(client, false);
                if (owner) window_state(owner)->first_responder = owner;
                return;
            }
            if (state->editable) replace(self, state->selection, state->selection_length, u"\t");
        });
    command("cancelOperation:", [this, ts](Address self) {
        const auto state = ts(self);
        if (state->field_editor && state->client) {
            // Escape restores the field's value from before editing.
            const auto client = state->client;
            if (const auto target_cell = control_cell(*this, client)) state->value = cell_text(*this, target_cell);
            state->aborting = true;
            end_field_editing(client, false);
        }
    });
    command("copy:", [this, ts](Address self) {
        const auto state = ts(self);
        if (!state->selection_length || state->secure) return;
        if (const auto server = window_server_or_null())
            server->set_clipboard_text(drawing::utf8(std::u16string_view(state->value).substr(state->selection, state->selection_length)));
    });
    command("cut:", [this, ts, replace](Address self) {
        const auto state = ts(self);
        if (!state->selection_length || state->secure || !state->editable) return;
        if (const auto server = window_server_or_null())
            server->set_clipboard_text(drawing::utf8(std::u16string_view(state->value).substr(state->selection, state->selection_length)));
        replace(self, state->selection, state->selection_length, u"");
    });
    command("paste:", [this, ts, replace](Address self) {
        const auto state = ts(self);
        if (!state->editable) return;
        const auto server = window_server_or_null();
        const auto clip = server ? server->clipboard_text() : std::nullopt;
        if (!clip) return;
        auto value = drawing::utf16(*clip);
        if (state->field_editor) std::replace_if(value.begin(), value.end(), [](char16_t c) { return c == u'\n' || c == u'\r'; }, u' ');
        replace(self, state->selection, state->selection_length, value);
    });
    // -delete: removes the selection without touching the pasteboard.
    command("delete:", [ts, replace](Address self) {
        const auto state = ts(self);
        if (state->editable && state->selection_length) replace(self, state->selection, state->selection_length, u"");
    });
    // A paragraph break is a newline here (the field editor ends editing, as for Return).
    command("insertParagraphSeparator:", [this](Address self) { static_cast<void>(send_id(self, "insertNewline:", {self})); });
    // A tab character even in the field editor.
    command("insertTabIgnoringFieldEditor:", [ts, replace](Address self) {
        const auto state = ts(self);
        if (state->editable) replace(self, state->selection, state->selection_length, u"\t");
    });
    command("noop:", [](Address) {});
    method(t, "baseWritingDirection", [ts](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ts(s.x[0])->writing_direction); }, "q@:");
    method(t, "setBaseWritingDirection:", [this, ts](CpuState& s, GuestMemory&) {
        ts(s.x[0])->writing_direction = static_cast<std::int64_t>(s.x[2]);
        invalidate_view(s.x[0]);
    }, "v@:q");
    // The menu toggles of the text view's settings flip the kept values.
    for (const auto* base : {"ContinuousSpellChecking", "GrammarChecking", "AutomaticQuoteSubstitution", "AutomaticDashSubstitution",
             "AutomaticLinkDetection", "AutomaticDataDetection", "AutomaticTextReplacement", "AutomaticSpellingCorrection",
             "AutomaticTextCompletion", "SmartInsertDelete"}) {
        const std::string name = base;
        method(text_view, "toggle" + name + ":", [this, name](CpuState& s, GuestMemory&) {
            const bool on = send_bool(s.x[0], "is" + name + "Enabled");
            static_cast<void>(send_id(s.x[0], "set" + name + "Enabled:", {on ? 0u : 1u}));
        }, "v@:@");
    }
    // Services pasteboards: the selection as plain text.
    const auto plain_types = [this](Address board_types) {
        for (const auto type : items(board_types)) {
            const auto name = text(type);
            if (name == "public.utf8-plain-text" || name == "NSStringPboardType" || name == "public.plain-text") return true;
        }
        return false;
    };
    method(text_view, "writeSelectionToPasteboard:types:", [this, ts, plain_types](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        if (!s.x[2] || !state->selection_length || state->secure || !plain_types(s.x[3])) { s.x[0] = 0; return; }
        const auto selected = drawing::utf8(std::u16string_view(state->value).substr(state->selection, state->selection_length));
        static_cast<void>(send_id(s.x[2], "declareTypes:owner:", {auto_array({auto_string("public.utf8-plain-text")}), 0}));
        s.x[0] = send_id(s.x[2], "setString:forType:", {auto_string(selected), auto_string("public.utf8-plain-text")}) & 0xff;
    }, "B@:@@");
    method(text_view, "writeSelectionToPasteboard:type:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = send_id(s.x[0], "writeSelectionToPasteboard:types:", {s.x[2], s.x[3] ? auto_array({s.x[3]}) : auto_array({})}) & 0xff;
    }, "B@:@@");
    method(text_view, "readSelectionFromPasteboard:", [this, ts, replace](CpuState& s, GuestMemory&) {
        const auto state = ts(s.x[0]);
        const auto value = s.x[2] && state->editable ? send_id(s.x[2], "stringForType:", {auto_string("public.utf8-plain-text")}) : 0;
        if (!value) { s.x[0] = 0; return; }
        replace(s.x[0], state->selection, state->selection_length, drawing::utf16(text(value)));
        s.x[0] = 1;
    }, "B@:@");
    method(text_view, "readSelectionFromPasteboard:type:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = send_id(s.x[0], "readSelectionFromPasteboard:", {s.x[2]}) & 0xff;
    }, "B@:@@");

    // ---- mouse: caret placement and drag selection ----
    method(t, "mouseDown:", [this, ts, move_to](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto state = ts(self);
        if (!state->selectable && !state->editable) return;
        if (const auto owner = view_state(self)->window)
            if (window_state(owner)->first_responder != self) static_cast<void>(send_id(owner, "makeFirstResponder:", {self}));
        const auto index_at = [&](Point local) {
            CallRegs registers;
            put_point(registers, 0, local);
            return static_cast<std::size_t>(send(self, "_alchemyIndexAtPoint:", registers).x0);
        };
        const auto clicks = event_state(event)->event.click_count;
        if (clicks >= 2) { static_cast<void>(send_id(self, "selectAll:", {0})); return; }
        move_to(self, index_at(event_location_in(*this, self, event)), false);
        static_cast<void>(track_mouse(*this, self, event, [](bool) {}, [&](Point local) { move_to(self, index_at(local), true); }));
    }, "v@:@");

    // ---- drawing ----
    method(t, "drawRect:", [this, ts](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ts(self);
        const auto bounds = view_bounds(*view_state(self));
        const auto& th = theme_for(self);
        resolve_dynamic_colors(*this, self);
        auto& canvas = require_canvas("NSTextView drawing");
        if (state->draws_background) canvas.fill_rect(bounds, color_value(state->background_color, th.text_background));
        const auto style = style_of(*this, self, *state);
        const auto metrics = drawing::font_metrics(style.font);
        const auto lines = lines_of(*this, self, *state);
        const auto shown = shown_text(*state);
        const auto owner = view_state(self)->window;
        const bool focused = owner && window_state(owner)->first_responder == self && window_state(owner)->key;
        // Field editor: keep the caret inside the visible width.
        if (state->field_editor) {
            const auto x = caret_x(style.font, shown, state->selection + state->selection_length);
            const auto visible = bounds.size.width - 4;
            if (x - state->scroll_x > visible) state->scroll_x = x - visible;
            if (x - state->scroll_x < 0) state->scroll_x = x;
        }
        const double left = state->inset.width + line_padding(*this, *state) - state->scroll_x;
        // A field editor centres its single line.
        const double first_top = state->field_editor ? std::max(0.0, (bounds.size.height - (metrics.ascent + metrics.descent)) / 2)
                                                     : state->inset.height;
        canvas.save();
        canvas.clip_to_rect(bounds);
        for (std::size_t row = 0; row < lines.size(); ++row) {
            const auto& line = lines[row];
            const auto top = first_top + metrics.line_height() * static_cast<double>(row);
            if (top > bounds.origin.y + bounds.size.height) break;
            if (top + metrics.line_height() < bounds.origin.y) continue;
            const auto text_line = std::u16string_view(shown).substr(line.start, line.length);
            const auto widths = drawing::prefix_widths(style.font, text_line);
            const auto at = [&](std::size_t offset) { return offset && !widths.empty() ? widths[std::min(offset, widths.size()) - 1] : 0.0; };
            // Selection.
            const auto from = std::max(state->selection, line.start);
            const auto to = std::min(state->selection + state->selection_length, line.start + line.length);
            if (state->selection_length && from < to) {
                const auto color = focused ? th.selection.with_alpha(th.dark ? 0.75 : 0.35) : th.unfocused_selection;
                canvas.fill_rect({{left + at(from - line.start), top}, {at(to - line.start) - at(from - line.start), metrics.line_height()}}, color);
            }
            auto drawn = text_line;
            while (!drawn.empty() && (drawn.back() == u'\r' || drawn.back() == u'\n')) drawn.remove_suffix(1);
            // Each character sits at its laid-out position. The canvas draws
            // with the device size's hinted advances, which differ from the
            // measured (outline) widths by up to half a pixel per character
            // at small sizes; drawn as one run, a line drifts past its wrap
            // width (into the padding, under a scroller) and away from the
            // caret and selection, and drawn by words, the drift inside a
            // word eats the space after it.
            for (std::size_t from = 0; from < drawn.size();) {
                auto to = from + 1;
                if ((drawn[from] & 0xfc00) == 0xd800 && to < drawn.size() && (drawn[to] & 0xfc00) == 0xdc00) ++to;   // a surrogate pair
                while (to < drawn.size() && drawn[to] >= 0x300 && drawn[to] < 0x370) ++to;   // combining marks stay with their base
                if (drawn[from] != u' ' && drawn[from] != u'\t')
                    canvas.draw_text(drawn.substr(from, to - from), style.font, {left + at(from), top + metrics.ascent}, style.color);
                from = to;
            }
            // Marked (composing) text is underlined.
            if (state->marked) {
                const auto m0 = std::max(state->marked->first, line.start);
                const auto m1 = std::min(state->marked->first + state->marked->second, line.start + line.length);
                if (m0 < m1)
                    canvas.fill_rect({{left + at(m0 - line.start), top + metrics.ascent + 2}, {at(m1 - line.start) - at(m0 - line.start), 1}}, style.color);
            }
            // Caret.
            const auto caret_at = state->selection + state->selection_length;
            const bool last = row + 1 == lines.size();
            const bool on_line = caret_at >= line.start && (caret_at < line.start + line.length || last ||
                                                            (caret_at == line.start + line.length && lines[row + 1].start > caret_at));
            if (focused && state->editable && !state->selection_length && on_line)
                canvas.fill_rect({{std::round(left + at(caret_at - line.start)), top}, {1, metrics.ascent + metrics.descent}},
                                 color_value(state->insertion_color, th.text));
        }
        canvas.restore();
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // NSTextField's own key presses while editing reach the field editor.
    method(text_field, "keyDown:", [this](CpuState& s, GuestMemory&) {
        if (const auto editor = extension<ControlState>(s.x[0])->editor) { static_cast<void>(send_id(editor, "keyDown:", {s.x[2]})); return; }
        forward(s.x[0], "keyDown:", s.x[2]);
    }, "v@:@");
}

} // namespace alchemy::gui::appkit
