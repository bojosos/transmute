#include "alchemy/gui/value_provider.hpp"

#include <array>
#include <bit>
#include <initializer_list>
#include <stdexcept>
#include <string>
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
//   no system sharing service exists (+sharingServiceNamed: is nil, the list
//   empty). An application's own services
//   (-initWithTitle:image:alternateImage:handler:) are real: -performWithItems:
//   tells the delegate and runs the handler, and NSSharingServicePicker shows
//   the services its delegate offers as a menu at the given rect;
// - NSHapticFeedbackManager: the default performer does nothing, as AppKit's
//   does on a Mac without a Force Touch trackpad;
// - NSHelpManager: context help is kept per object; no help is shown
//   (-showContextHelpForObject:locationHint: answers NO), help books are not
//   registered (NO), and opening or searching a help book stops explicitly
//   (there is no Help Viewer).
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
    // The media apps' names (VLC, mpv, OBS, Firefox; Krita). Values follow
    // Apple's naming (unverified) unless noted; the About panel options are the
    // keys -orderFrontStandardAboutPanelWithOptions: reads (application.cpp).
    for (const auto* name : {"NSWorkspaceApplicationKey", "NSWorkspaceWillSleepNotification",
                             "NSWorkspaceDidTerminateApplicationNotification", "NSWindowDidChangeScreenProfileNotification"})
        string(appkit, std::string("_") + name, name);
    for (const auto* name : {"_NSAccessibilityCloseButtonAttribute", "_NSAccessibilityFullScreenButtonAttribute",
                             "_NSAccessibilityMinimizeButtonAttribute", "_NSAccessibilityZoomButtonAttribute",
                             "_NSAccessibilityUnknownOrientationValue"})
        string(appkit, name, accessibility_value(name));
    for (const auto* name : {"AudioInput", "Compose", "GoBack", "GoForward", "Pause", "Play", "SkipAhead", "SkipBack"})
        string(appkit, std::string("_NSImageNameTouchBar") + name + "Template", std::string("NSTouchBar") + name + "Template");
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_NSImageNameNetwork", "NSNetwork"}, {"_NSWorkspaceRecycleOperation", "recycle"},
             {"_NSVerticalGlyphFormAttributeName", "CTVerticalForms"}, {"_NSCharacterEncodingDocumentOption", "CharacterEncoding"},
             {"_NSPrintPanelAccessorySummaryItemNameKey", "name"}, {"_NSPrintPanelAccessorySummaryItemDescriptionKey", "description"},
             {"_NSAppearanceNameAccessibilityHighContrastVibrantLight", "NSAppearanceNameAccessibilityVibrantLight"},
             {"_NSAppearanceNameAccessibilityHighContrastVibrantDark", "NSAppearanceNameAccessibilityVibrantDark"},
             {"_NSAboutPanelOptionApplicationIcon", "ApplicationIcon"}, {"_NSAboutPanelOptionApplicationName", "ApplicationName"},
             {"_NSAboutPanelOptionApplicationVersion", "ApplicationVersion"}})
        string(appkit, symbol, value);
    // NSDarkGray (1/3 of the NSBlack-NSWhite range) and NSAppKitVersionNumber,
    // the macOS 14 value AppKit reports (NSAppKitVersionNumber14_0, unverified).
    value(appkit, "_NSDarkGray", std::bit_cast<std::uint64_t>(1.0 / 3.0));
    value(appkit, "_NSAppKitVersionNumber", std::bit_cast<std::uint64_t>(2487.0));
    // BOOL NSApplicationLoad(void): Cocoa from Carbon code; the shared application is made.
    raw(appkit, "_NSApplicationLoad", [this](CpuState& s, GuestMemory&) {
        const auto application_class = objc.find_class("NSApplication");
        s.x[0] = application_class && objc.send(application_class, objc.selector("sharedApplication")).x0 ? 1 : 0;
    });
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
    // NSStatusBarButton: darwin/appkit/status_items.cpp.
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
    real_properties(attachment, {"lineLayoutPadding"});
    // -allowsTextAttachmentView is YES until set (text attachment views are kept, not shown).
    method(attachment, "allowsTextAttachmentView", [this](CpuState& s, GuestMemory&) { s.x[0] = stored_integer(s.x[0], "noAttachmentView") ? 0 : 1; }, false, "B@:");
    method(attachment, "setAllowsTextAttachmentView:", [this](CpuState& s, GuestMemory&) { store_integer(s.x[0], "noAttachmentView", (s.x[2] & 0xff) ? 0 : 1); },
           false, "v@:B");
    // The image: the one set, else one read from the contents or the file wrapper's data.
    const auto attachment_image = [this](Address self) -> Address {
        if (const auto image = stored(self, "image")) return image;
        Address data = stored(self, "contents");
        if (!data) if (const auto wrapper = stored(self, "fileWrapper")) data = objc.send(wrapper, objc.selector("regularFileContents")).x0;
        if (!data) return 0;
        const auto image_class = objc.find_class("NSImage");
        const auto made = objc.send(objc.send(image_class, objc.selector("alloc")).x0, objc.selector("initWithData:"), [&] { CallRegs r; r.x[2] = data; return r; }()).x0;
        return made ? objc.send(made, objc.selector("autorelease")).x0 : 0;
    };
    method(attachment, "imageForBounds:textContainer:characterIndex:", [attachment_image](CpuState& s, GuestMemory&) { s.x[0] = attachment_image(s.x[0]); }, false,
           "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@Q");
    // The bounds set, else the image's size at the baseline (CGRectZero without an image).
    method(attachment, "attachmentBoundsForTextContainer:proposedLineFragment:glyphPosition:characterIndex:", [this, attachment_image](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto bounds = objc.send(self, objc.selector("bounds"));
        if (bounds.d(2) != 0 || bounds.d(3) != 0) {
            for (unsigned i = 0; i < 4; ++i) s.v[i] = {std::bit_cast<std::uint64_t>(bounds.d(i)), 0};
            return;
        }
        double width = 0, height = 0;
        if (const auto image = attachment_image(self)) {
            const auto size = objc.send(image, objc.selector("size"));
            width = size.d(0);
            height = size.d(1);
        }
        for (unsigned i = 0; i < 4; ++i) s.v[i] = {std::bit_cast<std::uint64_t>(i == 2 ? width : i == 3 ? height : 0.0), 0};
    }, false, "{CGRect={CGPoint=dd}{CGSize=dd}}@:@{CGRect={CGPoint=dd}{CGSize=dd}}{CGPoint=dd}Q");
    method(attachment, "textAttachmentWithImage:", [this, attachment](CpuState& s, GuestMemory&) {
        const auto made = create(s.x[0] ? s.x[0] : attachment);
        store(made, "image", s.x[2]);
        s.x[0] = objc.send(made, objc.selector("autorelease")).x0;
    }, true, "@@:@");
    // Text attachment view providers by file type (kept: attachment views are not shown).
    static std::map<std::string, Address> view_providers;
    method(attachment, "registerTextAttachmentViewProviderClass:forFileType:", [this](CpuState& s, GuestMemory&) {
        if (s.x[3]) view_providers[cf.utf8(s.x[3])] = s.x[2];
    }, true, "v@:#@");
    method(attachment, "textAttachmentViewProviderClassForFileType:", [this](CpuState& s, GuestMemory&) {
        const auto found = s.x[2] ? view_providers.find(cf.utf8(s.x[2])) : view_providers.end();
        s.x[0] = found == view_providers.end() ? 0 : found->second;
    }, true, "#@:@");
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
    // TextKit 2 content storage (OBS): the text lives in an NSTextStorage
    // (made on first use) that -attributedString answers, with the delegate
    // kept. TextKit 2 layout (NSTextLayoutManager) is not provided, so the
    // content manager has no layout managers and adding one stops explicitly.
    const auto content_manager = cls(appkit, "NSTextContentManager", "NSObject");
    weak_properties(content_manager, {"delegate"});
    method(content_manager, "textLayoutManagers", [this](CpuState& s, GuestMemory&) {
        s.x[0] = objc.send(objc.send(objc.find_class("NSArray"), objc.selector("array")).x0, objc.selector("self")).x0;
    });
    method(content_manager, "addTextLayoutManager:", [](CpuState&, GuestMemory&) {
        throw std::runtime_error("unsupported: Alchemy: -[NSTextContentManager addTextLayoutManager:] (TextKit 2 layout is not provided)");
    }, false, "v@:@");
    const auto content_storage = cls(appkit, "NSTextContentStorage", "NSTextContentManager");
    const auto text_storage_of = [this](Address self) {
        auto value = stored(self, "textStorage");
        if (!value) {
            if (const auto storage_class = objc.find_class("NSTextStorage")) {
                const auto made = create(storage_class);
                store(self, "textStorage", made);
                objc.send(made, objc.selector("release"));
                value = made;
            }
        }
        return value;
    };
    method(content_storage, "textStorage", [text_storage_of](CpuState& s, GuestMemory&) { s.x[0] = text_storage_of(s.x[0]); });
    method(content_storage, "setTextStorage:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "textStorage", s.x[2]); }, false, "v@:@");
    method(content_storage, "attributedString", [text_storage_of](CpuState& s, GuestMemory&) { s.x[0] = text_storage_of(s.x[0]); });
    // Touch Bars are never shown (no Touch Bar), so the sharing item only keeps its settings.
    const auto sharing_item = cls(appkit, "NSSharingServicePickerTouchBarItem", "NSTouchBarItem");
    object_properties(sharing_item, {"identifier", "customizationLabel", "delegate", "buttonTitle", "buttonImage"});
    integer_properties(sharing_item, {"enabled"});
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

    // ---- sharing: no system services; an application's own services ----
    const auto msg = [this](Address receiver, const char* selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers{};
        unsigned index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return objc.send(receiver, selector, registers).x0;
    };
    const auto responds = [this](Address object, const char* selector) { return object && objc.responds(object, objc.selector(selector)); };
    const auto sharing = cls(appkit, "NSSharingService", "NSObject");
    method(sharing, "sharingServiceNamed:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    method(sharing, "sharingServicesForItems:", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); }, true);
    object_properties(sharing, {"subject", "recipients", "attachmentFileURLs", "permanentLink", "messageBody", "accountName"});
    weak_properties(sharing, {"delegate"});
    method(sharing, "initWithTitle:image:alternateImage:handler:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[5]) throw std::invalid_argument("Alchemy: -[NSSharingService initWithTitle:image:alternateImage:handler:] needs a title and a handler");
        store(s.x[0], "title", s.x[2]);
        store(s.x[0], "image", s.x[3]);
        store(s.x[0], "alternateImage", s.x[4]);
        const auto handler = bridge->block_copy(s.x[5]);
        store(s.x[0], "handler", handler);
        bridge->block_release(handler);
    });
    for (const auto* name : {"title", "image", "alternateImage"})
        method(sharing, name, [this, key = std::string(name)](CpuState& s, GuestMemory&) { s.x[0] = stored(s.x[0], key); });
    // The menu item title is the title until one is set.
    method(sharing, "menuItemTitle", [this](CpuState& s, GuestMemory&) {
        const auto title = stored(s.x[0], "menuItemTitle");
        s.x[0] = title ? title : stored(s.x[0], "title");
    });
    method(sharing, "setMenuItemTitle:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "menuItemTitle", s.x[2]); }, false, "v@:@");
    method(sharing, "canPerformWithItems:", [this](CpuState& s, GuestMemory&) { s.x[0] = stored(s.x[0], "handler") ? 1 : 0; }, false, "B@:@");
    method(sharing, "performWithItems:", [this, msg, responds](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], items = s.x[2];
        const auto handler = stored(self, "handler");
        if (!handler) throw std::logic_error("Alchemy: -[NSSharingService performWithItems:] on a service without a handler");
        const auto delegate = static_cast<Address>(stored_integer(self, "delegate"));
        if (responds(delegate, "sharingService:willShareItems:")) static_cast<void>(msg(delegate, "sharingService:willShareItems:", {self, items}));
        static_cast<void>(bridge->invoke_block(handler, {}));
        if (responds(delegate, "sharingService:didShareItems:")) static_cast<void>(msg(delegate, "sharingService:didShareItems:", {self, items}));
    }, false, "v@:@");
    const auto picker = cls(appkit, "NSSharingServicePicker", "NSObject");
    object_properties(picker, {"items"});
    weak_properties(picker, {"delegate"});
    method(picker, "initWithItems:", [this](CpuState& s, GuestMemory&) { store(s.x[0], "items", s.x[2]); });
    // -showRelativeToRect:ofView:preferredEdge:: the services the delegate
    // offers (there are no proposed system services) as a menu below the
    // rect. Choosing one tells the delegate, gives the service the delegate's
    // service delegate and performs it; dismissing the menu (or no services)
    // tells the delegate with nil.
    method(picker, "showRelativeToRect:ofView:preferredEdge:", [this, msg, responds](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], view = s.x[2];
        const double x = std::bit_cast<double>(s.v[0][0]), y = std::bit_cast<double>(s.v[1][0]), height = std::bit_cast<double>(s.v[3][0]);
        const auto items = stored(self, "items");
        const auto delegate = static_cast<Address>(stored_integer(self, "delegate"));
        auto services = bridge->autorelease(cf.array({}));
        if (responds(delegate, "sharingServicePicker:sharingServicesForItems:proposedSharingServices:"))
            services = msg(delegate, "sharingServicePicker:sharingServicesForItems:proposedSharingServices:", {self, items, services});
        const auto count = services ? cf.length(services) : 0;
        const auto dismissed = [&] {
            if (responds(delegate, "sharingServicePicker:didChooseSharingService:"))
                static_cast<void>(msg(delegate, "sharingServicePicker:didChooseSharingService:", {self, 0}));
        };
        if (count == 0 || !view) { dismissed(); return; }
        const auto menu = msg(msg(objc.find_class("NSMenu"), "alloc"), "initWithTitle:", {bridge->autorelease(cf.string(""))});
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto service = cf.element(services, i);
            const auto item = msg(msg(objc.find_class("NSMenuItem"), "alloc"), "initWithTitle:action:keyEquivalent:",
                                  {msg(service, "menuItemTitle"), objc.selector("_alchemyShareWithService:"), bridge->autorelease(cf.string(""))});
            static_cast<void>(msg(item, "setTarget:", {self}));
            static_cast<void>(msg(item, "setRepresentedObject:", {service}));
            if (const auto image = msg(service, "image")) static_cast<void>(msg(item, "setImage:", {image}));
            static_cast<void>(msg(menu, "addItem:", {item}));
            release(item);
        }
        // The menu's top left at the rect's bottom left (the top left in a flipped view).
        const bool flipped = (msg(view, "isFlipped") & 0xff) != 0;
        CallRegs popup{};
        popup.x[2] = 0;
        popup.set_d(0, x);
        popup.set_d(1, flipped ? y + height : y);
        popup.x[3] = view;
        const bool chosen = (objc.send(menu, "popUpMenuPositioningItem:atLocation:inView:", popup).x0 & 0xff) != 0;
        release(menu);
        if (!chosen) dismissed();
    }, false, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@Q");
    method(picker, "_alchemyShareWithService:", [this, msg, responds](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto service = msg(s.x[2], "representedObject");
        const auto delegate = static_cast<Address>(stored_integer(self, "delegate"));
        if (responds(delegate, "sharingServicePicker:didChooseSharingService:"))
            static_cast<void>(msg(delegate, "sharingServicePicker:didChooseSharingService:", {self, service}));
        if (responds(delegate, "sharingServicePicker:delegateForSharingService:"))
            static_cast<void>(msg(service, "setDelegate:", {msg(delegate, "sharingServicePicker:delegateForSharingService:", {self, service})}));
        static_cast<void>(msg(service, "performWithItems:", {stored(self, "items")}));
    }, false, "v@:@");
    method(picker, "close", [](CpuState&, GuestMemory&) {}, false, "v@:");   // the menu closes when it is chosen from

    // ---- haptics: no Force Touch trackpad ----
    const auto haptics = cls(appkit, "NSHapticFeedbackManager", "NSObject");
    const auto performer = cls(appkit, "_NSHapticFeedbackPerformer", "NSObject");
    method(performer, "performFeedbackPattern:performanceTime:", [](CpuState&, GuestMemory&) {}, false, "v@:qQ");
    method(haptics, "defaultPerformer", [this, performer](CpuState& s, GuestMemory&) {
        static Address shared{};
        if (!shared) shared = create(performer);   // kept for the process
        s.x[0] = shared;
    }, true);

    // ---- help: context help kept, no Help Viewer ----
    const auto help = cls(appkit, "NSHelpManager", "NSObject");
    singleton(help, "sharedHelpManager");
    static bool context_mode{};
    method(help, "isContextHelpModeActive", [](CpuState& s, GuestMemory&) { s.x[0] = context_mode ? 1 : 0; }, true, "B@:");
    method(help, "setContextHelpModeActive:", [](CpuState& s, GuestMemory&) { context_mode = (s.x[2] & 0xff) != 0; }, true, "v@:B");
    // Context help per object: kept in the manager's own storage, keyed by the object's address.
    const auto help_key = [](Address object) { return "help:" + std::to_string(object); };
    method(help, "setContextHelp:forObject:", [this, help_key](CpuState& s, GuestMemory&) { store(s.x[0], help_key(s.x[3]), s.x[2]); }, false, "v@:@@");
    method(help, "removeContextHelpForObject:", [this, help_key](CpuState& s, GuestMemory&) { store(s.x[0], help_key(s.x[2]), 0); }, false, "v@:@");
    method(help, "contextHelpForObject:", [this, help_key](CpuState& s, GuestMemory&) { s.x[0] = stored(s.x[0], help_key(s.x[2])); });
    method(help, "showContextHelpForObject:locationHint:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:@{CGPoint=dd}");
    method(help, "registerBooksInBundle:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:@");
    for (const auto* name : {"openHelpAnchor:inBook:", "findString:inBook:"})
        method(help, name, [selector = std::string(name)](CpuState&, GuestMemory&) {
            throw std::runtime_error("unsupported: Alchemy: -[NSHelpManager " + selector + "] (no Help Viewer)");
        }, false, "v@:@@");
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
