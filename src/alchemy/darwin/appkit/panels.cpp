#include "alchemy/darwin/appkit/controls_internal.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

#include "alchemy/host/ui.hpp"

#include <filesystem>

// AppKit classes that applications reference or subclass around their
// controls, beyond the drawn controls themselves:
//  * NSPanel (NSWindow with the panel properties; a floating panel is a
//    floating-level window);
//  * NSToolbar / NSToolbarItem as objects (identifiers, items, delegate
//    item creation); a window does not show a toolbar, so -[NSWindow
//    setToolbar:] with a toolbar fails explicitly;
//  * NSSavePanel / NSOpenPanel: the Windows common item dialog (file
//    types from allowed extensions, UTIs and UTTypes; title, prompt, name
//    label, directory and name; files, folders, multiple selection),
//    modal or as a sheet / with a completion handler;
//  * NSPrintInfo (paper, margins, orientation), while NSPrintPanel /
//    NSPageLayout / NSColorPanel / NSFontPanel fail when shown;
//  * NSGestureRecognizer and its Click / Pan / Press / Magnification /
//    Rotation subclasses as objects attached to views (the window server
//    reports no gestures, so they stay in NSGestureRecognizerStatePossible);
//  * NSPasteboardItem, NSDraggingItem;
//  * NSAnimation / NSViewAnimation (run to completion when started:
//    frames are interpolated with redisplay, fades show or hide the target);
//  * NSLayoutManager line metrics;
//  * a few constants and NSAccessibilityUnignoredDescendant.
namespace alchemy::gui::appkit {
namespace {
std::mutex file_dialog_mutex;
FileDialogPresenter file_dialog_override;

// Filename extensions of a uniform type identifier: the application's
// exported / imported type declarations, then the common public types; an
// unknown identifier allows any file.
std::optional<std::vector<std::string>> extensions_for_uti(AppKit& kit, const std::string& uti) {
    if (uti == "public.data" || uti == "public.item" || uti == "public.content") return std::vector<std::string>{};
    const auto bundle = kit.send(kit.objc.find_class("NSBundle"), "mainBundle").x0;
    const auto info = bundle ? kit.send(bundle, "infoDictionary").x0 : 0;
    for (const auto* key : {"UTExportedTypeDeclarations", "UTImportedTypeDeclarations"}) {
        const auto list = info ? kit.send_id(info, "objectForKey:", {kit.auto_string(key)}) : 0;
        for (const auto entry : kit.items(list)) {
            if (kit.text(kit.send_id(entry, "objectForKey:", {kit.auto_string("UTTypeIdentifier")})) != uti) continue;
            const auto tags = kit.send_id(entry, "objectForKey:", {kit.auto_string("UTTypeTagSpecification")});
            const auto value = tags ? kit.send_id(tags, "objectForKey:", {kit.auto_string("public.filename-extension")}) : 0;
            if (!value) continue;
            if (kit.cf.kind(value) == ObjectKind::string) return std::vector<std::string>{kit.text(value)};
            std::vector<std::string> result;
            for (const auto item : kit.items(value)) result.push_back(kit.text(item));
            return result;
        }
    }
    static const std::map<std::string, std::vector<std::string>> common{
        {"public.plain-text", {"txt"}}, {"public.text", {"txt"}}, {"public.json", {"json"}}, {"public.xml", {"xml"}},
        {"public.comma-separated-values-text", {"csv"}}, {"public.tab-separated-values-text", {"tsv", "txt"}},
        {"com.adobe.pdf", {"pdf"}}, {"public.png", {"png"}}, {"public.jpeg", {"jpg", "jpeg"}}, {"public.tiff", {"tif", "tiff"}},
        {"com.compuserve.gif", {"gif"}}, {"public.heic", {"heic"}},
        {"public.image", {"png", "jpg", "jpeg", "tif", "tiff", "gif", "bmp", "heic", "psd"}},
        {"public.audio", {"wav", "aif", "aiff", "aifc", "mp3", "m4a", "caf", "flac", "aac"}},
        {"public.movie", {"mov", "mp4", "m4v", "avi", "mpg", "mpeg"}}, {"public.mpeg-4", {"mp4"}},
        {"com.apple.quicktime-movie", {"mov"}}, {"com.microsoft.waveform-audio", {"wav"}}, {"public.aiff-audio", {"aif", "aiff"}},
        {"public.mp3", {"mp3"}}, {"public.midi-audio", {"mid", "midi"}}, {"public.zip-archive", {"zip"}},
        {"com.apple.property-list", {"plist"}}, {"public.rtf", {"rtf"}}, {"public.html", {"html", "htm"}}};
    if (const auto found = common.find(uti); found != common.end()) return found->second;
    return std::nullopt;
}

#ifdef _WIN32
// The host's file dialog (host::ui::run_file_dialog; on win32 the common item
// dialog, IFileOpenDialog / IFileSaveDialog).
std::optional<std::vector<std::string>> run_common_dialog(AppKit& kit, const FileDialogRequest& request) {
    host::ui::FileDialog dialog;
    dialog.save = request.save;
    dialog.folders = !request.save && request.choose_directories && !request.choose_files;
    dialog.multiple = request.multiple;
    dialog.show_hidden = request.shows_hidden;
    dialog.title = request.title.empty() ? request.message : request.title;
    dialog.ok_label = request.prompt;
    dialog.name_label = request.name_label;
    dialog.name = request.name;
    if (!request.directory.empty()) dialog.directory = kit.host_path(request.directory);
    dialog.extensions = request.extensions;
    dialog.allow_other_types = request.allows_other_types;
    dialog.owner = reinterpret_cast<std::uintptr_t>(request.owner);
    const auto chosen = host::ui::run_file_dialog(dialog);
    if (!chosen) fail("the host file dialog failed (" + std::string(host::name(chosen.error())) + ")");
    if (!*chosen) return std::nullopt;
    std::vector<std::string> paths;
    for (const auto& path : **chosen) {
        const std::u8string text(path.begin(), path.end());
        paths.push_back(guest_path_string(std::filesystem::path(text)));
    }
    return paths;
}
#endif

struct ToolbarState final : ObjectPayload {
    Address identifier{}, delegate{};         // identifier +1
    std::vector<Address> items;               // +1
    Address selected{};                       // +1 selected item identifier
    Address default_identifiers{}, principal{};   // +1 -defaultItemIdentifiers, -principalItemIdentifier
    std::uint64_t display_mode{}, size_mode{};
    bool visible{true}, customizable{}, autosaves{}, baseline_separator{true};
};
struct ToolbarItemState final : ObjectPayload {
    Address identifier{}, label{}, palette_label{}, tool_tip{}, image{}, view{}, menu_form{}, title{};   // +1
    Address target{}, action{}, toolbar{};
    std::int64_t tag{}, visibility_priority{}, style{};   // style: NSToolbarItemStyle
    bool hidden{};
    Size min_size, max_size;
    bool enabled{true}, autovalidates{true}, bordered{};
};
struct SavePanelState final : ObjectPayload {
    Address directory{}, name{}, title{}, message{}, prompt{}, allowed_types{}, accessory{}, delegate{};   // +1 except delegate
    Address name_label{}, content_types{}, tag_names{};   // +1
    std::vector<Address> results;             // +1 NSURLs chosen by the last run
    bool can_select_hidden_extension{}, shows_tag_field{true}, running{};
    bool can_create_directories{true}, shows_hidden{}, extension_hidden{true}, can_choose_files{true}, can_choose_directories{};
    bool multiple{}, resolves_aliases{true}, treats_packages_as_directories{}, allows_other_types{};
};
struct PrintInfoState final : ObjectPayload {
    Size paper{612, 792};                     // US Letter, points
    double left{72}, right{72}, top{90}, bottom{90}, scaling{1};
    std::int64_t orientation{}, horizontal_pagination{2}, vertical_pagination{0};
    bool horizontally_centered{true}, vertically_centered{true};
    Address paper_name{}, printer{};          // +1
};
struct GestureState final : ObjectPayload {
    Address target{}, action{}, delegate{}, view{};
    std::int64_t state{};                     // NSGestureRecognizerStatePossible
    std::uint64_t button_mask{1}, allowed_touch_types{};
    std::int64_t clicks{1}, touches{1};
    double magnification{}, rotation{}, press_duration{0.5}, allowable_movement{10};
    Point translation;
    bool enabled{true}, delays_primary{}, delays_secondary{}, delays_other{}, delays_key{}, delays_magnification{}, delays_rotation{};
};
struct PasteboardItemState final : ObjectPayload {
    std::vector<std::pair<std::string, Address>> values;   // type -> +1 NSString / NSData / property list
    std::vector<std::pair<std::string, Address>> providers;   // type -> data provider (retained +1)
};
struct DraggingItemState final : ObjectPayload {
    Address item{}, contents{};               // +1
    Rect frame;
    Address components_provider{};            // +1 block
};
struct AnimationState final : ObjectPayload {
    double duration{0.25}, progress{}, frame_rate{60};
    std::uint64_t curve{}, blocking_mode{};
    Address delegate{};
    Address view_animations{};                // +1 NSArray of dictionaries (NSViewAnimation)
    bool animating{};
    std::vector<float> marks;                 // progress marks, ascending
};

// NSAnimationCurve applied to a progress in [0, 1].
double eased(std::uint64_t curve, double t) {
    switch (curve) {   // NSAnimationEaseInOut, EaseIn, EaseOut, Linear
        case 0: return t * t * (3 - 2 * t);
        case 1: return t * t;
        case 2: return 1 - (1 - t) * (1 - t);
        default: return t;
    }
}

Rect rect_value(AppKit& kit, Address value) {
    if (!value) return {};
    return result_rect(kit.send(value, "rectValue"));
}
} // namespace

void AppKit::install_panels() {
    // ---- constants ----
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, std::string_view>>{
             {"_NSCalibratedWhiteColorSpace", "NSCalibratedWhiteColorSpace"},
             {"_NSDeviceRGBColorSpace", "NSDeviceRGBColorSpace"},
             {"_NSDeviceWhiteColorSpace", "NSDeviceWhiteColorSpace"},
             {"_NSPatternColorSpace", "NSPatternColorSpace"},
             {"_NSNamedColorSpace", "NSNamedColorSpace"},
             {"_NSGeneralPboard", "Apple CFPasteboard general"},
             {"_NSDragPboard", "Apple CFPasteboard drag"},
             {"_NSToolbarFlexibleSpaceItemIdentifier", "NSToolbarFlexibleSpaceItem"},
             {"_NSToolbarSpaceItemIdentifier", "NSToolbarSpaceItem"},
             {"_NSToolbarSeparatorItemIdentifier", "NSToolbarSeparatorItem"},
             {"_NSToolbarWillAddItemNotification", "NSToolbarWillAddItemNotification"},
             {"_NSToolbarDidRemoveItemNotification", "NSToolbarDidRemoveItemNotification"},
             {"_NSViewAnimationTargetKey", "NSViewAnimationTargetKey"},
             {"_NSViewAnimationStartFrameKey", "NSViewAnimationStartFrameKey"},
             {"_NSViewAnimationEndFrameKey", "NSViewAnimationEndFrameKey"},
             {"_NSViewAnimationEffectKey", "NSViewAnimationEffectKey"},
             {"_NSViewAnimationFadeInEffect", "NSViewAnimationFadeInEffect"},
             {"_NSViewAnimationFadeOutEffect", "NSViewAnimationFadeOutEffect"},
             {"_NSAnimationProgressMarkNotification", "NSAnimationProgressMarkNotification"},
             {"_NSPrintPaperName", "NSPaperName"}, {"_NSPrintPaperSize", "NSPaperSize"},
             {"_NSPrintOrientation", "NSOrientation"}, {"_NSPrintScalingFactor", "NSScalingFactor"},
             {"_NSPrintLeftMargin", "NSLeftMargin"}, {"_NSPrintRightMargin", "NSRightMargin"},
             {"_NSPrintTopMargin", "NSTopMargin"}, {"_NSPrintBottomMargin", "NSBottomMargin"}})
        string_constant(symbol, value);
    // NSAccessibilityUnignoredDescendant: the element itself unless it says
    // it is ignored, else the first unignored descendant through its
    // accessibility children (nil when there is none).
    for (const auto library : {appkit_library, cocoa_library})
        function(library, "_NSAccessibilityUnignoredDescendant", [this](CpuState& s, GuestMemory&) {
            std::function<Address(Address, int)> walk = [this, &walk](Address element, int depth) -> Address {
                if (!element || depth > 32) return 0;
                const bool ignored = responds(element, "accessibilityIsIgnored") ? send_bool(element, "accessibilityIsIgnored")
                                     : responds(element, "isAccessibilityElement") ? !send_bool(element, "isAccessibilityElement")
                                                                                   : false;
                if (!ignored) return element;
                if (!responds(element, "accessibilityChildren")) return 0;
                for (const auto child : items(send(element, "accessibilityChildren").x0))
                    if (const auto found = walk(child, depth + 1)) return found;
                return 0;
            };
            s.x[0] = walk(s.x[0], 0);
        });

    // ---- NSPanel ----
    panel = declare_class("NSPanel", "NSWindow");
    const auto flag = [this](const char* getter, const char* setter, bool WindowState::*member) {
        method(panel, getter, [this, member](CpuState& s, GuestMemory&) { return_bool(s, (*window_state(s.x[0])).*member); }, "B@:");
        method(panel, setter, [this, member](CpuState& s, GuestMemory&) { (*window_state(s.x[0])).*member = bool_arg(s, 2); }, "v@:B");
    };
    flag("becomesKeyOnlyIfNeeded", "setBecomesKeyOnlyIfNeeded:", &WindowState::becomes_key_only_if_needed);
    flag("worksWhenModal", "setWorksWhenModal:", &WindowState::works_when_modal);
    method(panel, "isFloatingPanel", [this](CpuState& s, GuestMemory&) { return_bool(s, window_state(s.x[0])->floating_panel); }, "B@:");
    method(panel, "setFloatingPanel:", [this](CpuState& s, GuestMemory&) {
        const bool floating = bool_arg(s, 2);
        window_state(s.x[0])->floating_panel = floating;
        static_cast<void>(send_id(s.x[0], "setLevel:", {floating ? 3u : 0u}));   // NSFloatingWindowLevel / NSNormalWindowLevel
    }, "v@:B");
    // Panels do not become the main window (AppKit's NSPanel answer).
    method(panel, "canBecomeMainWindow", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");

    // ---- NSToolbar / NSToolbarItem ----
    toolbar = declare_class("NSToolbar", "NSObject");
    toolbar_item = declare_class("NSToolbarItem", "NSObject");
    const auto tb = [this](Address value) { return objc.host_state<ToolbarState>(value); };
    const auto ti = [this](Address value) { return objc.host_state<ToolbarItemState>(value); };
    method(toolbar, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithIdentifier:", {auto_string("")}); });
    method(toolbar, "initWithIdentifier:", [this, tb](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], toolbar, sel("init")).x0;
        s.x[0] = self;
        release(std::exchange(tb(self)->identifier, s.x[2] ? cf.copy(s.x[2]) : string("")));
    });
    method(toolbar, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<ToolbarState>(s.x[0])) {
            release(std::exchange(state->identifier, 0));
            release(std::exchange(state->selected, 0));
            release(std::exchange(state->default_identifiers, 0));
            release(std::exchange(state->principal, 0));
            for (const auto item : std::exchange(state->items, {})) release(item);
        }
        static_cast<void>(objc.send_super(s.x[0], toolbar, sel("dealloc")));
    }, "v@:");
    method(toolbar, "identifier", [tb](CpuState& s, GuestMemory&) { s.x[0] = tb(s.x[0])->identifier; });
    method(toolbar, "delegate", [tb](CpuState& s, GuestMemory&) { s.x[0] = tb(s.x[0])->delegate; });
    method(toolbar, "setDelegate:", [tb](CpuState& s, GuestMemory&) { tb(s.x[0])->delegate = s.x[2]; }, "v@:@");
    method(toolbar, "items", [this, tb](CpuState& s, GuestMemory&) { s.x[0] = auto_array(tb(s.x[0])->items); });
    method(toolbar, "visibleItems", [this, tb](CpuState& s, GuestMemory&) { s.x[0] = auto_array(tb(s.x[0])->items); });
    // Inserting asks the delegate for the item (or makes the standard space items).
    method(toolbar, "insertItemWithItemIdentifier:atIndex:", [this, tb, ti](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = tb(self);
        const auto identifier = text(s.x[2]);
        if (s.x[3] > state->items.size()) fail("-[NSToolbar insertItemWithItemIdentifier:atIndex:] out of range");
        Address item{};
        if (identifier == "NSToolbarFlexibleSpaceItem" || identifier == "NSToolbarSpaceItem" || identifier == "NSToolbarSeparatorItem") {
            item = send_id(send(toolbar_item, "alloc").x0, "initWithItemIdentifier:", {s.x[2]});
        } else {
            if (!state->delegate || !responds(state->delegate, "toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar:"))
                fail("NSToolbar item '" + identifier + "' without a delegate to make it");
            item = retain(send_id(state->delegate, "toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar:", {self, s.x[2], 1}));
            if (!item) return;
        }
        ti(item)->toolbar = self;
        state->items.insert(state->items.begin() + static_cast<std::ptrdiff_t>(s.x[3]), item);
        const std::pair<Address, Address> entry{auto_string("item"), item};
        const auto user_info = cf.dictionary(std::span(&entry, 1));
        post("NSToolbarWillAddItemNotification", self, state->delegate, "toolbarWillAddItem:", autorelease(user_info));
    }, "v@:@q");
    method(toolbar, "removeItemAtIndex:", [this, tb, ti](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = tb(self);
        if (s.x[2] >= state->items.size()) fail("-[NSToolbar removeItemAtIndex:] out of range");
        const auto item = state->items[s.x[2]];
        state->items.erase(state->items.begin() + static_cast<std::ptrdiff_t>(s.x[2]));
        ti(item)->toolbar = 0;
        const std::pair<Address, Address> entry{auto_string("item"), item};
        const auto user_info = cf.dictionary(std::span(&entry, 1));
        post("NSToolbarDidRemoveItemNotification", self, state->delegate, "toolbarDidRemoveItem:", autorelease(user_info));
        release(item);
    }, "v@:q");
    method(toolbar, "isVisible", [tb](CpuState& s, GuestMemory&) { return_bool(s, tb(s.x[0])->visible); }, "B@:");
    method(toolbar, "setVisible:", [tb](CpuState& s, GuestMemory&) { tb(s.x[0])->visible = bool_arg(s, 2); }, "v@:B");
    method(toolbar, "displayMode", [tb](CpuState& s, GuestMemory&) { s.x[0] = tb(s.x[0])->display_mode; }, "Q@:");
    method(toolbar, "setDisplayMode:", [tb](CpuState& s, GuestMemory&) { tb(s.x[0])->display_mode = s.x[2]; }, "v@:Q");
    method(toolbar, "sizeMode", [tb](CpuState& s, GuestMemory&) { s.x[0] = tb(s.x[0])->size_mode; }, "Q@:");
    method(toolbar, "setSizeMode:", [tb](CpuState& s, GuestMemory&) { tb(s.x[0])->size_mode = s.x[2]; }, "v@:Q");
    method(toolbar, "allowsUserCustomization", [tb](CpuState& s, GuestMemory&) { return_bool(s, tb(s.x[0])->customizable); }, "B@:");
    method(toolbar, "setAllowsUserCustomization:", [tb](CpuState& s, GuestMemory&) { tb(s.x[0])->customizable = bool_arg(s, 2); }, "v@:B");
    method(toolbar, "autosavesConfiguration", [tb](CpuState& s, GuestMemory&) { return_bool(s, tb(s.x[0])->autosaves); }, "B@:");
    method(toolbar, "setAutosavesConfiguration:", [tb](CpuState& s, GuestMemory&) { tb(s.x[0])->autosaves = bool_arg(s, 2); }, "v@:B");
    // -autosaves (the current name of -autosavesConfiguration).
    method(toolbar, "autosaves", [tb](CpuState& s, GuestMemory&) { return_bool(s, tb(s.x[0])->autosaves); }, "B@:");
    method(toolbar, "setAutosaves:", [tb](CpuState& s, GuestMemory&) { tb(s.x[0])->autosaves = bool_arg(s, 2); }, "v@:B");
    // The default items without a delegate, and the item centered in the
    // toolbar; the delegate's -toolbarDefaultItemIdentifiers: wins when it answers.
    method(toolbar, "defaultItemIdentifiers", [this, tb](CpuState& s, GuestMemory&) {
        const auto state = tb(s.x[0]);
        if (state->delegate && responds(state->delegate, "toolbarDefaultItemIdentifiers:")) {
            s.x[0] = send_id(state->delegate, "toolbarDefaultItemIdentifiers:", {s.x[0]});
            return;
        }
        s.x[0] = state->default_identifiers ? state->default_identifiers : auto_array({});
    });
    method(toolbar, "setDefaultItemIdentifiers:", [this, tb](CpuState& s, GuestMemory&) {
        release(std::exchange(tb(s.x[0])->default_identifiers, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(toolbar, "principalItemIdentifier", [tb](CpuState& s, GuestMemory&) { s.x[0] = tb(s.x[0])->principal; });
    method(toolbar, "setPrincipalItemIdentifier:", [this, tb](CpuState& s, GuestMemory&) {
        release(std::exchange(tb(s.x[0])->principal, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(toolbar, "showsBaselineSeparator", [tb](CpuState& s, GuestMemory&) { return_bool(s, tb(s.x[0])->baseline_separator); }, "B@:");
    method(toolbar, "setShowsBaselineSeparator:", [tb](CpuState& s, GuestMemory&) { tb(s.x[0])->baseline_separator = bool_arg(s, 2); }, "v@:B");
    method(toolbar, "selectedItemIdentifier", [tb](CpuState& s, GuestMemory&) { s.x[0] = tb(s.x[0])->selected; });
    method(toolbar, "setSelectedItemIdentifier:", [this, tb](CpuState& s, GuestMemory&) {
        release(std::exchange(tb(s.x[0])->selected, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    // Validation: items with a target-action ask their target (or the
    // responder chain's target) through -validateToolbarItem:.
    method(toolbar, "validateVisibleItems", [this, tb, ti](CpuState& s, GuestMemory&) {
        for (const auto item : tb(s.x[0])->items) {
            const auto state = ti(item);
            if (!state->autovalidates || !state->action) continue;
            const auto receiver = target_for_action(state->action, state->target);
            if (receiver && responds(receiver, "validateToolbarItem:"))
                state->enabled = send_bool(receiver, "validateToolbarItem:", {item});
        }
    }, "v@:");
    method(toolbar_item, "initWithItemIdentifier:", [this, ti](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], toolbar_item, sel("init")).x0;
        s.x[0] = self;
        release(std::exchange(ti(self)->identifier, s.x[2] ? cf.copy(s.x[2]) : string("")));
    });
    method(toolbar_item, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<ToolbarItemState>(s.x[0]))
            for (auto* slot : {&state->identifier, &state->label, &state->palette_label, &state->tool_tip, &state->image, &state->view,
                     &state->menu_form, &state->title})
                release(std::exchange(*slot, 0));
        static_cast<void>(objc.send_super(s.x[0], toolbar_item, sel("dealloc")));
    }, "v@:");
    method(toolbar_item, "itemIdentifier", [ti](CpuState& s, GuestMemory&) { s.x[0] = ti(s.x[0])->identifier; });
    method(toolbar_item, "toolbar", [ti](CpuState& s, GuestMemory&) { s.x[0] = ti(s.x[0])->toolbar; });
    const auto item_object = [this, ti](const char* getter, const char* setter, Address ToolbarItemState::*member, bool copy) {
        method(toolbar_item, getter, [ti, member](CpuState& s, GuestMemory&) { s.x[0] = (*ti(s.x[0])).*member; });
        method(toolbar_item, setter, [this, ti, member, copy](CpuState& s, GuestMemory&) {
            auto& slot = (*ti(s.x[0])).*member;
            if (copy) release(std::exchange(slot, s.x[2] ? cf.copy(s.x[2]) : 0));
            else assign(slot, s.x[2]);
        }, "v@:@");
    };
    item_object("label", "setLabel:", &ToolbarItemState::label, true);
    item_object("paletteLabel", "setPaletteLabel:", &ToolbarItemState::palette_label, true);
    item_object("toolTip", "setToolTip:", &ToolbarItemState::tool_tip, true);
    item_object("image", "setImage:", &ToolbarItemState::image, false);
    item_object("view", "setView:", &ToolbarItemState::view, false);
    item_object("menuFormRepresentation", "setMenuFormRepresentation:", &ToolbarItemState::menu_form, false);
    item_object("title", "setTitle:", &ToolbarItemState::title, true);
    method(toolbar_item, "isHidden", [ti](CpuState& s, GuestMemory&) { return_bool(s, ti(s.x[0])->hidden); }, "B@:");
    method(toolbar_item, "setHidden:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->hidden = bool_arg(s, 2); }, "v@:B");
    method(toolbar_item, "style", [ti](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ti(s.x[0])->style); }, "q@:");
    method(toolbar_item, "setStyle:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->style = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    // Window toolbars are not shown, so no item is visible in one.
    method(toolbar_item, "isVisible", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    method(toolbar_item, "target", [ti](CpuState& s, GuestMemory&) { s.x[0] = ti(s.x[0])->target; });
    method(toolbar_item, "setTarget:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->target = s.x[2]; }, "v@:@");
    method(toolbar_item, "action", [ti](CpuState& s, GuestMemory&) { s.x[0] = ti(s.x[0])->action; }, ":@:");
    method(toolbar_item, "setAction:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->action = s.x[2]; }, "v@::");
    method(toolbar_item, "tag", [ti](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ti(s.x[0])->tag); }, "q@:");
    method(toolbar_item, "setTag:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->tag = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(toolbar_item, "isEnabled", [ti](CpuState& s, GuestMemory&) { return_bool(s, ti(s.x[0])->enabled); }, "B@:");
    method(toolbar_item, "setEnabled:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->enabled = bool_arg(s, 2); }, "v@:B");
    method(toolbar_item, "autovalidates", [ti](CpuState& s, GuestMemory&) { return_bool(s, ti(s.x[0])->autovalidates); }, "B@:");
    method(toolbar_item, "setAutovalidates:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->autovalidates = bool_arg(s, 2); }, "v@:B");
    method(toolbar_item, "isBordered", [ti](CpuState& s, GuestMemory&) { return_bool(s, ti(s.x[0])->bordered); }, "B@:");
    method(toolbar_item, "setBordered:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->bordered = bool_arg(s, 2); }, "v@:B");
    method(toolbar_item, "visibilityPriority", [ti](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(ti(s.x[0])->visibility_priority); }, "q@:");
    method(toolbar_item, "setVisibilityPriority:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->visibility_priority = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(toolbar_item, "minSize", [ti](CpuState& s, GuestMemory&) { return_size(s, ti(s.x[0])->min_size); }, "{CGSize=dd}@:");
    method(toolbar_item, "setMinSize:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->min_size = size_arg(s); }, "v@:{CGSize=dd}");
    method(toolbar_item, "maxSize", [ti](CpuState& s, GuestMemory&) { return_size(s, ti(s.x[0])->max_size); }, "{CGSize=dd}@:");
    method(toolbar_item, "setMaxSize:", [ti](CpuState& s, GuestMemory&) { ti(s.x[0])->max_size = size_arg(s); }, "v@:{CGSize=dd}");
    method(toolbar_item, "validate", [this, ti](CpuState& s, GuestMemory&) {
        const auto state = ti(s.x[0]);
        if (!state->action) return;
        const auto receiver = target_for_action(state->action, state->target);
        if (receiver && responds(receiver, "validateToolbarItem:")) state->enabled = send_bool(receiver, "validateToolbarItem:", {s.x[0]});
    }, "v@:");
    // Windows do not show toolbars (no toolbar area above the content).
    method(window, "toolbar", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(window, "setToolbar:", [](CpuState& s, GuestMemory&) {
        if (s.x[2]) unsupported("-[NSWindow setToolbar:] (window toolbars are not shown)");
    }, "v@:@");
    method(window, "toggleToolbarShown:", [](CpuState&, GuestMemory&) { unsupported("-[NSWindow toggleToolbarShown:] (window toolbars)"); }, "v@:@");

    // ---- NSSavePanel / NSOpenPanel ----
    save_panel = declare_class("NSSavePanel", "NSPanel");
    open_panel = declare_class("NSOpenPanel", "NSSavePanel");
    // The panel's window state is the object's payload: the panel
    // configuration lives in a side table until the panel is deallocated.
    static std::map<Address, std::shared_ptr<SavePanelState>> panel_states;
    const auto sp = [](Address value) {
        auto& slot = panel_states[value];
        if (!slot) slot = std::make_shared<SavePanelState>();
        return slot;
    };
    method(save_panel, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto found = panel_states.find(s.x[0]); found != panel_states.end()) {
            const auto state = found->second;
            panel_states.erase(found);
            for (auto* slot : {&state->directory, &state->name, &state->title, &state->message, &state->prompt,
                     &state->allowed_types, &state->accessory, &state->name_label, &state->content_types, &state->tag_names})
                release(std::exchange(*slot, 0));
            for (const auto url : std::exchange(state->results, {})) release(url);
        }
        static_cast<void>(objc.send_super(s.x[0], save_panel, sel("dealloc")));
    }, "v@:");
    for (const auto cls : {save_panel, open_panel}) {
        const bool opening = cls == open_panel;
        class_method(cls, opening ? "openPanel" : "savePanel", [this, opening](CpuState& s, GuestMemory&) {
            CallRegs init;
            put_rect(init, 0, {{0, 0}, {480, 320}});
            init.x[2] = style::titled | style::closable | style::resizable;
            init.x[3] = 2;
            init.x[4] = 1;
            const auto made = send(send(s.x[0], "alloc").x0, "initWithContentRect:styleMask:backing:defer:", init).x0;
            static_cast<void>(send_id(made, "setReleasedWhenClosed:", {0}));
            if (opening) static_cast<void>(send_id(made, "setCanChooseFiles:", {1}));
            s.x[0] = autorelease(made);
        });
    }
    const auto panel_object = [this, sp](const char* getter, const char* setter, Address SavePanelState::*member, bool copy) {
        method(save_panel, getter, [sp, member](CpuState& s, GuestMemory&) { s.x[0] = (*sp(s.x[0])).*member; });
        method(save_panel, setter, [this, sp, member, copy](CpuState& s, GuestMemory&) {
            auto& slot = (*sp(s.x[0])).*member;
            if (copy) release(std::exchange(slot, s.x[2] ? cf.copy(s.x[2]) : 0));
            else assign(slot, s.x[2]);
        }, "v@:@");
    };
    panel_object("directoryURL", "setDirectoryURL:", &SavePanelState::directory, false);
    panel_object("nameFieldStringValue", "setNameFieldStringValue:", &SavePanelState::name, true);
    panel_object("message", "setMessage:", &SavePanelState::message, true);
    panel_object("prompt", "setPrompt:", &SavePanelState::prompt, true);
    panel_object("allowedFileTypes", "setAllowedFileTypes:", &SavePanelState::allowed_types, false);
    panel_object("accessoryView", "setAccessoryView:", &SavePanelState::accessory, false);
    // Finder tags: the Windows file dialog has no tag field, so the tags are
    // the ones the application set (none were entered).
    panel_object("tagNames", "setTagNames:", &SavePanelState::tag_names, true);
    method(save_panel, "delegate", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp(s.x[0])->delegate; });
    method(save_panel, "setDelegate:", [sp](CpuState& s, GuestMemory&) { sp(s.x[0])->delegate = s.x[2]; }, "v@:@");
    const auto panel_flag = [this, sp](Address cls, const char* getter, const char* setter, bool SavePanelState::*member) {
        method(cls, getter, [sp, member](CpuState& s, GuestMemory&) { return_bool(s, (*sp(s.x[0])).*member); }, "B@:");
        method(cls, setter, [sp, member](CpuState& s, GuestMemory&) { (*sp(s.x[0])).*member = bool_arg(s, 2); }, "v@:B");
    };
    panel_flag(save_panel, "canCreateDirectories", "setCanCreateDirectories:", &SavePanelState::can_create_directories);
    panel_flag(save_panel, "showsHiddenFiles", "setShowsHiddenFiles:", &SavePanelState::shows_hidden);
    panel_flag(save_panel, "isExtensionHidden", "setExtensionHidden:", &SavePanelState::extension_hidden);
    panel_flag(save_panel, "treatsFilePackagesAsDirectories", "setTreatsFilePackagesAsDirectories:", &SavePanelState::treats_packages_as_directories);
    panel_flag(save_panel, "allowsOtherFileTypes", "setAllowsOtherFileTypes:", &SavePanelState::allows_other_types);
    panel_flag(save_panel, "canSelectHiddenExtension", "setCanSelectHiddenExtension:", &SavePanelState::can_select_hidden_extension);
    panel_flag(save_panel, "showsTagField", "setShowsTagField:", &SavePanelState::shows_tag_field);
    // -ok: / -cancel: end a running panel; the Windows dialog cannot be
    // ended from the application, and a panel that is not running has
    // nothing to end.
    for (const auto* name : {"ok:", "cancel:"}) {
        const std::string selector = name;
        method(save_panel, selector, [sp, selector](CpuState& s, GuestMemory&) {
            if (sp(s.x[0])->running) unsupported("-[NSSavePanel " + selector + "] while the Windows file dialog is open");
        }, "v@:@");
    }
    panel_flag(open_panel, "canChooseFiles", "setCanChooseFiles:", &SavePanelState::can_choose_files);
    panel_flag(open_panel, "canChooseDirectories", "setCanChooseDirectories:", &SavePanelState::can_choose_directories);
    panel_flag(open_panel, "allowsMultipleSelection", "setAllowsMultipleSelection:", &SavePanelState::multiple);
    panel_flag(open_panel, "resolvesAliases", "setResolvesAliases:", &SavePanelState::resolves_aliases);
    method(save_panel, "setTitle:", [this, sp](CpuState& s, GuestMemory&) {
        release(std::exchange(sp(s.x[0])->title, s.x[2] ? cf.copy(s.x[2]) : 0));
        CallRegs registers;
        registers.x[2] = s.x[2];
        static_cast<void>(objc.send_super(s.x[0], save_panel, sel("setTitle:"), registers));
    }, "v@:@");
    panel_object("nameFieldLabel", "setNameFieldLabel:", &SavePanelState::name_label, true);
    panel_object("allowedContentTypes", "setAllowedContentTypes:", &SavePanelState::content_types, false);
    // An alias of -setAllowedFileTypes: (extensions or UTIs).
    method(save_panel, "setAllowedTypes:", [this](CpuState& s, GuestMemory&) { static_cast<void>(send_id(s.x[0], "setAllowedFileTypes:", {s.x[2]})); }, "v@:@");
    method(save_panel, "allowedTypes", [this](CpuState& s, GuestMemory&) { s.x[0] = send(s.x[0], "allowedFileTypes").x0; });
    // -runModal: the common item dialog, owned by the key window.
    method(save_panel, "runModal", [this, sp](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = sp(self);
        struct Running {
            SavePanelState& state;
            explicit Running(SavePanelState& value) : state(value) { state.running = true; }
            ~Running() { state.running = false; }
        } running(*state);
        FileDialogRequest request;
        request.save = !is_kind(self, open_panel);
        request.title = text(state->title);
        request.message = text(state->message);
        request.prompt = text(state->prompt);
        request.name_label = text(state->name_label);
        request.name = text(state->name);
        if (state->directory) request.directory = cf.url_path(state->directory);
        request.choose_files = state->can_choose_files;
        request.choose_directories = state->can_choose_directories;
        request.multiple = state->multiple;
        request.shows_hidden = state->shows_hidden;
        request.allows_other_types = state->allows_other_types;
        bool any_type = false;
        const auto add_type = [&](const std::string& type) {
            if (type.find('.') == std::string::npos) { request.extensions.push_back(type); return; }
            const auto found = extensions_for_uti(*this, type);
            if (!found || found->empty()) { any_type = true; return; }
            request.extensions.insert(request.extensions.end(), found->begin(), found->end());
        };
        for (const auto type : items(state->allowed_types)) add_type(text(type));
        for (const auto type : items(state->content_types)) {
            if (responds(type, "preferredFilenameExtension")) {
                if (const auto extension = send(type, "preferredFilenameExtension").x0) { request.extensions.push_back(text(extension)); continue; }
            }
            add_type(text(send(type, "identifier").x0));
        }
        if (any_type) request.allows_other_types = true;
        {
            std::lock_guard lock(mutex);
            const auto owner = key_window ? key_window : main_window;
            if (owner && window_state(owner)->id)
                if (const auto server = window_server_or_null()) request.owner = server->native_handle(window_state(owner)->id);
        }
        FileDialogPresenter presenter;
        {
            std::lock_guard lock(file_dialog_mutex);
            presenter = file_dialog_override;
        }
#ifdef _WIN32
        // Cloaked runs show no windows: a file dialog would be the one visible window.
        if (!presenter) {
            if (const auto cloak = host_environment("ALCHEMY_GUI_CLOAK"); !cloak.empty() && cloak != "0")
                unsupported("NSSavePanel / NSOpenPanel in a cloaked run (ALCHEMY_GUI_CLOAK: the file dialog would be visible)");
            presenter = [this](const FileDialogRequest& value) { return run_common_dialog(*this, value); };
        }
#endif
        if (!presenter) unsupported("NSSavePanel / NSOpenPanel without the Windows file dialog");
        std::optional<std::vector<std::string>> chosen;
        ++modal_depth;
        try { chosen = presenter(request); } catch (...) { --modal_depth; throw; }
        --modal_depth;
        for (const auto url : std::exchange(state->results, {})) release(url);
        if (!chosen || chosen->empty()) { s.x[0] = 0; return; }   // NSModalResponseCancel
        for (const auto& path : *chosen) {
            const bool directory = request.choose_directories && !request.choose_files;
            state->results.push_back(cf.file_url(path, directory));
        }
        s.x[0] = 1;   // NSModalResponseOK
    }, "q@:");
    method(save_panel, "runModalForDirectory:file:", [this](CpuState& s, GuestMemory&) {
        if (s.x[2]) static_cast<void>(send_id(s.x[0], "setDirectoryURL:", {autorelease(cf.file_url(text(s.x[2]), true))}));
        if (s.x[3]) static_cast<void>(send_id(s.x[0], "setNameFieldStringValue:", {s.x[3]}));
        s.x[0] = send(s.x[0], "runModal").x0;
    }, "q@:@@");
    method(save_panel, "runModalForDirectory:file:types:", [this](CpuState& s, GuestMemory&) {
        if (s.x[4]) static_cast<void>(send_id(s.x[0], "setAllowedFileTypes:", {s.x[4]}));
        s.x[0] = send_id(s.x[0], "runModalForDirectory:file:", {s.x[2], s.x[3]});
    }, "q@:@@@");
    // The sheet and completion-handler forms run the dialog once the current
    // event is handled, then call the handler with the response.
    const auto run_later = [this](Address panel, Address block) {
        const auto self = retain(panel);
        const auto handler = block ? guest_call(libsystem_library, "__Block_copy", {block}) : 0;
        auto* loop = run_loop_or_null();
        if (!loop) fail("NSSavePanel completion handlers need the main run loop");
        auto source = std::make_shared<SourceId>();
        *source = loop->add_source(common_modes, [this, self, handler, source, loop] {
            loop->remove_source(*source);
            const auto finish = [&] {
                release(self);
                if (handler) static_cast<void>(guest_call(libsystem_library, "__Block_release", {handler}));
            };
            Address response{};
            try {
                response = send(self, "runModal").x0;
                if (handler) static_cast<void>(invoke_block(handler, {response}));
            } catch (...) { finish(); throw; }
            finish();
        });
        loop->signal_source(*source);
    };
    method(save_panel, "beginSheetModalForWindow:completionHandler:", [run_later](CpuState& s, GuestMemory&) { run_later(s.x[0], s.x[3]); }, "v@:@@?");
    method(save_panel, "beginWithCompletionHandler:", [run_later](CpuState& s, GuestMemory&) { run_later(s.x[0], s.x[2]); }, "v@:@?");
    method(save_panel, "URL", [sp](CpuState& s, GuestMemory&) {
        const auto state = sp(s.x[0]);
        s.x[0] = state->results.empty() ? 0 : state->results.front();
    });
    method(open_panel, "URLs", [this, sp](CpuState& s, GuestMemory&) { s.x[0] = auto_array(sp(s.x[0])->results); });

    // ---- NSPrintInfo, and the panels that need system UI ----
    print_info = declare_class("NSPrintInfo", "NSObject");
    const auto pi = [this](Address value) { return objc.host_state<PrintInfoState>(value); };
    static Address shared_print_info{};
    class_method(print_info, "sharedPrintInfo", [this](CpuState& s, GuestMemory&) {
        if (!shared_print_info) shared_print_info = alloc_init(print_info);
        s.x[0] = shared_print_info;
    });
    class_method(print_info, "setSharedPrintInfo:", [this](CpuState& s, GuestMemory&) {
        const auto old = shared_print_info;
        shared_print_info = retain(s.x[2]);
        release(old);
    }, "v@:@");
    method(print_info, "init", [this, pi](CpuState& s, GuestMemory&) {
        s.x[0] = objc.send_super(s.x[0], print_info, sel("init")).x0;
        pi(s.x[0])->paper_name = string("na-letter");
    });
    method(print_info, "initWithDictionary:", [this, pi](CpuState& s, GuestMemory&) {
        const auto self = send(s.x[0], "init").x0;
        s.x[0] = self;
        if (!s.x[2]) return;
        const auto state = pi(self);
        const auto number = [&](std::string_view key, double& slot) {
            const auto value = send_id(s.x[2], "objectForKey:", {auto_string(key)});
            if (value) slot = send(value, "doubleValue").d(0);
        };
        number("NSLeftMargin", state->left);
        number("NSRightMargin", state->right);
        number("NSTopMargin", state->top);
        number("NSBottomMargin", state->bottom);
        number("NSScalingFactor", state->scaling);
        if (const auto size = send_id(s.x[2], "objectForKey:", {auto_string("NSPaperSize")})) state->paper = result_size(send(size, "sizeValue"));
    });
    method(print_info, "copyWithZone:", [this, pi](CpuState& s, GuestMemory&) {
        const auto copy = alloc_init(print_info);
        auto to = pi(copy);
        const auto from = pi(s.x[0]);
        release(to->paper_name);
        *to = *from;
        to->paper_name = retain(to->paper_name);
        to->printer = retain(to->printer);
        s.x[0] = copy;
    });
    const auto print_real = [this, pi](const char* getter, const char* setter, double PrintInfoState::*member) {
        method(print_info, getter, [pi, member](CpuState& s, GuestMemory&) { set_d(s, 0, (*pi(s.x[0])).*member); }, "d@:");
        method(print_info, setter, [pi, member](CpuState& s, GuestMemory&) { (*pi(s.x[0])).*member = d(s, 0); }, "v@:d");
    };
    print_real("leftMargin", "setLeftMargin:", &PrintInfoState::left);
    print_real("rightMargin", "setRightMargin:", &PrintInfoState::right);
    print_real("topMargin", "setTopMargin:", &PrintInfoState::top);
    print_real("bottomMargin", "setBottomMargin:", &PrintInfoState::bottom);
    print_real("scalingFactor", "setScalingFactor:", &PrintInfoState::scaling);
    method(print_info, "paperSize", [pi](CpuState& s, GuestMemory&) { return_size(s, pi(s.x[0])->paper); }, "{CGSize=dd}@:");
    method(print_info, "setPaperSize:", [pi](CpuState& s, GuestMemory&) { pi(s.x[0])->paper = size_arg(s); }, "v@:{CGSize=dd}");
    method(print_info, "paperName", [pi](CpuState& s, GuestMemory&) { s.x[0] = pi(s.x[0])->paper_name; });
    method(print_info, "setPaperName:", [this, pi](CpuState& s, GuestMemory&) {
        release(std::exchange(pi(s.x[0])->paper_name, s.x[2] ? cf.copy(s.x[2]) : 0));
    }, "v@:@");
    method(print_info, "orientation", [pi](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(pi(s.x[0])->orientation); }, "q@:");
    method(print_info, "setOrientation:", [pi](CpuState& s, GuestMemory&) {
        const auto state = pi(s.x[0]);
        const auto value = static_cast<std::int64_t>(s.x[2]);
        // The paper size follows the orientation (landscape: wider than tall).
        const bool landscape = state->paper.width > state->paper.height;
        if ((value == 1) != landscape) std::swap(state->paper.width, state->paper.height);
        state->orientation = value;
    }, "v@:q");
    method(print_info, "horizontalPagination", [pi](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(pi(s.x[0])->horizontal_pagination); }, "q@:");
    method(print_info, "setHorizontalPagination:", [pi](CpuState& s, GuestMemory&) { pi(s.x[0])->horizontal_pagination = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(print_info, "verticalPagination", [pi](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(pi(s.x[0])->vertical_pagination); }, "q@:");
    method(print_info, "setVerticalPagination:", [pi](CpuState& s, GuestMemory&) { pi(s.x[0])->vertical_pagination = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(print_info, "isHorizontallyCentered", [pi](CpuState& s, GuestMemory&) { return_bool(s, pi(s.x[0])->horizontally_centered); }, "B@:");
    method(print_info, "setHorizontallyCentered:", [pi](CpuState& s, GuestMemory&) { pi(s.x[0])->horizontally_centered = bool_arg(s, 2); }, "v@:B");
    method(print_info, "isVerticallyCentered", [pi](CpuState& s, GuestMemory&) { return_bool(s, pi(s.x[0])->vertically_centered); }, "B@:");
    method(print_info, "setVerticallyCentered:", [pi](CpuState& s, GuestMemory&) { pi(s.x[0])->vertically_centered = bool_arg(s, 2); }, "v@:B");
    method(print_info, "imageablePageBounds", [pi](CpuState& s, GuestMemory&) {
        const auto state = pi(s.x[0]);
        return_rect(s, {{18, 18}, {std::max(0.0, state->paper.width - 36), std::max(0.0, state->paper.height - 36)}});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(print_info, "setUpPrintOperationDefaultValues", [](CpuState&, GuestMemory&) {}, "v@:");
    for (const auto* name : {"PMPrintSession", "PMPrintSettings", "PMPageFormat", "printer", "setPrinter:", "dictionary",
             "printSettings", "updateFromPMPrintSettings", "updateFromPMPageFormat"})
        method(print_info, name, [selector = std::string(name)](CpuState&, GuestMemory&) {
            unsupported("-[NSPrintInfo " + selector + "] (printing system)");
        });
    for (const auto& [name, super_name] : std::initializer_list<std::pair<const char*, const char*>>{
             {"NSPrintPanel", "NSObject"}, {"NSPageLayout", "NSObject"}, {"NSColorPanel", "NSPanel"}, {"NSFontPanel", "NSPanel"}}) {
        const auto cls = declare_class(name, super_name);
        const std::string class_name = name;
        for (const auto* factory : {"printPanel", "pageLayout", "sharedColorPanel", "sharedFontPanel"})
            class_method(cls, factory, [class_name](CpuState&, GuestMemory&) { unsupported(class_name + " (system panel UI)"); });
        for (const auto* query : {"sharedColorPanelExists", "sharedFontPanelExists"})
            class_method(cls, query, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
        for (const auto* run : {"runModal", "runModalWithPrintInfo:", "beginSheetWithPrintInfo:modalForWindow:delegate:didEndSelector:contextInfo:",
                 "beginSheetUsingPrintInfo:onWindow:completionHandler:", "orderFront:", "makeKeyAndOrderFront:"})
            method(cls, run, [class_name](CpuState&, GuestMemory&) { unsupported(class_name + " (system panel UI)"); });
    }

    // ---- gesture recognizers ----
    gesture_recognizer = declare_class("NSGestureRecognizer", "NSObject");
    const auto gr = [this](Address value) { return objc.host_state<GestureState>(value); };
    method(gesture_recognizer, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send_id(s.x[0], "initWithTarget:action:", {0, 0}); });
    method(gesture_recognizer, "initWithTarget:action:", [this, gr](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], gesture_recognizer, sel("init")).x0;
        s.x[0] = self;
        gr(self)->target = s.x[2];
        gr(self)->action = s.x[3];
    });
    method(gesture_recognizer, "target", [gr](CpuState& s, GuestMemory&) { s.x[0] = gr(s.x[0])->target; });
    method(gesture_recognizer, "setTarget:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->target = s.x[2]; }, "v@:@");
    method(gesture_recognizer, "action", [gr](CpuState& s, GuestMemory&) { s.x[0] = gr(s.x[0])->action; }, ":@:");
    method(gesture_recognizer, "setAction:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->action = s.x[2]; }, "v@::");
    method(gesture_recognizer, "delegate", [gr](CpuState& s, GuestMemory&) { s.x[0] = gr(s.x[0])->delegate; });
    method(gesture_recognizer, "setDelegate:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->delegate = s.x[2]; }, "v@:@");
    method(gesture_recognizer, "view", [gr](CpuState& s, GuestMemory&) { s.x[0] = gr(s.x[0])->view; });
    method(gesture_recognizer, "state", [gr](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(gr(s.x[0])->state); }, "q@:");
    method(gesture_recognizer, "isEnabled", [gr](CpuState& s, GuestMemory&) { return_bool(s, gr(s.x[0])->enabled); }, "B@:");
    method(gesture_recognizer, "setEnabled:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->enabled = bool_arg(s, 2); }, "v@:B");
    method(gesture_recognizer, "allowedTouchTypes", [gr](CpuState& s, GuestMemory&) { s.x[0] = gr(s.x[0])->allowed_touch_types; }, "Q@:");
    method(gesture_recognizer, "setAllowedTouchTypes:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->allowed_touch_types = s.x[2]; }, "v@:Q");
    for (const auto& [getter, setter, member] : std::initializer_list<std::tuple<const char*, const char*, bool GestureState::*>>{
             {"delaysPrimaryMouseButtonEvents", "setDelaysPrimaryMouseButtonEvents:", &GestureState::delays_primary},
             {"delaysSecondaryMouseButtonEvents", "setDelaysSecondaryMouseButtonEvents:", &GestureState::delays_secondary},
             {"delaysOtherMouseButtonEvents", "setDelaysOtherMouseButtonEvents:", &GestureState::delays_other},
             {"delaysKeyEvents", "setDelaysKeyEvents:", &GestureState::delays_key},
             {"delaysMagnificationEvents", "setDelaysMagnificationEvents:", &GestureState::delays_magnification},
             {"delaysRotationEvents", "setDelaysRotationEvents:", &GestureState::delays_rotation}}) {
        const auto field = member;
        method(gesture_recognizer, getter, [gr, field](CpuState& s, GuestMemory&) { return_bool(s, (*gr(s.x[0])).*field); }, "B@:");
        method(gesture_recognizer, setter, [gr, field](CpuState& s, GuestMemory&) { (*gr(s.x[0])).*field = bool_arg(s, 2); }, "v@:B");
    }
    method(gesture_recognizer, "locationInView:", [this, gr](CpuState& s, GuestMemory&) {
        // The pointer's current location in the view (gestures are not
        // recognized, so there is no gesture location of its own).
        const auto target_view = s.x[2] ? s.x[2] : gr(s.x[0])->view;
        Point location{};
        if (target_view && view_state(target_view)->window) {
            const auto window_object = view_state(target_view)->window;
            location = window_to_view(target_view, result_point(send(window_object, "mouseLocationOutsideOfEventStream")));
        }
        return_point(s, location);
    }, "{CGPoint=dd}@:@");
    method(gesture_recognizer, "reset", [gr](CpuState& s, GuestMemory&) {
        const auto state = gr(s.x[0]);
        state->state = 0;
        state->magnification = state->rotation = 0;
        state->translation = {};
    }, "v@:");
    const auto click = declare_class("NSClickGestureRecognizer", "NSGestureRecognizer");
    const auto pan = declare_class("NSPanGestureRecognizer", "NSGestureRecognizer");
    const auto press = declare_class("NSPressGestureRecognizer", "NSGestureRecognizer");
    const auto magnify = declare_class("NSMagnificationGestureRecognizer", "NSGestureRecognizer");
    const auto rotate = declare_class("NSRotationGestureRecognizer", "NSGestureRecognizer");
    for (const auto cls : {click, pan, press}) {
        method(cls, "buttonMask", [gr](CpuState& s, GuestMemory&) { s.x[0] = gr(s.x[0])->button_mask; }, "Q@:");
        method(cls, "setButtonMask:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->button_mask = s.x[2]; }, "v@:Q");
        method(cls, "numberOfTouchesRequired", [gr](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(gr(s.x[0])->touches); }, "q@:");
        method(cls, "setNumberOfTouchesRequired:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->touches = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    }
    method(click, "numberOfClicksRequired", [gr](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(gr(s.x[0])->clicks); }, "q@:");
    method(click, "setNumberOfClicksRequired:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->clicks = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    method(pan, "translationInView:", [gr](CpuState& s, GuestMemory&) { return_point(s, gr(s.x[0])->translation); }, "{CGPoint=dd}@:@");
    method(pan, "setTranslation:inView:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->translation = point_arg(s); }, "v@:{CGPoint=dd}@");
    method(pan, "velocityInView:", [](CpuState& s, GuestMemory&) { return_point(s, {}); }, "{CGPoint=dd}@:@");
    method(press, "minimumPressDuration", [gr](CpuState& s, GuestMemory&) { set_d(s, 0, gr(s.x[0])->press_duration); }, "d@:");
    method(press, "setMinimumPressDuration:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->press_duration = d(s, 0); }, "v@:d");
    method(press, "allowableMovement", [gr](CpuState& s, GuestMemory&) { set_d(s, 0, gr(s.x[0])->allowable_movement); }, "d@:");
    method(press, "setAllowableMovement:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->allowable_movement = d(s, 0); }, "v@:d");
    method(magnify, "magnification", [gr](CpuState& s, GuestMemory&) { set_d(s, 0, gr(s.x[0])->magnification); }, "d@:");
    method(magnify, "setMagnification:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->magnification = d(s, 0); }, "v@:d");
    method(rotate, "rotation", [gr](CpuState& s, GuestMemory&) { set_d(s, 0, gr(s.x[0])->rotation); }, "d@:");
    method(rotate, "setRotation:", [gr](CpuState& s, GuestMemory&) { gr(s.x[0])->rotation = d(s, 0); }, "v@:d");
    method(rotate, "rotationInDegrees", [gr](CpuState& s, GuestMemory&) { set_d(s, 0, gr(s.x[0])->rotation * 180 / 3.14159265358979323846); }, "d@:");
    // NSView's recognizers.
    method(view, "gestureRecognizers", [this](CpuState& s, GuestMemory&) { s.x[0] = auto_array(view_state(s.x[0])->gesture_recognizers); });
    method(view, "addGestureRecognizer:", [this, gr](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        auto& list = view_state(s.x[0])->gesture_recognizers;
        if (std::find(list.begin(), list.end(), s.x[2]) != list.end()) return;
        if (const auto previous = gr(s.x[2])->view; previous && previous != s.x[0])
            static_cast<void>(send_id(previous, "removeGestureRecognizer:", {s.x[2]}));
        list.push_back(retain(s.x[2]));
        gr(s.x[2])->view = s.x[0];
    }, "v@:@");
    method(view, "removeGestureRecognizer:", [this, gr](CpuState& s, GuestMemory&) {
        auto& list = view_state(s.x[0])->gesture_recognizers;
        const auto found = std::find(list.begin(), list.end(), s.x[2]);
        if (found == list.end()) return;
        list.erase(found);
        gr(s.x[2])->view = 0;
        release(s.x[2]);
    }, "v@:@");
    method(view, "setGestureRecognizers:", [this](CpuState& s, GuestMemory&) {
        for (const auto old : std::vector<Address>(view_state(s.x[0])->gesture_recognizers))
            static_cast<void>(send_id(s.x[0], "removeGestureRecognizer:", {old}));
        for (const auto added : items(s.x[2])) static_cast<void>(send_id(s.x[0], "addGestureRecognizer:", {added}));
    }, "v@:@");

    // ---- NSPasteboardItem ----
    pasteboard_item = declare_class("NSPasteboardItem", "NSObject");
    const auto pb = [this](Address value) { return objc.host_state<PasteboardItemState>(value); };
    method(pasteboard_item, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<PasteboardItemState>(s.x[0])) {
            for (auto& [type, value] : std::exchange(state->values, {})) release(value);
            for (auto& [type, provider] : std::exchange(state->providers, {})) release(provider);
        }
        static_cast<void>(objc.send_super(s.x[0], pasteboard_item, sel("dealloc")));
    }, "v@:");
    const auto put = [this, pb](Address self, Address type, Address value, bool copy) {
        if (!type) fail("NSPasteboardItem value without a type");
        const auto key = text(type);
        auto& values = pb(self)->values;
        const auto stored = copy ? cf.copy(value) : retain(value);
        for (auto& [existing, slot] : values)
            if (existing == key) { release(std::exchange(slot, stored)); return; }
        values.emplace_back(key, stored);
    };
    method(pasteboard_item, "setString:forType:", [put](CpuState& s, GuestMemory&) { put(s.x[0], s.x[3], s.x[2], true); s.x[0] = 1; }, "B@:@@");
    method(pasteboard_item, "setData:forType:", [put](CpuState& s, GuestMemory&) { put(s.x[0], s.x[3], s.x[2], false); s.x[0] = 1; }, "B@:@@");
    method(pasteboard_item, "setPropertyList:forType:", [put](CpuState& s, GuestMemory&) { put(s.x[0], s.x[3], s.x[2], false); s.x[0] = 1; }, "B@:@@");
    method(pasteboard_item, "setDataProvider:forTypes:", [this, pb](CpuState& s, GuestMemory&) {
        for (const auto type : items(s.x[3])) pb(s.x[0])->providers.emplace_back(text(type), retain(s.x[2]));
        s.x[0] = 1;
    }, "B@:@@");
    method(pasteboard_item, "types", [this, pb](CpuState& s, GuestMemory&) {
        std::vector<Address> list;
        for (const auto& [type, value] : pb(s.x[0])->values) list.push_back(auto_string(type));
        for (const auto& [type, provider] : pb(s.x[0])->providers) list.push_back(auto_string(type));
        s.x[0] = auto_array(list);
    });
    method(pasteboard_item, "availableTypeFromArray:", [this, pb](CpuState& s, GuestMemory&) {
        const auto state = pb(s.x[0]);
        for (const auto wanted : items(s.x[2])) {
            const auto key = text(wanted);
            for (const auto& [type, value] : state->values) if (type == key) { s.x[0] = wanted; return; }
            for (const auto& [type, provider] : state->providers) if (type == key) { s.x[0] = wanted; return; }
        }
        s.x[0] = 0;
    });
    // Reading a provided type asks the provider (-pasteboard:item:provideDataForType:
    // with a nil pasteboard: the item is not on a pasteboard).
    const auto value_for = [this, pb](Address self, Address type) -> Address {
        const auto key = text(type);
        auto state = pb(self);
        for (const auto& [existing, value] : state->values) if (existing == key) return value;
        for (const auto& [existing, provider] : std::vector(state->providers))
            if (existing == key) {
                CallRegs registers;
                registers.x[2] = 0; registers.x[3] = self; registers.x[4] = type;
                static_cast<void>(send(provider, "pasteboard:item:provideDataForType:", registers));
                for (const auto& [now, value] : pb(self)->values) if (now == key) return value;
            }
        return 0;
    };
    method(pasteboard_item, "dataForType:", [this, value_for](CpuState& s, GuestMemory&) {
        const auto value = value_for(s.x[0], s.x[2]);
        if (value && cf.kind(value) == ObjectKind::string) { s.x[0] = send_id(value, "dataUsingEncoding:", {4}); return; }   // UTF-8
        s.x[0] = value;
    });
    method(pasteboard_item, "stringForType:", [this, value_for](CpuState& s, GuestMemory&) {
        const auto value = value_for(s.x[0], s.x[2]);
        if (value && cf.kind(value) != ObjectKind::string && responds(value, "bytes")) {
            const auto bytes = data_bytes(value);
            s.x[0] = auto_string(std::string(bytes.begin(), bytes.end()));
            return;
        }
        s.x[0] = value;
    });
    method(pasteboard_item, "propertyListForType:", [value_for](CpuState& s, GuestMemory&) { s.x[0] = value_for(s.x[0], s.x[2]); });
    // NSPasteboardWriting: the item writes its own types.
    method(pasteboard_item, "writableTypesForPasteboard:", [this](CpuState& s, GuestMemory&) { s.x[0] = send(s.x[0], "types").x0; });
    method(pasteboard_item, "pasteboardPropertyListForType:", [value_for](CpuState& s, GuestMemory&) { s.x[0] = value_for(s.x[0], s.x[2]); });

    // ---- NSDraggingItem ----
    dragging_item = declare_class("NSDraggingItem", "NSObject");
    const auto di = [this](Address value) { return objc.host_state<DraggingItemState>(value); };
    method(dragging_item, "initWithPasteboardWriter:", [this, di](CpuState& s, GuestMemory&) {
        const auto self = objc.send_super(s.x[0], dragging_item, sel("init")).x0;
        s.x[0] = self;
        di(self)->item = retain(s.x[2]);
    });
    method(dragging_item, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<DraggingItemState>(s.x[0]))
            for (auto* slot : {&state->item, &state->contents, &state->components_provider}) release(std::exchange(*slot, 0));
        static_cast<void>(objc.send_super(s.x[0], dragging_item, sel("dealloc")));
    }, "v@:");
    method(dragging_item, "item", [di](CpuState& s, GuestMemory&) { s.x[0] = di(s.x[0])->item; });
    method(dragging_item, "draggingFrame", [di](CpuState& s, GuestMemory&) { return_rect(s, di(s.x[0])->frame); }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    method(dragging_item, "setDraggingFrame:", [di](CpuState& s, GuestMemory&) { di(s.x[0])->frame = rect_arg(s); }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    method(dragging_item, "setDraggingFrame:contents:", [this, di](CpuState& s, GuestMemory&) {
        di(s.x[0])->frame = rect_arg(s);
        assign(di(s.x[0])->contents, s.x[2]);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    method(dragging_item, "imageComponentsProvider", [di](CpuState& s, GuestMemory&) { s.x[0] = di(s.x[0])->components_provider; });
    method(dragging_item, "setImageComponentsProvider:", [this, di](CpuState& s, GuestMemory&) {
        release(std::exchange(di(s.x[0])->components_provider, s.x[2] ? send(s.x[2], "copy").x0 : 0));
    }, "v@:@");
    // -[NSView beginDraggingSessionWithItems:event:source:] is
    // windows/drag_session.cpp's (drag sessions within the application).

    // ---- NSAnimation / NSViewAnimation ----
    animation = declare_class("NSAnimation", "NSObject");
    view_animation = declare_class("NSViewAnimation", "NSAnimation");
    const auto an = [this](Address value) { return objc.host_state<AnimationState>(value); };
    method(animation, "initWithDuration:animationCurve:", [this, an](CpuState& s, GuestMemory&) {
        const auto duration = d(s, 0);
        const auto curve = s.x[2];
        const auto self = objc.send_super(s.x[0], animation, sel("init")).x0;
        s.x[0] = self;
        an(self)->duration = std::max(0.0, duration);
        an(self)->curve = curve;
    }, "@@:dQ");
    method(animation, "dealloc", [this](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<AnimationState>(s.x[0])) release(std::exchange(state->view_animations, 0));
        static_cast<void>(objc.send_super(s.x[0], animation, sel("dealloc")));
    }, "v@:");
    method(animation, "duration", [an](CpuState& s, GuestMemory&) { set_d(s, 0, an(s.x[0])->duration); }, "d@:");
    method(animation, "setDuration:", [an](CpuState& s, GuestMemory&) { an(s.x[0])->duration = std::max(0.0, d(s, 0)); }, "v@:d");
    method(animation, "frameRate", [an](CpuState& s, GuestMemory&) { set_f(s, 0, static_cast<float>(an(s.x[0])->frame_rate)); }, "f@:");
    method(animation, "setFrameRate:", [an](CpuState& s, GuestMemory&) { const auto rate = f(s, 0); an(s.x[0])->frame_rate = rate > 0 ? rate : 60; }, "v@:f");
    method(animation, "animationCurve", [an](CpuState& s, GuestMemory&) { s.x[0] = an(s.x[0])->curve; }, "Q@:");
    method(animation, "setAnimationCurve:", [an](CpuState& s, GuestMemory&) { an(s.x[0])->curve = s.x[2]; }, "v@:Q");
    method(animation, "animationBlockingMode", [an](CpuState& s, GuestMemory&) { s.x[0] = an(s.x[0])->blocking_mode; }, "Q@:");
    method(animation, "setAnimationBlockingMode:", [an](CpuState& s, GuestMemory&) { an(s.x[0])->blocking_mode = s.x[2]; }, "v@:Q");
    method(animation, "delegate", [an](CpuState& s, GuestMemory&) { s.x[0] = an(s.x[0])->delegate; });
    method(animation, "setDelegate:", [an](CpuState& s, GuestMemory&) { an(s.x[0])->delegate = s.x[2]; }, "v@:@");
    method(animation, "isAnimating", [an](CpuState& s, GuestMemory&) { return_bool(s, an(s.x[0])->animating); }, "B@:");
    method(animation, "currentProgress", [an](CpuState& s, GuestMemory&) { set_f(s, 0, static_cast<float>(an(s.x[0])->progress)); }, "f@:");
    method(animation, "setCurrentProgress:", [an](CpuState& s, GuestMemory&) { an(s.x[0])->progress = std::clamp<double>(f(s, 0), 0, 1); }, "v@:f");
    method(animation, "currentValue", [an](CpuState& s, GuestMemory&) {
        const auto state = an(s.x[0]);
        set_f(s, 0, static_cast<float>(eased(state->curve, state->progress)));
    }, "f@:");
    // Starting runs the animation to completion in the calling thread: every
    // frame updates the progress (-setCurrentProgress:, which NSViewAnimation
    // applies to its targets) and displays the windows, then the delegate
    // hears -animationDidEnd:. A delegate may refuse to start.
    method(animation, "startAnimation", [this, an](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = an(self);
        if (state->animating) return;
        if (state->delegate && responds(state->delegate, "animationShouldStart:") && !send_bool(state->delegate, "animationShouldStart:", {self})) return;
        state->animating = true;
        state->progress = 0;
        static_cast<void>(send(self, "_alchemyBeginAnimation"));
        const auto frames = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(state->duration * state->frame_rate)));
        const auto start = std::chrono::steady_clock::now();
        for (std::int64_t frame = 1; frame <= frames && an(self)->animating; ++frame) {
            const auto progress = static_cast<double>(frame) / static_cast<double>(frames);
            std::this_thread::sleep_until(start + std::chrono::duration<double>(state->duration * progress));
            const auto previous = static_cast<double>(frame - 1) / static_cast<double>(frames);
            CallRegs registers;
            registers.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(progress)), 0};
            static_cast<void>(send(self, "setCurrentProgress:", registers));
            // Progress marks passed in this frame: -animation:didReachProgressMark:.
            for (const auto mark : std::vector<float>(an(self)->marks))
                if (mark > previous && mark <= progress)
                    if (const auto delegate = an(self)->delegate; delegate && responds(delegate, "animation:didReachProgressMark:")) {
                        CallRegs told;
                        told.x[2] = self;
                        told.v[0] = {std::bit_cast<std::uint32_t>(mark), 0};
                        static_cast<void>(send(delegate, "animation:didReachProgressMark:", told));
                    }
            display_pending();
        }
        const bool completed = an(self)->animating;
        an(self)->animating = false;
        static_cast<void>(send(self, "_alchemyEndAnimation"));
        if (const auto delegate = an(self)->delegate)
            static_cast<void>(send_optional(delegate, completed ? "animationDidEnd:" : "animationDidStop:", [&] { CallRegs r; r.x[2] = self; return r; }()));
    }, "v@:");
    method(animation, "stopAnimation", [an](CpuState& s, GuestMemory&) { an(s.x[0])->animating = false; }, "v@:");
    method(animation, "progressMarks", [this, an](CpuState& s, GuestMemory&) {
        std::vector<Address> values;
        for (const auto mark : an(s.x[0])->marks) values.push_back(autorelease(cf.real(mark)));
        s.x[0] = auto_array(values);
    });
    method(animation, "setProgressMarks:", [this, an](CpuState& s, GuestMemory&) {
        auto& marks = an(s.x[0])->marks;
        marks.clear();
        for (const auto value : items(s.x[2])) marks.push_back(static_cast<float>(send(value, "doubleValue").d(0)));
        std::sort(marks.begin(), marks.end());
    }, "v@:@");
    method(animation, "addProgressMark:", [an](CpuState& s, GuestMemory&) {
        auto& marks = an(s.x[0])->marks;
        const auto mark = std::clamp(f(s, 0), 0.0f, 1.0f);
        if (std::find(marks.begin(), marks.end(), mark) == marks.end()) { marks.push_back(mark); std::sort(marks.begin(), marks.end()); }
    }, "v@:f");
    method(animation, "removeProgressMark:", [an](CpuState& s, GuestMemory&) {
        auto& marks = an(s.x[0])->marks;
        marks.erase(std::remove(marks.begin(), marks.end(), f(s, 0)), marks.end());
    }, "v@:f");
    method(animation, "_alchemyBeginAnimation", [](CpuState&, GuestMemory&) {}, "v@:");
    method(animation, "_alchemyEndAnimation", [](CpuState&, GuestMemory&) {}, "v@:");
    method(view_animation, "initWithViewAnimations:", [this, an](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.set_d(0, 0.5);
        registers.x[2] = 0;   // NSAnimationEaseInOut
        const auto self = objc.send_super(s.x[0], view_animation, sel("initWithDuration:animationCurve:"), registers).x0;
        s.x[0] = self;
        an(self)->view_animations = s.x[2] ? send(s.x[2], "copy").x0 : 0;
    });
    method(view_animation, "viewAnimations", [an](CpuState& s, GuestMemory&) { s.x[0] = an(s.x[0])->view_animations; });
    method(view_animation, "setViewAnimations:", [this, an](CpuState& s, GuestMemory&) {
        release(std::exchange(an(s.x[0])->view_animations, s.x[2] ? send(s.x[2], "copy").x0 : 0));
    }, "v@:@");
    // One target of a view animation.
    struct Target { Address object{}; bool is_window{}; std::optional<Rect> start, end; std::string effect; };
    const auto targets_of = [this, an](Address self) {
        std::vector<Target> list;
        for (const auto entry : items(an(self)->view_animations)) {
            const auto value = [&](std::string_view key) { return send_id(entry, "objectForKey:", {auto_string(key)}); };
            Target t;
            t.object = value("NSViewAnimationTargetKey");
            if (!t.object) continue;
            t.is_window = is_kind(t.object, window);
            if (const auto start = value("NSViewAnimationStartFrameKey")) t.start = rect_value(*this, start);
            if (const auto end = value("NSViewAnimationEndFrameKey")) t.end = rect_value(*this, end);
            if (const auto effect = value("NSViewAnimationEffectKey")) t.effect = text(effect);
            list.push_back(t);
        }
        return list;
    };
    const auto frame_of = [this](const Target& t) {
        return result_rect(send(t.object, "frame"));
    };
    const auto set_frame = [this](const Target& t, Rect r) {
        CallRegs registers;
        put_rect(registers, 0, r);
        registers.x[2] = 1;
        static_cast<void>(send(t.object, t.is_window ? "setFrame:display:" : "setFrame:", registers));
    };
    static std::map<Address, std::vector<std::pair<Address, Rect>>> animation_starts;   // animation -> target start frames
    method(view_animation, "_alchemyBeginAnimation", [this, targets_of, frame_of, set_frame](CpuState& s, GuestMemory&) {
        auto& starts = animation_starts[s.x[0]];
        starts.clear();
        for (const auto& t : targets_of(s.x[0])) {
            if (t.start) set_frame(t, *t.start);
            starts.emplace_back(t.object, t.start.value_or(frame_of(t)));
            if (t.effect == "NSViewAnimationFadeInEffect") {   // shown from the start (no transparency ramp)
                if (t.is_window) static_cast<void>(send_id(t.object, "orderFront:", {0}));
                else static_cast<void>(send_id(t.object, "setHidden:", {0}));
            }
        }
    }, "v@:");
    method(view_animation, "setCurrentProgress:", [this, an, targets_of, set_frame](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        an(self)->progress = std::clamp<double>(f(s, 0), 0, 1);
        const auto value = eased(an(self)->curve, an(self)->progress);
        const auto& starts = animation_starts[self];
        for (const auto& t : targets_of(self)) {
            if (!t.end) continue;
            Rect from = *t.end;
            for (const auto& [object, frame] : starts) if (object == t.object) from = frame;
            const auto mix = [&](double a, double b) { return a + (b - a) * value; };
            set_frame(t, {{mix(from.origin.x, t.end->origin.x), mix(from.origin.y, t.end->origin.y)},
                          {mix(from.size.width, t.end->size.width), mix(from.size.height, t.end->size.height)}});
        }
    }, "v@:f");
    method(view_animation, "_alchemyEndAnimation", [this, targets_of, set_frame](CpuState& s, GuestMemory&) {
        for (const auto& t : targets_of(s.x[0])) {
            if (t.end) set_frame(t, *t.end);
            if (t.effect == "NSViewAnimationFadeOutEffect") {
                if (t.is_window) static_cast<void>(send_id(t.object, "orderOut:", {0}));
                else static_cast<void>(send_id(t.object, "setHidden:", {1}));
            }
        }
        animation_starts.erase(s.x[0]);
    }, "v@:");

    // ---- NSLayoutManager (line metrics) ----
    const auto layout_manager = declare_class("NSLayoutManager", "NSObject");
    method(layout_manager, "defaultLineHeightForFont:", [this](CpuState& s, GuestMemory&) {
        set_d(s, 0, std::ceil(drawing::font_metrics(font_spec(s.x[2])).line_height()));
    }, "d@:@");
    method(layout_manager, "defaultBaselineOffsetForFont:", [this](CpuState& s, GuestMemory&) {
        set_d(s, 0, std::ceil(drawing::font_metrics(font_spec(s.x[2])).ascent));
    }, "d@:@");
    method(layout_manager, "usesFontLeading", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    method(layout_manager, "setUsesFontLeading:", [](CpuState& s, GuestMemory&) {
        if (!bool_arg(s, 2)) unsupported("-[NSLayoutManager setUsesFontLeading:NO]");
    }, "v@:B");
}

void set_file_dialog_override(FileDialogPresenter presenter) {
    std::lock_guard lock(file_dialog_mutex);
    file_dialog_override = std::move(presenter);
}

} // namespace alchemy::gui::appkit
