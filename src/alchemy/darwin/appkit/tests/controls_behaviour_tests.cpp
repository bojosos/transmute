// The G3 controls (src/alchemy/darwin/kit/completion/controls/) over AppKit and the
// fake window server: their API, drawing (pixels of the presented backing
// store), mouse and keyboard handling through window-server events, and nib
// decoding from project-authored NIBArchives (nib_archive.cpp encodes
// them). Native classes stand in for guest subclasses and targets, so every
// call goes through ObjcRuntime::send. Headless: no host windows.
#include "alchemy/darwin/appkit/controls_internal.hpp"
#include "alchemy/darwin/appkit/nib_archive.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/appkit/controls/layout.hpp"
#include "alchemy/darwin/appkit/drawing.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"
#include "alchemy/darwin/foundation/date_format.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace gui = alchemy::gui;
namespace nib = alchemy::gui::nib;
using Address = std::uint64_t;
using alchemy::CallRegs;
using alchemy::CallResult;
using transmute::CpuState;
using transmute::GuestMemory;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<std::string> journal;
bool log_has(const std::string& entry) { return std::find(journal.begin(), journal.end(), entry) != journal.end(); }
std::string joined() {
    std::string out;
    for (const auto& entry : journal) out += (out.empty() ? "" : ", ") + entry;
    return out;
}

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices darwin{process};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    std::unique_ptr<alchemy::FoundationBridge> foundation;
    gui::testing::FakeWindowServer server;
    std::shared_ptr<gui::completion_kit::Support> support;
    Address app{};

    Fixture() {
        alchemy::install_darwin_numeric(process);
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
        alchemy::install_darwin_frameworks(process, objc, cf);
        gui::appkit::set_window_server_override(&server);
        gui::InstallContext context{process, objc, cf};
        gui::install_appkit_gui(context);
        gui::install_quartzcore(context);
        gui::install_quartz2d(context);   // the CoreText layer (NSAttributedString, NSParagraphStyle) the text system builds on
        gui::install_support_kit(context); // libobjc gap fillers (associated objects) installed before the QLab groups
        support = std::make_shared<gui::completion_kit::Support>(process, objc, cf);
        gui::completion_kit::install_group_controls(*support);
        app = id(cls("NSApplication"), "sharedApplication");
        static_cast<void>(id(app, "finishLaunching"));
        static_cast<void>(id(app, "setAppearance:", {id(cls("NSAppearance"), "appearanceNamed:", {str("NSAppearanceNameAqua")})}));
    }
    ~Fixture() { gui::appkit::set_window_server_override(nullptr); }

    Address cls(std::string_view name) {
        const auto value = objc.find_class(name);
        require(value != 0, "missing class " + std::string(name));
        return value;
    }
    CallResult send(Address receiver, std::string_view selector, CallRegs registers = {}) {
        return objc.send(receiver, objc.selector(selector), std::move(registers));
    }
    Address id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        std::size_t index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return send(receiver, selector, registers).x0;
    }
    bool yes(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        return (id(receiver, selector, arguments) & 0xff) != 0;
    }
    std::int64_t integer(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        return static_cast<std::int64_t>(id(receiver, selector, arguments));
    }
    Address str(std::string_view text) { return cf.string(text); }
    std::string text(Address object) { return object ? cf.utf8(object) : std::string("(nil)"); }
    Address subclass(std::string name, std::string_view super) {
        if (const auto existing = objc.find_class(name)) return existing;
        return objc.register_native_class(std::move(name), cls(super), 8);
    }
    void add(Address target, std::string name, transmute::GuestCallHandler handler, std::string types = "v@:@") {
        static_cast<void>(objc.add_native_method(target, std::move(name), std::move(handler), false, std::move(types)));
    }
    Address next_event() { return id(app, "nextEventMatchingMask:untilDate:inMode:dequeue:", {~0ULL, 0, 0, 1}); }
    void pump() { while (const auto event = next_event()) static_cast<void>(id(app, "sendEvent:", {event})); }
    Address with_rect(Address receiver, std::string_view selector, gui::Rect rect) {
        CallRegs registers;
        gui::appkit::put_rect(registers, 0, rect);
        return send(receiver, selector, registers).x0;
    }
    Address make_window(gui::Rect content) {
        CallRegs init;
        gui::appkit::put_rect(init, 0, content);
        init.x[2] = gui::style::titled | gui::style::closable;
        init.x[3] = 2;
        init.x[4] = 0;
        const auto made = send(id(cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
        static_cast<void>(id(made, "setReleasedWhenClosed:", {0}));
        static_cast<void>(id(made, "setBackgroundColor:", {id(cls("NSColor"), "whiteColor")}));
        return made;
    }
    void show(Address window) {
        static_cast<void>(id(window, "makeKeyAndOrderFront:", {0}));
        static_cast<void>(next_event());
    }
    void click(Address window, gui::Point location, std::int64_t clicks = 1) {
        gui::Event down;
        down.type = gui::event_type::left_mouse_down;
        down.window = id(window, "windowNumber");
        down.location = location;
        down.click_count = clicks;
        down.pressed_buttons = 1;
        server.queue(down);
        auto up = down;
        up.type = gui::event_type::left_mouse_up;
        up.pressed_buttons = 0;
        server.queue(up);
        pump();
    }
    void key(Address window, std::uint16_t code, std::string characters) {
        gui::Event press;
        press.type = gui::event_type::key_down;
        press.window = id(window, "windowNumber");
        press.key_code = code;
        press.characters = characters;
        press.characters_ignoring_modifiers = characters;
        server.queue(press);
        pump();
    }
    std::array<std::uint8_t, 4> pixel(Address window, double x, double y) {
        static_cast<void>(next_event());   // display pending changes
        return server.pixel(id(window, "windowNumber"), x, y);
    }
    // A target object recording its action messages in the journal.
    Address target(std::string name) {
        const auto made_class = subclass("TestG3Target_" + name, "NSObject");
        add(made_class, "acted:", [this, name](CpuState& s, GuestMemory&) { journal.push_back(name + ":" + describe(s.x[2])); });
        add(made_class, "doubleActed:", [name](CpuState&, GuestMemory&) { journal.push_back(name + ":double"); });
        return id(id(made_class, "alloc"), "init");
    }
    std::function<std::string(Address)> describe = [](Address) { return std::string(); };
    // A compiler-emitted (global) block whose invoke runs `handler`.
    Address block(std::string name, transmute::GuestCallHandler handler) {
        const auto invoke = process.install_function(std::move(name), std::move(handler), "alchemy.test");
        const auto value = process.allocate_guest(32), descriptor = process.allocate_guest(16);
        engine.write64(descriptor, 0);
        engine.write64(descriptor + 8, 32);
        engine.write64(value, process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock")->address);
        engine.write32(value + 8, 1U << 28);
        engine.write32(value + 12, 0);
        engine.write64(value + 16, invoke);
        engine.write64(value + 24, descriptor);
        return value;
    }
    Address with_double(Address receiver, std::string_view selector, double value, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        registers.set_d(0, value);
        std::size_t index = 2;
        for (const auto argument : arguments) registers.x[index++] = argument;
        return send(receiver, selector, registers).x0;
    }
    Address date(double reference) { return with_double(cls("NSDate"), "dateWithTimeIntervalSinceReferenceDate:", reference); }
    double seconds(Address date_object) { return send(date_object, "timeIntervalSinceReferenceDate").d(0); }
};

// Pixels that are not near white (marks, text, tints) in a window rectangle.
std::size_t marked(Fixture& f, Address window, gui::Rect rect) {
    static_cast<void>(f.next_event());   // display pending changes
    std::size_t count = 0;
    for (double y = rect.origin.y; y < rect.origin.y + rect.size.height; y += 0.5)
        for (double x = rect.origin.x; x < rect.origin.x + rect.size.width; x += 0.5) {
            const auto v = f.server.pixel(f.id(window, "windowNumber"), x, y);
            if (v[0] < 200 || v[1] < 200 || v[2] < 200) ++count;
        }
    return count;
}

bool is_accent(std::array<std::uint8_t, 4> bgra) { return bgra[0] > 180 && bgra[2] < 80; }
bool is_light(std::array<std::uint8_t, 4> bgra) { return bgra[0] > 225 && bgra[1] > 225 && bgra[2] > 225; }
std::string bgra_text(std::array<std::uint8_t, 4> v) {
    return "bgra(" + std::to_string(v[0]) + "," + std::to_string(v[1]) + "," + std::to_string(v[2]) + "," + std::to_string(v[3]) + ")";
}
// Dark (text) pixels in a window rectangle (points, y up).
std::size_t inked(Fixture& f, Address window, gui::Rect rect) {
    static_cast<void>(f.next_event());   // display pending changes
    std::size_t count = 0;
    for (double y = rect.origin.y; y < rect.origin.y + rect.size.height; y += 0.5)
        for (double x = rect.origin.x; x < rect.origin.x + rect.size.width; x += 0.5) {
            const auto v = f.server.pixel(f.id(window, "windowNumber"), x, y);
            if (v[0] < 110 && v[1] < 110 && v[2] < 110) ++count;
        }
    return count;
}

// ---- NSSegmentedControl ----------------------------------------------------------------------
void segmented_controls(Fixture& f) {
    journal.clear();
    f.describe = [&f](Address sender) { return std::to_string(f.integer(sender, "selectedSegment")); };
    const auto target = f.target("segments");
    const auto labels = f.cf.array(std::vector<Address>{f.str("One"), f.str("Two"), f.str("Three")});
    const auto control = f.id(f.cls("NSSegmentedControl"), "segmentedControlWithLabels:trackingMode:target:action:",
                              {labels, 0, target, f.objc.selector("acted:")});
    require(f.integer(control, "segmentCount") == 3 && f.text(f.id(control, "labelForSegment:", {1})) == "Two", "segments from labels");
    require(f.integer(control, "selectedSegment") == -1 && f.id(f.id(control, "cell"), "class") == f.cls("NSSegmentedCell"), "initial state");
    const auto fitting = f.send(control, "frame").d(2);
    require(fitting >= 3 * 20 && fitting < 400, "-sizeToFit width " + std::to_string(fitting));
    // Selection API (select one).
    static_cast<void>(f.id(control, "setSelectedSegment:", {1}));
    require(f.yes(control, "isSelectedForSegment:", {1}) && !f.yes(control, "isSelectedForSegment:", {0}), "-setSelectedSegment:");
    for (Address i = 0; i < 3; ++i) static_cast<void>(f.id(control, "setTag:forSegment:", {10 + i, i}));
    require(f.yes(control, "selectSegmentWithTag:", {12}) && f.integer(control, "selectedSegment") == 2 && !f.yes(control, "isSelectedForSegment:", {1}),
            "-selectSegmentWithTag:");
    require(!f.yes(control, "selectSegmentWithTag:", {99}) && f.integer(control, "selectedSegment") == 2, "an unknown tag selects nothing");
    static_cast<void>(f.id(control, "setSelectedSegment:", {static_cast<Address>(-1)}));
    require(f.integer(control, "selectedSegment") == -1 && !f.yes(control, "isSelectedForSegment:", {2}), "deselection");

    // In a window: equal 80-point segments at x 20..260.
    const auto window = f.make_window({{0, 0}, {300, 100}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {control}));
    CallRegs frame;
    gui::appkit::put_rect(frame, 0, {{20, 20}, {240, 24}});
    static_cast<void>(f.send(control, "setFrame:", frame));
    static_cast<void>(f.id(control, "setSegmentDistribution:", {2}));
    f.show(window);
    f.click(window, {230, 32});
    require(log_has("segments:2") && f.integer(control, "selectedSegment") == 2, "a click selects the third segment: " + joined());
    const auto lit = f.pixel(window, 185, 25), plain = f.pixel(window, 25, 25);
    require(is_accent(lit), "the selected segment is drawn in the accent color: " + bgra_text(lit));
    require(is_light(plain), "an unselected segment is drawn as the control face: " + bgra_text(plain));
    require(inked(f, window, {{20, 20}, {80, 24}}) > 10, "the first segment's label was drawn");
    // A release on another segment does not click.
    journal.clear();
    gui::Event down;
    down.type = gui::event_type::left_mouse_down;
    down.window = f.id(window, "windowNumber");
    down.location = {30, 32};
    down.click_count = 1;
    down.pressed_buttons = 1;
    f.server.queue(down);
    auto away = down;
    away.type = gui::event_type::left_mouse_up;
    away.location = {150, 32};
    away.pressed_buttons = 0;
    f.server.queue(away);
    f.pump();
    require(journal.empty() && f.integer(control, "selectedSegment") == 2, "a release on another segment clicked: " + joined());
    // Disabled segments ignore clicks.
    static_cast<void>(f.id(control, "setEnabled:forSegment:", {0, 0}));
    f.click(window, {30, 32});
    require(journal.empty() && !f.yes(control, "isEnabledForSegment:", {0}), "a disabled segment clicked");
    static_cast<void>(f.id(control, "setEnabled:forSegment:", {1, 0}));
    // Select any: clicks toggle segments.
    static_cast<void>(f.id(control, "setTrackingMode:", {1}));
    f.click(window, {30, 32});
    f.click(window, {110, 32});
    require(f.yes(control, "isSelectedForSegment:", {0}) && f.yes(control, "isSelectedForSegment:", {1}) &&
                f.yes(control, "isSelectedForSegment:", {2}), "select any keeps several selected");
    f.click(window, {110, 32});
    require(!f.yes(control, "isSelectedForSegment:", {1}) && log_has("segments:1"), "select any toggles");
    // Momentary: selected only while the action runs.
    journal.clear();
    static_cast<void>(f.id(control, "setTrackingMode:", {2}));
    f.click(window, {110, 32});
    require(log_has("segments:1") && f.integer(control, "selectedSegment") == -1, "momentary tracking: " + joined());
    // Fixed widths and the cell's segment hit testing.
    static_cast<void>(f.id(control, "setSegmentDistribution:", {0}));
    CallRegs width;
    width.set_d(0, 100);
    width.x[2] = 0;
    static_cast<void>(f.send(control, "setWidth:forSegment:", width));
    require(f.send(control, "widthForSegment:", [] { CallRegs r; r.x[2] = 0; return r; }()).d(0) == 100, "-widthForSegment:");
    CallRegs probe;
    gui::appkit::put_point(probe, 0, {105, 10});
    gui::appkit::put_rect(probe, 2, {{0, 0}, {240, 24}});
    require(f.send(f.id(control, "cell"), "_alchemySegmentAtPoint:inFrame:", probe).x0 == 1, "the second segment starts after the fixed 100");
    // Out-of-range segment indexes fail explicitly.
    bool threw = false;
    try { static_cast<void>(f.id(control, "labelForSegment:", {7})); } catch (const std::exception&) { threw = true; }
    require(threw, "an out-of-range segment did not fail");
    static_cast<void>(f.id(window, "close"));
}

// ---- NSPathControl ---------------------------------------------------------------------------
void path_controls(Fixture& f) {
    journal.clear();
    f.describe = [&f](Address sender) {
        const auto clicked = f.id(sender, "clickedPathComponentCell");
        return clicked ? f.text(f.id(clicked, "stringValue")) : std::string("none");
    };
    const auto target = f.target("path");
    const auto control = f.with_rect(f.id(f.cls("NSPathControl"), "alloc"), "initWithFrame:", {{10, 30}, {380, 22}});
    require(f.id(f.id(control, "cell"), "class") == f.cls("NSPathCell"), "path cell class");
    const auto url = f.id(f.cls("NSURL"), "URLWithString:", {f.str("file:///Users/bob/Music/song.wav")});
    static_cast<void>(f.id(control, "setURL:", {url}));
    const auto components = f.id(control, "pathComponentCells");
    // The URL's path as Foundation reports it here (a host test maps guest
    // paths onto a volume): the components are its split (layout.cpp).
    const auto path = f.text(f.id(url, "path"));
    const auto parts = alchemy::gui::completion_kit::controls::layout::split_path(path, "Macintosh HD");
    require(f.integer(components, "count") == static_cast<std::int64_t>(parts.size()) && parts.size() >= 5,
            "component cells: " + std::to_string(f.integer(components, "count")) + " for " + path);
    const auto title = [&](Address i) { return f.text(f.id(f.id(components, "objectAtIndex:", {i}), "stringValue")); };
    for (std::size_t i = 0; i < parts.size(); ++i) {
        require(title(i) == parts[i].title, "component title " + std::to_string(i) + ": " + title(i));
        require(f.text(f.id(f.id(f.id(components, "objectAtIndex:", {i}), "URL"), "path")) == parts[i].path, "component URL " + std::to_string(i));
    }
    require(parts.back().title == "song.wav" && parts[parts.size() - 2].title == "Music", "the deepest components");
    require(f.text(f.id(f.id(control, "URL"), "path")) == path, "-URL");
    static_cast<void>(f.id(control, "setTarget:", {target}));
    static_cast<void>(f.id(control, "setAction:", {f.objc.selector("acted:")}));
    static_cast<void>(f.id(control, "setDoubleAction:", {f.objc.selector("doubleActed:")}));
    const auto window = f.make_window({{0, 0}, {400, 80}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {control}));
    f.show(window);
    // The third component's rectangle, then a click in it (view coordinates are flipped).
    CallRegs rect;
    rect.x[2] = f.id(components, "objectAtIndex:", {2});
    gui::appkit::put_rect(rect, 0, {{0, 0}, {380, 22}});
    rect.x[3] = control;
    const auto area = gui::appkit::result_rect(f.send(f.id(control, "cell"), "rectOfPathComponentCell:withFrame:inView:", rect));
    require(area.size.width > 10 && area.origin.x > 40, "component rectangle");
    f.click(window, {10 + area.origin.x + area.size.width / 2, 41});
    require(log_has("path:bob"), "a click on a component: " + joined());
    require(inked(f, window, {{10, 30}, {380, 22}}) > 30, "the path components were drawn");
    journal.clear();
    f.click(window, {10 + area.origin.x + area.size.width / 2, 41}, 2);
    require(log_has("path:double"), "a double click sends the double action: " + joined());
    // A short control keeps the last component readable.
    CallRegs narrow;
    gui::appkit::put_rect(narrow, 0, {{10, 30}, {120, 22}});
    static_cast<void>(f.send(control, "setFrame:", narrow));
    rect.x[2] = f.id(components, "objectAtIndex:", {4});
    gui::appkit::put_rect(rect, 0, {{0, 0}, {120, 22}});
    const auto last = gui::appkit::result_rect(f.send(f.id(control, "cell"), "rectOfPathComponentCell:withFrame:inView:", rect));
    require(last.origin.x + last.size.width <= 116 && last.size.width > 20, "the last component fits the short control");
    // Pop-up style: the last component fills the control.
    static_cast<void>(f.id(control, "setPathStyle:", {2}));
    rect.x[2] = f.id(components, "objectAtIndex:", {static_cast<Address>(parts.size() - 1)});
    gui::appkit::put_rect(rect, 0, {{0, 0}, {120, 22}});
    const auto whole = gui::appkit::result_rect(f.send(f.id(control, "cell"), "rectOfPathComponentCell:withFrame:inView:", rect));
    require(f.integer(control, "pathStyle") == 2 && whole.size.width == 120, "pop-up style");
    static_cast<void>(f.id(window, "close"));
}

// ---- nib decoding --------------------------------------------------------------------------------
struct NibBuilder {
    nib::Archive archive;
    std::vector<std::vector<nib::Value>> pending;
    std::uint32_t class_index(const std::string& name) {
        const auto found = std::find(archive.class_names.begin(), archive.class_names.end(), name);
        if (found != archive.class_names.end()) return static_cast<std::uint32_t>(found - archive.class_names.begin());
        archive.class_names.push_back(name);
        return static_cast<std::uint32_t>(archive.class_names.size() - 1);
    }
    std::uint32_t key(const std::string& name) {
        const auto found = std::find(archive.keys.begin(), archive.keys.end(), name);
        if (found != archive.keys.end()) return static_cast<std::uint32_t>(found - archive.keys.begin());
        archive.keys.push_back(name);
        return static_cast<std::uint32_t>(archive.keys.size() - 1);
    }
    std::uint32_t declare(const std::string& cls) {
        archive.objects.push_back({class_index(cls), 0, 0});
        pending.emplace_back();
        return static_cast<std::uint32_t>(archive.objects.size() - 1);
    }
    void put(std::uint32_t object, const std::string& name, nib::Value value) {
        value.key = key(name);
        pending.at(object).push_back(std::move(value));
    }
    void ref(std::uint32_t object, const std::string& name, std::uint32_t target) {
        nib::Value v; v.type = nib::ValueType::object; v.integer = target; put(object, name, v);
    }
    void integer(std::uint32_t object, const std::string& name, std::int64_t value) {
        nib::Value v; v.type = nib::ValueType::int32; v.integer = value; put(object, name, v);
    }
    void real(std::uint32_t object, const std::string& name, double value) {
        nib::Value v; v.type = nib::ValueType::float64; v.real = value; put(object, name, v);
    }
    void boolean(std::uint32_t object, const std::string& name, bool value) {
        nib::Value v; v.type = value ? nib::ValueType::true_ : nib::ValueType::false_; v.integer = value; put(object, name, v);
    }
    std::uint32_t string(const std::string& text) {
        const auto object = declare("NSString");
        nib::Value v; v.type = nib::ValueType::data; v.data.assign(text.begin(), text.end()); put(object, "NS.bytes", v);
        return object;
    }
    std::uint32_t array(const std::string& cls, const std::vector<std::uint32_t>& elements) {
        const auto object = declare(cls);
        boolean(object, "NSInlinedValue", false);
        for (const auto element : elements) ref(object, "UINibEncoderEmptyKey", element);
        return object;
    }
    std::uint32_t view(const std::string& cls, std::uint32_t superview, const std::string& frame) {
        const auto object = declare(cls);
        integer(object, "NSvFlags", 268);
        ref(object, "NSFrame", string(frame));
        ref(object, "NSSuperview", superview);
        return object;
    }
    nib::Archive finish() {
        archive.values.clear();
        for (std::size_t i = 0; i < archive.objects.size(); ++i) {
            archive.objects[i].first_value = static_cast<std::uint32_t>(archive.values.size());
            archive.objects[i].value_count = static_cast<std::uint32_t>(pending[i].size());
            archive.values.insert(archive.values.end(), pending[i].begin(), pending[i].end());
        }
        archive.coder_version = 10;
        return archive;
    }
};

// A window holding every G3 control, its outlets connected to the owner.
struct ControlsNib {
    NibBuilder b;
    std::uint32_t root{}, data{}, owner{}, window_template{}, content{};
    std::vector<std::uint32_t> subviews, connections, objects;
    ControlsNib() {
        root = b.declare("NSObject");
        data = b.declare("NSIBObjectData");
        owner = b.declare("NSCustomObject");
        window_template = b.declare("NSWindowTemplate");
        content = b.declare("NSView");
        b.ref(root, "IB.objectdata", data);
        b.ref(owner, "NSClassName", b.string("TestG3NibOwner"));
        b.integer(window_template, "NSWindowStyleMask", 7);
        b.ref(window_template, "NSWindowRect", b.string("{{100, 100}, {500, 300}}"));
        b.ref(window_template, "NSWindowTitle", b.string("Controls"));
        b.ref(window_template, "NSWindowClass", b.string("NSWindow"));
        b.ref(window_template, "NSWindowView", content);
        b.integer(content, "NSvFlags", 256);
        b.ref(content, "NSFrameSize", b.string("{500, 300}"));
    }
    void outlet(std::uint32_t destination, const std::string& label) {
        const auto connector = b.declare("NSNibOutletConnector");
        b.ref(connector, "NSSource", owner);
        b.ref(connector, "NSDestination", destination);
        b.ref(connector, "NSLabel", b.string(label));
        connections.push_back(connector);
    }
    std::vector<std::uint8_t> encode() {
        b.ref(content, "NSSubviews", b.array("NSMutableArray", subviews));
        b.ref(data, "NSRoot", owner);
        b.ref(data, "NSVisibleWindows", b.array("NSMutableSet", {window_template}));
        b.ref(data, "NSConnections", b.array("NSMutableArray", connections));
        std::vector<std::uint32_t> keys{window_template, content}, values{owner, window_template};
        for (const auto view : subviews) { keys.push_back(view); values.push_back(content); }
        for (const auto object : objects) { keys.push_back(object); values.push_back(owner); }
        b.ref(data, "NSObjectsKeys", b.array("NSArray", keys));
        b.ref(data, "NSObjectsValues", b.array("NSArray", values));
        return b.finish().encode();
    }
};

std::map<std::string, Address> load_controls_nib(Fixture& f, std::vector<std::uint8_t> bytes, const std::vector<std::string>& outlets) {
    static std::map<std::string, Address> found;
    found.clear();
    const auto owner_class = f.subclass("TestG3NibOwner", "NSObject");
    for (const auto& name : outlets) {
        std::string setter = "set" + name + ":";
        setter[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(setter[3])));
        f.add(owner_class, setter, [name](CpuState& s, GuestMemory&) { found[name] = s.x[2]; });
    }
    const auto owner = f.id(f.id(owner_class, "alloc"), "init");
    const auto block = f.process.allocate_guest(bytes.size());
    f.engine.write_bytes(block, bytes);
    const auto data = f.id(f.cls("NSData"), "dataWithBytes:length:", {block, bytes.size()});
    const auto loaded = f.id(f.id(f.cls("NSNib"), "alloc"), "initWithNibData:bundle:", {data, 0});
    const auto slot = f.process.allocate_guest(8);
    require((f.id(loaded, "instantiateWithOwner:topLevelObjects:", {owner, slot}) & 0xff) == 1, "-instantiateWithOwner:topLevelObjects:");
    for (const auto& name : outlets) require(found.contains(name) && found[name], "nib outlet " + name + " was not connected");
    return found;
}

void nib_decoding(Fixture& f) {
    ControlsNib n;
    auto& b = n.b;
    // NSSegmentedControl (as Workspace.nib archives it).
    const auto segmented = b.view("NSSegmentedControl", n.content, "{{8, 7}, {82, 21}}");
    const auto segmented_cell = b.declare("NSSegmentedCell");
    const auto item_a = b.declare("NSSegmentItem"), item_b = b.declare("NSSegmentItem");
    b.ref(segmented, "NSCell", segmented_cell);
    b.boolean(segmented, "NSEnabled", true);
    b.integer(segmented_cell, "NSCellFlags", 75497472);
    b.integer(segmented_cell, "NSCellFlags2", 131072);
    b.integer(segmented_cell, "NSControlSize2", 1);
    b.ref(segmented_cell, "NSControlView", segmented);
    b.ref(segmented_cell, "NSSegmentImages", b.array("NSMutableArray", {item_a, item_b}));
    b.integer(segmented_cell, "NSSegmentStyle", 1);
    b.integer(segmented_cell, "NSSegmentDistribution", 2);
    b.ref(item_a, "NSSegmentItemLabel", b.string("Cues"));
    b.boolean(item_a, "NSSegmentItemSelected", true);
    b.integer(item_a, "NSSegmentItemImageScaling", 0);
    b.ref(item_b, "NSSegmentItemLabel", b.string("Carts"));
    b.integer(item_b, "NSSegmentItemTag", 1);
    b.real(item_b, "NSSegmentItemWidth", 0);
    n.subviews.push_back(segmented);
    n.outlet(segmented, "segments");
    // NSPathControl (as Inspector.nib archives it, without the NSURL).
    const auto path = b.view("NSPathControl", n.content, "{{0, 200}, {446, 20}}");
    const auto path_cell = b.declare("NSPathCell");
    b.ref(path, "NSCell", path_cell);
    b.boolean(path, "NSEnabled", true);
    b.integer(path_cell, "NSCellFlags", 337641473);
    b.integer(path_cell, "NSCellFlags2", 131072);
    b.ref(path_cell, "NSControlView", path);
    b.ref(path_cell, "NSDelegate", path);
    std::vector<std::uint32_t> component_cells;
    for (const auto* title : {"Macintosh HD", "Applications"}) {
        const auto component = b.declare("NSPathComponentCell");
        b.integer(component, "NSCellFlags", 67108928);
        b.integer(component, "NSCellFlags2", 268567552);
        b.ref(component, "NSContents", b.string(title));
        component_cells.push_back(component);
    }
    b.ref(path_cell, "NSPathComponentCells", b.array("NSMutableArray", component_cells));
    n.subviews.push_back(path);
    n.outlet(path, "path");
    // NSSearchField (as StatusWindow.nib archives it).
    const auto search = b.view("NSSearchField", n.content, "{{0, 230}, {200, 22}}");
    const auto search_cell = b.declare("NSSearchFieldCell");
    b.ref(search, "NSCell", search_cell);
    b.boolean(search, "NSEnabled", true);
    b.integer(search_cell, "NSCellFlags", 347078721);
    b.integer(search_cell, "NSCellFlags2", 268436544);
    b.ref(search_cell, "NSContents", b.string(""));
    b.ref(search_cell, "NSPlaceholderString", b.string("Search"));
    b.ref(search_cell, "NSControlView", search);
    b.boolean(search_cell, "NSDrawsBackground", true);
    for (const auto* key : {"NSSearchButtonCell", "NSCancelButtonCell"}) {
        const auto button_cell = b.declare("NSButtonCell");
        b.integer(button_cell, "NSCellFlags", 0);
        b.integer(button_cell, "NSButtonFlags", 138428416);
        b.ref(button_cell, "NSControlView", search);
        b.ref(search_cell, key, button_cell);
    }
    b.integer(search_cell, "NSMaximumRecents", 255);
    n.subviews.push_back(search);
    n.outlet(search, "search");
    // NSDatePicker (as F53LicenseController.nib archives it; the current date).
    const auto picker = b.view("NSDatePicker", n.content, "{{220, 230}, {106, 27}}");
    const auto picker_cell = b.declare("NSDatePickerCell");
    b.ref(picker, "NSCell", picker_cell);
    b.boolean(picker, "NSEnabled", true);
    b.integer(picker_cell, "NSCellFlags", 71303168);
    b.integer(picker_cell, "NSCellFlags2", 0);
    b.ref(picker_cell, "NSControlView", picker);
    b.real(picker_cell, "NSTimeInterval", 0);
    b.integer(picker_cell, "NSDatePickerElements", 224);
    b.boolean(picker_cell, "NSDatePickerUseCurrentDateDuringDecoding", true);
    n.subviews.push_back(picker);
    n.outlet(picker, "picker");
    // An NSTextView network (as Inspector.nib archives the notes view).
    const auto text = b.view("NSTextView", n.content, "{{0, 100}, {300, 60}}");
    const auto container = b.declare("NSTextContainer");
    const auto manager = b.declare("NSLayoutManager");
    const auto storage = b.declare("NSTextStorage");
    const auto shared = b.declare("NSTextViewSharedData");
    const auto font = b.declare("NSFont");
    b.ref(font, "NSName", b.string("Helvetica"));
    b.real(font, "NSSize", 15);
    const auto attributes = b.declare("NSDictionary");
    b.boolean(attributes, "NSInlinedValue", true);
    b.ref(attributes, "UINibEncoderEmptyKey", b.string("NSFont"));
    b.ref(attributes, "UINibEncoderEmptyKey", font);
    b.ref(text, "NSTextContainer", container);
    b.ref(text, "NSSharedData", shared);
    b.integer(text, "NSTVFlags", 134);
    b.ref(text, "NSMaxSize", b.string("{909, 10000000}"));
    b.ref(container, "NSLayoutManager", manager);
    b.ref(container, "NSTextView", text);
    b.integer(container, "NSTCFlags", 1);
    b.ref(manager, "NSTextStorage", storage);
    b.ref(manager, "NSTextContainers", b.array("NSMutableArray", {container}));
    b.integer(manager, "NSLMFlags", 102);
    b.ref(storage, "NSString", b.string("Notes go here"));
    b.ref(storage, "NSAttributes", attributes);
    b.integer(shared, "NSFlags", 0x4044F2F);   // selectable, editable, rich text, draws background ...
    b.integer(shared, "NSTextCheckingTypes", 0);
    n.subviews.push_back(text);
    n.outlet(text, "text");
    // NSCollectionView with its _NSCollectionViewCore and flow layout (as QLabTemplateChooser.nib).
    const auto collection = b.view("NSCollectionView", n.content, "{{320, 0}, {180, 90}}");
    const auto core = b.declare("_NSCollectionViewCore");
    const auto flow = b.declare("NSCollectionViewFlowLayout");
    b.ref(collection, "NSCollectionViewCore", core);
    b.boolean(collection, "NSSelectable", true);
    b.ref(core, "UICollectionLayout", flow);
    n.subviews.push_back(collection);
    n.outlet(collection, "collection");
    // NSComboTableView (an NSTableView in the archive).
    const auto combo_table = b.view("NSComboTableView", n.content, "{{360, 100}, {100, 60}}");
    b.ref(combo_table, "NSTableColumns", b.array("NSMutableArray", {}));
    n.subviews.push_back(combo_table);
    n.outlet(combo_table, "combo");
    // A pan gesture recognizer and a text tab as top-level objects.
    const auto pan = b.declare("NSPanGestureRecognizer");
    b.integer(pan, "NSGestureRecognizer.allowedTouchTypes", 1);
    b.ref(pan, "NSGestureRecognizer.action", b.string("panned:"));
    b.ref(pan, "NSGestureRecognizer.target", n.owner);
    b.integer(pan, "NSPanGestureRecognizer.buttonMask", 2);
    b.integer(pan, "NSPanGestureRecognizer.numberOfTouchesRequired", 1);
    n.objects.push_back(pan);
    n.outlet(pan, "pan");
    const auto tab = b.declare("NSTextTab");
    b.real(tab, "NSLocation", 56);
    n.objects.push_back(tab);
    n.outlet(tab, "tab");
    // Keys real applications archive (the frameworks wave, U4).
    // A push-on-push-off button: NSAuxButtonType, periodic delay and interval
    // (milliseconds), and the control's copies of the cell settings.
    const auto toggle = b.view("NSButton", n.content, "{{300, 10}, {80, 24}}");
    const auto toggle_cell = b.declare("NSButtonCell");
    b.ref(toggle, "NSCell", toggle_cell);
    b.boolean(toggle, "NSEnabled", true);
    b.integer(toggle, "NSControlSendActionMask", 2);
    b.integer(toggle, "NSControlLineBreakMode", 4);
    b.boolean(toggle, "NSControlRefusesFirstResponder", true);
    b.boolean(toggle, "NSControlContinuous", true);   // the complement of the cell's flag
    b.integer(toggle_cell, "NSCellFlags", 0x04000000);
    b.ref(toggle_cell, "NSContents", b.string("Toggle"));
    b.integer(toggle_cell, "NSAuxButtonType", 1);
    b.integer(toggle_cell, "NSPeriodicDelay", 300);
    b.integer(toggle_cell, "NSPeriodicInterval", 50);
    b.ref(toggle_cell, "NSControlView", toggle);
    n.subviews.push_back(toggle);
    n.outlet(toggle, "toggle");
    // A continuous stepper (NSCellFlags 0x40000); the control's range keys are zeros.
    const auto stepper = b.view("NSStepper", n.content, "{{390, 10}, {19, 28}}");
    const auto stepper_cell = b.declare("NSStepperCell");
    b.ref(stepper, "NSCell", stepper_cell);
    b.boolean(stepper, "NSControlContinuous", false);
    b.real(stepper, "NSStepperMaxValue", 0);
    b.integer(stepper_cell, "NSCellFlags", 0x04040000);
    b.real(stepper_cell, "NSMaxValue", 10);
    b.ref(stepper_cell, "NSControlView", stepper);
    n.subviews.push_back(stepper);
    n.outlet(stepper, "stepper");
    // A pop-up button that leaves item states alone.
    const auto popup = b.view("NSPopUpButton", n.content, "{{300, 40}, {120, 26}}");
    const auto popup_cell = b.declare("NSPopUpButtonCell");
    b.ref(popup, "NSCell", popup_cell);
    b.integer(popup_cell, "NSCellFlags", 0x04000000);
    b.boolean(popup_cell, "NSAltersState", false);
    b.boolean(popup_cell, "NSUsesItemFromMenu", false);
    b.integer(popup_cell, "NSArrowPosition", 1);
    b.integer(popup_cell, "NSPreferredEdge", 3);
    b.ref(popup_cell, "NSControlView", popup);
    n.subviews.push_back(popup);
    n.outlet(popup, "popup");
    // An image view aligned right, scaled to fill.
    const auto image_view = b.view("NSImageView", n.content, "{{300, 70}, {48, 48}}");
    const auto image_cell = b.declare("NSImageCell");
    b.ref(image_view, "NSCell", image_cell);
    b.integer(image_cell, "NSCellFlags", 0x08000000);
    b.integer(image_cell, "NSAlign", 8);
    b.integer(image_cell, "NSScale", 1);
    b.integer(image_cell, "NSStyle", 0);
    b.ref(image_cell, "NSControlView", image_view);
    n.subviews.push_back(image_view);
    n.outlet(image_view, "imageView");
    // A transparent box with its content margins.
    const auto box = b.view("NSBox", n.content, "{{360, 70}, {100, 60}}");
    b.boolean(box, "NSTransparent", true);
    b.ref(box, "NSOffsets", b.string("{3, 4}"));
    n.subviews.push_back(box);
    n.outlet(box, "box");
    // A text field's preferred layout width.
    const auto field = b.view("NSTextField", n.content, "{{300, 130}, {120, 22}}");
    const auto field_cell = b.declare("NSTextFieldCell");
    b.ref(field, "NSCell", field_cell);
    b.real(field, "NSPreferredMaxLayoutWidth", 250);
    b.integer(field_cell, "NSCellFlags", 0x04000000);
    b.ref(field_cell, "NSContents", b.string("Label"));
    b.ref(field_cell, "NSControlView", field);
    n.subviews.push_back(field);
    n.outlet(field, "field");
    // A table: grid, style, row heights, type-select, highlight style, the
    // control's action, and a column with a header tool tip from before NSResizingMask.
    const auto table = b.view("NSTableView", n.content, "{{300, 160}, {150, 80}}");
    const auto column = b.declare("NSTableColumn");
    b.ref(column, "NSIdentifier", b.string("name"));
    b.ref(column, "NSHeaderToolTip", b.string("The cue name"));
    b.boolean(column, "NSIsResizeable", false);
    b.ref(table, "NSTableColumns", b.array("NSMutableArray", {column}));
    const auto grid = b.declare("NSColor");
    b.integer(grid, "NSColorSpace", 1);
    {
        nib::Value rgb;
        rgb.type = nib::ValueType::data;
        const std::string components = "1 0 0";
        rgb.data.assign(components.begin(), components.end());
        rgb.data.push_back(0);
        b.put(grid, "NSRGB", rgb);
    }
    b.ref(table, "NSGridColor", grid);
    b.integer(table, "NSGridStyleMask", 2);
    b.integer(table, "NSTableViewStyle", 4);
    b.boolean(table, "NSTableViewUseARH", true);
    b.boolean(table, "NSAllowsTypeSelect", false);
    b.integer(table, "NSTableViewSelectionHighlightStyle", -1);
    b.ref(table, "NSControlAction", b.string("tableClicked:"));
    n.subviews.push_back(table);
    n.outlet(table, "table");
    n.objects.push_back(column);
    n.outlet(column, "column");
    // A scroll view's content insets and magnification bounds, a scroller's value.
    const auto scroll = b.view("NSScrollView", n.content, "{{10, 200}, {120, 80}}");
    b.ref(scroll, "NSContentInsets", b.string("{1, 2, 3, 4}"));
    b.real(scroll, "NSMaxMagnification", 8);
    b.real(scroll, "NSMagnification", 1);
    n.subviews.push_back(scroll);
    n.outlet(scroll, "scroll");
    const auto bar = b.view("NSScroller", n.content, "{{140, 200}, {15, 80}}");
    b.real(bar, "NSCurValue", 0.5);
    b.real(bar, "NSPercent", 0.25);
    n.subviews.push_back(bar);
    n.outlet(bar, "scroller");
    // A menu without a state column, an item with an identifier.
    const auto menu = b.declare("NSMenu");
    const auto item = b.declare("NSMenuItem");
    b.ref(menu, "NSTitle", b.string("Actions"));
    b.boolean(menu, "NSMenuExcludeMarkColumn", true);
    b.ref(item, "NSTitle", b.string("Start"));
    b.ref(item, "NSMenuItemIdentifier", b.string("Start"));
    b.ref(menu, "NSMenuItems", b.array("NSMutableArray", {item}));
    n.objects.push_back(menu);
    n.outlet(menu, "menu");
    const auto bytes = n.encode();
    auto outlets = std::vector<std::string>{"segments", "path", "search", "picker", "text", "collection", "combo", "pan", "tab", "toggle", "stepper", "popup", "imageView",
                                             "box", "field", "table", "column", "scroll", "scroller", "menu"};
    const auto made = load_controls_nib(f, bytes, outlets);
    // Keys real applications archive (U4).
    const auto toggle_control = made.at("toggle");
    const auto toggle_button_cell = f.id(toggle_control, "cell");
    require(f.id(toggle_button_cell, "_buttonType") == 1 && f.id(toggle_button_cell, "showsStateBy") == 12 &&
                f.integer(toggle_button_cell, "lineBreakMode") == 4 && !f.yes(toggle_button_cell, "isContinuous") &&
                !f.yes(toggle_control, "acceptsFirstResponder"),
            "decoded button type and control settings");
    {
        const auto delay = f.process.allocate_guest(8);
        static_cast<void>(f.id(toggle_button_cell, "getPeriodicDelay:interval:", {delay, delay + 4}));
        const auto as_float = [&](Address at) { return std::bit_cast<float>(static_cast<std::uint32_t>(f.engine.read64(at) & 0xffffffff)); };
        require(std::abs(as_float(delay) - 0.3f) < 1e-6 && std::abs(as_float(delay + 4) - 0.05f) < 1e-6, "decoded periodic delay and interval");
    }
    require(f.yes(f.id(made.at("stepper"), "cell"), "isContinuous") && f.send(made.at("stepper"), "maxValue").d(0) == 10,
            "decoded continuous stepper (the cell's range)");
    const auto popup_control = f.id(made.at("popup"), "cell");
    require(!f.yes(popup_control, "altersStateOfSelectedItem") && !f.yes(popup_control, "usesItemFromMenu") &&
                f.integer(popup_control, "arrowPosition") == 1 && f.integer(popup_control, "preferredEdge") == 3,
            "decoded pop-up settings");
    require(f.integer(made.at("imageView"), "imageAlignment") == 8 && f.integer(made.at("imageView"), "imageScaling") == 1, "decoded image cell");
    {
        const auto margins = f.send(made.at("box"), "contentViewMargins");
        require(f.yes(made.at("box"), "isTransparent") && margins.d(0) == 3 && margins.d(1) == 4, "decoded transparent box and margins");
    }
    require(f.send(made.at("field"), "preferredMaxLayoutWidth").d(0) == 250, "decoded preferred layout width");
    const auto table_view = made.at("table");
    require(f.integer(table_view, "gridStyleMask") == 2 && f.integer(table_view, "style") == 4 && f.yes(table_view, "usesAutomaticRowHeights") &&
                !f.yes(table_view, "allowsTypeSelect") && f.integer(table_view, "selectionHighlightStyle") == -1 &&
                f.id(table_view, "action") == f.objc.selector("tableClicked:"),
            "decoded table settings");
    {
        const auto rgba = f.process.allocate_guest(32);
        static_cast<void>(f.id(f.id(table_view, "gridColor"), "getRed:green:blue:alpha:", {rgba, rgba + 8, rgba + 16, rgba + 24}));
        require(std::bit_cast<double>(f.engine.read64(rgba)) > 0.99 && std::bit_cast<double>(f.engine.read64(rgba + 8)) < 0.01, "decoded grid color");
    }
    require(f.text(f.id(made.at("column"), "headerToolTip")) == "The cue name" && f.integer(made.at("column"), "resizingMask") == 0,
            "decoded column tool tip and NSIsResizeable");
    {
        const auto insets = f.send(made.at("scroll"), "contentInsets");
        require(insets.d(0) == 1 && insets.d(1) == 2 && insets.d(2) == 3 && insets.d(3) == 4 && f.send(made.at("scroll"), "maxMagnification").d(0) == 8,
                "decoded content insets and magnification bounds");
    }
    require(f.send(made.at("scroller"), "doubleValue").d(0) == 0.5 && f.send(made.at("scroller"), "knobProportion").d(0) == 0.25, "decoded scroller");
    require(!f.yes(made.at("menu"), "showsStateColumn"), "decoded menu without a state column");
    // A pop-up that alters item states marks the selected item on and the previous one off.
    {
        const auto chooser = f.with_rect(f.id(f.cls("NSPopUpButton"), "alloc"), "initWithFrame:", {{0, 0}, {120, 26}});
        for (const auto* title : {"One", "Two", "Three"}) static_cast<void>(f.id(chooser, "addItemWithTitle:", {f.str(title)}));
        static_cast<void>(f.id(chooser, "selectItemAtIndex:", {1}));
        static_cast<void>(f.id(chooser, "selectItemAtIndex:", {2}));
        const auto state_of = [&](Address index) { return f.integer(f.id(chooser, "itemAtIndex:", {index}), "state"); };
        require(state_of(1) == 0 && state_of(2) == 1, "a pop-up alters the selected item's state");
        static_cast<void>(f.id(f.id(chooser, "cell"), "setAltersStateOfSelectedItem:", {0}));
        static_cast<void>(f.id(chooser, "selectItemAtIndex:", {0}));
        require(state_of(0) == 0 && state_of(2) == 1, "without -altersStateOfSelectedItem item states stay");
    }
    // Search field.
    const auto search_field = made.at("search");
    require(f.id(f.id(search_field, "cell"), "class") == f.cls("NSSearchFieldCell") && f.integer(search_field, "maximumRecents") == 255 &&
                f.text(f.id(search_field, "placeholderString")) == "Search" && f.id(f.id(search_field, "cell"), "cancelButtonCell") != 0,
            "decoded search field");
    // Date picker: the current date, year-month-day elements.
    const auto date_picker = made.at("picker");
    const auto now = alchemy::CoreFoundation::absolute_time_now();
    const auto shown = f.send(f.id(date_picker, "dateValue"), "timeIntervalSinceReferenceDate").d(0);
    require(std::abs(shown - now) < 60 && f.id(date_picker, "datePickerElements") == 224, "decoded date picker");
    // Text view: the storage's text and font, the shared data's flags, the network.
    const auto text_view = made.at("text");
    const auto text_storage = f.id(text_view, "textStorage");
    require(f.text(f.id(text_view, "string")) == "Notes go here" && text_storage && f.text(f.id(text_storage, "string")) == "Notes go here",
            "decoded text view text");
    require(f.id(text_storage, "class") == f.cls("NSTextStorage") && f.id(f.id(text_view, "layoutManager"), "textStorage") == text_storage &&
                f.id(f.id(text_view, "textContainer"), "textView") == text_view, "decoded text network");
    require(f.yes(text_view, "isEditable") && f.yes(text_view, "isRichText") && f.send(f.id(text_view, "font"), "pointSize").d(0) == 15,
            "decoded text view settings");
    // Collection view: the archived layout.
    const auto collection_view = made.at("collection");
    require(f.id(f.id(collection_view, "collectionViewLayout"), "class") == f.cls("NSCollectionViewFlowLayout") && f.yes(collection_view, "isSelectable"),
            "decoded collection view");
    require(f.id(made.at("combo"), "class") == f.cls("NSComboTableView"), "decoded combo table view");
    // Gesture recognizer and text tab.
    const auto recognizer = made.at("pan");
    require(f.id(recognizer, "action") == f.objc.selector("panned:") && f.id(recognizer, "buttonMask") == 2 && f.id(recognizer, "target") != 0,
            "decoded pan gesture recognizer");
    require(f.send(made.at("tab"), "location").d(0) == 56 && f.integer(made.at("tab"), "alignment") == 0, "decoded text tab");
    const auto control = made.at("segments");
    require(f.integer(control, "segmentCount") == 2 && f.text(f.id(control, "labelForSegment:", {1})) == "Carts", "decoded segments");
    require(f.integer(control, "selectedSegment") == 0 && f.integer(control, "tagForSegment:", {1}) == 1 &&
                f.integer(control, "segmentDistribution") == 2 && f.integer(control, "segmentStyle") == 1, "decoded segment state");
    require(f.integer(f.id(control, "cell"), "controlSize") == 1, "decoded control size");
    const auto path_control = made.at("path");
    const auto components = f.id(path_control, "pathComponentCells");
    require(f.integer(components, "count") == 2 && f.text(f.id(f.id(components, "objectAtIndex:", {1}), "stringValue")) == "Applications",
            "decoded path components");
    require(f.id(f.id(path_control, "cell"), "delegate") == path_control, "decoded path cell delegate");
    // The decoded window draws (the selected segment in the accent color).
    const auto window = f.id(control, "window");
    require(window != 0, "the decoded controls are in the nib's window");
    static_cast<void>(f.next_event());
    const auto lit = f.server.pixel(f.id(window, "windowNumber"), 8 + 20, 7 + 10);
    require(is_accent(lit), "the decoded selected segment is drawn in the accent color: " + bgra_text(lit));
    static_cast<void>(f.id(window, "close"));
}

// ---- NSSearchField -------------------------------------------------------------------------
void search_fields(Fixture& f) {
    journal.clear();
    f.describe = [&f](Address sender) { return f.text(f.id(sender, "stringValue")); };
    const auto target = f.target("search");
    const auto field = f.with_rect(f.id(f.cls("NSSearchField"), "alloc"), "initWithFrame:", {{20, 20}, {200, 22}});
    require(f.id(f.id(field, "cell"), "class") == f.cls("NSSearchFieldCell"), "search field cell class");
    static_cast<void>(f.id(field, "setTarget:", {target}));
    static_cast<void>(f.id(field, "setAction:", {f.objc.selector("acted:")}));
    static_cast<void>(f.id(field, "setPlaceholderString:", {f.str("Filter")}));
    static_cast<void>(f.id(field, "setSendsSearchStringImmediately:", {1}));
    require(f.yes(field, "sendsSearchStringImmediately") && !f.yes(field, "sendsWholeSearchString"), "search modes");
    const auto window = f.make_window({{0, 0}, {260, 60}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {field}));
    f.show(window);
    require(marked(f, window, {{24, 23}, {18, 16}}) > 6, "the search button (magnifier) was drawn");
    require(marked(f, window, {{204, 27}, {8, 8}}) < 4, "no cancel button without text");
    // Typing searches at once.
    f.click(window, {120, 31});
    const auto editor = f.id(field, "currentEditor");
    require(editor != 0, "clicking the text area starts editing");
    const auto editor_frame = f.send(editor, "frame");
    require(editor_frame.d(0) >= 20, "the field editor sits beside the search button");
    static_cast<void>(f.id(editor, "insertText:", {f.str("ab")}));
    require(log_has("search:ab"), "the immediate search: " + joined());
    const auto recents = f.id(field, "recentSearches");
    require(f.integer(recents, "count") == 1 && f.text(f.id(recents, "objectAtIndex:", {0})) == "ab", "recent searches");
    require(marked(f, window, {{204, 27}, {8, 8}}) > 6, "the cancel button appears with text");
    // The cancel button clears the field and searches again.
    journal.clear();
    f.click(window, {208, 31});
    require(log_has("search:") && f.text(f.id(field, "stringValue")).empty(), "the cancel button: " + joined());
    // Whole-string searches wait for Return.
    journal.clear();
    static_cast<void>(f.id(field, "setSendsSearchStringImmediately:", {0}));
    static_cast<void>(f.id(field, "setSendsWholeSearchString:", {1}));
    const auto editing = f.id(field, "currentEditor");
    require(editing != 0, "still editing");
    static_cast<void>(f.id(editing, "insertText:", {f.str("cd")}));
    require(journal.empty(), "a whole-string search was sent while typing");
    static_cast<void>(f.id(editing, "insertNewline:", {0}));
    require(log_has("search:cd"), "Return sends the whole search string: " + joined());
    static_cast<void>(f.id(window, "close"));
}

// ---- NSDatePicker --------------------------------------------------------------------------
void date_pickers(Fixture& f) {
    journal.clear();
    f.describe = [&f](Address sender) { return std::to_string(std::llround(f.seconds(f.id(sender, "dateValue")))); };
    const auto target = f.target("date");
    const auto picker = f.with_rect(f.id(f.cls("NSDatePicker"), "alloc"), "initWithFrame:", {{20, 20}, {160, 24}});
    static_cast<void>(f.id(picker, "setDatePickerElements:", {0xe0}));
    static_cast<void>(f.id(picker, "setTarget:", {target}));
    static_cast<void>(f.id(picker, "setAction:", {f.objc.selector("acted:")}));
    // 2026-10-14 12:00 UTC: the 14th in any zone within 12 hours of GMT.
    const double noon = alchemy::gui::completion_kit::dates::reference_seconds({2026, 10, 14, 12, 0, 0}, 0);
    static_cast<void>(f.id(picker, "setDateValue:", {f.date(noon)}));
    require(f.seconds(f.id(picker, "dateValue")) == noon, "-dateValue");
    const auto window = f.make_window({{0, 0}, {240, 60}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {picker}));
    f.show(window);
    require(inked(f, window, {{20, 20}, {140, 24}}) > 20, "the date was drawn");
    // A click on the day field (after "10/"), then the up arrow: one day later.
    alchemy::gui::appkit::drawing::FontSpec font;
    const auto day_x = 20 + 4 + alchemy::gui::appkit::drawing::text_width(font, u"10/1");
    f.click(window, {day_x, 32});
    require(f.id(window, "firstResponder") == picker, "a click makes the picker first responder");
    f.key(window, 0x7e, "");
    require(f.seconds(f.id(picker, "dateValue")) == noon + 86400 && log_has("date:" + std::to_string(std::llround(noon + 86400))),
            "the up arrow steps the day: " + joined());
    // Typed digits replace the field: the 20th.
    f.key(window, 0x13, "2");
    f.key(window, 0x1d, "0");
    require(f.seconds(f.id(picker, "dateValue")) == noon + 6 * 86400, "typed day 20");
    // The maximum date holds.
    static_cast<void>(f.id(picker, "setMaxDate:", {f.date(noon + 6 * 86400)}));
    f.key(window, 0x7e, "");
    require(f.seconds(f.id(picker, "dateValue")) == noon + 6 * 86400, "the maximum date clamps");
    // The graphical style is not provided.
    bool threw = false;
    try { static_cast<void>(f.id(picker, "setDatePickerStyle:", {1})); } catch (const std::exception&) { threw = true; }
    require(threw, "the calendar style did not fail explicitly");
    static_cast<void>(f.id(window, "close"));
}

// ---- NSColorWell ---------------------------------------------------------------------------
void color_wells(Fixture& f) {
    const auto well = f.with_rect(f.id(f.cls("NSColorWell"), "alloc"), "initWithFrame:", {{20, 20}, {44, 24}});
    static_cast<void>(f.id(well, "setColor:", {f.id(f.cls("NSColor"), "redColor")}));
    const auto window = f.make_window({{0, 0}, {100, 60}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {well}));
    f.show(window);
    const auto inside = f.pixel(window, 42, 32);
    require(inside[2] > 200 && inside[1] < 60 && inside[0] < 60, "the well shows its colour: " + bgra_text(inside));
    const auto other = f.with_rect(f.id(f.cls("NSColorWell"), "alloc"), "initWithFrame:", {{0, 0}, {10, 10}});
    static_cast<void>(f.id(other, "setColor:", {f.id(f.cls("NSColor"), "blueColor")}));
    static_cast<void>(f.id(well, "takeColorFrom:", {other}));
    static_cast<void>(f.id(well, "setBordered:", {0}));
    const auto corner = f.pixel(window, 21, 21);
    require(corner[0] > 200 && corner[2] < 60, "a borderless well fills its bounds with the colour: " + bgra_text(corner));
    require(!f.yes(well, "isActive"), "inactive well");
    bool threw = false;
    try { static_cast<void>(f.id(well, "activate:", {1})); } catch (const std::exception&) { threw = true; }
    require(threw, "activating a well (the colour panel) did not fail explicitly");
    static_cast<void>(f.id(window, "close"));
}

// ---- the text system ------------------------------------------------------------------------
void text_system(Fixture& f) {
    const auto view = f.with_rect(f.id(f.cls("NSTextView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 100}});
    static_cast<void>(f.id(view, "setString:", {f.str("hello world")}));
    const auto storage = f.id(view, "textStorage");
    require(storage && f.id(storage, "class") == f.cls("NSTextStorage") && f.text(f.id(storage, "string")) == "hello world", "the view's storage");
    const auto manager = f.id(view, "layoutManager");
    require(f.id(manager, "textStorage") == storage && f.integer(f.id(manager, "textContainers"), "count") == 1 &&
                f.id(f.id(view, "textContainer"), "layoutManager") == manager && f.id(manager, "firstTextView") == view,
            "the text network");
    // Storage edits reach the view; view edits reach the storage.
    static_cast<void>(f.id(storage, "replaceCharactersInRange:withString:", {0, 5, f.str("HELLO")}));
    require(f.text(f.id(view, "string")) == "HELLO world", "a storage edit shows in the view");
    static_cast<void>(f.id(view, "setSelectedRange:", {11, 0}));
    static_cast<void>(f.id(view, "insertText:", {f.str("!")}));
    require(f.text(f.id(storage, "string")) == "HELLO world!", "a typed character reaches the storage");
    // The storage's first font is the view's.
    const auto big = f.with_double(f.cls("NSFont"), "systemFontOfSize:", 20);
    static_cast<void>(f.id(storage, "addAttribute:value:range:", {f.str("NSFont"), big, 0, 12}));
    require(f.send(f.id(view, "font"), "pointSize").d(0) == 20, "the view follows the storage's font");
    // Temporary attributes follow edits.
    const auto yellow = f.id(f.cls("NSColor"), "yellowColor");
    static_cast<void>(f.id(manager, "addTemporaryAttribute:value:forCharacterRange:", {f.str("NSBackgroundColor"), yellow, 6, 5}));
    const auto range = f.process.allocate_guest(16);
    auto attributes = f.id(manager, "temporaryAttributesAtCharacterIndex:effectiveRange:", {7, range});
    require(f.integer(attributes, "count") == 1 && f.engine.read64(range) == 6 && f.engine.read64(range + 8) == 5, "a temporary attribute run");
    static_cast<void>(f.id(storage, "replaceCharactersInRange:withString:", {0, 0, f.str("Oh ")}));
    attributes = f.id(manager, "temporaryAttributesAtCharacterIndex:effectiveRange:", {10, range});
    require(f.integer(attributes, "count") == 1 && f.engine.read64(range) == 9 && f.engine.read64(range + 8) == 5, "the run moved with the edit");
    static_cast<void>(f.id(manager, "removeTemporaryAttribute:forCharacterRange:", {f.str("NSBackgroundColor"), 0, 15}));
    require(f.integer(f.id(manager, "temporaryAttributesAtCharacterIndex:effectiveRange:", {10, range}), "count") == 0, "removed temporary attribute");
    // Word and paragraph selection.
    auto chosen = f.send(view, "selectionRangeForProposedRange:granularity:", [] { CallRegs r; r.x[2] = 9; r.x[3] = 0; r.x[4] = 1; return r; }());
    require(chosen.x0 == 9 && chosen.x1 == 5, "word granularity: " + std::to_string(chosen.x0) + "+" + std::to_string(chosen.x1));
    static_cast<void>(f.id(view, "setString:", {f.str("line one\nline two")}));
    chosen = f.send(view, "selectionRangeForProposedRange:granularity:", [] { CallRegs r; r.x[2] = 12; r.x[3] = 0; r.x[4] = 2; return r; }());
    require(chosen.x0 == 9 && chosen.x1 == 8, "paragraph granularity");
    // Line breaks.
    static_cast<void>(f.id(view, "setString:", {f.str("ab")}));
    static_cast<void>(f.id(view, "setSelectedRange:", {1, 0}));
    static_cast<void>(f.id(view, "insertLineBreak:", {0}));
    const auto units = f.cf.units(f.id(view, "string"));
    require(units.size() == 3 && units[1] == 0x2028, "-insertLineBreak: puts a line separator");
    // Typing attributes colour the typed text.
    const auto red = f.id(f.cls("NSColor"), "redColor");
    const auto typing = f.cf.dictionary(std::vector<std::pair<Address, Address>>{{f.str("NSColor"), red}});
    static_cast<void>(f.id(view, "setTypingAttributes:", {typing}));
    require(f.id(f.id(view, "typingAttributes"), "objectForKey:", {f.str("NSColor")}) == red, "-typingAttributes");
    static_cast<void>(f.id(view, "setSelectedRange:", {3, 0}));
    static_cast<void>(f.id(view, "insertText:", {f.str("x")}));
    require(f.id(storage, "attribute:atIndex:effectiveRange:", {f.str("NSColor"), 3, 0}) == red, "typed text takes the typing attributes");
    // Settings kept for the view.
    static_cast<void>(f.id(view, "setInsertionPointColor:", {red}));
    static_cast<void>(f.id(view, "setEnabledTextCheckingTypes:", {4}));
    static_cast<void>(f.id(view, "setSelectedTextAttributes:", {typing}));
    require(f.id(view, "insertionPointColor") == red && f.id(view, "enabledTextCheckingTypes") == 4 &&
                f.id(f.id(view, "selectedTextAttributes"), "objectForKey:", {f.str("NSColor")}) == red, "text view settings");
    // Selected ranges (with NSValue ranges when Foundation has them).
    if (f.objc.responds(f.cls("NSValue"), f.objc.selector("valueWithRange:"))) {
        const auto first = f.id(f.cls("NSValue"), "valueWithRange:", {0, 1});
        const auto second = f.id(f.cls("NSValue"), "valueWithRange:", {2, 2});
        static_cast<void>(f.id(view, "setSelectedRanges:", {f.cf.array(std::vector<Address>{first, second})}));
        const auto ranges = f.id(view, "selectedRanges");
        require(f.integer(ranges, "count") == 2 && f.send(view, "selectedRange").x0 == 0 && f.send(view, "selectedRange").x1 == 1, "selected ranges");
    }
}

// ---- NSLayoutManager over CoreText and the TextKit long tail (frameworks wave U11) ------------
void text_layout(Fixture& f) {
    const auto rect_of = [](const CallResult& r) { return gui::Rect{{r.d(0), r.d(1)}, {r.d(2), r.d(3)}}; };
    const auto glyph_query = [&f](Address manager, std::string_view selector, std::uint64_t index, Address range = 0) {
        CallRegs r;
        r.x[2] = index;
        r.x[3] = range;
        r.x[4] = 0;
        return f.send(manager, selector, r);
    };
    // A storage in two fonts, laid out by a manager in one container without a view.
    const auto storage = f.id(f.id(f.cls("NSTextStorage"), "alloc"), "initWithString:", {f.str("Hello world\nSecond line")});
    const auto small = f.with_double(f.cls("NSFont"), "systemFontOfSize:", 12), large = f.with_double(f.cls("NSFont"), "systemFontOfSize:", 24);
    static_cast<void>(f.id(storage, "addAttribute:value:range:", {f.str("NSFont"), small, 0, 12}));
    static_cast<void>(f.id(storage, "addAttribute:value:range:", {f.str("NSFont"), large, 12, 11}));
    const auto manager = f.id(f.id(f.cls("NSLayoutManager"), "alloc"), "init");
    CallRegs size;
    gui::appkit::put_size(size, 0, {200, 1e7});
    const auto container = f.send(f.id(f.cls("NSTextContainer"), "alloc"), "initWithSize:", size).x0;
    static_cast<void>(f.id(manager, "addTextContainer:", {container}));
    static_cast<void>(f.id(storage, "addLayoutManager:", {manager}));
    require(f.integer(manager, "numberOfGlyphs") == 23, "a glyph per character: " + std::to_string(f.integer(manager, "numberOfGlyphs")));
    require(static_cast<std::uint32_t>(f.integer(manager, "glyphAtIndex:", {11})) == 0x00FFFFFF && f.integer(manager, "glyphAtIndex:", {0}) != 0,
            "the line separator is a control glyph, letters are glyphs");
    require(f.integer(manager, "characterIndexForGlyphAtIndex:", {5}) == 5 && f.integer(manager, "glyphIndexForCharacterAtIndex:", {12}) == 12,
            "glyph <-> character");
    const auto range = f.process.allocate_guest(16);
    const auto first = rect_of(glyph_query(manager, "lineFragmentRectForGlyphAtIndex:effectiveRange:", 0, range));
    require(first.origin.y == 0 && first.size.width == 200 && f.engine.read64(range) == 0 && f.engine.read64(range + 8) == 12,
            "the first line fragment spans the container and holds 'Hello world\\n'");
    const auto second = rect_of(glyph_query(manager, "lineFragmentRectForGlyphAtIndex:effectiveRange:", 12, range));
    const auto second_start = f.engine.read64(range);
    const auto first_used = rect_of(glyph_query(manager, "lineFragmentUsedRectForGlyphAtIndex:effectiveRange:", 0, 0));
    const auto second_used = rect_of(glyph_query(manager, "lineFragmentUsedRectForGlyphAtIndex:effectiveRange:", 12, 0));
    require(second.origin.y >= first.origin.y + first.size.height - 0.01 && second_used.size.height > first_used.size.height && second_start == 12,
            "the second line, in the larger font, is below and taller: first used h " + std::to_string(first_used.size.height) + " fragment h " +
                std::to_string(first.size.height) + ", second y " + std::to_string(second.origin.y) + " used h " + std::to_string(second_used.size.height));
    const auto used = rect_of(f.send(manager, "usedRectForTextContainer:", [&] { CallRegs r; r.x[2] = container; return r; }()));
    require(used.size.width > 20 && used.size.width < 200 && std::abs(used.size.height - (second.origin.y + second.size.height)) < 0.01,
            "the used rectangle is the text's");
    const auto location = f.send(manager, "locationForGlyphAtIndex:", [] { CallRegs r; r.x[2] = 1; return r; }());
    require(location.d(0) > 5 && location.d(1) > 0, "a glyph's location is after the padding and the first letter, on the baseline");
    const auto word = rect_of(f.send(manager, "boundingRectForGlyphRange:inTextContainer:", [&] { CallRegs r; r.x[2] = 6; r.x[3] = 5; r.x[4] = container; return r; }()));
    require(word.origin.x > location.d(0) && word.size.width > 10 && word.origin.y == 0, "the bounding rectangle of 'world'");
    CallRegs point;
    point.set_d(0, word.origin.x + 1);
    point.set_d(1, 5);
    point.x[2] = container;
    require(f.send(manager, "glyphIndexForPoint:inTextContainer:", point).x0 == 6, "the glyph under a point");
    point.x[3] = 0;
    require(f.send(manager, "characterIndexForPoint:inTextContainer:fractionOfDistanceBetweenInsertionPoints:", point).x0 == 6, "the character under a point");
    require(rect_of(f.send(manager, "extraLineFragmentRect")).size.height == 0, "no extra line fragment after text");
    static_cast<void>(f.id(storage, "replaceCharactersInRange:withString:", {23, 0, f.str("\n")}));
    const auto extra = rect_of(f.send(manager, "extraLineFragmentRect"));
    require(extra.size.height > 0 && std::abs(extra.origin.y - (second.origin.y + second.size.height)) < 0.5 && f.id(manager, "extraLineFragmentTextContainer") == container,
            "text ending in a newline has an extra line fragment below: y " + std::to_string(extra.origin.y) + " h " + std::to_string(extra.size.height) +
                " second bottom " + std::to_string(second.origin.y + second.size.height) + " glyphs " + std::to_string(f.integer(manager, "numberOfGlyphs")) +
                " unlaid " + std::to_string(f.integer(manager, "firstUnlaidCharacterIndex")));
    // Glyph arrays.
    const auto glyphs = f.process.allocate_guest(64), characters = f.process.allocate_guest(128);
    CallRegs get;
    get.x[2] = 10; get.x[3] = 4; get.x[4] = glyphs; get.x[5] = 0; get.x[6] = characters; get.x[7] = 0;
    require(f.send(manager, "getGlyphsInRange:glyphs:properties:characterIndexes:bidiLevels:", get).x0 == 4 && f.engine.read64(characters + 8) == 11 &&
                f.engine.read64(characters + 16) == 12,
            "-getGlyphsInRange:... characters");
    // Wrapping, enumeration and the line limit.
    static_cast<void>(f.send(container, "setSize:", [] { CallRegs r; gui::appkit::put_size(r, 0, {50, 1e7}); return r; }()));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> fragments;
    const auto each_line = f.block("u11_lines", [&f, &fragments](CpuState& s, GuestMemory&) { fragments.emplace_back(s.x[2], s.x[3]); static_cast<void>(f); });
    static_cast<void>(f.id(manager, "enumerateLineFragmentsForGlyphRange:usingBlock:", {0, 24, each_line}));
    require(fragments.size() >= 4 && fragments.front().first == 0, "a narrow container wraps the text into more lines");
    static_cast<void>(f.id(container, "setMaximumNumberOfLines:", {1}));
    require(f.integer(manager, "firstUnlaidCharacterIndex") < 12 && f.integer(container, "maximumNumberOfLines") == 1, "a line limit leaves text unlaid");
    static_cast<void>(f.id(container, "setMaximumNumberOfLines:", {0}));
    // A second container takes the text the first cannot hold.
    static_cast<void>(f.send(container, "setSize:", [&] { CallRegs r; gui::appkit::put_size(r, 0, {200, first.size.height}); return r; }()));
    const auto overflow = f.send(f.id(f.cls("NSTextContainer"), "alloc"), "initWithSize:", size).x0;
    static_cast<void>(f.id(manager, "addTextContainer:", {overflow}));
    const auto second_range = f.send(manager, "glyphRangeForTextContainer:", [&] { CallRegs r; r.x[2] = overflow; return r; }());
    require(second_range.x0 == 12 && f.id(manager, "textContainerForGlyphAtIndex:effectiveRange:", {13, 0}) == overflow,
            "the second container continues the text");
    // Containers and the explicit stops.
    point.set_d(0, 10);
    point.set_d(1, 1);
    require((f.send(container, "containsPoint:", point).x0 & 0xff) != 0, "a point inside the container");
    bool stopped{};
    try { static_cast<void>(f.id(manager, "setTypesetter:", {0})); } catch (const std::exception&) { stopped = true; }
    require(stopped, "custom typesetters stop explicitly");
    static_cast<void>(f.id(container, "setExclusionPaths:", {f.cf.array(std::vector<Address>{f.str("not a path")})}));
    stopped = false;
    try { static_cast<void>(f.integer(manager, "numberOfGlyphs")); } catch (const std::exception&) { stopped = true; }
    require(stopped, "exclusion paths stop explicitly");
    static_cast<void>(f.id(container, "setExclusionPaths:", {0}));

    // Drawing: a view that draws the glyphs through the manager.
    const auto drawn = f.id(f.id(f.cls("NSTextStorage"), "alloc"), "initWithString:", {f.str("MMMM")});
    static_cast<void>(f.id(drawn, "addAttribute:value:range:", {f.str("NSFont"), large, 0, 4}));
    const auto painter = f.id(f.id(f.cls("NSLayoutManager"), "alloc"), "init");
    const auto box = f.send(f.id(f.cls("NSTextContainer"), "alloc"), "initWithSize:", size).x0;
    static_cast<void>(f.id(painter, "addTextContainer:", {box}));
    static_cast<void>(f.id(drawn, "addLayoutManager:", {painter}));
    const auto canvas_class = f.subclass("U11GlyphView", "NSView");
    f.add(canvas_class, "isFlipped", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    f.add(canvas_class, "drawRect:", [&f, painter](CpuState&, GuestMemory&) {
        CallRegs r;
        r.x[2] = 0;
        r.x[3] = 4;
        r.set_d(0, 10);
        r.set_d(1, 10);
        static_cast<void>(f.send(painter, "drawGlyphsForGlyphRange:atPoint:", r));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    const auto window = f.make_window({{100, 100}, {200, 80}});
    const auto canvas = f.with_rect(f.id(canvas_class, "alloc"), "initWithFrame:", {{0, 0}, {200, 80}});
    static_cast<void>(f.id(window, "setContentView:", {canvas}));
    f.show(window);
    // Pixels are y up: the flipped view's text 10 points below the top is at y 40..70.
    require(inked(f, window, {{12, 40}, {60, 30}}) > 20 && inked(f, window, {{120, 2}, {60, 25}}) == 0,
            "the glyphs are drawn at the container origin");
    static_cast<void>(f.id(window, "close"));

    // NSTextStorage's views of its text and its font / colour.
    const auto text = f.id(f.id(f.cls("NSTextStorage"), "alloc"), "initWithString:", {f.str("one two\nthree")});
    static_cast<void>(f.id(text, "addAttribute:value:range:", {f.str("NSFont"), large, 4, 3}));
    require(f.integer(f.id(text, "paragraphs"), "count") == 2 && f.integer(f.id(text, "words"), "count") == 3 &&
                f.integer(f.id(text, "attributeRuns"), "count") == 3 && f.integer(f.id(text, "characters"), "count") == 13,
            "paragraphs, words, attribute runs and characters");
    require(f.text(f.id(f.id(f.id(text, "words"), "objectAtIndex:", {1}), "string")) == "two", "the second word");
    static_cast<void>(f.id(text, "setFont:", {small}));
    const auto red = f.id(f.cls("NSColor"), "redColor");
    static_cast<void>(f.id(text, "setForegroundColor:", {red}));
    require(f.id(text, "font") == small && f.id(text, "foregroundColor") == red && f.integer(f.id(text, "attributeRuns"), "count") == 1,
            "-setFont: and -setForegroundColor: cover the text");

    // Paragraph style additions and text lists.
    const auto list = f.id(f.id(f.cls("NSTextList"), "alloc"), "initWithMarkerFormat:options:", {f.str("{decimal}."), 0});
    require(f.text(f.id(list, "markerForItemNumber:", {3})) == "3." && f.yes(list, "isOrdered") && f.integer(list, "startingItemNumber") == 1,
            "a decimal list");
    const auto roman = f.id(f.id(f.cls("NSTextList"), "alloc"), "initWithMarkerFormat:options:startingItemNumber:", {f.str("({upper-roman})"), 0, 4});
    const auto alpha = f.id(f.id(f.cls("NSTextList"), "alloc"), "initWithMarkerFormat:options:", {f.str("{lower-alpha}"), 0});
    const auto disc = f.id(f.id(f.cls("NSTextList"), "alloc"), "initWithMarkerFormat:options:", {f.str("{disc}"), 0});
    require(f.text(f.id(roman, "markerForItemNumber:", {14})) == "(XIV)" && f.integer(roman, "startingItemNumber") == 4 &&
                f.text(f.id(alpha, "markerForItemNumber:", {28})) == "ab" && f.text(f.id(disc, "markerForItemNumber:", {1})) == "\xE2\x80\xA2" &&
                !f.yes(disc, "isOrdered"),
            "roman, alphabetic and bullet markers");
    const auto paragraph = f.id(f.id(f.cls("NSMutableParagraphStyle"), "alloc"), "init");
    static_cast<void>(f.id(paragraph, "setHeaderLevel:", {2}));
    static_cast<void>(f.id(paragraph, "setTextLists:", {f.cf.array(std::vector<Address>{list})}));
    static_cast<void>(f.id(paragraph, "setUsesDefaultHyphenation:", {1}));
    const auto copy = f.id(paragraph, "copy");
    require(f.integer(copy, "headerLevel") == 2 && f.integer(f.id(copy, "textLists"), "count") == 1 && f.yes(copy, "usesDefaultHyphenation") &&
                std::abs(f.send(paragraph, "tighteningFactorForTruncation").s(0) - 0.05f) < 1e-6 && f.integer(f.id(copy, "textBlocks"), "count") == 0,
            "header level, text lists and hyphenation travel with a copy");
    stopped = false;
    try { static_cast<void>(f.id(paragraph, "setTextBlocks:", {f.cf.array(std::vector<Address>{list})})); } catch (const std::exception&) { stopped = true; }
    require(stopped, "text blocks stop explicitly");

}

// ---- NSTextTab and paragraph styles -----------------------------------------------------------
void text_tabs(Fixture& f) {
    const auto tab_class = f.cls("NSTextTab");
    CallRegs centered;
    centered.x[2] = 1;   // NSTextAlignmentCenter
    centered.set_d(0, 100);
    centered.x[3] = f.cf.dictionary(std::vector<std::pair<Address, Address>>{});
    const auto center = f.send(f.id(tab_class, "alloc"), "initWithTextAlignment:location:options:", centered).x0;
    require(f.integer(center, "alignment") == 1 && f.send(center, "location").d(0) == 100 && f.integer(center, "tabStopType") == 2, "a centre tab");
    const auto left = f.with_double(f.id(tab_class, "alloc"), "initWithType:location:", 200, {0});
    require(f.integer(left, "alignment") == 0 && f.integer(left, "tabStopType") == 0, "a left tab");
    const auto style = f.id(f.id(f.cls("NSMutableParagraphStyle"), "alloc"), "init");
    auto stops = f.id(style, "tabStops");
    require(f.integer(stops, "count") == 12 && f.send(f.id(stops, "objectAtIndex:", {0}), "location").d(0) == 28, "AppKit's default tab stops");
    static_cast<void>(f.id(style, "setTabStops:", {f.cf.array(std::vector<Address>{left, center})}));
    stops = f.id(style, "tabStops");
    require(f.integer(stops, "count") == 2 && f.send(f.id(stops, "objectAtIndex:", {0}), "location").d(0) == 100, "tab stops sorted by location");
    const auto copy = f.id(style, "copy");
    require(f.yes(copy, "isEqual:", {style}) && f.integer(f.id(copy, "tabStops"), "count") == 2, "a copied style keeps its tabs");
    static_cast<void>(f.id(style, "removeTabStop:", {center}));
    require(f.integer(f.id(style, "tabStops"), "count") == 1 && !f.yes(copy, "isEqual:", {style}), "-removeTabStop:");
    static_cast<void>(f.id(style, "addTabStop:", {center}));
    require(f.send(f.id(f.id(style, "tabStops"), "objectAtIndex:", {0}), "location").d(0) == 100, "-addTabStop: keeps the order");
    require(f.yes(center, "isEqual:", {f.id(center, "copy")}), "tab equality");
}

// ---- tables and outlines -------------------------------------------------------------------------
void tables(Fixture& f) {
    journal.clear();
    static std::int64_t rows = 5;
    const auto source_class = f.subclass("TestG3TableSource", "NSObject");
    f.add(source_class, "numberOfRowsInTableView:", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(rows); }, "q@:@");
    f.add(source_class, "tableView:viewForTableColumn:row:", [&f](CpuState& s, GuestMemory&) {
        s.x[0] = f.id(f.cls("NSTextField"), "labelWithString:", {f.str("row " + std::to_string(s.x[4]))});
    }, "@@:@@q");
    f.add(source_class, "tableView:didAddRowView:forRow:", [](CpuState& s, GuestMemory&) { journal.push_back("added:" + std::to_string(s.x[4])); }, "v@:@@q");
    const auto source = f.id(f.id(source_class, "alloc"), "init");
    const auto table = f.with_rect(f.id(f.cls("NSTableView"), "alloc"), "initWithFrame:", {{0, 0}, {200, 120}});
    const auto column = f.id(f.id(f.cls("NSTableColumn"), "alloc"), "initWithIdentifier:", {f.str("c")});
    static_cast<void>(f.with_double(column, "setWidth:", 200));
    static_cast<void>(f.id(table, "addTableColumn:", {column}));
    static_cast<void>(f.id(table, "setDataSource:", {source}));
    static_cast<void>(f.id(table, "setDelegate:", {source}));
    const auto window = f.make_window({{0, 0}, {200, 120}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {table}));
    static_cast<void>(f.id(table, "reloadData"));
    f.show(window);
    const auto row_view = f.id(table, "rowViewAtRow:makeIfNecessary:", {2, 0});
    require(row_view && f.id(row_view, "class") == f.cls("NSTableRowView") && log_has("added:2"), "row views: " + joined());
    const auto cell = f.id(table, "viewAtColumn:row:makeIfNecessary:", {0, 2, 0});
    require(cell && f.id(cell, "superview") == row_view && f.integer(table, "rowForView:", {cell}) == 2 && f.integer(table, "rowForView:", {row_view}) == 2,
            "cell views live in their row views");
    static std::vector<std::pair<Address, std::int64_t>> enumerated;
    enumerated.clear();
    const auto block = f.block("test_g3_row_views", [](CpuState& s, GuestMemory&) { enumerated.emplace_back(s.x[1], static_cast<std::int64_t>(s.x[2])); });
    static_cast<void>(f.id(table, "enumerateAvailableRowViewsUsingBlock:", {block}));
    require(enumerated.size() == 5 && enumerated[2].first == row_view && enumerated[2].second == 2, "row view enumeration");
    // Selection reaches the row views; moved rows keep it.
    static_cast<void>(f.id(table, "selectRowIndexes:byExtendingSelection:", {f.id(f.cls("NSIndexSet"), "indexSetWithIndex:", {2}), 0}));
    require(f.yes(f.id(table, "rowViewAtRow:makeIfNecessary:", {2, 0}), "isSelected"), "a selected row view");
    static_cast<void>(f.id(table, "moveRowAtIndex:toIndex:", {2, 4}));
    require(f.integer(table, "selectedRow") == 4 && f.yes(f.id(table, "rowViewAtRow:makeIfNecessary:", {4, 0}), "isSelected"), "the selection moves with its row");
    static_cast<void>(f.id(table, "setDropRow:dropOperation:", {1, 0}));
    require(f.yes(f.id(table, "rowViewAtRow:makeIfNecessary:", {1, 0}), "isTargetForDropOperation"), "the drop target row");
    require(f.integer(table, "editedColumn") == -1 && f.integer(table, "editedRow") == -1, "no cell is edited");
    require(f.integer(f.id(table, "accessibilityVisibleRows"), "count") == 5 && f.integer(f.id(table, "accessibilityVisibleColumns"), "count") == 1 &&
                f.integer(f.id(table, "accessibilitySelectedCells"), "count") == 1, "table accessibility");
    // The selected row draws (the row view's selection) in the selection colour.
    static_cast<void>(f.id(window, "makeFirstResponder:", {table}));
    const auto table_frame = f.send(table, "frame");
    const auto selected = f.pixel(window, 190, table_frame.d(1) + table_frame.d(3) - (4 * 19 + 9));
    require(selected[0] > 180 && selected[2] < 140, "the selected row's colour: " + bgra_text(selected));
    static_cast<void>(f.id(window, "close"));

    // NSOutlineView: item-level updates keep the selected items.
    static std::vector<Address> roots, children;
    roots = {f.str("A"), f.str("B")};
    children = {f.str("a1"), f.str("a2")};
    const auto outline_source = f.subclass("TestG3OutlineSource", "NSObject");
    f.add(outline_source, "outlineView:numberOfChildrenOfItem:", [](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[3] == 0 ? roots.size() : s.x[3] == roots[0] ? children.size() : 0;
    }, "q@:@@");
    f.add(outline_source, "outlineView:child:ofItem:", [](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[4] == 0 ? roots[s.x[3]] : children[s.x[3]];
    }, "@@:@q@");
    f.add(outline_source, "outlineView:isItemExpandable:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[3] == roots[0]; }, "B@:@@");
    f.add(outline_source, "outlineView:objectValueForTableColumn:byItem:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[4]; }, "@@:@@@");
    const auto tree = f.with_rect(f.id(f.cls("NSOutlineView"), "alloc"), "initWithFrame:", {{0, 0}, {200, 120}});
    static_cast<void>(f.id(tree, "addTableColumn:", {f.id(f.id(f.cls("NSTableColumn"), "alloc"), "initWithIdentifier:", {f.str("t")})}));
    static_cast<void>(f.id(tree, "setDataSource:", {f.id(f.id(outline_source, "alloc"), "init")}));
    static_cast<void>(f.id(tree, "reloadData"));
    static_cast<void>(f.id(tree, "expandItem:", {roots[0]}));
    require(f.integer(tree, "numberOfChildrenOfItem:", {0}) == 2 && f.integer(tree, "childIndexForItem:", {children[1]}) == 1 &&
                f.id(tree, "child:ofItem:", {1, roots[0]}) == children[1], "outline children");
    static_cast<void>(f.id(tree, "selectRowIndexes:byExtendingSelection:", {f.id(f.cls("NSIndexSet"), "indexSetWithIndex:", {3}), 0}));
    require(f.id(tree, "itemAtRow:", {3}) == roots[1], "B is row 3");
    children.push_back(f.str("a3"));
    static_cast<void>(f.id(tree, "insertItemsAtIndexes:inParent:withAnimation:", {f.id(f.cls("NSIndexSet"), "indexSetWithIndex:", {2}), roots[0], 0}));
    require(f.integer(tree, "selectedRow") == 4 && f.id(tree, "itemAtRow:", {4}) == roots[1], "the selected item stays selected after an insert");
}

// ---- NSCollectionView ----------------------------------------------------------------------------
void collection_views(Fixture& f) {
    journal.clear();
    const auto item_class = f.subclass("TestG3CollectionItem", "NSCollectionViewItem");
    f.add(item_class, "loadView", [&f](CpuState& s, GuestMemory&) {
        const auto made = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {10, 10}});
        static_cast<void>(f.id(s.x[0], "setView:", {made}));
    }, "v@:");
    const auto source_class = f.subclass("TestG3CollectionSource", "NSObject");
    f.add(source_class, "collectionView:numberOfItemsInSection:", [](CpuState& s, GuestMemory&) { s.x[0] = 7; }, "q@:@q");
    f.add(source_class, "collectionView:itemForRepresentedObjectAtIndexPath:", [&f](CpuState& s, GuestMemory&) {
        s.x[0] = f.id(s.x[2], "makeItemWithIdentifier:forIndexPath:", {f.str("cell"), s.x[3]});
    }, "@@:@@");
    f.add(source_class, "collectionView:didSelectItemsAtIndexPaths:", [&f](CpuState& s, GuestMemory&) {
        journal.push_back("selected:" + std::to_string(f.id(s.x[3], "count")));
    }, "v@:@@");
    const auto source = f.id(f.id(source_class, "alloc"), "init");
    const auto layout = f.id(f.id(f.cls("NSCollectionViewFlowLayout"), "alloc"), "init");
    CallRegs item_size;
    gui::appkit::put_size(item_size, 0, {100, 80});
    static_cast<void>(f.send(layout, "setItemSize:", item_size));
    static_cast<void>(f.with_double(layout, "setMinimumInteritemSpacing:", 10));
    static_cast<void>(f.with_double(layout, "setMinimumLineSpacing:", 12));
    CallRegs insets;
    for (unsigned i = 0; i < 4; ++i) insets.set_d(i, 20);
    static_cast<void>(f.send(layout, "setSectionInset:", insets));
    const auto view = f.with_rect(f.id(f.cls("NSCollectionView"), "alloc"), "initWithFrame:", {{0, 0}, {470, 300}});
    static_cast<void>(f.id(view, "setCollectionViewLayout:", {layout}));
    static_cast<void>(f.id(view, "registerClass:forItemWithIdentifier:", {item_class, f.str("cell")}));
    static_cast<void>(f.id(view, "setDataSource:", {source}));
    static_cast<void>(f.id(view, "setDelegate:", {source}));
    static_cast<void>(f.id(view, "setSelectable:", {1}));
    const auto window = f.make_window({{0, 0}, {470, 300}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {view}));
    static_cast<void>(f.id(view, "reloadData"));
    require(f.integer(view, "numberOfItemsInSection:", {0}) == 7, "item count");
    const auto frame_of = [&](Address index) { return f.send(view, "frameForItemAtIndex:", [&] { CallRegs r; r.x[2] = index; return r; }()); };
    require(frame_of(3).d(0) == 350 && frame_of(3).d(1) == 20 && frame_of(4).d(0) == 20 && frame_of(4).d(1) == 112 && frame_of(4).d(2) == 100,
            "flow layout frames");
    CallRegs point;
    gui::appkit::put_point(point, 0, {25, 25});
    const auto path = f.send(view, "indexPathForItemAtPoint:", point).x0;
    require(path && f.id(path, "item") == 0 && f.id(path, "section") == 0, "-indexPathForItemAtPoint:");
    f.show(window);
    // A click on item 5 (second row, second column) selects it.
    const auto view_frame = f.send(view, "frame");
    const auto height = view_frame.d(3);
    f.click(window, {view_frame.d(0) + 180, view_frame.d(1) + height - 152});
    const auto selection = f.id(view, "selectionIndexPaths");
    require(f.integer(selection, "count") == 1 && f.id(f.id(selection, "anyObject"), "item") == 5 && log_has("selected:1"), "click selection: " + joined());
    require(f.yes(f.id(view, "itemAtIndex:", {5}), "isSelected"), "the item knows it is selected");
    f.key(window, 0x7c, "");
    require(f.id(f.id(f.id(view, "selectionIndexPaths"), "anyObject"), "item") == 6 && !f.yes(f.id(view, "itemAtIndex:", {5}), "isSelected"),
            "the right arrow moves the selection");
    static_cast<void>(f.id(window, "close"));
}

// ---- NSRulerView and NSScrollView rulers / insets -------------------------------------------------
void rulers(Fixture& f) {
    const auto scroll = f.with_rect(f.id(f.cls("NSScrollView"), "alloc"), "initWithFrame:", {{0, 0}, {300, 200}});
    const auto document = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {600, 600}});
    static_cast<void>(f.id(scroll, "setDocumentView:", {document}));
    static_cast<void>(f.id(scroll, "setHasHorizontalRuler:", {1}));
    static_cast<void>(f.id(scroll, "setRulersVisible:", {1}));
    const auto ruler = f.id(scroll, "horizontalRulerView");
    require(ruler && f.id(ruler, "class") == f.cls("NSRulerView") && f.id(ruler, "scrollView") == scroll && f.id(ruler, "orientation") == 0, "the ruler");
    const auto thickness = f.send(ruler, "requiredThickness").d(0);
    const auto clip = f.id(scroll, "contentView");
    require(thickness == 31 && f.send(ruler, "frame").d(1) == 0 && f.send(clip, "frame").d(1) == 31, "the ruler sits above the clip view");
    require(f.text(f.id(ruler, "measurementUnits")) == "Inches", "default units");
    bool threw = false;
    try { static_cast<void>(f.id(ruler, "setMeasurementUnits:", {f.str("Furlongs")})); } catch (const std::exception&) { threw = true; }
    require(threw, "unregistered units did not fail");
    CallRegs unit;
    unit.x[2] = f.str("Furlongs");
    unit.x[3] = f.str("fur");
    unit.set_d(0, 14256);
    unit.x[4] = f.cf.array(std::vector<Address>{f.cf.integer(2)});
    unit.x[5] = f.cf.array(std::vector<Address>{f.cf.real(0.5)});
    static_cast<void>(f.send(f.cls("NSRulerView"), "registerUnitWithName:abbreviation:unitToPointsConversionFactor:stepUpCycle:stepDownCycle:", unit));
    static_cast<void>(f.id(ruler, "setMeasurementUnits:", {f.str("Points")}));
    static_cast<void>(f.id(ruler, "setMeasurementUnits:", {f.str("Furlongs")}));
    static_cast<void>(f.id(ruler, "setMeasurementUnits:", {f.str("Points")}));
    static_cast<void>(f.id(ruler, "setClientView:", {document}));
    // Content insets push the clip view in.
    CallRegs insets;
    insets.set_d(0, 10); insets.set_d(1, 0); insets.set_d(2, 0); insets.set_d(3, 0);
    static_cast<void>(f.send(scroll, "setContentInsets:", insets));
    require(f.send(clip, "frame").d(1) == 41 && f.send(scroll, "contentInsets").d(0) == 10, "content insets");
    const auto window = f.make_window({{0, 0}, {300, 200}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {scroll}));
    f.show(window);
    require(marked(f, window, {{0, 200 - 31}, {300, 31}}) > 60, "the ruler's marks were drawn");
    static_cast<void>(f.id(scroll, "setRulersVisible:", {0}));
    require(f.id(ruler, "superview") == 0 && f.send(clip, "frame").d(1) == 10, "hidden rulers leave the scroll view");
    static_cast<void>(f.id(window, "close"));
}

// ---- accessibility ------------------------------------------------------------------------------
void accessibility(Fixture& f) {
    const auto view = f.with_rect(f.id(f.cls("NSView"), "alloc"), "initWithFrame:", {{0, 0}, {100, 50}});
    static_cast<void>(f.id(view, "setAccessibilityLabel:", {f.str("Cue list")}));
    static_cast<void>(f.id(view, "setAccessibilityIdentifier:", {f.str("cues")}));
    static_cast<void>(f.id(view, "setAccessibilityRole:", {f.str("AXTable")}));
    static_cast<void>(f.id(view, "setAccessibilityIndex:", {3}));
    require(f.text(f.id(view, "accessibilityLabel")) == "Cue list" && f.text(f.id(view, "accessibilityIdentifier")) == "cues" &&
                f.text(f.id(view, "accessibilityRole")) == "AXTable" && f.integer(view, "accessibilityIndex") == 3, "view accessibility properties");
    CallRegs rect;
    gui::appkit::put_rect(rect, 0, {{1, 2}, {3, 4}});
    rect.x[2] = f.str("AXRow");
    rect.x[3] = f.str("Row");
    rect.x[4] = view;
    const auto element = f.send(f.cls("NSAccessibilityElement"), "accessibilityElementWithRole:frame:label:parent:", rect).x0;
    const auto frame = f.send(element, "accessibilityFrame");
    require(f.text(f.id(element, "accessibilityRole")) == "AXRow" && f.text(f.id(element, "accessibilityLabel")) == "Row" &&
                f.id(element, "accessibilityParent") == view && frame.d(0) == 1 && frame.d(3) == 4 && f.yes(element, "isAccessibilityElement"),
            "accessibility element");
    static_cast<void>(f.id(element, "setAccessibilityIdentifier:", {f.str("row-1")}));
    require(f.text(f.id(element, "accessibilityIdentifier")) == "row-1", "element identifier");
}

// ---- small additions -------------------------------------------------------------------------------
void small_additions(Fixture& f) {
    journal.clear();
    const auto picture = f.id(f.id(f.cls("NSImage"), "alloc"), "initWithSize:", {});
    const auto target = f.target("small");
    const auto image_button = f.id(f.cls("NSButton"), "buttonWithImage:target:action:", {picture, target, f.objc.selector("acted:")});
    require(f.id(image_button, "image") == picture && f.id(image_button, "imagePosition") == 1 && f.id(image_button, "target") == target, "+buttonWithImage:target:action:");
    const auto red = f.id(f.cls("NSColor"), "redColor");
    static_cast<void>(f.id(image_button, "setContentTintColor:", {red}));
    require(f.id(image_button, "contentTintColor") == red, "-contentTintColor");
    const auto cell = f.id(image_button, "cell");
    require(f.id(cell, "highlightsBy") == 6 && f.id(cell, "showsStateBy") == 0, "push button masks");
    static_cast<void>(f.id(image_button, "setButtonType:", {3}));
    require(f.id(cell, "highlightsBy") == 1 && f.id(cell, "showsStateBy") == 1, "switch masks");
    static_cast<void>(f.id(cell, "setHighlightsBy:", {0}));
    require(f.id(cell, "highlightsBy") == 0, "-setHighlightsBy:");
    // NSMenu.
    const auto menu = f.id(f.id(f.cls("NSMenu"), "alloc"), "initWithTitle:", {f.str("M")});
    static_cast<void>(f.id(menu, "addItemWithTitle:action:keyEquivalent:", {f.str("Last"), 0, f.str("")}));
    const auto inserted = f.id(menu, "insertItemWithTitle:action:keyEquivalent:atIndex:", {f.str("First"), 0, f.str("f"), 0});
    require(f.text(f.id(inserted, "title")) == "First" && f.id(menu, "itemAtIndex:", {0}) == inserted && f.integer(menu, "numberOfItems") == 2, "menu insertion");
    // NSProgress file-operation properties.
    const auto progress = f.id(f.cls("NSProgress"), "progressWithTotalUnitCount:", {10});
    static_cast<void>(f.id(progress, "setKind:", {f.str("NSProgressKindFile")}));
    static_cast<void>(f.id(progress, "setFileOperationKind:", {f.str("NSProgressFileOperationKindCopying")}));
    static_cast<void>(f.id(progress, "setFileTotalCount:", {f.cf.integer(5)}));
    static_cast<void>(f.id(progress, "setFileCompletedCount:", {f.cf.integer(2)}));
    static_cast<void>(f.id(progress, "setCancellable:", {0}));
    require(f.text(f.id(progress, "kind")) == "NSProgressKindFile" && f.text(f.id(progress, "fileOperationKind")) == "NSProgressFileOperationKindCopying" &&
                f.cf.number_value(f.id(progress, "fileTotalCount")).value_or(0) == 5 &&
                f.cf.number_value(f.id(f.id(progress, "userInfo"), "objectForKey:", {f.str("NSProgressFileCompletedCountKey")})).value_or(0) == 2,
            "NSProgress file properties");
    static_cast<void>(f.id(progress, "cancel"));
    require(!f.yes(progress, "isCancellable") && !f.yes(progress, "isCancelled"), "a progress that cannot be cancelled");
    // NSScroller parts and the slider's bar.
    const auto bar = f.with_rect(f.id(f.cls("NSScroller"), "alloc"), "initWithFrame:", {{0, 0}, {15, 100}});
    static_cast<void>(f.with_double(bar, "setKnobProportion:", 0.5));
    static_cast<void>(f.with_double(bar, "setDoubleValue:", 1.0));
    const auto knob = f.send(bar, "rectForPart:", [] { CallRegs r; r.x[2] = 2; return r; }());
    const auto slot = f.send(bar, "rectForPart:", [] { CallRegs r; r.x[2] = 6; return r; }());
    require(slot.d(1) == 3 && slot.d(3) == 94 && knob.d(3) == 47 && knob.d(1) == 50, "scroller parts");
    const auto slider = f.with_rect(f.id(f.cls("NSSlider"), "alloc"), "initWithFrame:", {{0, 0}, {100, 24}});
    const auto bar_rect = f.send(f.id(slider, "cell"), "barRectFlipped:", [] { CallRegs r; r.x[2] = 1; return r; }());
    require(bar_rect.d(0) == 10 && bar_rect.d(2) == 80, "-barRectFlipped:");
    // A text field that sends its action when editing ends otherwise than by Return.
    f.describe = [&f](Address sender) { return f.text(f.id(sender, "stringValue")); };
    const auto field = f.with_rect(f.id(f.cls("NSTextField"), "alloc"), "initWithFrame:", {{10, 10}, {150, 22}});
    static_cast<void>(f.id(f.id(field, "cell"), "setSendsActionOnEndEditing:", {1}));
    static_cast<void>(f.id(field, "setTarget:", {target}));
    static_cast<void>(f.id(field, "setAction:", {f.objc.selector("acted:")}));
    const auto window = f.make_window({{0, 0}, {200, 50}});
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {field}));
    f.show(window);
    f.click(window, {50, 21});
    const auto editor = f.id(field, "currentEditor");
    require(editor != 0, "editing");
    static_cast<void>(f.id(editor, "insertText:", {f.str("typed")}));
    static_cast<void>(f.id(editor, "insertTab:", {0}));
    require(log_has("small:typed"), "the action on end editing: " + joined());
    journal.clear();
    f.click(window, {50, 21});
    static_cast<void>(f.id(f.id(field, "currentEditor"), "cancelOperation:", {0}));
    require(journal.empty(), "a cancelled edit sent the action");
    static_cast<void>(f.id(window, "close"));
    // Font family members and the font manager's selection.
    const auto fonts = f.id(f.cls("NSFontManager"), "sharedFontManager");
    const auto members = f.id(fonts, "availableMembersOfFontFamily:", {f.str("Helvetica")});
    require(members && f.integer(members, "count") >= 1 && f.integer(f.id(members, "objectAtIndex:", {0}), "count") == 4,
            "-availableMembersOfFontFamily:");
    require(f.id(fonts, "availableMembersOfFontFamily:", {f.str("No Such Family Here")}) == 0, "an unknown family has no members");
    const auto chosen = f.with_double(f.cls("NSFont"), "systemFontOfSize:", 17);
    static_cast<void>(f.id(fonts, "setSelectedFont:isMultiple:", {chosen, 0}));
    require(f.id(fonts, "selectedFont") == chosen && f.id(fonts, "fontPanel:", {0}) == 0, "the font manager's selected font");
    // NSTouch answers its API (no touches are delivered).
    require(f.objc.responds(f.id(f.cls("NSTouch"), "alloc"), f.objc.selector("normalizedPosition")), "NSTouch");
}

// ---- appearance: push buttons in dark-aqua and aqua windows -------------------------------------
// Pixels of the title area brighter / darker than a threshold.
std::pair<std::size_t, std::size_t> light_and_dark(Fixture& f, Address window, gui::Rect rect) {
    static_cast<void>(f.next_event());
    std::size_t light = 0, dark = 0;
    for (double y = rect.origin.y; y < rect.origin.y + rect.size.height; y += 0.5)
        for (double x = rect.origin.x; x < rect.origin.x + rect.size.width; x += 0.5) {
            const auto v = f.server.pixel(f.id(window, "windowNumber"), x, y);
            const auto luma = (v[0] + v[1] + v[2]) / 3;
            if (luma > 200) ++light;
            if (luma < 60) ++dark;
        }
    return {light, dark};
}

void appearances(Fixture& f) {
    const auto dark_aqua = f.id(f.cls("NSAppearance"), "appearanceNamed:", {f.str("NSAppearanceNameDarkAqua")});
    const auto aqua = f.id(f.cls("NSAppearance"), "appearanceNamed:", {f.str("NSAppearanceNameAqua")});
    const auto make = [&](Address appearance, Address background) {
        const auto window = f.make_window({{0, 0}, {300, 80}});
        static_cast<void>(f.id(window, "setAppearance:", {appearance}));
        static_cast<void>(f.id(window, "setBackgroundColor:", {background}));
        const auto plain = f.with_rect(f.id(f.cls("NSButton"), "alloc"), "initWithFrame:", {{10, 40}, {280, 32}});
        static_cast<void>(f.id(plain, "setTitle:", {f.str("MMMMMMMMMMMMMMMMMMMM")}));
        // A title coloured with the semantic control text colour (taken
        // outside drawing, as applications do when they build titles).
        const auto semantic = f.with_rect(f.id(f.cls("NSButton"), "alloc"), "initWithFrame:", {{10, 4}, {280, 32}});
        const auto attributes = f.cf.dictionary(std::vector<std::pair<Address, Address>>{{f.str("NSColor"), f.id(f.cls("NSColor"), "controlTextColor")}});
        const auto title = f.id(f.id(f.cls("NSAttributedString"), "alloc"), "initWithString:attributes:", {f.str("MMMMMMMMMMMMMMMMMMMM"), attributes});
        static_cast<void>(f.id(semantic, "setAttributedTitle:", {title}));
        static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {plain}));
        static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {semantic}));
        f.show(window);
        return window;
    };
    // The text band of each button (inside the bezel, around the title).
    const gui::Rect top_title{{60, 50}, {180, 10}}, bottom_title{{60, 14}, {180, 10}};
    const auto black = f.id(f.cls("NSColor"), "blackColor"), white = f.id(f.cls("NSColor"), "whiteColor");
    const auto dark_window = make(dark_aqua, black);
    for (const auto& band : {top_title, bottom_title}) {
        const auto [light, dark] = light_and_dark(f, dark_window, band);
        require(light > 150 && dark < light, "dark appearance: light title text on the dark bezel (light " + std::to_string(light) +
                                                ", dark " + std::to_string(dark) + ")");
    }
    // The bezel is a dark face, not the light one.
    const auto face = f.pixel(dark_window, 20, 56);
    require(face[0] < 120 && face[1] < 120 && face[2] < 120, "dark appearance: a dark bezel face " + bgra_text(face));
    const auto light_window = make(aqua, white);
    for (const auto& band : {top_title, bottom_title}) {
        const auto [light, dark] = light_and_dark(f, light_window, band);
        require(dark > 150, "aqua appearance: dark title text (dark " + std::to_string(dark) + ", light " + std::to_string(light) + ")");
    }
    // The same semantic colour drew light in one window and dark in the other.
    require(f.id(f.cls("NSColor"), "controlTextColor") == f.id(f.cls("NSColor"), "controlTextColor"), "one shared dynamic colour");
    static_cast<void>(f.id(dark_window, "close"));
    static_cast<void>(f.id(light_window, "close"));

    // A text view with default colours in a scroll view of a dark window: a
    // dark background, light text.
    const auto text_window = f.make_window({{0, 0}, {300, 120}});
    static_cast<void>(f.id(text_window, "setAppearance:", {dark_aqua}));
    static_cast<void>(f.id(text_window, "setBackgroundColor:", {black}));
    const auto scroll = f.with_rect(f.id(f.cls("NSScrollView"), "alloc"), "initWithFrame:", {{10, 10}, {280, 100}});
    static_cast<void>(f.id(scroll, "setHasVerticalScroller:", {1}));
    const auto text = f.with_rect(f.id(f.cls("NSTextView"), "alloc"), "initWithFrame:", {{0, 0}, {265, 100}});
    static_cast<void>(f.id(text, "setString:", {f.str("MMMMMMMMMMMMMMMMMMMM MMMMMMMMMM\nMMMMMMMMMMMMMMMMMMMM MMMMMMMMMM")}));
    static_cast<void>(f.id(scroll, "setDocumentView:", {text}));
    static_cast<void>(f.id(f.id(text_window, "contentView"), "addSubview:", {scroll}));
    f.show(text_window);
    const auto background = f.pixel(text_window, 200, 30);
    require(background[0] < 80 && background[1] < 80 && background[2] < 80, "dark text view background " + bgra_text(background));
    {
        const auto [light, dark] = light_and_dark(f, text_window, {{14, 92}, {200, 14}});
        require(light > 150 && dark > light, "light text on the dark text view (light " + std::to_string(light) + ", dark " + std::to_string(dark) + ")");
    }
    // Semantic background colours given to the text view and its clip view
    // follow the dark window too.
    static_cast<void>(f.id(text, "setBackgroundColor:", {f.id(f.cls("NSColor"), "textBackgroundColor")}));
    static_cast<void>(f.id(text, "setTextColor:", {f.id(f.cls("NSColor"), "textColor")}));
    static_cast<void>(f.id(f.id(scroll, "contentView"), "setBackgroundColor:", {f.id(f.cls("NSColor"), "textBackgroundColor")}));
    static_cast<void>(f.id(text, "setNeedsDisplay:", {1}));
    const auto semantic_background = f.pixel(text_window, 200, 30);
    require(semantic_background[0] < 80 && semantic_background[2] < 80, "textBackgroundColor in the dark window " + bgra_text(semantic_background));
    {
        const auto [light, dark] = light_and_dark(f, text_window, {{14, 92}, {200, 14}});
        require(light > 150, "textColor in the dark window (light " + std::to_string(light) + ")");
    }
    // A colour taken outside drawing resolves for the application's
    // appearance, whatever window drew last (the nib loader copies archived
    // catalog colours this way).
    static_cast<void>(f.id(f.app, "setAppearance:", {dark_aqua}));
    const auto light_again = make(aqua, white);   // an aqua window draws last
    const auto taken = f.send(f.id(f.cls("NSColor"), "textBackgroundColor"), "redComponent").d(0);
    require(taken < 0.3, "textBackgroundColor taken outside drawing follows the dark application: " + std::to_string(taken));
    static_cast<void>(f.id(f.app, "setAppearance:", {aqua}));
    require(f.send(f.id(f.cls("NSColor"), "textBackgroundColor"), "redComponent").d(0) > 0.9, "and the aqua application");
    static_cast<void>(f.id(light_again, "close"));
    static_cast<void>(f.id(text_window, "close"));
}

// ---- a text view in a scroll view: width, wrapping, scroller -----------------------------------
void scrolling_text(Fixture& f) {
    // As QLab.nib's license box: a bezel-bordered scroll view with a legacy
    // vertical scroller; the text view sized to the clip view, width and
    // height sizable.
    const auto window = f.make_window({{0, 0}, {720, 200}});
    // On a 1x display, as the integrator's capture: text is hinted at 13 pixels.
    f.server.change_backing_scale(f.id(window, "windowNumber"), 1);
    const auto scroll = f.with_rect(f.id(f.cls("NSScrollView"), "alloc"), "initWithFrame:", {{20, 20}, {678, 146}});
    static_cast<void>(f.id(scroll, "setBorderType:", {2}));
    static_cast<void>(f.id(scroll, "setHasVerticalScroller:", {1}));
    static_cast<void>(f.id(scroll, "setAutohidesScrollers:", {0}));
    const auto text = f.with_rect(f.id(f.cls("NSTextView"), "alloc"), "initWithFrame:", {{0, 0}, {676, 144}});
    static_cast<void>(f.id(text, "setAutoresizingMask:", {0x12}));
    static_cast<void>(f.id(scroll, "setDocumentView:", {text}));
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {scroll}));
    // The text arrives through the storage (as an RTF document would).
    // The first paragraph is one line of short words whose hinted advances
    // at 13 pixels (Segoe UI's space, E and M round up by 0.3 to 0.45) are
    // wider than their measured outline widths: drawn as one run, the line
    // would end some 30 points past its laid-out end.
    std::string first;
    for (int i = 0; i < 28; ++i) first += i ? " ME" : "ME";
    std::string body = first + "\n";
    const char* const sentences[] = {"NOTICE TO USER: PLEASE READ THIS AGREEMENT CAREFULLY. ",
                                     "BY COPYING, INSTALLING, OR USING ALL OR ANY PORTION OF THE SOFTWARE, ",
                                     "YOU ACCEPT ALL THE TERMS AND CONDITIONS OF THIS AGREEMENT. ",
                                     "IF YOU DO NOT AGREE TO THE TERMS OF THIS AGREEMENT, DO NOT INSTALL, COPY OR USE THE SOFTWARE. ",
                                     "THIS AGREEMENT IS ENFORCEABLE AGAINST YOU AND ANY LEGAL ENTITY THAT OBTAINED THE SOFTWARE. "};
    for (int i = 0; i < 60; ++i) body += sentences[(i * 3) % 5];
    const auto attributed = f.id(f.id(f.cls("NSAttributedString"), "alloc"), "initWithString:", {f.str(body)});
    static_cast<void>(f.id(f.id(text, "textStorage"), "setAttributedString:", {attributed}));
    static_cast<void>(f.id(text, "setFont:", {f.with_double(f.cls("NSFont"), "systemFontOfSize:", 13)}));
    f.show(window);
    const auto clip = f.id(scroll, "contentView");
    const auto clip_frame = f.send(clip, "frame");
    const auto text_frame = f.send(text, "frame");
    const auto content = f.send(scroll, "contentSize");
    require(clip_frame.d(2) == 661 && content.d(0) == 661, "the clip view excludes the border and the scroller: " + std::to_string(clip_frame.d(2)));
    require(text_frame.d(2) <= clip_frame.d(2), "the text view fits the clip view: " + std::to_string(text_frame.d(2)));
    require(text_frame.d(3) > clip_frame.d(3) * 2, "the text view grew to its text: " + std::to_string(text_frame.d(3)));
    // The container follows the text view's width (widthTracksTextView).
    const auto container = f.id(text, "textContainer");
    require(f.yes(container, "widthTracksTextView"), "the container tracks the text view's width");
    // Nothing is drawn under the scroller: its column shows the scroller.
    const auto scroller = f.id(scroll, "verticalScroller");
    require(f.send(scroller, "knobProportion").d(0) < 0.5 && !f.yes(scroller, "isHidden"), "the scroller knob reflects the overflow");
    const auto knob = f.send(scroller, "rectForPart:", [] { CallRegs r; r.x[2] = 2; return r; }());
    require(knob.d(3) > 10 && knob.d(3) < 70, "a knob of the visible part: " + std::to_string(knob.d(3)));
    // Pixels: the scroller's track and knob in its column, no text there.
    const auto scroller_frame = f.send(scroller, "frame");
    const double column_x = 20 + scroller_frame.d(0) + scroller_frame.d(2) / 2;
    const auto track = f.pixel(window, column_x, 20 + 10);                     // the bottom of the track (flipped scroll view)
    const auto knob_pixel = f.pixel(window, column_x, 20 + 146 - 1 - 3 - knob.d(3) / 2);   // the knob at the top
    require(track != knob_pixel && knob_pixel[0] < 220, "the knob is drawn over the track: " + bgra_text(track) + " " + bgra_text(knob_pixel));
    const auto edge = f.pixel(window, 20 + scroller_frame.d(0) + 0.25, 20 + 10);   // the track's separator line
    require(edge[0] < 240, "the scroller track's edge is drawn: " + bgra_text(edge));
    // No text runs into the scroller column.
    const gui::Rect column{{20 + scroller_frame.d(0) + 2, 20 + 4}, {scroller_frame.d(2) - 4, 140}};
    const auto [light_column, dark_column] = light_and_dark(f, window, column);
    require(dark_column == 0, "text drawn under the scroller: " + std::to_string(dark_column));
    static_cast<void>(light_column);
    // Lines sit inside the line fragment padding (5 points) on both sides:
    // ink in the body, none in the leading or trailing padding of the clip.
    require(f.send(container, "lineFragmentPadding").d(0) == 5, "the default line fragment padding");
    const double clip_left = 20 + 1;   // the bezel border
    const auto [light_body, dark_body] = light_and_dark(f, window, {{clip_left + 40, 20 + 4}, {500, 130}});
    require(dark_body > 100, "the text is drawn: " + std::to_string(dark_body));
    static_cast<void>(light_body);
    // Glyphs stay where the layout puts them: the first line's ink ends at
    // the caret position after its last character (its top 13 points).
    {
        CallRegs registers;
        registers.x[2] = first.size();
        const auto caret = f.send(text, "_alchemyCaretRect:", registers);
        const auto number = f.id(window, "windowNumber");
        const double top = 20 + 146 - 1;   // the clip's top edge (window coordinates grow upward)
        double ink_end = 0;
        for (double y = top - 15; y < top - 2; y += 0.5)
            for (double x = clip_left; x < clip_left + clip_frame.d(2); x += 0.5) {
                const auto v = f.server.pixel(number, x, y);
                if ((v[0] + v[1] + v[2]) / 3 < 160) ink_end = std::max(ink_end, x - clip_left);
            }
        require(caret.d(0) > 500 && caret.d(0) < 651, "the first line is long: " + std::to_string(caret.d(0)));
        // (Within a word the drawn glyphs may run a little past their
        // measured advances; the line's padding absorbs that.)
        require(ink_end <= caret.d(0) + 2, "the line's glyphs run past its laid-out end: ink to " + std::to_string(ink_end) +
                                                 ", line end " + std::to_string(caret.d(0)));
    }
    // Every sample of a 4-point band in each padding is the plain background
    // (8 x 260 samples at half-point steps).
    const auto [light_lead, dark_lead] = light_and_dark(f, window, {{clip_left, 20 + 4}, {4, 130}});
    require(light_lead == 8 * 260, "text drawn in the leading padding: " + std::to_string(8 * 260 - light_lead));
    static_cast<void>(dark_lead);
    const auto [light_trail, dark_trail] = light_and_dark(f, window, {{clip_left + clip_frame.d(2) - 4, 20 + 4}, {4, 130}});
    require(light_trail == 8 * 260, "text drawn in the trailing padding, toward the scroller: " + std::to_string(8 * 260 - light_trail));
    static_cast<void>(dark_trail);
    static_cast<void>(f.id(window, "close"));
}

} // namespace

int main() {
    try {
        Fixture f;
        segmented_controls(f);
        path_controls(f);
        search_fields(f);
        date_pickers(f);
        color_wells(f);
        text_system(f);
        text_layout(f);
        text_tabs(f);
        tables(f);
        collection_views(f);
        rulers(f);
        accessibility(f);
        small_additions(f);
        appearances(f);
        scrolling_text(f);
        nib_decoding(f);
    } catch (const std::exception& error) {
        std::cerr << "alchemy_gui_appkit_controls_behaviour: " << error.what() << '\n';
        return 1;
    }
    std::cout << "alchemy_gui_appkit_controls_behaviour: passed\n";
    return 0;
}
