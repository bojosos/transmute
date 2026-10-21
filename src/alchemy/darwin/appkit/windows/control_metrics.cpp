#include "alchemy/darwin/appkit/windows/windows_internal.hpp"

#include <cmath>

// Auto Layout metrics of the standard controls (QLab gap group G4): the
// alignment rectangle insets and intrinsic content sizes (in alignment
// rectangle space, as -intrinsicContentSize is defined) that constraints in
// Interface Builder nibs are written against. The values are the ones
// QLab's nibs encode: for every constraint between a control and a plain
// view the archived frames differ from the constraint by these insets
// (rounded push buttons 5 / 7 / 7 / 7 points top / left / bottom / right at
// the regular size and 3 / 6 / 7 / 6 small, bezeled pop-up buttons 1 / 3 / 4 /
// 4 and 1 / 4 / 4 / 4, labels 0 / 2 / 0 / 2), and the alignment heights
// follow from the archived frames (push buttons 32 - 12 = 20 points, small
// 27 - 10 = 17; pop-up buttons 25 - 5 = 20, small 22 - 5 = 17).
// Installed on NSButton, NSPopUpButton and NSTextField only where those
// classes inherit NSControl's generic implementation (so a class-specific
// one from the controls providers stays).
namespace alchemy::gui::completion_kit::windows {
namespace {
using Insets = std::array<double, 4>;   // top, left, bottom, right

double text_width(Address control, Address string) {
    auto& k = kit();
    if (!string) return 0;
    appkit::TextStyle style;
    style.font = k.font_spec(k.send(control, "font").x0);
    return std::ceil(k.measure_string(k.string16(string), style, 0).width);
}

bool push_button(Address button) {
    auto& k = kit();
    return k.send(button, "bezelStyle").x0 == 1 && k.send_bool(button, "isBordered");
}
std::int64_t control_size(Address control) { return static_cast<std::int64_t>(kit().send(control, "controlSize").x0); }

Insets button_insets(Address button) {
    if (!push_button(button)) return {};
    switch (control_size(button)) {
    case 0: return {5, 7, 7, 7};
    case 1: return {3, 6, 7, 6};
    default: return {};
    }
}
Insets popup_insets(Address popup) {
    switch (control_size(popup)) {
    case 0: return {1, 3, 4, 4};
    case 1: return {1, 4, 4, 4};
    default: return {};
    }
}
Insets text_field_insets(Address field) {
    auto& k = kit();
    return k.send_bool(field, "isBezeled") || k.send_bool(field, "isBordered") ? Insets{} : Insets{0, 2, 0, 2};
}

void return_insets(CpuState& s, const Insets& insets) {
    for (unsigned i = 0; i < 4; ++i) appkit::set_d(s, i, insets[i]);
}

// Whether `cls` implements `selector` itself rather than inheriting NSControl's.
bool inherits_control(Address cls, std::string_view selector) {
    auto& k = kit();
    const auto sel = k.sel(selector);
    const auto own = k.guest_call(objc_library, "_class_getMethodImplementation", {cls, sel});
    const auto generic = k.guest_call(objc_library, "_class_getMethodImplementation", {k.control, sel});
    return own == generic;
}
} // namespace

void install_control_metrics(Support& support) {
    static_cast<void>(support);
    auto& k = kit();
    const auto button = k.objc.find_class("NSButton"), popup = k.objc.find_class("NSPopUpButton"), field = k.objc.find_class("NSTextField");
    const auto install = [&](Address cls, const char* selector, Handler handler, const char* types) {
        if (cls && inherits_control(cls, selector)) k.method(cls, selector, std::move(handler), types);
    };
    // NSPopUpButton is an NSButton subclass: its own metrics go in first.
    // Pop-up buttons: the selected item's title, the arrows and padding.
    install(popup, "alignmentRectInsets", [](CpuState& s, GuestMemory&) { return_insets(s, popup_insets(s.x[0])); }, "{NSEdgeInsets=dddd}@:");
    install(popup, "intrinsicContentSize", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = s.x[0];
        const bool small = control_size(self) == 1;
        const auto title = text_width(self, kk.send(self, "titleOfSelectedItem").x0);
        appkit::return_size(s, {title + (small ? 32 : 38), small ? 17.0 : 20.0});
    }, "{CGSize=dd}@:");
    // Buttons: rounded push buttons have Apple's fixed alignment height and
    // the title plus side padding; other buttons keep their cell size.
    install(button, "alignmentRectInsets", [](CpuState& s, GuestMemory&) { return_insets(s, button_insets(s.x[0])); }, "{NSEdgeInsets=dddd}@:");
    install(button, "intrinsicContentSize", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = s.x[0];
        const auto cell_size = appkit::result_size(kk.send(kk.send(self, "cell").x0, "cellSize"));
        if (!push_button(self)) { appkit::return_size(s, cell_size); return; }
        const bool small = control_size(self) == 1;
        const auto title = text_width(self, kk.send(self, "title").x0);
        const auto image = kk.send(self, "image").x0;
        const auto picture = image ? appkit::result_size(kk.send(image, "size")).width + (title > 0 ? 4 : 0) : 0.0;
        appkit::return_size(s, {title + picture + (small ? 20 : 24), small ? 17.0 : 20.0});
    }, "{CGSize=dd}@:");
    // Text fields: labels hug their text (2 points of inset each side);
    // editable bezeled fields have no intrinsic width.
    install(field, "alignmentRectInsets", [](CpuState& s, GuestMemory&) { return_insets(s, text_field_insets(s.x[0])); }, "{NSEdgeInsets=dddd}@:");
    install(field, "intrinsicContentSize", [](CpuState& s, GuestMemory&) {
        auto& kk = kit();
        const auto self = s.x[0];
        const auto cell_size = appkit::result_size(kk.send(kk.send(self, "cell").x0, "cellSize"));
        const auto insets = text_field_insets(self);
        const bool editable_field = kk.send_bool(self, "isEditable") && kk.send_bool(self, "isBezeled");
        appkit::return_size(s, {editable_field ? -1.0 : std::max(0.0, cell_size.width - insets[1] - insets[3]), cell_size.height});
    }, "{CGSize=dd}@:");
}

} // namespace alchemy::gui::completion_kit::windows
