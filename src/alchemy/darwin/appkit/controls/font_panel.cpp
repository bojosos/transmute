#include "alchemy/darwin/appkit/controls/g3.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

// NSFontPanel: the shared font panel, a floating utility panel AppKit builds
// from its own controls: a family pop-up (NSFontManager's available
// families), a typeface pop-up (the family's members), a size field with a
// pop-up of common sizes, and a sample line in the chosen font; the accessory
// view sits under them.
//
// -setPanelFont:isMultiple: shows a font (the text's selected font) and
// forgets earlier choices. Choosing a family, a typeface or a size records
// that choice and asks the font manager to -modifyFontViaPanel: (its action,
// -changeFont:, to the first responder); -panelConvertFont: then applies
// only what was chosen to each font it is given (a family keeps the font's
// traits where the family has them, a typeface replaces the font, a size
// keeps the face). Closing the panel keeps it for the next -orderFront:.
// The panel's modes (-setPanelFont: mode masks), collections, favourites and
// effects (shadow, colour, underline) are not provided.
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

constexpr double width = 300, margin = 10, row_height = 24, row_gap = 8, sample_height = 40;
constexpr std::array<int, 12> common_sizes{9, 10, 11, 12, 13, 14, 18, 24, 36, 48, 64, 72};

struct PanelState {
    Address panel{};                   // the shared panel (immortal)
    Address font{};                    // +1: the font shown (-setPanelFont:), or the last chosen
    bool multiple{}, enabled{true};
    bool family_chosen{}, face_chosen{}, size_chosen{};
    std::string family, face;          // the chosen family / face (PostScript name)
    double size{12};
    Address accessory{};               // +1
    Address family_popup{}, face_popup{}, size_field{}, size_popup{}, sample{};   // owned by the content view
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
Address font_manager(AppKit& kit) { return kit.send_id(kit.objc.find_class("NSFontManager"), "sharedFontManager"); }
Address font_named(AppKit& kit, const std::string& name, double size) {
    CallRegs registers;
    registers.x[2] = kit.auto_string(name);
    registers.set_d(0, size);
    return kit.send(kit.objc.find_class("NSFont"), "fontWithName:size:", registers).x0;
}

// The family's members: (PostScript name, face name) pairs.
std::vector<std::pair<std::string, std::string>> members_of(AppKit& kit, const std::string& family) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto members = kit.send_id(font_manager(kit), "availableMembersOfFontFamily:", {kit.auto_string(family)});
    for (const auto member : members ? kit.items(members) : std::vector<Address>{}) {
        const auto parts = kit.items(member);
        if (parts.size() >= 2) out.emplace_back(kit.text(parts[0]), kit.text(parts[1]));
    }
    return out;
}

// The controls show the panel's family, face and size, and the sample its font.
void show(AppKit& kit) {
    auto& state = panel_state();
    if (!state.family_popup) return;
    static_cast<void>(kit.send_id(state.family_popup, "selectItemWithTitle:", {kit.auto_string(state.family)}));
    static_cast<void>(kit.send(state.face_popup, "removeAllItems"));
    std::int64_t face_index = 0, index = 0;
    for (const auto& [name, face] : members_of(kit, state.family)) {
        static_cast<void>(kit.send_id(state.face_popup, "addItemWithTitle:", {kit.auto_string(face)}));
        if (name == state.face) face_index = index;
        ++index;
    }
    if (index) static_cast<void>(kit.send_id(state.face_popup, "selectItemAtIndex:", {static_cast<Address>(face_index)}));
    CallRegs size;
    size.set_d(0, state.size);
    static_cast<void>(kit.send(state.size_field, "setDoubleValue:", size));
    if (state.font) static_cast<void>(kit.send_id(state.sample, "setFont:", {state.font}));
}

void rebuild(AppKit& kit) {
    auto& state = panel_state();
    const auto content = kit.send_id(state.panel, "contentView");
    for (const auto view : kit.items(kit.send_id(content, "subviews"))) static_cast<void>(kit.send(view, "removeFromSuperview"));
    const double accessory_height = state.accessory ? result_rect(kit.send(state.accessory, "frame")).size.height : 0;
    const double height = margin + 3 * (row_height + row_gap) + sample_height + (state.accessory ? accessory_height + row_gap : 0) + margin;
    CallRegs size;
    size.set_d(0, width);
    size.set_d(1, height);
    static_cast<void>(kit.send(state.panel, "setContentSize:", size));
    double top = margin;
    const auto place = [&](Address view, double x, double w, double h) {
        CallRegs frame;
        put_rect(frame, 0, {{x, height - top - h}, {w, h}});
        static_cast<void>(kit.send(view, "setFrame:", frame));
        static_cast<void>(kit.send_id(content, "addSubview:", {view}));
    };
    const auto row = [&](const char* label_text, Address control, const char* action, double control_width) {
        const auto label = kit.send_id(kit.objc.find_class("NSTextField"), "labelWithString:", {kit.auto_string(label_text)});
        place(label, margin, 70, row_height);
        static_cast<void>(kit.send_id(control, "setTarget:", {state.panel}));
        static_cast<void>(kit.send_id(control, "setAction:", {kit.sel(action)}));
        place(control, margin + 74, control_width, row_height);
        kit.release(control);
    };
    state.family_popup = make_control(kit, "NSPopUpButton", {{0, 0}, {width - 2 * margin - 74, row_height}});
    const auto families = kit.send_id(font_manager(kit), "availableFontFamilies");
    for (const auto family : families ? kit.items(families) : std::vector<Address>{})
        static_cast<void>(kit.send_id(state.family_popup, "addItemWithTitle:", {family}));
    row("Family", state.family_popup, "_alchemyFamilyChosen:", width - 2 * margin - 74);
    top += row_height + row_gap;
    state.face_popup = make_control(kit, "NSPopUpButton", {{0, 0}, {width - 2 * margin - 74, row_height}});
    row("Typeface", state.face_popup, "_alchemyFaceChosen:", width - 2 * margin - 74);
    top += row_height + row_gap;
    state.size_field = make_control(kit, "NSTextField", {{0, 0}, {60, row_height}});
    row("Size", state.size_field, "_alchemySizeChosen:", 60);
    state.size_popup = make_control(kit, "NSPopUpButton", {{0, 0}, {80, row_height}});
    for (const auto value : common_sizes)
        static_cast<void>(kit.send_id(state.size_popup, "addItemWithTitle:", {kit.auto_string(std::to_string(value))}));
    static_cast<void>(kit.send_id(state.size_popup, "setTarget:", {state.panel}));
    static_cast<void>(kit.send_id(state.size_popup, "setAction:", {kit.sel("_alchemySizePicked:")}));
    place(state.size_popup, margin + 74 + 68, 80, row_height);
    kit.release(state.size_popup);
    top += row_height + row_gap;
    state.sample = kit.send_id(kit.objc.find_class("NSTextField"), "labelWithString:", {kit.auto_string("AaBbCcDd 0123")});
    place(state.sample, margin, width - 2 * margin, sample_height);
    top += sample_height;
    if (state.accessory) {
        top += row_gap;
        const auto frame = result_rect(kit.send(state.accessory, "frame"));
        place(state.accessory, margin, std::min(frame.size.width, width - 2 * margin), accessory_height);
    }
    show(kit);
}

// The panel shows `font` and forgets earlier choices.
void take_font(AppKit& kit, Address font) {
    auto& state = panel_state();
    state.family_chosen = state.face_chosen = state.size_chosen = false;
    if (!font) return;
    kit.assign(state.font, font);
    state.family = kit.text(kit.send_id(font, "familyName"));
    state.face = kit.text(kit.send_id(font, "fontName"));
    state.size = kit.send(font, "pointSize").d(0);
    show(kit);
}

// A user's choice: the font shown follows it, and the font manager sends -changeFont:.
void chosen(AppKit& kit) {
    auto& state = panel_state();
    if (const auto font = font_named(kit, state.face, state.size)) kit.assign(state.font, font);
    show(kit);
    if (state.enabled) static_cast<void>(kit.send_id(font_manager(kit), "modifyFontViaPanel:", {state.panel}));
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
    static_cast<void>(kit.send_id(panel, "setTitle:", {kit.auto_string("Fonts")}));
    static_cast<void>(kit.send_id(panel, "setReleasedWhenClosed:", {0}));
    static_cast<void>(kit.send_id(panel, "setFloatingPanel:", {1}));
    static_cast<void>(kit.send_id(panel, "setHidesOnDeactivate:", {1}));
    if (!state.font) {
        CallRegs size;
        size.set_d(0, 12);
        state.font = kit.retain(kit.send(kit.objc.find_class("NSFont"), "systemFontOfSize:", size).x0);
        state.family = kit.text(kit.send_id(state.font, "familyName"));
        state.face = kit.text(kit.send_id(state.font, "fontName"));
    }
    rebuild(kit);
    return panel;
}

} // namespace

void install_font_panel(AppKit& kit) {
    auto* const k = &kit;
    const auto p = kit.declare_class("NSFontPanel", "NSPanel");
    kit.class_method(p, "sharedFontPanel", [k, p](CpuState& s, GuestMemory&) { s.x[0] = shared_panel(*k, p); });
    kit.class_method(p, "sharedFontPanelExists", [](CpuState& s, GuestMemory&) { return_bool(s, panel_state().panel != 0); }, "B@:");
    kit.method(p, "setPanelFont:isMultiple:", [k](CpuState& s, GuestMemory&) {
        panel_state().multiple = bool_arg(s, 3);
        take_font(*k, s.x[2]);
    }, "v@:@B");
    // Only what the user chose since the last -setPanelFont: is applied.
    kit.method(p, "panelConvertFont:", [k](CpuState& s, GuestMemory&) {
        const auto& state = panel_state();
        auto font = s.x[2];
        if (!font) return;
        const auto manager = font_manager(*k);
        if (state.face_chosen) {
            CallRegs registers;
            registers.x[2] = font;
            registers.x[3] = k->auto_string(state.face);
            if (const auto made = k->send(manager, "convertFont:toFace:", registers).x0) font = made;
        } else if (state.family_chosen) {
            if (const auto made = k->send_id(manager, "convertFont:toFamily:", {font, k->auto_string(state.family)})) font = made;
        }
        if (state.size_chosen) {
            CallRegs registers;
            registers.x[2] = font;
            registers.set_d(0, state.size);
            if (const auto made = k->send(manager, "convertFont:toSize:", registers).x0) font = made;
        }
        s.x[0] = font;
    });
    kit.method(p, "isEnabled", [](CpuState& s, GuestMemory&) { return_bool(s, panel_state().enabled); }, "B@:");
    kit.method(p, "setEnabled:", [](CpuState& s, GuestMemory&) { panel_state().enabled = bool_arg(s, 2); }, "v@:B");
    kit.method(p, "accessoryView", [](CpuState& s, GuestMemory&) { s.x[0] = panel_state().accessory; });
    kit.method(p, "setAccessoryView:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        if (s.x[2] == state.accessory) return;
        if (state.accessory) static_cast<void>(k->send(state.accessory, "removeFromSuperview"));
        k->assign(state.accessory, s.x[2]);
        if (state.panel) rebuild(*k);
    }, "v@:@");
    kit.method(p, "reloadDefaultFontFamilies", [k](CpuState&, GuestMemory&) { if (panel_state().panel) rebuild(*k); }, "v@:");
    kit.method(p, "worksWhenModal", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    // The panel's own controls.
    kit.method(p, "_alchemyFamilyChosen:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        const auto title = k->send_id(s.x[2], "titleOfSelectedItem");
        if (!title) return;
        state.family = k->text(title);
        const auto members = members_of(*k, state.family);
        // The font's own face in the new family when there is one with the same face name, else the first.
        std::string current_face_name;
        for (const auto& [name, face] : members_of(*k, k->text(k->send_id(state.font, "familyName"))))
            if (name == state.face) current_face_name = face;
        state.face = members.empty() ? state.face : members.front().first;
        for (const auto& [name, face] : members) if (face == current_face_name) state.face = name;
        state.family_chosen = true;
        state.face_chosen = false;
        chosen(*k);
    }, "v@:@");
    kit.method(p, "_alchemyFaceChosen:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        const auto index = static_cast<std::int64_t>(k->send(s.x[2], "indexOfSelectedItem").x0);
        const auto members = members_of(*k, state.family);
        if (index < 0 || index >= static_cast<std::int64_t>(members.size())) return;
        state.face = members[static_cast<std::size_t>(index)].first;
        state.face_chosen = true;
        chosen(*k);
    }, "v@:@");
    kit.method(p, "_alchemySizeChosen:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        const double value = k->send(s.x[2], "doubleValue").d(0);
        if (!(value > 0) || value > 1000) { show(*k); return; }
        state.size = value;
        state.size_chosen = true;
        chosen(*k);
    }, "v@:@");
    kit.method(p, "_alchemySizePicked:", [k](CpuState& s, GuestMemory&) {
        auto& state = panel_state();
        const auto index = static_cast<std::int64_t>(k->send(s.x[2], "indexOfSelectedItem").x0);
        if (index < 0 || index >= static_cast<std::int64_t>(common_sizes.size())) return;
        state.size = common_sizes[static_cast<std::size_t>(index)];
        state.size_chosen = true;
        chosen(*k);
    }, "v@:@");
}

} // namespace alchemy::gui::completion_kit::controls
