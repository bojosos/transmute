#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cmath>
#include <map>

// NSRulerView (subclassed by QLab for its timelines) and NSTextView's
// -setRulerVisible:. A ruler belongs to a scroll view (which places it:
// containers.cpp's tiling) and measures its client view (else the scroll
// view's document view) in its measurement units: a hash mark every unit
// step, labels every unit, AppKit's step-down cycle subdividing a unit when
// there is room for the marks. Units are registered by name with
// +registerUnitWithName:abbreviation:unitToPointsConversionFactor:stepUpCycle:stepDownCycle:
// (Inches, Centimeters, Points and Picas are registered, as in AppKit).
// Markers and the accessory view are kept; markers are not drawn or dragged.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

struct Unit {
    std::string abbreviation;
    double points{};                      // points per unit
    std::vector<double> step_up, step_down;
};
std::map<std::string, Unit>& units() {
    static std::map<std::string, Unit> table{
        {"Inches", {"in", 72.0, {2.0}, {0.5, 0.2}}},
        {"Centimeters", {"cm", 28.35, {2.0}, {0.5, 0.2}}},
        {"Points", {"pt", 1.0, {10.0}, {0.5}}},
        {"Picas", {"pc", 12.0, {10.0}, {0.5}}},
    };
    return table;
}

struct RulerState final : ObjectPayload {
    Address scroll_view{}, client_view{};    // not retained
    Address accessory_view{};                // +1
    std::vector<Address> markers;            // +1
    std::string units{"Inches"};
    std::int64_t orientation{};              // NSHorizontalRuler 0, NSVerticalRuler 1
    double origin_offset{}, rule_thickness{16}, marker_thickness{15}, accessory_thickness{};
};

std::shared_ptr<RulerState> ruler_of(AppKit& kit, Address view) { return kit.extension<RulerState>(view); }

const Unit& unit_of(const RulerState& state) {
    const auto found = units().find(state.units);
    if (found == units().end()) unsupported("NSRulerView measurement units \"" + state.units + "\" are not registered");
    return found->second;
}

std::vector<double> numbers_of(AppKit& kit, Address array) {
    std::vector<double> values;
    for (const auto value : array ? kit.items(array) : std::vector<Address>{}) values.push_back(kit.cf.number_value(value).value_or(0));
    return values;
}

void retile(AppKit& kit, Address ruler) {
    if (const auto scroll = ruler_of(kit, ruler)->scroll_view) static_cast<void>(kit.send(scroll, "tile"));
    kit.invalidate_view(ruler);
}

} // namespace

void install_rulers(AppKit& kit) {
    auto* const k = &kit;
    const auto r = kit.declare_class("NSRulerView", "NSView");
    kit.string_constant("_NSRulerViewUnitInches", "Inches");
    kit.string_constant("_NSRulerViewUnitCentimeters", "Centimeters");
    kit.string_constant("_NSRulerViewUnitPoints", "Points");
    kit.string_constant("_NSRulerViewUnitPicas", "Picas");

    kit.class_method(r, "registerUnitWithName:abbreviation:unitToPointsConversionFactor:stepUpCycle:stepDownCycle:", [k](CpuState& s, GuestMemory&) {
        // (name, abbreviation, factor d0, step up, step down)
        if (!s.x[2] || d(s, 0) <= 0) unsupported("+[NSRulerView registerUnitWithName:...] without a name or a positive factor");
        Unit unit{k->text(s.x[3]), d(s, 0), numbers_of(*k, s.x[4]), numbers_of(*k, s.x[5])};
        for (const auto step : unit.step_up) if (step <= 1) unsupported("NSRulerView step-up cycle values must exceed 1");
        for (const auto step : unit.step_down) if (step <= 0 || step >= 1) unsupported("NSRulerView step-down cycle values must be in (0, 1)");
        units()[k->text(s.x[2])] = std::move(unit);
    }, "v@:@@d@@");
    kit.method(r, "initWithScrollView:orientation:", [k, r](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, {{0, 0}, {s.x[3] ? 16.0 : 100.0, s.x[3] ? 100.0 : 16.0}});
        const auto self = k->objc.send_super(s.x[0], r, k->sel("initWithFrame:"), registers).x0;
        s.x[0] = self;
        if (!self) return;
        const auto state = ruler_of(*k, self);
        state->scroll_view = s.x[2];
        state->orientation = static_cast<std::int64_t>(s.x[3]);
    }, "@@:@Q");
    kit.method(r, "initWithFrame:", [k, r](CpuState& s, GuestMemory&) {
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        s.x[0] = k->objc.send_super(s.x[0], r, k->sel("initWithFrame:"), registers).x0;
        if (s.x[0]) static_cast<void>(ruler_of(*k, s.x[0]));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(r, "dealloc", [k, r](CpuState& s, GuestMemory&) {
        if (const auto state = k->find_extension<RulerState>(s.x[0])) {
            k->release(std::exchange(state->accessory_view, 0));
            for (const auto marker : std::exchange(state->markers, {})) k->release(marker);
        }
        static_cast<void>(k->objc.send_super(s.x[0], r, k->sel("dealloc")));
    }, "v@:");
    kit.method(r, "isFlipped", [k](CpuState& s, GuestMemory&) {
        // A vertical ruler follows its scroll view's document view.
        const auto state = ruler_of(*k, s.x[0]);
        if (state->orientation == 1 && state->scroll_view) {
            const auto document = k->send(state->scroll_view, "documentView").x0;
            return_bool(s, !document || k->send_bool(document, "isFlipped"));
            return;
        }
        s.x[0] = 1;
    }, "B@:");
    kit.method(r, "scrollView", [k](CpuState& s, GuestMemory&) { s.x[0] = ruler_of(*k, s.x[0])->scroll_view; });
    kit.method(r, "setScrollView:", [k](CpuState& s, GuestMemory&) { ruler_of(*k, s.x[0])->scroll_view = s.x[2]; }, "v@:@");
    kit.method(r, "orientation", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ruler_of(*k, s.x[0])->orientation); }, "Q@:");
    kit.method(r, "setOrientation:", [k](CpuState& s, GuestMemory&) {
        ruler_of(*k, s.x[0])->orientation = static_cast<std::int64_t>(s.x[2]);
        retile(*k, s.x[0]);
    }, "v@:Q");
    kit.method(r, "clientView", [k](CpuState& s, GuestMemory&) { s.x[0] = ruler_of(*k, s.x[0])->client_view; });
    kit.method(r, "setClientView:", [k](CpuState& s, GuestMemory&) {
        const auto state = ruler_of(*k, s.x[0]);
        if (state->client_view == s.x[2]) return;
        // The old client hears that it lost the ruler (NSRulerView's client protocol).
        if (const auto old = state->client_view) {
            CallRegs registers;
            registers.x[2] = s.x[0];
            registers.x[3] = s.x[2];
            static_cast<void>(k->send_optional(old, "rulerView:willSetClientView:", registers));
        }
        state->client_view = s.x[2];
        for (const auto marker : std::exchange(state->markers, {})) k->release(marker);
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    kit.method(r, "accessoryView", [k](CpuState& s, GuestMemory&) { s.x[0] = ruler_of(*k, s.x[0])->accessory_view; });
    kit.method(r, "setAccessoryView:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ruler_of(*k, self);
        if (state->accessory_view) static_cast<void>(k->send(state->accessory_view, "removeFromSuperview"));
        k->assign(state->accessory_view, s.x[2]);
        if (s.x[2]) static_cast<void>(k->send_id(self, "addSubview:", {s.x[2]}));
        retile(*k, self);
    }, "v@:@");
    kit.method(r, "measurementUnits", [k](CpuState& s, GuestMemory&) { s.x[0] = k->auto_string(ruler_of(*k, s.x[0])->units); });
    kit.method(r, "setMeasurementUnits:", [k](CpuState& s, GuestMemory&) {
        const auto name = k->text(s.x[2]);
        if (!units().contains(name)) unsupported("-[NSRulerView setMeasurementUnits:] with unregistered units \"" + name + "\"");
        ruler_of(*k, s.x[0])->units = name;
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    const auto real = [&kit, r, k](const char* getter, const char* setter, double RulerState::*member, bool tile) {
        kit.method(r, getter, [k, member](CpuState& s, GuestMemory&) { set_d(s, 0, (*ruler_of(*k, s.x[0])).*member); }, "d@:");
        kit.method(r, setter, [k, member, tile](CpuState& s, GuestMemory&) {
            (*ruler_of(*k, s.x[0])).*member = d(s, 0);
            if (tile) retile(*k, s.x[0]); else k->invalidate_view(s.x[0]);
        }, "v@:d");
    };
    real("originOffset", "setOriginOffset:", &RulerState::origin_offset, false);
    real("ruleThickness", "setRuleThickness:", &RulerState::rule_thickness, true);
    real("reservedThicknessForMarkers", "setReservedThicknessForMarkers:", &RulerState::marker_thickness, true);
    real("reservedThicknessForAccessoryView", "setReservedThicknessForAccessoryView:", &RulerState::accessory_thickness, true);
    kit.method(r, "requiredThickness", [k](CpuState& s, GuestMemory&) {
        const auto state = ruler_of(*k, s.x[0]);
        double accessory = state->accessory_thickness;
        if (state->accessory_view) {
            const auto size = k->view_state(state->accessory_view)->frame.size;
            accessory = std::max(accessory, state->orientation == 0 ? size.height : size.width);
        }
        set_d(s, 0, state->rule_thickness + state->marker_thickness + accessory);
    }, "d@:");
    kit.method(r, "baselineLocation", [k](CpuState& s, GuestMemory&) {
        const auto state = ruler_of(*k, s.x[0]);
        set_d(s, 0, state->accessory_thickness + state->marker_thickness);
    }, "d@:");
    // Markers (kept; not drawn).
    kit.method(r, "markers", [k](CpuState& s, GuestMemory&) {
        const auto& list = ruler_of(*k, s.x[0])->markers;
        s.x[0] = list.empty() ? 0 : k->auto_array(list);
    });
    kit.method(r, "setMarkers:", [k](CpuState& s, GuestMemory&) {
        const auto state = ruler_of(*k, s.x[0]);
        std::vector<Address> fresh;
        for (const auto marker : s.x[2] ? k->items(s.x[2]) : std::vector<Address>{}) fresh.push_back(k->retain(marker));
        for (const auto old : std::exchange(state->markers, fresh)) k->release(old);
    }, "v@:@");
    kit.method(r, "addMarker:", [k](CpuState& s, GuestMemory&) { if (s.x[2]) ruler_of(*k, s.x[0])->markers.push_back(k->retain(s.x[2])); }, "v@:@");
    kit.method(r, "removeMarker:", [k](CpuState& s, GuestMemory&) {
        auto& list = ruler_of(*k, s.x[0])->markers;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found == list.end()) return;
        list.erase(found);
        k->release(s.x[2]);
    }, "v@:@");
    kit.method(r, "invalidateHashMarks", [k](CpuState& s, GuestMemory&) { k->invalidate_view(s.x[0]); }, "v@:");
    kit.method(r, "moveRulerlineFromLocation:toLocation:", [k](CpuState& s, GuestMemory&) { k->invalidate_view(s.x[0]); }, "v@:dd");
    // Drawing: the background and the hash marks.
    kit.method(r, "drawRect:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        static_cast<void>(k->send(self, "drawHashMarksAndLabelsInRect:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(r, "drawHashMarksAndLabelsInRect:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = ruler_of(*k, self);
        const auto& unit = unit_of(*state);
        const auto& th = k->theme_for(self);
        auto& canvas = k->require_canvas("NSRulerView drawing");
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const bool horizontal = state->orientation == 0;
        canvas.fill_rect(bounds, th.header_background);
        // The edge toward the content.
        if (horizontal) canvas.fill_rect({{bounds.origin.x, bounds.origin.y + bounds.size.height - 1}, {bounds.size.width, 1}}, th.separator);
        else canvas.fill_rect({{bounds.origin.x + bounds.size.width - 1, bounds.origin.y}, {1, bounds.size.height}}, th.separator);
        // The client's zero in ruler coordinates (plus the origin offset).
        auto client = state->client_view;
        if (!client && state->scroll_view) client = k->send(state->scroll_view, "documentView").x0;
        double zero = 0;
        if (client && k->view_state(client)->window && k->view_state(self)->window) {
            const auto at = k->window_to_view(self, k->view_to_window(client, {0, 0}));
            zero = horizontal ? at.x : at.y;
        }
        zero += state->origin_offset;
        // Subdivide a unit while marks stay at least 4 points apart.
        std::vector<double> steps{unit.points};
        for (std::size_t i = 0; i < 6; ++i) {
            const auto next = steps.back() * unit.step_down[i % std::max<std::size_t>(1, unit.step_down.size())];
            if (unit.step_down.empty() || next < 4) break;
            steps.push_back(next);
        }
        // Labels every unit, stepping up while they would crowd (40 points).
        double label_step = unit.points;
        for (std::size_t i = 0; label_step < 40 && i < 12; ++i) label_step *= unit.step_up.empty() ? 2 : unit.step_up[i % unit.step_up.size()];
        const double extent = horizontal ? bounds.size.width : bounds.size.height;
        const double start = horizontal ? bounds.origin.x : bounds.origin.y;
        const double cross = horizontal ? bounds.size.height : bounds.size.width;
        const double base = horizontal ? bounds.origin.y + bounds.size.height : bounds.origin.x + bounds.size.width;   // the content edge
        const auto mark = [&](double along, double length) {
            if (horizontal) canvas.fill_rect({{std::round(along), base - length}, {1, length}}, th.secondary_text);
            else canvas.fill_rect({{base - length, std::round(along)}, {length, 1}}, th.secondary_text);
        };
        for (std::size_t level = steps.size(); level-- > 0;) {
            const double step = steps[level];
            const double length = level == 0 ? std::min(cross * 0.6, state->rule_thickness) : std::max(2.0, cross * 0.25 / static_cast<double>(level));
            const auto first = std::floor((start - zero) / step);
            for (double n = first; zero + n * step <= start + extent; n += 1) mark(zero + n * step, length);
        }
        TextStyle style;
        style.font.size = 9;
        style.color = th.secondary_text;
        const auto first = std::floor((start - zero) / label_step);
        for (double n = first; zero + n * label_step <= start + extent; n += 1) {
            const double at = zero + n * label_step;
            const auto value = static_cast<long long>(std::llround(n * label_step / unit.points));
            const auto text = drawing::utf16(std::to_string(value));
            if (horizontal) draw_line_in(*k, text, style, {{at + 2, bounds.origin.y}, {label_step - 4, cross - state->rule_thickness * 0.6}}, 0, false);
            else draw_line_in(*k, text, style, {{bounds.origin.x + 1, at + 1}, {cross - 4, 11}}, 0, false);
        }
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");

    // NSTextView: the rulers of the enclosing scroll view.
    kit.method(kit.text_view, "isRulerVisible", [k](CpuState& s, GuestMemory&) {
        const auto scroll = k->send(s.x[0], "enclosingScrollView").x0;
        return_bool(s, scroll && k->send_bool(scroll, "rulersVisible") && k->send_bool(scroll, "hasHorizontalRuler"));
    }, "B@:");
    kit.method(kit.text_view, "setRulerVisible:", [k](CpuState& s, GuestMemory&) {
        const auto scroll = k->send(s.x[0], "enclosingScrollView").x0;
        if (!scroll) return;
        if (bool_arg(s, 2)) {
            static_cast<void>(k->send_id(scroll, "setHasHorizontalRuler:", {1}));
            static_cast<void>(k->send_id(k->send(scroll, "horizontalRulerView").x0, "setClientView:", {s.x[0]}));
        }
        static_cast<void>(k->send_id(scroll, "setRulersVisible:", {bool_arg(s, 2) ? 1u : 0u}));
    }, "v@:B");
}

} // namespace alchemy::gui::completion_kit::controls
