// QLab gap group G4 over the AppKit provider and the fake window server
// (headless): Auto Layout end to end (anchors, the window pass, window
// resizing, broken required constraints, nib-archived constraints, fitting
// sizes, detached trees, -layout / -updateConstraints overrides, translating
// views inside constraint-laid-out ones). Native classes stand in for guest
// subclasses, so every call goes through ObjcRuntime::send.
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/nib_archive.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"
#include "alchemy/path.hpp"
#include "alchemy/darwin/appkit/tests/windows_appkit_support.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace windows_appkit_tests {
namespace gui = alchemy::gui;
namespace nib = alchemy::gui::nib;

std::vector<std::string> journal;

Fixture::Fixture() {
    alchemy::install_darwin_numeric(process);
    foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
    alchemy::install_darwin_frameworks(process, objc, cf);
    gui::appkit::set_window_server_override(&server);
    gui::InstallContext context{process, objc, cf};
    gui::install_appkit_gui(context);
    gui::install_quartzcore(context);
    gui::install_quartz2d(context);   // AppKit drawing, as in install_darwin_gui
    support = std::make_shared<gui::completion_kit::Support>(process, objc, cf);
    gui::completion_kit::install_group_windows(*support);
    app = id(cls("NSApplication"), "sharedApplication");
    static_cast<void>(id(app, "finishLaunching"));
}
Fixture::~Fixture() { gui::appkit::set_window_server_override(nullptr); }

Address Fixture::cls(std::string_view name) {
    const auto value = objc.find_class(name);
    require(value != 0, "missing class " + std::string(name));
    return value;
}
CallResult Fixture::send(Address receiver, std::string_view selector, CallRegs registers) {
    return objc.send(receiver, objc.selector(selector), std::move(registers));
}
Address Fixture::id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments) {
    CallRegs registers;
    std::size_t index = 2;
    for (const auto value : arguments) registers.x[index++] = value;
    return send(receiver, selector, registers).x0;
}
Address Fixture::subclass(std::string name, std::string_view super) { return objc.register_native_class(std::move(name), cls(super), 8); }
void Fixture::add(Address target, std::string name, transmute::GuestCallHandler handler, std::string types) {
    static_cast<void>(objc.add_native_method(target, std::move(name), std::move(handler), false, std::move(types)));
}
Address Fixture::with_rect(Address receiver, std::string_view selector, gui::Rect rect) {
    CallRegs registers;
    gui::appkit::put_rect(registers, 0, rect);
    return send(receiver, selector, registers).x0;
}
Address Fixture::view(gui::Rect frame, std::string_view cls_name) {
    return with_rect(id(cls(cls_name), "alloc"), "initWithFrame:", frame);
}
Address Fixture::make_window(gui::Rect content) {
    CallRegs init;
    gui::appkit::put_rect(init, 0, content);
    init.x[2] = gui::style::titled | gui::style::closable | gui::style::resizable;
    init.x[3] = 2;
    init.x[4] = 0;
    const auto made = send(id(cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    static_cast<void>(id(made, "setReleasedWhenClosed:", {0}));
    return made;
}
gui::Rect Fixture::frame(Address view_object) { return gui::appkit::result_rect(send(view_object, "frame")); }
Address Fixture::anchor_constraint(Address first, std::string_view anchor, std::string_view relation, Address second,
                                   std::string_view second_anchor, double constant) {
    CallRegs registers;
    registers.x[2] = id(second, second_anchor);
    registers.set_d(0, constant);
    const auto made = send(id(first, anchor), std::string(relation) + "Anchor:constant:", registers).x0;
    static_cast<void>(id(made, "setActive:", {1}));
    return made;
}
Address Fixture::size_constraint(Address first, std::string_view anchor, std::string_view relation, double constant) {
    CallRegs registers;
    registers.set_d(0, constant);
    const auto made = send(id(first, anchor), std::string(relation) + "Constant:", registers).x0;
    static_cast<void>(id(made, "setActive:", {1}));
    return made;
}
void Fixture::set_size(Address window, gui::Size size) {
    CallRegs registers;
    gui::appkit::put_size(registers, 0, size);
    static_cast<void>(send(window, "setContentSize:", registers));
}

void expect_frame(Fixture& f, Address view_object, gui::Rect wanted, const std::string& what) {
    const auto got = f.frame(view_object);
    const auto close = [](double a, double b) { return std::abs(a - b) < 0.01; };
    require(close(got.origin.x, wanted.origin.x) && close(got.origin.y, wanted.origin.y) && close(got.size.width, wanted.size.width) &&
                close(got.size.height, wanted.size.height),
            what + ": got {" + std::to_string(got.origin.x) + ", " + std::to_string(got.origin.y) + ", " + std::to_string(got.size.width) +
                ", " + std::to_string(got.size.height) + "}, wanted {" + std::to_string(wanted.origin.x) + ", " +
                std::to_string(wanted.origin.y) + ", " + std::to_string(wanted.size.width) + ", " + std::to_string(wanted.size.height) + "}");
}

namespace {
void anchors_and_window_pass(Fixture& f) {
    const auto window = f.make_window({{100, 100}, {400, 300}});
    const auto content = f.id(window, "contentView");
    const auto a = f.view({{0, 0}, {10, 10}}), b = f.view({{0, 0}, {10, 10}});
    for (const auto v : {a, b}) {
        static_cast<void>(f.id(v, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
        static_cast<void>(f.id(content, "addSubview:", {v}));
    }
    f.anchor_constraint(a, "leadingAnchor", "constraintEqualTo", content, "leadingAnchor", 20);
    f.anchor_constraint(a, "topAnchor", "constraintEqualTo", content, "topAnchor", 20);
    f.size_constraint(a, "widthAnchor", "constraintEqualTo", 100);
    f.size_constraint(a, "heightAnchor", "constraintEqualTo", 50);
    f.anchor_constraint(b, "leadingAnchor", "constraintEqualTo", a, "trailingAnchor", 8);
    f.anchor_constraint(b, "trailingAnchor", "constraintEqualTo", content, "trailingAnchor", -20);
    f.anchor_constraint(b, "topAnchor", "constraintEqualTo", a, "topAnchor", 0);
    f.anchor_constraint(b, "bottomAnchor", "constraintEqualTo", content, "bottomAnchor", -20);
    require(f.objc.find_host_state<gui::appkit::ViewState>(content)->constraints.size() == 6, "content holds the constraints between its subviews and itself");
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    expect_frame(f, a, {{20, 230}, {100, 50}}, "a (unflipped content: top-down 20 from the top)");
    expect_frame(f, b, {{128, 20}, {252, 260}}, "b fills the rest");
    // A window resize lays out again in the display cycle.
    f.set_size(window, {500, 400});
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.id(f.app, "nextEventMatchingMask:untilDate:inMode:dequeue:", {~0ULL, 0, 0, 1}));
    expect_frame(f, a, {{20, 330}, {100, 50}}, "a after the resize");
    expect_frame(f, b, {{128, 20}, {352, 360}}, "b after the resize");
    // A constant change updates the engine in place.
    const auto width = f.objc.find_host_state<gui::appkit::ViewState>(a)->constraints.at(0);
    CallRegs constant;
    constant.set_d(0, 150);
    static_cast<void>(f.send(width, "setConstant:", constant));
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    expect_frame(f, b, {{178, 20}, {302, 360}}, "b after a's width constant changed");
    // A conflicting required constraint is broken, not fatal.
    f.size_constraint(a, "widthAnchor", "constraintEqualTo", 120);
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    const auto w = f.frame(a).size.width;
    require(w == 150 || w == 120, "a conflicting required width breaks one constraint");
    // Removing a view drops the constraints that cross into it.
    static_cast<void>(f.id(b, "removeFromSuperview"));
    require(f.objc.find_host_state<gui::appkit::ViewState>(content)->constraints.size() == 2, "b's constraints left with it");
    static_cast<void>(f.id(window, "close"));
}

void window_grows_for_required_constraints(Fixture& f) {
    const auto window = f.make_window({{100, 100}, {300, 200}});
    const auto content = f.id(window, "contentView");
    const auto c = f.view({{0, 0}, {10, 10}});
    static_cast<void>(f.id(c, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
    static_cast<void>(f.id(content, "addSubview:", {c}));
    f.anchor_constraint(c, "leadingAnchor", "constraintEqualTo", content, "leadingAnchor", 20);
    f.anchor_constraint(c, "trailingAnchor", "constraintEqualTo", content, "trailingAnchor", -20);
    f.anchor_constraint(c, "topAnchor", "constraintEqualTo", content, "topAnchor", 10);
    f.anchor_constraint(c, "bottomAnchor", "constraintEqualTo", content, "bottomAnchor", -10);
    f.size_constraint(c, "widthAnchor", "constraintGreaterThanOrEqualTo", 600);
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    expect_frame(f, content, {{0, 0}, {640, 200}}, "the window grew to the required width");
    expect_frame(f, c, {{20, 10}, {600, 180}}, "the view at its minimum width");
    static_cast<void>(f.id(window, "close"));
}

void translating_views_follow_their_masks(Fixture& f) {
    const auto window = f.make_window({{100, 100}, {400, 300}});
    const auto content = f.id(window, "contentView");
    const auto p = f.view({{0, 0}, {100, 50}});
    static_cast<void>(f.id(p, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
    static_cast<void>(f.id(content, "addSubview:", {p}));
    const auto q = f.view({{10, 10}, {80, 30}});
    static_cast<void>(f.id(q, "setAutoresizingMask:", {2 | 32}));   // width sizable, max y margin
    static_cast<void>(f.id(p, "addSubview:", {q}));
    for (const auto& [anchor, constant] : std::initializer_list<std::pair<const char*, double>>{
             {"leadingAnchor", 20}, {"trailingAnchor", -20}, {"topAnchor", 20}, {"bottomAnchor", -20}})
        f.anchor_constraint(p, anchor, "constraintEqualTo", content, anchor, constant);
    // A view inside q laid out by constraints relative to q.
    const auto r = f.view({{0, 0}, {1, 1}});
    static_cast<void>(f.id(r, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
    static_cast<void>(f.id(q, "addSubview:", {r}));
    f.anchor_constraint(r, "trailingAnchor", "constraintEqualTo", q, "trailingAnchor", -5);
    f.anchor_constraint(r, "topAnchor", "constraintEqualTo", q, "topAnchor", 5);
    f.size_constraint(r, "widthAnchor", "constraintEqualTo", 30);
    f.size_constraint(r, "heightAnchor", "constraintEqualTo", 10);
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    expect_frame(f, p, {{20, 20}, {360, 260}}, "p pinned to the content");
    expect_frame(f, q, {{10, 10}, {340, 30}}, "q follows its width-sizable mask");
    expect_frame(f, r, {{305, 15}, {30, 10}}, "r pinned inside q");
    // q is an island (nothing outside refers into it): resizing it lays out
    // its contents again.
    CallRegs size;
    gui::appkit::put_size(size, 0, {200, 30});
    static_cast<void>(f.send(q, "setFrameSize:", size));
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    expect_frame(f, r, {{165, 15}, {30, 10}}, "r follows q's new width");
    static_cast<void>(f.id(window, "close"));
}

void overrides_fitting_and_detached(Fixture& f) {
    // -updateConstraints and -layout overrides run in the pass.
    const auto custom = f.subclass("G4TestLayoutView", "NSView");
    f.add(custom, "updateConstraints", [&f, custom](CpuState& s, GuestMemory&) {
        journal.push_back("updateConstraints");
        static_cast<void>(f.objc.send_super(s.x[0], custom, f.objc.selector("updateConstraints")));
    }, "v@:");
    f.add(custom, "layout", [&f, custom](CpuState& s, GuestMemory&) {
        journal.push_back("layout");
        static_cast<void>(f.objc.send_super(s.x[0], custom, f.objc.selector("layout")));
    }, "v@:");
    f.add(custom, "intrinsicContentSize", [](CpuState& s, GuestMemory&) {
        gui::appkit::return_size(s, {80, 20});
    }, "{CGSize=dd}@:");
    const auto container = f.view({{0, 0}, {300, 100}});
    const auto child = f.view({{0, 0}, {1, 1}}, "G4TestLayoutView");
    static_cast<void>(f.id(child, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
    static_cast<void>(f.id(container, "addSubview:", {child}));
    f.anchor_constraint(child, "leadingAnchor", "constraintEqualTo", container, "leadingAnchor", 10);
    f.anchor_constraint(child, "topAnchor", "constraintEqualTo", container, "topAnchor", 10);
    f.anchor_constraint(container, "trailingAnchor", "constraintGreaterThanOrEqualTo", child, "trailingAnchor", 10);
    f.anchor_constraint(container, "bottomAnchor", "constraintEqualTo", child, "bottomAnchor", 10);
    journal.clear();
    const auto fitting = gui::appkit::result_size(f.send(container, "fittingSize"));
    require(fitting.width == 100 && fitting.height == 40, "fitting size " + std::to_string(fitting.width) + " x " + std::to_string(fitting.height));
    static_cast<void>(f.id(container, "layoutSubtreeIfNeeded"));
    expect_frame(f, child, {{10, 10}, {80, 80}}, "detached tree laid out at its own size (bottom pinned)");
    require(std::find(journal.begin(), journal.end(), "layout") != journal.end(), "-layout override ran");
    require(std::count(journal.begin(), journal.end(), "updateConstraints") == 1, "-updateConstraints ran once (fitting size, then layout)");
}

// A window whose views and constraints come from a NIBArchive, as
// Interface Builder writes them.
void nib_constraints(Fixture& f) {
    NibBuilder b;
    const auto root = b.declare("NSObject");
    const auto data = b.declare("NSIBObjectData");
    const auto owner = b.declare("NSCustomObject");
    const auto window_template = b.declare("NSWindowTemplate");
    const auto content = b.declare("NSView");
    const auto left = b.declare("NSView");
    const auto right = b.declare("NSView");
    const auto c1 = b.declare("NSLayoutConstraint"), c2 = b.declare("NSLayoutConstraint"), c3 = b.declare("NSLayoutConstraint");
    const auto c4 = b.declare("NSLayoutConstraint"), c5 = b.declare("NSLayoutConstraint"), c6 = b.declare("NSLayoutConstraint");
    const auto c7 = b.declare("NSLayoutConstraint"), c8 = b.declare("NSLayoutConstraint");
    b.ref(root, "IB.objectdata", data);
    b.ref(owner, "NSClassName", b.string("NSObject"));
    b.integer(window_template, "NSWindowStyleMask", 15);
    b.ref(window_template, "NSWindowRect", b.string("{{100, 100}, {300, 120}}"));
    b.ref(window_template, "NSWindowView", content);
    b.integer(content, "NSvFlags", 256);
    b.ref(content, "NSFrameSize", b.string("{300, 120}"));
    b.ref(content, "NSSubviews", b.array("NSMutableArray", {left, right}));
    b.ref(content, "NSViewConstraints", b.array("NSArray", {c1, c2, c3, c4, c5, c6, c7, c8}));
    for (const auto v : {left, right}) {
        b.integer(v, "NSvFlags", 268);
        b.ref(v, "NSFrame", b.string("{{0, 0}, {5, 5}}"));
        b.ref(v, "NSSuperview", content);
        b.boolean(v, "NSDoNotTranslateAutoresizingMask", true);
    }
    b.ref(left, "NSHuggingPriority", b.string("{249, 750}"));
    const auto constraint = [&](std::uint32_t c, std::uint32_t first, int a1, std::optional<std::uint32_t> second, int a2, double constant,
                                bool space = false) {
        b.ref(c, "NSFirstItem", first);
        b.integer(c, "NSFirstAttribute", a1);
        b.integer(c, "NSFirstAttributeV2", a1);
        if (second) {
            b.ref(c, "NSSecondItem", *second);
            b.integer(c, "NSSecondAttribute", a2);
            b.integer(c, "NSSecondAttributeV2", a2);
        }
        nib::Value v; v.type = nib::ValueType::float64; v.real = constant;
        b.put(c, "NSConstant", v);
        b.put(c, "NSConstantV2", v);
        b.boolean(c, "NSShouldBeArchived", true);
        if (space) b.ref(c, "NSSymbolicConstant", b.string("NSSpace"));
    };
    constraint(c1, left, 5, content, 5, 0, true);           // left.leading = content.leading + standard (20)
    constraint(c2, right, 5, left, 6, 0, true);             // right.leading = left.trailing + standard (8)
    constraint(c3, content, 6, right, 6, 20);               // content.trailing = right.trailing + 20
    constraint(c4, left, 7, right, 7, 0);                   // equal widths
    constraint(c5, left, 3, content, 3, 10);                // tops
    constraint(c6, right, 3, left, 3, 0);
    constraint(c7, left, 8, std::nullopt, 0, 30);           // height 30
    constraint(c8, right, 8, left, 8, 0);                   // equal heights
    b.ref(data, "NSRoot", owner);
    b.ref(data, "NSVisibleWindows", b.array("NSMutableSet", {}));
    b.ref(data, "NSConnections", b.array("NSMutableArray", {}));
    b.ref(data, "NSObjectsKeys", b.array("NSArray", {window_template, content}));
    b.ref(data, "NSObjectsValues", b.array("NSArray", {owner, window_template}));
    const auto bytes = b.finish().encode();
    const auto nib_data = f.objc.send(f.cls("NSData"), f.objc.selector("dataWithBytes:length:"), [&] {
        CallRegs r;
        const auto block = f.process.allocate_guest(bytes.size());
        f.process.main_engine().write_bytes(block, bytes);
        r.x[2] = block; r.x[3] = bytes.size();
        return r;
    }()).x0;
    const auto loaded = f.id(f.id(f.cls("NSNib"), "alloc"), "initWithNibData:bundle:", {nib_data, 0});
    const auto out = f.process.allocate_guest(8);
    require(f.id(loaded, "instantiateWithOwner:topLevelObjects:", {0, out}) != 0, "nib instantiated");
    const auto objects = f.process.main_engine().read64(out);
    Address window{};
    for (const auto item : f.foundation_items(objects))
        if (f.objc.find_host_state<gui::appkit::WindowState>(item)) window = item;
    require(window != 0, "the nib's window");
    const auto content_view = f.id(window, "contentView");
    const auto views = f.foundation_items(f.id(content_view, "subviews"));
    require(views.size() == 2, "two subviews");
    require(f.id(views[0], "translatesAutoresizingMaskIntoConstraints") == 0, "NSDoNotTranslateAutoresizingMask");
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    expect_frame(f, views[0], {{20, 80}, {126, 30}}, "left (standard space to the superview)");
    expect_frame(f, views[1], {{154, 80}, {126, 30}}, "right (standard space between siblings)");
    static_cast<void>(f.id(window, "close"));
}
// Push buttons and labels are laid out by their alignment rectangles.
void control_metrics(Fixture& f) {
    const auto window = f.make_window({{100, 100}, {300, 200}});
    const auto content = f.id(window, "contentView");
    const auto button = f.view({{0, 0}, {10, 10}}, "NSButton");
    static_cast<void>(f.id(button, "setBezelStyle:", {1}));
    static_cast<void>(f.id(button, "setBordered:", {1}));
    static_cast<void>(f.id(button, "setTitle:", {f.str("OK")}));
    const auto label = f.view({{0, 0}, {10, 10}}, "NSTextField");
    static_cast<void>(f.id(label, "setBezeled:", {0}));
    static_cast<void>(f.id(label, "setBordered:", {0}));
    static_cast<void>(f.id(label, "setEditable:", {0}));
    static_cast<void>(f.id(label, "setStringValue:", {f.str("Name")}));
    for (const auto v : {button, label}) {
        static_cast<void>(f.id(v, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
        static_cast<void>(f.id(content, "addSubview:", {v}));
    }
    f.anchor_constraint(button, "trailingAnchor", "constraintEqualTo", content, "trailingAnchor", -20);
    f.anchor_constraint(button, "bottomAnchor", "constraintEqualTo", content, "bottomAnchor", -20);
    f.anchor_constraint(label, "leadingAnchor", "constraintEqualTo", content, "leadingAnchor", 20);
    f.anchor_constraint(label, "topAnchor", "constraintEqualTo", content, "topAnchor", 20);
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    const auto b = f.frame(button), l = f.frame(label);
    require(b.size.height == 32 && b.origin.y == 13 && b.origin.x + b.size.width == 287, "push button frame around its alignment rectangle");
    const auto intrinsic = gui::appkit::result_size(f.send(button, "intrinsicContentSize"));
    require(b.size.width == intrinsic.width + 14 && intrinsic.height == 20, "push button intrinsic size in alignment space");
    require(l.origin.x == 18 && l.size.width > 20, "label frame 2 points outside its alignment rectangle");
    static_cast<void>(f.id(window, "close"));
}
// NSStackView and NSGridView arrange their views inside a window's layout.
void stacks_and_grids(Fixture& f) {
    const auto window = f.make_window({{100, 100}, {400, 300}});
    const auto content = f.id(window, "contentView");
    const auto sized = f.subclass("G4TestSizedView", "NSView");
    f.add(sized, "intrinsicContentSize", [&f](CpuState& s, GuestMemory&) {
        const auto tag = static_cast<double>(f.id(s.x[0], "tag"));
        gui::appkit::return_size(s, {tag, 20});
    }, "{CGSize=dd}@:");
    const auto make = [&](std::int64_t width) {
        const auto v = f.view({{0, 0}, {1, 1}}, "G4TestSizedView");
        f.objc.find_host_state<gui::appkit::ViewState>(v)->tag = width;
        return v;
    };
    // A horizontal stack (fill, spacing 10) pinned to the top of the window.
    const auto a = make(50), b = make(70), c = make(30);
    const auto stack = f.id(f.cls("NSStackView"), "stackViewWithViews:", {f.array({a, b})});
    static_cast<void>(f.id(stack, "addView:inGravity:", {c, 1}));
    static_cast<void>(f.id(stack, "setDistribution:", {0}));
    CallRegs spacing;
    spacing.set_d(0, 10);
    static_cast<void>(f.send(stack, "setSpacing:", spacing));
    static_cast<void>(f.id(stack, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
    static_cast<void>(f.id(content, "addSubview:", {stack}));
    f.anchor_constraint(stack, "leadingAnchor", "constraintEqualTo", content, "leadingAnchor", 20);
    f.anchor_constraint(stack, "trailingAnchor", "constraintEqualTo", content, "trailingAnchor", -20);
    f.anchor_constraint(stack, "topAnchor", "constraintEqualTo", content, "topAnchor", 20);
    require(f.foundation_items(f.id(stack, "views")).size() == 3, "-views lists every gravity");
    // A 2 x 2 grid below it, sized by its content.
    const auto d = make(40), e = make(90), g = make(60), h = make(20);
    const auto rows = f.array({f.array({d, e}), f.array({g, h})});
    const auto grid = f.id(f.cls("NSGridView"), "gridViewWithViews:", {rows});
    CallRegs gap;
    gap.set_d(0, 8);
    static_cast<void>(f.send(grid, "setColumnSpacing:", gap));
    static_cast<void>(f.send(grid, "setRowSpacing:", gap));
    static_cast<void>(f.id(grid, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
    static_cast<void>(f.id(content, "addSubview:", {grid}));
    f.anchor_constraint(grid, "leadingAnchor", "constraintEqualTo", content, "leadingAnchor", 20);
    f.anchor_constraint(grid, "topAnchor", "constraintEqualTo", stack, "bottomAnchor", 12);
    require(f.id(grid, "numberOfRows") == 2 && f.id(grid, "numberOfColumns") == 2, "grid size");
    require(f.id(f.id(grid, "cellAtColumnIndex:rowIndex:", {1, 0}), "contentView") == e, "cellAtColumnIndex:rowIndex:");
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    expect_frame(f, stack, {{20, 260}, {360, 20}}, "stack view pinned and hugging its height");
    // Fill: the extra width goes to the arranged views at equal hugging (250);
    // the solver gives it to one of them, the others keep their sizes.
    const auto fa = f.frame(a), fb = f.frame(b), fc = f.frame(c);
    require(fa.origin.x == 0 && std::abs(fa.size.width + fb.size.width + fc.size.width + 20 - 360) < 0.01, "fill spans the stack");
    require(fb.origin.x == fa.size.width + 10 && fc.origin.x == fb.origin.x + fb.size.width + 10, "stack spacing");
    expect_frame(f, d, {{0, 28}, {40, 20}}, "grid cell 0,0 (leading, top of an unflipped grid)");
    expect_frame(f, e, {{68, 28}, {90, 20}}, "grid cell 0,1 starts after the widest first-column view");
    expect_frame(f, h, {{68, 0}, {20, 20}}, "grid cell 1,1");
    expect_frame(f, grid, {{20, 200}, {158, 48}}, "grid sized by its content");
    // Hidden rows take no space.
    static_cast<void>(f.id(f.id(grid, "rowAtIndex:", {1}), "setHidden:", {1}));
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    require(f.frame(grid).size.height == 20, "a hidden row collapses");
    static_cast<void>(f.id(window, "close"));
}
// Document-modal sheets: attached at the top of the parent, the parent takes
// no input, -endSheet:returnCode: closes the sheet and calls the handler.
void sheets(Fixture& f) {
    const auto parent = f.make_window({{100, 100}, {400, 300}});
    const auto sheet = f.make_window({{0, 0}, {200, 100}});
    static_cast<void>(f.id(parent, "makeKeyAndOrderFront:", {0}));
    std::int64_t answered = 0;
    const auto handler = f.block([&answered](CpuState& s, GuestMemory&) { answered = static_cast<std::int64_t>(s.x[1]); });
    static_cast<void>(f.id(parent, "beginSheet:completionHandler:", {sheet, handler}));
    require(f.id(parent, "attachedSheet") == sheet && f.id(sheet, "sheetParent") == parent && f.id(sheet, "isSheet") == 1, "sheet attached");
    const auto parent_content = gui::appkit::result_rect(f.send(parent, "frame"));
    const auto frame = gui::appkit::result_rect(f.send(sheet, "frame"));
    require(std::abs(frame.origin.x + frame.size.width / 2 - (parent_content.origin.x + parent_content.size.width / 2)) < 1,
            "the sheet is centered on its parent");
    const auto kit = gui::appkit::instance();
    require(!kit->input_allowed_for(parent) && kit->input_allowed_for(sheet), "the parent takes no input while the sheet is up");
    CallRegs end;
    end.x[2] = sheet;
    end.x[3] = 1000;   // NSModalResponseOK
    static_cast<void>(f.send(parent, "endSheet:returnCode:", end));
    require(answered == 1000, "the completion handler got the return code");
    require(f.id(parent, "attachedSheet") == 0 && f.id(sheet, "isVisible") == 0, "the sheet is gone");
    require(kit->input_allowed_for(parent), "the parent takes input again");
    static_cast<void>(f.id(sheet, "close"));
    static_cast<void>(f.id(parent, "close"));
}

// Nib connectors: accessibility, tool tips (help connectors), auxiliary
// actions, the base connector, Cocoa bindings (sent to -bind:...), and the
// window template keys.
void nib_connectors(Fixture& f) {
    const auto target_class = f.subclass("G4TestConnectorTarget", "NSView");
    Address double_action{}, bound_to{}, bound_object{};
    std::string bound_name, bound_path;
    f.add(target_class, "setDoubleAction:", [&](CpuState& s, GuestMemory&) { double_action = s.x[2]; }, "v@::");
    f.add(target_class, "setTarget:", [](CpuState&, GuestMemory&) {}, "v@:@");
    f.add(target_class, "bind:toObject:withKeyPath:options:", [&](CpuState& s, GuestMemory&) {
        bound_name = f.text(s.x[2]);
        bound_object = s.x[3];
        bound_path = f.text(s.x[4]);
        bound_to = s.x[0];
    }, "v@:@@@@");
    NibBuilder b;
    const auto root = b.declare("NSObject");
    const auto data = b.declare("NSIBObjectData");
    const auto owner = b.declare("NSCustomObject");
    const auto window_template = b.declare("NSWindowTemplate");
    const auto content = b.declare("NSView");
    const auto target = b.declare("NSCustomView");
    const auto ax = b.declare("NSNibAXAttributeConnector");
    const auto help = b.declare("NSIBHelpConnector");
    const auto aux = b.declare("NSNibAuxiliaryActionConnector");
    const auto base = b.declare("NSNibConnector");
    const auto binding = b.declare("NSNibBindingConnector");
    b.ref(root, "IB.objectdata", data);
    b.ref(owner, "NSClassName", b.string("NSObject"));
    b.integer(window_template, "NSWindowStyleMask", 15);
    b.ref(window_template, "NSWindowRect", b.string("{{100, 100}, {300, 120}}"));
    b.ref(window_template, "NSWindowView", content);
    b.integer(window_template, "NSWTFlags", 0x60780000);
    b.ref(window_template, "NSWindowSubtitle", b.string("Sub"));
    b.ref(window_template, "NSUserInterfaceItemIdentifier", b.string("main"));
    b.integer(content, "NSvFlags", 256);
    b.ref(content, "NSFrameSize", b.string("{300, 120}"));
    b.ref(content, "NSSubviews", b.array("NSMutableArray", {target}));
    b.ref(target, "NSClassName", b.string("G4TestConnectorTarget"));
    b.ref(target, "NSFrame", b.string("{{10, 10}, {50, 20}}"));
    b.ref(target, "NSSuperview", content);
    b.ref(ax, "AXDestinationArchiveKey", target);
    b.ref(ax, "AXAttributeTypeArchiveKey", b.string("AXDescription"));
    b.ref(ax, "AXAttributeValueArchiveKey", b.string("cue number"));
    b.ref(help, "NSDestination", target);
    b.ref(help, "NSFile", b.string("NSToolTipHelpKey"));
    b.ref(help, "NSMarker", b.string("A tip"));
    b.ref(aux, "NSSource", target);
    b.ref(aux, "NSDestination", owner);
    b.ref(aux, "NSLabel", b.string("open:"));
    b.ref(aux, "NSTrigger", b.string("doubleAction"));
    b.ref(base, "NSSource", target);
    b.ref(base, "NSLabel", b.string("ordering"));
    b.ref(binding, "NSSource", target);
    b.ref(binding, "NSDestination", owner);
    b.ref(binding, "NSLabel", b.string("value: self.level"));
    b.ref(binding, "NSBinding", b.string("value"));
    b.ref(binding, "NSKeyPath", b.string("self.level"));
    b.integer(binding, "NSNibBindingConnectorVersion", 2);
    b.ref(data, "NSRoot", owner);
    b.ref(data, "NSVisibleWindows", b.array("NSMutableSet", {}));
    b.ref(data, "NSConnections", b.array("NSMutableArray", {help, aux, base, binding}));
    b.ref(data, "NSAccessibilityConnectors", b.array("NSMutableArray", {ax}));
    b.ref(data, "NSObjectsKeys", b.array("NSArray", {window_template, content}));
    b.ref(data, "NSObjectsValues", b.array("NSArray", {owner, window_template}));
    const auto bytes = b.finish().encode();
    const auto block = f.process.allocate_guest(bytes.size());
    f.process.main_engine().write_bytes(block, bytes);
    CallRegs make;
    make.x[2] = block;
    make.x[3] = bytes.size();
    const auto nib_data = f.send(f.cls("NSData"), "dataWithBytes:length:", make).x0;
    const auto owner_object = f.id(f.cls("NSObject"), "new");
    const auto loaded = f.id(f.id(f.cls("NSNib"), "alloc"), "initWithNibData:bundle:", {nib_data, 0});
    const auto out = f.process.allocate_guest(8);
    require(f.id(loaded, "instantiateWithOwner:topLevelObjects:", {owner_object, out}) != 0, "nib instantiated");
    Address window{};
    for (const auto item : f.foundation_items(f.process.main_engine().read64(out)))
        if (f.objc.find_host_state<gui::appkit::WindowState>(item)) window = item;
    require(window != 0, "the nib's window");
    const auto view = f.foundation_items(f.id(f.id(window, "contentView"), "subviews")).at(0);
    require(f.text(f.id(view, "accessibilityAttributeValue:", {f.str("AXDescription")})) == "cue number", "accessibility override");
    require(f.text(f.id(view, "toolTip")) == "A tip", "tool tip from the help connector");
    require(double_action == f.objc.selector("open:"), "auxiliary double action");
    require(bound_to == view && bound_name == "value" && bound_object == owner_object && bound_path == "self.level", "binding");
    require(f.text(f.id(window, "subtitle")) == "Sub" && f.text(f.id(window, "identifier")) == "main", "window template subtitle and identifier");
    require(f.id(window, "isReleasedWhenClosed") == 0, "NSWTFlags: not released when closed");
    static_cast<void>(f.id(window, "close"));
}
// Open / save panels over a test presenter, document saving through the
// save panel, NSAlert's suppression checkbox and +alertWithError:.
void panels_documents_alerts(Fixture& f) {
    const auto host_target = (std::filesystem::temp_directory_path() / "alchemy_g4_document.txt");
    // The guest sees host drives under /Volumes/<letter>.
    const auto target = alchemy::guest_path_string(host_target);
    f.process.map_guest_path = [](std::string_view guest) -> std::filesystem::path {
        if (guest.starts_with("/Volumes/") && guest.size() > 10) return std::filesystem::path(std::string(guest.substr(9, 1)) + ":" + std::string(guest.substr(10)));
        return std::filesystem::path(std::string(guest));
    };
    std::error_code ignored;
    std::filesystem::remove(host_target, ignored);
    gui::appkit::FileDialogRequest seen;
    gui::appkit::set_file_dialog_override([&](const gui::appkit::FileDialogRequest& request) {
        seen = request;
        return std::optional<std::vector<std::string>>(std::vector<std::string>{target});
    });
    const auto panel = f.id(f.cls("NSSavePanel"), "savePanel");
    static_cast<void>(f.id(panel, "setAllowedFileTypes:", {f.array({f.str("txt"), f.str("public.json")})}));
    static_cast<void>(f.id(panel, "setNameFieldLabel:", {f.str("Save as:")}));
    require(f.id(panel, "runModal") == 1, "the save panel returns OK");
    require(seen.save && seen.name_label == "Save as:" && seen.extensions == std::vector<std::string>{"txt", "json"}, "panel request");
    require(f.text(f.id(f.id(panel, "URL"), "path")) == target, "the chosen URL: " + f.text(f.id(f.id(panel, "URL"), "path")) + " vs " + target);
    // A document saved through the panel.
    const auto document_class = f.subclass("G4TestDocument", "NSDocument");
    f.add(document_class, "dataOfType:error:", [&f](CpuState& s, GuestMemory&) {
        const auto block = f.process.allocate_guest(5);
        f.process.main_engine().write_bytes(block, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("hello"), 5));
        CallRegs make;
        make.x[2] = block;
        make.x[3] = 5;
        s.x[0] = f.send(f.cls("NSData"), "dataWithBytes:length:", make).x0;
    }, "@@:@^@");
    const auto document = f.id(document_class, "new");
    static_cast<void>(f.id(document, "setFileType:", {f.str("public.plain-text")}));
    static_cast<void>(f.id(document, "updateChangeCount:", {0}));
    static_cast<void>(f.id(document, "saveDocument:", {0}));
    std::ifstream written(host_target, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
    written.close();
    require(contents == "hello", "the document's data was written");
    require(f.text(f.id(f.id(document, "fileURL"), "path")) == target && f.id(document, "isDocumentEdited") == 0, "saved document state");
    std::filesystem::remove(host_target, ignored);
    gui::appkit::set_file_dialog_override({});
    // NSAlert: the suppression checkbox's state comes back in the button.
    gui::appkit::set_alert_presenter_override([](const gui::appkit::AlertRequest& request) -> std::size_t {
        if (!request.suppression.empty() && request.suppression_result) *request.suppression_result = true;
        return 0;
    });
    const auto alert = f.id(f.cls("NSAlert"), "new");
    static_cast<void>(f.id(alert, "setMessageText:", {f.str("Hello")}));
    static_cast<void>(f.id(alert, "setShowsSuppressionButton:", {1}));
    require(f.id(alert, "runModal") == 1000, "first button");
    require(f.id(f.id(alert, "suppressionButton"), "state") == 1, "the suppression checkbox was checked");
    gui::appkit::set_alert_presenter_override({});
}
// Workspace configuration, drag-and-drop helper classes, the tab view
// controller and toolbar items.
void workspace_and_dragging(Fixture& f) {
    const auto configuration = f.id(f.cls("NSWorkspaceOpenConfiguration"), "configuration");
    require(f.id(configuration, "activates") == 1, "activates by default");
    static_cast<void>(f.id(configuration, "setActivates:", {0}));
    require(f.id(configuration, "activates") == 0, "setActivates:");
    const auto component = f.id(f.cls("NSDraggingImageComponent"), "draggingImageComponentWithKey:", {f.str("icon")});
    require(f.text(f.id(component, "key")) == "icon", "dragging image component key");
    const auto readable = f.foundation_items(f.id(f.cls("NSFilePromiseReceiver"), "readableDraggedTypes"));
    require(!readable.empty(), "file promise pasteboard types");
    const auto context_class = f.cls("NSAnimationContext");
    static_cast<void>(f.id(context_class, "beginGrouping"));
    static_cast<void>(f.id(context_class, "beginGrouping"));
    require(f.id(f.id(context_class, "currentContext"), "groupingLevel") == 2, "nested animation groups");
    static_cast<void>(f.id(context_class, "endGrouping"));
    static_cast<void>(f.id(context_class, "endGrouping"));
    require(f.id(f.id(context_class, "currentContext"), "groupingLevel") == 0, "groups closed");
    const auto provider = f.id(f.id(f.cls("NSFilePromiseProvider"), "alloc"), "initWithFileType:delegate:", {f.str("public.plain-text"), 0});
    require(f.text(f.id(provider, "fileType")) == "public.plain-text", "file promise provider type");
    // The tab view controller's items go to its tab view.
    const auto controller = f.id(f.cls("NSTabViewController"), "new");
    const auto first = f.id(f.id(f.cls("NSTabViewItem"), "alloc"), "initWithIdentifier:", {f.str("a")});
    const auto second = f.id(f.id(f.cls("NSTabViewItem"), "alloc"), "initWithIdentifier:", {f.str("b")});
    static_cast<void>(f.id(controller, "setTabViewItems:", {f.array({first, second})}));
    require(f.foundation_items(f.id(controller, "tabViewItems")).size() == 2, "setTabViewItems:");
    static_cast<void>(f.id(controller, "setTabViewItems:", {f.array({second})}));
    require(f.foundation_items(f.id(controller, "tabViewItems")).size() == 1, "setTabViewItems: replaces the items");
    // Toolbar default and principal items.
    const auto toolbar = f.id(f.id(f.cls("NSToolbar"), "alloc"), "initWithIdentifier:", {f.str("main")});
    static_cast<void>(f.id(toolbar, "setDefaultItemIdentifiers:", {f.array({f.str("go")})}));
    static_cast<void>(f.id(toolbar, "setPrincipalItemIdentifier:", {f.str("go")}));
    static_cast<void>(f.id(toolbar, "setAutosaves:", {1}));
    require(f.foundation_items(f.id(toolbar, "defaultItemIdentifiers")).size() == 1 && f.text(f.id(toolbar, "principalItemIdentifier")) == "go" &&
                f.id(toolbar, "autosaves") == 1, "toolbar items");
}
// NSPopover: shown below its anchor in a child window, closed by a click
// elsewhere when transient, by -performClose: otherwise.
void popovers(Fixture& f) {
    const auto window = f.make_window({{100, 300}, {400, 300}});
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    const auto anchor = f.view({{50, 100}, {80, 20}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {anchor}));
    const auto controller = f.id(f.cls("NSViewController"), "new");
    static_cast<void>(f.id(controller, "setView:", {f.view({{0, 0}, {200, 120}})}));
    const auto popover = f.id(f.cls("NSPopover"), "new");
    static_cast<void>(f.id(popover, "setContentViewController:", {controller}));
    static_cast<void>(f.id(popover, "setBehavior:", {1}));   // transient
    CallRegs show;
    gui::appkit::put_rect(show, 0, {});
    show.x[2] = anchor;
    show.x[3] = 1;   // NSMinYEdge: below an unflipped view
    static_cast<void>(f.send(popover, "showRelativeToRect:ofView:preferredEdge:", show));
    require(f.id(popover, "isShown") == 1, "the popover is shown");
    const auto children = f.foundation_items(f.id(window, "childWindows"));
    require(children.size() == 1, "the popover window is a child window");
    const auto frame = gui::appkit::result_rect(f.send(children[0], "frame"));
    const auto parent = gui::appkit::result_rect(f.send(window, "frame"));
    require(std::abs(frame.size.width - 200) < 0.5 && frame.origin.y + frame.size.height < parent.origin.y + 100 + 1,
            "below the anchor at the content size");
    // A click in the parent window closes a transient popover.
    gui::appkit::instance()->mouse_down_hook(window);
    require(f.id(popover, "isShown") == 0 && f.foundation_items(f.id(window, "childWindows")).empty(), "closed by a click outside");
    static_cast<void>(f.id(popover, "setBehavior:", {0}));
    static_cast<void>(f.send(popover, "showRelativeToRect:ofView:preferredEdge:", show));
    gui::appkit::instance()->mouse_down_hook(window);
    require(f.id(popover, "isShown") == 1, "an application-defined popover stays");
    static_cast<void>(f.id(popover, "performClose:", {0}));
    require(f.id(popover, "isShown") == 0, "-performClose:");
    // Appearances: a view's own, inherited by its subviews, else the window's.
    const auto dark = f.id(f.cls("NSAppearance"), "appearanceNamed:", {f.str("NSAppearanceNameDarkAqua")});
    const auto light = f.id(f.cls("NSAppearance"), "appearanceNamed:", {f.str("NSAppearanceNameAqua")});
    static_cast<void>(f.id(window, "setAppearance:", {light}));
    const auto inner = f.view({{0, 0}, {10, 10}});
    static_cast<void>(f.id(anchor, "addSubview:", {inner}));
    require(f.id(inner, "effectiveAppearance") == light, "the window's appearance is inherited");
    static_cast<void>(f.id(anchor, "setAppearance:", {dark}));
    require(f.id(inner, "effectiveAppearance") == dark && f.id(f.id(window, "contentView"), "effectiveAppearance") == light,
            "a view's appearance applies to its subtree");
    static_cast<void>(f.id(window, "close"));
}
// A drag from one view to another in the same window.
void drag_sessions(Fixture& f) {
    const auto window = f.make_window({{100, 100}, {400, 300}});
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    const auto content = f.id(window, "contentView");
    const auto source_class = f.subclass("G4TestDragSource", "NSView");
    std::uint64_t ended_operation = 99;
    std::string received;
    f.add(source_class, "draggingSession:sourceOperationMaskForDraggingContext:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "Q@:@q");
    f.add(source_class, "draggingSession:endedAtPoint:operation:", [&](CpuState& s, GuestMemory&) { ended_operation = s.x[3]; }, "v@:@{CGPoint=dd}Q");
    const auto destination_class = f.subclass("G4TestDropTarget", "NSView");
    f.add(destination_class, "draggingEntered:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "Q@:@");
    f.add(destination_class, "performDragOperation:", [&](CpuState& s, GuestMemory&) {
        const auto board = f.id(s.x[2], "draggingPasteboard");
        received = f.text(f.id(board, "stringForType:", {f.str("com.test.cue")}));
        s.x[0] = 1;
    }, "B@:@");
    const auto source = f.view({{10, 10}, {50, 50}}, "G4TestDragSource");
    const auto destination = f.view({{200, 10}, {100, 100}}, "G4TestDropTarget");
    static_cast<void>(f.id(content, "addSubview:", {source}));
    static_cast<void>(f.id(content, "addSubview:", {destination}));
    static_cast<void>(f.id(destination, "registerForDraggedTypes:", {f.array({f.str("com.test.cue")})}));
    const auto item = f.id(f.cls("NSPasteboardItem"), "new");
    static_cast<void>(f.id(item, "setString:forType:", {f.str("cue-1"), f.str("com.test.cue")}));
    const auto dragging = f.id(f.id(f.cls("NSDraggingItem"), "alloc"), "initWithPasteboardWriter:", {item});
    const auto number = f.id(window, "windowNumber");
    gui::Event down;
    down.type = gui::event_type::left_mouse_down;
    down.window = number;
    down.location = {30, 30};
    down.pressed_buttons = 1;
    const auto down_event = gui::appkit::instance()->make_event(down, window);
    auto move = down;
    move.type = gui::event_type::left_mouse_dragged;
    move.location = {250, 50};
    f.server.queue(move);
    auto up = move;
    up.type = gui::event_type::left_mouse_up;
    up.pressed_buttons = 0;
    f.server.queue(up);
    const auto session = f.id(source, "beginDraggingSessionWithItems:event:source:", {f.array({dragging}), down_event, source});
    require(session != 0, "a dragging session");
    require(received == "cue-1" && ended_operation == 1, "dropped on the destination with a copy");
    static_cast<void>(f.id(window, "close"));
}
} // namespace

Address Fixture::block(transmute::GuestCallHandler handler) {
    static int serial = 0;
    const auto invoke = process.install_function("_g4_test_block_" + std::to_string(serial++), std::move(handler), "/usr/lib/libg4test.dylib");
    auto isa = process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock");
    if (!isa) isa = process.resolve("/usr/lib/system/libsystem_blocks.dylib", "__NSConcreteGlobalBlock");
    require(isa.has_value(), "the global block class");
    const auto descriptor = process.allocate_guest(16);
    auto& m = process.main_engine();
    m.write64(descriptor, 0);
    m.write64(descriptor + 8, 32);
    const auto literal = process.allocate_guest(32);
    m.write64(literal, isa->address);
    m.write64(literal + 8, 1ULL << 28);   // BLOCK_IS_GLOBAL
    m.write64(literal + 16, invoke);
    m.write64(literal + 24, descriptor);
    return literal;
}

std::vector<Address> Fixture::foundation_items(Address array) {
    std::vector<Address> values;
    if (!array) return values;
    const auto count = id(array, "count");
    for (Address i = 0; i < count; ++i) values.push_back(id(array, "objectAtIndex:", {i}));
    return values;
}

// Layout guides, safe areas and the window's content layout guide (the frameworks wave, U10).
void layout_guides_and_safe_areas(Fixture& f) {
    const auto window = f.make_window({{100, 100}, {400, 300}});
    const auto content = f.id(window, "contentView");
    const auto guide = f.id(f.id(f.cls("NSLayoutGuide"), "alloc"), "init");
    static_cast<void>(f.id(guide, "setIdentifier:", {f.str("u10 guide")}));
    static_cast<void>(f.id(content, "addLayoutGuide:", {guide}));
    require(f.id(guide, "owningView") == content && f.foundation_items(f.id(content, "layoutGuides")) == std::vector<Address>{guide} &&
                f.text(f.id(guide, "identifier")) == "u10 guide",
            "a custom guide belongs to its view");
    f.anchor_constraint(guide, "leadingAnchor", "constraintEqualTo", content, "leadingAnchor", 10);
    f.anchor_constraint(guide, "trailingAnchor", "constraintEqualTo", content, "trailingAnchor", -10);
    f.anchor_constraint(guide, "topAnchor", "constraintEqualTo", content, "topAnchor", 20);
    f.size_constraint(guide, "heightAnchor", "constraintEqualTo", 100);
    const auto v = f.view({{0, 0}, {10, 10}});
    static_cast<void>(f.id(v, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
    static_cast<void>(f.id(content, "addSubview:", {v}));
    f.anchor_constraint(v, "leadingAnchor", "constraintEqualTo", guide, "leadingAnchor", 0);
    f.anchor_constraint(v, "trailingAnchor", "constraintEqualTo", guide, "centerXAnchor", 0);
    f.anchor_constraint(v, "topAnchor", "constraintEqualTo", guide, "bottomAnchor", 5);
    f.anchor_constraint(v, "heightAnchor", "constraintEqualTo", guide, "heightAnchor", 0);
    static_cast<void>(f.id(window, "layoutIfNeeded"));
    const auto guide_frame = gui::appkit::result_rect(f.send(guide, "frame"));
    require(std::abs(guide_frame.origin.x - 10) < 0.01 && std::abs(guide_frame.origin.y - 180) < 0.01 &&
                std::abs(guide_frame.size.width - 380) < 0.01 && std::abs(guide_frame.size.height - 100) < 0.01,
            "the guide's solved frame in its owner's coordinates");
    expect_frame(f, v, {{10, 75}, {190, 100}}, "a view placed against the guide");
    require(f.foundation_items(f.id(guide, "constraintsAffectingLayoutForOrientation:", {0})).size() == 4 &&
                f.foundation_items(f.id(guide, "constraintsAffectingLayoutForOrientation:", {1})).size() == 4,
            "the constraints that refer to the guide, by axis");
    bool stopped{};
    try { static_cast<void>(f.id(guide, "hasAmbiguousLayout")); } catch (const std::exception&) { stopped = true; }
    require(stopped, "layout ambiguity stops explicitly");
    static_cast<void>(f.id(content, "removeLayoutGuide:", {guide}));
    require(f.id(guide, "owningView") == 0 && f.foundation_items(f.id(content, "layoutGuides")).empty() &&
                f.objc.find_host_state<gui::appkit::ViewState>(content)->constraints.empty(),
            "removing the guide drops the constraints that refer to it");
    bool refused{};
    try { f.anchor_constraint(guide, "leadingAnchor", "constraintEqualTo", content, "leadingAnchor", 0); } catch (const std::exception&) { refused = true; }
    require(refused, "a guide without an owning view cannot be constrained");
    static_cast<void>(f.id(window, "close"));

    // A full-size content view: the safe area starts below the title bar.
    CallRegs init;
    gui::appkit::put_rect(init, 0, {{100, 100}, {400, 300}});
    init.x[2] = gui::style::titled | gui::style::closable | gui::style::resizable | gui::style::full_size_content_view;
    init.x[3] = 2;
    const auto full = f.send(f.id(f.cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    static_cast<void>(f.id(full, "setReleasedWhenClosed:", {0}));
    const auto top_content = f.id(full, "contentView");
    const auto layout_rect = gui::appkit::result_rect(f.send(full, "contentLayoutRect"));
    const double band = 300 - layout_rect.size.height;
    const auto insets = [&f](Address view) {
        const auto r = f.send(view, "safeAreaInsets");
        return std::array<double, 4>{r.d(0), r.d(1), r.d(2), r.d(3)};
    };
    require(band > 0 && insets(top_content) == std::array<double, 4>{band, 0, 0, 0}, "the content view's safe area is below the title bar");
    const auto child = f.view({{0, 0}, {400, 300}});
    static_cast<void>(f.id(top_content, "addSubview:", {child}));
    CallRegs extra;
    for (unsigned i = 0; i < 4; ++i) extra.set_d(i, std::array<double, 4>{10, 5, 0, 0}[i]);
    static_cast<void>(f.send(top_content, "setAdditionalSafeAreaInsets:", extra));
    require(insets(top_content) == std::array<double, 4>{band + 10, 5, 0, 0} && insets(child) == std::array<double, 4>{band + 10, 5, 0, 0} &&
                f.send(top_content, "additionalSafeAreaInsets").d(0) == 10,
            "additional insets narrow the view's safe area and its subviews'");
    const auto safe = f.id(top_content, "safeAreaLayoutGuide"), margins = f.id(top_content, "layoutMarginsGuide");
    require(safe && safe == f.id(top_content, "safeAreaLayoutGuide") && margins && safe != margins &&
                f.foundation_items(f.id(top_content, "layoutGuides")).empty(),
            "the system guides are made once and are not custom guides");
    const auto in_safe = f.view({{0, 0}, {10, 10}}), in_margins = f.view({{0, 0}, {10, 10}});
    for (const auto [view, guide_object] : {std::pair{in_safe, safe}, std::pair{in_margins, margins}}) {
        static_cast<void>(f.id(view, "setTranslatesAutoresizingMaskIntoConstraints:", {0}));
        static_cast<void>(f.id(top_content, "addSubview:", {view}));
        for (const auto* edge : {"leadingAnchor", "trailingAnchor", "topAnchor", "bottomAnchor"})
            f.anchor_constraint(view, edge, "constraintEqualTo", guide_object, edge, 0);
    }
    static_cast<void>(f.id(full, "layoutIfNeeded"));
    expect_frame(f, in_safe, {{5, 0}, {395, 300 - band - 10}}, "a view pinned to the safe area guide");
    expect_frame(f, in_margins, {{25, 20}, {355, 300 - band - 10 - 40}}, "a view pinned to the layout margins guide");
    // The window's content layout guide is its content layout rectangle.
    const auto content_guide = f.id(full, "contentLayoutGuide");
    const auto content_frame = gui::appkit::result_rect(f.send(content_guide, "frame"));
    require(content_guide && f.id(content_guide, "owningView") == top_content && std::abs(content_frame.size.height - layout_rect.size.height) < 0.01 &&
                std::abs(content_frame.size.width - 400) < 0.01 && std::abs(content_frame.origin.y) < 0.01,
            "the window's content layout guide");
    // Insets that change move the views in the next pass.
    for (unsigned i = 0; i < 4; ++i) extra.set_d(i, 0);
    static_cast<void>(f.send(top_content, "setAdditionalSafeAreaInsets:", extra));
    static_cast<void>(f.id(full, "layoutIfNeeded"));
    expect_frame(f, in_safe, {{0, 0}, {400, 300 - band}}, "the safe area guide follows the insets");
    static_cast<void>(f.id(full, "close"));
}

// NSSplitView holding priorities and NSSplitViewController / NSSplitViewItem (the frameworks wave, U10).
void split_views_and_controllers(Fixture& f) {
    // A plain split view: the lowest holding priority takes a resize.
    const auto split = f.view({{0, 0}, {300, 100}}, "NSSplitView");
    static_cast<void>(f.id(split, "setVertical:", {1}));
    const auto one = f.view({{0, 0}, {100, 100}}), two = f.view({{0, 0}, {100, 100}});
    for (const auto pane : {one, two}) static_cast<void>(f.id(split, "addArrangedSubview:", {pane}));
    const double divider = f.send(split, "dividerThickness").d(0);
    CallRegs position;
    position.set_d(0, 100);
    position.x[2] = 0;
    static_cast<void>(f.send(split, "setPosition:ofDividerAtIndex:", position));
    CallRegs holding;
    holding.set_s(0, 260.0f);
    holding.x[2] = 0;
    static_cast<void>(f.send(split, "setHoldingPriority:forSubviewAtIndex:", holding));
    require(std::abs(f.send(split, "holdingPriorityForSubviewAtIndex:", [] { CallRegs r; r.x[2] = 0; return r; }()).s(0) - 260) < 1e-6 &&
                std::abs(f.send(split, "holdingPriorityForSubviewAtIndex:", [] { CallRegs r; r.x[2] = 1; return r; }()).s(0) - 250) < 1e-6,
            "holding priorities by subview index");
    CallRegs grow;
    gui::appkit::put_size(grow, 0, {400, 100});
    static_cast<void>(f.send(split, "setFrameSize:", grow));
    require(std::abs(f.frame(one).size.width - 100) < 0.01 && std::abs(f.frame(two).size.width - (300 - divider)) < 0.01,
            "the higher holding priority keeps its size");

    // A split view controller with a sidebar, a content pane and a collapsed inspector.
    const auto pane_class = f.subclass("U10PaneController", "NSViewController");
    f.add(pane_class, "loadView", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("loaded");
        static_cast<void>(f.id(s.x[0], "setView:", {f.view({{0, 0}, {50, 50}})}));
    }, "v@:");
    const auto vc1 = f.id(f.id(pane_class, "alloc"), "init"), vc2 = f.id(f.id(pane_class, "alloc"), "init"), vc3 = f.id(f.id(pane_class, "alloc"), "init");
    const auto item_class = f.cls("NSSplitViewItem");
    const auto sidebar = f.id(item_class, "sidebarWithViewController:", {vc1});
    const auto main_item = f.id(item_class, "splitViewItemWithViewController:", {vc2});
    const auto inspector = f.id(item_class, "inspectorWithViewController:", {vc3});
    require((f.id(sidebar, "canCollapse") & 0xff) && (f.id(sidebar, "isSpringLoaded") & 0xff) && f.id(sidebar, "behavior") == 1 &&
                std::abs(f.send(sidebar, "preferredThicknessFraction").d(0) - 0.15) < 1e-9 && std::abs(f.send(sidebar, "holdingPriority").s(0) - 250) < 1e-6,
            "sidebar defaults");
    require(f.id(inspector, "behavior") == 3 && f.send(inspector, "minimumThickness").d(0) == 270 && f.send(inspector, "maximumThickness").d(0) == 270 &&
                f.id(main_item, "behavior") == 0 && (f.id(main_item, "canCollapse") & 0xff) == 0 && f.send(main_item, "preferredThicknessFraction").d(0) < 0,
            "inspector and plain item defaults");
    static_cast<void>(f.id(inspector, "setCollapsed:", {1}));
    const auto svc = f.id(f.id(f.cls("NSSplitViewController"), "alloc"), "init");
    for (const auto item : {sidebar, main_item, inspector}) static_cast<void>(f.id(svc, "addSplitViewItem:", {item}));
    const auto split_view = f.id(svc, "splitView");
    static_cast<void>(f.id(split_view, "setVertical:", {1}));
    require(f.id(split_view, "delegate") == svc && f.foundation_items(f.id(svc, "childViewControllers")).size() == 3 &&
                f.id(svc, "splitViewItemForViewController:", {vc1}) == sidebar,
            "the controller delegates its split view and holds the items' view controllers");
    journal.clear();
    const auto window = f.make_window({{100, 100}, {800, 500}});
    static_cast<void>(f.id(window, "setContentView:", {f.id(svc, "view")}));
    require(f.id(svc, "view") == split_view && journal.size() == 2 && (f.id(vc3, "isViewLoaded") & 0xff) == 0 &&
                f.foundation_items(f.id(split_view, "arrangedSubviews")).size() == 2,
            "views load lazily: the collapsed inspector's view is not loaded");
    const auto width = [&f](Address vc) { return f.frame(f.id(vc, "view")).size.width; };
    const double gap = f.send(split_view, "dividerThickness").d(0);
    require(std::abs(f.frame(split_view).size.width - 800) < 0.01 && std::abs(width(vc1) - 120) < 0.01 &&
                std::abs(width(vc2) - (800 - gap - 120)) < 0.01,
            "the sidebar takes its preferred fraction, the content the rest");
    CallRegs priority;
    priority.set_s(0, 260.0f);
    static_cast<void>(f.send(sidebar, "setHoldingPriority:", priority));
    f.set_size(window, {1000, 500});
    require(std::abs(width(vc1) - 120) < 0.01 && std::abs(width(vc2) - (1000 - gap - 120)) < 0.01, "a resize goes to the lowest holding priority");
    static_cast<void>(f.id(svc, "toggleSidebar:", {0}));
    require((f.id(sidebar, "isCollapsed") & 0xff) && width(vc1) == 0 && (f.id(f.id(vc1, "view"), "isHidden") & 0xff) &&
                std::abs(width(vc2) - (1000 - gap)) < 0.01,
            "-toggleSidebar: collapses the sidebar; the content takes its space");
    static_cast<void>(f.id(svc, "toggleSidebar:", {0}));
    require((f.id(sidebar, "isCollapsed") & 0xff) == 0 && std::abs(width(vc1) - 120) < 0.01 && std::abs(width(vc2) - (1000 - gap - 120)) < 0.01,
            "and expands it to its former thickness");
    static_cast<void>(f.id(f.id(inspector, "animator"), "setCollapsed:", {0}));
    require(journal.size() == 3 && f.foundation_items(f.id(split_view, "arrangedSubviews")).size() == 3 &&
                std::abs(width(vc3) - 270) < 0.01 && std::abs(width(vc2) - (1000 - 2 * gap - 120 - 270)) < 0.01,
            "the inspector's view loads when it expands, at its fixed thickness");
    CallRegs limit;
    limit.x[2] = split_view;
    limit.set_d(0, 0);
    limit.x[3] = 1;
    require(std::abs(f.send(svc, "splitView:constrainMinCoordinate:ofSubviewAt:", limit).d(0) - (1000 - gap - 270)) < 0.01,
            "a divider keeps the inspector within its thickness");
    CallRegs menu_item;
    menu_item.x[2] = f.str("Toggle Sidebar");
    menu_item.x[3] = f.objc.selector("toggleSidebar:");
    menu_item.x[4] = f.str("");
    const auto toggle = f.send(f.id(f.cls("NSMenuItem"), "alloc"), "initWithTitle:action:keyEquivalent:", menu_item).x0;
    require((f.id(svc, "validateUserInterfaceItem:", {toggle}) & 0xff) != 0, "the sidebar toggle validates");
    // Sidebars collapse below the inline threshold and expand above it.
    CallRegs threshold;
    threshold.set_d(0, 200);
    static_cast<void>(f.send(svc, "setMinimumThicknessForInlineSidebars:", threshold));
    require((f.id(sidebar, "isCollapsed") & 0xff) != 0, "a sidebar thinner than the inline threshold collapses");
    threshold.set_d(0, 100);
    static_cast<void>(f.send(svc, "setMinimumThicknessForInlineSidebars:", threshold));
    require((f.id(sidebar, "isCollapsed") & 0xff) == 0 && std::abs(width(vc1) - 120) < 0.01, "and expands when it fits again");
    static_cast<void>(f.id(svc, "removeSplitViewItem:", {main_item}));
    require(f.foundation_items(f.id(svc, "splitViewItems")).size() == 2 && f.id(vc2, "parentViewController") == 0 &&
                f.foundation_items(f.id(split_view, "arrangedSubviews")).size() == 2,
            "removing an item removes its pane and child view controller");
    bool stopped{};
    try { static_cast<void>(f.id(sidebar, "addTopAlignedAccessoryViewController:", {vc2})); } catch (const std::exception&) { stopped = true; }
    require(stopped, "accessory view controllers stop explicitly");
    static_cast<void>(f.id(window, "close"));
}

void auto_layout_tests() {
    Fixture f;
    anchors_and_window_pass(f);
    window_grows_for_required_constraints(f);
    translating_views_follow_their_masks(f);
    overrides_fitting_and_detached(f);
    nib_constraints(f);
    control_metrics(f);
    stacks_and_grids(f);
    sheets(f);
    nib_connectors(f);
    panels_documents_alerts(f);
    workspace_and_dragging(f);
    popovers(f);
    drag_sessions(f);
    layout_guides_and_safe_areas(f);
    split_views_and_controllers(f);
}

} // namespace windows_appkit_tests

int main() {
    try {
        windows_appkit_tests::auto_layout_tests();
    } catch (const std::exception& error) {
        std::cerr << "alchemy_gui_appkit_windows: " << error.what() << '\n';
        return 1;
    }
    std::cout << "alchemy_gui_appkit_windows: passed\n";
    return 0;
}
