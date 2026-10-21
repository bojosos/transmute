// AppKit's NSWorkspace, NSRunningApplication and accessibility surface as
// Chrome reaches it (every provider installed, alchemy/gui/tests/fill_fixture.hpp):
// the workspace's own notification center, the frontmost application, the
// application lookups by bundle identifier, the current application
// activating itself, and the NSAccessibility informal protocol's defaults.
#include "alchemy/gui/tests/fill_fixture.hpp"

namespace {
using namespace fill_test;

constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

void workspace(Fixture& f) {
    const auto workspace = f.send(f.cls("NSWorkspace"), "sharedWorkspace");
    require(workspace && workspace == f.send(f.cls("NSWorkspace"), "sharedWorkspace"), "sharedWorkspace is one object");
    const auto center = f.send(workspace, "notificationCenter");
    require(center && center == f.send(workspace, "notificationCenter"), "notificationCenter is one object");
    require(f.send(center, "isKindOfClass:", {f.cls("NSNotificationCenter")}) & 0xff, "notificationCenter is an NSNotificationCenter");
    require(center != f.send(f.cls("NSNotificationCenter"), "defaultCenter"), "the workspace center is not the default center");
    // Only the running application has a bundle identifier LaunchServices knows.
    const auto other = f.cf.string("com.example.alchemy-not-installed");
    require(f.send(workspace, "URLForApplicationWithBundleIdentifier:", {other}) == 0, "an unknown bundle identifier has no URL");
    require(f.cf.array_values(f.send(workspace, "URLsForApplicationsWithBundleIdentifier:", {other})).empty(),
            "an unknown bundle identifier has no URLs");
    require(f.send(workspace, "URLForApplicationWithBundleIdentifier:", {0}) == 0, "a nil identifier has no URL");
    const auto bundle = f.call(core_foundation, "_CFBundleGetMainBundle");
    if (const auto mine = bundle ? f.call(core_foundation, "_CFBundleGetIdentifier", {bundle}) : 0) {
        const auto url = f.send(workspace, "URLForApplicationWithBundleIdentifier:", {mine});
        require(url && f.cf.equal(url, f.call(core_foundation, "_CFBundleCopyBundleURL", {bundle})), "the own identifier names the main bundle");
        require(f.cf.array_values(f.send(workspace, "URLsForApplicationsWithBundleIdentifier:", {mine})).size() == 1, "one URL for the own identifier");
    }
}

void running_application(Fixture& f) {
    const auto running = f.cls("NSRunningApplication");
    const auto current = f.send(running, "currentApplication");
    require(current && current == f.send(running, "currentApplication"), "currentApplication is one object");
    const auto app = f.send(f.cls("NSApplication"), "sharedApplication");
    require(app, "NSApp");
    const auto workspace = f.send(f.cls("NSWorkspace"), "sharedWorkspace");
    const auto frontmost = [&] { return f.send(workspace, "frontmostApplication"); };
    // Frontmost while active; a Windows program is in front otherwise (nil).
    require(frontmost() == ((f.send(app, "isActive") & 0xff) ? current : 0), "frontmostApplication follows NSApp's active state");
    require(f.send(workspace, "menuBarOwningApplication") == frontmost(), "menuBarOwningApplication is the frontmost application");
    require((f.send(current, "activateWithOptions:", {0}) & 0xff) == 1, "the current application activates itself");
    require(frontmost() == ((f.send(app, "isActive") & 0xff) ? current : 0), "frontmostApplication after activation");
    require((f.send(current, "hide") & 0xff) == 1 && (f.send(current, "unhide") & 0xff) == 1, "the current application hides and unhides");
    require((f.send(current, "isHidden") & 0xff) == (f.send(app, "isHidden") & 0xff), "isHidden is NSApp's");
    // Another NSRunningApplication instance is not the current application:
    // it cannot be changed; nor is any application terminated this way.
    const auto stranger = f.send(f.send(running, "alloc"), "init");
    require((f.send(stranger, "activateWithOptions:", {0}) & 0xff) == 0, "another application does not activate");
    require((f.send(current, "terminate") & 0xff) == 0 && (f.send(current, "forceTerminate") & 0xff) == 0, "terminate reports failure");
    require(f.send(running, "runningApplicationWithProcessIdentifier:", {0x7ffffffe}) == 0, "an unknown pid has no application");
}

// The NSAccessibility informal protocol's NSObject defaults (what subclasses
// reach through super) and an NSAccessibilityElement subclass's tree.
void accessibility(Fixture& f) {
    const auto object = f.send(f.send(f.cls("NSObject"), "alloc"), "init");
    const auto names = [&](const char* selector) { return f.cf.array_values(f.send(object, selector)).size(); };
    require(names("accessibilityAttributeNames") == 0 && names("accessibilityActionNames") == 0 &&
            names("accessibilityParameterizedAttributeNames") == 0, "an NSObject has no attributes or actions");
    const auto role = f.cf.string("AXRole");
    require((f.send(object, "accessibilityIsAttributeSettable:", {role}) & 0xff) == 0, "nothing is settable");
    static_cast<void>(f.send(object, "accessibilitySetValue:forAttribute:", {f.cf.string("x"), role}));
    static_cast<void>(f.send(object, "accessibilityPerformAction:", {f.cf.string("AXPress")}));
    require(f.send(object, "accessibilityAttributeValue:forParameter:", {role, 0}) == 0, "no parameterized values");
    require((f.send(object, "accessibilityIsIgnored") & 0xff) == 1, "a plain object is ignored");
    alchemy::CallRegs point;
    point.x[0] = object;
    require(f.objc.send(object, "accessibilityHitTest:", point).x0 == object, "hit testing answers the object");
    require(f.send(object, "accessibilityFocusedUIElement") == object, "focus answers the object");
    require((f.send(object, "accessibilityNotifiesWhenDestroyed") & 0xff) == 0, "no destruction notices");
    require(f.send(object, "accessibilityArrayAttributeCount:", {f.cf.string("AXChildren")}) == 0, "no children");

    // A guest-style subclass of NSAccessibilityElement (as Chrome's
    // AXPlatformNodeCocoa is), with children through the overrides table.
    const auto element_class = f.cls("NSAccessibilityElement");
    const auto subclass = f.objc.register_native_class("AlchemyTestAXNode", element_class, 16);
    require(subclass != 0, "NSAccessibilityElement is subclassable");
    const auto parent = f.send(f.send(subclass, "alloc"), "init");
    require((f.send(parent, "accessibilityIsIgnored") & 0xff) == 0, "an element is not ignored");
    std::vector<Address> kids;
    for (int index = 0; index < 3; ++index) kids.push_back(f.send(f.send(element_class, "alloc"), "init"));
    const auto children = f.cf.array(kids);
    static_cast<void>(f.send(parent, "accessibilitySetOverrideValue:forAttribute:", {children, f.cf.string("AXChildren")}));
    const auto ax_children = f.cf.string("AXChildren");
    require(f.send(parent, "accessibilityArrayAttributeCount:", {ax_children}) == 3, "three children");
    const auto last = f.send(parent, "accessibilityIndexOfChild:", {kids[2]});
    require(last == 2, "index of the last child");
    require(f.send(parent, "accessibilityIndexOfChild:", {object}) == 0x7fffffffffffffffULL, "a stranger is NSNotFound");
    alchemy::CallRegs window;
    window.x[2] = ax_children; window.x[3] = 1; window.x[4] = 5;
    const auto tail = f.cf.array_values(f.objc.send(parent, "accessibilityArrayAttributeValues:index:maxCount:", window).x0);
    require(tail.size() == 2 && tail[0] == kids[1] && tail[1] == kids[2], "the values from index 1 (max 5)");
    window.x[3] = 7;
    require(f.cf.array_values(f.objc.send(parent, "accessibilityArrayAttributeValues:index:maxCount:", window).x0).empty(),
            "an index past the end gives none");
}

} // namespace

int main() {
    return run("appkit-workspace", {{"workspace", workspace}, {"running application", running_application},
                                  {"accessibility", accessibility}});
}
