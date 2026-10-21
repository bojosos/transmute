// AppKit string drawing (Quartz 2D's -drawInRect:withAttributes:, over
// CoreText) with NSParagraphStyle alignments in the arm64 NSTextAlignment
// numbering (left 0, center 1, right 2): pixels presented through the fake
// window server show left, centered and right-aligned lines where they belong.
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace gui = alchemy::gui;
using Address = std::uint64_t;
using alchemy::CallRegs;
using transmute::CpuState;
using transmute::GuestMemory;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
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
    Address app{};

    // AppKit with the Quartz 2D layer that draws its strings, as in
    // install_darwin_gui.
    Fixture() {
        alchemy::install_darwin_numeric(process);
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
        alchemy::install_darwin_frameworks(process, objc, cf);
        gui::appkit::set_window_server_override(&server);
        gui::InstallContext context{process, objc, cf};
        gui::install_appkit_gui(context);
        gui::install_quartzcore(context);
        gui::install_quartz2d(context);
        app = id(cls("NSApplication"), "sharedApplication");
        static_cast<void>(id(app, "finishLaunching"));
        static_cast<void>(id(app, "setAppearance:", {id(cls("NSAppearance"), "appearanceNamed:", {cf.string("NSAppearanceNameAqua")})}));
    }
    ~Fixture() { gui::appkit::set_window_server_override(nullptr); }
    Address cls(std::string_view name) {
        const auto value = objc.find_class(name);
        require(value != 0, "missing class " + std::string(name));
        return value;
    }
    Address id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        std::size_t index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return objc.send(receiver, objc.selector(selector), registers).x0;
    }
};

// The leftmost and rightmost inked columns (dark pixels) of a band of rows.
std::pair<int, int> ink(Fixture& f, Address window, int y0, int y1, int width) {
    int first = -1, last = -1;
    for (int x = 0; x < width; ++x)
        for (int y = y0; y < y1; ++y) {
            const auto p = f.server.pixel(static_cast<gui::WindowId>(f.id(window, "windowNumber")), x + 0.25, y + 0.25);
            if (p[0] < 110 && p[1] < 110 && p[2] < 110) {
                if (first < 0) first = x;
                last = x;
                break;
            }
        }
    return {first, last};
}

void alignments() {
    Fixture f;
    // Paragraph styles set to the arm64 NSTextAlignment values.
    std::array<Address, 3> styles{};
    for (std::uint64_t value = 0; value < 3; ++value) {
        const auto style = f.id(f.id(f.cls("NSMutableParagraphStyle"), "alloc"), "init");
        static_cast<void>(f.id(style, "setAlignment:", {value}));
        styles[value] = style;
    }
    const auto view_class = f.objc.register_native_class("TestAlignmentView", f.cls("NSView"), 8);
    static_cast<void>(f.objc.add_native_method(view_class, "drawRect:", [&f, styles](CpuState&, GuestMemory&) {
        const auto text = f.cf.string("HHHHHH");
        for (std::size_t band = 0; band < 3; ++band) {
            const std::array<std::pair<Address, Address>, 2> entries{{
                {f.cf.string("NSParagraphStyle"), styles[band]},
                {f.cf.string("NSColor"), f.id(f.cls("NSColor"), "blackColor")}}};
            const auto attributes = f.cf.dictionary(entries);
            CallRegs registers;
            // Bands from the top: left (y 70-100), center (40-70), right (10-40).
            gui::appkit::put_rect(registers, 0, {{0, 70.0 - 30.0 * static_cast<double>(band)}, {300, 30}});
            registers.x[2] = attributes;
            static_cast<void>(f.objc.send(text, f.objc.selector("drawInRect:withAttributes:"), registers));
        }
    }, false, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}"));

    CallRegs init;
    gui::appkit::put_rect(init, 0, {{100, 100}, {300, 100}});
    init.x[2] = gui::style::titled;
    init.x[3] = 2;
    init.x[4] = 0;
    const auto window = f.objc.send(f.id(f.cls("NSWindow"), "alloc"), f.objc.selector("initWithContentRect:styleMask:backing:defer:"), init).x0;
    static_cast<void>(f.id(window, "setReleasedWhenClosed:", {0}));
    static_cast<void>(f.id(window, "setBackgroundColor:", {f.id(f.cls("NSColor"), "whiteColor")}));
    CallRegs frame;
    gui::appkit::put_rect(frame, 0, {{0, 0}, {300, 100}});
    const auto view = f.objc.send(f.id(view_class, "alloc"), f.objc.selector("initWithFrame:"), frame).x0;
    static_cast<void>(f.id(f.id(window, "contentView"), "addSubview:", {view}));
    static_cast<void>(f.id(window, "makeKeyAndOrderFront:", {0}));
    static_cast<void>(f.id(f.app, "nextEventMatchingMask:untilDate:inMode:dequeue:", {~0ULL, 0, 0, 1}));

    const auto left = ink(f, window, 70, 100, 300), center = ink(f, window, 40, 70, 300), right = ink(f, window, 10, 40, 300);
    const auto describe = [](std::pair<int, int> span) { return "[" + std::to_string(span.first) + ", " + std::to_string(span.second) + "]"; };
    require(left.first >= 0 && center.first >= 0 && right.first >= 0, "every band has text: left " + describe(left) + " center " +
            describe(center) + " right " + describe(right));
    require(left.first < 10, "left-aligned text starts at the left edge: " + describe(left));
    const auto middle = (center.first + center.second) / 2;
    require(middle > 140 && middle < 160, "centered text (alignment 1) is centered: " + describe(center));
    require(right.second > 285, "right-aligned text (alignment 2) ends at the right edge: " + describe(right));
    std::cout << "alignments: left " << describe(left) << " center " << describe(center) << " right " << describe(right) << '\n';
    static_cast<void>(f.id(window, "close"));
}

} // namespace

int main() {
    try {
        alignments();
        std::cout << "Alchemy AppKit text alignment tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
