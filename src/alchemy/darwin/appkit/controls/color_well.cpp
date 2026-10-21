#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <cmath>

// NSColorWell (subclassed by QLab for its cue colours): the colour, the
// bordered and borderless looks (a swatch inside a bezel, or the colour
// filling the well; translucent colours over a checkerboard), the active
// state (one active well at a time, drawn with the accent border), target
// and action, -takeColorFrom:, and nib decoding (NSColor, NSIsBordered,
// NSEnabled). A click activates the well, which shows the shared colour
// panel as AppKit does; NSColorPanel is not provided (panels.cpp fails
// explicitly), so that path fails explicitly too.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;
using drawing::Color;

struct WellState final : ControlState {
    Address color{};                   // +1
    std::int64_t style{};              // NSColorWellStyle (default 0, minimal 1, expanded 2)
    bool active{}, bordered{true}, enabled{true}, supports_alpha{true};
};

Address active_well{};                 // the one active well (not retained)

std::shared_ptr<WellState> well_of(AppKit& kit, Address view) {
    const auto state = kit.view_state(view);
    if (!state->extension) state->extension = std::make_shared<WellState>();
    auto typed = std::dynamic_pointer_cast<WellState>(state->extension);
    if (!typed) fail("NSColorWell state of another type");
    return typed;
}

void draw_swatch(AppKit& kit, drawing::Canvas& canvas, Rect area, Color color) {
    if (color.a < 1) {
        // A checkerboard under translucent colours.
        canvas.save();
        canvas.clip_to_rect(area);
        canvas.fill_rect(area, Color::gray(1));
        const double cell = 4;
        for (double y = area.origin.y, row = 0; y < area.origin.y + area.size.height; y += cell, ++row)
            for (double x = area.origin.x + (static_cast<int>(row) % 2) * cell; x < area.origin.x + area.size.width; x += 2 * cell)
                canvas.fill_rect({{x, y}, {cell, cell}}, Color::gray(0.8));
        canvas.restore();
    }
    canvas.fill_rect(area, color);
    static_cast<void>(kit);
}

} // namespace

void install_color_well(AppKit& kit) {
    auto* const k = &kit;
    const auto w = kit.declare_class("NSColorWell", "NSControl");
    kit.method(w, "initWithFrame:", [k, w](CpuState& s, GuestMemory&) {
        static_cast<void>(well_of(*k, s.x[0]));   // before NSControl makes its state
        CallRegs registers;
        put_rect(registers, 0, rect_arg(s));
        const auto self = k->objc.send_super(s.x[0], w, k->sel("initWithFrame:"), registers).x0;
        s.x[0] = self;
        if (self) k->assign(well_of(*k, self)->color, k->color_object(1, 1, 1, 1));
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(w, "initWithCoder:", [k, w](CpuState& s, GuestMemory&) {
        static_cast<void>(well_of(*k, s.x[0]));
        const auto self = super_init_with_coder(*k, s.x[0], w, s.x[2]);
        s.x[0] = self;
        if (!self) return;
        const Decoder in{*k, s.x[2]};
        const auto state = well_of(*k, self);
        if (const auto color = in.object("NSColor")) k->assign(state->color, color);
        else k->assign(state->color, k->color_object(1, 1, 1, 1));
        state->bordered = in.flag("NSIsBordered", true);
        state->enabled = in.flag("NSEnabled", true);
        state->style = in.integer("NSColorWellStyle", 0);
    });
    kit.method(w, "dealloc", [k, w](CpuState& s, GuestMemory&) {
        if (const auto state = k->find_extension<WellState>(s.x[0])) k->release(std::exchange(state->color, 0));
        if (active_well == s.x[0]) active_well = 0;
        static_cast<void>(k->objc.send_super(s.x[0], w, k->sel("dealloc")));
    }, "v@:");
    kit.method(w, "color", [k](CpuState& s, GuestMemory&) { s.x[0] = well_of(*k, s.x[0])->color; });
    kit.method(w, "setColor:", [k](CpuState& s, GuestMemory&) {
        if (!s.x[2]) unsupported("-[NSColorWell setColor:] with nil");
        k->assign(well_of(*k, s.x[0])->color, s.x[2]);
        k->invalidate_view(s.x[0]);
    }, "v@:@");
    kit.method(w, "takeColorFrom:", [k](CpuState& s, GuestMemory&) {
        if (s.x[2] && k->responds(s.x[2], "color")) static_cast<void>(k->send_id(s.x[0], "setColor:", {k->send_id(s.x[2], "color")}));
    }, "v@:@");
    kit.method(w, "isActive", [k](CpuState& s, GuestMemory&) { return_bool(s, well_of(*k, s.x[0])->active); }, "B@:");
    // Activation: exclusive deactivates the other active well; the well
    // then edits the shared colour panel's colour.
    kit.method(w, "activate:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = well_of(*k, self);
        if (active_well && active_well != self && bool_arg(s, 2)) static_cast<void>(k->send(active_well, "deactivate"));
        state->active = true;
        active_well = self;
        k->invalidate_view(self);
        const auto panel_class = k->objc.find_class("NSColorPanel");
        if (!panel_class) unsupported("-[NSColorWell activate:] (no NSColorPanel)");
        const auto panel = k->send(panel_class, "sharedColorPanel").x0;
        static_cast<void>(k->send_id(panel, "setColor:", {state->color}));
        static_cast<void>(k->send_id(panel, "orderFront:", {self}));
    }, "v@:B");
    kit.method(w, "deactivate", [k](CpuState& s, GuestMemory&) {
        well_of(*k, s.x[0])->active = false;
        if (active_well == s.x[0]) active_well = 0;
        k->invalidate_view(s.x[0]);
    }, "v@:");
    const auto flag = [&kit, w, k](const char* getter, const char* setter, bool WellState::*member) {
        kit.method(w, getter, [k, member](CpuState& s, GuestMemory&) { return_bool(s, (*well_of(*k, s.x[0])).*member); }, "B@:");
        kit.method(w, setter, [k, member](CpuState& s, GuestMemory&) {
            (*well_of(*k, s.x[0])).*member = bool_arg(s, 2);
            k->invalidate_view(s.x[0]);
        }, "v@:B");
    };
    flag("isBordered", "setBordered:", &WellState::bordered);
    flag("isEnabled", "setEnabled:", &WellState::enabled);
    flag("supportsAlpha", "setSupportsAlpha:", &WellState::supports_alpha);
    kit.method(w, "colorWellStyle", [k](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(well_of(*k, s.x[0])->style); }, "q@:");
    kit.method(w, "setColorWellStyle:", [k](CpuState& s, GuestMemory&) {
        well_of(*k, s.x[0])->style = static_cast<std::int64_t>(s.x[2]);
        k->invalidate_view(s.x[0]);
    }, "v@:q");
    kit.method(w, "acceptsFirstMouse:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
    kit.method(w, "drawRect:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = well_of(*k, self);
        const auto bounds = AppKit::view_bounds(*k->view_state(self));
        const auto& th = k->theme_for(self);
        if (state->bordered) {
            draw_bezel(*k, bounds, 4, th.control_face, state->active ? th.accent : th.control_border);
            if (state->active) draw_bezel(*k, {{bounds.origin.x + 1, bounds.origin.y + 1}, {bounds.size.width - 2, bounds.size.height - 2}}, 3,
                                          th.control_face, th.accent);
        }
        CallRegs registers;
        put_rect(registers, 0, state->bordered ? Rect{{bounds.origin.x + 5, bounds.origin.y + 5},
                                                      {std::max(0.0, bounds.size.width - 10), std::max(0.0, bounds.size.height - 10)}}
                                               : bounds);
        static_cast<void>(k->send(self, "drawWellInside:", registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    kit.method(w, "drawWellInside:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = well_of(*k, self);
        const auto& th = k->theme_for(self);
        auto& canvas = k->require_canvas("NSColorWell drawing");
        const auto area = rect_arg(s);
        auto color = k->color_value(state->color, Color::gray(1));
        if (!state->enabled) color = color.with_alpha(0.5);
        draw_swatch(*k, canvas, area, color);
        canvas.stroke_rect(area, th.control_border, 1);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // A click activates an inactive well and deactivates an active one (the
    // action is sent when the colour panel changes the colour).
    kit.method(w, "mouseDown:", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = well_of(*k, self);
        if (!state->enabled) return;
        if (!track_mouse(*k, self, s.x[2], [](bool) {})) return;
        if (state->active) static_cast<void>(k->send(self, "deactivate"));
        else static_cast<void>(k->send_id(self, "activate:", {1}));
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit::controls
