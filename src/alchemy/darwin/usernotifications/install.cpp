#include "alchemy/darwin/usernotifications/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// UserNotifications: notifications are not allowed (authorization denied,
// requests fail with UNErrorCodeNotificationsNotAllowed); content, actions,
// categories and requests are real value objects.
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
    for (const auto target : {content, mutable_content})
        object_properties(target, {"title", "subtitle", "body", "userInfo", "attachments", "categoryIdentifier",
                                   "threadIdentifier", "sound", "badge", "targetContentIdentifier"});
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
    method(center, "getPendingNotificationRequestsWithCompletionHandler:", empty_list, false, "v@:@?");
    for (const auto* selector : {"removeDeliveredNotificationsWithIdentifiers:", "removePendingNotificationRequestsWithIdentifiers:"})
        method(center, selector, nothing, false, "v@:@");   // nothing was delivered or is pending
    for (const auto* selector : {"removeAllDeliveredNotifications", "removeAllPendingNotificationRequests"})
        method(center, selector, nothing, false, "v@:");
}

} // namespace

void install_usernotifications(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::usernotifications
