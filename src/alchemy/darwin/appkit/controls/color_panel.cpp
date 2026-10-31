#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// NSColorPanel: the shared colour panel, a floating utility panel AppKit
// builds from its own controls. A colour swatch, a picker popup and one
// slider with a value field per component: the slider pickers Gray
// (brightness), RGB (0-255) and HSB (hue 0-360, saturation and brightness
// 0-100), and an opacity row while -showsAlpha. The other modes (CMYK,
// colour lists, the wheel, crayons, custom palettes) are kept by -mode but
// shown with the RGB sliders. The accessory view sits under the sliders.
//
// Changing a component sets the panel's colour, posts
// NSColorPanelColorDidChangeNotification, updates the active colour well
// (color_well.cpp) and sends the panel's action (-changeColor: unless set)
// to its target (nil: the responder chain), on every slider movement while
// -isContinuous, else when the slider is released. -setColor: posts the
// notification without sending the action, as AppKit does. Closing the
// panel deactivates the active colour well.
//
// Not provided (explicit stops): colour lists (-attachColorList:,
// -detachColorList:) and dragging colours (+dragColor:withEvent:fromView:).
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

// NSColorPanelMode
constexpr std::int64_t mode_gray = 0, mode_rgb = 1, mode_hsb = 3;
constexpr Address opacity_tag = 100;
constexpr double width = 260, margin = 10, swatch_height = 30, popup_height = 24, row_height = 24, row_gap = 4;

struct Component { const char* label; double maximum; };
std::int64_t shown_mode(std::int64_t mode) { return mode == mode_gray || mode == mode_hsb ? mode : mode_rgb; }
std::vector<Component> components(std::int64_t mode) {
    switch (shown_mode(mode)) {
    case mode_gray: return {{"Brightness", 100}};
    case mode_hsb: return {{"Hue", 360}, {"Saturation", 100}, {"Brightness", 100}};
    default: return {{"Red", 255}, {"Green", 255}, {"Blue", 255}};
    }
}

// The components of `color` in the panel's units for `mode`.
std::vector<double> values_of(std::int64_t mode, Color color) {
    switch (shown_mode(mode)) {
    case mode_gray: return {(0.299 * color.r + 0.587 * color.g + 0.114 * color.b) * 100};
    case mode_hsb: {
        const double high = std::max({color.r, color.g, color.b}), low = std::min({color.r, color.g, color.b}), range = high - low;
        double hue = 0;
        if (range > 0) {
            if (high == color.r) hue = std::fmod((color.g - color.b) / range, 6.0);
            else if (high == color.g) hue = (color.b - color.r) / range + 2;
            else hue = (color.r - color.g) / range + 4;
            hue *= 60;
            if (hue < 0) hue += 360;
        }
        return {hue, high > 0 ? range / high * 100 : 0, high * 100};
    }
    default: return {color.r * 255, color.g * 255, color.b * 255};
    }
}

Color color_of(std::int64_t mode, const std::vector<double>& values, double alpha) {
    switch (shown_mode(mode)) {
    case mode_gray: return Color::gray(values[0] / 100, alpha);
    case mode_hsb: {
        const double hue = std::fmod(values[0], 360.0) / 60, saturation = values[1] / 100, brightness = values[2] / 100;
        const double chroma = brightness * saturation, x = chroma * (1 - std::abs(std::fmod(hue, 2.0) - 1)), m = brightness - chroma;
        std::array<double, 3> rgb{};
        switch (static_cast<int>(hue)) {
        case 0: rgb = {chroma, x, 0}; break;
        case 1: rgb = {x, chroma, 0}; break;
        case 2: rgb = {0, chroma, x}; break;
        case 3: rgb = {0, x, chroma}; break;
        case 4: rgb = {x, 0, chroma}; break;
        default: rgb = {chroma, 0, x}; break;
        }
        return {rgb[0] + m, rgb[1] + m, rgb[2] + m, alpha};
    }
    default: return {values[0] / 255, values[1] / 255, values[2] / 255, alpha};
    }
}

struct PanelState {
    Address panel{};                   // the shared panel (immortal)
    Color color{1, 1, 1, 1};
    std::vector<double> values;        // the current mode's components (kept, so a grey keeps its hue)
    std::int64_t mode{mode_rgb};
    std::int64_t initial_mode{mode_rgb};   // +setPickerMode: before the panel exists
    std::uint64_t picker_mask{};       // +setPickerMask: (kept: the slider pickers are always offered)
    bool continuous{true}, shows_alpha{true};
    Address accessory{};               // +1
    Address target{};                  // not retained
    Address action{};                  // SEL (0: changeColor:)
    Address swatch{};                  // the content's controls, owned by the content view
    std::vector<Address> sliders, fields;   // by tag order: the components, then opacity
};
PanelState& panel_state() {
    static PanelState state;
    return state;
}

Address make_control(AppKit& kit, std::string_view class_name, Rect frame) {
    CallRegs registers;
    put_rect(registers, 0, frame);
    return kit.send(kit.send(kit.objc.find_class(class_name), "alloc").x0, "initWithFrame:", registers).x0;   // +1
}

// The sliders and fields show the current values.
void show_values(AppKit& kit) {
    auto& state = panel_state();
    for (std::size_t i = 0; i < state.sliders.size(); ++i) {
        const bool opacity = i == state.values.size();
        const double value = opacity ? state.color.a * 100 : state.values[i];
        CallRegs registers;
        registers.set_d(0, value);
        static_cast<void>(kit.send(state.sliders[i], "setDoubleValue:", registers));
        static_cast<void>(kit.send_id(state.fields[i], "setIntegerValue:", {static_cast<Address>(std::llround(value))}));
    }
    if (state.swatch) static_cast<void>(kit.send_id(state.swatch, "setColor:", {kit.color_object(state.color.r, state.color.g, state.color.b, state.color.a)}));
}

// Builds the panel's content for its mode, alpha row and accessory view, and
// sizes the panel to it.
void rebuild(AppKit& kit) {
    auto& state = panel_state();
    const auto panel = state.panel;
    const auto content = kit.send_id(panel, "contentView");
    for (const auto view : kit.items(kit.send_id(content, "subviews"))) static_cast<void>(kit.send(view, "removeFromSuperview"));
    state.sliders.clear();
    state.fields.clear();
    const auto rows = components(state.mode);
    const std::size_t row_count = rows.size() + (state.shows_alpha ? 1 : 0);
    const double accessory_height = state.accessory ? result_rect(kit.send(state.accessory, "frame")).size.height : 0;
    const double height = margin + swatch_height + 8 + popup_height + 8 + static_cast<double>(row_count) * (row_height + row_gap) +
                          (state.accessory ? accessory_height + 8 : 0) + margin;
    CallRegs size;
    size.set_d(0, width);
    size.set_d(1, height);
    static_cast<void>(kit.send(panel, "setContentSize:", size));
    double top = margin;
    const auto place = [&](Address view, double x, double w, double h) {
        CallRegs frame;
        put_rect(frame, 0, {{x, height - top - h}, {w, h}});
        static_cast<void>(kit.send(view, "setFrame:", frame));
        static_cast<void>(kit.send_id(content, "addSubview:", {view}));
    };
    // The swatch: a colour well that is never activated.
    state.swatch = make_control(kit, "NSColorWell", {{0, 0}, {width - 2 * margin, swatch_height}});
    static_cast<void>(kit.send_id(state.swatch, "setEnabled:", {0}));
    place(state.swatch, margin, width - 2 * margin, swatch_height);
    kit.release(state.swatch);
    top += swatch_height + 8;
    // The picker popup.
    const auto popup = make_control(kit, "NSPopUpButton", {{0, 0}, {width - 2 * margin, popup_height}});
    for (const auto* title : {"Gray", "RGB", "HSB"}) static_cast<void>(kit.send_id(popup, "addItemWithTitle:", {kit.auto_string(title)}));
    const auto shown = shown_mode(state.mode);
    static_cast<void>(kit.send_id(popup, "selectItemAtIndex:", {static_cast<Address>(shown == mode_gray ? 0 : shown == mode_hsb ? 2 : 1)}));
    static_cast<void>(kit.send_id(popup, "setTarget:", {panel}));
    static_cast<void>(kit.send_id(popup, "setAction:", {kit.sel("_alchemyPickerChosen:")}));
    place(popup, margin, width - 2 * margin, popup_height);
    kit.release(popup);
    top += popup_height + 8;
    // A row per component, then opacity.
    for (std::size_t i = 0; i < row_count; ++i) {
        const bool opacity = i == rows.size();
        const auto label_text = opacity ? "Opacity" : rows[i].label;
        const double maximum = opacity ? 100 : rows[i].maximum;
        const auto tag = opacity ? opacity_tag : static_cast<Address>(i);
        const auto label = kit.send_id(kit.objc.find_class("NSTextField"), "labelWithString:", {kit.auto_string(label_text)});
        place(label, margin, 74, row_height);
        const auto slider = make_control(kit, "NSSlider", {{0, 0}, {118, row_height}});
        CallRegs range;
        range.set_d(0, 0);
        static_cast<void>(kit.send(slider, "setMinValue:", range));
        range.set_d(0, maximum);
        static_cast<void>(kit.send(slider, "setMaxValue:", range));
        static_cast<void>(kit.send_id(slider, "setContinuous:", {state.continuous ? 1U : 0U}));
        const auto field = make_control(kit, "NSTextField", {{0, 0}, {38, row_height}});
        for (const auto control : {slider, field}) {
            static_cast<void>(kit.send_id(control, "setTag:", {tag}));
            static_cast<void>(kit.send_id(control, "setTarget:", {panel}));
            static_cast<void>(kit.send_id(control, "setAction:", {kit.sel("_alchemyComponentChanged:")}));
        }
        place(slider, margin + 78, 118, row_height);
        place(field, margin + 78 + 124, 38, row_height);
        state.sliders.push_back(slider);
        state.fields.push_back(field);
        kit.release(slider);
        kit.release(field);
        top += row_height + row_gap;
    }
    if (state.accessory) {
        top += 4;
        const auto frame = result_rect(kit.send(state.accessory, "frame"));
        place(state.accessory, margin, std::min(frame.size.width, width - 2 * margin), accessory_height);
    }
    show_values(kit);
}

// A user's change: the notification, the active well, the action.
void user_changed(AppKit& kit) {
    auto& state = panel_state();
    show_values(kit);
    kit.post("NSColorPanelColorDidChangeNotification", state.panel);
    color_panel_changed(kit, state.panel);
    static_cast<void>(kit.send_action(state.action ? state.action : kit.sel("changeColor:"), state.target, state.panel));
}

Address shared_panel(AppKit& kit, Address panel_class) {
    auto& state = panel_state();
    if (state.panel) return state.panel;
    CallRegs registers;
    put_rect(registers, 0, {{0, 0}, {width, 200}});
    registers.x[2] = 1 | 2 | 16;   // titled, closable, utility
    registers.x[3] = 2;            // NSBackingStoreBuffered
    registers.x[4] = 0;
    const auto panel = kit.send(kit.send(panel_class, "alloc").x0, "initWithContentRect:styleMask:backing:defer:", registers).x0;
    if (const auto record = kit.process.objects().find(panel)) record->immortal = true;
    state.panel = panel;
    state.mode = state.initial_mode;
    state.values = values_of(state.mode, state.color);
    static_cast<void>(kit.send_id(panel, "setTitle:", {kit.auto_string("Colors")}));
    static_cast<void>(kit.send_id(panel, "setReleasedWhenClosed:", {0}));
    static_cast<void>(kit.send_id(panel, "setFloatingPanel:", {1}));
    static_cast<void>(kit.send_id(panel, "setHidesOnDeactivate:", {1}));
    rebuild(kit);
    return panel;
}

} // namespace

void install_color_panel(AppKit& kit) {
    auto* const k = &kit;
    const auto p = kit.declare_class("NSColorPanel", "NSPanel");
    kit.class_method(p, "sharedColorPanel", [k, p](CpuState& s, GuestMemory&) { s.x[0] = shared_panel(*k, p); });
    kit.class_method(p, "sharedColorPanelExists", [](CpuState& s, GuestMemory&) { return_bool(s, panel_state().panel != 0); }, "B@:");
    kit.class_method(p, "setPickerMode:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        state.initial_mode = static_cast<std::int64_t>(s.x[2]);
        if (state.panel) static_cast<void>(k->send_id(state.panel, "setMode:", {s.x[2]}));
    }, "v@:q");
    kit.class_method(p, "setPickerMask:", [](CpuState& s, GuestMemory&) { panel_state().picker_mask = s.x[2]; }, "v@:Q");
    kit.class_method(p, "dragColor:withEvent:fromView:", [](CpuState&, GuestMemory&) {
        unsupported("+[NSColorPanel dragColor:withEvent:fromView:] (dragging colours)");
    }, "B@:@@@");
    kit.method(p, "color", [k](CpuState& s, GuestMemory&) {
        const auto& color = panel_state().color;
        s.x[0] = k->color_object(color.r, color.g, color.b, color.a);
    });
    kit.method(p, "setColor:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        const auto color = k->color_value(s.x[2], state.color);
        state.color = state.shows_alpha ? color : Color{color.r, color.g, color.b, 1};
        state.values = values_of(state.mode, state.color);
        show_values(*k);
        k->post("NSColorPanelColorDidChangeNotification", s.x[0]);
    }, "v@:@");
    kit.method(p, "alpha", [](CpuState& s, GuestMemory&) {
        const auto& state = panel_state();
        set_d(s, 0, state.shows_alpha ? state.color.a : 1.0);
    }, "d@:");
    kit.method(p, "mode", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(panel_state().mode); }, "q@:");
    kit.method(p, "setMode:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        const auto mode = static_cast<std::int64_t>(s.x[2]);
        if (mode < -1 || mode > 7) throw std::invalid_argument("Alchemy: -[NSColorPanel setMode:] with an unknown mode");
        const bool reshape = shown_mode(mode) != shown_mode(state.mode);
        state.mode = mode;
        state.values = values_of(mode, state.color);
        if (reshape) rebuild(*k);
        else show_values(*k);
    }, "v@:q");
    kit.method(p, "showsAlpha", [](CpuState& s, GuestMemory&) { return_bool(s, panel_state().shows_alpha); }, "B@:");
    kit.method(p, "setShowsAlpha:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        const bool shows = bool_arg(s, 2);
        if (shows == state.shows_alpha) return;
        state.shows_alpha = shows;
        rebuild(*k);
    }, "v@:B");
    kit.method(p, "isContinuous", [](CpuState& s, GuestMemory&) { return_bool(s, panel_state().continuous); }, "B@:");
    kit.method(p, "setContinuous:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        state.continuous = bool_arg(s, 2);
        for (const auto slider : state.sliders) static_cast<void>(k->send_id(slider, "setContinuous:", {state.continuous ? 1U : 0U}));
    }, "v@:B");
    kit.method(p, "accessoryView", [](CpuState& s, GuestMemory&) { s.x[0] = panel_state().accessory; });
    kit.method(p, "setAccessoryView:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        if (s.x[2] == state.accessory) return;
        if (state.accessory) static_cast<void>(k->send(state.accessory, "removeFromSuperview"));
        k->assign(state.accessory, s.x[2]);
        rebuild(*k);
    }, "v@:@");
    kit.method(p, "setTarget:", [](CpuState& s, GuestMemory&) { panel_state().target = s.x[2]; }, "v@:@");
    kit.method(p, "setAction:", [](CpuState& s, GuestMemory&) { panel_state().action = s.x[2]; }, "v@::");
    for (const auto* name : {"attachColorList:", "detachColorList:"})
        kit.method(p, name, [selector = std::string(name)](CpuState&, GuestMemory&) {
            unsupported("-[NSColorPanel " + selector + "] (colour lists)");
        }, "v@:@");
    // The panel's own controls.
    kit.method(p, "_alchemyComponentChanged:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        const auto tag = k->send(s.x[2], "tag").x0;
        const auto rows = components(state.mode);
        const double maximum = tag == opacity_tag ? 100 : tag < rows.size() ? rows[tag].maximum : 0;
        if (maximum == 0) return;
        const double value = std::clamp(k->send(s.x[2], "doubleValue").d(0), 0.0, maximum);
        if (tag == opacity_tag) {
            state.color.a = value / 100;
        } else {
            state.values[tag] = value;
            state.color = color_of(state.mode, state.values, state.color.a);
        }
        user_changed(*k);
    }, "v@:@");
    kit.method(p, "_alchemyPickerChosen:", [k](CpuState& s, GuestMemory&) {
        const auto index = static_cast<std::int64_t>(k->send(s.x[2], "indexOfSelectedItem").x0);
        const std::int64_t mode = index == 0 ? mode_gray : index == 2 ? mode_hsb : mode_rgb;
        static_cast<void>(k->send_id(s.x[0], "setMode:", {static_cast<Address>(mode)}));
    }, "v@:@");
    // Closing (or ordering out) the panel deactivates the active colour well.
    for (const auto& [name, types] : std::initializer_list<std::pair<const char*, const char*>>{{"close", "v@:"}, {"orderOut:", "v@:@"}})
        kit.method(p, name, [k, p, selector = std::string(name)](CpuState& s, GuestMemory&) {
            CallRegs registers;
            registers.x[2] = s.x[2];
            static_cast<void>(k->objc.send_super(s.x[0], p, k->sel(selector), registers));
            color_panel_closed(*k);
        }, types);
}

} // namespace alchemy::gui::completion_kit::controls
