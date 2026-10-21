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
});

#undef LIBRARY
} // namespace
