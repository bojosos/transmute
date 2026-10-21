#include "alchemy/darwin/appkit/controls/g3.hpp"

// NSAccessibilityElement (subclassed by QLab for its cue-list rows) and the
// NSAccessibility properties of NSView and of accessibility elements:
// label, title, value, identifier, role, role description, subrole, help,
// description, parent, frame, frame in parent space, index, enabled,
// focused, children, and whether the object is an accessibility element.
// The values are stored on the object (associated objects, released with
// it); there is no screen-reader bridge on the host, so nothing reads them
// but the application itself. Table views answer their accessibility
// getters from the table (containers.cpp).
namespace alchemy::gui::completion_kit::controls {
namespace {
using namespace appkit;

constexpr std::string_view objc_runtime = "/usr/lib/libobjc.A.dylib";
// objc_AssociationPolicy.
constexpr Address assign_policy = 0, retain_policy = 1, copy_policy = 3;

// An accessibility frame (a rect value with no Foundation dependency).
struct FrameState final : ObjectPayload { Rect frame; };
Address frame_class{};

struct Property {
    enum class Kind { object_copy, object_retain, object_weak, rect, integer, boolean } kind;
    Address key{};   // the association key (a guest address of its own)
};

Address associated(AppKit& kit, Address object, Address key) {
    return kit.guest_call(objc_runtime, "_objc_getAssociatedObject", {object, key});
}
void associate(AppKit& kit, Address object, Address key, Address value, Address policy) {
    static_cast<void>(kit.guest_call(objc_runtime, "_objc_setAssociatedObject", {object, key, value, policy}));
}

void install_property(AppKit& kit, Address cls, const std::string& name, Property::Kind kind, bool element_default = false) {
    auto* const k = &kit;
    const auto key = kit.process.allocate_guest(8);   // its address is the key
    std::string setter = "setAccessibility" + name + ":";
    std::string getter = "accessibility" + name;
    if (kind == Property::Kind::boolean) getter = "isAccessibility" + name;
    switch (kind) {
    case Property::Kind::object_copy: case Property::Kind::object_retain: case Property::Kind::object_weak: {
        const auto policy = kind == Property::Kind::object_copy ? copy_policy : kind == Property::Kind::object_retain ? retain_policy : assign_policy;
        kit.method(cls, getter, [k, key](CpuState& s, GuestMemory&) { s.x[0] = associated(*k, s.x[0], key); });
        kit.method(cls, setter, [k, key, policy](CpuState& s, GuestMemory&) { associate(*k, s.x[0], key, s.x[2], policy); }, "v@:@");
        break;
    }
    case Property::Kind::rect:
        kit.method(cls, getter, [k, key](CpuState& s, GuestMemory&) {
            const auto holder = associated(*k, s.x[0], key);
            const auto state = holder ? k->objc.find_host_state<FrameState>(holder) : nullptr;
            return_rect(s, state ? state->frame : Rect{});
        }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
        kit.method(cls, setter, [k, key](CpuState& s, GuestMemory&) {
            const auto holder = k->objc.allocate(frame_class);
            k->objc.host_state<FrameState>(holder)->frame = rect_arg(s);
            associate(*k, s.x[0], key, holder, retain_policy);
            k->release(holder);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
        break;
    case Property::Kind::integer:
        kit.method(cls, getter, [k, key](CpuState& s, GuestMemory&) {
            const auto number = associated(*k, s.x[0], key);
            s.x[0] = static_cast<Address>(number ? static_cast<std::int64_t>(k->cf.number_value(number).value_or(0)) : 0);
        }, "q@:");
        kit.method(cls, setter, [k, key](CpuState& s, GuestMemory&) {
            const auto number = k->cf.integer(static_cast<std::int64_t>(s.x[2]));
            associate(*k, s.x[0], key, number, retain_policy);
            k->release(number);
        }, "v@:q");
        break;
    case Property::Kind::boolean:
        kit.method(cls, getter, [k, key, element_default](CpuState& s, GuestMemory&) {
            const auto number = associated(*k, s.x[0], key);
            return_bool(s, number ? k->cf.number_value(number).value_or(0) != 0 : element_default);
        }, "B@:");
        kit.method(cls, setter, [k, key](CpuState& s, GuestMemory&) {
            const auto number = k->cf.integer(bool_arg(s, 2) ? 1 : 0);
            associate(*k, s.x[0], key, number, retain_policy);
            k->release(number);
        }, "v@:B");
        break;
    }
}

// Views keep the properties applications set on them; the ones AppKit
// computes for views (whether a view is an element, its children, frame,
// enabled and focused state) are left to the view classes. Elements store
// them all.
void install_properties(AppKit& kit, Address cls, bool element) {
    using K = Property::Kind;
    for (const auto* name : {"Label", "Title", "Identifier", "RoleDescription", "Description", "Role", "Subrole"})
        install_property(kit, cls, name, K::object_copy);
    install_property(kit, cls, "Value", K::object_retain);
    install_property(kit, cls, "Parent", K::object_weak);
    install_property(kit, cls, "FrameInParentSpace", K::rect);
    install_property(kit, cls, "Index", K::integer);
    if (!element) return;
    install_property(kit, cls, "Help", K::object_copy);
    install_property(kit, cls, "Children", K::object_copy);
    install_property(kit, cls, "Element", K::boolean, true);
    install_property(kit, cls, "Enabled", K::boolean, true);
    install_property(kit, cls, "Focused", K::boolean, false);
}

// AppKit's NSObject (NSAccessibility) informal protocol, the defaults that
// subclasses (Chrome's windows, views and application) call through super:
// no attribute names, no actions or parameterized
// attributes, nothing settable; an object is ignored unless it is an
// accessibility element; hit testing and focus answer the object itself; the
// array helpers read -accessibilityAttributeValue: (sent, so overrides win).
void install_informal_protocol(AppKit& kit) {
    auto* const k = &kit;
    const auto object = kit.objc.find_class("NSObject");
    const auto empty_array = [k](CpuState& s, GuestMemory&) { s.x[0] = k->send(k->objc.find_class("NSArray"), "array").x0; };
    kit.method(object, "accessibilityAttributeNames", empty_array);
    kit.method(object, "accessibilityActionNames", empty_array);
    kit.method(object, "accessibilityParameterizedAttributeNames", empty_array);
    kit.method(object, "accessibilityIsAttributeSettable:", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:@");
    kit.method(object, "accessibilitySetValue:forAttribute:", [](CpuState&, GuestMemory&) {}, "v@:@@");   // not settable: ignored
    kit.method(object, "accessibilityPerformAction:", [](CpuState&, GuestMemory&) {}, "v@:@");           // no actions
    kit.method(object, "accessibilityActionDescription:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    kit.method(object, "accessibilityAttributeValue:forParameter:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    kit.method(object, "accessibilityIsIgnored", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const bool element = k->objc.responds(self, k->sel("isAccessibilityElement")) &&
                             (k->send(self, "isAccessibilityElement").x0 & 0xff) != 0;
        return_bool(s, !element);
    }, "B@:");
    kit.method(object, "accessibilityHitTest:", [](CpuState&, GuestMemory&) {}, "@@:{CGPoint=dd}");   // returns self
    kit.method(object, "accessibilityFocusedUIElement", [](CpuState&, GuestMemory&) {});            // returns self
    kit.method(object, "accessibilityNotifiesWhenDestroyed", [](CpuState& s, GuestMemory&) { return_bool(s, false); }, "B@:");
    const auto array_value = [k](Address self, Address attribute) -> Address {
        const auto value = k->send_id(self, "accessibilityAttributeValue:", {attribute});
        // An array (CF arrays included): it answers -objectAtIndex:.
        return value && k->objc.responds(value, k->sel("objectAtIndex:")) ? value : 0;
    };
    kit.method(object, "accessibilityArrayAttributeCount:", [k, array_value](CpuState& s, GuestMemory&) {
        const auto values = array_value(s.x[0], s.x[2]);
        s.x[0] = values ? k->send(values, "count").x0 : 0;
    }, "Q@:@");
    kit.method(object, "accessibilityArrayAttributeValues:index:maxCount:", [k, array_value](CpuState& s, GuestMemory&) {
        const auto index = s.x[3], limit = s.x[4];   // read before any nested send
        const auto values = array_value(s.x[0], s.x[2]);
        const auto count = values ? k->send(values, "count").x0 : 0;
        if (!values || index >= count) { s.x[0] = k->send(k->objc.find_class("NSArray"), "array").x0; return; }
        CallRegs range;
        range.x[2] = index; range.x[3] = std::min<Address>(limit, count - index);
        s.x[0] = k->send(values, "subarrayWithRange:", range).x0;
    }, "@@:@QQ");
    kit.method(object, "accessibilityIndexOfChild:", [k, array_value](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], child = s.x[2];   // read before any nested send
        const auto attribute = k->cf.string("AXChildren");
        const auto children = array_value(self, attribute);
        k->release(attribute);
        s.x[0] = children ? k->send_id(children, "indexOfObject:", {child}) : 0x7fffffffffffffffULL;   // NSNotFound
    }, "Q@:@");
}

} // namespace

void install_accessibility(AppKit& kit) {
    auto* const k = &kit;
    frame_class = kit.objc.find_class("_AlchemyAccessibilityFrame");
    if (!frame_class) frame_class = kit.objc.register_native_class("_AlchemyAccessibilityFrame", kit.objc.find_class("NSObject"), 8);
    const auto element_class = kit.declare_class("NSAccessibilityElement", "NSObject");
    install_properties(kit, element_class, true);
    install_properties(kit, kit.view, false);
    kit.class_method(element_class, "accessibilityElementWithRole:frame:label:parent:", [k](CpuState& s, GuestMemory&) {
        // (role, frame d0..d3, label, parent)
        const auto made = k->alloc_init(s.x[0]);
        static_cast<void>(k->send_id(made, "setAccessibilityRole:", {s.x[2]}));
        CallRegs frame;
        put_rect(frame, 0, rect_arg(s));
        static_cast<void>(k->send(made, "setAccessibilityFrame:", frame));
        static_cast<void>(k->send_id(made, "setAccessibilityLabel:", {s.x[3]}));
        static_cast<void>(k->send_id(made, "setAccessibilityParent:", {s.x[4]}));
        s.x[0] = k->autorelease(made);
    }, "@@:@{CGRect={CGPoint=dd}{CGSize=dd}}@@");
    // An element's frame in parent space places it on screen through its parent (a view).
    kit.method(element_class, "accessibilityFrame", [k](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        CallRegs none;
        const auto stored = result_rect(k->send(self, "_alchemyStoredAccessibilityFrame", none));
        const auto parent = k->send(self, "accessibilityParent").x0;
        const auto local = result_rect(k->send(self, "accessibilityFrameInParentSpace", none));
        if ((stored.size.width > 0 || stored.size.height > 0) || !parent || !k->is_kind(parent, k->view)) { return_rect(s, stored); return; }
        const auto window_object = k->view_state(parent)->window;
        if (!window_object) { return_rect(s, stored); return; }
        const auto in_window = k->view_rect_in_window(parent, local);
        const auto origin = k->content_rect(window_object).origin;
        return_rect(s, {{in_window.origin.x + origin.x, in_window.origin.y + origin.y}, in_window.size});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    install_informal_protocol(kit);
    // The stored frame (the property's getter before the parent-space answer above replaced it).
    {
        const auto key = kit.process.allocate_guest(8);
        kit.method(element_class, "_alchemyStoredAccessibilityFrame", [k, key](CpuState& s, GuestMemory&) {
            const auto holder = associated(*k, s.x[0], key);
            const auto state = holder ? k->objc.find_host_state<FrameState>(holder) : nullptr;
            return_rect(s, state ? state->frame : Rect{});
        }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
        kit.method(element_class, "setAccessibilityFrame:", [k, key](CpuState& s, GuestMemory&) {
            const auto holder = k->objc.allocate(frame_class);
            k->objc.host_state<FrameState>(holder)->frame = rect_arg(s);
            associate(*k, s.x[0], key, holder, retain_policy);
            k->release(holder);
        }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    }
}

} // namespace alchemy::gui::completion_kit::controls
