#include "alchemy/gui/value_provider.hpp"

#include <algorithm>

// NSUserNotificationCenter (denied, see below), and the macOS 13-15 NSMenu / NSMenuItem API Chrome's start-up menu building reaches
// (main menu, application menu, Services), added to the AppKit provider's
// classes where they lack it. Menus are never displayed under Alchemy, so
// presentation properties are stored and returned:
// - +[NSMenuItem sectionHeaderWithTitle:] (macOS 14): a disabled item with the
//   title, isSectionHeader YES.
// - NSMenuItem subtitle, badge (NSMenuItemBadge), the on/off/mixed state
//   images, allowsKeyEquivalentWhenHidden, the automatic key-equivalent
//   localization/mirroring flags (default YES), isHighlighted (NO: nothing is
//   tracked), userKeyEquivalent ("": no user key bindings).
// - NSMenu presentationStyle (Regular 0 / Palette 1), selectionMode
//   (Automatic 0 / SelectOne 1 / SelectAny 2), selectedItems (the items whose
//   state is on), userInterfaceLayoutDirection, +paletteMenuWithColors:titles:
//   selectionHandler: (one item per color; the color is the item's
//   represented object); the menu bar is never shown (+menuBarVisible NO,
//   -menuBarHeight 0).
// - Copies (-copyWithZone:, AppKit's) carry these stored values along.
// - NSMenuItemBadge: count or string badges of type None 0 / Updates 1 /
//   NewItems 2 / Alerts 3.
namespace alchemy::gui::values {
namespace {

constexpr std::int64_t badge_none = 0, badge_updates = 1, badge_new_items = 2, badge_alerts = 3;
constexpr std::int64_t state_on = 1, state_off = 0;
constexpr std::int64_t presentation_palette = 1, selection_any = 2;

} // namespace

void Provider::install_menus() {
    if (!objc.find_class("NSMenu") || !objc.find_class("NSMenuItem")) return;
    // Their -dealloc / -copyWithZone: (AppKit's) clear and copy the macOS 14
    // fields below.
    observe_appkit_lifetimes();
    const auto object_property = [this](std::string_view cls, std::string_view name) {
        const std::string key(name);
        extend(cls, name, [this, key](CpuState& s, GuestMemory&) { s.x[0] = stored(s.x[0], key); });
        extend(cls, setter_name(name), [this, key](CpuState& s, GuestMemory&) { store(s.x[0], key, s.x[2]); }, false, "v@:@");
    };
    // An integer/BOOL property; `default_value` when never set.
    const auto integer_property = [this](std::string_view cls, std::string_view getter, std::string_view setter,
                                         std::int64_t default_value, const char* type) {
        const std::string key(setter);
        extend(cls, getter, [this, key, default_value](CpuState& s, GuestMemory&) {
            const auto state = properties(s.x[0], false);
            std::int64_t value = default_value;
            if (state) {
                std::lock_guard lock(state->mutex);
                if (const auto found = state->integers.find(key); found != state->integers.end()) value = found->second;
            }
            s.x[0] = static_cast<Address>(value);
        }, false, std::string(type) + "@:");
        extend(cls, setter, [this, key, type](CpuState& s, GuestMemory&) {
            const std::int64_t value = std::string_view(type) == "B" ? static_cast<std::int64_t>(s.x[2] & 0xff) : static_cast<std::int64_t>(s.x[2]);
            store_integer(s.x[0], key, value);
        }, false, std::string("v@:") + type);
    };

    // ---- NSMenuItem ----
    extend("NSMenuItem", "sectionHeaderWithTitle:", [this](CpuState& s, GuestMemory&) {
        CallRegs registers{};
        const auto empty = cf.string("");
        registers.x[2] = s.x[2] ? s.x[2] : empty;
        registers.x[3] = 0;
        registers.x[4] = empty;
        const auto item = objc.send(objc.allocate(s.x[0]), "initWithTitle:action:keyEquivalent:", registers).x0;
        release(empty);
        CallRegs disable{};
        disable.x[2] = 0;
        static_cast<void>(objc.send(item, "setEnabled:", disable));
        store_integer(item, "sectionHeader", 1);
        s.x[0] = bridge->autorelease(item);
    }, true);
    extend("NSMenuItem", "isSectionHeader", [this](CpuState& s, GuestMemory&) { s.x[0] = stored_integer(s.x[0], "sectionHeader") != 0; }, false, "B@:");
    for (const auto* name : {"subtitle", "badge", "onStateImage", "offStateImage", "mixedStateImage", "identifier"}) object_property("NSMenuItem", name);
    integer_property("NSMenuItem", "allowsKeyEquivalentWhenHidden", "setAllowsKeyEquivalentWhenHidden:", 0, "B");
    integer_property("NSMenuItem", "allowsAutomaticKeyEquivalentLocalization", "setAllowsAutomaticKeyEquivalentLocalization:", 1, "B");
    integer_property("NSMenuItem", "allowsAutomaticKeyEquivalentMirroring", "setAllowsAutomaticKeyEquivalentMirroring:", 1, "B");
    extend("NSMenuItem", "isHighlighted", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    extend("NSMenuItem", "userKeyEquivalent", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.string("")); });

    // ---- NSMenuItemBadge ----
    const auto badge = cls(fill::appkit, "NSMenuItemBadge", "NSObject");
    object_properties(badge, {"stringValue"});
    integer_properties(badge, {"itemCount", "type"});
    method(badge, "initWithCount:type:", [this](CpuState& s, GuestMemory&) {
        store_integer(s.x[0], "itemCount", static_cast<std::int64_t>(s.x[2]));
        store_integer(s.x[0], "type", static_cast<std::int64_t>(s.x[3]));
    }, false, "@@:qq");
    method(badge, "initWithCount:", [this](CpuState& s, GuestMemory&) {
        store_integer(s.x[0], "itemCount", static_cast<std::int64_t>(s.x[2]));
        store_integer(s.x[0], "type", badge_none);
    }, false, "@@:q");
    method(badge, "initWithString:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "stringValue", s.x[2]);
        store_integer(s.x[0], "type", badge_none);
    });
    for (const auto& [selector, type] : {std::pair{"updatesWithCount:", badge_updates}, std::pair{"newItemsWithCount:", badge_new_items},
                                         std::pair{"alertsWithCount:", badge_alerts}}) {
        method(badge, selector, [this, type](CpuState& s, GuestMemory&) {
            const auto object = create(s.x[0]);
            store_integer(object, "itemCount", static_cast<std::int64_t>(s.x[2]));
            store_integer(object, "type", type);
            s.x[0] = bridge->autorelease(object);
        }, true, "@@:q");
    }

    // ---- NSMenu ----
    integer_property("NSMenu", "presentationStyle", "setPresentationStyle:", 0, "q");
    integer_property("NSMenu", "selectionMode", "setSelectionMode:", 0, "q");
    integer_property("NSMenu", "userInterfaceLayoutDirection", "setUserInterfaceLayoutDirection:", 0, "q");
    // selectedItems: the items whose state is on; setSelectedItems: turns the
    // given items on and every other item off.
    extend("NSMenu", "selectedItems", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> selected;
        const auto list = objc.send(s.x[0], "itemArray").x0;
        for (const auto item : list ? cf.array_values(list) : std::vector<Address>{})
            if (static_cast<std::int64_t>(objc.send(item, "state").x0) == state_on) selected.push_back(item);
        s.x[0] = bridge->autorelease(cf.array(selected));
    });
    extend("NSMenu", "setSelectedItems:", [this](CpuState& s, GuestMemory&) {
        const auto wanted = s.x[2] ? cf.array_values(s.x[2]) : std::vector<Address>{};
        const auto list = objc.send(s.x[0], "itemArray").x0;
        for (const auto item : list ? cf.array_values(list) : std::vector<Address>{}) {
            CallRegs registers{};
            registers.x[2] = static_cast<Address>(std::find(wanted.begin(), wanted.end(), item) != wanted.end() ? state_on : state_off);
            static_cast<void>(objc.send(item, "setState:", registers));
        }
    }, false, "v@:@");
    // +paletteMenuWithColors:titles:selectionHandler: (and the templateImage: variant).
    const auto palette = [this](CpuState& s, Address handler) {
        const auto menu = objc.send(objc.allocate(s.x[0]), "init").x0;
        store_integer(menu, "setPresentationStyle:", presentation_palette);
        store_integer(menu, "setSelectionMode:", selection_any);
        if (handler) {
            const auto copied = bridge->block_copy(handler);
            store(menu, "paletteSelectionHandler", copied);
            bridge->block_release(copied);
        }
        const auto colors = s.x[2] ? cf.array_values(s.x[2]) : std::vector<Address>{};
        const auto titles = s.x[3] ? cf.array_values(s.x[3]) : std::vector<Address>{};
        const auto empty = cf.string("");
        for (std::size_t i = 0; i < colors.size(); ++i) {
            CallRegs registers{};
            registers.x[2] = i < titles.size() ? titles[i] : empty;
            registers.x[3] = 0;
            registers.x[4] = empty;
            const auto item = objc.send(objc.send(objc.find_class("NSMenuItem"), "alloc").x0, "initWithTitle:action:keyEquivalent:", registers).x0;
            CallRegs represented{};
            represented.x[2] = colors[i];
            static_cast<void>(objc.send(item, "setRepresentedObject:", represented));
            CallRegs add{};
            add.x[2] = item;
            static_cast<void>(objc.send(menu, "addItem:", add));
            release(item);
        }
        release(empty);
        s.x[0] = bridge->autorelease(menu);
    };
    extend("NSMenu", "paletteMenuWithColors:titles:selectionHandler:", [palette](CpuState& s, GuestMemory&) { palette(s, s.x[4]); }, true);
    extend("NSMenu", "paletteMenuWithColors:titles:templateImage:selectionHandler:", [palette](CpuState& s, GuestMemory&) { palette(s, s.x[5]); }, true);
    // The menu bar is never shown: Alchemy windows have no global menu bar.
    extend("NSMenu", "menuBarVisible", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "B@:");
    extend("NSMenu", "setMenuBarVisible:", [](CpuState&, GuestMemory&) {}, true, "v@:B");
    extend("NSMenu", "menuBarHeight", [](CpuState& s, GuestMemory&) { fill::return_real(s, 0.0); }, false, "d@:");

    // ---- NSUserNotificationCenter / NSUserNotification (deprecated Foundation API) ----
    // Notifications are denied, as with UserNotifications: delivering or
    // scheduling shows nothing and no notification is ever delivered or
    // pending, so the lists stay empty and the delegate is never called.
    const auto notification = objc.find_class("NSUserNotification") ? objc.find_class("NSUserNotification")
                                                                     : cls(fill::foundation, "NSUserNotification", "NSObject");
    static_cast<void>(notification);
    track_dealloc("NSUserNotification");
    for (const auto* name : {"title", "subtitle", "informativeText", "identifier", "userInfo", "soundName", "actionButtonTitle",
                             "otherButtonTitle", "deliveryDate", "contentImage", "response", "responsePlaceholder"})
        object_property("NSUserNotification", name);
    integer_property("NSUserNotification", "hasActionButton", "setHasActionButton:", 1, "B");
    integer_property("NSUserNotification", "hasReplyButton", "setHasReplyButton:", 0, "B");
    extend("NSUserNotification", "isPresented", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    extend("NSUserNotification", "isRemote", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    const auto center = objc.find_class("NSUserNotificationCenter") ? objc.find_class("NSUserNotificationCenter")
                                                                     : cls(fill::foundation, "NSUserNotificationCenter", "NSObject");
    extend("NSUserNotificationCenter", "defaultUserNotificationCenter", [this, center](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        auto& instance = singletons[{center, "defaultUserNotificationCenter"}];
        if (!instance) {
            instance = objc.send(objc.allocate(center), "init").x0;
            if (const auto record = process.objects().find(instance)) record->immortal = true;
        }
        s.x[0] = instance;
    }, true);
    extend("NSUserNotificationCenter", "delegate", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(stored_integer(s.x[0], "delegate")); });
    extend("NSUserNotificationCenter", "setDelegate:", [this](CpuState& s, GuestMemory&) {
        store_integer(s.x[0], "delegate", static_cast<std::int64_t>(s.x[2]));
    }, false, "v@:@");
    for (const auto* selector : {"deliverNotification:", "scheduleNotification:", "removeDeliveredNotification:", "removeScheduledNotification:",
                                 "setScheduledNotifications:"})
        extend("NSUserNotificationCenter", selector, [](CpuState&, GuestMemory&) {}, false, "v@:@");
    extend("NSUserNotificationCenter", "removeAllDeliveredNotifications", [](CpuState&, GuestMemory&) {}, false, "v@:");
    for (const auto* selector : {"deliveredNotifications", "scheduledNotifications"})
        extend("NSUserNotificationCenter", selector, [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); });
}

} // namespace alchemy::gui::values
