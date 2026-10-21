#include "alchemy/darwin/kit/support/support_kit.hpp"

#include <array>

// AppKit accessibility names and functions Qt's Cocoa accessibility bridge
// (QMacAccessibilityElement, QCocoaAccessibility) binds. The constants carry
// AppKit's values. No assistive-technology client exists on the host side of
// Alchemy, so notifications posted with NSAccessibilityPostNotification have
// no receiver (they are dropped, as on macOS with no client listening); the
// "unignored" helpers treat every element as not ignored.
namespace alchemy::gui::support_kit {
namespace {
constexpr std::string_view appkit_library = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
} // namespace

void install_accessibility(Support& support) {
    const auto lib = appkit_library;
    const auto constant = [&](const char* symbol, const char* value) {
        if (!support.process.resolve(lib, symbol)) support.string_constant(lib, symbol, value);
    };
    for (const auto* role : {"Application", "Button", "Cell", "CheckBox", "ColorWell", "Column", "ComboBox", "Group",
                             "GrowArea", "Image", "Incrementor", "Link", "List", "MenuBar", "MenuButton", "MenuItem", "Menu",
                             "Outline", "PopUpButton", "ProgressIndicator", "RadioButton", "Row", "ScrollBar", "Slider",
                             "SplitGroup", "Splitter", "StaticText", "Table", "TextArea", "TextField", "Toolbar", "Unknown",
                             "ValueIndicator", "Window", "ScrollArea", "Sheet", "Drawer", "Browser", "Grid", "TabGroup",
                             "LevelIndicator", "Layout", "Ruler", "Popover", "DisclosureTriangle"}) {
        const auto symbol = "_NSAccessibility" + std::string(role) + "Role";
        constant(symbol.c_str(), ("AX" + std::string(role)).c_str());
    }
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_NSAccessibilitySearchFieldSubrole", "AXSearchField"},
             {"_NSAccessibilitySecureTextFieldSubrole", "AXSecureTextField"},
             {"_NSAccessibilityChildrenAttribute", "AXChildren"},
             {"_NSAccessibilityFocusedAttribute", "AXFocused"},
             {"_NSAccessibilitySelectedTextRangeAttribute", "AXSelectedTextRange"},
             {"_NSAccessibilityValueAttribute", "AXValue"},
             {"_NSAccessibilityRoleAttribute", "AXRole"},
             {"_NSAccessibilityTitleAttribute", "AXTitle"},
             {"_NSAccessibilityParentAttribute", "AXParent"},
             {"_NSAccessibilityWindowAttribute", "AXWindow"},
             {"_NSAccessibilityAttributedStringForRangeParameterizedAttribute", "AXAttributedStringForRange"},
             {"_NSAccessibilityBoundsForRangeParameterizedAttribute", "AXBoundsForRange"},
             {"_NSAccessibilityLineForIndexParameterizedAttribute", "AXLineForIndex"},
             {"_NSAccessibilityRangeForLineParameterizedAttribute", "AXRangeForLine"},
             {"_NSAccessibilityRangeForPositionParameterizedAttribute", "AXRangeForPosition"},
             {"_NSAccessibilityStringForRangeParameterizedAttribute", "AXStringForRange"},
             {"_NSAccessibilityStyleRangeForIndexParameterizedAttribute", "AXStyleRangeForIndex"},
             {"_NSAccessibilityDecrementAction", "AXDecrement"},
             {"_NSAccessibilityIncrementAction", "AXIncrement"},
             {"_NSAccessibilityPressAction", "AXPress"},
             {"_NSAccessibilityRaiseAction", "AXRaise"},
             {"_NSAccessibilityShowMenuAction", "AXShowMenu"},
             {"_NSAccessibilityFocusedUIElementChangedNotification", "AXFocusedUIElementChanged"},
             {"_NSAccessibilitySelectedTextChangedNotification", "AXSelectedTextChanged"},
             {"_NSAccessibilityTitleChangedNotification", "AXTitleChanged"},
             {"_NSAccessibilityUIElementDestroyedNotification", "AXUIElementDestroyed"},
             {"_NSAccessibilityValueChangedNotification", "AXValueChanged"}})
        constant(symbol, value);

    auto* s = &support;
    support.fill_function(lib, "_NSAccessibilityPostNotification", [](CpuState&, GuestMemory&) {});
    // NSAccessibilityRoleDescription(role, subrole): the role name without
    // "AX", in lower case words ("AXPopUpButton" -> "pop up button").
    support.fill_function(lib, "_NSAccessibilityRoleDescription", [s](CpuState& c, GuestMemory&) {
        if (!c.x[0]) { c.x[0] = 0; return; }
        auto role = s->cf.utf8(c.x[0]);
        if (role.starts_with("AX")) role.erase(0, 2);
        std::string words;
        for (std::size_t index = 0; index < role.size(); ++index) {
            const auto ch = role[index];
            if (ch >= 'A' && ch <= 'Z') {
                if (!words.empty()) words.push_back(' ');
                words.push_back(static_cast<char>(ch - 'A' + 'a'));
            } else {
                words.push_back(ch);
            }
        }
        const auto text = s->cf.string(words);
        c.x[0] = s->send(text, "autorelease").x0;
    });
    support.fill_function(lib, "_NSAccessibilityUnignoredAncestor", [](CpuState&, GuestMemory&) {});   // returns its argument
    support.fill_function(lib, "_NSAccessibilityUnignoredChildren", [](CpuState&, GuestMemory&) {});   // returns its argument
    support.fill_function(lib, "_NSAccessibilityUnignoredChildrenForOnlyChild", [s](CpuState& c, GuestMemory&) {
        if (!c.x[0]) { c.x[0] = 0; return; }
        const std::array values{c.x[0]};
        const auto array = s->cf.array(values);
        c.x[0] = s->send(array, "autorelease").x0;
    });
}

} // namespace alchemy::gui::support_kit
