#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// NSTokenField / NSTokenFieldCell: a text field whose value is an array of
// tokens (represented objects). Not editing, the tokens are drawn as pills
// (rounded, or squared for the squared styles) with their display strings;
// NSTokenStyleNone draws them as plain text. Editing goes through the text
// field's field editor: the tokens become their editing strings joined by the
// tokenizing character, and when editing ends the text is tokenized again:
// split at the tokenizing characters (a comma by default), trimmed, each
// piece mapped to a represented object by the delegate
// (-tokenField:representedObjectForEditingString:, else the string), with
// -tokenField:shouldAddObjects:atIndex: asked for the pieces that are not
// already tokens. Display and editing strings come from the delegate
// (-tokenField:displayStringForRepresentedObject:, ...editingString...),
// else the object (a string, or its description). Not provided: tokens made
// while typing (they are made when editing ends), completions (the
// completion delay is kept) and token menus.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

// NSTokenStyle
constexpr std::int64_t style_none = 1, style_squared = 3, style_plain_squared = 4;

struct TokenState final : ObjectPayload {
    std::vector<Address> tokens;       // +1 represented objects
    Address tokenizing{};              // +1 NSCharacterSet (nil: the comma)
    std::int64_t style{};              // NSTokenStyleDefault
    double completion_delay{};
};
std::shared_ptr<TokenState> tokens_of(AppKit& kit, Address cell) { return cell_extension<TokenState>(kit, cell); }

Address delegate_of(AppKit& kit, Address cell) {
    const auto owner = cell_state(kit, cell)->control_view;
    return owner ? kit.send(owner, "delegate").x0 : 0;
}

std::u16string text_of(AppKit& kit, Address object) {
    if (!object) return {};
    const auto string_class = kit.objc.find_class("NSString");
    if (string_class && kit.is_kind(object, string_class)) return kit.string16(object);
    return kit.string16(kit.send(object, "description").x0);
}

// The string the delegate gives for a token (`selector`), else the object's own.
std::u16string token_string(AppKit& kit, Address cell, Address token, const char* selector) {
    const auto owner = cell_state(kit, cell)->control_view;
    const auto delegate = delegate_of(kit, cell);
    if (delegate && kit.responds(delegate, selector))
        if (const auto answer = kit.send_id(delegate, selector, {owner, token})) return kit.string16(answer);
    return text_of(kit, token);
}

char16_t joiner(AppKit& kit, const TokenState& state) {
    if (!state.tokenizing) return u',';
    for (const char16_t candidate : {u',', u';', u'\n', u' '})
        if (kit.send_bool(state.tokenizing, "characterIsMember:", {candidate})) return candidate;
    return u',';
}

// The cell's text (what the field editor edits) follows the tokens.
void sync_text(AppKit& kit, Address cell) {
    const auto state = tokens_of(kit, cell);
    std::u16string text;
    const auto separator = joiner(kit, *state);
    for (std::size_t i = 0; i < state->tokens.size(); ++i) {
        if (i) { text += separator; if (separator == u',' || separator == u';') text += u' '; }
        text += token_string(kit, cell, state->tokens[i], "tokenField:editingStringForRepresentedObject:");
    }
    kit.release(std::exchange(cell_state(kit, cell)->contents, kit.string(drawing::utf8(text))));
    if (const auto owner = cell_state(kit, cell)->control_view) kit.invalidate_view(owner);
}

void set_tokens(AppKit& kit, Address cell, std::vector<Address> tokens) {
    const auto state = tokens_of(kit, cell);
    for (const auto token : tokens) static_cast<void>(kit.retain(token));
    for (const auto token : state->tokens) kit.release(token);
    state->tokens = std::move(tokens);
    sync_text(kit, cell);
}

bool is_separator(AppKit& kit, const TokenState& state, char16_t c) {
    if (!state.tokenizing) return c == u',';
    return kit.send_bool(state.tokenizing, "characterIsMember:", {static_cast<Address>(c)});
}

std::u16string trimmed(std::u16string_view text) {
    const auto space = [](char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r'; };
    std::size_t begin = 0, end = text.size();
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    return std::u16string(text.substr(begin, end - begin));
}

// Text to tokens: pieces already shown keep their objects; new ones come
// from the delegate and are offered to -tokenField:shouldAddObjects:atIndex:.
void tokenize(AppKit& kit, Address cell, std::u16string_view text) {
    const auto state = tokens_of(kit, cell);
    const auto owner = cell_state(kit, cell)->control_view;
    const auto delegate = delegate_of(kit, cell);
    std::vector<std::u16string> pieces;
    std::u16string current;
    for (const auto c : text) {
        if (is_separator(kit, *state, c)) { pieces.push_back(trimmed(current)); current.clear(); }
        else current += c;
    }
    pieces.push_back(trimmed(current));
    std::vector<Address> result;
    std::vector<bool> taken(state->tokens.size(), false);
    for (const auto& piece : pieces) {
        if (piece.empty()) continue;
        Address object{};
        for (std::size_t i = 0; i < state->tokens.size() && !object; ++i)
            if (!taken[i] && token_string(kit, cell, state->tokens[i], "tokenField:editingStringForRepresentedObject:") == piece) {
                taken[i] = true;
                object = state->tokens[i];
            }
        if (!object) {
            const auto string = kit.auto_string(drawing::utf8(piece));
            object = delegate && kit.responds(delegate, "tokenField:representedObjectForEditingString:")
                         ? kit.send_id(delegate, "tokenField:representedObjectForEditingString:", {owner, string}) : string;
            if (!object) continue;
            if (delegate && kit.responds(delegate, "tokenField:shouldAddObjects:atIndex:")) {
                const auto allowed = kit.send_id(delegate, "tokenField:shouldAddObjects:atIndex:",
                                                 {owner, kit.auto_array({object}), static_cast<Address>(result.size())});
                for (const auto added : allowed ? kit.items(allowed) : std::vector<Address>{}) result.push_back(added);
                continue;
            }
        }
        result.push_back(object);
    }
    set_tokens(kit, cell, std::move(result));
}

void draw_tokens(AppKit& kit, Address cell, Rect frame, Address owner) {
    const auto state = tokens_of(kit, cell);
    const auto info = cell_state(kit, cell);
    const auto& th = kit.theme_for(owner);
    auto& canvas = kit.require_canvas("NSTokenFieldCell drawing");
    if (info->bezeled || info->bordered) draw_bezel(kit, frame, 0, kit.color_value(info->background_color, th.text_background), th.control_border);
    auto style = cell_text_style(kit, cell, th);
    if (!info->enabled) style.color = th.disabled_text;
    const double height = std::max(0.0, std::min(frame.size.height - 4, 19.0));
    const double top = frame.origin.y + (frame.size.height - height) / 2;
    const double radius = state->style == style_squared || state->style == style_plain_squared ? 3 : height / 2;
    double x = frame.origin.x + 4;
    const double limit = frame.origin.x + frame.size.width - 4;
    if (state->tokens.empty() && info->placeholder) {
        style.color = th.secondary_text;
        draw_line_in(kit, kit.string16(info->placeholder), style, {{x, frame.origin.y}, {std::max(0.0, limit - x), frame.size.height}}, 0);
        return;
    }
    for (const auto token : state->tokens) {
        const auto text = token_string(kit, cell, token, "tokenField:displayStringForRepresentedObject:");
        const double width = std::ceil(drawing::text_width(style.font, text)) + 12;
        if (x >= limit) break;
        const Rect pill{{x, top}, {std::min(width, limit - x), height}};
        if (state->style != style_plain_squared) {
            drawing::Path path;
            path.add_rounded_rect(pill, radius, radius);
            canvas.fill_path(path, th.accent.with_alpha(0.18));
            canvas.stroke_path(path, th.accent.with_alpha(0.55), 1);
        }
        draw_line_in(kit, text, style, {{pill.origin.x + 6, pill.origin.y}, {std::max(0.0, pill.size.width - 12), pill.size.height}}, 1);
        x += width + 3;
    }
}

} // namespace

void install_token_field(AppKit& kit) {
    auto* const k = &kit;
    const auto c = kit.declare_class("NSTokenFieldCell", "NSTextFieldCell");
    const auto f = kit.declare_class("NSTokenField", "NSTextField");
    static_cast<void>(kit.send_id(f, "setCellClass:", {c}));
    for (const auto owner : {c, f}) {
        kit.class_method(owner, "defaultCompletionDelay", [](CpuState& s, GuestMemory&) { set_d(s, 0, 0); }, "d@:");
        kit.class_method(owner, "defaultTokenizingCharacterSet", [k](CpuState& s, GuestMemory&) {
            const auto set_class = k->objc.find_class("NSCharacterSet");
            if (!set_class) throw std::runtime_error("unsupported: Alchemy: +defaultTokenizingCharacterSet without NSCharacterSet");
            s.x[0] = k->send_id(set_class, "characterSetWithCharactersInString:", {k->auto_string(",")});
        });
    }
    kit.method(c, "objectValue", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(tokens_of(*k, s.x[0])->tokens); });
    kit.method(c, "setObjectValue:", [k](CpuState& s, GuestMemory&) {
        const auto value = s.x[2];
        const auto array_class = k->objc.find_class("NSArray");
        if (!value) set_tokens(*k, s.x[0], {});
        else if (array_class && k->is_kind(value, array_class)) set_tokens(*k, s.x[0], k->items(value));
        else tokenize(*k, s.x[0], text_of(*k, value));
    }, "v@:@");
    kit.method(c, "setStringValue:", [k](CpuState& s, GuestMemory&) { tokenize(*k, s.x[0], s.x[2] ? k->string16(s.x[2]) : std::u16string()); }, "v@:@");
    kit.method(c, "tokenStyle", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(tokens_of(*k, s.x[0])->style); }, "q@:");
    kit.method(c, "setTokenStyle:", [k](CpuState& s, GuestMemory&) {
        tokens_of(*k, s.x[0])->style = static_cast<std::int64_t>(s.x[2]);
        if (const auto owner = cell_state(*k, s.x[0])->control_view) k->invalidate_view(owner);
    }, "v@:q");
    kit.method(c, "tokenizingCharacterSet", [k](CpuState& s, GuestMemory&) {
        const auto set = tokens_of(*k, s.x[0])->tokenizing;
        s.x[0] = set ? set : k->send(k->objc.object_class(s.x[0]), "defaultTokenizingCharacterSet").x0;
    });
    kit.method(c, "setTokenizingCharacterSet:", [k](CpuState& s, GuestMemory&) {
        k->assign(tokens_of(*k, s.x[0])->tokenizing, s.x[2]);
        sync_text(*k, s.x[0]);
    }, "v@:@");
    kit.method(c, "completionDelay", [k](CpuState& s, GuestMemory&) { set_d(s, 0, tokens_of(*k, s.x[0])->completion_delay); }, "d@:");
    kit.method(c, "setCompletionDelay:", [k](CpuState& s, GuestMemory&) { tokens_of(*k, s.x[0])->completion_delay = d(s, 0); }, "v@:d");
    kit.method(c, "drawWithFrame:inView:", [k, c](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const bool editing = owner && k->is_kind(owner, k->control) && k->extension<ControlState>(owner)->editor;
        if (editing || tokens_of(*k, self)->style == style_none) {
            CallRegs registers;
            put_rect(registers, 0, rect_arg(s));
            registers.x[2] = owner;
            static_cast<void>(k->objc.send_super(self, c, k->sel("drawWithFrame:inView:"), registers));
            return;
        }
        draw_tokens(*k, self, rect_arg(s), owner);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(c, "dealloc", [k, c](CpuState& s, GuestMemory&) {
        if (const auto cell = k->objc.find_host_state<CellState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<TokenState>(cell->extension)) {
                for (const auto token : state->tokens) k->release(token);
                state->tokens.clear();
                k->release(std::exchange(state->tokenizing, 0));
            }
        static_cast<void>(k->objc.send_super(s.x[0], c, k->sel("dealloc")));
    }, "v@:");

    // The control: its cell's API; the edited text is tokenized when editing ends.
    for (const auto& [name, types, redraw] : std::initializer_list<std::tuple<const char*, const char*, bool>>{
             {"tokenStyle", "q@:", false}, {"setTokenStyle:", "v@:q", true}, {"tokenizingCharacterSet", "@@:", false},
             {"setTokenizingCharacterSet:", "v@:@", true}, {"completionDelay", "d@:", false}, {"setCompletionDelay:", "v@:d", false}})
        forward_to_cell(kit, f, name, types, redraw);
    kit.method(f, "textDidEndEditing:", [k, f](CpuState& s, GuestMemory&) {
        if (const auto cell = control_cell(*k, s.x[0])) tokenize(*k, cell, cell_text(*k, cell));
        CallRegs registers;
        registers.x[2] = s.x[2];
        static_cast<void>(k->objc.send_super(s.x[0], f, k->sel("textDidEndEditing:"), registers));
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit::controls
