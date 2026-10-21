#include "alchemy/darwin/appkit/controls_internal.hpp"

#include <algorithm>
#include <cmath>

// More standard controls on the software drawing layer: NSSliderCell /
// NSSlider (linear, horizontal or vertical, tick marks), NSStepperCell /
// NSStepper and NSComboBoxCell / NSComboBox (an editable text field with a
// popup list of its items, internal list or data source). They draw with the
// flat theme of controls.cpp and track the mouse with its helpers.
namespace alchemy::gui::appkit {
namespace {
using drawing::Color;

Rect inset(Rect r, double dx, double dy) {
    return {{r.origin.x + dx, r.origin.y + dy}, {std::max(0.0, r.size.width - 2 * dx), std::max(0.0, r.size.height - 2 * dy)}};
}

constexpr double combo_button_width = 20;

// A message forwarded from a control to its cell (the AppKit NSControl API
// of these controls is the cell's), with every argument register.
Handler cell_forwarder(AppKit& kit, std::string name, bool redisplay) {
    return [&kit, name = std::move(name), redisplay](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto target = control_cell(kit, self);
        if (!target) { s.x[0] = 0; s.x[1] = 0; return; }
        CallRegs registers;
        for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
        for (unsigned i = 0; i < 8; ++i) registers.v[i] = s.v[i];
        const auto result = kit.send(target, name, registers);
        s.x[0] = result.x0; s.x[1] = result.x1;
        for (unsigned i = 0; i < 4; ++i) s.v[i] = result.v[i];
        if (redisplay) kit.invalidate_view(self);
    };
}
} // namespace

void AppKit::install_more_controls() {
    slider_cell = declare_class("NSSliderCell", "NSActionCell");
    slider = declare_class("NSSlider", "NSControl");
    stepper_cell = declare_class("NSStepperCell", "NSActionCell");
    stepper = declare_class("NSStepper", "NSControl");
    combo_box_cell = declare_class("NSComboBoxCell", "NSTextFieldCell");
    combo_box = declare_class("NSComboBox", "NSTextField");
    static_cast<void>(send_id(slider, "setCellClass:", {slider_cell}));
    static_cast<void>(send_id(stepper, "setCellClass:", {stepper_cell}));
    static_cast<void>(send_id(combo_box, "setCellClass:", {combo_box_cell}));
    for (const auto* name : {"NSComboBoxSelectionDidChangeNotification", "NSComboBoxSelectionIsChangingNotification",
             "NSComboBoxWillPopUpNotification", "NSComboBoxWillDismissNotification"})
        string_constant(std::string("_") + name, name);

    const auto cs = [this](Address value) { return cell_state(*this, value); };
    const auto number_of = [this, cs](Address cell_object) {
        const auto contents = cs(cell_object)->contents;
        if (!contents) return 0.0;
        if (const auto value = cf.number_value(contents)) return *value;
        try { return std::stod(text(contents)); } catch (...) { return 0.0; }
    };
    const auto set_number = [this, cs](Address cell_object, double value) {
        release(std::exchange(cs(cell_object)->contents, cf.real(value)));
        if (const auto owner = cs(cell_object)->control_view; owner && is_kind(owner, view)) invalidate_view(owner);
    };
    const auto real_property = [this, cs](Address cls, const char* getter, const char* setter, double CellState::*member,
                                          std::function<void(Address)> after = {}) {
        method(cls, getter, [cs, member](CpuState& s, GuestMemory&) { set_d(s, 0, (*cs(s.x[0])).*member); }, "d@:");
        method(cls, setter, [this, cs, member, after](CpuState& s, GuestMemory&) {
            (*cs(s.x[0])).*member = d(s, 0);
            if (after) after(s.x[0]);
            if (const auto owner = cs(s.x[0])->control_view; owner && is_kind(owner, view)) invalidate_view(owner);
        }, "v@:d");
    };
    const auto bool_property = [this, cs](Address cls, const char* getter, const char* setter, bool CellState::*member) {
        method(cls, getter, [cs, member](CpuState& s, GuestMemory&) { return_bool(s, (*cs(s.x[0])).*member); }, "B@:");
        method(cls, setter, [this, cs, member](CpuState& s, GuestMemory&) {
            (*cs(s.x[0])).*member = bool_arg(s, 2);
            if (const auto owner = cs(s.x[0])->control_view; owner && is_kind(owner, view)) invalidate_view(owner);
        }, "v@:B");
    };
    const auto forward = [this](Address cls, std::initializer_list<std::tuple<const char*, const char*, bool>> list) {
        for (const auto& [name, types, redisplay] : list) method(cls, name, cell_forwarder(*this, name, redisplay), types);
    };
    // Value setters of the numeric cells keep the value inside [min, max].
    const auto clamp_after_set = [this, cs, number_of, set_number](Address cls) {
        for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"setDoubleValue:", "v@:d"}, {"setFloatValue:", "v@:f"}, {"setIntValue:", "v@:i"},
                 {"setIntegerValue:", "v@:q"}, {"setObjectValue:", "v@:@"}, {"setStringValue:", "v@:@"}}) {
            const std::string selector = name;
            method(cls, selector, [this, cls, selector, cs, number_of, set_number](CpuState& s, GuestMemory&) {
                CallRegs registers;
                for (unsigned i = 2; i < 8; ++i) registers.x[i] = s.x[i];
                for (unsigned i = 0; i < 8; ++i) registers.v[i] = s.v[i];
                static_cast<void>(objc.send_super(s.x[0], cls, sel(selector), registers));
                const auto state = cs(s.x[0]);
                const auto value = number_of(s.x[0]);
                const auto clamped = std::clamp(value, std::min(state->min_value, state->max_value), std::max(state->min_value, state->max_value));
                if (clamped != value || !state->contents || !cf.number_value(state->contents)) set_number(s.x[0], clamped);
            }, types);
        }
    };

    // ---- NSSliderCell ----
    const auto sc = slider_cell;
    method(sc, "init", [this, cs, set_number](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], slider_cell, sel("init")).x0;
        s.x[0] = self;
        const auto state = cs(self);
        state->min_value = 0;
        state->max_value = 1;
        state->continuous = true;
        state->bezeled = state->bordered = false;
        set_number(self, 0);
    });
    clamp_after_set(sc);
    const auto relayout = [this, cs, number_of, set_number](Address self) {
        const auto state = cs(self);
        set_number(self, std::clamp(number_of(self), std::min(state->min_value, state->max_value), std::max(state->min_value, state->max_value)));
    };
    real_property(sc, "minValue", "setMinValue:", &CellState::min_value, relayout);
    real_property(sc, "maxValue", "setMaxValue:", &CellState::max_value, relayout);
    real_property(sc, "altIncrementValue", "setAltIncrementValue:", &CellState::alt_increment);
    bool_property(sc, "allowsTickMarkValuesOnly", "setAllowsTickMarkValuesOnly:", &CellState::ticks_only);
    method(sc, "numberOfTickMarks", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->tick_marks); }, "q@:");
    method(sc, "setNumberOfTickMarks:", [this, cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->tick_marks = std::max<std::int64_t>(0, static_cast<std::int64_t>(s.x[2]));
        if (const auto owner = cs(s.x[0])->control_view) invalidate_view(owner);
    }, "v@:q");
    method(sc, "tickMarkPosition", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->tick_position); }, "Q@:");
    method(sc, "setTickMarkPosition:", [this, cs](CpuState& s, GuestMemory&) {
        cs(s.x[0])->tick_position = static_cast<std::int64_t>(s.x[2]);
        if (const auto owner = cs(s.x[0])->control_view) invalidate_view(owner);
    }, "v@:Q");
    // NSSliderTypeLinear only; a circular slider fails explicitly.
    method(sc, "sliderType", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:");
    method(sc, "setSliderType:", [](CpuState& s, GuestMemory&) {
        if (s.x[2] != 0) unsupported("NSSliderTypeCircular (circular sliders)");
    }, "v@:Q");
    const auto is_vertical = [this, cs](Address self) {
        const auto state = cs(self);
        if (state->vertical >= 0) return state->vertical == 1;
        const auto owner = state->control_view;
        if (!owner || !is_kind(owner, view)) return false;
        const auto frame = view_state(owner)->frame;
        return frame.size.height > frame.size.width;
    };
    method(sc, "isVertical", [is_vertical](CpuState& s, GuestMemory&) { return_bool(s, is_vertical(s.x[0])); }, "B@:");
    method(sc, "setVertical:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->vertical = bool_arg(s, 2) ? 1 : 0; }, "v@:B");
    const auto tick_value = [cs](Address self, std::int64_t index) {
        const auto state = cs(self);
        if (state->tick_marks <= 1) return state->min_value;
        return state->min_value + (state->max_value - state->min_value) * static_cast<double>(index) / static_cast<double>(state->tick_marks - 1);
    };
    const auto closest_tick = [cs, tick_value](Address self, double value) {
        const auto state = cs(self);
        if (state->tick_marks <= 0) return value;
        if (state->tick_marks == 1) return tick_value(self, 0);
        const auto range = state->max_value - state->min_value;
        const auto step = range / static_cast<double>(state->tick_marks - 1);
        if (step == 0) return state->min_value;
        const auto index = std::clamp<double>(std::round((value - state->min_value) / step), 0, static_cast<double>(state->tick_marks - 1));
        return tick_value(self, static_cast<std::int64_t>(index));
    };
    method(sc, "tickMarkValueAtIndex:", [tick_value](CpuState& s, GuestMemory&) { set_d(s, 0, tick_value(s.x[0], static_cast<std::int64_t>(s.x[2]))); }, "d@:q");
    method(sc, "closestTickMarkValueToValue:", [closest_tick](CpuState& s, GuestMemory&) { set_d(s, 0, closest_tick(s.x[0], d(s, 0))); }, "d@:d");
    method(sc, "knobThickness", [](CpuState& s, GuestMemory&) { set_d(s, 0, 20); }, "d@:");
    // The knob's travel inside `frame`: the track runs from `start` over
    // `length` points along the slider's axis (flipped view coordinates).
    struct Travel { double start, length; bool vertical; };
    const auto travel_of = [is_vertical](Address self, Rect frame) {
        const bool vertical = is_vertical(self);
        constexpr double knob = 20;
        const auto extent = vertical ? frame.size.height : frame.size.width;
        return Travel{(vertical ? frame.origin.y : frame.origin.x) + knob / 2, std::max(0.0, extent - knob), vertical};
    };
    const auto fraction_of = [cs, number_of](Address self) {
        const auto state = cs(self);
        const auto range = state->max_value - state->min_value;
        return range != 0 ? std::clamp((number_of(self) - state->min_value) / range, 0.0, 1.0) : 0.0;
    };
    method(sc, "knobRectFlipped:", [this, travel_of, fraction_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto owner = cell_state(*this, self)->control_view;
        const auto frame = owner && is_kind(owner, view) ? view_bounds(*view_state(owner)) : Rect{};
        const auto t = travel_of(self, frame);
        const auto fraction = fraction_of(self);
        const auto along = t.vertical ? t.start + (1 - fraction) * t.length : t.start + fraction * t.length;
        return_rect(s, t.vertical ? Rect{{frame.origin.x + (frame.size.width - 20) / 2, along - 10}, {20, 20}}
                                  : Rect{{along - 10, frame.origin.y + (frame.size.height - 20) / 2}, {20, 20}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:B");
    // The bar the knob travels along (the track as drawn below; the knob's
    // centre stays inside it). The rect is in the control view's coordinates.
    method(sc, "barRectFlipped:", [this, travel_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto owner = cell_state(*this, self)->control_view;
        const auto frame = owner && is_kind(owner, view) ? view_bounds(*view_state(owner)) : Rect{};
        const auto t = travel_of(self, frame);
        constexpr double thickness = 4;
        return_rect(s, t.vertical ? Rect{{frame.origin.x + frame.size.width / 2 - thickness / 2, t.start}, {thickness, t.length}}
                                  : Rect{{t.start, frame.origin.y + frame.size.height / 2 - thickness / 2}, {t.length, thickness}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:B");
    method(sc, "cellSizeForBounds:", [this](CpuState& s, GuestMemory&) {
        const auto bounds = rect_arg(s);
        const bool vertical = cell_state(*this, s.x[0])->vertical == 1;
        return_size(s, vertical ? Size{24, std::min(bounds.size.height, 100.0)} : Size{std::min(bounds.size.width, 100.0), 24});
    }, "{CGSize=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(sc, "drawWithFrame:inView:", [this, cs, travel_of, fraction_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(s.x[2]);
        auto& canvas = require_canvas("NSSliderCell drawing");
        const auto t = travel_of(self, frame);
        const auto fraction = fraction_of(self);
        constexpr double thickness = 4;
        const auto cross_center = t.vertical ? frame.origin.x + frame.size.width / 2 : frame.origin.y + frame.size.height / 2;
        const auto along = t.vertical ? t.start + (1 - fraction) * t.length : t.start + fraction * t.length;
        const auto along_rect = [&](double from, double to) {
            const auto a = std::min(from, to), b = std::max(from, to);
            return t.vertical ? Rect{{cross_center - thickness / 2, a}, {thickness, b - a}}
                              : Rect{{a, cross_center - thickness / 2}, {b - a, thickness}};
        };
        drawing::Path track;
        const auto whole = along_rect(t.start, t.start + t.length);
        track.add_rounded_rect(whole, thickness / 2, thickness / 2);
        canvas.fill_path(track, th.separator);
        // The filled part runs from the minimum end (left, or the bottom of
        // a vertical slider) to the knob.
        const auto filled = t.vertical ? along_rect(along, t.start + t.length) : along_rect(t.start, along);
        if (state->enabled && filled.size.width > 0 && filled.size.height > 0) {
            drawing::Path fill;
            fill.add_rounded_rect(filled, thickness / 2, thickness / 2);
            canvas.fill_path(fill, th.accent);
        }
        // Tick marks below a horizontal slider (NSTickMarkPositionBelow) or
        // above it (NSTickMarkPositionAbove = 1); right / left of a vertical one.
        if (state->tick_marks > 0) {
            const bool before = state->tick_position == 1;
            for (std::int64_t i = 0; i < state->tick_marks; ++i) {
                const auto position = state->tick_marks == 1 ? t.start + t.length / 2
                                                             : t.start + t.length * static_cast<double>(i) / static_cast<double>(state->tick_marks - 1);
                const auto at = t.vertical ? t.start + t.length - (position - t.start) : position;
                const auto offset = before ? -9.0 : 7.0;
                if (t.vertical) canvas.fill_rect({{cross_center + (before ? -9.0 : 7.0), std::round(at)}, {2, 1}}, th.secondary_text);
                else canvas.fill_rect({{std::round(at), cross_center + offset}, {1, 2}}, th.secondary_text);
            }
        }
        // The knob.
        const Rect knob = t.vertical ? Rect{{cross_center - 10, along - 10}, {20, 20}} : Rect{{along - 10, cross_center - 10}, {20, 20}};
        drawing::Path outer;
        outer.add_oval(knob);
        canvas.fill_path(outer, th.control_border);
        drawing::Path inner;
        inner.add_oval(inset(knob, 0.5, 0.5));
        canvas.fill_path(inner, state->highlighted ? th.control_face_pressed : (state->enabled ? Color::gray(1) : th.control_background));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // ---- NSSlider ----
    const auto sl = slider;
    forward(sl, {{"minValue", "d@:", false}, {"setMinValue:", "v@:d", true}, {"maxValue", "d@:", false},
                 {"setMaxValue:", "v@:d", true}, {"altIncrementValue", "d@:", false}, {"setAltIncrementValue:", "v@:d", false},
                 {"numberOfTickMarks", "q@:", false}, {"setNumberOfTickMarks:", "v@:q", true},
                 {"tickMarkPosition", "Q@:", false}, {"setTickMarkPosition:", "v@:Q", true},
                 {"allowsTickMarkValuesOnly", "B@:", false}, {"setAllowsTickMarkValuesOnly:", "v@:B", false},
                 {"tickMarkValueAtIndex:", "d@:q", false}, {"closestTickMarkValueToValue:", "d@:d", false},
                 {"isVertical", "B@:", false}, {"setVertical:", "v@:B", true}, {"sliderType", "Q@:", false},
                 {"setSliderType:", "v@:Q", true}, {"knobThickness", "d@:", false}});
    method(sl, "initWithFrame:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = objc.send_super(s.x[0], slider, sel("initWithFrame:"), registers).x0;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    for (const auto* name : {"sliderWithTarget:action:", "sliderWithValue:minValue:maxValue:target:action:"}) {
        const std::string selector = name;
        class_method(sl, selector, [this, selector](CpuState& s, GuestMemory&) {
            const bool valued = selector.starts_with("sliderWithValue");
            const auto value = d(s, 0), low = d(s, 1), high = d(s, 2);
            const auto made = send(send(s.x[0], "alloc").x0, "initWithFrame:", [] { CallRegs r; put_rect(r, 0, {{0, 0}, {96, 24}}); return r; }()).x0;
            if (valued) {
                CallRegs r;
                r.set_d(0, low); static_cast<void>(send(made, "setMinValue:", r));
                r.set_d(0, high); static_cast<void>(send(made, "setMaxValue:", r));
                r.set_d(0, value); static_cast<void>(send(made, "setDoubleValue:", r));
            }
            static_cast<void>(send_id(made, "setTarget:", {s.x[2]}));
            static_cast<void>(send_id(made, "setAction:", {s.x[3]}));
            s.x[0] = autorelease(made);
        });
    }
    method(sl, "acceptsFirstMouse:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    // Dragging: the value follows the pointer along the track (snapped to
    // tick marks when only their values are allowed); a continuous slider
    // sends its action on every change, any slider on the release.
    method(sl, "mouseDown:", [this, cs, travel_of, closest_tick, set_number, number_of](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event_object = s.x[2];
        const auto target = control_cell(*this, self);
        if (!target || !cs(target)->enabled) return;
        const auto state = cs(target);
        const auto frame = view_bounds(*view_state(self));
        const auto t = travel_of(target, frame);
        const auto update = [&](Point location) {
            const auto along = t.vertical ? location.y : location.x;
            auto fraction = t.length > 0 ? std::clamp((along - t.start) / t.length, 0.0, 1.0) : 0.0;
            if (t.vertical) fraction = 1 - fraction;   // flipped: the maximum is at the top
            auto value = state->min_value + fraction * (state->max_value - state->min_value);
            if (state->ticks_only) value = closest_tick(target, value);
            if (value == number_of(target)) return;
            set_number(target, value);
            if (state->continuous) static_cast<void>(send_id(self, "sendAction:to:", {state->action, state->target}));
        };
        update(event_location_in(*this, self, event_object));
        static_cast<void>(track_mouse(*this, self, event_object, [&](bool) { state->highlighted = true; invalidate_view(self); }, update));
        state->highlighted = false;
        invalidate_view(self);
        static_cast<void>(send_id(self, "sendAction:to:", {state->action, state->target}));
    }, "v@:@");

    // ---- NSStepperCell ----
    const auto tc = stepper_cell;
    method(tc, "init", [this, cs, set_number](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], stepper_cell, sel("init")).x0;
        s.x[0] = self;
        const auto state = cs(self);
        state->min_value = 0;
        state->max_value = 59;
        state->increment = 1;
        state->value_wraps = true;
        state->autorepeat = true;
        state->bezeled = state->bordered = false;
        set_number(self, 0);
    });
    clamp_after_set(tc);
    real_property(tc, "minValue", "setMinValue:", &CellState::min_value, relayout);
    real_property(tc, "maxValue", "setMaxValue:", &CellState::max_value, relayout);
    real_property(tc, "increment", "setIncrement:", &CellState::increment);
    bool_property(tc, "valueWraps", "setValueWraps:", &CellState::value_wraps);
    bool_property(tc, "autorepeat", "setAutorepeat:", &CellState::autorepeat);
    // One step up (+1) or down (-1), wrapping or stopping at the limits.
    method(tc, "_alchemyStep:", [cs, number_of, set_number](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = cs(self);
        const auto direction = static_cast<std::int64_t>(s.x[2]) > 0 ? 1.0 : -1.0;
        auto value = number_of(self) + direction * state->increment;
        if (value > state->max_value) value = state->value_wraps ? state->min_value : state->max_value;
        if (value < state->min_value) value = state->value_wraps ? state->max_value : state->min_value;
        set_number(self, value);
    }, "v@:q");
    method(tc, "cellSizeForBounds:", [](CpuState& s, GuestMemory&) { return_size(s, {19, 28}); }, "{CGSize=dd}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(tc, "drawWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(s.x[2]);
        auto& canvas = require_canvas("NSStepperCell drawing");
        const double width = std::min(frame.size.width, 15.0), height = std::min(frame.size.height, 24.0);
        const Rect body{{frame.origin.x + std::floor((frame.size.width - width) / 2), frame.origin.y + std::floor((frame.size.height - height) / 2)},
                        {width, height}};
        draw_bezel(*this, body, 4, state->enabled ? th.control_face : th.control_face.with_alpha(0.5), th.control_border);
        const bool flipped = current_flipped();
        const auto middle = body.origin.y + body.size.height / 2;
        canvas.fill_rect({{body.origin.x + 2, middle}, {body.size.width - 4, 1}}, th.separator);
        // Chevrons: up in the top half, down in the bottom half.
        const auto chevron = [&](bool up) {
            const auto cx = body.origin.x + body.size.width / 2;
            const auto half = body.size.height / 4;
            const auto top_half = flipped ? up : !up;
            const auto cy = top_half ? (flipped ? body.origin.y + half : middle + half) : (flipped ? middle + half : body.origin.y + half);
            const double dir = (up == flipped) ? -1 : 1;   // the tip points up on screen for `up`
            drawing::Path mark;
            mark.move_to({cx - 3, cy - dir * 1.5});
            mark.line_to({cx, cy + dir * 1.5});
            mark.line_to({cx + 3, cy - dir * 1.5});
            canvas.stroke_path(mark, state->enabled ? th.text : th.disabled_text, 1.4);
        };
        chevron(true);
        chevron(false);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // ---- NSStepper ----
    const auto st = stepper;
    forward(st, {{"minValue", "d@:", false}, {"setMinValue:", "v@:d", true}, {"maxValue", "d@:", false},
                 {"setMaxValue:", "v@:d", true}, {"increment", "d@:", false}, {"setIncrement:", "v@:d", false},
                 {"valueWraps", "B@:", false}, {"setValueWraps:", "v@:B", false}, {"autorepeat", "B@:", false},
                 {"setAutorepeat:", "v@:B", false}});
    method(st, "initWithFrame:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = objc.send_super(s.x[0], stepper, sel("initWithFrame:"), registers).x0;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(st, "acceptsFirstMouse:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    // A press in the upper half steps up, in the lower half down; the step
    // and the action happen on the press (AppKit's behaviour); holding the
    // button does not repeat (no autorepeat timer).
    method(st, "mouseDown:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event_object = s.x[2];
        const auto target = control_cell(*this, self);
        if (!target || !cs(target)->enabled) return;
        const auto state = cs(target);
        const auto bounds = view_bounds(*view_state(self));
        const auto location = event_location_in(*this, self, event_object);
        const bool flipped = send_bool(self, "isFlipped");
        const auto from_top = flipped ? location.y - bounds.origin.y : bounds.origin.y + bounds.size.height - location.y;
        const Address direction = from_top < bounds.size.height / 2 ? 1 : static_cast<Address>(-1);
        static_cast<void>(send_id(target, "_alchemyStep:", {direction}));
        static_cast<void>(send_id(self, "sendAction:to:", {state->action, state->target}));
        static_cast<void>(track_mouse(*this, self, event_object, [&](bool inside) { state->highlighted = inside; invalidate_view(self); }));
    }, "v@:@");

    // ---- NSComboBoxCell ----
    const auto cc = combo_box_cell;
    method(cc, "initTextCell:", [this, cs](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        const auto self = objc.send_super(s.x[0], combo_box_cell, sel("initTextCell:"), registers).x0;
        s.x[0] = self;
        cs(self)->trailing_inset = combo_button_width;
    });
    // The items: the internal list, or the data source's.
    const auto item_count = [this, cs](Address self) -> std::int64_t {
        const auto state = cs(self);
        if (!state->uses_data_source) return static_cast<std::int64_t>(state->combo_items.size());
        if (!state->data_source) return 0;
        const auto owner = state->control_view;
        if (owner && responds(state->data_source, "numberOfItemsInComboBox:"))
            return static_cast<std::int64_t>(send_id(state->data_source, "numberOfItemsInComboBox:", {owner}));
        if (responds(state->data_source, "numberOfItemsInComboBoxCell:"))
            return static_cast<std::int64_t>(send_id(state->data_source, "numberOfItemsInComboBoxCell:", {self}));
        return 0;
    };
    const auto item_at = [this, cs](Address self, std::int64_t index) -> Address {
        const auto state = cs(self);
        if (index < 0) return 0;
        if (!state->uses_data_source)
            return index < static_cast<std::int64_t>(state->combo_items.size()) ? state->combo_items[static_cast<std::size_t>(index)] : 0;
        if (!state->data_source) return 0;
        const auto owner = state->control_view;
        if (owner && responds(state->data_source, "comboBox:objectValueForItemAtIndex:"))
            return send_id(state->data_source, "comboBox:objectValueForItemAtIndex:", {owner, static_cast<Address>(index)});
        if (responds(state->data_source, "comboBoxCell:objectValueForItemAtIndex:"))
            return send_id(state->data_source, "comboBoxCell:objectValueForItemAtIndex:", {self, static_cast<Address>(index)});
        return 0;
    };
    const auto item_text = [this](Address value) -> std::string {
        if (!value) return {};
        if (cf.kind(value) == ObjectKind::string) return text(value);
        if (responds(value, "stringValue")) return text(send(value, "stringValue").x0);
        return text(send(value, "description").x0);
    };
    const auto internal_list = [cs](Address self) {
        const auto state = cs(self);
        if (state->uses_data_source) unsupported("NSComboBox internal item list while it uses a data source");
        return state;
    };
    const auto list_changed = [this, cs](Address self) {
        if (const auto owner = cs(self)->control_view; owner && is_kind(owner, view)) invalidate_view(owner);
    };
    method(cc, "addItemWithObjectValue:", [this, internal_list, list_changed](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[NSComboBox addItemWithObjectValue:] with nil");
        internal_list(s.x[0])->combo_items.push_back(retain(s.x[2]));
        list_changed(s.x[0]);
    }, "v@:@");
    method(cc, "addItemsWithObjectValues:", [this, internal_list, list_changed](CpuState& s, GuestMemory&) {
        const auto state = internal_list(s.x[0]);
        for (const auto value : items(s.x[2])) state->combo_items.push_back(retain(value));
        list_changed(s.x[0]);
    }, "v@:@");
    method(cc, "insertItemWithObjectValue:atIndex:", [this, internal_list, list_changed](CpuState& s, GuestMemory&) {
        const auto state = internal_list(s.x[0]);
        if (!s.x[2] || s.x[3] > state->combo_items.size()) fail("-[NSComboBox insertItemWithObjectValue:atIndex:] out of range");
        state->combo_items.insert(state->combo_items.begin() + static_cast<std::ptrdiff_t>(s.x[3]), retain(s.x[2]));
        if (state->combo_selected >= static_cast<std::int64_t>(s.x[3])) ++state->combo_selected;
        list_changed(s.x[0]);
    }, "v@:@q");
    method(cc, "removeItemAtIndex:", [this, internal_list, list_changed](CpuState& s, GuestMemory&) {
        const auto state = internal_list(s.x[0]);
        if (s.x[2] >= state->combo_items.size()) fail("-[NSComboBox removeItemAtIndex:] out of range");
        const auto index = static_cast<std::int64_t>(s.x[2]);
        release(state->combo_items[static_cast<std::size_t>(index)]);
        state->combo_items.erase(state->combo_items.begin() + index);
        if (state->combo_selected == index) state->combo_selected = -1;
        else if (state->combo_selected > index) --state->combo_selected;
        list_changed(s.x[0]);
    }, "v@:q");
    method(cc, "removeItemWithObjectValue:", [this, internal_list](CpuState& s, GuestMemory&) {
        const auto state = internal_list(s.x[0]);
        for (std::size_t i = 0; i < state->combo_items.size(); ++i)
            if (send_bool(state->combo_items[i], "isEqual:", {s.x[2]})) {
                static_cast<void>(send_id(s.x[0], "removeItemAtIndex:", {static_cast<Address>(i)}));
                return;
            }
    }, "v@:@");
    method(cc, "removeAllItems", [this, internal_list, list_changed](CpuState& s, GuestMemory&) {
        const auto state = internal_list(s.x[0]);
        for (const auto item : std::exchange(state->combo_items, {})) release(item);
        state->combo_selected = -1;
        list_changed(s.x[0]);
    }, "v@:");
    method(cc, "objectValues", [this, internal_list](CpuState& s, GuestMemory&) { s.x[0] = auto_array(internal_list(s.x[0])->combo_items); });
    method(cc, "numberOfItems", [item_count](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(item_count(s.x[0])); }, "q@:");
    method(cc, "itemObjectValueAtIndex:", [item_at, item_count](CpuState& s, GuestMemory&) {
        const auto index = static_cast<std::int64_t>(s.x[2]);
        if (index < 0 || index >= item_count(s.x[0])) fail("-[NSComboBox itemObjectValueAtIndex:] out of range");
        s.x[0] = item_at(s.x[0], index);
    }, "@@:q");
    method(cc, "indexOfItemWithObjectValue:", [this, item_at, item_count](CpuState& s, GuestMemory&) {
        const auto count = item_count(s.x[0]);
        for (std::int64_t i = 0; i < count; ++i)
            if (const auto value = item_at(s.x[0], i); value && send_bool(value, "isEqual:", {s.x[2]})) { s.x[0] = static_cast<Address>(i); return; }
        s.x[0] = not_found;
    }, "q@:@");
    method(cc, "indexOfSelectedItem", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->combo_selected); }, "q@:");
    method(cc, "objectValueOfSelectedItem", [this, cs, item_at, internal_list](CpuState& s, GuestMemory&) {
        static_cast<void>(internal_list(s.x[0]));
        s.x[0] = item_at(s.x[0], cs(s.x[0])->combo_selected);
    });
    // Selecting an item shows it in the field and posts the selection
    // notifications (to the control's delegate too).
    method(cc, "selectItemAtIndex:", [this, cs, item_at, item_count, item_text](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = cs(self);
        const auto index = static_cast<std::int64_t>(s.x[2]);
        if (index < -1 || index >= item_count(self)) fail("-[NSComboBox selectItemAtIndex:] out of range");
        if (state->combo_selected == index) return;
        const auto owner = state->control_view;
        const auto delegate = owner && is_kind(owner, control) ? extension<ControlState>(owner)->delegate : 0;
        if (owner) post("NSComboBoxSelectionIsChangingNotification", owner, delegate, "comboBoxSelectionIsChanging:");
        state->combo_selected = index;
        if (index >= 0) {
            const auto shown = item_text(item_at(self, index));
            release(std::exchange(state->contents, string(shown)));
            if (owner && is_kind(owner, control))
                if (const auto editor = extension<ControlState>(owner)->editor)
                    static_cast<void>(send_id(editor, "setString:", {auto_string(shown)}));
        }
        if (owner) {
            invalidate_view(owner);
            post("NSComboBoxSelectionDidChangeNotification", owner, delegate, "comboBoxSelectionDidChange:");
        }
    }, "v@:q");
    method(cc, "selectItemWithObjectValue:", [this](CpuState& s, GuestMemory&) {
        const auto index = send_id(s.x[0], "indexOfItemWithObjectValue:", {s.x[2]});
        static_cast<void>(send_id(s.x[0], "selectItemAtIndex:", {index == not_found ? static_cast<Address>(-1) : index}));
    }, "v@:@");
    method(cc, "deselectItemAtIndex:", [cs](CpuState& s, GuestMemory&) {
        if (cs(s.x[0])->combo_selected == static_cast<std::int64_t>(s.x[2])) cs(s.x[0])->combo_selected = -1;
    }, "v@:q");
    bool_property(cc, "usesDataSource", "setUsesDataSource:", &CellState::uses_data_source);
    bool_property(cc, "completes", "setCompletes:", &CellState::completes);
    bool_property(cc, "isButtonBordered", "setButtonBordered:", &CellState::button_bordered);
    bool_property(cc, "hasVerticalScroller", "setHasVerticalScroller:", &CellState::has_vertical_scroller);
    method(cc, "dataSource", [cs](CpuState& s, GuestMemory&) { s.x[0] = cs(s.x[0])->data_source; });
    method(cc, "setDataSource:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->data_source = s.x[2]; }, "v@:@");
    method(cc, "numberOfVisibleItems", [cs](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(cs(s.x[0])->visible_items); }, "q@:");
    method(cc, "setNumberOfVisibleItems:", [cs](CpuState& s, GuestMemory&) { cs(s.x[0])->visible_items = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    real_property(cc, "itemHeight", "setItemHeight:", &CellState::item_height);
    method(cc, "noteNumberOfItemsChanged", [list_changed](CpuState& s, GuestMemory&) { list_changed(s.x[0]); }, "v@:");
    method(cc, "reloadData", [list_changed](CpuState& s, GuestMemory&) { list_changed(s.x[0]); }, "v@:");
    method(cc, "scrollItemAtIndexToTop:", [](CpuState&, GuestMemory&) {}, "v@:q");        // the list is not open
    method(cc, "scrollItemAtIndexToVisible:", [](CpuState&, GuestMemory&) {}, "v@:q");
    // -completedString: the first item starting with the typed prefix.
    method(cc, "completedString:", [this, item_at, item_count, item_text](CpuState& s, GuestMemory&) {
        const auto prefix = text(s.x[2]);
        const auto count = item_count(s.x[0]);
        for (std::int64_t i = 0; i < count && !prefix.empty(); ++i) {
            const auto candidate = item_text(item_at(s.x[0], i));
            if (candidate.size() >= prefix.size() && candidate.compare(0, prefix.size(), prefix) == 0) { s.x[0] = auto_string(candidate); return; }
        }
        s.x[0] = s.x[2];
    });
    method(cc, "drawWithFrame:inView:", [this, cs](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], owner = s.x[2];
        const auto state = cs(self);
        const auto frame = rect_arg(s);
        const auto& th = theme_for(owner);
        auto& canvas = require_canvas("NSComboBoxCell drawing");
        // The text field part, then the popup button at the trailing edge.
        CallRegs registers;
        put_rect(registers, 0, {frame.origin, {std::max(0.0, frame.size.width - combo_button_width + 1), frame.size.height}});
        registers.x[2] = owner;
        static_cast<void>(objc.send_super(self, combo_box_cell, sel("drawWithFrame:inView:"), registers));
        const Rect button_rect{{frame.origin.x + frame.size.width - combo_button_width, frame.origin.y}, {combo_button_width, frame.size.height}};
        draw_bezel(*this, button_rect, 4, state->highlighted ? th.accent.with_alpha(0.8) : th.accent, state->button_bordered ? th.accent : th.accent);
        const auto cx = button_rect.origin.x + button_rect.size.width / 2;
        const auto cy = button_rect.origin.y + button_rect.size.height / 2;
        const double dir = current_flipped() ? 1 : -1;
        drawing::Path chevron;
        chevron.move_to({cx - 3.5, cy - dir * 1.5});
        chevron.line_to({cx, cy + dir * 2});
        chevron.line_to({cx + 3.5, cy - dir * 1.5});
        canvas.stroke_path(chevron, state->enabled ? th.accent_text : th.accent_text.with_alpha(0.5), 1.5);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // ---- NSComboBox ----
    const auto cb = combo_box;
    forward(cb, {{"addItemWithObjectValue:", "v@:@", true}, {"addItemsWithObjectValues:", "v@:@", true},
                 {"insertItemWithObjectValue:atIndex:", "v@:@q", true}, {"removeItemAtIndex:", "v@:q", true},
                 {"removeItemWithObjectValue:", "v@:@", true}, {"removeAllItems", "v@:", true}, {"objectValues", "@@:", false},
                 {"numberOfItems", "q@:", false}, {"itemObjectValueAtIndex:", "@@:q", false},
                 {"indexOfItemWithObjectValue:", "q@:@", false}, {"indexOfSelectedItem", "q@:", false},
                 {"objectValueOfSelectedItem", "@@:", false}, {"selectItemAtIndex:", "v@:q", true},
                 {"selectItemWithObjectValue:", "v@:@", true}, {"deselectItemAtIndex:", "v@:q", true},
                 {"usesDataSource", "B@:", false}, {"setUsesDataSource:", "v@:B", true}, {"completes", "B@:", false},
                 {"setCompletes:", "v@:B", false}, {"isButtonBordered", "B@:", false}, {"setButtonBordered:", "v@:B", true},
                 {"hasVerticalScroller", "B@:", false}, {"setHasVerticalScroller:", "v@:B", false},
                 {"dataSource", "@@:", false}, {"setDataSource:", "v@:@", true}, {"numberOfVisibleItems", "q@:", false},
                 {"setNumberOfVisibleItems:", "v@:q", false}, {"itemHeight", "d@:", false}, {"setItemHeight:", "v@:d", false},
                 {"noteNumberOfItemsChanged", "v@:", true}, {"reloadData", "v@:", true},
                 {"scrollItemAtIndexToTop:", "v@:q", false}, {"scrollItemAtIndexToVisible:", "v@:q", false}});
    // A press on the button pops the item list up (a menu window over the
    // field); choosing an item selects it and sends the action. A press on
    // the text part edits like a text field.
    method(cb, "mouseDown:", [this, cs, item_at, item_count, item_text](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], event_object = s.x[2];
        const auto target = control_cell(*this, self);
        if (!target) return;
        const auto state = cs(target);
        const auto bounds = view_bounds(*view_state(self));
        const auto location = event_location_in(*this, self, event_object);
        if (location.x < bounds.origin.x + bounds.size.width - combo_button_width) {
            CallRegs registers;
            registers.x[2] = event_object;
            static_cast<void>(objc.send_super(self, combo_box, sel("mouseDown:"), registers));
            return;
        }
        if (!state->enabled) return;
        const auto count = item_count(target);
        if (count <= 0) return;
        const auto delegate = extension<ControlState>(self)->delegate;
        post("NSComboBoxWillPopUpNotification", self, delegate, "comboBoxWillPopUp:");
        const auto list = alloc_init(menu);
        std::int64_t chosen = -1;
        try {
            for (std::int64_t i = 0; i < count; ++i)
                static_cast<void>(send_id(list, "addItemWithTitle:action:keyEquivalent:",
                                          {auto_string(item_text(item_at(target, i))), 0, auto_string("")}));
            state->highlighted = true;
            invalidate_view(self);
            if (const auto owner = view_state(self)->window) display_window(owner);
            chosen = popup_menu_tracking(list, self, bounds, state->combo_selected);
        } catch (...) {
            state->highlighted = false;
            release(list);
            throw;
        }
        state->highlighted = false;
        release(list);
        invalidate_view(self);
        post("NSComboBoxWillDismissNotification", self, delegate, "comboBoxWillDismiss:");
        if (chosen < 0) return;
        static_cast<void>(send_id(target, "selectItemAtIndex:", {static_cast<Address>(chosen)}));
        static_cast<void>(send_id(self, "sendAction:to:", {state->action, state->target}));
    }, "v@:@");
}

} // namespace alchemy::gui::appkit
