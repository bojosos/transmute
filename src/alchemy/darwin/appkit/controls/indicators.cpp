#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <tuple>
#include <utility>

// NSSwitch and NSLevelIndicator / NSLevelIndicatorCell, drawn on AppKit's
// software layer.
//
// NSSwitch (a cell-less control): -state (on or off), a pill with a round
// knob, the accent colour while on; a click (released over the switch)
// toggles it and sends the action. Disabled switches are drawn faded and
// ignore clicks. Its intrinsic size is 38 x 22.
//
// NSLevelIndicator forwards its API to its NSLevelIndicatorCell, whose value
// is the cell's value. The styles: relevancy (a row of thin bars), continuous
// capacity (a bar filled to the value), discrete capacity (one segment per
// unit of the range) and rating (stars, or the rating images when set, with
// placeholders after the value while editable). The fill is the warning or
// critical colour once the value passes those levels (inverted when the
// critical value is below the warning value, as AppKit does), and tick marks
// (major ones longer) are drawn below or above. An editable indicator takes
// its value from a click or drag and sends the action. The default range is
// 0 to 2 (unverified against AppKit).
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

// ---- NSSwitch ----
struct SwitchState final : ControlState {
    bool on{}, enabled{true}, pressed{};
};
std::shared_ptr<SwitchState> switch_of(AppKit& kit, Address view) {
    const auto state = kit.view_state(view);
    if (!state->extension) state->extension = std::make_shared<SwitchState>();
    auto typed = std::dynamic_pointer_cast<SwitchState>(state->extension);
    if (!typed) fail("NSSwitch state of another type");
    return typed;
}

// ---- NSLevelIndicatorCell ----
namespace style {
inline constexpr std::int64_t relevancy = 0, continuous = 1, discrete = 2, rating = 3;
}
struct LevelState final : ObjectPayload {
    double minimum{0}, maximum{2}, warning{0}, critical{0};
    std::int64_t style{style::continuous}, ticks{}, major_ticks{}, tick_position{};   // NSTickMarkPositionBelow
    Address fill{}, warning_fill{}, critical_fill{}, rating_image{}, placeholder_image{};   // +1
    bool tiered{};
};
std::shared_ptr<LevelState> level(AppKit& kit, Address cell) { return cell_extension<LevelState>(kit, cell); }

double cell_value(AppKit& kit, Address cell) { return kit.send(cell, "doubleValue").d(0); }

Color fill_for(AppKit& kit, const LevelState& state, double value) {
    const Color normal{0.2, 0.74, 0.3, 1}, warn{1.0, 0.78, 0.1, 1}, crit{0.93, 0.25, 0.2, 1};
    const bool inverted = state.critical < state.warning;
    const auto passes = [&](double level) { return level != 0 && (inverted ? value <= level : value >= level); };
    if (passes(state.critical)) return kit.color_value(state.critical_fill, crit);
    if (passes(state.warning)) return kit.color_value(state.warning_fill, warn);
    return kit.color_value(state.fill, normal);
}

// The bar area inside the cell frame, leaving room for tick marks.
Rect bar_rect(const LevelState& state, Rect frame, bool flipped) {
    if (state.ticks <= 0 || state.style == style::rating) return frame;
    const double room = 6;
    const bool below = state.tick_position == 0;
    // Below in a flipped view is towards larger y.
    const bool shift = below == flipped;
    return {{frame.origin.x, frame.origin.y + (shift ? 0 : room)}, {frame.size.width, std::max(0.0, frame.size.height - room)}};
}

drawing::Path star(Point center, double outer) {
    drawing::Path path;
    const double inner = outer * 0.45;
    for (int i = 0; i < 10; ++i) {
        const double angle = 3.14159265358979323846 * (0.5 + i / 5.0);
        const double r = i % 2 == 0 ? outer : inner;
        const Point p{center.x + r * std::cos(angle), center.y + r * std::sin(angle)};
        if (i == 0) path.move_to(p);
        else path.line_to(p);
    }
    path.close();
    return path;
}

void draw_level(AppKit& kit, Address cell, Rect frame, Address owner) {
    const auto state = level(kit, cell);
    const auto info = cell_state(kit, cell);
    const auto& th = kit.theme_for(owner);
    auto& canvas = kit.require_canvas("NSLevelIndicatorCell drawing");
    const bool flipped = owner && kit.send_bool(owner, "isFlipped");
    const double value = cell_value(kit, cell), range = std::max(1e-9, state->maximum - state->minimum);
    const double proportion = std::clamp((value - state->minimum) / range, 0.0, 1.0);
    const auto bar = bar_rect(*state, frame, flipped);
    const double alpha = info->enabled ? 1 : 0.5;
    switch (state->style) {
    case style::rating: {
        const auto count = static_cast<std::int64_t>(std::llround(range));
        if (count <= 0) break;
        const double cell_width = bar.size.width / static_cast<double>(count);
        const double outer = std::min(cell_width, bar.size.height) * 0.45;
        for (std::int64_t i = 0; i < count; ++i) {
            const Rect slot{{bar.origin.x + cell_width * static_cast<double>(i), bar.origin.y}, {cell_width, bar.size.height}};
            const Point center{slot.origin.x + slot.size.width / 2, slot.origin.y + slot.size.height / 2};
            const bool filled = state->minimum + static_cast<double>(i) + 0.5 <= value;
            const auto image = filled ? state->rating_image : state->placeholder_image;
            if (image) {
                CallRegs draw;
                put_rect(draw, 0, {{center.x - outer, center.y - outer}, {2 * outer, 2 * outer}});
                static_cast<void>(kit.send(image, "drawInRect:", draw));
            } else if (filled) {
                canvas.fill_path(star(center, outer), th.text.with_alpha(alpha));
            } else if (info->editable) {
                drawing::Path dot;
                dot.add_oval({{center.x - 1.5, center.y - 1.5}, {3, 3}});
                canvas.fill_path(dot, th.secondary_text.with_alpha(alpha));
            }
        }
        return;
    }
    case style::discrete: {
        const auto count = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(range)));
        const double segment = bar.size.width / static_cast<double>(count);
        const auto color = fill_for(kit, *state, value).with_alpha(alpha);
        for (std::int64_t i = 0; i < count; ++i) {
            const Rect area{{bar.origin.x + segment * static_cast<double>(i) + 1, bar.origin.y + 1}, {std::max(0.0, segment - 2), std::max(0.0, bar.size.height - 2)}};
            const bool lit = state->minimum + static_cast<double>(i) + 1 <= value + 1e-9;
            canvas.fill_rect(area, lit ? (state->tiered ? fill_for(kit, *state, state->minimum + static_cast<double>(i) + 1).with_alpha(alpha) : color)
                                       : th.control_face.with_alpha(alpha));
            canvas.stroke_rect(area, th.control_border, 1);
        }
        break;
    }
    case style::relevancy: {
        // Thin vertical bars across the filled part.
        for (double x = bar.origin.x; x < bar.origin.x + bar.size.width * proportion; x += 2)
            canvas.fill_rect({{x, bar.origin.y}, {1, bar.size.height}}, th.secondary_text.with_alpha(alpha));
        break;
    }
    default: {
        draw_bezel(kit, bar, 2, th.control_face.with_alpha(alpha), th.control_border);
        const Rect filled{{bar.origin.x + 1, bar.origin.y + 1}, {std::max(0.0, (bar.size.width - 2) * proportion), std::max(0.0, bar.size.height - 2)}};
        if (filled.size.width > 0) canvas.fill_rect(filled, fill_for(kit, *state, value).with_alpha(alpha));
        break;
    }
    }
    // Tick marks.
    if (state->ticks > 0) {
        const bool below = state->tick_position == 0;
        const bool at_low_y = below != flipped;   // below is towards smaller y when not flipped
        const double edge = at_low_y ? frame.origin.y : frame.origin.y + frame.size.height;
        for (std::int64_t i = 0; i < state->ticks; ++i) {
            const double x = state->ticks == 1 ? bar.origin.x + bar.size.width / 2
                                               : bar.origin.x + bar.size.width * static_cast<double>(i) / static_cast<double>(state->ticks - 1);
            const bool major = state->major_ticks > 1 && state->ticks > 1 && i % std::max<std::int64_t>(1, (state->ticks - 1) / (state->major_ticks - 1)) == 0;
            const double length = major ? 5 : 3;
            canvas.fill_rect({{std::floor(x), at_low_y ? edge : edge - length}, {1, length}}, th.secondary_text);
        }
    }
}

// The value an editable indicator takes at `x` (view coordinates of its bounds).
double value_at(const LevelState& state, Rect bounds, double x) {
    const double range = state.maximum - state.minimum;
    const double proportion = std::clamp((x - bounds.origin.x) / std::max(1e-9, bounds.size.width), 0.0, 1.0);
    if (state.style == style::rating || state.style == style::discrete) return state.minimum + std::ceil(proportion * range - 1e-9);
    return state.minimum + proportion * range;
}

} // namespace

void install_indicators(AppKit& kit) {
    auto* const k = &kit;
    // ---- NSSwitch ----
    const auto sw = kit.declare_class("NSSwitch", "NSControl");
    kit.method(sw, "initWithFrame:", [k, sw](CpuState& s, GuestMemory&) {
        static_cast<void>(switch_of(*k, s.x[0]));   // before NSControl makes its state
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = k->objc.send_super(s.x[0], sw, k->sel("initWithFrame:"), registers).x0;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(sw, "initWithCoder:", [k, sw](CpuState& s, GuestMemory&) {
        static_cast<void>(switch_of(*k, s.x[0]));
        const auto self = super_init_with_coder(*k, s.x[0], sw, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        const auto state = switch_of(*k, self);
        state->enabled = in.flag("NSEnabled", true);
        state->on = in.integer("NSControlState", in.integer("NSState", 0)) != 0;
    });
    kit.method(sw, "state", [k](CpuState& s, GuestMemory&) { s.x[0] = switch_of(*k, s.x[0])->on ? 1 : 0; }, "q@:");
    kit.method(sw, "setState:", [k](CpuState& s, GuestMemory&) {
        switch_of(*k, s.x[0])->on = static_cast<std::int64_t>(s.x[2]) != 0;
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(sw, "isEnabled", [k](CpuState& s, GuestMemory&) { return_bool(s, switch_of(*k, s.x[0])->enabled); }, "B@:");
    kit.method(sw, "setEnabled:", [k](CpuState& s, GuestMemory&) {
        switch_of(*k, s.x[0])->enabled = bool_arg(s, 2);
        k->invalidate_view(s.x[0]);
    }, "v@:B");
    kit.method(sw, "intrinsicContentSize", [](CpuState& s, GuestMemory&) { return_size(s, {38, 22}); }, "{CGSize=dd}@:");
    kit.method(sw, "acceptsFirstMouse:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    kit.method(sw, "drawRect:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = switch_of(*k, self);
        const auto& th = k->theme_for(self);
        auto& canvas = k->require_canvas("NSSwitch drawing");
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const double height = std::min(bounds.size.height, 22.0), width = std::min(bounds.size.width, 38.0);
        const Rect pill{{bounds.origin.x + (bounds.size.width - width) / 2, bounds.origin.y + (bounds.size.height - height) / 2}, {width, height}};
        const double alpha = state->enabled ? 1 : 0.5;
        auto track = state->on ? th.accent : th.control_face_pressed;
        if (state->pressed) track = {track.r * 0.85, track.g * 0.85, track.b * 0.85, track.a};
        drawing::Path body;
        body.add_rounded_rect(pill, height / 2, height / 2);
        canvas.fill_path(body, track.with_alpha(alpha));
        canvas.stroke_path(body, th.control_border.with_alpha(alpha), 1);
        const double knob = height - 4;
        const double x = state->on ? pill.origin.x + pill.size.width - 2 - knob : pill.origin.x + 2;
        drawing::Path round;
        round.add_oval({{x, pill.origin.y + 2}, {knob, knob}});
        canvas.fill_path(round, Color::gray(1, alpha));
        canvas.stroke_path(round, th.control_border.with_alpha(alpha), 0.5);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(sw, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = switch_of(*k, self);
        if (!state->enabled) return;
        state->pressed = true;
        k->invalidate_view(self);
        bool released_inside{};
        try {
            released_inside = track_mouse(*k, self, s.x[2], [&](bool inside) {
                state->pressed = inside;
                k->invalidate_view(self);
            });
        } catch (...) {
            state->pressed = false;
            throw;
        }
        state->pressed = false;
        if (released_inside) state->on = !state->on;
        k->invalidate_view(self);
        if (released_inside) static_cast<void>(k->send_action(k->send(self, "action").x0, k->send(self, "target").x0, self));
    }, "v@:@");
    kit.method(sw, "performClick:", [k](CpuState& s, GuestMemory&) {
        const auto state = switch_of(*k, s.x[0]);
        if (!state->enabled) return;
        state->on = !state->on;
        k->invalidate_view(s.x[0]);
        static_cast<void>(k->send_action(k->send(s.x[0], "action").x0, k->send(s.x[0], "target").x0, s.x[0]));
    }, "v@:@");

    // ---- NSLevelIndicatorCell ----
    const auto c = kit.declare_class("NSLevelIndicatorCell", "NSActionCell");
    const auto n = kit.declare_class("NSLevelIndicator", "NSControl");
    static_cast<void>(kit.send_id(n, "setCellClass:", {c}));
    kit.method(c, "initWithLevelIndicatorStyle:", [k](CpuState& s, GuestMemory&) {
        const auto self = k->send(s.x[0], "init").x0;
        s.x[0] = self;
        if (self) level(*k, self)->style = static_cast<std::int64_t>(s.x[2]);
    }, "@@:q");
    kit.method(c, "initWithCoder:", [k, c](CpuState& s, GuestMemory&) {
        const auto self = super_init_with_coder(*k, s.x[0], c, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        const auto state = level(*k, self);
        state->minimum = in.real("NSMinValue", 0);
        state->maximum = in.real("NSMaxValue", 2);
        state->warning = in.real("NSWarningValue", 0);
        state->critical = in.real("NSCriticalValue", 0);
        state->style = in.integer("NSIndicatorStyle", style::continuous);
        state->ticks = in.integer("NSNumberOfTickMarks", 0);
        state->major_ticks = in.integer("NSNumberOfMajorTickMarks", 0);
        state->tick_position = in.integer("NSTickMarkPosition", 0);
        if (in.has("NSValue")) {
            CallRegs value;
            value.set_d(0, in.real("NSValue", 0));
            static_cast<void>(k->send(self, "setDoubleValue:", value));
        }
    });
    kit.method(c, "dealloc", [k, c](CpuState& s, GuestMemory&) {
        if (const auto cell = k->objc.find_host_state<CellState>(s.x[0]))
            if (const auto state = std::dynamic_pointer_cast<LevelState>(cell->extension))
                for (auto* slot : {&state->fill, &state->warning_fill, &state->critical_fill, &state->rating_image, &state->placeholder_image})
                    k->release(std::exchange(*slot, 0));
        static_cast<void>(k->objc.send_super(s.x[0], c, k->sel("dealloc")));
    }, "v@:");
    const auto redisplay = [k](Address cell) {
        if (const auto owner = cell_state(*k, cell)->control_view) k->invalidate_view(owner);
    };
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, double LevelState::*>>{
             {"minValue", "setMinValue:", &LevelState::minimum}, {"maxValue", "setMaxValue:", &LevelState::maximum},
             {"warningValue", "setWarningValue:", &LevelState::warning}, {"criticalValue", "setCriticalValue:", &LevelState::critical}}) {
        const auto field = member;
        kit.method(c, getter, [k, field](CpuState& s, GuestMemory&) { set_d(s, 0, (*level(*k, s.x[0])).*field); }, "d@:");
        kit.method(c, setter, [k, field, redisplay](CpuState& s, GuestMemory&) {
            (*level(*k, s.x[0])).*field = d(s, 0);
            redisplay(s.x[0]);
        }, "v@:d");
    }
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, std::int64_t LevelState::*>>{
             {"levelIndicatorStyle", "setLevelIndicatorStyle:", &LevelState::style},
             {"numberOfTickMarks", "setNumberOfTickMarks:", &LevelState::ticks},
             {"numberOfMajorTickMarks", "setNumberOfMajorTickMarks:", &LevelState::major_ticks},
             {"tickMarkPosition", "setTickMarkPosition:", &LevelState::tick_position}}) {
        const auto field = member;
        kit.method(c, getter, [k, field](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>((*level(*k, s.x[0])).*field); }, "q@:");
        kit.method(c, setter, [k, field, redisplay](CpuState& s, GuestMemory&) {
            (*level(*k, s.x[0])).*field = static_cast<std::int64_t>(s.x[2]);
            redisplay(s.x[0]);
        }, "v@:q");
    }
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, Address LevelState::*>>{
             {"fillColor", "setFillColor:", &LevelState::fill}, {"warningFillColor", "setWarningFillColor:", &LevelState::warning_fill},
             {"criticalFillColor", "setCriticalFillColor:", &LevelState::critical_fill},
             {"ratingImage", "setRatingImage:", &LevelState::rating_image},
             {"ratingPlaceholderImage", "setRatingPlaceholderImage:", &LevelState::placeholder_image}}) {
        const auto field = member;
        kit.method(c, getter, [k, field](CpuState& s, GuestMemory&) { s.x[0] = (*level(*k, s.x[0])).*field; });
        kit.method(c, setter, [k, field, redisplay](CpuState& s, GuestMemory&) {
            k->assign((*level(*k, s.x[0])).*field, s.x[2]);
            redisplay(s.x[0]);
        }, "v@:@");
    }
    kit.method(c, "drawsTieredCapacityLevels", [k](CpuState& s, GuestMemory&) { return_bool(s, level(*k, s.x[0])->tiered); }, "B@:");
    kit.method(c, "setDrawsTieredCapacityLevels:", [k, redisplay](CpuState& s, GuestMemory&) {
        level(*k, s.x[0])->tiered = bool_arg(s, 2);
        redisplay(s.x[0]);
    }, "v@:B");
    // The tick marks' positions along the bar (NSSlider's API).
    kit.method(c, "rectOfTickMarkAtIndex:", [k](CpuState& s, GuestMemory&) {
        const auto state = level(*k, s.x[0]);
        const auto index = static_cast<std::int64_t>(s.x[2]);
        if (index < 0 || index >= state->ticks) throw std::out_of_range("Alchemy: -[NSLevelIndicatorCell rectOfTickMarkAtIndex:] beyond the tick marks");
        const auto owner = cell_state(*k, s.x[0])->control_view;
        const auto bounds = owner ? AppKit::view_bounds(*k->view_state(owner)) : Rect{};
        const double x = state->ticks == 1 ? bounds.size.width / 2 : bounds.size.width * static_cast<double>(index) / static_cast<double>(state->ticks - 1);
        return_rect(s, {{bounds.origin.x + x, bounds.origin.y}, {1, 5}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:q");
    kit.method(c, "tickMarkValueAtIndex:", [k](CpuState& s, GuestMemory&) {
        const auto state = level(*k, s.x[0]);
        const auto index = static_cast<std::int64_t>(s.x[2]);
        if (index < 0 || index >= state->ticks) throw std::out_of_range("Alchemy: -[NSLevelIndicatorCell tickMarkValueAtIndex:] beyond the tick marks");
        set_d(s, 0, state->ticks == 1 ? (state->minimum + state->maximum) / 2
                                      : state->minimum + (state->maximum - state->minimum) * static_cast<double>(index) / static_cast<double>(state->ticks - 1));
    }, "d@:q");
    kit.method(c, "cellSize", [k](CpuState& s, GuestMemory&) {
        const auto state = level(*k, s.x[0]);
        const double range = std::max(1.0, std::round(state->maximum - state->minimum));
        return_size(s, state->style == style::rating ? Size{13 * range, 13} : Size{96, state->ticks > 0 ? 24.0 : 18.0});
    }, "{CGSize=dd}@:");
    kit.method(c, "drawWithFrame:inView:", [k](CpuState& s, GuestMemory&) { draw_level(*k, s.x[0], rect_arg(s), s.x[2]); },
               "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");

    // ---- NSLevelIndicator: the cell's API, editing by click or drag ----
    for (const auto& [name, types, redraw] : std::initializer_list<std::tuple<const char*, const char*, bool>>{
             {"minValue", "d@:", false}, {"setMinValue:", "v@:d", true}, {"maxValue", "d@:", false}, {"setMaxValue:", "v@:d", true},
             {"warningValue", "d@:", false}, {"setWarningValue:", "v@:d", true}, {"criticalValue", "d@:", false},
             {"setCriticalValue:", "v@:d", true}, {"levelIndicatorStyle", "q@:", false}, {"setLevelIndicatorStyle:", "v@:q", true},
             {"numberOfTickMarks", "q@:", false}, {"setNumberOfTickMarks:", "v@:q", true}, {"numberOfMajorTickMarks", "q@:", false},
             {"setNumberOfMajorTickMarks:", "v@:q", true}, {"tickMarkPosition", "q@:", false}, {"setTickMarkPosition:", "v@:q", true},
             {"fillColor", "@@:", false}, {"setFillColor:", "v@:@", true}, {"warningFillColor", "@@:", false},
             {"setWarningFillColor:", "v@:@", true}, {"criticalFillColor", "@@:", false}, {"setCriticalFillColor:", "v@:@", true},
             {"ratingImage", "@@:", false}, {"setRatingImage:", "v@:@", true}, {"ratingPlaceholderImage", "@@:", false},
             {"setRatingPlaceholderImage:", "v@:@", true}, {"drawsTieredCapacityLevels", "B@:", false},
             {"setDrawsTieredCapacityLevels:", "v@:B", true}, {"rectOfTickMarkAtIndex:", "{CGRect={CGPoint=dd}{CGSize=dd}}@:q", false},
             {"tickMarkValueAtIndex:", "d@:q", false}, {"isEditable", "B@:", false}, {"setEditable:", "v@:B", true}})
        forward_to_cell(kit, n, name, types, redraw);
    kit.method(n, "intrinsicContentSize", [k](CpuState& s, GuestMemory&) {
        const auto cell = control_cell(*k, s.x[0]);
        return_size(s, cell ? result_size(k->send(cell, "cellSize")) : Size{96, 18});
    }, "{CGSize=dd}@:");
    kit.method(n, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto cell = control_cell(*k, self);
        if (!cell) return;
        const auto info = cell_state(*k, cell);
        if (!info->enabled || !info->editable) return;
        const auto state = level(*k, cell);
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const auto set = [&](Point p) {
            CallRegs value;
            value.set_d(0, value_at(*state, bounds, p.x));
            static_cast<void>(k->send(cell, "setDoubleValue:", value));
            k->invalidate_view(self);
        };
        set(event_location_in(*k, self, s.x[2]));
        static_cast<void>(track_mouse(*k, self, s.x[2], [](bool) {}, set));
        static_cast<void>(k->send_action(info->action, info->target, self));
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit::controls
