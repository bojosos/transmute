// UserNotifications (src/alchemy/darwin/usernotifications/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/UserNotifications.framework/Versions/A/UserNotifications"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case usernotifications_case("usernotifications", [](Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library(LIBRARY), "UserNotifications identity");
    const auto center = f.send(f.cls("UNUserNotificationCenter"), "currentNotificationCenter");
    require(center && center == f.send(f.cls("UNUserNotificationCenter"), "currentNotificationCenter"), "notification center singleton");
    const auto content = f.send(f.send(f.cls("UNMutableNotificationContent"), "alloc"), "init");
    static_cast<void>(f.send(content, "setTitle:", {f.cf.string("Hello")}));
    require(f.cf.utf8(f.send(content, "title")) == "Hello", "content title");
    const auto action = f.send(f.cls("UNNotificationAction"), "actionWithIdentifier:title:options:", {f.cf.string("id"), f.cf.string("T"), 4});
    require(f.cf.utf8(f.send(action, "identifier")) == "id" && f.send(action, "options") == 4, "action");
    require(f.string_constant(LIBRARY, "_UNNotificationDefaultActionIdentifier") == "com.apple.UNNotificationDefaultActionIdentifier", "default action");
    // Triggers.
    alchemy::CallRegs interval{};
    interval.set_d(0, 120.0);
    interval.x[2] = 1;
    const auto trigger = f.objc.send(f.cls("UNTimeIntervalNotificationTrigger"), "triggerWithTimeInterval:repeats:", interval).x0;
    require(trigger && f.send(trigger, "repeats") == 1 && f.send(trigger, "nextTriggerDate") != 0, "UNTimeIntervalNotificationTrigger");
    bool refused{};
    alchemy::CallRegs short_repeat{};
    short_repeat.set_d(0, 5.0);
    short_repeat.x[2] = 1;
    try { static_cast<void>(f.objc.send(f.cls("UNTimeIntervalNotificationTrigger"), "triggerWithTimeInterval:repeats:", short_repeat)); }
    catch (const std::exception&) { refused = true; }
    require(refused, "a repeating trigger needs at least 60 seconds");
    const auto calendar = f.send(f.cls("UNCalendarNotificationTrigger"), "triggerWithDateMatchingComponents:repeats:", {f.cf.string("components"), 0});
    require(calendar && f.send(calendar, "repeats") == 0 && f.send(calendar, "dateComponents") != 0, "UNCalendarNotificationTrigger");
    // A real NSDateComponents: the next 09:00 (NSCalendar's nextDateAfterDate:matchingComponents:options:).
    const auto components = f.send(f.send(f.cls("NSDateComponents"), "alloc"), "init");
    static_cast<void>(f.send(components, "setHour:", {9}));
    const auto daily = f.send(f.cls("UNCalendarNotificationTrigger"), "triggerWithDateMatchingComponents:repeats:", {components, 1});
    require(f.send(daily, "nextTriggerDate") != 0, "a calendar trigger's next date");
    const auto icon = f.send(f.cls("UNNotificationActionIcon"), "iconWithSystemImageName:", {f.cf.string("bell")});
    require(f.cf.utf8(f.send(icon, "systemImageName")) == "bell", "UNNotificationActionIcon");
    require(f.send(center, "supportsContentExtensions") == 0, "no content extensions");
});

#undef LIBRARY
} // namespace
