#include "alchemy/darwin/usernotifications/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <bit>
#include <stdexcept>

// UserNotifications: notifications are not allowed (authorization denied,
// requests and badge counts fail with UNErrorCodeNotificationsNotAllowed);
// content, actions, action icons, categories, triggers (time interval with
// its next fire date, calendar, push), notifications and requests are real
// value objects. Categories set on the center read back from
// getNotificationCategoriesWithCompletionHandler:.
// String constants carry the values of Apple's public headers; values
// marked "(unverified)" follow the framework's naming convention and could
// not be checked against a header. Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::usernotifications {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view user_notifications = "/System/Library/Frameworks/UserNotifications.framework/Versions/A/UserNotifications";
constexpr std::int64_t un_error_notifications_not_allowed = 1;
constexpr std::int64_t un_authorization_status_denied = 1;

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(user_notifications));
    const auto nothing = [](CpuState&, GuestMemory&) {};
    string(user_notifications, "_UNNotificationDefaultActionIdentifier", "com.apple.UNNotificationDefaultActionIdentifier");
    string(user_notifications, "_UNNotificationDismissActionIdentifier", "com.apple.UNNotificationDismissActionIdentifier");
    string(user_notifications, "_UNNotificationAttachmentOptionsTypeHintKey", "UNNotificationAttachmentOptionsTypeHintKey");   // (unverified)
    string(user_notifications, "_UNErrorDomain", "UNErrorDomain");
    const auto content = cls(user_notifications, "UNNotificationContent", "NSObject");
    const auto mutable_content = cls(user_notifications, "UNMutableNotificationContent", "UNNotificationContent");
    for (const auto target : {content, mutable_content}) {
        object_properties(target, {"title", "subtitle", "body", "userInfo", "attachments", "categoryIdentifier",
                                   "threadIdentifier", "sound", "badge", "targetContentIdentifier", "launchImageName",
                                   "summaryArgument", "filterCriteria"});
        integer_properties(target, {"summaryArgumentCount"});
        real_properties(target, {"relevanceScore"});
    }
    integer_properties(content, {"interruptionLevel"});
    integer_properties(mutable_content, {"interruptionLevel"});
    const auto action = cls(user_notifications, "UNNotificationAction", "NSObject");
    object_properties(action, {"identifier", "title"});
    integer_properties(action, {"options"});
    const auto text_action = cls(user_notifications, "UNTextInputNotificationAction", "UNNotificationAction");
    object_properties(text_action, {"identifier", "title", "textInputButtonTitle", "textInputPlaceholder"});
    integer_properties(text_action, {"options"});
    method(action, "actionWithIdentifier:title:options:", [this](CpuState& s, GuestMemory&) {
        const auto object = create(s.x[0]);
        store(object, "identifier", s.x[2]);
        store(object, "title", s.x[3]);
        store_integer(object, "options", static_cast<std::int64_t>(s.x[4]));
        s.x[0] = bridge->autorelease(object);
    }, true);
    method(text_action, "actionWithIdentifier:title:options:textInputButtonTitle:textInputPlaceholder:", [this](CpuState& s, GuestMemory&) {
        const auto object = create(s.x[0]);
        store(object, "identifier", s.x[2]);
        store(object, "title", s.x[3]);
        store_integer(object, "options", static_cast<std::int64_t>(s.x[4]));
        store(object, "textInputButtonTitle", s.x[5]);
        store(object, "textInputPlaceholder", s.x[6]);
        s.x[0] = bridge->autorelease(object);
    }, true);
    const auto category = cls(user_notifications, "UNNotificationCategory", "NSObject");
    object_properties(category, {"identifier", "actions", "intentIdentifiers"});
    integer_properties(category, {"options"});
    method(category, "categoryWithIdentifier:actions:intentIdentifiers:options:", [this](CpuState& s, GuestMemory&) {
        const auto object = create(s.x[0]);
        store(object, "identifier", s.x[2]);
        store(object, "actions", s.x[3]);
        store(object, "intentIdentifiers", s.x[4]);
        store_integer(object, "options", static_cast<std::int64_t>(s.x[5]));
        s.x[0] = bridge->autorelease(object);
    }, true);
    // Triggers.
    const auto trigger = cls(user_notifications, "UNNotificationTrigger", "NSObject");
    integer_properties(trigger, {"repeats"});
    const auto interval_trigger = cls(user_notifications, "UNTimeIntervalNotificationTrigger", "UNNotificationTrigger");
    method(interval_trigger, "timeInterval", [this](CpuState& s, GuestMemory&) {
        s.v[0][0] = static_cast<std::uint64_t>(stored_integer(s.x[0], "interval"));   // the double's bits
    }, false, "d@:");
    method(interval_trigger, "triggerWithTimeInterval:repeats:", [this, interval_trigger](CpuState& s, GuestMemory&) {
        const auto interval = std::bit_cast<double>(s.v[0][0]);
        if (!(interval > 0) || ((s.x[2] & 0xff) && interval < 60)) {   // the documented minimum for repeating triggers
            throw std::invalid_argument("Alchemy: UNTimeIntervalNotificationTrigger: the time interval must be greater than 0 (60 when repeating)");
        }
        const auto object = create(interval_trigger);
        store_integer(object, "interval", static_cast<std::int64_t>(s.v[0][0]));
        store_integer(object, "repeats", (s.x[2] & 0xff) ? 1 : 0);
        const auto created = bridge->cf.date(CoreFoundation::absolute_time_now());
        store(object, "created", created);
        release(created);
        s.x[0] = bridge->autorelease(object);
    }, true, "@@:dB");
    // -nextTriggerDate: the creation time plus the interval.
    method(interval_trigger, "nextTriggerDate", [this](CpuState& s, GuestMemory&) {
        const auto created = stored(s.x[0], "created");
        const auto interval = std::bit_cast<double>(static_cast<std::uint64_t>(stored_integer(s.x[0], "interval")));
        s.x[0] = created ? bridge->autorelease(bridge->cf.date(bridge->cf.date_time(created) + interval)) : 0;
    });
    const auto calendar_trigger = cls(user_notifications, "UNCalendarNotificationTrigger", "UNNotificationTrigger");
    object_properties(calendar_trigger, {"dateComponents"});
    method(calendar_trigger, "triggerWithDateMatchingComponents:repeats:", [this, calendar_trigger](CpuState& s, GuestMemory&) {
        const auto object = create(calendar_trigger);
        store(object, "dateComponents", s.x[2]);
        store_integer(object, "repeats", (s.x[3] & 0xff) ? 1 : 0);
        s.x[0] = bridge->autorelease(object);
    }, true, "@@:@B");
    // -nextTriggerDate: the next date matching the components (NSCalendar's own matching).
    method(calendar_trigger, "nextTriggerDate", [this](CpuState& s, GuestMemory&) {
        const auto components = stored(s.x[0], "dateComponents");
        const auto calendar_class = objc.find_class("NSCalendar");
        if (!components || !calendar_class) { s.x[0] = 0; return; }
        const auto calendar = objc.send(calendar_class, objc.selector("currentCalendar"), CallRegs{}).x0;
        const auto now = bridge->cf.date(CoreFoundation::absolute_time_now());
        CallRegs registers{};
        registers.x[2] = now;
        registers.x[3] = components;
        registers.x[4] = 1 << 10;   // NSCalendarMatchNextTime
        const auto next = objc.send_if_responds(calendar, objc.selector("nextDateAfterDate:matchingComponents:options:"), registers);
        s.x[0] = next ? next->x0 : 0;
        release(now);
    });
    static_cast<void>(cls(user_notifications, "UNPushNotificationTrigger", "UNNotificationTrigger"));
    // A delivered notification (none is delivered here; a value object).
    const auto notification = cls(user_notifications, "UNNotification", "NSObject");
    object_properties(notification, {"date", "request"});
    // Action icons.
    const auto icon = cls(user_notifications, "UNNotificationActionIcon", "NSObject");
    object_properties(icon, {"systemImageName", "templateImageName"});
    for (const auto& [selector, key] : std::initializer_list<std::pair<const char*, const char*>>{
             {"iconWithSystemImageName:", "systemImageName"}, {"iconWithTemplateImageName:", "templateImageName"}})
        method(icon, selector, [this, icon, name = key](CpuState& s, GuestMemory&) {
            const auto object = create(icon);
            store(object, name, s.x[2]);
            s.x[0] = bridge->autorelease(object);
        }, true);
    const auto request = cls(user_notifications, "UNNotificationRequest", "NSObject");
    object_properties(request, {"identifier", "content", "trigger"});
    method(request, "requestWithIdentifier:content:trigger:", [this](CpuState& s, GuestMemory&) {
        const auto object = create(s.x[0]);
        store(object, "identifier", s.x[2]);
        store(object, "content", s.x[3]);
        store(object, "trigger", s.x[4]);
        s.x[0] = bridge->autorelease(object);
    }, true);
    const auto attachment = cls(user_notifications, "UNNotificationAttachment", "NSObject");
    object_properties(attachment, {"identifier", "URL", "type"});
    method(attachment, "attachmentWithIdentifier:URL:options:error:", [this](CpuState& s, GuestMemory&) {
        const auto object = create(s.x[0]);
        store(object, "identifier", s.x[2]);
        store(object, "URL", s.x[3]);
        if (const auto hint = s.x[4] ? cf.dictionary_value(s.x[4], cf.string("UNNotificationAttachmentOptionsTypeHintKey")) : 0)
            store(object, "type", hint);
        s.x[0] = bridge->autorelease(object);
    }, true);
    const auto response = cls(user_notifications, "UNNotificationResponse", "NSObject");
    object_properties(response, {"notification", "actionIdentifier"});
    const auto text_response = cls(user_notifications, "UNTextInputNotificationResponse", "UNNotificationResponse");
    object_properties(text_response, {"notification", "actionIdentifier", "userText"});
    const auto settings = cls(user_notifications, "UNNotificationSettings", "NSObject");
    integer_properties(settings, {"authorizationStatus", "alertSetting", "soundSetting", "badgeSetting",
                                  "notificationCenterSetting", "lockScreenSetting", "alertStyle"});
    // UNNotificationSound: value objects (no notification is ever shown).
    const auto sound = cls(user_notifications, "UNNotificationSound", "NSObject");
    object_properties(sound, {"name"});
    singleton(sound, "defaultSound");
    singleton(sound, "defaultCriticalSound");
    method(sound, "soundNamed:", [this, sound](CpuState& s, GuestMemory&) {
        const auto object = create(sound);
        store(object, "name", s.x[2]);
        s.x[0] = bridge->autorelease(object);
    }, true);
    const auto center = cls(user_notifications, "UNUserNotificationCenter", "NSObject");
    singleton(center, "currentNotificationCenter");
    weak_properties(center, {"delegate"});
    object_properties(center, {"notificationCategories"});
    // Authorization is denied: there is no notification server.
    method(center, "requestAuthorizationWithOptions:completionHandler:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[3]) return;
        const auto block = bridge->block_copy(s.x[3]);
        bridge->dispatch_async(bridge->main_queue(), [this, block] {
            const auto error = bridge->error("UNErrorDomain", un_error_notifications_not_allowed, "Notifications are not allowed");
            static_cast<void>(bridge->invoke_block(block, {0, error}));
            release(error);
            bridge->block_release(block);
        });
    }, false, "v@:Q@?");
    method(center, "getNotificationSettingsWithCompletionHandler:", [this, settings](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto block = bridge->block_copy(s.x[2]);
        bridge->dispatch_async(bridge->main_queue(), [this, block, settings] {
            const auto value = create(settings);
            store_integer(value, "authorizationStatus", un_authorization_status_denied);
            for (const auto* key : {"alertSetting", "soundSetting", "badgeSetting", "notificationCenterSetting", "lockScreenSetting"})
                store_integer(value, key, 1);   // UNNotificationSettingDisabled
            static_cast<void>(bridge->invoke_block(block, {value}));
            release(value);
            bridge->block_release(block);
        });
    }, false, "v@:@?");
    method(center, "addNotificationRequest:withCompletionHandler:", [this](CpuState& s, GuestMemory&) {
        complete_with_error(s.x[3], "UNErrorDomain", un_error_notifications_not_allowed, "Notifications are not allowed", 0);
    }, false, "v@:@@?");
    const auto empty_list = [this](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto block = bridge->block_copy(s.x[2]);
        bridge->dispatch_async(bridge->main_queue(), [this, block] {
            const auto empty = cf.array({});
            static_cast<void>(bridge->invoke_block(block, {empty}));
            release(empty);
            bridge->block_release(block);
        });
    };
    method(center, "getDeliveredNotificationsWithCompletionHandler:", empty_list, false, "v@:@?");
    // The categories set on the center (an NSSet; empty when none was set).
    method(center, "getNotificationCategoriesWithCompletionHandler:", [this](CpuState& s, GuestMemory&) {
        if (!s.x[2]) return;
        const auto block = bridge->block_copy(s.x[2]);
        const auto center = retain(s.x[0]);
        bridge->dispatch_async(bridge->main_queue(), [this, block, center] {
            auto categories = retain(stored(center, "notificationCategories"));
            if (!categories) categories = retain(objc.send(objc.find_class("NSSet"), objc.selector("set"), CallRegs{}).x0);
            static_cast<void>(bridge->invoke_block(block, {categories}));
            release(categories);
            release(center);
            bridge->block_release(block);
        });
    }, false, "v@:@?");
    // Badges need the notification permission.
    method(center, "setBadgeCount:withCompletionHandler:", [this](CpuState& s, GuestMemory&) {
        complete_with_error(s.x[3], "UNErrorDomain", un_error_notifications_not_allowed, "Notifications are not allowed", 0);
    }, false, "v@:q@?");
    method(center, "supportsContentExtensions", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    method(center, "getPendingNotificationRequestsWithCompletionHandler:", empty_list, false, "v@:@?");
    for (const auto* selector : {"removeDeliveredNotificationsWithIdentifiers:", "removePendingNotificationRequestsWithIdentifiers:"})
        method(center, selector, nothing, false, "v@:@");   // nothing was delivered or is pending
    for (const auto* selector : {"removeAllDeliveredNotifications", "removeAllPendingNotificationRequests"})
        method(center, selector, nothing, false, "v@:");
}

} // namespace

void install_usernotifications(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::usernotifications
