#include "alchemy/darwin/appkit/controls_internal.hpp"

#include <vector>

// Dynamic system colours. AppKit's semantic colours (controlTextColor,
// labelColor, windowBackgroundColor, ...) are shared objects that resolve
// against the appearance being drawn in (NSAppearance's current drawing
// appearance: the view's effective appearance while it draws), so a colour
// taken once draws light text in a dark-aqua window and dark text in an
// aqua one. Here each one is a single immortal NSColor whose components are
// those of the appearance last drawn in: AppKit's controls resolve them for
// their view before drawing (resolve_dynamic_colors), and every view does
// as its -viewWillDraw runs (before its -drawRect:). Values are AppKit's
// light / dark ones (graphics.cpp's table); vibrant dark is dark.
namespace alchemy::gui::appkit {
namespace {
using drawing::Color;

struct DynamicColor {
    const char* name;
    Color light, dark;
    Address object{};   // immortal NSColor
};

std::vector<DynamicColor>& table() {
    static std::vector<DynamicColor> colors{
        {"controlColor", Color::gray(1), Color::gray(1, 0.25)},
        {"controlBackgroundColor", Color::gray(1), Color::gray(0.118)},
        {"textBackgroundColor", Color::gray(1), Color::gray(0.118)},
        {"windowBackgroundColor", Color::gray(0.925), Color::gray(0.196)},
        {"underPageBackgroundColor", Color::gray(0.59), Color::gray(0.16)},
        {"textColor", Color::gray(0), Color::gray(1, 0.85)},
        {"labelColor", Color::gray(0, 0.85), Color::gray(1, 0.85)},
        {"controlTextColor", Color::gray(0, 0.85), Color::gray(1, 0.85)},
        {"secondaryLabelColor", Color::gray(0, 0.5), Color::gray(1, 0.55)},
        {"tertiaryLabelColor", Color::gray(0, 0.26), Color::gray(1, 0.25)},
        {"quaternaryLabelColor", Color::gray(0, 0.1), Color::gray(1, 0.1)},
        {"placeholderTextColor", Color::gray(0, 0.25), Color::gray(1, 0.25)},
        {"disabledControlTextColor", Color::gray(0, 0.25), Color::gray(1, 0.25)},
        {"selectedTextColor", Color::gray(0), Color::gray(1)},
        {"selectedControlTextColor", Color::gray(0, 0.85), Color::gray(1, 0.85)},
        {"headerTextColor", Color::gray(0, 0.85), Color::gray(1)},
        {"windowFrameTextColor", Color::gray(0, 0.85), Color::gray(1, 0.85)},
        {"selectedControlColor", {0.7, 0.84, 1, 1}, {0.25, 0.4, 0.6, 1}},
        {"unemphasizedSelectedContentBackgroundColor", Color::gray(0.86), Color::gray(0.27)},
        {"unemphasizedSelectedTextBackgroundColor", Color::gray(0.86), Color::gray(0.27)},
        {"gridColor", Color::gray(0.9), Color::gray(1, 0.1)},
        {"separatorColor", Color::gray(0, 0.1), Color::gray(1, 0.1)},
        {"headerColor", Color::gray(1), Color::gray(0.2)},
        {"controlHighlightColor", Color::gray(1), Color::gray(1, 0.25)},
        {"scrollBarColor", Color::gray(0.67), Color::gray(0.2)},
        {"selectedKnobColor", Color::gray(0.4), Color::gray(0.6)},
        {"windowFrameColor", Color::gray(0.67), Color::gray(0.25)},
        {"highlightColor", Color::gray(1), Color::gray(0.7)},
        {"linkColor", {0, 0.41, 0.85, 1}, {0.2, 0.55, 1, 1}},
        {"systemBlueColor", {0, 0.478, 1, 1}, {0.04, 0.52, 1, 1}},
        {"systemRedColor", {1, 0.23, 0.19, 1}, {1, 0.27, 0.23, 1}},
        {"systemGreenColor", {0.16, 0.8, 0.25, 1}, {0.2, 0.84, 0.29, 1}},
        {"systemOrangeColor", {1, 0.58, 0, 1}, {1, 0.62, 0.04, 1}},
        {"systemYellowColor", {1, 0.8, 0, 1}, {1, 0.84, 0.04, 1}},
        {"systemGrayColor", Color::gray(0.56), Color::gray(0.6)},
        {"systemPurpleColor", {0.69, 0.32, 0.87, 1}, {0.75, 0.35, 0.95, 1}},
        {"systemPinkColor", {1, 0.18, 0.33, 1}, {1, 0.22, 0.37, 1}},
        {"systemTealColor", {0.35, 0.78, 0.98, 1}, {0.39, 0.82, 1, 1}},
        {"systemBrownColor", {0.64, 0.52, 0.37, 1}, {0.67, 0.56, 0.41, 1}},
        {"systemIndigoColor", {0.35, 0.34, 0.84, 1}, {0.37, 0.36, 0.9, 1}},
    };
    return colors;
}
int resolved = -1;   // the appearance the colours hold: -1 none yet, 0 light, 1 dark

void apply(AppKit& kit, bool dark) {
    if (resolved == (dark ? 1 : 0)) return;
    resolved = dark ? 1 : 0;
    for (const auto& entry : table()) {
        if (!entry.object) continue;
        const auto& value = dark ? entry.dark : entry.light;
        const auto state = kit.objc.host_state<ColorState>(entry.object);
        state->red = value.r; state->green = value.g; state->blue = value.b; state->alpha = value.a;
    }
}

void resolve_for(AppKit& kit, Address view) { apply(kit, kit.dark_appearance(view)); }

} // namespace

void install_appearance(AppKit& kit) {
    auto* const k = &kit;
    resolved = -1;
    for (auto& entry : table()) {
        entry.object = kit.retain(kit.color_object(entry.light.r, entry.light.g, entry.light.b, entry.light.a));
        if (const auto record = kit.process.objects().find(entry.object)) record->immortal = true;
        const auto object = entry.object;
        // Taken outside drawing, a colour resolves for the current appearance
        // (the application's effective one); inside a display pass it keeps
        // the appearance of the view being drawn. Colours copied when taken
        // (the nib loader's archived catalog colours) thus get the
        // application's appearance, not that of a window drawn earlier.
        kit.class_method(kit.color, entry.name, [k, object](CpuState& s, GuestMemory&) {
            if (k->draw_depth == 0 || resolved < 0) resolve_for(*k, 0);
            s.x[0] = object;
        });
    }
    set_dynamic_color_resolver(&resolve_for);
    // Every view resolves them for its own appearance before it draws.
    kit.method(kit.view, "viewWillDraw", [k](CpuState& s, GuestMemory&) { resolve_for(*k, s.x[0]); }, "v@:");
}

} // namespace alchemy::gui::appkit
