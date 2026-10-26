#include "alchemy/darwin/appkit/controls_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>

// AppKit's standard controls on the software drawing layer: NSCell /
// NSActionCell, NSControl (target-action, cell forwarding, tracking),
// NSButtonCell / NSButton (push, checkbox, radio, toggle), NSTextFieldCell /
// NSTextField / NSSecureTextField (labels and editable fields; the editing
// itself is the field editor of text.cpp), NSImageCell / NSImageView,
// NSProgressIndicator, NSBox, NSPopUpButtonCell / NSPopUpButton with a popup
// menu window, and the default-button / key-equivalent path of NSWindow.
// Everything draws with a flat macOS-like theme (graphics.cpp Theme).
namespace alchemy::gui::appkit {
namespace {
using drawing::Color;

constexpr std::uint64_t left_mouse_up_mask = 1ULL << event_type::left_mouse_up;
constexpr std::uint64_t left_mouse_dragged_mask = 1ULL << event_type::left_mouse_dragged;
constexpr std::uint64_t left_mouse_down_mask = 1ULL << event_type::left_mouse_down;
constexpr std::uint64_t mouse_moved_mask = 1ULL << event_type::mouse_moved;
constexpr std::uint64_t key_down_mask = 1ULL << event_type::key_down;

Rect inset(Rect r, double dx, double dy) {
    return {{r.origin.x + dx, r.origin.y + dy}, {std::max(0.0, r.size.width - 2 * dx), std::max(0.0, r.size.height - 2 * dy)}};
}
bool flipped_context(AppKit& kit) { return kit.current_flipped(); }
std::weak_ptr<AppKit> installed_kit;

// A cell's image: template images take the cell's -contentTintColor (their
// alpha in the tint), as AppKit tints them; others draw as they are.
void draw_cell_image(AppKit& kit, const CellState& state, Address image, Rect rect, double alpha) {
    const bool is_template = state.content_tint && kit.responds(image, "isTemplate") && kit.send_bool(image, "isTemplate");
    if (!is_template) { kit.draw_image_in(image, rect, alpha, true); return; }
    auto pixels = kit.image_pixels(image);
    if (!pixels) return;
    const auto tint = kit.color_value(state.content_tint);
    const auto channel = [](double value) { return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255), 0L, 255L)); };
    for (std::size_t i = 0; i + 3 < pixels->rgba.size(); i += 4) {
        pixels->rgba[i] = channel(tint.r);
        pixels->rgba[i + 1] = channel(tint.g);
        pixels->rgba[i + 2] = channel(tint.b);
    }
    kit.require_canvas("tinted image").draw_image(*pixels, rect, alpha * tint.a, kit.current_flipped());
}
} // namespace

std::shared_ptr<AppKit> installed_appkit() { return installed_kit.lock(); }

namespace {
std::atomic<DynamicColorResolver> dynamic_color_resolver{};
} // namespace
void set_dynamic_color_resolver(DynamicColorResolver resolver) { dynamic_color_resolver.store(resolver); }
void resolve_dynamic_colors(AppKit& kit, Address view) {
    if (const auto resolver = dynamic_color_resolver.load()) resolver(kit, view);
}

// ---- shared helpers ------------------------------------------------------------------------
Address control_cell(AppKit& kit, Address value) {
    if (!value || !kit.is_kind(value, kit.control)) return 0;
    return kit.extension<ControlState>(value)->cell;
}
std::shared_ptr<CellState> cell_state(AppKit& kit, Address value) {
    if (!value) fail("nil NSCell");
    return kit.objc.host_state<CellState>(value);
}
std::u16string cell_text(AppKit& kit, Address value) {
    const auto contents = cell_state(kit, value)->contents;
    if (!contents) return {};
    if (kit.cf.kind(contents) == ObjectKind::number) {
        const auto number = kit.cf.number_value(contents).value_or(0);
        if (number == std::floor(number) && std::abs(number) < 1e15) return drawing::utf16(std::to_string(static_cast<std::int64_t>(number)));
        return drawing::utf16(kit.text(kit.cf.description(contents)));
    }
    return kit.string16(contents);
}
TextStyle cell_text_style(AppKit& kit, Address value, const Theme& theme) {
    const auto state = cell_state(kit, value);
    TextStyle style;
    style.font = kit.font_spec(state->font);
    if (!state->font) style.font.size = state->control_size == 1 ? 11 : state->control_size == 2 ? 9 : 13;
    style.color = state->enabled ? kit.color_value(state->title_color ? state->title_color : state->text_color, theme.text) : theme.disabled_text;
    style.alignment = state->alignment;
    style.line_break = state->line_break;
    return style;
}
std::u16string truncate_text(const drawing::FontSpec& font, std::u16string_view value, double width) {
    if (drawing::text_width(font, value) <= width) return std::u16string(value);
    const std::u16string ellipsis = std::u16string(1, char16_t{0x2026});
    const auto widths = drawing::prefix_widths(font, value);
    const auto room = width - drawing::text_width(font, ellipsis);
    std::size_t keep = 0;
    while (keep < widths.size() && widths[keep] <= room) ++keep;
    return std::u16string(value.substr(0, keep)) + ellipsis;
}
void draw_line_in(AppKit& kit, std::u16string_view value, const TextStyle& style, Rect rect, std::int64_t alignment, bool truncate) {
    auto& canvas = kit.require_canvas("control text");
    const auto metrics = drawing::font_metrics(style.font);
    const auto shown = truncate ? truncate_text(style.font, value, rect.size.width) : std::u16string(value);
    const auto width = drawing::text_width(style.font, shown);
    double x = rect.origin.x;
    if (alignment == 1) x += rect.size.width - width;
    else if (alignment == 2) x += (rect.size.width - width) / 2;
    const auto content = metrics.ascent + metrics.descent;
    const auto top_gap = (rect.size.height - content) / 2;
    const bool flipped = flipped_context(kit);
    const double baseline = flipped ? rect.origin.y + top_gap + metrics.ascent : rect.origin.y + top_gap + metrics.descent;
    canvas.save();
    canvas.clip_to_rect(rect);
    canvas.draw_text(shown, style.font, {std::round(x), baseline}, style.color);
    canvas.restore();
}
void draw_bezel(AppKit& kit, Rect rect, double radius, Color face, Color border) {
    auto& canvas = kit.require_canvas("control bezel");
    drawing::Path outer;
    outer.add_rounded_rect(rect, radius, radius);
    canvas.fill_path(outer, border);
    drawing::Path inner;
    inner.add_rounded_rect(inset(rect, 1, 1), std::max(0.0, radius - 1), std::max(0.0, radius - 1));
    canvas.fill_path(inner, face);
}
Point event_location_in(AppKit& kit, Address target, Address event) {
    return kit.window_to_view(target, kit.event_state(event)->event.location);
}

bool track_mouse(AppKit& kit, Address target, Address down_event, const std::function<void(bool)>& highlight,
                 const std::function<void(Point)>& moved) {
    const auto bounds_of = [&] { return AppKit::view_bounds(*kit.view_state(target)); };
    auto location = event_location_in(kit, target, down_event);
    bool inside = rect_contains(bounds_of(), location);
    highlight(inside);
    const auto owner = kit.view_state(target)->window;
    if (owner) kit.display_window(owner);
    for (;;) {
        auto event = kit.next_event(left_mouse_up_mask | left_mouse_dragged_mask, monotonic_seconds() + 0.25,
                                    event_tracking_mode, true);
        if (!event) {
            // No event for a while: stop when the button is no longer down
            // (a release the window server did not report to this window).
            const auto server = window_server_or_null();
            if (!server || (server->pressed_mouse_buttons() & 1) == 0) break;
            continue;
        }
        const auto info = kit.event_state(event);
        if (info->window && info->window == owner) location = kit.window_to_view(target, info->event.location);
        const bool now_inside = rect_contains(bounds_of(), location);
        if (moved) moved(location);
        if (now_inside != inside) {
            inside = now_inside;
            highlight(inside);
            if (owner) kit.display_window(owner);
        }
        if (info->event.type == event_type::left_mouse_up) break;
    }
    highlight(false);
    if (owner) kit.display_window(owner);
    return inside;
}

// ---- installation ------------------------------------------------------------------------------
void AppKit::install_controls() {
    installed_kit = weak_from_this();
    cell = declare_class("NSCell", "NSObject");
    action_cell = declare_class("NSActionCell", "NSCell");
    button_cell = declare_class("NSButtonCell", "NSActionCell");
    text_field_cell = declare_class("NSTextFieldCell", "NSActionCell");
    secure_text_field_cell = declare_class("NSSecureTextFieldCell", "NSTextFieldCell");
    image_cell = declare_class("NSImageCell", "NSCell");
    popup_button_cell = declare_class("NSPopUpButtonCell", "NSButtonCell");
    control = declare_class("NSControl", "NSView");
    button = declare_class("NSButton", "NSControl");
    text_field = declare_class("NSTextField", "NSControl");
    secure_text_field = declare_class("NSSecureTextField", "NSTextField");
    image_view = declare_class("NSImageView", "NSControl");
    popup_button = declare_class("NSPopUpButton", "NSButton");
    progress_indicator = declare_class("NSProgressIndicator", "NSView");
    box = declare_class("NSBox", "NSView");
    menu_view = declare_class("_AlchemyMenuView", "NSView");
    string_constant("_NSControlTextDidChangeNotification", "NSControlTextDidChangeNotification");
    string_constant("_NSControlTextDidBeginEditingNotification", "NSControlTextDidBeginEditingNotification");
    string_constant("_NSControlTextDidEndEditingNotification", "NSControlTextDidEndEditingNotification");
    // The text attribute names (NSFontAttributeName, ...) are CoreText's
    // (darwin/coretext/font_exports.cpp).

    const auto cs = [this](Address value) { return cell_state(*this, value); };

    // ---- NSCell ----
    const auto c = cell;
    method(c, "init", [this](CpuState& s, GuestMemory&) {
        const auto empty = string("");
        try { s.x[0] = send_id(s.x[0], "initTextCell:", {empty}); } catch (...) { release(empty); throw; }
        release(empty);
    });
    method(c, "initTextCell:", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        state->kind = CellState::Kind::text;
        release(std::exchange(state->contents, s.x[2] ? cf.copy(s.x[2]) : 0));
    });
    method(c, "initImageCell:", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        state->kind = CellState::Kind::image;
        assign(state->image, s.x[2]);
    });
    method(c, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<CellState>(s.x[0]))
            for (auto* slot : {&state->contents, &state->title, &state->alternate_title, &state->image, &state->alternate_image,
                     &state->key_equivalent, &state->placeholder, &state->font, &state->background_color, &state->text_color,
                     &state->represented, &state->formatter, &state->menu, &state->attributed_title, &state->title_color,
                     &state->content_tint, &state->bezel_color, &state->sound})
                release(std::exchange(*slot, 0));
        if (const auto state = objc.find_host_state<CellState>(s.x[0]))
            for (const auto item : std::exchange(state->combo_items, {})) release(item);
        static_cast<void>(objc.send_super(s.x[0], cell, sel("dealloc")));
    }, "v@:");
    method(c, "copyWithZone:", [this, cs](CpuState& s, GuestMemory&) {
        const auto copy = objc.allocate(objc.object_class(s.x[0]));
        const auto from = cs(s.x[0]);
        auto to = std::make_shared<CellState>(*from);
        for (auto* slot : {&to->contents, &to->title, &to->alternate_title, &to->image, &to->alternate_image, &to->key_equivalent,
                 &to->placeholder, &to->font, &to->background_color, &to->text_color, &to->represented, &to->formatter, &to->menu,
                 &to->attributed_title, &to->title_color, &to->content_tint, &to->bezel_color, &to->sound})
            *slot = retain(*slot);
        for (auto& item : to->combo_items) item = retain(item);
        // Subclass state is the subclass's to copy (its -copyWithZone: after super's).
        to->extension.reset();
        static_cast<void>(objc.host_state<CellState>(copy, [&] { return to; }));
        s.x[0] = copy;
    });
    const auto changed = [this, cs](Address self) {
        const auto view = cs(self)->control_view;
        if (view && is_kind(view, this->view)) invalidate_view(view);
    };
    // Values.
    method(c, "objectValue", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->contents; });
    method(c, "setObjectValue:", [this, cs, changed](CpuState& s, GuestMemory&) {
        release(std::exchange(cs(s.x[0])->contents, s.x[2] ? cf.copy(s.x[2]) : 0));
        changed(s.x[0]);
    }, "v@:@");
    method(c, "stringValue", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_string(drawing::utf8(cell_text(*this, s.x[0]))); });
    method(c, "setStringValue:", [this, cs, changed](CpuState& s, GuestMemory&) {
        const auto value = s.x[2] ? (cf.kind(s.x[2]) == ObjectKind::string ? cf.copy(s.x[2]) : string(text(s.x[2]))) : string("");
        release(std::exchange(cs(s.x[0])->contents, value));
        changed(s.x[0]);
    }, "v@:@");
    method(c, "attributedStringValue", [this](CpuState& s, GuestMemory&) {
        const auto attributed = objc.find_class("NSAttributedString");
        const auto value = auto_string(drawing::utf8(cell_text(*this, s.x[0])));
        s.x[0] = autorelease(send_id(send(attributed, "alloc").x0, "initWithString:", {value}));
    });
    method(c, "setAttributedStringValue:", [this, cs, changed](CpuState& s, GuestMemory&) {
        const auto value = s.x[2] ? cf.copy(send(s.x[2], "string").x0) : string("");
        release(std::exchange(cs(s.x[0])->contents, value));
        changed(s.x[0]);
    }, "v@:@");
    const auto number = [this](Address self) {
        const auto contents = cell_state(*this, self)->contents;
        if (!contents) return 0.0;
        if (const auto value = cf.number_value(contents)) return *value;
        try { return std::stod(text(contents)); } catch (...) { return 0.0; }
    };
    for (const auto* name : {"intValue", "integerValue"})
        method(c, name, [number](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(number(s.x[0]))); }, "q@:");
    method(c, "doubleValue", [number](CpuState& s, GuestMemory&) { set_d(s, 0, number(s.x[0])); }, "d@:");
    method(c, "floatValue", [number](CpuState& s, GuestMemory&) { set_f(s, 0, static_cast<float>(number(s.x[0]))); }, "f@:");
    const auto set_number = [this, cs, changed](Address self, double value) {
        release(std::exchange(cs(self)->contents, cf.real(value)));
        changed(self);
    };
    method(c, "setIntValue:", [set_number](CpuState& s, GuestMemory&) { set_number(s.x[0], static_cast<std::int32_t>(s.x[2])); }, "v@:i");
    method(c, "setIntegerValue:", [set_number](CpuState& s, GuestMemory&) { set_number(s.x[0], static_cast<double>(static_cast<std::int64_t>(s.x[2]))); }, "v@:q");
    method(c, "setDoubleValue:", [set_number](CpuState& s, GuestMemory&) { set_number(s.x[0], d(s, 0)); }, "v@:d");
    method(c, "setFloatValue:", [set_number](CpuState& s, GuestMemory&) { set_number(s.x[0], f(s, 0)); }, "v@:f");
    // Plain properties.
    const auto flag = [this, cs, changed](const char* getter, const char* setter, bool CellState::*member) {
        method(cell, getter, [cs, member](CpuState& s, GuestMemory&) { return_bool(s, (*cs(s.x[0])).*member); }, "B@:");
        method(cell, setter, [cs, changed, member](CpuState& s, GuestMemory&) {
            (*cs(s.x[0])).*member = bool_arg(s, 2);
            changed(s.x[0]);
        }, "v@:B");
    };
    flag("isEnabled", "setEnabled:", &CellState::enabled);
    flag("isHighlighted", "setHighlighted:", &CellState::highlighted);
    flag("isBordered", "setBordered:", &CellState::bordered);
    flag("isBezeled", "setBezeled:", &CellState::bezeled);
    flag("isEditable", "setEditable:", &CellState::editable);
    flag("isSelectable", "setSelectable:", &CellState::selectable);
    flag("isScrollable", "setScrollable:", &CellState::scrollable);
    flag("wraps", "setWraps:", &CellState::wraps);
    flag("isContinuous", "setContinuous:", &CellState::continuous);
    flag("allowsMixedState", "setAllowsMixedState:", &CellState::allows_mixed);
    flag("isTransparent", "setTransparent:", &CellState::transparent);
    flag("drawsBackground", "setDrawsBackground:", &CellState::draws_background);
    flag("usesSingleLineMode", "setUsesSingleLineMode:", &CellState::single_line);
    flag("truncatesLastVisibleLine", "setTruncatesLastVisibleLine:", &CellState::truncates_last_line);
    flag("showsFirstResponder", "setShowsFirstResponder:", &CellState::shows_first_responder);
    flag("allowsUndo", "setAllowsUndo:", &CellState::allows_undo);
    flag("refusesFirstResponder", "setRefusesFirstResponder:", &CellState::refuses_first_responder);
    const auto integer = [this, cs, changed](const char* getter, const char* setter, std::int64_t CellState::*member) {
        method(cell, getter, [cs, member](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>((*cs(s.x[0])).*member); }, "q@:");
        method(cell, setter, [cs, changed, member](CpuState& s, GuestMemory&) {
            (*cs(s.x[0])).*member = static_cast<std::int64_t>(s.x[2]);
            changed(s.x[0]);
        }, "v@:q");
    };
    integer("tag", "setTag:", &CellState::tag);
    integer("baseWritingDirection", "setBaseWritingDirection:", &CellState::writing_direction);
    integer("userInterfaceLayoutDirection", "setUserInterfaceLayoutDirection:", &CellState::layout_direction);
    integer("backgroundStyle", "setBackgroundStyle:", &CellState::background_style);
    // The style the cell's interior draws with is the one it was given.
    method(c, "interiorBackgroundStyle", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->background_style); }, "q@:");
    // -compare: orders cells by their string values.
    method(c, "compare:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !is_kind(s.x[2], cell)) fail("-[NSCell compare:] with an object that is not a cell");
        s.x[0] = send_id(send(s.x[0], "stringValue").x0, "compare:", {send(s.x[2], "stringValue").x0});
    }, "q@:@");
    method(c, "alignment", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(alignment_to_guest(cs(s.x[0])->alignment)); }, "q@:");
    method(c, "setAlignment:", [cs, changed](CpuState& s, GuestMemory&) {
        cs(s.x[0])->alignment = alignment_from_guest(static_cast<std::int64_t>(s.x[2]));
        changed(s.x[0]);
    }, "v@:q");
    integer("lineBreakMode", "setLineBreakMode:", &CellState::line_break);
    integer("controlSize", "setControlSize:", &CellState::control_size);
    integer("focusRingType", "setFocusRingType:", &CellState::focus_ring_type);
    integer("imagePosition", "setImagePosition:", &CellState::image_position);
    integer("imageScaling", "setImageScaling:", &CellState::image_scaling);
    integer("bezelStyle", "setBezelStyle:", &CellState::bezel_style);
    method(c, "state", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->state); }, "q@:");
    method(c, "setState:", [cs, changed](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        auto value = static_cast<std::int64_t>(s.x[2]);
        if (value < 0 && !state->allows_mixed) value = state_on;
        if (value > 0) value = state_on;
        state->state = value;
        changed(s.x[0]);
    }, "v@:q");
    method(c, "nextState", [cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        std::int64_t next = state->state == state_on ? state_off : state_on;
        if (state->allows_mixed) next = state->state == state_off ? state_on : state->state == state_on ? state_mixed : state_off;
        s.x[0] = static_cast<Address>(next);
    }, "q@:");
    method(c, "setNextState", [this](CpuState& s, GuestMemory&) {
        const auto next = send(s.x[0], "nextState").x0;
        static_cast<void>(send_id(s.x[0], "setState:", {next}));
    }, "v@:");
    const auto object = [this, cs, changed](const char* getter, const char* setter, Address CellState::*member, bool copy) {
        if (getter) method(cell, getter, [cs, member](CpuState& s, GuestMemory&) { s.x[0] = (*cs(s.x[0])).*member; });
        method(cell, setter, [this, cs, changed, member, copy](CpuState& s, GuestMemory&) {
            auto& slot = (*cs(s.x[0])).*member;
            if (copy) release(std::exchange(slot, s.x[2] ? cf.copy(s.x[2]) : 0));
            else assign(slot, s.x[2]);
            changed(s.x[0]);
        }, "v@:@");
    };
    object(nullptr, "setFont:", &CellState::font, false);
    // A cell without a font of its own draws with (and answers) the system font.
    method(cell, "font", [this, cs](CpuState& s, GuestMemory&) {
        const auto value = cs(s.x[0])->font;
        s.x[0] = value ? value : font_object({});
    });
    object("image", "setImage:", &CellState::image, false);
    object("representedObject", "setRepresentedObject:", &CellState::represented, false);
    object("formatter", "setFormatter:", &CellState::formatter, false);
    object("placeholderString", "setPlaceholderString:", &CellState::placeholder, true);
    object("backgroundColor", "setBackgroundColor:", &CellState::background_color, false);
    object("textColor", "setTextColor:", &CellState::text_color, false);
    // The cell's contextual menu (NSPopUpButtonCell keeps its items in the same slot).
    object("menu", "setMenu:", &CellState::menu, false);
    method(c, "title", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        s.x[0] = state->title ? state->title : auto_string(drawing::utf8(cell_text(*this, s.x[0])));
    });
    method(c, "setTitle:", [this](CpuState& s, GuestMemory&) { static_cast<void>(send_id(s.x[0], "setStringValue:", {s.x[2]})); }, "v@:@");
    method(c, "controlView", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->control_view; });
    method(c, "setControlView:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->control_view = s.x[2]; }, "v@:@");
    method(c, "type", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->kind); }, "Q@:");
    method(c, "setType:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->kind = static_cast<CellState::Kind>(std::min<Address>(s.x[2], 2)); }, "v@:Q");
    method(c, "target", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->target; });
    method(c, "setTarget:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->target = s.x[2]; }, "v@:@");
    method(c, "action", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->action; }, ":@:");
    method(c, "setAction:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->action = s.x[2]; }, "v@::");
    method(c, "sendActionOn:", [cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        const auto old = state->send_action_mask;
        state->send_action_mask = s.x[2];
        s.x[0] = old;
    }, "q@:Q");
    method(c, "acceptsFirstResponder", [cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        return_bool(s, state->enabled && !state->refuses_first_responder && (state->editable || state->selectable));
    }, "B@:");
    method(c, "keyEquivalent", [this, cs](CpuState& s, GuestMemory&) {
        const auto key = cs(s.x[0])->key_equivalent;
        s.x[0] = key ? key : auto_string("");
    });
    method(c, "setKeyEquivalent:", [this, cs](CpuState& s, GuestMemory&) {
        release(std::exchange(cs(s.x[0])->key_equivalent, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(c, "keyEquivalentModifierMask", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->key_modifier_mask; }, "Q@:");
    method(c, "setKeyEquivalentModifierMask:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->key_modifier_mask = s.x[2]; }, "v@:Q");
    // Geometry.
    method(c, "cellSize", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, {{0, 0}, {10000, 10000}});
        return_size(s, result_size(send(s.x[0], "cellSizeForBounds:", registers)));
    }, "{CGSize=dd}@:");
    method(c, "cellSizeForBounds:", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        const auto bounds = rect_arg(s);
        const auto style = cell_text_style(*this, s.x[0], theme(false));
        const auto metrics = drawing::font_metrics(style.font);
        Size size{};
        if (state->kind == CellState::Kind::image && state->image) {
            size = result_size(send(state->image, "size"));
        } else {
            const auto measured = measure_string(cell_text(*this, s.x[0]), style, state->wraps ? bounds.size.width - 4 : 0);
            size = {measured.width + 4, std::max(measured.height, std::ceil(metrics.line_height()))};
            if (state->bezeled || state->bordered) size = {size.width + 6, size.height + 6};
        }
        return_size(s, {std::min(size.width, bounds.size.width), std::min(size.height, bounds.size.height)});
    }, "{CGSize=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    for (const auto* name : {"drawingRectForBounds:", "titleRectForBounds:", "imageRectForBounds:"})
        method(c, name, [cs](CpuState& s, GuestMemory&) {
            const auto state = cs(s.x[0]);
            const auto r = rect_arg(s);
            return_rect(s, state->bezeled || state->bordered ? inset(r, 3, 2) : inset(r, 2, 0));
        }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // Drawing.
    method(c, "drawWithFrame:inView:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        registers.x[2] = s.x[2];
        static_cast<void>(send(s.x[0], "drawInteriorWithFrame:inView:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(c, "drawInteriorWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        if (state->kind == CellState::Kind::image) {
            if (state->image) draw_image_in(state->image, frame, state->enabled ? 1 : 0.5, true);
            return;
        }
        if (state->kind != CellState::Kind::text) return;
        const auto& th = theme_for(s.x[2]);
        const auto style = cell_text_style(*this, self, th);
        if (state->wraps) draw_string(cell_text(*this, self), style, inset(frame, 2, 1), true);
        else draw_line_in(*this, cell_text(*this, self), style, inset(frame, 2, 0), style.alignment);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(c, "highlight:withFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->highlighted = bool_arg(s, 2);
        if (s.x[3]) invalidate_view(s.x[3]);
    }, "v@:B{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(c, "performClick:", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        if (!state->enabled) return;
        if (state->control_view) { static_cast<void>(send_id(state->control_view, "performClick:", {s.x[2]})); return; }
        static_cast<void>(send_action(state->action, state->target, s.x[0]));
    }, "v@:@");

    // ---- NSControl ----
    const auto k = control;
    const auto ctl = [this](Address value) { return extension<ControlState>(value); };
    static std::map<Address, Address> cell_classes;   // +setCellClass: overrides
    class_method(k, "cellClass", [this](CpuState& s, GuestMemory&) {
        for (auto current = s.x[0]; current; current = superclass_of(current))
            if (const auto found = cell_classes.find(current); found != cell_classes.end()) { s.x[0] = found->second; return; }
        s.x[0] = 0;
    });
    class_method(k, "setCellClass:", [](CpuState& s, GuestMemory&) { cell_classes[s.x[0]] = s.x[2]; }, "v@:#");
    cell_classes[button] = button_cell;
    cell_classes[text_field] = text_field_cell;
    cell_classes[secure_text_field] = secure_text_field_cell;
    cell_classes[image_view] = image_cell;
    cell_classes[popup_button] = popup_button_cell;
    method(k, "initWithFrame:", [this, ctl](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        const auto self = objc.send_super(s.x[0], control, sel("initWithFrame:"), registers).x0;
        s.x[0] = self;
        if (!self || ctl(self)->cell) return;
        const auto cls = send(objc.object_class(self), "cellClass").x0;
        if (!cls) return;
        const auto made = send(send(cls, "alloc").x0, "init").x0;
        ctl(self)->cell = made;
        static_cast<void>(send_id(made, "setControlView:", {self}));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(k, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = find_extension<ControlState>(s.x[0])) {
            if (state->cell && objc.find_host_state<CellState>(state->cell)) cell_state(*this, state->cell)->control_view = 0;
            release(std::exchange(state->cell, 0));
            release(std::exchange(state->box_title, 0));
            release(std::exchange(state->fill_color, 0));
            release(std::exchange(state->border_color, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], control, sel("dealloc")));
    }, "v@:");
    method(k, "cell", [ctl](CpuState& s, GuestMemory&) { s.x[0] = ctl(s.x[0])->cell; });
    method(k, "setCell:", [this, ctl](CpuState& s, GuestMemory&) {
        const auto state = ctl(s.x[0]);
        assign(state->cell, s.x[2]);
        if (s.x[2]) static_cast<void>(send_id(s.x[2], "setControlView:", {s.x[0]}));
        invalidate_view(s.x[0]);
    }, "v@:@");
    method(k, "selectedCell", [ctl](CpuState& s, GuestMemory&) { s.x[0] = ctl(s.x[0])->cell; });
    method(k, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    // Cell forwarding (AppKit's NSControl API is the cell's).
    const auto forward_cell = [this, ctl](std::string name, std::string types, bool redisplay) {
        method(control, name, [this, ctl, name, redisplay](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto target = ctl(self)->cell;
            if (!target) { s.x[0] = 0; s.x[1] = 0; return; }
            CallRegs registers;
            for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
            for (unsigned i = 0; i < 8; ++i) registers.v[i] = s.v[i];
            const auto result = send(target, name, registers);
            s.x[0] = result.x0; s.x[1] = result.x1;
            for (unsigned i = 0; i < 4; ++i) s.v[i] = result.v[i];
            if (redisplay) invalidate_view(self);
        }, std::move(types));
    };
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"objectValue", "@@:"}, {"stringValue", "@@:"}, {"attributedStringValue", "@@:"}, {"intValue", "i@:"},
             {"integerValue", "q@:"}, {"doubleValue", "d@:"}, {"floatValue", "f@:"}, {"isEnabled", "B@:"},
             {"alignment", "q@:"}, {"font", "@@:"}, {"lineBreakMode", "q@:"}, {"controlSize", "Q@:"},
             {"isContinuous", "B@:"}, {"usesSingleLineMode", "B@:"}, {"formatter", "@@:"},
             {"isHighlighted", "B@:"}, {"cellSize", "{CGSize=dd}@:"}})
        forward_cell(name, types, false);
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"setObjectValue:", "v@:@"}, {"setStringValue:", "v@:@"}, {"setAttributedStringValue:", "v@:@"},
             {"setIntValue:", "v@:i"}, {"setIntegerValue:", "v@:q"}, {"setDoubleValue:", "v@:d"}, {"setFloatValue:", "v@:f"},
             {"setEnabled:", "v@:B"}, {"setAlignment:", "v@:q"}, {"setFont:", "v@:@"}, {"setLineBreakMode:", "v@:q"},
             {"setControlSize:", "v@:Q"}, {"setContinuous:", "v@:B"}, {"setUsesSingleLineMode:", "v@:B"},
             {"setFormatter:", "v@:@"}, {"setHighlighted:", "v@:B"}})
        forward_cell(name, types, true);
    // The cell's writing direction; NSWritingDirectionNatural without a cell.
    method(k, "baseWritingDirection", [this, ctl](CpuState& s, GuestMemory&) {
        const auto c2 = ctl(s.x[0])->cell;
        s.x[0] = c2 ? send(c2, "baseWritingDirection").x0 : static_cast<Address>(-1);
    }, "q@:");
    method(k, "setBaseWritingDirection:", [this, ctl](CpuState& s, GuestMemory&) {
        if (const auto c2 = ctl(s.x[0])->cell) static_cast<void>(send_id(c2, "setBaseWritingDirection:", {s.x[2]}));
        invalidate_view(s.x[0]);
    }, "v@:q");
    method(k, "target", [ctl, cs](CpuState& s, GuestMemory&) {
        const auto state = ctl(s.x[0]);
        s.x[0] = state->cell ? cs(state->cell)->target : state->target;
    });
    method(k, "setTarget:", [ctl, cs](CpuState& s, GuestMemory&) {
        const auto state = ctl(s.x[0]);
        if (state->cell) cs(state->cell)->target = s.x[2]; else state->target = s.x[2];
    }, "v@:@");
    method(k, "action", [ctl, cs](CpuState& s, GuestMemory&) {
        const auto state = ctl(s.x[0]);
        s.x[0] = state->cell ? cs(state->cell)->action : state->action;
    }, ":@:");
    method(k, "setAction:", [ctl, cs](CpuState& s, GuestMemory&) {
        const auto state = ctl(s.x[0]);
        if (state->cell) cs(state->cell)->action = s.x[2]; else state->action = s.x[2];
    }, "v@::");
    method(k, "tag", [ctl, cs](CpuState& s, GuestMemory&) {
        const auto c2 = ctl(s.x[0])->cell;
        s.x[0] = c2 ? static_cast<Address>(cs(c2)->tag) : static_cast<Address>(-1);
    }, "q@:");
    method(k, "setTag:", [ctl, cs](CpuState& s, GuestMemory&) { if (const auto c2 = ctl(s.x[0])->cell) cs(c2)->tag = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(k, "sendActionOn:", [this, ctl](CpuState& s, GuestMemory&) {
        const auto c2 = ctl(s.x[0])->cell;
        s.x[0] = c2 ? send_id(c2, "sendActionOn:", {s.x[2]}) : 0;
    }, "q@:Q");
    method(k, "sendAction:to:", [this](CpuState& s, GuestMemory&) { return_bool(s, s.x[2] && send_action(s.x[2], s.x[3], s.x[0])); }, "B@::@");
    // The field editor asks its control before editing starts or ends; the
    // control asks its delegate (-control:textShould{Begin,End}Editing:),
    // and agrees when the delegate does not answer.
    for (const auto* name : {"Begin", "End"}) {
        const std::string phase = name;
        method(k, "textShould" + phase + "Editing:", [this, phase](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto delegate = responds(self, "delegate") ? send(self, "delegate").x0 : 0;
            const auto ask = "control:textShould" + phase + "Editing:";
            s.x[0] = delegate && responds(delegate, ask) ? send_id(delegate, ask, {self, s.x[2]}) & 0xff : 1;
        }, "B@:@");
    }
    // The tag of the selected cell (-1 without one).
    method(k, "selectedTag", [this](CpuState& s, GuestMemory&) {
        const auto selected = send(s.x[0], "selectedCell").x0;
        s.x[0] = selected ? send(selected, "tag").x0 : static_cast<Address>(-1);
    }, "q@:");
    method(k, "refusesFirstResponder", [ctl](CpuState& s, GuestMemory&) { return_bool(s, ctl(s.x[0])->refuses_first_responder); }, "B@:");
    method(k, "setRefusesFirstResponder:", [ctl](CpuState& s, GuestMemory&) { ctl(s.x[0])->refuses_first_responder = bool_arg(s, 2); }, "v@:B");
    method(k, "acceptsFirstResponder", [this, ctl](CpuState& s, GuestMemory&) {
        const auto state = ctl(s.x[0]);
        return_bool(s, !state->refuses_first_responder && state->cell && send_bool(state->cell, "acceptsFirstResponder"));
    }, "B@:");
    method(k, "ignoresMultiClick", [ctl](CpuState& s, GuestMemory&) { return_bool(s, ctl(s.x[0])->ignores_multi_click); }, "B@:");
    method(k, "setIgnoresMultiClick:", [ctl](CpuState& s, GuestMemory&) { ctl(s.x[0])->ignores_multi_click = bool_arg(s, 2); }, "v@:B");
    for (const auto* name : {"takeStringValueFrom:", "takeIntValueFrom:", "takeDoubleValueFrom:", "takeObjectValueFrom:"}) {
        const std::string selector = name;
        method(k, selector, [this, selector](CpuState& s, GuestMemory&) {
            const auto getter = selector == "takeStringValueFrom:" ? "stringValue" : selector == "takeObjectValueFrom:" ? "objectValue" : nullptr;
            if (getter) { static_cast<void>(send_id(s.x[0], selector == "takeStringValueFrom:" ? "setStringValue:" : "setObjectValue:", {send(s.x[2], getter).x0})); return; }
            CallRegs registers;
            registers.v[0] = send(s.x[2], "doubleValue").v[0];
            static_cast<void>(send(s.x[0], "setDoubleValue:", registers));
        }, "v@:@");
    }
    method(k, "drawRect:", [this, ctl](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = ctl(self)->cell;
        if (!target) return;
        resolve_dynamic_colors(*this, self);
        CallRegs registers;
        put_rect(registers, 0, view_bounds(*view_state(self)));
        registers.x[2] = self;
        static_cast<void>(send(target, "drawWithFrame:inView:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(k, "updateCell:", [this](CpuState& s, GuestMemory&) { invalidate_view(s.x[0]); }, "v@:@");
    method(k, "updateCellInside:", [this](CpuState& s, GuestMemory&) { invalidate_view(s.x[0]); }, "v@:@");
    method(k, "drawCell:", [this](CpuState& s, GuestMemory&) { invalidate_view(s.x[0]); }, "v@:@");
    method(k, "selectCell:", [](CpuState&, GuestMemory&) {}, "v@:@");
    method(k, "sizeToFit", [this](CpuState& s, GuestMemory&) {
        const auto size = result_size(send(s.x[0], "fittingSize"));
        CallRegs registers;
        put_size(registers, 0, size);
        static_cast<void>(send(s.x[0], "setFrameSize:", registers));
    }, "v@:");
    for (const auto* name : {"fittingSize", "intrinsicContentSize"})
        method(k, name, [this, ctl](CpuState& s, GuestMemory&) {
            const auto target = ctl(s.x[0])->cell;
            return_size(s, target ? result_size(send(target, "cellSize")) : Size{-1, -1});
        }, "{CGSize=dd}@:");
    method(k, "mouseDown:", [this, ctl, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto target = ctl(self)->cell;
        if (!target || !cs(target)->enabled) return;
        const bool clicked = track_mouse(*this, self, event, [&](bool inside) { cs(target)->highlighted = inside; invalidate_view(self); });
        if (!clicked) return;
        static_cast<void>(send(self, "_alchemyClicked"));
    }, "v@:@");
    // A completed click: the cell's next state and the action.
    method(k, "_alchemyClicked", [this, ctl, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = ctl(self)->cell;
        if (!target) return;
        static_cast<void>(send_id(self, "sendAction:to:", {cs(target)->action, cs(target)->target}));
    }, "v@:");
    method(k, "performClick:", [this, ctl, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = ctl(self)->cell;
        if (!target || !cs(target)->enabled) return;
        static_cast<void>(send(self, "_alchemyClicked"));
    }, "v@:@");
    method(k, "currentEditor", [ctl](CpuState& s, GuestMemory&) { s.x[0] = ctl(s.x[0])->editor; });
    method(k, "abortEditing", [this, ctl](CpuState& s, GuestMemory&) {
        const auto editor = ctl(s.x[0])->editor;
        if (!editor) { s.x[0] = 0; return; }
        static_cast<void>(send(s.x[0], "_alchemyEndEditing"));
        s.x[0] = 1;
    }, "B@:");
    method(k, "validateEditing", [this, ctl, cs](CpuState& s, GuestMemory&) {
        const auto editor = ctl(s.x[0])->editor;
        if (editor) {
            const auto value = send(editor, "string").x0;
            const auto target = ctl(s.x[0])->cell;
            if (target) release(std::exchange(cs(target)->contents, cf.copy(value)));
        }
    }, "v@:");
    method(k, "_alchemyEndEditing", [](CpuState&, GuestMemory&) {}, "v@:");

    install_buttons();
    install_text_fields();
    install_simple_views();
    install_popups();
    install_more_controls();
}

// ---- NSButtonCell / NSButton ------------------------------------------------------------------
void AppKit::install_buttons() {
    const auto cs = [this](Address value) { return cell_state(*this, value); };
    const auto b = button_cell;
    method(b, "initTextCell:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], button_cell, sel("initTextCell:"), [&] { CallRegs r; r.x[2] = s.x[2]; return r; }()).x0;
        s.x[0] = self;
        const auto state = cs(self);
        assign(state->title, state->contents);
        release(std::exchange(state->contents, 0));
        state->bordered = true;
        state->bezeled = true;
        state->alignment = 2;   // centred
        state->button_type = button_type::momentary_push_in;
        state->bezel_style = bezel_style::rounded;
    });
    method(b, "init", [this](CpuState& s, GuestMemory&) {
        const auto title = string("Button");
        try { s.x[0] = send_id(s.x[0], "initTextCell:", {title}); } catch (...) { release(title); throw; }
        release(title);
    });
    method(b, "initImageCell:", [this, cs](CpuState& s, GuestMemory&) {
        s.x[0] = send_id(s.x[0], "initTextCell:", {auto_string("")});
        assign(cs(s.x[0])->image, s.x[2]);
        cs(s.x[0])->image_position = 1;   // NSImageOnly
    });
    method(b, "title", [this, cs](CpuState& s, GuestMemory&) {
        const auto title = cs(s.x[0])->title;
        s.x[0] = title ? title : auto_string("");
    });
    method(b, "setTitle:", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        release(std::exchange(state->title, s.x[2] ? cf.copy(s.x[2]) : string("")));
        release(std::exchange(state->attributed_title, 0));
        release(std::exchange(state->title_color, 0));
        if (state->control_view) invalidate_view(state->control_view);
    }, "v@:@");
    method(b, "alternateTitle", [this, cs](CpuState& s, GuestMemory&) {
        const auto title = cs(s.x[0])->alternate_title;
        s.x[0] = title ? title : auto_string("");
    });
    method(b, "setAlternateTitle:", [this, cs](CpuState& s, GuestMemory&) {
        release(std::exchange(cs(s.x[0])->alternate_title, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(b, "alternateImage", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->alternate_image; });
    // Kept for continuous buttons' callers (buttons do not repeat while held).
    method(b, "getPeriodicDelay:interval:", [this, cs](CpuState& s, GuestMemory& m) {
        const auto state = cs(s.x[0]);
        if (s.x[2]) m.write32(s.x[2], std::bit_cast<std::uint32_t>(static_cast<float>(state->periodic_delay)));
        if (s.x[3]) m.write32(s.x[3], std::bit_cast<std::uint32_t>(static_cast<float>(state->periodic_interval)));
        static_cast<void>(this);
    }, "v@:^f^f");
    method(b, "setPeriodicDelay:interval:", [cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->periodic_delay = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        cs(s.x[0])->periodic_interval = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[1][0]));
    }, "v@:ff");
    method(b, "setAlternateImage:", [this, cs](CpuState& s, GuestMemory&) { assign(cs(s.x[0])->alternate_image, s.x[2]); }, "v@:@");
    method(b, "attributedTitle", [this, cs](CpuState& s, GuestMemory&) {
        if (const auto stored = cs(s.x[0])->attributed_title) { s.x[0] = stored; return; }
        const auto attributed = objc.find_class("NSAttributedString");
        s.x[0] = autorelease(send_id(send(attributed, "alloc").x0, "initWithString:", {send(s.x[0], "title").x0}));
    });
    // Stores the title itself (not through -setTitle:, which subclasses
    // override); the title draws in the string's foreground color at index 0.
    method(b, "setAttributedTitle:", [this, cs](CpuState& s, GuestMemory& m) {
        const auto state = cs(s.x[0]);
        const auto string_value = s.x[2] ? send(s.x[2], "string").x0 : 0;
        release(std::exchange(state->title, string_value ? cf.copy(string_value) : string("")));
        release(std::exchange(state->attributed_title, s.x[2] ? send(s.x[2], "copy").x0 : 0));
        Address color_value{};
        if (s.x[2] && send(s.x[2], "length").x0) {
            CallRegs registers;
            registers.x[2] = auto_string("NSColor");   // NSForegroundColorAttributeName
            registers.x[3] = 0;
            registers.x[4] = 0;
            color_value = send(s.x[2], "attribute:atIndex:effectiveRange:", registers).x0;
        }
        static_cast<void>(m);
        release(std::exchange(state->title_color, retain(color_value)));
        if (state->control_view) invalidate_view(state->control_view);
    }, "v@:@");
    method(b, "setButtonType:", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        state->button_type = static_cast<std::int64_t>(s.x[2]);
        // AppKit's NSCellStyleMask pair per type (contents 1, push-in 2, gray 4, background 8).
        switch (state->button_type) {
        case button_type::momentary_light: state->highlights_by = 4 | 8; state->shows_state_by = 0; break;
        case button_type::push_on_push_off: state->highlights_by = 2 | 4; state->shows_state_by = 4 | 8; break;
        case button_type::toggle: state->highlights_by = 2 | 1; state->shows_state_by = 1; break;
        case button_type::switch_: case button_type::radio: state->highlights_by = 1; state->shows_state_by = 1; break;
        case button_type::momentary_change: state->highlights_by = 1; state->shows_state_by = 0; break;
        case button_type::on_off: state->highlights_by = 4 | 8; state->shows_state_by = 4 | 8; break;
        default: state->highlights_by = 2 | 4; state->shows_state_by = 0; break;
        }
        if (state->button_type == button_type::switch_ || state->button_type == button_type::radio) {
            state->bordered = false;
            state->alignment = 4;   // natural
        }
        if (state->control_view) invalidate_view(state->control_view);
    }, "v@:Q");
    method(b, "_buttonType", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->button_type); }, "Q@:");
    method(b, "objectValue", [this, cs](CpuState& s, GuestMemory&) { s.x[0] = autorelease(cf.integer(cs(s.x[0])->state)); });
    method(b, "setObjectValue:", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[2] ? cf.number_value(s.x[2]).value_or(0) : 0;
        static_cast<void>(send_id(s.x[0], "setState:", {static_cast<Address>(static_cast<std::int64_t>(value))}));
    }, "v@:@");
    method(b, "intValue", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->state); }, "i@:");
    method(b, "integerValue", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->state); }, "q@:");
    method(b, "setIntValue:", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(s.x[0], "setState:", {static_cast<Address>(static_cast<std::int64_t>(static_cast<std::int32_t>(s.x[2])))}));
    }, "v@:i");
    method(b, "setIntegerValue:", [this](CpuState& s, GuestMemory&) { static_cast<void>(send_id(s.x[0], "setState:", {s.x[2]})); }, "v@:q");
    method(b, "stringValue", [this, cs](CpuState& s, GuestMemory&) { s.x[0] = auto_string(std::to_string(cs(s.x[0])->state)); });
    method(b, "acceptsFirstResponder", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");   // no full keyboard access
    method(b, "cellSizeForBounds:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = cs(self);
        const auto style = cell_text_style(*this, self, theme(false));
        const auto title = string16(send(self, "title").x0);
        const auto width = std::ceil(drawing::text_width(style.font, title));
        const auto metrics = drawing::font_metrics(style.font);
        Size size;
        Size picture{};
        if (state->image) picture = result_size(send(state->image, "size"));
        if (state->button_type == button_type::switch_ || state->button_type == button_type::radio) {
            size = {18 + (title.empty() ? 0 : width + 4), std::max(18.0, std::ceil(metrics.line_height()))};
        } else if (state->bordered && state->bezel_style == bezel_style::rounded) {
            const auto content = title.empty() ? picture.width : width + (state->image && state->image_position != 0 ? picture.width + 4 : 0);
            size = {std::max(content + 28, 32.0), 32};
        } else {
            const auto content = title.empty() ? picture.width : width + (state->image && state->image_position != 0 ? picture.width + 4 : 0);
            size = {content + (state->bordered ? 16 : 4), std::max({picture.height + (state->bordered ? 8 : 0), std::ceil(metrics.line_height()) + 6})};
        }
        return_size(s, size);
    }, "{CGSize=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(b, "drawWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(owner);
        auto& canvas = require_canvas("NSButtonCell drawing");
        auto style = cell_text_style(*this, self, th);
        const auto title = string16(send(self, state->state == state_on && state->alternate_title ? "alternateTitle" : "title").x0);
        const auto type = state->button_type;
        if (type == button_type::switch_ || type == button_type::radio) {
            const double side = state->control_size == 1 ? 12 : state->control_size == 2 ? 10 : 14;
            const Rect mark{{frame.origin.x + 2, frame.origin.y + std::floor((frame.size.height - side) / 2)}, {side, side}};
            const bool on = state->state != state_off;
            const auto face = on ? (state->enabled ? th.accent : th.disabled_text) : th.control_background;
            const auto border = on ? face : th.control_border;
            const double radius = type == button_type::radio ? side / 2 : 3;
            draw_bezel(*this, mark, radius, state->highlighted ? th.control_face_pressed : face, border);
            const bool flipped = flipped_context(*this);
            const auto at = [&](double x, double y) {   // unit coordinates of the mark, y up
                return Point{mark.origin.x + x * side, flipped ? mark.origin.y + (1 - y) * side : mark.origin.y + y * side};
            };
            if (on && type == button_type::radio) {
                drawing::Path dot;
                dot.add_oval({{mark.origin.x + side * 0.32, mark.origin.y + side * 0.32}, {side * 0.36, side * 0.36}});
                canvas.fill_path(dot, th.accent_text);
            } else if (state->state == state_on) {
                drawing::Path check;
                check.move_to(at(0.22, 0.52));
                check.line_to(at(0.42, 0.30));
                check.line_to(at(0.78, 0.74));
                canvas.stroke_path(check, th.accent_text, 1.8);
            } else if (state->state == state_mixed) {
                canvas.fill_rect({{mark.origin.x + side * 0.25, mark.origin.y + side / 2 - 1}, {side * 0.5, 2}}, th.accent_text);
            }
            if (!title.empty())
                draw_line_in(*this, title, style, {{mark.origin.x + side + 5, frame.origin.y}, {std::max(0.0, frame.size.width - side - 7), frame.size.height}}, 0);
            return;
        }
        // AppKit's drawing steps, each a message a subclass may override:
        // the bezel, the image, then the title.
        if (state->bordered && !state->transparent) {
            CallRegs bezel;
            put_rect(bezel, 0, frame);
            bezel.x[2] = owner;
            static_cast<void>(send(self, "drawBezelWithFrame:inView:", bezel));
        }
        // Image and title.
        Rect content = inset(frame, state->bordered ? 8 : 2, 0);
        if (state->image && state->image_position != 0 /* NSNoImage */) {
            const auto picture = result_size(send(state->image, "size"));
            const auto drawn = std::min(picture.height, frame.size.height - 4);
            const auto scale = picture.height > 0 ? drawn / picture.height : 1;
            const Size image_size{picture.width * scale, drawn};
            Rect image_rect{{content.origin.x, frame.origin.y + (frame.size.height - image_size.height) / 2}, image_size};
            const auto draw_picture = [&] {
                CallRegs registers;
                registers.x[2] = state->image;
                put_rect(registers, 0, image_rect);
                registers.x[3] = owner;
                static_cast<void>(send(self, "drawImage:withFrame:inView:", registers));
            };
            if (title.empty() || state->image_position == 1 /* NSImageOnly */) {
                image_rect.origin.x = frame.origin.x + (frame.size.width - image_size.width) / 2;
                draw_picture();
                return;
            }
            if (state->image_position == 3 /* NSImageRight */) {
                image_rect.origin.x = content.origin.x + content.size.width - image_size.width;
                content.size.width -= image_size.width + 4;
            } else {
                content.origin.x += image_size.width + 4;
                content.size.width -= image_size.width + 4;
            }
            draw_picture();
        }
        if (title.empty()) return;
        CallRegs registers;
        registers.x[2] = send(self, state->state == state_on && state->alternate_title ? "attributedAlternateTitle" : "attributedTitle").x0;
        put_rect(registers, 0, content);
        registers.x[3] = owner;
        static_cast<void>(send(self, "drawTitle:withFrame:inView:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(b, "attributedAlternateTitle", [this](CpuState& s, GuestMemory&) {
        const auto attributed = objc.find_class("NSAttributedString");
        s.x[0] = autorelease(send_id(send(attributed, "alloc").x0, "initWithString:", {send(s.x[0], "alternateTitle").x0}));
    });
    method(b, "drawImage:withFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        if (s.x[2]) draw_cell_image(*this, *state, s.x[2], rect_arg(s), state->enabled ? 1 : 0.5);
    }, "v@:@{CGRect={CGPoint=dd}{CGSize=dd}}@");
    // The title in the cell's style for the view's appearance: the string's
    // own foreground colour when it has one (semantic colours resolve for the
    // view, appearance.cpp), the accent text on a default button, the content
    // tint on a borderless one, AppKit's control / disabled control text
    // colours otherwise. Answers the rect drawn in.
    method(b, "drawTitle:withFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[3], title_object = s.x[2];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(owner);
        resolve_dynamic_colors(*this, owner);
        auto style = cell_text_style(*this, self, th);
        const auto key = state->key_equivalent ? text(state->key_equivalent) : std::string();
        const bool is_default = key == "\r" && state->enabled && state->bordered && !state->transparent &&
                                state->button_type != button_type::switch_ && state->button_type != button_type::radio;
        if (is_default && !state->title_color) style.color = th.accent_text;
        if (state->content_tint && !state->bordered && state->enabled) style.color = color_value(state->content_tint, style.color);
        if (title_object && !state->title_color && responds(title_object, "attribute:atIndex:effectiveRange:") &&
            send(title_object, "length").x0 > 0) {
            CallRegs at;
            at.x[2] = auto_string("NSColor");
            at.x[3] = 0;
            at.x[4] = 0;
            if (const auto own = send(title_object, "attribute:atIndex:effectiveRange:", at).x0)
                style.color = state->enabled ? color_value(own, style.color) : color_value(own, style.color).with_alpha(0.5);
        }
        const auto title = title_object ? string16(title_object) : std::u16string{};
        if (!title.empty()) draw_line_in(*this, title, style, frame, state->alignment == 4 ? 2 : state->alignment);
        return_rect(s, frame);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:@{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(b, "drawBezelWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(owner);
        // NSCellStyleMask: push-in, change gray and change background all
        // draw the pressed face (-highlightsBy while highlighted,
        // -showsStateBy while on).
        constexpr std::uint64_t pressed_look = 2 | 4 | 8;
        const bool pressed = (state->highlighted && (state->highlights_by & pressed_look)) ||
                             (state->state == state_on && (state->shows_state_by & pressed_look));
        const auto key = state->key_equivalent ? text(state->key_equivalent) : std::string();
        const bool is_default = key == "\r" && state->enabled;
        {
            Rect face_rect = frame;
            double radius = 5;
            if (state->bezel_style == bezel_style::rounded) {
                const double height = state->control_size == 1 ? 19 : state->control_size == 2 ? 16 : 21;
                face_rect = {{frame.origin.x + 6, frame.origin.y + std::floor((frame.size.height - height) / 2)},
                             {std::max(0.0, frame.size.width - 12), std::min(height, frame.size.height)}};
            } else if (state->bezel_style == bezel_style::circular) {
                const auto side = std::min(frame.size.width, frame.size.height) - 4;
                face_rect = {{frame.origin.x + (frame.size.width - side) / 2, frame.origin.y + (frame.size.height - side) / 2}, {side, side}};
                radius = side / 2;
            } else if (state->bezel_style == bezel_style::inline_ || state->bezel_style == bezel_style::recessed ||
                       state->bezel_style == bezel_style::round_rect) {
                radius = std::min(frame.size.height / 2, 9.0);
            } else {
                face_rect = inset(frame, 1, 1);
                radius = 3;
            }
            auto face = is_default ? th.accent : th.control_face;
            if (pressed) face = is_default ? Color{th.accent.r * 0.8, th.accent.g * 0.8, th.accent.b * 0.85, 1} : th.control_face_pressed;
            if (!state->enabled) face = face.with_alpha(0.5);
            draw_bezel(*this, face_rect, radius, face, is_default ? face : th.control_border);
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // NSButton (cell forwarding for the button API).
    const auto n = button;
    const auto forward = [this](std::string name, std::string types, bool redisplay) {
        method(button, name, [this, name, redisplay](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto target = control_cell(*this, self);
            if (!target) { s.x[0] = 0; return; }
            CallRegs registers;
            for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
            for (unsigned i = 0; i < 8; ++i) registers.v[i] = s.v[i];
            const auto result = send(target, name, registers);
            s.x[0] = result.x0; s.x[1] = result.x1;
            for (unsigned i = 0; i < 4; ++i) s.v[i] = result.v[i];
            if (redisplay) invalidate_view(self);
        }, std::move(types));
    };
    for (const auto& [name, types, redisplay] : std::initializer_list<std::tuple<const char*, const char*, bool>>{
             {"title", "@@:", false}, {"setTitle:", "v@:@", true}, {"alternateTitle", "@@:", false},
             {"setAlternateTitle:", "v@:@", true}, {"attributedTitle", "@@:", false}, {"setAttributedTitle:", "v@:@", true},
             {"image", "@@:", false}, {"setImage:", "v@:@", true}, {"alternateImage", "@@:", false},
             {"setAlternateImage:", "v@:@", true}, {"imagePosition", "Q@:", false}, {"setImagePosition:", "v@:Q", true},
             {"imageScaling", "Q@:", false}, {"setImageScaling:", "v@:Q", true}, {"state", "q@:", false},
             {"setState:", "v@:q", true}, {"allowsMixedState", "B@:", false}, {"setAllowsMixedState:", "v@:B", true},
             {"setNextState", "v@:", true}, {"isBordered", "B@:", false}, {"setBordered:", "v@:B", true},
             {"isTransparent", "B@:", false}, {"setTransparent:", "v@:B", true}, {"bezelStyle", "Q@:", false},
             {"setBezelStyle:", "v@:Q", true}, {"setButtonType:", "v@:Q", true}, {"keyEquivalent", "@@:", false},
             {"setKeyEquivalent:", "v@:@", true}, {"keyEquivalentModifierMask", "Q@:", false},
             {"setKeyEquivalentModifierMask:", "v@:Q", false}, {"showsBorderOnlyWhileMouseInside", "B@:", false}}) {
        forward(name, types, redisplay);
    }
    method(button_cell, "showsBorderOnlyWhileMouseInside", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(button_cell, "highlightsBy", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->highlights_by; }, "Q@:");
    method(button_cell, "setHighlightsBy:", [this, cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->highlights_by = s.x[2];
        if (const auto owner = cs(s.x[0])->control_view) invalidate_view(owner);
    }, "v@:Q");
    method(button_cell, "showsStateBy", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->shows_state_by; }, "Q@:");
    method(button_cell, "setShowsStateBy:", [this, cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->shows_state_by = s.x[2];
        if (const auto owner = cs(s.x[0])->control_view) invalidate_view(owner);
    }, "v@:Q");
    // -contentTintColor of buttons and image views (their cells keep it).
    for (const auto cls : {button_cell, image_cell}) {
        method(cls, "contentTintColor", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->content_tint; });
        method(cls, "setContentTintColor:", [this, cs](CpuState& s, GuestMemory&) {
            assign(cs(s.x[0])->content_tint, s.x[2]);
            if (const auto owner = cs(s.x[0])->control_view) invalidate_view(owner);
        }, "v@:@");
    }
    for (const auto cls : {button, image_view}) {
        method(cls, "contentTintColor", [this](CpuState& s, GuestMemory&) {
            const auto target = control_cell(*this, s.x[0]);
            s.x[0] = target ? cell_state(*this, target)->content_tint : 0;
        });
        method(cls, "setContentTintColor:", [this](CpuState& s, GuestMemory&) {
            if (const auto target = control_cell(*this, s.x[0])) static_cast<void>(send_id(target, "setContentTintColor:", {s.x[2]}));
        }, "v@:@");
    }
    // The bezel colour, the image hugging the title, and the click sound,
    // kept by the cell; the button forwards them.
    method(button_cell, "bezelColor", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->bezel_color; });
    method(button_cell, "setBezelColor:", [this, cs](CpuState& s, GuestMemory&) {
        assign(cs(s.x[0])->bezel_color, s.x[2]);
        if (const auto owner = cs(s.x[0])->control_view) invalidate_view(owner);
    }, "v@:@");
    method(button_cell, "imageHugsTitle", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->image_hugs_title); }, "B@:");
    method(button_cell, "setImageHugsTitle:", [this, cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->image_hugs_title = bool_arg(s, 2);
        if (const auto owner = cs(s.x[0])->control_view) invalidate_view(owner);
    }, "v@:B");
    method(button_cell, "sound", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->sound; });
    method(button_cell, "setSound:", [this, cs](CpuState& s, GuestMemory&) { assign(cs(s.x[0])->sound, s.x[2]); }, "v@:@");
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"bezelColor", "@@:"}, {"imageHugsTitle", "B@:"}, {"sound", "@@:"}}) {
        const std::string getter = name;
        method(n, getter, [this, getter](CpuState& s, GuestMemory&) {
            const auto target = control_cell(*this, s.x[0]);
            s.x[0] = target ? send(target, getter).x0 : 0;
        }, types);
    }
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"setBezelColor:", "v@:@"}, {"setImageHugsTitle:", "v@:B"}, {"setSound:", "v@:@"}}) {
        const std::string setter = name;
        method(n, setter, [this, setter](CpuState& s, GuestMemory&) {
            if (const auto target = control_cell(*this, s.x[0])) static_cast<void>(send_id(target, setter, {s.x[2]}));
        }, types);
    }
    method(button_cell, "setShowsBorderOnlyWhileMouseInside:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(n, "setShowsBorderOnlyWhileMouseInside:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(n, "highlight:", [this](CpuState& s, GuestMemory&) {
        if (const auto target = control_cell(*this, s.x[0])) {
            cell_state(*this, target)->highlighted = bool_arg(s, 2);
            invalidate_view(s.x[0]);
        }
    }, "v@:B");
    for (const auto* name : {"buttonWithTitle:target:action:", "checkboxWithTitle:target:action:", "radioButtonWithTitle:target:action:"}) {
        const std::string_view kind = name;
        class_method(n, name, [this, kind](CpuState& s, GuestMemory&) {
            const auto made = send(send(s.x[0], "alloc").x0, "initWithFrame:", [] { CallRegs r; put_rect(r, 0, {{0, 0}, {80, 32}}); return r; }()).x0;
            if (kind.starts_with("checkbox")) static_cast<void>(send_id(made, "setButtonType:", {static_cast<Address>(button_type::switch_)}));
            if (kind.starts_with("radio")) static_cast<void>(send_id(made, "setButtonType:", {static_cast<Address>(button_type::radio)}));
            static_cast<void>(send_id(made, "setTitle:", {s.x[2]}));
            static_cast<void>(send_id(made, "setTarget:", {s.x[3]}));
            static_cast<void>(send_id(made, "setAction:", {s.x[4]}));
            static_cast<void>(send(made, "sizeToFit"));
            s.x[0] = autorelease(made);
        });
    }
    // The image beside the title (leading); image-only buttons are controls/small.cpp's.
    class_method(n, "buttonWithTitle:image:target:action:", [this](CpuState& s, GuestMemory&) {
        const auto made = send(send(s.x[0], "alloc").x0, "initWithFrame:", [] { CallRegs r; put_rect(r, 0, {{0, 0}, {80, 32}}); return r; }()).x0;
        static_cast<void>(send_id(made, "setTitle:", {s.x[2]}));
        static_cast<void>(send_id(made, "setImage:", {s.x[3]}));
        static_cast<void>(send_id(made, "setImagePosition:", {7}));   // NSImageLeading
        static_cast<void>(send_id(made, "setTarget:", {s.x[4]}));
        static_cast<void>(send_id(made, "setAction:", {s.x[5]}));
        static_cast<void>(send(made, "sizeToFit"));
        s.x[0] = autorelease(made);
    });
    // A completed click toggles the state (by button type), turns sibling
    // radio buttons with the same action off, then sends the action.
    method(n, "_alchemyClicked", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = control_cell(*this, self);
        if (!target) return;
        const auto state = cell_state(*this, target);
        const auto type = state->button_type;
        if (type == button_type::switch_ || type == button_type::push_on_push_off || type == button_type::toggle ||
            type == button_type::on_off) {
            static_cast<void>(send(self, "setNextState"));
        } else if (type == button_type::radio) {
            static_cast<void>(send_id(self, "setState:", {static_cast<Address>(state_on)}));
            if (const auto parent = view_state(self)->superview)
                for (const auto sibling : view_state(parent)->subviews) {
                    if (sibling == self || !is_kind(sibling, button)) continue;
                    const auto other = control_cell(*this, sibling);
                    if (!other || cell_state(*this, other)->button_type != button_type::radio) continue;
                    if (cell_state(*this, other)->action != state->action) continue;
                    static_cast<void>(send_id(sibling, "setState:", {static_cast<Address>(state_off)}));
                }
        }
        if (state->sound) static_cast<void>(send_optional(state->sound, "play"));
        static_cast<void>(send_id(self, "sendAction:to:", {state->action, state->target}));
    }, "v@:");
    // Key equivalents (the default button's Return, Escape for Cancel).
    method(n, "performKeyEquivalent:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = control_cell(*this, self);
        s.x[0] = 0;
        if (!target || !s.x[2]) return;
        const auto state = cell_state(*this, target);
        if (!state->enabled || !state->key_equivalent || view_state(self)->hidden) return;
        const auto key = text(state->key_equivalent);
        const auto info = event_state(s.x[2]);
        if (info->event.type != event_type::key_down || key.empty()) return;
        auto pressed = info->event.characters_ignoring_modifiers;
        if (pressed == "\x1b" && key == "\x1b") {}
        else if (pressed != key) return;
        constexpr std::uint64_t relevant = modifier::shift | modifier::control | modifier::option | modifier::command;
        if ((info->event.modifier_flags & relevant) != (state->key_modifier_mask & relevant)) return;
        static_cast<void>(send_id(self, "performClick:", {0}));
        s.x[0] = 1;
    }, "B@:@");
    method(n, "performClick:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = control_cell(*this, self);
        if (!target || !cell_state(*this, target)->enabled) return;
        static_cast<void>(send(self, "_alchemyClicked"));
    }, "v@:@");
}

// ---- NSTextFieldCell / NSTextField / NSSecureTextField ----------------------------------------
void AppKit::install_text_fields() {
    const auto cs = [this](Address value) { return cell_state(*this, value); };
    const auto t = text_field_cell;
    method(t, "initTextCell:", [this, cs](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto self = objc.send_super(s.x[0], text_field_cell, sel("initTextCell:"), registers).x0;
        s.x[0] = self;
        const auto state = cs(self);
        state->editable = true;
        state->selectable = true;
        state->bezeled = true;
        state->draws_background = true;
        state->scrollable = true;
    });
    method(t, "acceptsFirstResponder", [cs](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        return_bool(s, state->enabled && !state->refuses_first_responder && (state->editable || state->selectable));
    }, "B@:");
    method(t, "bezelStyle", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->frame_style_bezel ? 1 : 0); }, "Q@:");
    method(t, "setBezelStyle:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->frame_style_bezel = s.x[2] == 1; }, "v@:Q");
    method(t, "drawWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(owner);
        auto& canvas = require_canvas("NSTextFieldCell drawing");
        if (state->bezeled || state->bordered) {
            const auto background = state->draws_background || state->bezeled
                                        ? color_value(state->background_color, th.text_background) : Color{0, 0, 0, 0};
            const double radius = state->frame_style_bezel ? std::min(frame.size.height / 2, 8.0) : 0;
            draw_bezel(*this, frame, radius, background, th.control_border);
        } else if (state->draws_background && state->background_color) {
            canvas.fill_rect(frame, color_value(state->background_color));
        }
        // The field editor draws the text while editing.
        if (owner && is_kind(owner, control) && extension<ControlState>(owner)->editor) return;
        CallRegs registers;
        put_rect(registers, 0, frame);
        registers.x[2] = owner;
        static_cast<void>(send(self, "drawInteriorWithFrame:inView:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(t, "drawInteriorWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(owner);
        auto style = cell_text_style(*this, self, th);
        auto value = cell_text(*this, self);
        if (state->secure) value.assign(value.size(), char16_t{0x2022});
        if (value.empty() && state->placeholder) {
            value = string16(state->placeholder);
            style.color = th.secondary_text;
        }
        const auto area = state->bezeled || state->bordered ? inset(frame, 4, 2) : inset(frame, 2, 0);
        if (state->wraps && !state->single_line) {
            const auto previous = style;
            draw_string(value, style, area, true);
            static_cast<void>(previous);
        } else {
            draw_line_in(*this, value, style, area, style.alignment, state->line_break >= 3 || !state->scrollable);
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(secure_text_field_cell, "initTextCell:", [this, cs](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        s.x[0] = objc.send_super(s.x[0], secure_text_field_cell, sel("initTextCell:"), registers).x0;
        cs(s.x[0])->secure = true;
    });
    method(secure_text_field_cell, "echosBullets", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(secure_text_field_cell, "setEchosBullets:", [](CpuState& s, GuestMemory&) {
        if (!bool_arg(s, 2)) unsupported("-[NSSecureTextFieldCell setEchosBullets:NO]");
    }, "v@:B");

    const auto f = text_field;
    const auto forward_to_cell = [this](std::string name, std::string types, bool redisplay) {
        method(text_field, name, [this, name, redisplay](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto target = control_cell(*this, self);
            if (!target) { s.x[0] = 0; return; }
            CallRegs registers;
            for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
            for (unsigned i = 0; i < 8; ++i) registers.v[i] = s.v[i];
            const auto result = send(target, name, registers);
            s.x[0] = result.x0; s.x[1] = result.x1;
            for (unsigned i = 0; i < 4; ++i) s.v[i] = result.v[i];   // floating-point results (-preferredMaxLayoutWidth)
            if (redisplay) invalidate_view(self);
        }, std::move(types));
    };
    for (const auto& [name, types, redisplay] : std::initializer_list<std::tuple<const char*, const char*, bool>>{
             {"isEditable", "B@:", false}, {"setEditable:", "v@:B", true}, {"isSelectable", "B@:", false},
             {"setSelectable:", "v@:B", true}, {"isBezeled", "B@:", false}, {"setBezeled:", "v@:B", true},
             {"isBordered", "B@:", false}, {"setBordered:", "v@:B", true}, {"drawsBackground", "B@:", false},
             {"setDrawsBackground:", "v@:B", true}, {"backgroundColor", "@@:", false}, {"setBackgroundColor:", "v@:@", true},
             {"textColor", "@@:", false}, {"setTextColor:", "v@:@", true}, {"placeholderString", "@@:", false},
             {"setPlaceholderString:", "v@:@", true}, {"bezelStyle", "Q@:", false}, {"setBezelStyle:", "v@:Q", true},
             {"preferredMaxLayoutWidth", "d@:", false}, {"setPreferredMaxLayoutWidth:", "v@:d", true}}) {
        forward_to_cell(name, types, redisplay);
    }
    method(text_field_cell, "preferredMaxLayoutWidth", [cs](CpuState& s, GuestMemory&) { set_d(s, 0, cs(s.x[0])->preferred_max_width); }, "d@:");
    method(text_field_cell, "sendsActionOnEndEditing", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->sends_action_on_end_editing); }, "B@:");
    method(text_field_cell, "setSendsActionOnEndEditing:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->sends_action_on_end_editing = bool_arg(s, 2); }, "v@:B");
    method(text_field_cell, "setPreferredMaxLayoutWidth:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->preferred_max_width = std::max(0.0, d(s, 0)); }, "v@:d");
    method(f, "placeholderAttributedString", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(f, "setPlaceholderAttributedString:", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(s.x[0], "setPlaceholderString:", {s.x[2] ? send(s.x[2], "string").x0 : 0}));
    }, "v@:@");
    method(f, "delegate", [this](CpuState& s, GuestMemory&) { s.x[0] = extension<ControlState>(s.x[0])->delegate; });
    method(f, "setDelegate:", [this](CpuState& s, GuestMemory&) { extension<ControlState>(s.x[0])->delegate = s.x[2]; }, "v@:@");
    method(f, "allowsEditingTextAttributes", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(f, "setAllowsEditingTextAttributes:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(f, "importsGraphics", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(f, "setImportsGraphics:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(f, "maximumNumberOfLines", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    method(f, "setMaximumNumberOfLines:", [](CpuState&, GuestMemory&) {}, "v@:q");
    method(f, "isAutomaticTextCompletionEnabled", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(f, "setAutomaticTextCompletionEnabled:", [](CpuState&, GuestMemory&) {}, "v@:B");
    // Factories.
    const auto factory = [this](Address cls, Address value, bool editable, bool wraps) {
        const auto made = send(send(cls, "alloc").x0, "initWithFrame:", [] { CallRegs r; put_rect(r, 0, {{0, 0}, {100, 22}}); return r; }()).x0;
        const auto target = control_cell(*this, made);
        const auto state = cell_state(*this, target);
        release(std::exchange(state->contents, value ? cf.copy(value) : string("")));
        if (!editable) {
            state->editable = state->selectable = state->bezeled = state->bordered = state->draws_background = false;
            state->scrollable = false;
            state->line_break = wraps ? 0 : 4;
        }
        state->wraps = wraps;
        static_cast<void>(send(made, "sizeToFit"));
        return autorelease(made);
    };
    class_method(f, "labelWithString:", [factory](CpuState& s, GuestMemory&) { s.x[0] = factory(s.x[0], s.x[2], false, false); });
    class_method(f, "wrappingLabelWithString:", [factory](CpuState& s, GuestMemory&) { s.x[0] = factory(s.x[0], s.x[2], false, true); });
    class_method(f, "textFieldWithString:", [factory](CpuState& s, GuestMemory&) { s.x[0] = factory(s.x[0], s.x[2], true, false); });
    class_method(f, "labelWithAttributedString:", [this, factory](CpuState& s, GuestMemory&) {
        s.x[0] = factory(s.x[0], s.x[2] ? send(s.x[2], "string").x0 : 0, false, false);
    });
    // Editing goes through the field editor (text.cpp).
    method(f, "acceptsFirstResponder", [this](CpuState& s, GuestMemory&) {
        const auto target = control_cell(*this, s.x[0]);
        const auto state = target ? cell_state(*this, target) : nullptr;
        return_bool(s, state && state->enabled && (state->editable || state->selectable) &&
                           !extension<ControlState>(s.x[0])->refuses_first_responder);
    }, "B@:");
    method(f, "becomeFirstResponder", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = control_cell(*this, self);
        if (!target || !cell_state(*this, target)->enabled) { s.x[0] = 0; return; }
        begin_field_editing(self);
        s.x[0] = 1;
    }, "B@:");
    method(f, "mouseDown:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto owner = view_state(self)->window;
        if (!owner) return;
        const auto editor = extension<ControlState>(self)->editor;
        if (editor) { static_cast<void>(send_id(editor, "mouseDown:", {s.x[2]})); return; }
        if (!send_bool(self, "acceptsFirstResponder")) { forward(self, "mouseDown:", s.x[2]); return; }
        static_cast<void>(send_id(owner, "makeFirstResponder:", {self}));
        if (const auto now = extension<ControlState>(self)->editor) static_cast<void>(send_id(now, "mouseDown:", {s.x[2]}));
    }, "v@:@");
    method(f, "_alchemyEndEditing", [this](CpuState& s, GuestMemory&) { end_field_editing(s.x[0], false); }, "v@:");
    method(f, "selectText:", [this](CpuState& s, GuestMemory&) {
        const auto owner = view_state(s.x[0])->window;
        if (!owner) return;
        if (!extension<ControlState>(s.x[0])->editor) static_cast<void>(send_id(owner, "makeFirstResponder:", {s.x[0]}));
        if (const auto editor = extension<ControlState>(s.x[0])->editor) static_cast<void>(send_id(editor, "selectAll:", {0}));
    }, "v@:@");
    method(f, "textDidBeginEditing:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        post("NSControlTextDidBeginEditingNotification", self, extension<ControlState>(self)->delegate, "controlTextDidBeginEditing:");
    }, "v@:@");
    method(f, "textDidChange:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        // The cell follows the editor (stringValue during editing).
        if (const auto editor = extension<ControlState>(self)->editor)
            if (const auto target = control_cell(*this, self))
                release(std::exchange(cell_state(*this, target)->contents, cf.copy(send(editor, "string").x0)));
        post("NSControlTextDidChangeNotification", self, extension<ControlState>(self)->delegate, "controlTextDidChange:");
    }, "v@:@");
    method(f, "textDidEndEditing:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        post("NSControlTextDidEndEditingNotification", self, extension<ControlState>(self)->delegate, "controlTextDidEndEditing:");
    }, "v@:@");
    // The delegate decides on editor commands first (control:textView:doCommandBySelector:).
    method(f, "textView:doCommandBySelector:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto delegate = extension<ControlState>(self)->delegate;
        if (delegate && responds(delegate, "control:textView:doCommandBySelector:")) {
            s.x[0] = send_bool(delegate, "control:textView:doCommandBySelector:", {self, s.x[2], s.x[3]});
            return;
        }
        s.x[0] = 0;
    }, "B@:@:");
}

// ---- NSImageView, NSProgressIndicator, NSBox ------------------------------------------------
void AppKit::install_simple_views() {
    const auto cs = [this](Address value) { return cell_state(*this, value); };
    // NSImageCell / NSImageView.
    method(image_cell, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initImageCell:", {0}); });
    method(image_cell, "imageAlignment", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->image_alignment; }, "Q@:");
    method(image_cell, "setImageAlignment:", [this, cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->image_alignment = s.x[2];
        if (const auto view = cs(s.x[0])->control_view) invalidate_view(view);
    }, "v@:Q");
    method(image_cell, "imageFrameStyle", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->image_frame ? 1 : 0; }, "Q@:");
    method(image_cell, "setImageFrameStyle:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->image_frame = s.x[2] != 0; }, "v@:Q");
    method(image_cell, "drawWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = cs(self);
        auto frame = rect_arg(s);
        const auto& th = theme_for(s.x[2]);
        if (state->image_frame) {
            draw_bezel(*this, frame, 0, th.control_background, th.control_border);
            frame = inset(frame, 4, 4);
        }
        if (!state->image) return;
        const auto picture = result_size(send(state->image, "size"));
        if (picture.width <= 0 || picture.height <= 0) return;
        Size size = picture;
        const auto scaling = state->image_scaling;   // 0 down, 1 axes independently, 2 none, 3 up or down
        const auto fit = std::min(frame.size.width / picture.width, frame.size.height / picture.height);
        if (scaling == 1) size = frame.size;
        else if (scaling == 3 || (scaling == 0 && fit < 1)) size = {picture.width * fit, picture.height * fit};
        // NSImageAlignment: center 0, top 1, top-left 2, top-right 3, left 4,
        // bottom 5, bottom-left 6, bottom-right 7, right 8 (the view is flipped
        // or not: top is the larger y unless the control view is flipped).
        double fx = 0.5, fy = 0.5;
        switch (state->image_alignment) {
        case 1: fy = 1; break;
        case 2: fx = 0; fy = 1; break;
        case 3: fx = 1; fy = 1; break;
        case 4: fx = 0; break;
        case 5: fy = 0; break;
        case 6: fx = 0; fy = 0; break;
        case 7: fx = 1; fy = 0; break;
        case 8: fx = 1; break;
        default: break;
        }
        if (s.x[2] && send_bool(s.x[2], "isFlipped")) fy = 1 - fy;
        const Rect target{{frame.origin.x + (frame.size.width - size.width) * fx, frame.origin.y + (frame.size.height - size.height) * fy}, size};
        draw_cell_image(*this, *state, state->image, target, state->enabled ? 1 : 0.5);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"image", "@@:"}, {"setImage:", "v@:@"}, {"imageScaling", "Q@:"}, {"setImageScaling:", "v@:Q"},
             {"imageAlignment", "Q@:"}, {"setImageAlignment:", "v@:Q"}, {"imageFrameStyle", "Q@:"},
             {"setImageFrameStyle:", "v@:Q"}, {"isEditable", "B@:"}, {"setEditable:", "v@:B"}}) {
        const std::string selector = name;
        method(image_view, selector, [this, selector](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto target = control_cell(*this, self);
            if (!target) { s.x[0] = 0; return; }
            CallRegs registers;
            for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
            s.x[0] = send(target, selector, registers).x0;
            if (selector.starts_with("set")) invalidate_view(self);
        }, types);
    }
    method(image_view, "initWithFrame:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = objc.send_super(s.x[0], image_view, sel("initWithFrame:"), registers).x0;
        if (const auto target = control_cell(*this, s.x[0])) cell_state(*this, target)->editable = false;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_method(image_view, "imageViewWithImage:", [this](CpuState& s, GuestMemory&) {
        const auto picture = s.x[2];
        const auto size = picture ? result_size(send(picture, "size")) : Size{};
        CallRegs registers;
        put_rect(registers, 0, {{0, 0}, size});
        const auto made = send(send(s.x[0], "alloc").x0, "initWithFrame:", registers).x0;
        static_cast<void>(send_id(made, "setImage:", {picture}));
        s.x[0] = autorelease(made);
    });
    method(image_view, "animates", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(image_view, "setAnimates:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(image_view, "allowsCutCopyPaste", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(image_view, "setAllowsCutCopyPaste:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(image_view, "mouseDown:", [this](CpuState& s, GuestMemory&) { forward(s.x[0], "mouseDown:", s.x[2]); }, "v@:@");

    // NSProgressIndicator.
    const auto p = progress_indicator;
    const auto pi = [this](Address value) { return extension<ControlState>(value); };
    method(p, "initWithFrame:", [this, pi](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = objc.send_super(s.x[0], progress_indicator, sel("initWithFrame:"), registers).x0;
        static_cast<void>(pi(s.x[0]));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto real = [this, pi](const char* getter, const char* setter, double ControlState::*member) {
        method(progress_indicator, getter, [pi, member](CpuState& s, GuestMemory&) { set_d(s, 0, (*pi(s.x[0])).*member); }, "d@:");
        method(progress_indicator, setter, [this, pi, member](CpuState& s, GuestMemory&) {
            (*pi(s.x[0])).*member = d(s, 0);
            invalidate_view(s.x[0]);
        }, "v@:d");
    };
    real("doubleValue", "setDoubleValue:", &ControlState::value);
    real("minValue", "setMinValue:", &ControlState::min_value);
    real("maxValue", "setMaxValue:", &ControlState::max_value);
    method(p, "incrementBy:", [this, pi](CpuState& s, GuestMemory&) {
        const auto state = pi(s.x[0]);
        state->value = std::min(state->max_value, state->value + d(s, 0));
        invalidate_view(s.x[0]);
    }, "v@:d");
    const auto boolean = [this, pi](const char* getter, const char* setter, bool ControlState::*member) {
        method(progress_indicator, getter, [pi, member](CpuState& s, GuestMemory&) { return_bool(s, (*pi(s.x[0])).*member); }, "B@:");
        method(progress_indicator, setter, [this, pi, member](CpuState& s, GuestMemory&) {
            (*pi(s.x[0])).*member = bool_arg(s, 2);
            invalidate_view(s.x[0]);
        }, "v@:B");
    };
    boolean("isIndeterminate", "setIndeterminate:", &ControlState::indeterminate);
    boolean("isDisplayedWhenStopped", "setDisplayedWhenStopped:", &ControlState::displayed_when_stopped);
    method(p, "style", [pi](CpuState& s, GuestMemory&) { s.x[0] = pi(s.x[0])->bar_style ? 0 : 1; }, "Q@:");
    method(p, "setStyle:", [this, pi](CpuState& s, GuestMemory&) { pi(s.x[0])->bar_style = s.x[2] == 0; invalidate_view(s.x[0]); }, "v@:Q");
    method(p, "controlSize", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:");
    method(p, "setControlSize:", [](CpuState&, GuestMemory&) {}, "v@:Q");
    method(p, "isBezeled", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(p, "setBezeled:", [](CpuState&, GuestMemory&) {}, "v@:B");
    method(p, "usesThreadedAnimation", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(p, "setUsesThreadedAnimation:", [](CpuState&, GuestMemory&) {}, "v@:B");
    // Animation advances on every display of an animating indicator; the
    // window redraws it from the event loop (no separate timer thread).
    method(p, "startAnimation:", [this, pi](CpuState& s, GuestMemory&) { pi(s.x[0])->animating = true; invalidate_view(s.x[0]); }, "v@:@");
    method(p, "stopAnimation:", [this, pi](CpuState& s, GuestMemory&) { pi(s.x[0])->animating = false; invalidate_view(s.x[0]); }, "v@:@");
    method(p, "animate:", [this, pi](CpuState& s, GuestMemory&) { pi(s.x[0])->phase += 0.08; invalidate_view(s.x[0]); }, "v@:@");
    method(p, "sizeToFit", [this, pi](CpuState& s, GuestMemory&) {
        const auto frame = view_state(s.x[0])->frame;
        CallRegs registers;
        put_size(registers, 0, pi(s.x[0])->bar_style ? Size{frame.size.width, 20} : Size{32, 32});
        static_cast<void>(send(s.x[0], "setFrameSize:", registers));
    }, "v@:");
    method(p, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(p, "drawRect:", [this, pi](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = pi(self);
        const auto bounds = view_bounds(*view_state(self));
        const auto& th = theme_for(self);
        auto& canvas = require_canvas("NSProgressIndicator drawing");
        if (!state->animating && state->indeterminate && !state->displayed_when_stopped) return;
        if (state->bar_style) {
            const double thickness = std::min(6.0, bounds.size.height);
            const Rect track{{bounds.origin.x, bounds.origin.y + (bounds.size.height - thickness) / 2}, {bounds.size.width, thickness}};
            drawing::Path background;
            background.add_rounded_rect(track, thickness / 2, thickness / 2);
            canvas.fill_path(background, th.separator);
            Rect bar = track;
            if (state->indeterminate) {
                const auto span = track.size.width * 0.3;
                const auto position = std::fmod(state->phase, 1.0) * (track.size.width + span) - span;
                bar = drawing::intersect({{track.origin.x + position, track.origin.y}, {span, thickness}}, track);
                if (state->animating) state->phase += 0.02;
            } else {
                const auto range = state->max_value - state->min_value;
                const auto fraction = range > 0 ? std::clamp((state->value - state->min_value) / range, 0.0, 1.0) : 0.0;
                bar.size.width = track.size.width * fraction;
            }
            if (bar.size.width > 0) {
                drawing::Path fill;
                fill.add_rounded_rect(bar, thickness / 2, thickness / 2);
                canvas.fill_path(fill, th.accent);
            }
            return;
        }
        // Spinning: twelve fading spokes.
        const Point center{bounds.origin.x + bounds.size.width / 2, bounds.origin.y + bounds.size.height / 2};
        const auto radius = std::min(bounds.size.width, bounds.size.height) / 2;
        const int lead = static_cast<int>(state->phase * 12) % 12;
        for (int i = 0; i < 12; ++i) {
            const auto angle = i * std::numbers::pi / 6;
            const Point a{center.x + std::cos(angle) * radius * 0.45, center.y + std::sin(angle) * radius * 0.45};
            const Point b{center.x + std::cos(angle) * radius * 0.9, center.y + std::sin(angle) * radius * 0.9};
            const auto fade = 0.25 + 0.75 * (((i - lead + 12) % 12) / 11.0);
            canvas.stroke_line(a, b, th.secondary_text.with_alpha(fade), std::max(1.5, radius * 0.16));
        }
        if (state->animating) state->phase += 1.0 / 12;
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // NSBox.
    const auto x = box;
    const auto bx = [this](Address value) { return extension<ControlState>(value); };
    const auto layout_box = [this, bx](Address self) {
        const auto state = bx(self);
        if (!state->content_view) return;
        const auto bounds = view_bounds(*view_state(self));
        const bool titled = state->title_position != 0 && state->box_title && !string16(state->box_title).empty();
        const auto margins = state->border_type == 0 ? Size{} : state->box_margins;
        Rect frame = state->box_type == 2 ? Rect{} : inset(bounds, margins.width, margins.height);
        if (titled) frame.size.height = std::max(0.0, frame.size.height - 16);
        CallRegs registers;
        put_rect(registers, 0, frame);
        static_cast<void>(send(state->content_view, "setFrame:", registers));
    };
    method(x, "initWithFrame:", [this, bx, layout_box](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        const auto self = objc.send_super(s.x[0], box, sel("initWithFrame:"), registers).x0;
        s.x[0] = self;
        const auto state = bx(self);
        state->box_title = string("Title");
        CallRegs content;
        put_rect(content, 0, {});
        const auto made = send(send(view, "alloc").x0, "initWithFrame:", content).x0;
        static_cast<void>(send_id(self, "addSubview:", {made}));
        release(made);
        state->content_view = made;
        layout_box(self);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(x, "contentView", [bx](CpuState& s, GuestMemory&) { s.x[0] = bx(s.x[0])->content_view; });
    method(x, "setContentView:", [this, bx, layout_box](CpuState& s, GuestMemory&) {
        const auto state = bx(s.x[0]);
        if (state->content_view == s.x[2]) return;
        if (state->content_view) static_cast<void>(send(state->content_view, "removeFromSuperview"));
        state->content_view = s.x[2];
        if (s.x[2]) static_cast<void>(send_id(s.x[0], "addSubview:", {s.x[2]}));
        layout_box(s.x[0]);
    }, "v@:@");
    method(x, "title", [this, bx](CpuState& s, GuestMemory&) { const auto t = bx(s.x[0])->box_title; s.x[0] = t ? t : auto_string(""); });
    method(x, "setTitle:", [this, bx, layout_box](CpuState& s, GuestMemory&) {
        release(std::exchange(bx(s.x[0])->box_title, s.x[2] ? cf.copy(s.x[2]) : 0));
        layout_box(s.x[0]);
        invalidate_view(s.x[0]);
    }, "v@:@");
    const auto box_integer = [this, bx, layout_box](const char* getter, const char* setter, std::int64_t ControlState::*member) {
        method(box, getter, [bx, member](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>((*bx(s.x[0])).*member); }, "Q@:");
        method(box, setter, [this, bx, member, layout_box](CpuState& s, GuestMemory&) {
            (*bx(s.x[0])).*member = static_cast<std::int64_t>(s.x[2]);
            layout_box(s.x[0]);
            invalidate_view(s.x[0]);
        }, "v@:Q");
    };
    box_integer("boxType", "setBoxType:", &ControlState::box_type);
    box_integer("borderType", "setBorderType:", &ControlState::border_type);
    box_integer("titlePosition", "setTitlePosition:", &ControlState::title_position);
    method(x, "fillColor", [bx](CpuState& s, GuestMemory&) { s.x[0] = bx(s.x[0])->fill_color; });
    method(x, "setFillColor:", [this, bx](CpuState& s, GuestMemory&) { assign(bx(s.x[0])->fill_color, s.x[2]); invalidate_view(s.x[0]); }, "v@:@");
    method(x, "borderColor", [bx](CpuState& s, GuestMemory&) { s.x[0] = bx(s.x[0])->border_color; });
    method(x, "setBorderColor:", [this, bx](CpuState& s, GuestMemory&) { assign(bx(s.x[0])->border_color, s.x[2]); invalidate_view(s.x[0]); }, "v@:@");
    method(x, "cornerRadius", [bx](CpuState& s, GuestMemory&) { set_d(s, 0, bx(s.x[0])->corner_radius); }, "d@:");
    method(x, "borderWidth", [bx](CpuState& s, GuestMemory&) { set_d(s, 0, bx(s.x[0])->border_width); }, "d@:");
    method(x, "setBorderWidth:", [this, bx](CpuState& s, GuestMemory&) { bx(s.x[0])->border_width = std::max(0.0, d(s, 0)); invalidate_view(s.x[0]); }, "v@:d");
    method(x, "setCornerRadius:", [this, bx](CpuState& s, GuestMemory&) { bx(s.x[0])->corner_radius = d(s, 0); invalidate_view(s.x[0]); }, "v@:d");
    // A transparent box draws nothing (its content view still draws).
    method(x, "isTransparent", [bx](CpuState& s, GuestMemory&) { return_bool(s, bx(s.x[0])->box_transparent); }, "B@:");
    method(x, "setTransparent:", [this, bx](CpuState& s, GuestMemory&) {
        bx(s.x[0])->box_transparent = bool_arg(s, 2);
        invalidate_view(s.x[0]);
    }, "v@:B");
    method(x, "setContentViewMargins:", [bx, layout_box](CpuState& s, GuestMemory&) {
        bx(s.x[0])->box_margins = size_arg(s);
        layout_box(s.x[0]);
    }, "v@:{CGSize=dd}");
    method(x, "contentViewMargins", [bx](CpuState& s, GuestMemory&) { return_size(s, bx(s.x[0])->box_margins); }, "{CGSize=dd}@:");
    method(x, "setFrameFromContentFrame:", [this, bx](CpuState& s, GuestMemory&) {
        const auto margins = bx(s.x[0])->box_margins;
        CallRegs registers;
        put_rect(registers, 0, inset(rect_arg(s), -margins.width, -margins.height));
        static_cast<void>(send(s.x[0], "setFrame:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(x, "sizeToFit", [](CpuState&, GuestMemory&) {}, "v@:");
    method(x, "resizeSubviewsWithOldSize:", [layout_box](CpuState& s, GuestMemory&) { layout_box(s.x[0]); }, "v@:{CGSize=dd}");
    method(x, "drawRect:", [this, bx](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = bx(self);
        if (state->box_transparent) return;
        const auto bounds = view_bounds(*view_state(self));
        const auto& th = theme_for(self);
        auto& canvas = require_canvas("NSBox drawing");
        if (state->box_type == 2 /* separator */) {
            if (bounds.size.width >= bounds.size.height)
                canvas.fill_rect({{bounds.origin.x, bounds.origin.y + std::floor(bounds.size.height / 2)}, {bounds.size.width, 1}}, th.separator);
            else
                canvas.fill_rect({{bounds.origin.x + std::floor(bounds.size.width / 2), bounds.origin.y}, {1, bounds.size.height}}, th.separator);
            return;
        }
        const bool titled = state->title_position != 0 && state->box_title && !string16(state->box_title).empty();
        auto frame = bounds;
        const bool flipped = send_bool(self, "isFlipped");
        if (titled) {
            frame.size.height -= 16;
            if (flipped) frame.origin.y += 16;
            TextStyle style;
            style.font.size = 11;
            style.color = th.text;
            const Rect title_rect{{bounds.origin.x + 4, flipped ? bounds.origin.y : bounds.origin.y + bounds.size.height - 16}, {bounds.size.width - 8, 16}};
            draw_line_in(*this, string16(state->box_title), style, title_rect, 0);
        }
        if (state->border_type == 0 && state->box_type != 4) return;
        const auto fill = state->box_type == 4 ? color_value(state->fill_color, Color{0, 0, 0, 0})
                                               : (th.dark ? Color::gray(1, 0.05) : Color::gray(0, 0.05));
        const auto border = state->box_type == 4 ? color_value(state->border_color, th.separator) : th.separator;
        const auto radius = state->box_type == 4 ? state->corner_radius : 5;
        if (state->box_type == 4 && state->border_type == 0) {
            drawing::Path path;
            path.add_rounded_rect(frame, radius, radius);
            canvas.fill_path(path, fill);
            return;
        }
        if (state->box_type == 4) {
            // A custom box: its fill, then its border at the border width.
            drawing::Path path;
            path.add_rounded_rect(frame, radius, radius);
            canvas.fill_path(path, fill);
            if (const auto width = state->border_width; width > 0) {
                const auto half = width / 2;
                const Rect edge{{frame.origin.x + half, frame.origin.y + half},
                                {std::max(0.0, frame.size.width - width), std::max(0.0, frame.size.height - width)}};
                drawing::Path outline;
                outline.add_rounded_rect(edge, std::max(0.0, radius - half), std::max(0.0, radius - half));
                canvas.stroke_path(outline, border, width);
            }
            return;
        }
        draw_bezel(*this, frame, radius, fill, border);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // ---- key equivalents through the view hierarchy (the default button) ----
    method(view, "performKeyEquivalent:", [this](CpuState& s, GuestMemory&) {
        const auto children = view_state(s.x[0])->subviews;
        for (const auto child : children)
            if (send_bool(child, "performKeyEquivalent:", {s.x[2]})) { s.x[0] = 1; return; }
        s.x[0] = 0;
    }, "B@:@");
    method(window, "performKeyEquivalent:", [this](CpuState& s, GuestMemory&) {
        const auto content = window_state(s.x[0])->content_view;
        return_bool(s, content && send_bool(content, "performKeyEquivalent:", {s.x[2]}));
    }, "B@:@");
    // An unhandled key press reaching the window: key equivalents of its
    // views (Return for the default button, Escape for Cancel), then
    // NSResponder's default.
    method(window, "keyDown:", [this](CpuState& s, GuestMemory&) {
        if (send_bool(s.x[0], "performKeyEquivalent:", {s.x[2]})) return;
        forward(s.x[0], "keyDown:", s.x[2]);
    }, "v@:@");
    method(window, "defaultButtonCell", [this](CpuState& s, GuestMemory&) {
        const auto chosen = window_state(s.x[0])->default_button;
        s.x[0] = chosen ? control_cell(*this, chosen) : 0;
    });
    method(window, "setDefaultButtonCell:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (state->default_button) if (const auto old = control_cell(*this, state->default_button))
            static_cast<void>(send_id(old, "setKeyEquivalent:", {auto_string("")}));
        state->default_button = s.x[2] ? cell_state(*this, s.x[2])->control_view : 0;
        if (s.x[2]) static_cast<void>(send_id(s.x[2], "setKeyEquivalent:", {auto_string("\r")}));
    }, "v@:@");
    method(window, "fieldEditor:forObject:", [this](CpuState& s, GuestMemory&) {
        s.x[0] = bool_arg(s, 2) ? field_editor_for(s.x[0]) : window_state(s.x[0])->field_editor;
    }, "@@:B@");
    method(window, "endEditingFor:", [this](CpuState& s, GuestMemory&) {
        const auto state = window_state(s.x[0]);
        if (!state->field_editor) return;
        const auto client = send(state->field_editor, "delegate").x0;
        if (client && (!s.x[2] || client == s.x[2]) && is_kind(client, text_field)) end_field_editing(client, false);
    }, "v@:@");
}

// ---- NSPopUpButtonCell / NSPopUpButton and popup menus -----------------------------------------
// The rows of a popup menu window: item index per y range (flipped view coordinates).
namespace {
struct MenuRow { std::int64_t index; double top, bottom; bool enabled; };
std::vector<MenuRow> menu_rows(AppKit& kit, Address target_menu) {
    std::vector<MenuRow> rows;
    double y = 5;
    const auto list = kit.objc.host_state<MenuState>(target_menu)->items;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto entry = kit.objc.host_state<MenuItemState>(list[i]);
        if (entry->hidden) continue;
        if (entry->separator) { y += 11; continue; }
        rows.push_back({static_cast<std::int64_t>(i), y, y + 22, entry->enabled});
        y += 22;
    }
    return rows;
}
} // namespace

void AppKit::install_popups() {
    const auto cs = [this](Address value) { return cell_state(*this, value); };
    const auto pc = popup_button_cell;
    const auto menu_of = [this, cs](Address self) {
        const auto state = cs(self);
        if (!state->menu) state->menu = alloc_init(menu);
        return state->menu;
    };
    const auto items_of = [this, menu_of](Address self) { return objc.host_state<MenuState>(menu_of(self))->items; };
    method(pc, "initTextCell:pullsDown:", [this, cs](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto self = objc.send_super(s.x[0], popup_button_cell, sel("initTextCell:"), registers).x0;
        s.x[0] = self;
        cs(self)->pulls_down = bool_arg(s, 3);
        cs(self)->alignment = 0;
    });
    method(pc, "initTextCell:", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initTextCell:pullsDown:", {s.x[2], 0}); });
    method(pc, "menu", [menu_of](CpuState& s, GuestMemory&) { s.x[0] = menu_of(s.x[0]); });
    method(pc, "setMenu:", [this, cs](CpuState& s, GuestMemory&) {
        assign(cs(s.x[0])->menu, s.x[2]);
        cs(s.x[0])->selected_item = s.x[2] && !objc.host_state<MenuState>(s.x[2])->items.empty() ? 0 : -1;
    }, "v@:@");
    // The menu popped up over `frame` of `view` (a click's tracking without
    // the click): the chosen item is selected and its action, then the
    // cell's, is sent.
    method(pc, "performClickWithFrame:inView:", [this, cs, menu_of, items_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto state = cs(self);
        if (!state->enabled || !owner) return;
        const auto menu_object = menu_of(self);
        if (!menu_object) return;
        state->highlighted = true;
        invalidate_view(owner);
        std::int64_t chosen{};
        try { chosen = popup_menu_tracking(menu_object, owner, rect_arg(s), state->pulls_down ? -1 : state->selected_item); }
        catch (...) { state->highlighted = false; throw; }
        state->highlighted = false;
        invalidate_view(owner);
        if (chosen < 0) return;
        if (!state->pulls_down) static_cast<void>(send_id(self, "selectItemAtIndex:", {static_cast<Address>(chosen)}));
        const auto list = items_of(self);
        if (chosen < static_cast<std::int64_t>(list.size())) {
            const auto entry = objc.host_state<MenuItemState>(list[static_cast<std::size_t>(chosen)]);
            if (entry->action) static_cast<void>(send_action(entry->action, entry->target, list[static_cast<std::size_t>(chosen)]));
        }
        const auto sender = state->control_view ? state->control_view : self;
        static_cast<void>(send_action(state->action, state->target, sender));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(pc, "itemArray", [this, items_of](CpuState& s, GuestMemory&) { s.x[0] = auto_array(items_of(s.x[0])); });
    method(pc, "numberOfItems", [items_of](CpuState& s, GuestMemory&) { s.x[0] = items_of(s.x[0]).size(); }, "q@:");
    method(pc, "preferredEdge", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->preferred_edge; }, "Q@:");
    method(pc, "setPreferredEdge:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->preferred_edge = s.x[2]; }, "v@:Q");
    // Item enabling and the state column are the menu's.
    method(pc, "autoenablesItems", [this, menu_of](CpuState& s, GuestMemory&) { s.x[0] = send(menu_of(s.x[0]), "autoenablesItems").x0; }, "B@:");
    method(pc, "setAutoenablesItems:", [this, menu_of](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(menu_of(s.x[0]), "setAutoenablesItems:", {s.x[2] & 0xff}));
    }, "v@:B");
    method(pc, "pullsDown", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->pulls_down); }, "B@:");
    method(pc, "setPullsDown:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->pulls_down = bool_arg(s, 2); }, "v@:B");
    method(pc, "indexOfSelectedItem", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->selected_item); }, "q@:");
    method(pc, "selectedItem", [cs, items_of](CpuState& s, GuestMemory&) {
        const auto index = cs(s.x[0])->selected_item;
        const auto list = items_of(s.x[0]);
        s.x[0] = index >= 0 && index < static_cast<std::int64_t>(list.size()) ? list[static_cast<std::size_t>(index)] : 0;
    });
    // With -altersStateOfSelectedItem (the default) a pop-up (not a
    // pull-down) puts the selected item in the on state and the previous one off.
    method(pc, "selectItemAtIndex:", [this, cs, items_of](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        const auto list = items_of(s.x[0]);
        const auto index = static_cast<std::int64_t>(s.x[2]);
        const auto count = static_cast<std::int64_t>(list.size());
        const auto previous = state->selected_item;
        state->selected_item = index >= 0 && index < count ? index : -1;
        if (state->alters_state && !state->pulls_down && previous != state->selected_item) {
            if (previous >= 0 && previous < count) static_cast<void>(send_id(list[static_cast<std::size_t>(previous)], "setState:", {static_cast<Address>(state_off)}));
            if (state->selected_item >= 0) static_cast<void>(send_id(list[static_cast<std::size_t>(state->selected_item)], "setState:", {static_cast<Address>(state_on)}));
        }
    }, "v@:q");
    method(pc, "altersStateOfSelectedItem", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->alters_state); }, "B@:");
    method(pc, "setAltersStateOfSelectedItem:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->alters_state = bool_arg(s, 2); }, "v@:B");
    method(pc, "usesItemFromMenu", [cs](CpuState& s, GuestMemory&) { return_bool(s, cs(s.x[0])->uses_item_from_menu); }, "B@:");
    method(pc, "setUsesItemFromMenu:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->uses_item_from_menu = bool_arg(s, 2); }, "v@:B");
    // NSPopUpArrowPosition (no arrow 0, center 1, bottom 2): kept; the arrow is always drawn.
    method(pc, "arrowPosition", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->arrow_position; }, "Q@:");
    method(pc, "setArrowPosition:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->arrow_position = s.x[2]; }, "v@:Q");
    method(pc, "title", [this, cs, items_of](CpuState& s, GuestMemory&) {
        const auto state = cs(s.x[0]);
        const auto list = items_of(s.x[0]);
        if (state->pulls_down && !state->uses_item_from_menu && state->title) { s.x[0] = state->title; return; }
        const auto index = state->pulls_down ? 0 : state->selected_item;
        if (index >= 0 && index < static_cast<std::int64_t>(list.size())) { s.x[0] = send(list[static_cast<std::size_t>(index)], "title").x0; return; }
        s.x[0] = auto_string("");
    });
    method(pc, "cellSizeForBounds:", [this, items_of, cs](CpuState& s, GuestMemory&) {
        const auto style = cell_text_style(*this, s.x[0], theme(false));
        double widest{};
        for (const auto item : items_of(s.x[0])) widest = std::max(widest, drawing::text_width(style.font, string16(send(item, "title").x0)));
        static_cast<void>(cs);
        return_size(s, {std::ceil(widest) + 40, 25});
    }, "{CGSize=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(pc, "drawWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(owner);
        auto& canvas = require_canvas("NSPopUpButtonCell drawing");
        const double height = std::min(frame.size.height, 21.0);
        const Rect face{{frame.origin.x + 3, frame.origin.y + std::floor((frame.size.height - height) / 2)}, {std::max(0.0, frame.size.width - 6), height}};
        if (state->bordered) draw_bezel(*this, face, 5, state->highlighted ? th.control_face_pressed : th.control_face, th.control_border);
        const auto style = cell_text_style(*this, self, th);
        draw_line_in(*this, string16(send(self, "title").x0), style, {{face.origin.x + 9, face.origin.y}, {std::max(0.0, face.size.width - 30), face.size.height}}, 0);
        // The chevron (up/down arrows; a single down arrow for pull-downs).
        const bool flipped = flipped_context(*this);
        const double cx = face.origin.x + face.size.width - 12, cy = face.origin.y + face.size.height / 2;
        const auto at = [&](double dx, double dy) { return Point{cx + dx, flipped ? cy - dy : cy + dy}; };
        drawing::Path arrows;
        if (state->pulls_down) {
            arrows.move_to(at(-3.5, 1.5)); arrows.line_to(at(0, -2)); arrows.line_to(at(3.5, 1.5));
        } else {
            arrows.move_to(at(-3, 2)); arrows.line_to(at(0, 5)); arrows.line_to(at(3, 2));
            arrows.move_to(at(-3, -2)); arrows.line_to(at(0, -5)); arrows.line_to(at(3, -2));
        }
        canvas.stroke_path(arrows, style.color, 1.5);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    const auto p = popup_button;
    method(p, "initWithFrame:pullsDown:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        const auto self = send(s.x[0], "initWithFrame:", registers).x0;
        s.x[0] = self;
        if (const auto target = control_cell(*this, self)) cell_state(*this, target)->pulls_down = bool_arg(s, 2);
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}B");
    const auto pcell = [this](Address self) {
        const auto target = control_cell(*this, self);
        if (!target) fail("NSPopUpButton without a cell");
        return target;
    };
    for (const auto* name : {"menu", "pullsDown", "indexOfSelectedItem", "selectedItem"})
        method(p, name, [this, pcell, name](CpuState& s, GuestMemory&) { s.x[0] = send(pcell(s.x[0]), name).x0; });
    method(p, "setMenu:", [this, pcell](CpuState& s, GuestMemory&) { static_cast<void>(send_id(pcell(s.x[0]), "setMenu:", {s.x[2]})); invalidate_view(s.x[0]); }, "v@:@");
    method(p, "setPullsDown:", [this, pcell](CpuState& s, GuestMemory&) { static_cast<void>(send_id(pcell(s.x[0]), "setPullsDown:", {s.x[2]})); invalidate_view(s.x[0]); }, "v@:B");
    method(p, "selectItemAtIndex:", [this, pcell](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(pcell(s.x[0]), "selectItemAtIndex:", {s.x[2]}));
        invalidate_view(s.x[0]);
    }, "v@:q");
    const auto popup_menu = [this, pcell](Address self) { return send(pcell(self), "menu").x0; };
    const auto popup_items = [this, popup_menu](Address self) { return objc.host_state<MenuState>(popup_menu(self))->items; };
    method(p, "numberOfItems", [popup_items](CpuState& s, GuestMemory&) { s.x[0] = popup_items(s.x[0]).size(); }, "q@:");
    method(p, "itemArray", [this, popup_items](CpuState& s, GuestMemory&) { s.x[0] = auto_array(popup_items(s.x[0])); });
    method(p, "itemAtIndex:", [popup_items](CpuState& s, GuestMemory&) {
        const auto list = popup_items(s.x[0]);
        s.x[0] = s.x[2] < list.size() ? list[s.x[2]] : 0;
    }, "@@:q");
    method(p, "lastItem", [popup_items](CpuState& s, GuestMemory&) { const auto list = popup_items(s.x[0]); s.x[0] = list.empty() ? 0 : list.back(); });
    method(p, "itemTitleAtIndex:", [this, popup_items](CpuState& s, GuestMemory&) {
        const auto list = popup_items(s.x[0]);
        s.x[0] = s.x[2] < list.size() ? send(list[s.x[2]], "title").x0 : auto_string("");
    }, "@@:q");
    method(p, "itemTitles", [this, popup_items](CpuState& s, GuestMemory&) {
        std::vector<Address> titles;
        for (const auto item : popup_items(s.x[0])) titles.push_back(send(item, "title").x0);
        s.x[0] = auto_array(titles);
    });
    const auto index_where = [popup_items](Address self, const std::function<bool(Address)>& match) -> std::int64_t {
        const auto list = popup_items(self);
        for (std::size_t i = 0; i < list.size(); ++i) if (match(list[i])) return static_cast<std::int64_t>(i);
        return -1;
    };
    method(p, "indexOfItem:", [index_where](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2];
        s.x[0] = static_cast<Address>(index_where(s.x[0], [wanted](Address item) { return item == wanted; }));
    }, "q@:@");
    method(p, "indexOfItemWithTitle:", [this, index_where](CpuState& s, GuestMemory&) {
        const auto wanted = text(s.x[2]);
        s.x[0] = static_cast<Address>(index_where(s.x[0], [&](Address item) { return text(send(item, "title").x0) == wanted; }));
    }, "q@:@");
    method(p, "indexOfItemWithTag:", [this, index_where](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2];
        s.x[0] = static_cast<Address>(index_where(s.x[0], [&](Address item) { return send(item, "tag").x0 == wanted; }));
    }, "q@:q");
    method(p, "indexOfItemWithRepresentedObject:", [this, index_where](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2];
        s.x[0] = static_cast<Address>(index_where(s.x[0], [&](Address item) {
            const auto value = send(item, "representedObject").x0;
            return value == wanted || (value && wanted && send_bool(value, "isEqual:", {wanted}));
        }));
    }, "q@:@");
    method(p, "itemWithTitle:", [this, index_where, popup_items](CpuState& s, GuestMemory&) {
        const auto wanted = text(s.x[2]);
        const auto index = index_where(s.x[0], [&](Address item) { return text(send(item, "title").x0) == wanted; });
        s.x[0] = index >= 0 ? popup_items(s.x[0])[static_cast<std::size_t>(index)] : 0;
    });
    const auto select_index = [this](Address self, std::int64_t index) {
        static_cast<void>(send_id(self, "selectItemAtIndex:", {static_cast<Address>(index)}));
    };
    method(p, "selectItem:", [this, index_where, select_index](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2];
        select_index(s.x[0], wanted ? index_where(s.x[0], [wanted](Address item) { return item == wanted; }) : -1);
    }, "v@:@");
    method(p, "selectItemWithTitle:", [this, index_where, select_index](CpuState& s, GuestMemory&) {
        const auto wanted = text(s.x[2]);
        select_index(s.x[0], index_where(s.x[0], [&](Address item) { return text(send(item, "title").x0) == wanted; }));
    }, "v@:@");
    method(p, "selectItemWithTag:", [this, index_where, select_index](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2];
        const auto index = index_where(s.x[0], [&](Address item) { return send(item, "tag").x0 == wanted; });
        if (index >= 0) select_index(s.x[0], index);
        return_bool(s, index >= 0);
    }, "B@:q");
    method(p, "titleOfSelectedItem", [this](CpuState& s, GuestMemory&) {
        const auto item = send(s.x[0], "selectedItem").x0;
        s.x[0] = item ? send(item, "title").x0 : 0;
    });
    method(p, "selectedTag", [this](CpuState& s, GuestMemory&) {
        const auto item = send(s.x[0], "selectedItem").x0;
        s.x[0] = item ? send(item, "tag").x0 : static_cast<Address>(-1);
    }, "q@:");
    method(p, "addItemWithTitle:", [this, popup_menu](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto list_menu = popup_menu(self);
        // A title already present is removed first (AppKit keeps titles unique).
        const auto existing = send_id(self, "indexOfItemWithTitle:", {s.x[2]});
        if (static_cast<std::int64_t>(existing) >= 0) static_cast<void>(send_id(list_menu, "removeItemAtIndex:", {existing}));
        static_cast<void>(send_id(list_menu, "addItemWithTitle:action:keyEquivalent:", {s.x[2], 0, auto_string("")}));
        if (static_cast<std::int64_t>(send(self, "indexOfSelectedItem").x0) < 0) static_cast<void>(send_id(self, "selectItemAtIndex:", {0}));
        invalidate_view(self);
    }, "v@:@");
    method(p, "addItemsWithTitles:", [this](CpuState& s, GuestMemory&) {
        for (const auto title : items(s.x[2])) static_cast<void>(send_id(s.x[0], "addItemWithTitle:", {title}));
    }, "v@:@");
    method(p, "insertItemWithTitle:atIndex:", [this, popup_menu](CpuState& s, GuestMemory&) {
        const auto item = send_id(send(menu_item, "alloc").x0, "initWithTitle:action:keyEquivalent:", {s.x[2], 0, auto_string("")});
        static_cast<void>(send_id(popup_menu(s.x[0]), "insertItem:atIndex:", {item, s.x[3]}));
        release(item);
        if (static_cast<std::int64_t>(send(s.x[0], "indexOfSelectedItem").x0) < 0) static_cast<void>(send_id(s.x[0], "selectItemAtIndex:", {0}));
        invalidate_view(s.x[0]);
    }, "v@:@q");
    method(p, "removeItemAtIndex:", [this, popup_menu, pcell](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(popup_menu(s.x[0]), "removeItemAtIndex:", {s.x[2]}));
        const auto state = cell_state(*this, pcell(s.x[0]));
        const auto count = static_cast<std::int64_t>(objc.host_state<MenuState>(popup_menu(s.x[0]))->items.size());
        if (state->selected_item >= count) state->selected_item = count - 1;
        invalidate_view(s.x[0]);
    }, "v@:q");
    method(p, "removeItemWithTitle:", [this](CpuState& s, GuestMemory&) {
        const auto index = send_id(s.x[0], "indexOfItemWithTitle:", {s.x[2]});
        if (static_cast<std::int64_t>(index) >= 0) static_cast<void>(send_id(s.x[0], "removeItemAtIndex:", {index}));
    }, "v@:@");
    method(p, "removeAllItems", [this, popup_menu, pcell](CpuState& s, GuestMemory&) {
        static_cast<void>(send(popup_menu(s.x[0]), "removeAllItems"));
        cell_state(*this, pcell(s.x[0]))->selected_item = -1;
        invalidate_view(s.x[0]);
    }, "v@:");
    method(p, "setTitle:", [this](CpuState& s, GuestMemory&) {
        const auto index = send_id(s.x[0], "indexOfItemWithTitle:", {s.x[2]});
        if (static_cast<std::int64_t>(index) < 0) static_cast<void>(send_id(s.x[0], "addItemWithTitle:", {s.x[2]}));
        static_cast<void>(send_id(s.x[0], "selectItemWithTitle:", {s.x[2]}));
    }, "v@:@");
    method(p, "synchronizeTitleAndSelectedItem", [this](CpuState& s, GuestMemory&) { invalidate_view(s.x[0]); }, "v@:");
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{{"autoenablesItems", "B@:"}, {"preferredEdge", "Q@:"}})
        method(p, name, [this, pcell, name](CpuState& s, GuestMemory&) { s.x[0] = send(pcell(s.x[0]), name).x0; }, types);
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{{"setAutoenablesItems:", "v@:B"}, {"setPreferredEdge:", "v@:Q"}})
        method(p, name, [this, pcell, name](CpuState& s, GuestMemory&) { static_cast<void>(send_id(pcell(s.x[0]), name, {s.x[2]})); }, types);
    method(p, "showsStateColumn", [this, popup_menu](CpuState& s, GuestMemory&) { s.x[0] = send(popup_menu(s.x[0]), "showsStateColumn").x0; }, "B@:");
    method(p, "setShowsStateColumn:", [this, popup_menu](CpuState& s, GuestMemory&) {
        static_cast<void>(send_id(popup_menu(s.x[0]), "setShowsStateColumn:", {s.x[2] & 0xff}));
        invalidate_view(s.x[0]);
    }, "v@:B");
    // A click pops the menu up over the button; choosing an item selects it
    // and sends the item's action, then the button's.
    method(p, "mouseDown:", [this, pcell, popup_menu](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = pcell(self);
        const auto state = cell_state(*this, target);
        if (!state->enabled) return;
        state->highlighted = true;
        invalidate_view(self);
        if (const auto owner = view_state(self)->window) display_window(owner);
        const auto bounds = view_bounds(*view_state(self));
        const auto chosen = popup_menu_tracking(popup_menu(self), self, bounds, state->pulls_down ? -1 : state->selected_item);
        state->highlighted = false;
        invalidate_view(self);
        if (chosen < 0) return;
        if (!state->pulls_down) static_cast<void>(send_id(self, "selectItemAtIndex:", {static_cast<Address>(chosen)}));
        const auto list = objc.host_state<MenuState>(popup_menu(self))->items;
        if (chosen < static_cast<std::int64_t>(list.size())) {
            const auto item = list[static_cast<std::size_t>(chosen)];
            const auto entry = objc.host_state<MenuItemState>(item);
            if (entry->action) static_cast<void>(send_action(entry->action, entry->target, item));
        }
        static_cast<void>(send_id(self, "sendAction:to:", {state->action, state->target}));
    }, "v@:@");

    // NSMenu popups outside popup buttons (context menus).
    method(menu, "popUpMenuPositioningItem:atLocation:inView:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = s.x[3];
        const auto list = objc.host_state<MenuState>(self)->items;
        std::int64_t initial = -1;
        for (std::size_t i = 0; i < list.size(); ++i) if (list[i] == s.x[2]) initial = static_cast<std::int64_t>(i);
        const auto location = point_arg(s);
        const auto chosen = popup_menu_tracking(self, target, {location, {0, 0}}, initial);
        if (chosen < 0) { s.x[0] = 0; return; }
        const auto item = list[static_cast<std::size_t>(chosen)];
        const auto entry = objc.host_state<MenuItemState>(item);
        if (entry->action) static_cast<void>(send_action(entry->action, entry->target, item));
        s.x[0] = 1;
    }, "B@:@{CGPoint=dd}@");
    class_method(menu, "popUpContextMenu:withEvent:forView:", [this](CpuState& s, GuestMemory&) {
        const auto location = event_location_in(*this, s.x[4], s.x[3]);
        const auto chosen = popup_menu_tracking(s.x[2], s.x[4], {location, {0, 0}}, -1);
        if (chosen < 0) return;
        const auto item = objc.host_state<MenuState>(s.x[2])->items[static_cast<std::size_t>(chosen)];
        const auto entry = objc.host_state<MenuItemState>(item);
        if (entry->action) static_cast<void>(send_action(entry->action, entry->target, item));
    }, "v@:@@@");
    // The size of the menu as its popup window shows it: the widest title
    // (and the minimum width), 22-point rows and 11-point separators.
    method(menu, "size", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = objc.host_state<MenuState>(self);
        const auto rows = menu_rows(*this, self);
        TextStyle style;
        style.font.size = 13;
        double widest = state->minimum_width;
        for (const auto item : state->items) {
            const auto entry = objc.host_state<MenuItemState>(item);
            if (!entry->hidden && !entry->separator) widest = std::max(widest, drawing::text_width(style.font, string16(entry->title)) + 44);
        }
        return_size(s, rows.empty() ? Size{widest, 0} : Size{std::ceil(widest), std::ceil(rows.back().bottom + 5)});
    }, "{CGSize=dd}@:");

    // The popup menu window's view: draws the items of `tracking_menu`.
    method(menu_view, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(menu_view, "drawRect:", [this](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto bounds = view_bounds(*view_state(self));
        const auto& th = theme_for(self);
        auto& canvas = require_canvas("popup menu drawing");
        const auto background = th.dark ? Color::gray(0.17) : Color::gray(0.97);
        draw_bezel(*this, bounds, 5, background, th.control_border);
        if (!tracking_menu) return;
        const auto list = objc.host_state<MenuState>(tracking_menu)->items;
        const auto highlighted = static_cast<std::int64_t>(view_state(self)->tag);
        double y = 5;
        TextStyle style;
        style.font.size = 13;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto entry = objc.host_state<MenuItemState>(list[i]);
            if (entry->hidden) continue;
            if (entry->separator) {
                canvas.fill_rect({{bounds.origin.x + 10, y + 5}, {bounds.size.width - 20, 1}}, th.separator);
                y += 11;
                continue;
            }
            const Rect row{{bounds.origin.x + 5, y}, {bounds.size.width - 10, 22}};
            const bool lit = static_cast<std::int64_t>(i) == highlighted && entry->enabled;
            if (lit) {
                drawing::Path path;
                path.add_rounded_rect(row, 4, 4);
                canvas.fill_path(path, th.accent);
            }
            style.color = !entry->enabled ? th.disabled_text : lit ? th.accent_text : th.text;
            if (entry->state == state_on) draw_line_in(*this, std::u16string(1, char16_t{0x2713}), style, {{row.origin.x + 4, row.origin.y}, {14, row.size.height}}, 0, false);
            draw_line_in(*this, string16(entry->title), style, {{row.origin.x + 20, row.origin.y}, {row.size.width - 24, row.size.height}}, 0);
            y += 22;
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
}


std::int64_t AppKit::popup_menu_tracking(Address target_menu, Address anchor_view, Rect anchor, std::int64_t initial) {
    if (!target_menu) return -1;
    const auto delegate = objc.host_state<MenuState>(target_menu)->delegate;
    const auto notify = [this, delegate, target_menu](const char* selector) {
        if (!delegate) return;
        CallRegs registers;
        registers.x[2] = target_menu;
        static_cast<void>(send_optional(delegate, selector, registers));
    };
    notify("menuNeedsUpdate:");
    notify("menuWillOpen:");
    std::int64_t chosen{};
    try {
        chosen = popup_menu_rows_tracking(target_menu, anchor_view, anchor, initial);
    } catch (...) {
        notify("menuDidClose:");
        throw;
    }
    notify("menuDidClose:");
    return chosen;
}

std::int64_t AppKit::popup_menu_rows_tracking(Address target_menu, Address anchor_view, Rect anchor, std::int64_t initial) {
    auto& server = require_window_server("NSMenu popup");
    const auto owner = view_state(anchor_view)->window;
    if (!owner || !window_state(owner)->id) fail("popup menu over a view outside a shown window");
    const auto rows = menu_rows(*this, target_menu);
    if (rows.empty()) return -1;
    // Size: the widest title.
    TextStyle style;
    style.font.size = 13;
    double widest = anchor.size.width;
    for (const auto item : objc.host_state<MenuState>(target_menu)->items)
        widest = std::max(widest, drawing::text_width(style.font, string16(objc.host_state<MenuItemState>(item)->title)) + 44);
    const double height = rows.back().bottom + 5;
    // Screen placement: the initial item over the anchor (popup buttons) or
    // the menu's top-left corner at the anchor point.
    const auto window_origin = content_rect(owner).origin;
    const auto anchor_window = view_rect_in_window(anchor_view, anchor);
    double top_in_menu = 0;
    if (initial >= 0)
        for (const auto& row : rows) if (row.index == initial) top_in_menu = row.top;
    const double screen_top = window_origin.y + anchor_window.origin.y + anchor_window.size.height + top_in_menu;
    const Rect content{{window_origin.x + anchor_window.origin.x, screen_top - height}, {std::ceil(widest), std::ceil(height)}};
    CallRegs init;
    put_rect(init, 0, content);
    init.x[2] = 0;   // borderless
    init.x[3] = 2;
    init.x[4] = 0;
    const auto popup = send(send(window, "alloc").x0, "initWithContentRect:styleMask:backing:defer:", init).x0;
    const auto menu_content = send(send(menu_view, "alloc").x0, "initWithFrame:", [&] { CallRegs r; put_rect(r, 0, {{0, 0}, content.size}); return r; }()).x0;
    static_cast<void>(send_id(popup, "setContentView:", {menu_content}));
    release(menu_content);
    static_cast<void>(send_id(popup, "setReleasedWhenClosed:", {0}));
    static_cast<void>(send_id(popup, "setLevel:", {101}));   // NSPopUpMenuWindowLevel
    const auto previous_menu = std::exchange(tracking_menu, target_menu);
    view_state(menu_content)->tag = initial;
    static_cast<void>(send_id(popup, "orderFront:", {0}));
    display_window(popup, true);
    const auto popup_id = window_state(popup)->id;
    std::int64_t chosen = -1;
    bool released_once{};
    bool dragged{};
    const auto row_at = [&](Point screen) -> std::int64_t {
        const auto frame = server.content_rect(popup_id);
        const double y_from_top = frame.origin.y + frame.size.height - screen.y;
        if (screen.x < frame.origin.x || screen.x >= frame.origin.x + frame.size.width) return -1;
        for (const auto& row : rows) if (y_from_top >= row.top && y_from_top < row.bottom && row.enabled) return row.index;
        return -1;
    };
    try {
        const auto start = monotonic_seconds();
        for (;;) {
            const auto event = next_event(left_mouse_up_mask | left_mouse_down_mask | left_mouse_dragged_mask | mouse_moved_mask | key_down_mask,
                                          monotonic_seconds() + 0.25, event_tracking_mode, true);
            if (!event) {
                // A press that was released without an event reaching us.
                const auto pressed = server.pressed_mouse_buttons() & 1;
                if (!pressed && !released_once && monotonic_seconds() - start > 0.5) released_once = true;
                continue;
            }
            const auto info = event_state(event);
            const auto type = info->event.type;
            if (type == event_type::key_down) {
                const auto code = info->event.key_code;
                if (code == 0x35) break;                                       // Escape
                const auto current = view_state(menu_content)->tag;
                if (code == 0x24 || code == 0x4c) { chosen = current; break; } // Return
                if (code == 0x7d || code == 0x7e) {                            // down / up
                    std::size_t at = 0;
                    for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].index == current) at = i;
                    if (code == 0x7d && at + 1 < rows.size()) ++at;
                    if (code == 0x7e && at > 0) --at;
                    view_state(menu_content)->tag = rows[at].index;
                    display_window(popup, true);
                }
                continue;
            }
            const auto hit = row_at(info->event.screen_location);
            if (hit != view_state(menu_content)->tag) {
                view_state(menu_content)->tag = hit;
                display_window(popup, true);
                if (type != event_type::left_mouse_up) dragged = true;
            }
            if (type == event_type::left_mouse_up) {
                // The release of the opening press keeps the menu open (click
                // to choose) unless the mouse was first dragged to another
                // item (press-drag-release); a later release over an item
                // chooses it.
                if (!released_once && !dragged) { released_once = true; continue; }
                if (hit >= 0) { chosen = hit; break; }
                if (released_once) break;
                released_once = true;
            } else if (type == event_type::left_mouse_down && hit < 0) {
                break;   // a click outside dismisses
            }
        }
    } catch (...) {
        tracking_menu = previous_menu;
        static_cast<void>(send(popup, "close"));
        release(popup);
        throw;
    }
    tracking_menu = previous_menu;
    static_cast<void>(send(popup, "close"));
    release(popup);
    return chosen;
}

} // namespace alchemy::gui::appkit
