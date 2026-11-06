#include "alchemy/darwin/kit/support/support_kit.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// ApplicationServices' accessibility client (HIServices AXUIElement /
// AXObserver) for this process's own accessibility tree. There is no
// accessibility server on the host: an element of another process answers
// kAXErrorCannotComplete, and observers can only be made for this process.
// * Elements wrap the process's accessibility objects: the application
//   (AXUIElementCreateApplication(getpid())), its windows, views and
//   NSAccessibilityElements. AXUIElementCreateSystemWide answers the focused
//   application and element.
// * Attribute values: an object's informal -accessibilityAttributeValue:
//   override first, else the NSAccessibility properties (role, subrole, role
//   description, title, value, description = label, help, identifier,
//   parent, children, window, enabled, focused, position and size), with
//   AppKit's defaults where the object sets none: the role of the standard
//   classes, a window's title, a text field's and a slider's value, a
//   control's enabled state, the first responder's focus, the frame on the
//   screen, and children that skip views that are not accessibility elements
//   (plain NSViews). Positions are in AX coordinates (top-left origin of the
//   main screen). Accessibility objects come back as AXUIElements, arrays
//   element by element; other values as they are (+1).
// * Actions: the informal action names; AXPress (a custom
//   -accessibilityPerformPress, else a control's -performClick:) and AXRaise
//   (a window ordered front and made key). Settable: AXValue (a text field's
//   string, a slider's number, else the informal setter) and AXFocused.
// * Observers: notifications posted with NSAccessibilityPostNotification /
//   ...WithUserInfo for an observed element (an observed application element
//   gets its descendants') reach the callback on the main run loop. The run
//   loop source an observer answers exists so applications can add it; the
//   delivery does not depend on it.
// AXIsProcessTrusted stays NO (no server to trust the process: services.cpp).
namespace alchemy::gui::support_kit {
namespace {

constexpr std::string_view application_services = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";
constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// AXError.
constexpr std::int32_t ax_success = 0, ax_illegal_argument = -25201, ax_invalid_element = -25202, ax_invalid_observer = -25203,
                       ax_cannot_complete = -25204, ax_attribute_unsupported = -25205, ax_action_unsupported = -25206,
                       ax_notification_already_registered = -25209, ax_notification_not_registered = -25210, ax_no_value = -25212,
                       ax_parameterized_unsupported = -25213;

Rect result_rect(const CallResult& r) { return {{r.d(0), r.d(1)}, {r.d(2), r.d(3)}}; }

struct Element final : ObjectPayload {
    std::int64_t pid{};
    Address object{};       // +1: the accessibility object (0 for another process's elements and the system-wide one)
    bool system_wide{};
};
struct Registration { Address element{}, notification{}, refcon{}; };   // element, notification +1
struct Observer final : ObjectPayload {
    std::int64_t pid{};
    Address callback{};
    bool with_info{};
    Address source{};       // +1 CFRunLoopSource (made on demand)
    std::vector<Registration> registrations;
};

class Client {
public:
    explicit Client(Support& value) : s(value) {}
    Support& s;
    Address element_class{}, observer_class{};
    Address scratch{};      // 32 bytes for AXValueCreate (under scratch_mutex)
    std::mutex scratch_mutex;
    std::mutex mutex;
    std::vector<Address> observers;   // live observers (not retained)

    std::uint64_t ret(std::int32_t code) { return static_cast<std::uint32_t>(code); }
    std::int64_t own_pid() { return static_cast<std::int32_t>(s.call(libsystem, "_getpid", {})); }
    std::shared_ptr<Element> element_of(Address object) { return object ? s.objc.find_host_state<Element>(object) : nullptr; }
    Address cls(std::string_view name) { return s.objc.find_class(name); }
    bool is(Address object, std::string_view class_name) {
        const auto c = cls(class_name);
        return object && c && (s.send(object, "isKindOfClass:", [&] { CallRegs r; r.x[2] = c; return r; }()).x0 & 0xff);
    }
    bool responds(Address object, std::string_view selector) { return object && s.objc.responds(object, s.objc.selector(selector)); }
    Address string(std::string_view text) { return s.cf.string(text); }   // +1
    std::string text(Address value) { return value && s.cf.kind(value) == ObjectKind::string ? s.cf.utf8(value) : std::string(); }
    Address application() {
        const auto app_class = cls("NSApplication");
        return app_class ? s.send_id(app_class, "sharedApplication") : 0;
    }

    // A +1 element for `object` of this process.
    Address make(Address object, bool system_wide = false) {
        const auto made = s.objc.allocate(element_class);
        const auto state = s.objc.host_state<Element>(made);
        state->pid = own_pid();
        state->object = s.retain(object);
        state->system_wide = system_wide;
        return made;
    }
    // Whether `object` is an accessibility object (wrapped as an element).
    bool accessible(Address object) {
        return is(object, "NSApplication") || is(object, "NSWindow") || is(object, "NSView") || is(object, "NSAccessibilityElement") ||
               responds(object, "accessibilityRole");
    }
    // A value as the client sees it: accessibility objects as elements, arrays element by element (+1).
    Address converted(Address value) {
        if (!value) return 0;
        if (accessible(value) && !responds(value, "objectAtIndex:")) return make(value);
        if (responds(value, "objectAtIndex:") && s.cf.kind(value) != ObjectKind::string) {
            std::vector<Address> items;
            const auto count = s.send(value, "count").x0;
            for (Address i = 0; i < count; ++i) {
                const auto item = s.send_id(value, "objectAtIndex:", {i});
                items.push_back(accessible(item) ? make(item) : s.retain(item));
            }
            const auto array = s.cf.array(items);
            for (const auto item : items) s.release(item);
            return array;
        }
        return s.retain(value);
    }

    // ---- AppKit's defaults ----
    std::string default_role(Address object) {
        static const std::pair<const char*, const char*> table[] = {
            {"NSApplication", "AXApplication"}, {"NSWindow", "AXWindow"}, {"NSPopUpButton", "AXPopUpButton"}, {"NSButton", "AXButton"},
            {"NSSecureTextField", "AXTextField"}, {"NSSearchField", "AXTextField"}, {"NSComboBox", "AXComboBox"}, {"NSSlider", "AXSlider"},
            {"NSImageView", "AXImage"}, {"NSOutlineView", "AXOutline"}, {"NSTableView", "AXTable"}, {"NSScrollView", "AXScrollArea"},
            {"NSTextView", "AXTextArea"}, {"NSProgressIndicator", "AXProgressIndicator"}, {"NSTabView", "AXTabGroup"},
            {"NSSplitView", "AXSplitGroup"}, {"NSStepper", "AXIncrementor"}, {"NSColorWell", "AXColorWell"}, {"NSBox", "AXGroup"},
            {"NSLevelIndicator", "AXLevelIndicator"}, {"NSPathControl", "AXPathControl"}};
        for (const auto& [name, role] : table) if (is(object, name)) return role;
        if (is(object, "NSTextField")) return (s.send(object, "isEditable").x0 & 0xff) ? "AXTextField" : "AXStaticText";
        return {};
    }
    std::string role_of(Address object) {
        if (responds(object, "accessibilityRole")) if (const auto role = text(s.send_id(object, "accessibilityRole")); !role.empty()) return role;
        return default_role(object);
    }
    // Whether the object is an element of the tree (a plain NSView is not).
    bool element(Address object) {
        if (!object) return false;
        if (responds(object, "isAccessibilityElement") && responds(object, "accessibilityRole") && !text(s.send_id(object, "accessibilityRole")).empty())
            return true;
        return !role_of(object).empty();
    }
    // A view's children that are elements, looking through views that are not.
    void element_children(Address view, std::vector<Address>& out, int depth = 0) {
        if (depth > 64) return;
        const auto subviews = s.send_id(view, "subviews");
        const auto count = subviews ? s.send(subviews, "count").x0 : 0;
        for (Address i = 0; i < count; ++i) {
            const auto child = s.send_id(subviews, "objectAtIndex:", {i});
            if (s.send(child, "isHidden").x0 & 0xff) continue;
            if (element(child)) out.push_back(child);
            else element_children(child, out, depth + 1);
        }
    }
    std::vector<Address> children_of(Address object) {
        std::vector<Address> out;
        if (responds(object, "accessibilityChildren")) {
            if (const auto set = s.send_id(object, "accessibilityChildren")) {
                const auto count = s.send(set, "count").x0;
                for (Address i = 0; i < count; ++i) out.push_back(s.send_id(set, "objectAtIndex:", {i}));
                return out;
            }
        }
        if (is(object, "NSApplication")) return windows();
        if (is(object, "NSWindow")) { if (const auto content = s.send_id(object, "contentView")) element_children(content, out); return out; }
        if (is(object, "NSView")) element_children(object, out);
        return out;
    }
    std::vector<Address> windows() {
        std::vector<Address> out;
        const auto app = application();
        const auto list = app ? s.send_id(app, "windows") : 0;
        const auto count = list ? s.send(list, "count").x0 : 0;
        for (Address i = 0; i < count; ++i) {
            const auto window = s.send_id(list, "objectAtIndex:", {i});
            if (s.send(window, "isVisible").x0 & 0xff) out.push_back(window);
        }
        return out;
    }
    Address parent_of(Address object) {
        if (responds(object, "accessibilityParent")) if (const auto parent = s.send_id(object, "accessibilityParent")) return parent;
        if (is(object, "NSWindow")) return application();
        if (is(object, "NSView")) {
            for (auto view = s.send_id(object, "superview"); view; view = s.send_id(view, "superview"))
                if (element(view)) return view;
            return s.send_id(object, "window");
        }
        return 0;
    }
    // The object's frame on the screen (Cocoa coordinates), when it has one.
    std::optional<Rect> screen_frame(Address object) {
        if (is(object, "NSWindow")) return result_rect(s.send(object, "frame"));
        if (is(object, "NSView")) {
            const auto window = s.send_id(object, "window");
            if (!window) return std::nullopt;
            CallRegs bounds;
            const auto b = result_rect(s.send(object, "bounds"));
            bounds.set_d(0, b.origin.x); bounds.set_d(1, b.origin.y); bounds.set_d(2, b.size.width); bounds.set_d(3, b.size.height);
            bounds.x[2] = 0;
            const auto in_window = result_rect(s.send(object, "convertRect:toView:", bounds));
            CallRegs to_screen;
            to_screen.set_d(0, in_window.origin.x); to_screen.set_d(1, in_window.origin.y);
            to_screen.set_d(2, in_window.size.width); to_screen.set_d(3, in_window.size.height);
            return result_rect(s.send(window, "convertRectToScreen:", to_screen));
        }
        if (responds(object, "accessibilityFrame")) return result_rect(s.send(object, "accessibilityFrame"));
        return std::nullopt;
    }
    double screen_height() {
        const auto screen_class = cls("NSScreen");
        const auto screen = screen_class ? s.send_id(screen_class, "mainScreen") : 0;
        return screen ? result_rect(s.send(screen, "frame")).size.height : 0;
    }
    Address ax_value(std::uint64_t type, double a, double b) {   // +1 AXValue (CGPoint 1 / CGSize 2); 0 without AXValue (services.cpp)
        if (!s.process.resolve(application_services, "_AXValueCreate")) return 0;
        std::lock_guard lock(scratch_mutex);
        if (!scratch) scratch = s.process.allocate_guest(32, 8);
        s.memory().write64(scratch, std::bit_cast<std::uint64_t>(a));
        s.memory().write64(scratch + 8, std::bit_cast<std::uint64_t>(b));
        return s.call(application_services, "_AXValueCreate", {type, scratch});
    }
    Address focused_element(Address app) {
        const auto window = app ? s.send_id(app, "keyWindow") : 0;
        if (!window) return 0;
        auto responder = s.send_id(window, "firstResponder");
        // The field editor stands for the control it edits.
        if (responder && is(responder, "NSTextView") && responds(responder, "isFieldEditor") && (s.send(responder, "isFieldEditor").x0 & 0xff))
            if (const auto delegate = s.send_id(responder, "delegate")) responder = delegate;
        return responder && responder != window && is(responder, "NSView") ? responder : window;
    }

    // The value of `attribute` for `object` (+1 when found), or an AXError.
    std::pair<std::int32_t, Address> value_of(Address object, const std::string& attribute) {
        if (responds(object, "accessibilityAttributeValue:")) {
            const auto name = string(attribute);
            const auto value = s.send_id(object, "accessibilityAttributeValue:", {name});
            s.release(name);
            if (value) return {ax_success, converted(value)};
        }
        const auto with = [&](Address value) -> std::pair<std::int32_t, Address> { return value ? std::pair{ax_success, converted(value)} : std::pair{ax_no_value, Address{}}; };
        const auto property = [&](const char* getter) { return responds(object, getter) ? s.send_id(object, getter) : 0; };
        const auto owned = [&](Address value) -> std::pair<std::int32_t, Address> { return value ? std::pair{ax_success, value} : std::pair{ax_no_value, Address{}}; };
        if (attribute == "AXRole") { const auto role = role_of(object); return role.empty() ? std::pair{ax_no_value, Address{}} : owned(string(role)); }
        if (attribute == "AXSubrole") {
            if (const auto value = property("accessibilitySubrole")) return with(value);
            if (is(object, "NSSecureTextField")) return owned(string("AXSecureTextField"));
            if (is(object, "NSWindow")) return owned(string("AXStandardWindow"));
            return {ax_no_value, 0};
        }
        if (attribute == "AXRoleDescription") {
            if (const auto value = property("accessibilityRoleDescription")) return with(value);
            const auto role = role_of(object);
            if (role.empty() || !s.process.resolve(appkit, "_NSAccessibilityRoleDescription")) return {ax_no_value, 0};
            const auto name = string(role);
            const auto description = s.call(appkit, "_NSAccessibilityRoleDescription", {name, 0});
            s.release(name);
            return with(description);
        }
        if (attribute == "AXTitle") {
            if (const auto value = property("accessibilityTitle")) return with(value);
            if (is(object, "NSWindow") || is(object, "NSButton")) return with(s.send_id(object, "title"));
            if (is(object, "NSApplication")) return with(s.send_id(s.send_id(cls("NSProcessInfo"), "processInfo"), "processName"));
            return {ax_no_value, 0};
        }
        if (attribute == "AXValue") {
            if (const auto value = property("accessibilityValue")) return with(value);
            if (is(object, "NSTextField")) return with(s.send_id(object, "stringValue"));
            if (is(object, "NSSlider") || is(object, "NSLevelIndicator") || is(object, "NSProgressIndicator")) {
                CallRegs none;
                return owned(s.cf.real(s.send(object, "doubleValue", none).d(0)));
            }
            if (is(object, "NSButton")) return owned(s.cf.integer(static_cast<std::int64_t>(s.send(object, "state").x0)));
            return {ax_no_value, 0};
        }
        if (attribute == "AXDescription") return with(property("accessibilityLabel"));
        if (attribute == "AXHelp") return with(property("accessibilityHelp"));
        if (attribute == "AXIdentifier") return with(property("accessibilityIdentifier"));
        if (attribute == "AXParent") return with(parent_of(object));
        if (attribute == "AXChildren") {
            const auto children = children_of(object);
            std::vector<Address> items;
            for (const auto child : children) items.push_back(make(child));
            const auto array = s.cf.array(items);
            for (const auto item : items) s.release(item);
            return owned(array);
        }
        if (attribute == "AXWindow") return is(object, "NSView") ? with(s.send_id(object, "window")) : std::pair{ax_attribute_unsupported, Address{}};
        if (attribute == "AXEnabled") {
            if (responds(object, "isAccessibilityEnabled") && !is(object, "NSControl")) return owned(s.cf.boolean((s.send(object, "isAccessibilityEnabled").x0 & 0xff) != 0));
            if (is(object, "NSControl")) return owned(s.cf.boolean((s.send(object, "isEnabled").x0 & 0xff) != 0));
            return owned(s.cf.boolean(true));
        }
        if (attribute == "AXFocused") {
            if (responds(object, "isAccessibilityFocused") && !is(object, "NSView")) return owned(s.cf.boolean((s.send(object, "isAccessibilityFocused").x0 & 0xff) != 0));
            const auto focused = focused_element(application());
            return owned(s.cf.boolean(focused == object));
        }
        if (attribute == "AXPosition" || attribute == "AXSize") {
            const auto frame = screen_frame(object);
            if (!frame) return {ax_no_value, 0};
            if (attribute == "AXSize") return owned(ax_value(2, frame->size.width, frame->size.height));
            return owned(ax_value(1, frame->origin.x, screen_height() - frame->origin.y - frame->size.height));
        }
        if (is(object, "NSApplication")) {
            if (attribute == "AXWindows") {
                std::vector<Address> items;
                for (const auto window : windows()) items.push_back(make(window));
                const auto array = s.cf.array(items);
                for (const auto item : items) s.release(item);
                return owned(array);
            }
            if (attribute == "AXFocusedWindow") return with(s.send_id(object, "keyWindow"));
            if (attribute == "AXMainWindow") return with(s.send_id(object, "mainWindow"));
            if (attribute == "AXFocusedUIElement") return with(focused_element(object));
            if (attribute == "AXFrontmost") return owned(s.cf.boolean((s.send(object, "isActive").x0 & 0xff) != 0));
        }
        if (is(object, "NSWindow")) {
            if (attribute == "AXMain") return owned(s.cf.boolean((s.send(object, "isMainWindow").x0 & 0xff) != 0));
            if (attribute == "AXMinimized") return owned(s.cf.boolean((s.send(object, "isMiniaturized").x0 & 0xff) != 0));
        }
        return {ax_attribute_unsupported, 0};
    }
    std::vector<std::string> names_of(Address object) {
        std::vector<std::string> out;
        if (responds(object, "accessibilityAttributeNames"))
            if (const auto names = s.send_id(object, "accessibilityAttributeNames")) {
                const auto count = s.send(names, "count").x0;
                for (Address i = 0; i < count; ++i) out.push_back(text(s.send_id(names, "objectAtIndex:", {i})));
            }
        std::vector<std::string> candidates{"AXRole", "AXSubrole", "AXRoleDescription", "AXTitle", "AXValue", "AXDescription", "AXHelp",
                                            "AXIdentifier", "AXParent", "AXChildren", "AXEnabled", "AXFocused", "AXPosition", "AXSize"};
        if (is(object, "NSView")) candidates.push_back("AXWindow");
        if (is(object, "NSApplication")) for (const auto* name : {"AXWindows", "AXFocusedWindow", "AXMainWindow", "AXFocusedUIElement", "AXFrontmost"}) candidates.push_back(name);
        if (is(object, "NSWindow")) for (const auto* name : {"AXMain", "AXMinimized"}) candidates.push_back(name);
        for (const auto& name : candidates) {
            if (std::find(out.begin(), out.end(), name) != out.end()) continue;
            const auto [status, value] = value_of(object, name);
            if (value) s.release(value);
            if (status == ax_success || (status == ax_no_value && name != "AXSubrole" && name != "AXHelp" && name != "AXIdentifier")) out.push_back(name);
        }
        return out;
    }
    std::vector<std::string> actions_of(Address object) {
        std::vector<std::string> out;
        if (responds(object, "accessibilityActionNames"))
            if (const auto names = s.send_id(object, "accessibilityActionNames")) {
                const auto count = s.send(names, "count").x0;
                for (Address i = 0; i < count; ++i) out.push_back(text(s.send_id(names, "objectAtIndex:", {i})));
            }
        const auto add = [&](const char* name) { if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name); };
        if (responds(object, "accessibilityPerformPress") || is(object, "NSButton")) add("AXPress");
        if (is(object, "NSWindow")) add("AXRaise");
        return out;
    }
    Address strings(const std::vector<std::string>& names) {   // +1 CFArray of CFStrings
        std::vector<Address> items;
        for (const auto& name : names) items.push_back(string(name));
        const auto array = s.cf.array(items);
        for (const auto item : items) s.release(item);
        return array;
    }

    // ---- notifications ----
    void post(Address object, Address notification, Address info) {
        if (!object || !notification) return;
        struct Delivery { Address observer, callback, element, notification, refcon, info; bool with_info; };
        std::vector<Delivery> deliveries;
        {
            std::lock_guard lock(mutex);
            if (observers.empty()) return;   // the usual case: nobody observes
        }
        const auto app = application();
        {
            std::lock_guard lock(mutex);
            for (const auto observer : observers) {
                const auto state = s.objc.find_host_state<Observer>(observer);
                if (!state) continue;
                for (const auto& r : state->registrations) {
                    const auto target = element_of(r.element);
                    if (!target || !s.cf.equal(r.notification, notification)) continue;
                    if (target->object != object && !(target->object == app && app)) continue;
                    deliveries.push_back({s.retain(observer), state->callback, make(object), s.retain(notification), r.refcon, s.retain(info), state->with_info});
                }
            }
        }
        for (const auto& d : deliveries) {
            const auto deliver = [this, d](TimerId) {
                const std::vector<Address> arguments = d.with_info ? std::vector<Address>{d.observer, d.element, d.notification, d.info, d.refcon}
                                                                   : std::vector<Address>{d.observer, d.element, d.notification, d.refcon};
                try {
                    if (d.callback) static_cast<void>(EngineAdapter(s.process.calling_engine()).call(d.callback, std::span(arguments.data(), arguments.size())));
                } catch (...) {
                    for (const auto object : {d.observer, d.element, d.notification, d.info}) s.release(object);
                    throw;
                }
                for (const auto object : {d.observer, d.element, d.notification, d.info}) s.release(object);
            };
            if (main_run_loop_available()) static_cast<void>(main_run_loop().add_timer(monotonic_seconds(), 0, common_modes, deliver));
            else deliver(0);
        }
    }

    void install();
};

std::shared_ptr<Client>& client_instance() {
    static std::shared_ptr<Client> instance;
    return instance;
}

void Client::install() {
    // ---- elements ----
    // AXUIElementRef: a CF type of HIServices' range (0x700: AXValue and the
    // text markers are services.cpp's).
    element_class = s.declare_class("__AXUIElement", "NSObject", application_services);
    // CFGetTypeID's registry is the Security provider's bridge, installed
    // before the support kit in every process; partial test installs without
    // it keep the generic type id.
    std::shared_ptr<security::Bridge> types;
    try { types = security::bridge(); } catch (const std::exception&) {}
    if (types) types->register_type_id(element_class, static_cast<security::TypeId>(0x701));
    s.function(application_services, "_AXUIElementGetTypeID", [](CpuState& c, GuestMemory&) { c.x[0] = 0x701; });
    s.function(application_services, "_AXUIElementCreateApplication", [this](CpuState& c, GuestMemory&) {
        const auto pid = static_cast<std::int32_t>(c.x[0]);
        if (pid == own_pid()) { c.x[0] = make(application()); return; }
        const auto made = s.objc.allocate(element_class);
        s.objc.host_state<Element>(made)->pid = pid;   // another process: every message fails
        c.x[0] = made;
    });
    s.function(application_services, "_AXUIElementCreateSystemWide", [this](CpuState& c, GuestMemory&) { c.x[0] = make(0, true); });
    s.function(application_services, "_AXUIElementGetPid", [this](CpuState& c, GuestMemory& m) {
        const auto state = element_of(c.x[0]);
        if (!state || !c.x[1]) { c.x[0] = ret(ax_illegal_argument); return; }
        m.write32(c.x[1], static_cast<std::uint32_t>(state->pid));
        c.x[0] = ret(ax_success);
    });
    s.function(application_services, "_AXUIElementSetMessagingTimeout", [this](CpuState& c, GuestMemory&) {
        c.x[0] = ret(element_of(c.x[0]) || !c.x[0] ? ax_success : ax_illegal_argument);   // this process's answers are immediate
    });
    s.method(element_class, "dealloc", [this](CpuState& c, GuestMemory&) {
        if (const auto state = element_of(c.x[0])) s.release(std::exchange(state->object, 0));
        static_cast<void>(s.objc.send_super(c.x[0], element_class, s.objc.selector("dealloc")));
    }, "v@:");
    // Two elements of one object are equal (CFEqual), as AXUIElements are.
    s.method(element_class, "isEqual:", [this](CpuState& c, GuestMemory&) {
        const auto a = element_of(c.x[0]), b = element_of(c.x[2]);
        c.x[0] = c.x[0] == c.x[2] || (a && b && a->pid == b->pid && a->system_wide == b->system_wide && a->object == b->object);
    }, "B@:@");
    s.method(element_class, "hash", [this](CpuState& c, GuestMemory&) {
        const auto a = element_of(c.x[0]);
        c.x[0] = a ? (a->object ^ static_cast<Address>(a->pid)) : c.x[0];
    }, "Q@:");
    // The object of a usable element, or the AXError to answer.
    const auto target = [this](Address element_object, Address& object) -> std::int32_t {
        const auto state = element_of(element_object);
        if (!state) return ax_illegal_argument;
        if (state->pid != own_pid()) return ax_cannot_complete;   // no accessibility server for other processes
        object = state->object;
        return ax_success;
    };
    s.function(application_services, "_AXUIElementCopyAttributeValue", [this, target](CpuState& c, GuestMemory& m) {
        if (c.x[2]) m.write64(c.x[2], 0);
        if (!c.x[1] || !c.x[2]) { c.x[0] = ret(ax_illegal_argument); return; }
        Address object{};
        if (const auto code = target(c.x[0], object); code != ax_success) { c.x[0] = ret(code); return; }
        const auto attribute = text(c.x[1]);
        if (element_of(c.x[0])->system_wide) {
            const auto app = application();
            Address value{};
            if (attribute == "AXFocusedApplication") value = app && (s.send(app, "isActive").x0 & 0xff) ? make(app) : 0;
            else if (attribute == "AXFocusedUIElement") { if (const auto focused = focused_element(app)) value = make(focused); }
            else { c.x[0] = ret(ax_attribute_unsupported); return; }
            m.write64(c.x[2], value);
            c.x[0] = ret(value ? ax_success : ax_no_value);
            return;
        }
        if (!object) { c.x[0] = ret(ax_invalid_element); return; }
        const auto [code, value] = value_of(object, attribute);
        m.write64(c.x[2], value);
        c.x[0] = ret(code);
    });
    for (const auto& [symbol, kind] : std::initializer_list<std::pair<const char*, int>>{
             {"_AXUIElementCopyAttributeNames", 0}, {"_AXUIElementCopyActionNames", 1}, {"_AXUIElementCopyParameterizedAttributeNames", 2}}) {
        s.function(application_services, symbol, [this, target, kind = kind](CpuState& c, GuestMemory& m) {
            if (c.x[1]) m.write64(c.x[1], 0);
            if (!c.x[1]) { c.x[0] = ret(ax_illegal_argument); return; }
            Address object{};
            if (const auto code = target(c.x[0], object); code != ax_success) { c.x[0] = ret(code); return; }
            std::vector<std::string> names;
            if (element_of(c.x[0])->system_wide) { if (kind == 0) names = {"AXFocusedApplication", "AXFocusedUIElement"}; }
            else if (!object) { c.x[0] = ret(ax_invalid_element); return; }
            else if (kind == 0) names = names_of(object);
            else if (kind == 1) names = actions_of(object);
            else if (responds(object, "accessibilityParameterizedAttributeNames"))
                if (const auto list = s.send_id(object, "accessibilityParameterizedAttributeNames"))
                    for (Address i = 0, n = s.send(list, "count").x0; i < n; ++i) names.push_back(text(s.send_id(list, "objectAtIndex:", {i})));
            m.write64(c.x[1], strings(names));
            c.x[0] = ret(ax_success);
        });
    }
    s.function(application_services, "_AXUIElementCopyParameterizedAttributeValue", [this, target](CpuState& c, GuestMemory& m) {
        if (c.x[3]) m.write64(c.x[3], 0);
        if (!c.x[1] || !c.x[3]) { c.x[0] = ret(ax_illegal_argument); return; }
        Address object{};
        if (const auto code = target(c.x[0], object); code != ax_success) { c.x[0] = ret(code); return; }
        if (!object || !responds(object, "accessibilityAttributeValue:forParameter:")) { c.x[0] = ret(ax_parameterized_unsupported); return; }
        const auto value = s.send_id(object, "accessibilityAttributeValue:forParameter:", {c.x[1], c.x[2]});
        m.write64(c.x[3], converted(value));
        c.x[0] = ret(value ? ax_success : ax_no_value);
    });
    s.function(application_services, "_AXUIElementPerformAction", [this, target](CpuState& c, GuestMemory&) {
        Address object{};
        if (!c.x[1]) { c.x[0] = ret(ax_illegal_argument); return; }
        if (const auto code = target(c.x[0], object); code != ax_success) { c.x[0] = ret(code); return; }
        if (!object) { c.x[0] = ret(ax_action_unsupported); return; }
        const auto action = text(c.x[1]);
        const auto names = actions_of(object);
        if (std::find(names.begin(), names.end(), action) == names.end()) { c.x[0] = ret(ax_action_unsupported); return; }
        if (action == "AXPress" && responds(object, "accessibilityPerformPress")) static_cast<void>(s.send(object, "accessibilityPerformPress"));
        else if (action == "AXPress" && is(object, "NSButton")) static_cast<void>(s.send_id(object, "performClick:", {0}));
        else if (action == "AXRaise" && is(object, "NSWindow")) static_cast<void>(s.send_id(object, "makeKeyAndOrderFront:", {0}));
        else static_cast<void>(s.send_id(object, "accessibilityPerformAction:", {c.x[1]}));
        c.x[0] = ret(ax_success);
    });
    s.function(application_services, "_AXUIElementSetAttributeValue", [this, target](CpuState& c, GuestMemory&) {
        Address object{};
        if (!c.x[1] || !c.x[2]) { c.x[0] = ret(ax_illegal_argument); return; }
        if (const auto code = target(c.x[0], object); code != ax_success) { c.x[0] = ret(code); return; }
        if (!object) { c.x[0] = ret(ax_attribute_unsupported); return; }
        const auto attribute = text(c.x[1]);
        const auto value = c.x[2];
        if (responds(object, "accessibilityIsAttributeSettable:") && (s.send_id(object, "accessibilityIsAttributeSettable:", {c.x[1]}) & 0xff)) {
            static_cast<void>(s.send_id(object, "accessibilitySetValue:forAttribute:", {value, c.x[1]}));
            c.x[0] = ret(ax_success);
            return;
        }
        if (attribute == "AXValue" && is(object, "NSTextField") && s.cf.kind(value) == ObjectKind::string) {
            static_cast<void>(s.send_id(object, "setStringValue:", {value}));
        } else if (attribute == "AXValue" && is(object, "NSSlider") && s.cf.number_value(value)) {
            CallRegs r;
            r.set_d(0, *s.cf.number_value(value));
            static_cast<void>(s.send(object, "setDoubleValue:", r));
        } else if (attribute == "AXFocused" && is(object, "NSView")) {
            if (const auto window = s.send_id(object, "window")) static_cast<void>(s.send_id(window, "makeFirstResponder:", {object}));
        } else if ((attribute == "AXFocused" || attribute == "AXMain") && is(object, "NSWindow")) {
            static_cast<void>(s.send_id(object, "makeKeyAndOrderFront:", {0}));
        } else {
            c.x[0] = ret(ax_attribute_unsupported);
            return;
        }
        c.x[0] = ret(ax_success);
    });

    // ---- observers ----
    observer_class = s.declare_class("_AlchemyAXObserver", "NSObject");
    s.method(observer_class, "dealloc", [this](CpuState& c, GuestMemory&) {
        if (const auto state = s.objc.find_host_state<Observer>(c.x[0])) {
            {
                std::lock_guard lock(mutex);
                std::erase(observers, c.x[0]);
            }
            for (const auto& r : std::exchange(state->registrations, {})) { s.release(r.element); s.release(r.notification); }
            s.release(std::exchange(state->source, 0));
        }
        static_cast<void>(s.objc.send_super(c.x[0], observer_class, s.objc.selector("dealloc")));
    }, "v@:");
    for (const auto& [symbol, with_info] : std::initializer_list<std::pair<const char*, bool>>{{"_AXObserverCreate", false}, {"_AXObserverCreateWithInfoCallback", true}}) {
        s.function(application_services, symbol, [this, with_info = with_info](CpuState& c, GuestMemory& m) {
            if (c.x[2]) m.write64(c.x[2], 0);
            if (!c.x[1] || !c.x[2]) { c.x[0] = ret(ax_illegal_argument); return; }
            if (static_cast<std::int32_t>(c.x[0]) != own_pid()) { c.x[0] = ret(ax_cannot_complete); return; }
            const auto made = s.objc.allocate(observer_class);
            const auto state = s.objc.host_state<Observer>(made);
            state->pid = own_pid();
            state->callback = c.x[1];
            state->with_info = with_info;
            {
                std::lock_guard lock(mutex);
                observers.push_back(made);
            }
            m.write64(c.x[2], made);
            c.x[0] = ret(ax_success);
        });
    }
    const auto observer_of = [this](Address object) { return object && s.objc.object_class(object) == observer_class ? s.objc.find_host_state<Observer>(object) : nullptr; };
    s.function(application_services, "_AXObserverAddNotification", [this, observer_of](CpuState& c, GuestMemory&) {
        const auto observer = observer_of(c.x[0]);
        if (!observer) { c.x[0] = ret(ax_invalid_observer); return; }
        const auto element = element_of(c.x[1]);
        if (!element || !c.x[2]) { c.x[0] = ret(ax_illegal_argument); return; }
        if (element->pid != observer->pid) { c.x[0] = ret(ax_cannot_complete); return; }
        std::lock_guard lock(mutex);
        for (const auto& r : observer->registrations)
            if (s.objc.find_host_state<Element>(r.element)->object == element->object && s.cf.equal(r.notification, c.x[2])) {
                c.x[0] = ret(ax_notification_already_registered);
                return;
            }
        observer->registrations.push_back({s.retain(c.x[1]), s.cf.copy(c.x[2]), c.x[3]});
        c.x[0] = ret(ax_success);
    });
    s.function(application_services, "_AXObserverRemoveNotification", [this, observer_of](CpuState& c, GuestMemory&) {
        const auto observer = observer_of(c.x[0]);
        if (!observer) { c.x[0] = ret(ax_invalid_observer); return; }
        const auto element = element_of(c.x[1]);
        if (!element || !c.x[2]) { c.x[0] = ret(ax_illegal_argument); return; }
        Registration removed{};
        {
            std::lock_guard lock(mutex);
            auto& list = observer->registrations;
            for (auto it = list.begin(); it != list.end(); ++it)
                if (s.objc.find_host_state<Element>(it->element)->object == element->object && s.cf.equal(it->notification, c.x[2])) {
                    removed = *it;
                    list.erase(it);
                    break;
                }
        }
        if (!removed.element) { c.x[0] = ret(ax_notification_not_registered); return; }
        s.release(removed.element);
        s.release(removed.notification);
        c.x[0] = ret(ax_success);
    });
    // A run loop source the application may add (delivery uses the main run loop either way).
    s.function(application_services, "_AXObserverGetRunLoopSource", [this, observer_of](CpuState& c, GuestMemory&) {
        const auto observer = observer_of(c.x[0]);
        if (!observer) { c.x[0] = 0; return; }
        if (!observer->source) {
            const auto context = s.process.allocate_guest(80, 8);   // CFRunLoopSourceContext, version 0, no callbacks
            const std::vector<std::uint8_t> zero(80);
            s.memory().write_bytes(context, zero);
            observer->source = s.call(core_foundation, "_CFRunLoopSourceCreate", {0, 0, context});
        }
        c.x[0] = observer->source;
    });
}

} // namespace

void install_applicationservices_ax_client(Support& support) {
    auto& instance = client_instance();
    instance = std::make_shared<Client>(support);
    instance->install();
}

void ax_client_post(Address object, Address notification, Address info) {
    if (const auto& instance = client_instance()) instance->post(object, notification, info);
}

} // namespace alchemy::gui::support_kit
