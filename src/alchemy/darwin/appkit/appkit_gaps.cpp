#include "alchemy/gui/value_provider.hpp"

#include <array>
#include <utility>

// AppKit gaps of Chrome's framework:
// - the NSAccessibility attribute, notification, role, action and value
//   constants (Apple's "AX..." strings) and NSAccessibilityActionDescription;
// - string constants (notifications, image names and hints, Touch Bar item
//   identifiers, sharing service names, text-input attribute names);
// - classes: NSVisualEffectView, NSStatusBarButton, NSTextAttachment,
//   NSAccessibilityCustomAction, NSTouchBar and its items (with Electron's
//   color picker, slider and popover items and NSScrubber), the sharing
//   classes, NSColorList and NSGlassEffectView, each a real, subclassable host
//   class (the frame views Chrome subclasses are AppKit's, frame_view.cpp);
// - NSPerformService: no services are installed, so it performs none (NO);
//   no sharing service exists (+sharingServiceNamed: is nil, the list empty).
namespace alchemy::gui::values {
namespace {

using fill::appkit;

// "NSAccessibility<Name><Suffix>" -> "AX<Name>", for the suffixes Apple's
// constants use (NSAccessibilityConstants.h).
std::string accessibility_value(std::string_view symbol) {
    auto name = symbol.substr(std::string_view("_NSAccessibility").size());
    for (const std::string_view suffix : {"ParameterizedAttribute", "TextAttribute", "Attribute", "Notification", "Action",
                                          "Subrole", "Role", "Value", "Key"}) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) { name.remove_suffix(suffix.size()); break; }
    }
    return "AX" + std::string(name);
}

constexpr std::array accessibility_constants{
    "_NSAccessibilityApplicationActivatedNotification", "_NSAccessibilityApplicationDeactivatedNotification",
    "_NSAccessibilityApplicationHiddenNotification", "_NSAccessibilityApplicationShownNotification",
    "_NSAccessibilityAscendingSortDirectionValue", "_NSAccessibilityBackgroundColorTextAttribute", "_NSAccessibilityCancelAction",
    "_NSAccessibilityCellForColumnAndRowParameterizedAttribute", "_NSAccessibilityChildrenInNavigationOrderAttribute",
    "_NSAccessibilityColumnCountAttribute", "_NSAccessibilityColumnHeaderUIElementsAttribute",
    "_NSAccessibilityColumnIndexRangeAttribute", "_NSAccessibilityColumnsAttribute", "_NSAccessibilityConfirmAction",
    "_NSAccessibilityContentListSubrole", "_NSAccessibilityContentsAttribute", "_NSAccessibilityCreatedNotification",
    "_NSAccessibilityDefinitionListSubrole", "_NSAccessibilityDescendingSortDirectionValue", "_NSAccessibilityDescriptionAttribute",
    "_NSAccessibilityDisclosedByRowAttribute", "_NSAccessibilityDisclosedRowsAttribute", "_NSAccessibilityDisclosingAttribute",
    "_NSAccessibilityDisclosureLevelAttribute", "_NSAccessibilityDrawerCreatedNotification", "_NSAccessibilityEnabledAttribute",
    "_NSAccessibilityExpandedAttribute", "_NSAccessibilityFocusedWindowChangedNotification", "_NSAccessibilityFontSizeKey",
    "_NSAccessibilityFontNameKey", "_NSAccessibilityFontFamilyKey", "_NSAccessibilityVisibleNameKey",
    "_NSAccessibilityFontTextAttribute", "_NSAccessibilityForegroundColorTextAttribute", "_NSAccessibilityHeaderAttribute",
    "_NSAccessibilityHelpAttribute", "_NSAccessibilityHelpTagCreatedNotification", "_NSAccessibilityHorizontalOrientationValue",
    "_NSAccessibilityHorizontalScrollBarAttribute", "_NSAccessibilityIndexAttribute",
    "_NSAccessibilityInsertionPointLineNumberAttribute", "_NSAccessibilityLinkedUIElementsAttribute",
    "_NSAccessibilityMainWindowChangedNotification", "_NSAccessibilityMarkedMisspelledTextAttribute",
    "_NSAccessibilityMaxValueAttribute", "_NSAccessibilityMinValueAttribute", "_NSAccessibilityMovedNotification",
    "_NSAccessibilityNumberOfCharactersAttribute", "_NSAccessibilityOrientationAttribute",
    "_NSAccessibilityPlaceholderValueAttribute", "_NSAccessibilityPositionAttribute",
    "_NSAccessibilityRTFForRangeParameterizedAttribute", "_NSAccessibilityRadioGroupRole",
    "_NSAccessibilityRangeForIndexParameterizedAttribute", "_NSAccessibilityRequiredAttribute",
    "_NSAccessibilityResizedNotification", "_NSAccessibilityRoleDescriptionAttribute", "_NSAccessibilityRowCountAttribute",
    "_NSAccessibilityRowCountChangedNotification", "_NSAccessibilityRowHeaderUIElementsAttribute",
    "_NSAccessibilityRowIndexRangeAttribute", "_NSAccessibilityRowsAttribute", "_NSAccessibilitySelectedAttribute",
    "_NSAccessibilitySelectedCellsChangedNotification", "_NSAccessibilitySelectedChildrenAttribute",
    "_NSAccessibilitySelectedChildrenChangedNotification", "_NSAccessibilitySelectedChildrenMovedNotification",
    "_NSAccessibilitySelectedColumnsChangedNotification", "_NSAccessibilitySelectedRowsAttribute",
    "_NSAccessibilitySelectedTextAttribute", "_NSAccessibilitySheetCreatedNotification", "_NSAccessibilitySizeAttribute",
    "_NSAccessibilitySortDirectionAttribute", "_NSAccessibilitySplittersAttribute", "_NSAccessibilityStrikethroughTextAttribute",
    "_NSAccessibilitySubroleAttribute", "_NSAccessibilityTabButtonSubrole", "_NSAccessibilityTabsAttribute",
    "_NSAccessibilityTitleUIElementAttribute", "_NSAccessibilityToolbarButtonAttribute",
    "_NSAccessibilityTopLevelUIElementAttribute", "_NSAccessibilityURLAttribute", "_NSAccessibilityUnderlineTextAttribute",
    "_NSAccessibilityUnitsChangedNotification", "_NSAccessibilityUnknownSortDirectionValue",
    "_NSAccessibilityValueDescriptionAttribute", "_NSAccessibilityVerticalOrientationValue",
    "_NSAccessibilityVerticalScrollBarAttribute", "_NSAccessibilityVisibleCellsAttribute",
    "_NSAccessibilityVisibleCharacterRangeAttribute", "_NSAccessibilityVisibleChildrenAttribute",
    "_NSAccessibilityVisibleColumnsAttribute", "_NSAccessibilityVisibleRowsAttribute", "_NSAccessibilityWindowCreatedNotification",
    "_NSAccessibilityWindowDeminiaturizedNotification", "_NSAccessibilityWindowMiniaturizedNotification",
    "_NSAccessibilityWindowMovedNotification", "_NSAccessibilityWindowResizedNotification",
    "_NSAccessibilityUIElementsForSearchPredicateParameterizedAttribute"};

// The search-predicate keys follow the AX search predicate dictionary
// (AXUIElementsForSearchPredicate) as WebKit reads it (unverified against
// an AppKit header).
constexpr std::array<std::pair<const char*, const char*>, 7> search_constants{{
    {"_NSAccessibilitySearchCurrentElementKey", "AXStartElement"},
    {"_NSAccessibilitySearchDirectionKey", "AXDirection"},
    {"_NSAccessibilitySearchDirectionNext", "AXDirectionNext"},
    {"_NSAccessibilitySearchDirectionPrevious", "AXDirectionPrevious"},
    {"_NSAccessibilitySearchIdentifiersKey", "AXSearchKey"},
    {"_NSAccessibilitySearchResultsLimitKey", "AXResultsLimit"},
    {"_NSAccessibilitySearchTextKey", "AXSearchText"},
}};

// NSAccessibilityActionDescription: the standard actions' descriptions.
constexpr std::array<std::pair<const char*, const char*>, 11> action_descriptions{{
    {"AXPress", "press"}, {"AXIncrement", "increment"}, {"AXDecrement", "decrement"}, {"AXConfirm", "confirm"},
    {"AXPick", "pick"}, {"AXCancel", "cancel"}, {"AXRaise", "raise"}, {"AXShowMenu", "show menu"},
    {"AXDelete", "delete"}, {"AXShowAlternateUI", "show alternate UI"}, {"AXShowDefaultUI", "show default UI"},
}};

} // namespace

void Provider::install_appkit() {
    for (const auto* name : accessibility_constants) string(appkit, name, accessibility_value(name));
    for (const auto& [name, text] : search_constants) string(appkit, name, text);
    raw(appkit, "_NSAccessibilityActionDescription", [this](CpuState& s, GuestMemory&) {
        const auto action = s.x[0] && bridge->is_string(s.x[0]) ? cf.utf8(s.x[0]) : std::string{};
        for (const auto& [name, description] : action_descriptions)
            if (action == name) { s.x[0] = bridge->autorelease(cf.string(description)); return; }
        s.x[0] = 0;   // not a standard action
    });

    // ---- string constants ----
    for (const auto* name : {"NSApplicationLaunchUserNotificationKey", "NSMenuDidEndTrackingNotification",
                             "NSWindowWillMoveNotification", "NSWorkspaceScreensDidSleepNotification",
                             "NSWorkspaceScreensDidWakeNotification", "NSWorkspaceWillPowerOffNotification",
                             "NSWorkspaceSessionDidBecomeActiveNotification", "NSWorkspaceSessionDidResignActiveNotification",
                             "NSTextInputReplacementRangeAttributeName"})
        string(appkit, std::string("_") + name, name);
    string(appkit, "_NSMarkedClauseSegmentAttributeName", "NSMarkedClauseSegment");
    string(appkit, "_NSImageNameFolder", "NSFolder");
    // Image hint and Touch Bar identifier values equal their names (unverified).
    for (const auto* name : {"NSImageHintCTM", "NSImageHintInterpolation", "NSImageHintUserInterfaceLayoutDirection",
                             "NSTouchBarItemIdentifierCandidateList", "NSTouchBarItemIdentifierCharacterPicker",
                             "NSTouchBarItemIdentifierFlexibleSpace", "NSTouchBarItemIdentifierFixedSpaceSmall",
                             "NSTouchBarItemIdentifierFixedSpaceLarge", "NSTouchBarItemIdentifierOtherItemsProxy"})
        string(appkit, std::string("_") + name, name);
    string(appkit, "_NSSharingServiceNameAddToSafariReadingList", "com.apple.share.System.add-to-safari-reading-list");
    string(appkit, "_NSSharingServiceNameComposeEmail", "com.apple.share.Mail.compose");
    string(appkit, "_NSSharingServiceNameComposeMessage", "com.apple.share.Messages.compose");
    // BOOL NSPerformService(NSString *itemName, NSPasteboard *pboard): no
    // services are installed, so none is performed.
    constant(appkit, "_NSPerformService", 0);

    // ---- views ----
    const auto visual_effect = cls(appkit, "NSVisualEffectView", "NSView");
    integer_properties(visual_effect, {"material", "blendingMode", "state", "emphasized"});
    object_properties(visual_effect, {"maskImage"});
    method(visual_effect, "isEmphasized", [this](CpuState& s, GuestMemory&) { s.x[0] = stored_integer(s.x[0], "emphasized") != 0; }, false, "B@:");
    method(visual_effect, "interiorBackgroundStyle", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "q@:");   // NSBackgroundStyleNormal
    static_cast<void>(cls(appkit, "NSStatusBarButton", "NSButton"));
    // The private frame views (NSThemeFrame, NSNextStepFrame, _NSThemeWidget)
    // are AppKit's own: darwin/appkit/frame_view.cpp.
    // _NSServiceEntry: a Services-menu entry (private). Chrome looks the class
    // up by name at start-up and replaces its -bundleIdentifier (the providing
    // application's identifier); no services are installed, so no entry is
    // ever created by AppKit.
    const auto service_entry = cls(appkit, "_NSServiceEntry", "NSObject");
    object_properties(service_entry, {"bundleIdentifier"});
    // _NSServicesMenuUpdater (private): builds the Services menu; there are no
    // services, so it never adds an item.
    const auto services_updater = cls(appkit, "_NSServicesMenuUpdater", "NSObject");
    method(services_updater, "populateMenu:withServiceEntries:forDisplay:", [this](CpuState& s, GuestMemory&) {
        if (s.x[3] && cf.length(s.x[3]) != 0)
            throw std::runtime_error("unsupported: Alchemy: _NSServicesMenuUpdater has no Services menu items to build");
    }, false, "v@:@@B");
    const auto glass = cls(appkit, "NSGlassEffectView", "NSView");
    object_properties(glass, {"contentView", "tintColor"});
    real_properties(glass, {"cornerRadius"});

    // ---- text attachments ----
    const auto attachment = cls(appkit, "NSTextAttachment", "NSObject");
    object_properties(attachment, {"contents", "fileType", "image", "fileWrapper", "attachmentCell"});
    method(attachment, "initWithData:ofType:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "contents", s.x[2]);
        store(s.x[0], "fileType", s.x[3]);
    });
    method(attachment, "initWithFileWrapper:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "fileWrapper", s.x[2]); });
    // The attachment's layout bounds (CGRectZero: the image's size at the baseline).
    method(attachment, "bounds", [this](CpuState& s, GuestMemory&) {
        const auto values = properties(s.x[0]);
        std::lock_guard lock(values->mutex);
        for (unsigned i = 0; i < 4; ++i) {
            const auto found = values->reals.find("bounds" + std::to_string(i));
            s.v[i] = {std::bit_cast<std::uint64_t>(found == values->reals.end() ? 0.0 : found->second), 0};
        }
    }, false, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(attachment, "setBounds:", [this](CpuState& s, GuestMemory&) {
        const auto values = properties(s.x[0]);
        std::lock_guard lock(values->mutex);
        for (unsigned i = 0; i < 4; ++i) values->reals["bounds" + std::to_string(i)] = std::bit_cast<double>(s.v[i][0]);
    }, false, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // +[NSAttributedString attributedStringWithAttachment:]: U+FFFC carrying
    // the attachment under NSAttachmentAttributeName ("NSAttachment").
    if (const auto attributed = objc.find_class("NSAttributedString"); attributed && !objc.responds(attributed, objc.selector("attributedStringWithAttachment:"))) {
        static_cast<void>(objc.add_native_method(attributed, "attributedStringWithAttachment:", [this, attributed](CpuState& s, GuestMemory&) {
            const auto text = cf.string("\xEF\xBF\xBC");
            const auto key = cf.string("NSAttachment");
            const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> entries{{{key, s.x[2]}}};
            const auto attributes = cf.dictionary(entries);
            CallRegs registers{};
            registers.x[2] = text;
            registers.x[3] = attributes;
            const auto object = objc.send(objc.allocate(attributed), "initWithString:attributes:", registers).x0;
            release(attributes);
            release(key);
            release(text);
            s.x[0] = bridge->autorelease(object);
        }, true));
    }

    // ---- accessibility custom actions ----
    const auto custom_action = cls(appkit, "NSAccessibilityCustomAction", "NSObject");
    object_properties(custom_action, {"name", "handler"});
    weak_properties(custom_action, {"target"});
    integer_properties(custom_action, {"selector"});
    method(custom_action, "initWithName:target:selector:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "name", s.x[2]);
        store_integer(s.x[0], "target", static_cast<std::int64_t>(s.x[3]));
        store_integer(s.x[0], "selector", static_cast<std::int64_t>(s.x[4]));
    });
    method(custom_action, "initWithName:handler:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "name", s.x[2]);
        if (s.x[3]) {
            const auto block = bridge->block_copy(s.x[3]);
            store(s.x[0], "handler", block);
            bridge->block_release(block);
        }
    });

    // ---- Touch Bar (no Touch Bar hardware: bars are kept, never shown) ----
    const auto touch_bar = cls(appkit, "NSTouchBar", "NSObject");
    object_properties(touch_bar, {"customizationIdentifier", "defaultItemIdentifiers", "customizationAllowedItemIdentifiers",
                                  "customizationRequiredItemIdentifiers", "principalItemIdentifier",
                                  "escapeKeyReplacementItemIdentifier", "templateItems"});
    weak_properties(touch_bar, {"delegate"});
    method(touch_bar, "isVisible", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    method(touch_bar, "itemForIdentifier:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(touch_bar, "isAutomaticCustomizeTouchBarMenuItemEnabled", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "B@:");
    method(touch_bar, "setAutomaticCustomizeTouchBarMenuItemEnabled:", [](CpuState&, GuestMemory&) {}, true, "v@:B");
    const auto item = cls(appkit, "NSTouchBarItem", "NSObject");
    object_properties(item, {"identifier", "customizationLabel"});
    integer_properties(item, {"visibilityPriority"});
    method(item, "initWithIdentifier:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "identifier", s.x[2]); });
    method(item, "isVisible", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    method(item, "view", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    const auto custom_item = cls(appkit, "NSCustomTouchBarItem", "NSTouchBarItem");
    object_properties(custom_item, {"identifier", "customizationLabel", "view", "viewController"});
    const auto group_item = cls(appkit, "NSGroupTouchBarItem", "NSTouchBarItem");
    object_properties(group_item, {"identifier", "customizationLabel", "groupTouchBar"});
    method(group_item, "groupItemWithIdentifier:items:", [this, touch_bar](CpuState& s, GuestMemory&) {
        const auto object = create(s.x[0]);
        store(object, "identifier", s.x[2]);
        const auto bar = create(touch_bar);
        store(bar, "templateItems", s.x[3]);
        store(object, "groupTouchBar", bar);
        release(bar);
        s.x[0] = bridge->autorelease(object);
    }, true);
    const auto candidates = cls(appkit, "NSCandidateListTouchBarItem", "NSTouchBarItem");
    object_properties(candidates, {"identifier", "customizationLabel", "client", "candidates"});
    integer_properties(candidates, {"collapsed", "allowsCollapsing", "allowsTextInputContextCandidates"});
    weak_properties(candidates, {"delegate"});
    method(candidates, "setCandidates:forSelectedRange:inString:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "candidates", s.x[2]);
    }, false, "v@:@{_NSRange=QQ}@");
    method(candidates, "updateWithInsertionPointVisibility:", [](CpuState&, GuestMemory&) {}, false, "v@:B");
    // Color picker, slider and popover items: kept like the others.
    const auto color_picker = cls(appkit, "NSColorPickerTouchBarItem", "NSTouchBarItem");
    object_properties(color_picker, {"identifier", "customizationLabel", "color", "colorList", "allowedColorSpaces", "target"});
    integer_properties(color_picker, {"showsAlpha", "enabled", "action"});
    for (const auto* selector : {"colorPickerWithIdentifier:", "textColorPickerWithIdentifier:", "strokeColorPickerWithIdentifier:"})
        method(color_picker, selector, [this](CpuState& s, GuestMemory&) {
            const auto object = create(s.x[0]);
            store(object, "identifier", s.x[2]);
            store_integer(object, "enabled", 1);
            s.x[0] = bridge->autorelease(object);
        }, true);
    const auto slider_item = cls(appkit, "NSSliderTouchBarItem", "NSTouchBarItem");
    object_properties(slider_item, {"identifier", "customizationLabel", "slider", "label", "target", "minimumValueAccessory",
                                    "maximumValueAccessory"});
    integer_properties(slider_item, {"action"});
    real_properties(slider_item, {"doubleValue", "valueAccessoryWidth"});
    const auto popover_item = cls(appkit, "NSPopoverTouchBarItem", "NSTouchBarItem");
    object_properties(popover_item, {"identifier", "customizationLabel", "popoverTouchBar", "pressAndHoldTouchBar",
                                     "collapsedRepresentation", "collapsedRepresentationImage", "collapsedRepresentationLabel"});
    integer_properties(popover_item, {"showsCloseButton"});
    method(popover_item, "showPopover:", [](CpuState&, GuestMemory&) {}, false, "v@:@");   // no Touch Bar to show it on
    method(popover_item, "dismissPopover:", [](CpuState&, GuestMemory&) {}, false, "v@:@");

    // NSScrubber and its item views: real views (Touch Bar content), never
    // shown. The scrubber keeps its data source, delegate, layout and
    // selection; -reloadData asks the data source nothing, since no item is
    // ever displayed.
    const auto scrubber = cls(appkit, "NSScrubber", "NSView");
    weak_properties(scrubber, {"dataSource", "delegate"});
    object_properties(scrubber, {"scrubberLayout", "selectionBackgroundStyle", "selectionOverlayStyle", "backgroundColor",
                                 "backgroundView"});
    integer_properties(scrubber, {"mode", "itemAlignment", "continuous", "floatsSelectionViews", "showsArrowButtons",
                                  "showsAdditionalContentIndicators"});
    method(scrubber, "selectedIndex", [this](CpuState& s, GuestMemory&) {
        const auto state = properties(s.x[0]);
        std::lock_guard lock(state->mutex);
        const auto found = state->integers.find("selectedIndex");
        s.x[0] = found == state->integers.end() ? static_cast<std::uint64_t>(-1) : static_cast<std::uint64_t>(found->second);   // -1: none selected
    }, false, "q@:");
    method(scrubber, "setSelectedIndex:", [this](CpuState& s, GuestMemory&) {
        store_integer(s.x[0], "selectedIndex", static_cast<std::int64_t>(s.x[2]));
    }, false, "v@:q");
    method(scrubber, "numberOfItems", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "q@:");
    method(scrubber, "reloadData", [](CpuState&, GuestMemory&) {}, false, "v@:");
    method(scrubber, "registerClass:forItemIdentifier:", [](CpuState&, GuestMemory&) {}, false, "v@:#@");
    method(scrubber, "registerNib:forItemIdentifier:", [](CpuState&, GuestMemory&) {}, false, "v@:@@");
    method(scrubber, "makeItemWithIdentifier:owner:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "@@:@@");
    const auto arranged = cls(appkit, "NSScrubberArrangedView", "NSView");
    integer_properties(arranged, {"selected", "highlighted"});
    const auto scrubber_item = cls(appkit, "NSScrubberItemView", "NSScrubberArrangedView");
    const auto text_item = cls(appkit, "NSScrubberTextItemView", "NSScrubberItemView");
    object_properties(text_item, {"title", "textField"});
    const auto image_item = cls(appkit, "NSScrubberImageItemView", "NSScrubberItemView");
    object_properties(image_item, {"image", "imageView"});
    integer_properties(image_item, {"imageAlignment"});
    static_cast<void>(scrubber_item);
    const auto selection_style = cls(appkit, "NSScrubberSelectionStyle", "NSObject");
    singleton(selection_style, "outlineOverlayStyle");
    singleton(selection_style, "roundedBackgroundStyle");

    // NSColorList: named lists of colors. No system list is installed
    // (+availableColorLists is empty, +colorListNamed: nil); a list made by
    // -initWithName: keeps its keys in insertion order.
    const auto color_list = cls(appkit, "NSColorList", "NSObject");
    object_properties(color_list, {"name"});
    method(color_list, "availableColorLists", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); }, true);
    method(color_list, "colorListNamed:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    method(color_list, "initWithName:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "name", s.x[2]);
        const auto keys = cf.array({}, true), colors = cf.dictionary({}, true);
        store(s.x[0], "keys", keys);
        store(s.x[0], "colors", colors);
        release(keys);
        release(colors);
    });
    method(color_list, "isEditable", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "B@:");
    method(color_list, "allKeys", [this](CpuState& s, GuestMemory&) {
        const auto keys = stored(s.x[0], "keys");
        s.x[0] = keys ? bridge->autorelease(cf.copy(keys)) : bridge->autorelease(cf.array({}));
    });
    method(color_list, "colorWithKey:", [this](CpuState& s, GuestMemory&) {
        const auto colors = stored(s.x[0], "colors");
        s.x[0] = colors && s.x[2] ? cf.dictionary_value(colors, s.x[2]) : 0;
    });
    method(color_list, "setColor:forKey:", [this](CpuState& s, GuestMemory&) {
        const auto colors = stored(s.x[0], "colors"), keys = stored(s.x[0], "keys");
        if (!colors || !keys || !s.x[3]) return;
        if (!cf.dictionary_value(colors, s.x[3])) static_cast<void>(cf.invoke("CFArrayAppendValue", std::array<Address, 2>{keys, s.x[3]}));
        static_cast<void>(cf.invoke("CFDictionarySetValue", std::array<Address, 3>{colors, s.x[3], s.x[2]}));
    }, false, "v@:@@");
    // A key that is already in the list moves to the index.
    method(color_list, "insertColor:key:atIndex:", [this](CpuState& s, GuestMemory&) {
        const auto colors = stored(s.x[0], "colors"), keys = stored(s.x[0], "keys");
        if (!colors || !keys || !s.x[3]) return;
        CallRegs same{};
        same.x[2] = s.x[3];
        const auto existing = cf.length(keys);
        for (std::uint64_t i = 0; i < existing; ++i) {
            const auto key = cf.element(keys, i);
            if (key != s.x[3] && !(objc.send(key, "isEqual:", same).x0 & 0xff)) continue;
            static_cast<void>(cf.invoke("CFArrayRemoveValueAtIndex", std::array<Address, 2>{keys, i}));
            break;
        }
        const auto index = std::min<std::uint64_t>(s.x[4], cf.length(keys));
        static_cast<void>(cf.invoke("CFArrayInsertValueAtIndex", std::array<Address, 3>{keys, index, s.x[3]}));
        static_cast<void>(cf.invoke("CFDictionarySetValue", std::array<Address, 3>{colors, s.x[3], s.x[2]}));
    }, false, "v@:@@Q");

    // NSResponder's touchBar (NSTouchBarProvider): kept, never shown. A nil
    // bar is made on first access through -makeTouchBar (nil by default),
    // as AppKit does.
    if (objc.find_class("NSResponder")) {
        track_dealloc("NSResponder");
        // The contextual menu of a responder (NSApplication and
        // NSPopUpButton answer their own).
        extend("NSResponder", "menu", [this](CpuState& s, GuestMemory&) { s.x[0] = stored(s.x[0], "menu"); });
        extend("NSResponder", "setMenu:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "menu", s.x[2]); }, false, "v@:@");
        extend("NSResponder", "makeTouchBar", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        extend("NSResponder", "touchBar", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            auto bar = stored(self, "touchBar");
            if (!bar) {
                bar = objc.send(self, "makeTouchBar").x0;
                if (bar) store(self, "touchBar", bar);
            }
            s.x[0] = bar;
        });
        extend("NSResponder", "setTouchBar:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "touchBar", s.x[2]); }, false, "v@:@");
    }

    // ---- sharing: no sharing services exist ----
    const auto sharing = cls(appkit, "NSSharingService", "NSObject");
    method(sharing, "sharingServiceNamed:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    method(sharing, "sharingServicesForItems:", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); }, true);
    const auto picker = cls(appkit, "NSSharingServicePicker", "NSObject");
    object_properties(picker, {"items"});
    weak_properties(picker, {"delegate"});
    method(picker, "initWithItems:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "items", s.x[2]); });
    const auto preview_item = cls(appkit, "NSPreviewRepresentingActivityItem", "NSObject");
    object_properties(preview_item, {"item", "title", "imageProvider", "iconProvider"});
    method(preview_item, "initWithItem:title:image:icon:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "item", s.x[2]);
        store(s.x[0], "title", s.x[3]);
    });
    method(preview_item, "initWithItem:title:imageProvider:iconProvider:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "item", s.x[2]);
        store(s.x[0], "title", s.x[3]);
        store(s.x[0], "imageProvider", s.x[4]);
        store(s.x[0], "iconProvider", s.x[5]);
    });
}

} // namespace alchemy::gui::values
