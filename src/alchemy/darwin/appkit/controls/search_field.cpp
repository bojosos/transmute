#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

// NSSearchField / NSSearchFieldCell (the status window's and the light
// library's filters): a rounded text field with a search button (a
// magnifier) at the leading edge and a cancel button (a cross) at the
// trailing edge while there is text. Editing is the window's field editor
// between the two buttons. The action is sent as the user types: at once
// (-sendsSearchStringImmediately), after AppKit's short typing pause
// (performSelector:afterDelay:, 0.25 s here), or only on Return
// (-sendsWholeSearchString). The cancel button clears the text and sends
// the action; the search button pops the recent-searches menu built from
// -searchMenuTemplate (the NSSearchField...MenuItemTag items), else sends
// the action. Searches sent are remembered as recents (up to
// -maximumRecents). The delegate hears searchFieldDidStartSearching: /
// searchFieldDidEndSearching:. Nib decoding reads the button cells, the
// maximum recents, the menu template and the send modes.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

// The menu template's item tags.
constexpr std::int64_t recents_title_tag = 1000, recents_tag = 1001, clear_recents_tag = 1002, no_recents_tag = 1003;
constexpr double button_side = 16;
constexpr double typing_pause = 0.25;   // seconds before a non-immediate incremental search is sent

struct SearchState final : ObjectPayload {
    Address search_button{}, cancel_button{};   // +1 NSButtonCells
    Address menu_template{};                    // +1
    Address autosave_name{};                    // +1
    std::vector<Address> recents;               // +1 NSStrings, most recent first
    std::int64_t maximum_recents{10};
    bool sends_whole{}, sends_immediately{}, centers_placeholder{true}, searching{};
};

std::shared_ptr<SearchState> search_of(AppKit& kit, Address cell) { return cell_extension<SearchState>(kit, cell); }

Rect search_button_rect(Rect bounds) {
    return {{bounds.origin.x + 5, bounds.origin.y + std::floor((bounds.size.height - button_side) / 2)}, {button_side, button_side}};
}
Rect cancel_button_rect(Rect bounds) {
    return {{bounds.origin.x + bounds.size.width - button_side - 4, bounds.origin.y + std::floor((bounds.size.height - button_side) / 2)},
            {button_side, button_side}};
}
Rect search_text_rect(Rect bounds) {
    const double left = 5 + button_side + 2, right = button_side + 6;
    return {{bounds.origin.x + left, bounds.origin.y}, {std::max(0.0, bounds.size.width - left - right), bounds.size.height}};
}

// The text being searched for (the field editor's while editing).
std::u16string search_text(AppKit& kit, Address field) {
    if (const auto editor = kit.editing_field_editor(field)) return kit.string16(kit.send_id(editor, "string"));
    const auto cell = control_cell(kit, field);
    return cell ? cell_text(kit, cell) : std::u16string{};
}

void note_recent(AppKit& kit, Address cell, const std::u16string& text) {
    const auto state = search_of(kit, cell);
    if (text.empty() || state->maximum_recents <= 0) return;
    for (auto it = state->recents.begin(); it != state->recents.end(); ++it)
        if (kit.string16(*it) == text) { kit.release(*it); state->recents.erase(it); break; }
    state->recents.insert(state->recents.begin(), kit.string(drawing::utf8(text)));
    while (static_cast<std::int64_t>(state->recents.size()) > state->maximum_recents) {
        kit.release(state->recents.back());
        state->recents.pop_back();
    }
}

// The start/end of searching (the delegate's notifications).
void update_searching(AppKit& kit, Address field, bool now) {
    const auto cell = control_cell(kit, field);
    if (!cell) return;
    const auto state = search_of(kit, cell);
    if (state->searching == now) return;
    state->searching = now;
    const auto delegate = kit.extension<ControlState>(field)->delegate;
    if (delegate) {
        CallRegs registers;
        registers.x[2] = field;
        static_cast<void>(kit.send_optional(delegate, now ? "searchFieldDidStartSearching:" : "searchFieldDidEndSearching:", registers));
    }
}

void send_search(AppKit& kit, Address field) {
    const auto cell = control_cell(kit, field);
    if (!cell) return;
    const auto info = cell_state(kit, cell);
    const auto text = search_text(kit, field);
    note_recent(kit, cell, text);
    update_searching(kit, field, !text.empty());
    static_cast<void>(kit.send_id(field, "sendAction:to:", {info->action, info->target}));
}

void draw_magnifier(drawing::Canvas& canvas, Rect r, Color color, bool flipped) {
    const double radius = r.size.width * 0.28;
    const Point center{r.origin.x + r.size.width * 0.42, r.origin.y + (flipped ? r.size.height * 0.42 : r.size.height * 0.58)};
    drawing::Path ring;
    ring.add_oval({{center.x - radius, center.y - radius}, {2 * radius, 2 * radius}});
    canvas.stroke_path(ring, color, 1.4);
    const double reach = radius * std::numbers::sqrt2 / 2;
    const double dy = flipped ? 1 : -1;
    canvas.stroke_line({center.x + reach, center.y + dy * reach}, {r.origin.x + r.size.width * 0.82, center.y + dy * (r.size.width * 0.82 - (center.x - r.origin.x))},
                       color, 1.6);
}
void draw_cancel(drawing::Canvas& canvas, Rect r, Color circle, Color cross) {
    const Rect disc{{r.origin.x + 2, r.origin.y + 2}, {r.size.width - 4, r.size.height - 4}};
    drawing::Path path;
    path.add_oval(disc);
    canvas.fill_path(path, circle);
    const double c = disc.size.width / 2, k = c * 0.42;
    const Point mid{disc.origin.x + c, disc.origin.y + c};
    canvas.stroke_line({mid.x - k, mid.y - k}, {mid.x + k, mid.y + k}, cross, 1.3);
    canvas.stroke_line({mid.x - k, mid.y + k}, {mid.x + k, mid.y - k}, cross, 1.3);
}

} // namespace

void install_search_field(AppKit& kit) {
    auto* const k = &kit;
    const auto cell_class = kit.declare_class("NSSearchFieldCell", "NSTextFieldCell");
    const auto field_class = kit.declare_class("NSSearchField", "NSTextField");
    static_cast<void>(kit.send_id(field_class, "setCellClass:", {cell_class}));
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, std::int64_t>>{
             {"_NSSearchFieldRecentsTitleMenuItemTag", recents_title_tag}, {"_NSSearchFieldRecentsMenuItemTag", recents_tag},
             {"_NSSearchFieldClearRecentsMenuItemTag", clear_recents_tag}, {"_NSSearchFieldNoRecentsMenuItemTag", no_recents_tag}}) {
        // NSInteger constants exported as data.
        const auto cell = kit.process.allocate_guest(8);
        kit.memory().write64(cell, static_cast<Address>(value));
        for (const auto library : {appkit::appkit_library, appkit::cocoa_library})
            kit.process.install_export(std::string(library), symbol, {cell, ExportKind::data});
    }

    // ---- NSSearchFieldCell ----
    const auto c = cell_class;
    const auto setup = [k](Address self) {
        const auto cell = cell_state(*k, self);
        cell->leading_inset = 5 + button_side + 2 - 4;   // beside the search button (the field editor's area starts 4 in)
        cell->trailing_inset = button_side + 6 - 4;
        cell->scrollable = true;
        cell->single_line = true;
        static_cast<void>(search_of(*k, self));
    };
    const auto make_buttons = [k](Address self) {
        const auto state = search_of(*k, self);
        const auto button_cell_class = k->objc.find_class("NSButtonCell");
        if (!state->search_button) {
            state->search_button = k->send_id(k->send(button_cell_class, "alloc").x0, "initTextCell:", {k->auto_string("")});
            static_cast<void>(k->send_id(state->search_button, "setBordered:", {0}));
        }
        if (!state->cancel_button) {
            state->cancel_button = k->send_id(k->send(button_cell_class, "alloc").x0, "initTextCell:", {k->auto_string("")});
            static_cast<void>(k->send_id(state->cancel_button, "setBordered:", {0}));
        }
    };
    kit.method(c, "initTextCell:", [k, c, setup, make_buttons](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto self = k->objc.send_super(s.x[0], c, k->sel("initTextCell:"), registers).x0;
        s.x[0] = self;
        if (!self) return;
        setup(self);
        make_buttons(self);
        cell_state(*k, self)->frame_style_bezel = true;   // rounded
    });
    kit.method(c, "initWithCoder:", [k, c, setup, make_buttons](CpuState& s, GuestMemory&) {
        const auto self = super_init_with_coder(*k, s.x[0], c, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        setup(self);
        const Decoder in{*k, s.x[2]};
        const auto state = search_of(*k, self);
        if (const auto button = in.object("NSSearchButtonCell")) k->assign(state->search_button, button);
        if (const auto button = in.object("NSCancelButtonCell")) k->assign(state->cancel_button, button);
        make_buttons(self);
        state->maximum_recents = in.integer("NSMaximumRecents", state->maximum_recents);
        if (const auto menu = in.object("NSSearchMenuTemplate")) k->assign(state->menu_template, menu);
        if (const auto name = in.object("NSRecentsAutosaveName")) k->assign(state->autosave_name, name);
        state->sends_whole = in.flag("NSSendsWholeSearchString", state->sends_whole);
        state->sends_immediately = in.flag("NSSendsSearchStringImmediately", state->sends_immediately);
        cell_state(*k, self)->frame_style_bezel = true;
    });
    kit.method(c, "copyWithZone:", [k, c](CpuState& s, GuestMemory&) {
        const auto from = search_of(*k, s.x[0]);
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto copy = k->objc.send_super(s.x[0], c, k->sel("copyWithZone:"), registers).x0;
        auto to = std::make_shared<SearchState>(*from);
        for (auto* slot : {&to->search_button, &to->cancel_button, &to->menu_template, &to->autosave_name}) *slot = k->retain(*slot);
        for (auto& recent : to->recents) recent = k->retain(recent);
        cell_state(*k, copy)->extension = to;
        s.x[0] = copy;
    }, "@@:^v");
    kit.method(c, "dealloc", [k, c](CpuState& s, GuestMemory&) {
        if (const auto cell = k->objc.find_host_state<CellState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<SearchState>(cell->extension)) {
                for (auto* slot : {&state->search_button, &state->cancel_button, &state->menu_template, &state->autosave_name})
                    k->release(std::exchange(*slot, 0));
                for (const auto recent : std::exchange(state->recents, {})) k->release(recent);
            }
        static_cast<void>(k->objc.send_super(s.x[0], c, k->sel("dealloc")));
    }, "v@:");
    const auto object = [&kit, c, k](const char* getter, const char* setter, Address SearchState::*member) {
        kit.method(c, getter, [k, member](CpuState& s, GuestMemory&) { s.x[0] = (*search_of(*k, s.x[0])).*member; });
        kit.method(c, setter, [k, member](CpuState& s, GuestMemory&) { k->assign((*search_of(*k, s.x[0])).*member, s.x[2]); }, "v@:@");
    };
    object("searchButtonCell", "setSearchButtonCell:", &SearchState::search_button);
    object("cancelButtonCell", "setCancelButtonCell:", &SearchState::cancel_button);
    object("searchMenuTemplate", "setSearchMenuTemplate:", &SearchState::menu_template);
    object("recentsAutosaveName", "setRecentsAutosaveName:", &SearchState::autosave_name);
    kit.method(c, "resetSearchButtonCell", [k, make_buttons](CpuState& s, GuestMemory&) {
        k->release(std::exchange(search_of(*k, s.x[0])->search_button, 0));
        make_buttons(s.x[0]);
    }, "v@:");
    kit.method(c, "resetCancelButtonCell", [k, make_buttons](CpuState& s, GuestMemory&) {
        k->release(std::exchange(search_of(*k, s.x[0])->cancel_button, 0));
        make_buttons(s.x[0]);
    }, "v@:");
    const auto flag = [&kit, c, k](const char* getter, const char* setter, bool SearchState::*member) {
        kit.method(c, getter, [k, member](CpuState& s, GuestMemory&) { return_bool(s, (*search_of(*k, s.x[0])).*member); }, "B@:");
        kit.method(c, setter, [k, member](CpuState& s, GuestMemory&) { (*search_of(*k, s.x[0])).*member = bool_arg(s, 2); }, "v@:B");
    };
    flag("sendsWholeSearchString", "setSendsWholeSearchString:", &SearchState::sends_whole);
    flag("sendsSearchStringImmediately", "setSendsSearchStringImmediately:", &SearchState::sends_immediately);
    flag("centersPlaceholder", "setCentersPlaceholder:", &SearchState::centers_placeholder);
    kit.method(c, "maximumRecents", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(search_of(*k, s.x[0])->maximum_recents); }, "q@:");
    kit.method(c, "setMaximumRecents:", [k](CpuState& s, GuestMemory&) {
        const auto state = search_of(*k, s.x[0]);
        state->maximum_recents = std::clamp<std::int64_t>(static_cast<std::int64_t>(s.x[2]), 0, 254);
        while (static_cast<std::int64_t>(state->recents.size()) > state->maximum_recents) {
            k->release(state->recents.back());
            state->recents.pop_back();
        }
    }, "v@:q");
    kit.method(c, "recentSearches", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_array(search_of(*k, s.x[0])->recents); });
    kit.method(c, "setRecentSearches:", [k](CpuState& s, GuestMemory&) {
        const auto state = search_of(*k, s.x[0]);
        std::vector<Address> fresh;
        for (const auto value : s.x[2] ? k->items(s.x[2]) : std::vector<Address>{})
            if (value && static_cast<std::int64_t>(fresh.size()) < state->maximum_recents) fresh.push_back(k->cf.copy(value));
        for (const auto old : std::exchange(state->recents, fresh)) k->release(old);
    }, "v@:@");
    for (const auto& [name, rect_of] : std::initializer_list<std::pair<const char*, Rect (*)(Rect)>>{
             {"searchTextRectForBounds:", search_text_rect}, {"searchButtonRectForBounds:", search_button_rect},
             {"cancelButtonRectForBounds:", cancel_button_rect}}) {
        const auto compute = rect_of;
        kit.method(c, name, [compute](CpuState& s, GuestMemory&) { return_rect(s, compute(rect_arg(s))); },
                   "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    }
    // Drawing: the rounded field, the buttons, then the text between them.
    kit.method(c, "drawWithFrame:inView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto cell = cell_state(*k, self);
        const auto frame = rect_arg(s);
        const auto& th = k->theme_for(owner);
        auto& canvas = k->require_canvas("NSSearchFieldCell drawing");
        const auto background = cell->draws_background || cell->bezeled ? k->color_value(cell->background_color, th.text_background)
                                                                         : Color{0, 0, 0, 0};
        draw_bezel(*k, frame, std::min(frame.size.height / 2, 8.0), background, th.control_border);
        const bool flipped = k->current_flipped();
        draw_magnifier(canvas, search_button_rect(frame), cell->enabled ? th.secondary_text : th.disabled_text, flipped);
        const bool editing = owner && k->is_kind(owner, k->control) && k->extension<ControlState>(owner)->editor;
        const auto text = owner && k->is_kind(owner, k->control) ? search_text(*k, owner) : cell_text(*k, self);
        if (!text.empty()) draw_cancel(canvas, cancel_button_rect(frame), th.secondary_text.with_alpha(0.75), th.text_background);
        if (editing) return;   // the field editor draws the text
        CallRegs registers;
        put_rect(registers, 0, frame);
        registers.x[2] = owner;
        static_cast<void>(k->send(self, "drawInteriorWithFrame:inView:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    kit.method(c, "drawInteriorWithFrame:inView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto cell = cell_state(*k, self);
        const auto& th = k->theme_for(owner);
        auto style = cell_text_style(*k, self, th);
        auto value = cell_text(*k, self);
        const auto area = search_text_rect(rect_arg(s));
        std::int64_t alignment = style.alignment;
        if (value.empty() && cell->placeholder) {
            value = k->string16(cell->placeholder);
            style.color = th.secondary_text;
            if (!k->find_extension<ControlState>(owner) || search_of(*k, self)->centers_placeholder) alignment = 0;
        }
        draw_line_in(*k, value, style, area, alignment == 4 ? 0 : alignment);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    // The nib's button cells target the search field cell with these.
    kit.method(c, "_searchFieldSearch:", [k](CpuState& s, GuestMemory&) {
        if (const auto view = cell_state(*k, s.x[0])->control_view) send_search(*k, view);
    }, "v@:@");
    kit.method(c, "_searchFieldCancel:", [k](CpuState& s, GuestMemory&) {
        if (const auto view = cell_state(*k, s.x[0])->control_view) static_cast<void>(k->send(view, "_alchemyCancelSearch"));
    }, "v@:@");

    // ---- NSSearchField ----
    const auto n = field_class;
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{
             {"searchMenuTemplate", "@@:"}, {"setSearchMenuTemplate:", "v@:@"}, {"sendsWholeSearchString", "B@:"},
             {"setSendsWholeSearchString:", "v@:B"}, {"sendsSearchStringImmediately", "B@:"},
             {"setSendsSearchStringImmediately:", "v@:B"}, {"maximumRecents", "q@:"}, {"setMaximumRecents:", "v@:q"},
             {"recentSearches", "@@:"}, {"setRecentSearches:", "v@:@"}, {"recentsAutosaveName", "@@:"},
             {"setRecentsAutosaveName:", "v@:@"}, {"centersPlaceholder", "B@:"}, {"setCentersPlaceholder:", "v@:B"}})
        forward_to_cell(kit, n, name, types, false);
    const auto bounds_rect = [k](Address self, Rect (*compute)(Rect)) { return compute(AppKit::view_bounds(*k->view_state(self))); };
    kit.method(n, "rectForSearchTextWhenCentered:", [bounds_rect](CpuState& s, GuestMemory&) { return_rect(s, bounds_rect(s.x[0], search_text_rect)); },
               "{CGRect={CGPoint=dd}{CGSize=dd}}@:B");
    kit.method(n, "rectForSearchButtonWhenCentered:", [bounds_rect](CpuState& s, GuestMemory&) { return_rect(s, bounds_rect(s.x[0], search_button_rect)); },
               "{CGRect={CGPoint=dd}{CGSize=dd}}@:B");
    kit.method(n, "rectForCancelButtonWhenCentered:", [bounds_rect](CpuState& s, GuestMemory&) { return_rect(s, bounds_rect(s.x[0], cancel_button_rect)); },
               "{CGRect={CGPoint=dd}{CGSize=dd}}@:B");
    // Typing: the incremental search (unless only Return sends it).
    kit.method(n, "textDidChange:", [k, n](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        CallRegs registers;
        registers.x[2] = s.x[2];
        static_cast<void>(k->objc.send_super(self, n, k->sel("textDidChange:"), registers));
        const auto cell = control_cell(*k, self);
        if (!cell) return;
        const auto state = search_of(*k, cell);
        k->invalidate_view(self);   // the cancel button follows the text
        if (state->sends_whole) return;
        if (state->sends_immediately) { send_search(*k, self); return; }
        const auto object_class = k->objc.find_class("NSObject");
        static_cast<void>(k->send_id(object_class, "cancelPreviousPerformRequestsWithTarget:selector:object:",
                                     {self, k->sel("_alchemySendSearch:"), 0}));
        CallRegs later;
        later.x[2] = k->sel("_alchemySendSearch:");
        later.x[3] = 0;
        later.set_d(0, typing_pause);
        static_cast<void>(k->send(self, "performSelector:withObject:afterDelay:", later));
    }, "v@:@");
    kit.method(n, "_alchemySendSearch:", [k](CpuState& s, GuestMemory&) { send_search(*k, s.x[0]); }, "v@:@");
    // Return sends the whole string (NSTextField's end-of-editing action);
    // the recents and the searching state follow it.
    kit.method(n, "sendAction:to:", [k, n](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        if (const auto cell = control_cell(*k, self)) {
            const auto text = search_text(*k, self);
            if (!text.empty()) note_recent(*k, cell, text);
            update_searching(*k, self, !text.empty());
        }
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[3];
        s.x[0] = k->objc.send_super(self, n, k->sel("sendAction:to:"), registers).x0;
    }, "B@::@");
    kit.method(n, "_alchemyCancelSearch", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        // A pending incremental search (one is only ever scheduled through the run loop's delayed performs).
        const auto object_class = k->objc.find_class("NSObject");
        if (k->responds(object_class, "cancelPreviousPerformRequestsWithTarget:selector:object:"))
            static_cast<void>(k->send_id(object_class, "cancelPreviousPerformRequestsWithTarget:selector:object:",
                                         {self, k->sel("_alchemySendSearch:"), 0}));
        if (const auto editor = k->editing_field_editor(self)) static_cast<void>(k->send_id(editor, "setString:", {k->auto_string("")}));
        static_cast<void>(k->send_id(self, "setStringValue:", {k->auto_string("")}));
        k->invalidate_view(self);
        const auto cell = control_cell(*k, self);
        if (!cell) return;
        const auto info = cell_state(*k, cell);
        static_cast<void>(k->send_id(self, "sendAction:to:", {info->action, info->target}));
    }, "v@:");
    kit.method(n, "mouseDown:", [k, n](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto cell = control_cell(*k, self);
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const auto location = event_location_in(*k, self, event);
        if (cell && cell_state(*k, cell)->enabled) {
            const auto state = search_of(*k, cell);
            if (rect_contains(cancel_button_rect(bounds), location) && !search_text(*k, self).empty()) {
                if (track_mouse(*k, self, event, [](bool) {})) static_cast<void>(k->send(self, "_alchemyCancelSearch"));
                return;
            }
            if (rect_contains(search_button_rect(bounds), location)) {
                if (!state->menu_template) {
                    if (track_mouse(*k, self, event, [](bool) {})) send_search(*k, self);
                    return;
                }
                // The recent-searches menu from the template.
                const auto menu = k->autorelease(k->send_id(k->send(k->menu, "alloc").x0, "initWithTitle:", {k->auto_string("")}));
                struct Entry { std::int64_t kind; Address value; };   // 0 template item, 1 recent, 2 clear
                std::vector<Entry> entries;
                const bool any = !state->recents.empty();
                for (const auto item : k->items(k->send_id(state->menu_template, "itemArray"))) {
                    const auto tag = static_cast<std::int64_t>(k->send_id(item, "tag"));
                    if ((tag == recents_title_tag || tag == clear_recents_tag) && !any) continue;
                    if (tag == no_recents_tag && any) continue;
                    if (tag == recents_tag) {
                        for (const auto recent : state->recents) {
                            static_cast<void>(k->send_id(menu, "addItemWithTitle:action:keyEquivalent:", {recent, 0, k->auto_string("")}));
                            entries.push_back({1, recent});
                        }
                        continue;
                    }
                    const auto copy = k->autorelease(k->send(item, "copy").x0);
                    if (tag == recents_title_tag || tag == no_recents_tag) static_cast<void>(k->send_id(copy, "setEnabled:", {0}));
                    static_cast<void>(k->send_id(menu, "addItem:", {copy}));
                    entries.push_back({tag == clear_recents_tag ? 2 : 0, copy});
                }
                const auto chosen = k->popup_menu_tracking(menu, self, {{bounds.origin.x, bounds.origin.y}, {bounds.size.width, bounds.size.height}}, -1);
                if (chosen < 0 || chosen >= static_cast<std::int64_t>(entries.size())) return;
                const auto& entry = entries[static_cast<std::size_t>(chosen)];
                if (entry.kind == 1) {
                    static_cast<void>(k->send_id(self, "setStringValue:", {entry.value}));
                    send_search(*k, self);
                } else if (entry.kind == 2) {
                    for (const auto recent : std::exchange(state->recents, {})) k->release(recent);
                } else {
                    static_cast<void>(k->send_id(menu, "performActionForItemAtIndex:", {static_cast<Address>(chosen)}));
                }
                return;
            }
        }
        CallRegs registers;
        registers.x[2] = event;
        static_cast<void>(k->objc.send_super(self, n, k->sel("mouseDown:"), registers));
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit::controls
