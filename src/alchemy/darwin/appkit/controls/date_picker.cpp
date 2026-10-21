#include "alchemy/darwin/appkit/controls/g3.hpp"
#include "alchemy/darwin/appkit/controls/date_elements.hpp"
#include "alchemy/darwin/foundation/date_format.hpp"

#include <algorithm>
#include <cmath>

// NSDatePicker / NSDatePickerCell in the text-field styles (the license
// window's expiry date): the date shown by its elements in the cell's time
// zone (date_elements.cpp), a stepper at the trailing edge in the
// text-field-and-stepper style. A click selects a field (and the picker
// becomes first responder); the arrow keys and the stepper step the
// selected field, left/right move between fields, digits type into it and
// A/P set the half of the day. Every change is clamped to the minimum and
// maximum dates, offered to the delegate
// (datePickerCell:validateProposedDateValue:timeInterval:) and sent as the
// action. The calendar style and the time-zone and era elements are not
// provided (they fail explicitly). Nib decoding reads the date, its range,
// the elements, style and mode.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;
namespace elements = date_elements;

// NSDatePickerStyle / NSDatePickerMode.
namespace picker_style { inline constexpr std::int64_t text_field_and_stepper = 0, clock_and_calendar = 1, text_field = 2; }
constexpr double stepper_width = 13;

struct PickerState final : ObjectPayload {
    double date{};                      // reference seconds
    std::optional<double> min_date, max_date;
    double time_interval{};
    std::uint64_t elements{date_elements::year_month_day | date_elements::hour_minute_second};
    std::int64_t style{picker_style::text_field_and_stepper}, mode{};
    Address calendar{}, locale{}, time_zone{};   // +1
    Address delegate{};                          // not retained
    std::int64_t field{-1};                      // the selected field (-1: none)
    std::int64_t pending{};                      // digits typed into the field so far
};

std::shared_ptr<PickerState> picker_of(AppKit& kit, Address cell) { return cell_extension<PickerState>(kit, cell); }

void check_elements(std::uint64_t value) {
    if (value & (date_elements::time_zone | date_elements::era))
        unsupported("NSDatePicker time-zone and era elements (NSDatePickerElementFlag 0x" + [&] {
            char buffer[16];
            std::snprintf(buffer, sizeof buffer, "%llx", static_cast<unsigned long long>(value));
            return std::string(buffer);
        }() + ")");
}
void check_style(std::int64_t value) {
    if (value == picker_style::clock_and_calendar) unsupported("NSDatePickerStyleClockAndCalendar (the graphical calendar)");
}

// The zone offset (seconds east of GMT) at `reference` in the cell's zone.
std::int64_t zone_offset(AppKit& kit, const PickerState& state, double reference) {
    auto zone = state.time_zone;
    const auto zone_class = kit.objc.find_class("NSTimeZone");
    if (!zone && zone_class) zone = kit.send(zone_class, "localTimeZone").x0;
    if (!zone || !kit.responds(zone, "secondsFromGMTForDate:")) return 0;
    const auto date = kit.cf.date(reference);
    const auto offset = static_cast<std::int64_t>(kit.send_id(zone, "secondsFromGMTForDate:", {date}));
    kit.release(date);
    return offset;
}
elements::Civil civil_of(AppKit& kit, const PickerState& state, double reference) {
    const auto c = completion_kit::dates::civil(reference, zone_offset(kit, state, reference));
    return {c.year, c.month, c.day, c.hour, c.minute, c.second};
}
double reference_of(AppKit& kit, const PickerState& state, const elements::Civil& value, double hint) {
    completion_kit::dates::Civil c;
    c.year = value.year; c.month = value.month; c.day = value.day;
    c.hour = value.hour; c.minute = value.minute; c.second = value.second;
    // The fraction of a second is kept from the previous value.
    c.fraction = hint - std::floor(hint);
    return completion_kit::dates::reference_seconds(c, zone_offset(kit, state, hint));
}
elements::Formatted formatted(AppKit& kit, const PickerState& state) {
    return elements::format(state.elements, civil_of(kit, state, state.date));
}

Rect text_area(Rect bounds, const PickerState& state) {
    const double trailing = state.style == picker_style::text_field_and_stepper ? stepper_width + 2 : 0;
    return {{bounds.origin.x, bounds.origin.y}, {std::max(0.0, bounds.size.width - trailing), bounds.size.height}};
}
Rect stepper_area(Rect bounds) {
    const double height = std::min(bounds.size.height, 22.0);
    return {{bounds.origin.x + bounds.size.width - stepper_width, bounds.origin.y + (bounds.size.height - height) / 2}, {stepper_width, height}};
}

// A user change: clamp, ask the delegate, store, send the action.
void propose(AppKit& kit, Address cell, double reference) {
    const auto state = picker_of(kit, cell);
    if (state->min_date) reference = std::max(reference, *state->min_date);
    if (state->max_date) reference = std::min(reference, *state->max_date);
    auto interval = state->time_interval;
    if (state->delegate && kit.responds(state->delegate, "datePickerCell:validateProposedDateValue:timeInterval:")) {
        const auto slots = kit.process.allocate_guest(16);
        const auto proposed = kit.cf.date(reference);
        kit.memory().write64(slots, proposed);
        kit.memory().write64(slots + 8, std::bit_cast<std::uint64_t>(interval));
        try {
            static_cast<void>(kit.send_id(state->delegate, "datePickerCell:validateProposedDateValue:timeInterval:", {cell, slots, slots + 8}));
        } catch (...) {
            kit.release(proposed);
            kit.process.release_guest(slots);
            throw;
        }
        const auto answer = kit.memory().read64(slots);
        if (answer) reference = kit.cf.date_time(answer);
        interval = std::bit_cast<double>(kit.memory().read64(slots + 8));
        kit.release(proposed);
        kit.process.release_guest(slots);
    }
    state->date = reference;
    state->time_interval = interval;
    const auto info = cell_state(kit, cell);
    if (const auto view = info->control_view) {
        kit.invalidate_view(view);
        static_cast<void>(kit.send_id(view, "sendAction:to:", {info->action, info->target}));
    }
}

void step_field(AppKit& kit, Address cell, std::int64_t delta) {
    const auto state = picker_of(kit, cell);
    const auto shown = formatted(kit, *state);
    if (shown.fields.empty()) return;
    if (state->field < 0 || state->field >= static_cast<std::int64_t>(shown.fields.size())) state->field = 0;
    state->pending = 0;
    const auto kind = shown.fields[static_cast<std::size_t>(state->field)].kind;
    const auto next = elements::step(civil_of(kit, *state, state->date), kind, delta);
    propose(kit, cell, reference_of(kit, *state, next, state->date));
}

void draw_stepper(drawing::Canvas& canvas, AppKit& kit, Rect area, const Theme& th, bool enabled) {
    draw_bezel(kit, area, 4, th.control_face, th.control_border);
    const bool flipped = kit.current_flipped();
    const double cx = area.origin.x + area.size.width / 2;
    const double top = flipped ? area.origin.y : area.origin.y + area.size.height;
    const double dir = flipped ? 1 : -1;   // towards the bottom
    const auto color = enabled ? th.text : th.disabled_text;
    const double quarter = area.size.height / 4;
    drawing::Path up, down;
    up.move_to({cx - 3, top + dir * (quarter + 1.5)});
    up.line_to({cx + 3, top + dir * (quarter + 1.5)});
    up.line_to({cx, top + dir * (quarter - 1.5)});
    up.close();
    down.move_to({cx - 3, top + dir * (3 * quarter - 1.5)});
    down.line_to({cx + 3, top + dir * (3 * quarter - 1.5)});
    down.line_to({cx, top + dir * (3 * quarter + 1.5)});
    down.close();
    canvas.fill_path(up, color);
    canvas.fill_path(down, color);
}

} // namespace

void install_date_picker(AppKit& kit) {
    auto* const k = &kit;
    const auto cell_class = kit.declare_class("NSDatePickerCell", "NSActionCell");
    const auto control_class = kit.declare_class("NSDatePicker", "NSControl");
    static_cast<void>(kit.send_id(control_class, "setCellClass:", {cell_class}));

    // ---- NSDatePickerCell ----
    const auto c = cell_class;
    kit.method(c, "initTextCell:", [k, c](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto self = k->objc.send_super(s.x[0], c, k->sel("initTextCell:"), registers).x0;
        s.x[0] = self;
        if (!self) return;
        const auto cell = cell_state(*k, self);
        k->release(std::exchange(cell->contents, 0));
        cell->bezeled = cell->draws_background = true;
        cell->editable = cell->selectable = true;
        // A new date picker shows the current date.
        picker_of(*k, self)->date = CoreFoundation::absolute_time_now();
    });
    kit.method(c, "initWithCoder:", [k, c](CpuState& s, GuestMemory&) {
        const auto self = super_init_with_coder(*k, s.x[0], c, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        const auto cell = cell_state(*k, self);
        const auto state = picker_of(*k, self);
        // NSCell's decoding put the archived NSDate in the contents.
        if (const auto date = std::exchange(cell->contents, 0)) {
            state->date = k->send(date, "timeIntervalSinceReferenceDate").d(0);
            k->release(date);
        }
        if (in.flag("NSDatePickerUseCurrentDateDuringDecoding")) state->date = CoreFoundation::absolute_time_now();
        if (const auto date = in.object("NSMinDate")) state->min_date = k->send(date, "timeIntervalSinceReferenceDate").d(0);
        if (const auto date = in.object("NSMaxDate")) state->max_date = k->send(date, "timeIntervalSinceReferenceDate").d(0);
        state->time_interval = in.real("NSTimeInterval", 0);
        state->elements = static_cast<std::uint64_t>(in.integer("NSDatePickerElements", static_cast<std::int64_t>(state->elements)));
        state->style = in.integer("NSDatePickerType", in.integer("NSDatePickerStyle", picker_style::text_field_and_stepper));
        state->mode = in.integer("NSDatePickerMode", 0);
        check_elements(state->elements);
        check_style(state->style);
        cell->draws_background = in.flag("NSDrawsBackground", cell->draws_background);
        if (const auto delegate = in.object("NSDelegate")) state->delegate = delegate;
        cell->editable = cell->selectable = true;
    });
    kit.method(c, "copyWithZone:", [k, c](CpuState& s, GuestMemory&) {
        const auto from = picker_of(*k, s.x[0]);
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto copy = k->objc.send_super(s.x[0], c, k->sel("copyWithZone:"), registers).x0;
        auto to = std::make_shared<PickerState>(*from);
        for (auto* slot : {&to->calendar, &to->locale, &to->time_zone}) *slot = k->retain(*slot);
        cell_state(*k, copy)->extension = to;
        s.x[0] = copy;
    }, "@@:^v");
    kit.method(c, "dealloc", [k, c](CpuState& s, GuestMemory&) {
        if (const auto cell = k->objc.find_host_state<CellState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<PickerState>(cell->extension))
                for (auto* slot : {&state->calendar, &state->locale, &state->time_zone}) k->release(std::exchange(*slot, 0));
        static_cast<void>(k->objc.send_super(s.x[0], c, k->sel("dealloc")));
    }, "v@:");
    const auto redisplay = [k](Address cell) { if (const auto view = cell_state(*k, cell)->control_view) k->invalidate_view(view); };
    kit.method(c, "dateValue", [k](CpuState& s, GuestMemory&) { s.x[0] = k->autorelease(k->cf.date(picker_of(*k, s.x[0])->date)); });
    kit.method(c, "setDateValue:", [k, redisplay](CpuState& s, GuestMemory&) {
        if (!s.x[2]) unsupported("-[NSDatePickerCell setDateValue:] with nil");
        picker_of(*k, s.x[0])->date = k->send(s.x[2], "timeIntervalSinceReferenceDate").d(0);
        redisplay(s.x[0]);
    }, "v@:@");
    kit.method(c, "objectValue", [k](CpuState& s, GuestMemory&) { s.x[0] = k->autorelease(k->cf.date(picker_of(*k, s.x[0])->date)); });
    kit.method(c, "setObjectValue:", [k](CpuState& s, GuestMemory&) {
        if (s.x[2]) static_cast<void>(k->send_id(s.x[0], "setDateValue:", {s.x[2]}));
    }, "v@:@");
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, std::optional<double> PickerState::*>>{
             {"minDate", "setMinDate:", &PickerState::min_date}, {"maxDate", "setMaxDate:", &PickerState::max_date}}) {
        const auto field = member;
        kit.method(c, getter, [k, field](CpuState& s, GuestMemory&) {
            const auto value = (*picker_of(*k, s.x[0])).*field;
            s.x[0] = value ? k->autorelease(k->cf.date(*value)) : 0;
        });
        kit.method(c, setter, [k, field, redisplay](CpuState& s, GuestMemory&) {
            auto& slot = (*picker_of(*k, s.x[0])).*field;
            if (s.x[2]) slot = k->send(s.x[2], "timeIntervalSinceReferenceDate").d(0);
            else slot.reset();
            redisplay(s.x[0]);
        }, "v@:@");
    }
    kit.method(c, "timeInterval", [k](CpuState& s, GuestMemory&) { set_d(s, 0, picker_of(*k, s.x[0])->time_interval); }, "d@:");
    kit.method(c, "setTimeInterval:", [k](CpuState& s, GuestMemory&) { picker_of(*k, s.x[0])->time_interval = d(s, 0); }, "v@:d");
    kit.method(c, "datePickerElements", [k](CpuState& s, GuestMemory&) { s.x[0] = picker_of(*k, s.x[0])->elements; }, "Q@:");
    kit.method(c, "setDatePickerElements:", [k, redisplay](CpuState& s, GuestMemory&) {
        check_elements(s.x[2]);
        picker_of(*k, s.x[0])->elements = s.x[2];
        redisplay(s.x[0]);
    }, "v@:Q");
    kit.method(c, "datePickerStyle", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(picker_of(*k, s.x[0])->style); }, "Q@:");
    kit.method(c, "setDatePickerStyle:", [k, redisplay](CpuState& s, GuestMemory&) {
        check_style(static_cast<std::int64_t>(s.x[2]));
        picker_of(*k, s.x[0])->style = static_cast<std::int64_t>(s.x[2]);
        redisplay(s.x[0]);
    }, "v@:Q");
    kit.method(c, "datePickerMode", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(picker_of(*k, s.x[0])->mode); }, "Q@:");
    kit.method(c, "setDatePickerMode:", [k](CpuState& s, GuestMemory&) { picker_of(*k, s.x[0])->mode = static_cast<std::int64_t>(s.x[2]); }, "v@:Q");
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, Address PickerState::*>>{
             {"calendar", "setCalendar:", &PickerState::calendar}, {"locale", "setLocale:", &PickerState::locale},
             {"timeZone", "setTimeZone:", &PickerState::time_zone}}) {
        const auto field = member;
        kit.method(c, getter, [k, field](CpuState& s, GuestMemory&) { s.x[0] = (*picker_of(*k, s.x[0])).*field; });
        kit.method(c, setter, [k, field, redisplay](CpuState& s, GuestMemory&) {
            k->assign((*picker_of(*k, s.x[0])).*field, s.x[2]);
            redisplay(s.x[0]);
        }, "v@:@");
    }
    kit.method(c, "delegate", [k](CpuState& s, GuestMemory&) { s.x[0] = picker_of(*k, s.x[0])->delegate; });
    kit.method(c, "setDelegate:", [k](CpuState& s, GuestMemory&) { picker_of(*k, s.x[0])->delegate = s.x[2]; }, "v@:@");
    kit.method(c, "cellSizeForBounds:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = picker_of(*k, self);
        const auto style = cell_text_style(*k, self, theme(false));
        const auto text = formatted(*k, *state).text;
        double width = std::ceil(drawing::text_width(style.font, text)) + 12;
        if (state->style == picker_style::text_field_and_stepper) width += stepper_width + 2;
        const auto bounds = rect_arg(s);
        return_size(s, {std::min(width, bounds.size.width), std::min(std::ceil(drawing::font_metrics(style.font).line_height()) + 8, bounds.size.height)});
    }, "{CGSize=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(c, "drawWithFrame:inView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto cell = cell_state(*k, self);
        const auto state = picker_of(*k, self);
        check_style(state->style);
        const auto frame = rect_arg(s);
        const auto& th = k->theme_for(owner);
        auto& canvas = k->require_canvas("NSDatePickerCell drawing");
        const auto field_rect = text_area(frame, *state);
        if (cell->bezeled || cell->bordered) {
            const auto background = cell->draws_background ? k->color_value(cell->background_color, th.text_background) : Color{0, 0, 0, 0};
            draw_bezel(*k, field_rect, 0, background, th.control_border);
        } else if (cell->draws_background) {
            canvas.fill_rect(field_rect, k->color_value(cell->background_color, th.text_background));
        }
        auto style = cell_text_style(*k, self, th);
        const auto shown = formatted(*k, *state);
        const Rect text_rect{{field_rect.origin.x + 4, field_rect.origin.y}, {std::max(0.0, field_rect.size.width - 8), field_rect.size.height}};
        // The selected field while the picker is first responder.
        const auto window_object = owner ? k->view_state(owner)->window : 0;
        const bool focused = window_object && k->window_state(window_object)->first_responder == owner;
        if (focused && state->field >= 0 && state->field < static_cast<std::int64_t>(shown.fields.size())) {
            const auto& field = shown.fields[static_cast<std::size_t>(state->field)];
            const auto widths = drawing::prefix_widths(style.font, shown.text);
            const auto at = [&](std::size_t n) { return n ? widths[std::min(n, widths.size()) - 1] : 0.0; };
            const auto metrics = drawing::font_metrics(style.font);
            const double height = metrics.ascent + metrics.descent + 2;
            canvas.fill_rect({{text_rect.origin.x + at(field.start) - 1, text_rect.origin.y + (text_rect.size.height - height) / 2},
                              {at(field.start + field.length) - at(field.start) + 2, height}}, th.selection.with_alpha(th.dark ? 0.75 : 0.35));
        }
        draw_line_in(*k, shown.text, style, text_rect, 0, false);
        if (state->style == picker_style::text_field_and_stepper) draw_stepper(canvas, *k, stepper_area(frame), th, cell->enabled);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // ---- NSDatePicker ----
    const auto n = control_class;
    for (const auto& [name, types, redraw] : std::initializer_list<std::tuple<const char*, const char*, bool>>{
             {"dateValue", "@@:", false}, {"setDateValue:", "v@:@", true}, {"minDate", "@@:", false}, {"setMinDate:", "v@:@", true},
             {"maxDate", "@@:", false}, {"setMaxDate:", "v@:@", true}, {"timeInterval", "d@:", false}, {"setTimeInterval:", "v@:d", false},
             {"datePickerElements", "Q@:", false}, {"setDatePickerElements:", "v@:Q", true}, {"datePickerStyle", "Q@:", false},
             {"setDatePickerStyle:", "v@:Q", true}, {"datePickerMode", "Q@:", false}, {"setDatePickerMode:", "v@:Q", false},
             {"calendar", "@@:", false}, {"setCalendar:", "v@:@", true}, {"locale", "@@:", false}, {"setLocale:", "v@:@", true},
             {"timeZone", "@@:", false}, {"setTimeZone:", "v@:@", true}, {"textColor", "@@:", false}, {"setTextColor:", "v@:@", true},
             {"backgroundColor", "@@:", false}, {"setBackgroundColor:", "v@:@", true}, {"drawsBackground", "B@:", false},
             {"setDrawsBackground:", "v@:B", true}, {"isBezeled", "B@:", false}, {"setBezeled:", "v@:B", true},
             {"isBordered", "B@:", false}, {"setBordered:", "v@:B", true}, {"delegate", "@@:", false}, {"setDelegate:", "v@:@", false}})
        forward_to_cell(kit, n, name, types, redraw);
    kit.method(n, "acceptsFirstResponder", [k](CpuState& s, GuestMemory&) {
        const auto cell = control_cell(*k, s.x[0]);
        return_bool(s, cell && cell_state(*k, cell)->enabled && !k->extension<ControlState>(s.x[0])->refuses_first_responder);
    }, "B@:");
    kit.method(n, "becomeFirstResponder", [k](CpuState& s, GuestMemory&) {
        if (const auto cell = control_cell(*k, s.x[0])) {
            const auto state = picker_of(*k, cell);
            if (state->field < 0) state->field = 0;
            state->pending = 0;
        }
        k->invalidate_view(s.x[0]);
        s.x[0] = 1;
    }, "B@:");
    kit.method(n, "resignFirstResponder", [k](CpuState& s, GuestMemory&) {
        if (const auto cell = control_cell(*k, s.x[0])) picker_of(*k, cell)->pending = 0;
        k->invalidate_view(s.x[0]);
        s.x[0] = 1;
    }, "B@:");
    kit.method(n, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event = s.x[2];
        const auto cell = control_cell(*k, self);
        if (!cell || !cell_state(*k, cell)->enabled) return;
        const auto state = picker_of(*k, cell);
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const auto location = event_location_in(*k, self, event);
        if (const auto owner = k->view_state(self)->window)
            if (k->window_state(owner)->first_responder != self) static_cast<void>(k->send_id(owner, "makeFirstResponder:", {self}));
        if (state->style == picker_style::text_field_and_stepper && rect_contains(stepper_area(bounds), location)) {
            const auto area = stepper_area(bounds);
            const bool upper = location.y < area.origin.y + area.size.height / 2;   // flipped view: smaller y is the top
            step_field(*k, cell, upper ? 1 : -1);
            static_cast<void>(track_mouse(*k, self, event, [](bool) {}));
            return;
        }
        const auto shown = formatted(*k, *state);
        const auto style = cell_text_style(*k, cell, k->theme_for(self));
        const auto widths = drawing::prefix_widths(style.font, shown.text);
        const double x = location.x - (bounds.origin.x + 4);
        std::size_t index = 0;
        while (index < widths.size() && widths[index] < x) ++index;
        state->field = elements::field_at(shown, index);
        state->pending = 0;
        k->invalidate_view(self);
    }, "v@:@");
    kit.method(n, "keyDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto cell = control_cell(*k, self);
        if (!cell) return;
        const auto state = picker_of(*k, cell);
        const auto info = k->event_state(s.x[2]);
        const auto code = info->event.key_code;
        const auto shown = formatted(*k, *state);
        const auto count = static_cast<std::int64_t>(shown.fields.size());
        if (code == 0x7e || code == 0x7d) { step_field(*k, cell, code == 0x7e ? 1 : -1); return; }   // up / down
        if (code == 0x7b || code == 0x7c) {                                                          // left / right
            if (count) state->field = std::clamp<std::int64_t>(state->field + (code == 0x7b ? -1 : 1), 0, count - 1);
            state->pending = 0;
            k->invalidate_view(self);
            return;
        }
        const auto& characters = info->event.characters;
        if (characters.size() == 1 && count) {
            const char typed = characters[0];
            if (state->field < 0 || state->field >= count) state->field = 0;
            const auto kind = shown.fields[static_cast<std::size_t>(state->field)].kind;
            auto value = civil_of(*k, *state, state->date);
            if (typed >= '0' && typed <= '9') {
                const auto result = elements::type_digit(value, kind, state->pending, typed - '0');
                propose(*k, cell, reference_of(*k, *state, result.value, state->date));
                state->pending = result.pending;
                if (result.complete && state->field + 1 < count) state->field += 1;
                k->invalidate_view(self);
                return;
            }
            if (kind == elements::Field::am_pm && (typed == 'a' || typed == 'A' || typed == 'p' || typed == 'P')) {
                const bool pm = typed == 'p' || typed == 'P';
                if ((value.hour >= 12) != pm) value = elements::step(value, kind, 1);
                propose(*k, cell, reference_of(*k, *state, value, state->date));
                return;
            }
        }
        // Tab, Return and the rest: the responder chain (the key view loop).
        static_cast<void>(k->send_id(self, "interpretKeyEvents:", {k->auto_array({s.x[2]})}));
    }, "v@:@");
    // Commands interpretKeyEvents: maps (Tab moves to the next key view).
    kit.method(n, "insertTab:", [k](CpuState& s, GuestMemory&) {
        if (const auto owner = k->view_state(s.x[0])->window) static_cast<void>(k->send_id(owner, "selectNextKeyView:", {s.x[0]}));
    }, "v@:@");
    kit.method(n, "insertBacktab:", [k](CpuState& s, GuestMemory&) {
        if (const auto owner = k->view_state(s.x[0])->window) static_cast<void>(k->send_id(owner, "selectPreviousKeyView:", {s.x[0]}));
    }, "v@:@");
    kit.method(n, "doCommandBySelector:", [k](CpuState& s, GuestMemory&) {
        if (k->objc.responds(s.x[0], s.x[2])) {
            CallRegs registers;
            registers.x[2] = s.x[0];
            static_cast<void>(k->objc.send(s.x[0], s.x[2], registers));
        }
    }, "v@::");
}

} // namespace alchemy::gui::completion_kit::controls
