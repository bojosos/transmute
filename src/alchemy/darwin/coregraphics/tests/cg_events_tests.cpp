// CoreGraphics events and sources (src/alchemy/darwin/coregraphics/graphics_gaps.cpp):
// keyboard, mouse and scroll events made by the application, their fields,
// flags, timestamps and characters, copies, and event sources; transform
// components and display capture.
#include "alchemy/gui/tests/framework_case.hpp"

#include <bit>
#include <cmath>

namespace {
using namespace fill_test;

constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";

const Case coregraphics_events_case("coregraphics_events", [](Fixture& f) {
    const auto source = f.call(cg, "_CGEventSourceCreate", {1});   // kCGEventSourceStateHIDSystemState
    require(source && f.call(cg, "_CGEventSourceGetSourceStateID", {source}) == 1, "an event source keeps its state ID");
    static_cast<void>(f.call(cg, "_CGEventSourceSetUserData", {source, 77}));
    require(f.call(cg, "_CGEventSourceGetUserData", {source}) == 77, "source user data");

    // A key press of kVK_ANSI_A (0) made with the source.
    const auto key = f.call(cg, "_CGEventCreateKeyboardEvent", {source, 0, 1});
    require(f.call(cg, "_CGEventGetType", {key}) == 10, "kCGEventKeyDown");
    require(f.call(cg, "_CGEventGetIntegerValueField", {key, 9}) == 0 && f.call(cg, "_CGEventGetIntegerValueField", {key, 45}) == 1 &&
                f.call(cg, "_CGEventGetIntegerValueField", {key, 42}) == 77, "keycode, source state and user data fields");
    static_cast<void>(f.call(cg, "_CGEventSetFlags", {key, 1ULL << 20}));
    require(f.call(cg, "_CGEventGetFlags", {key}) == (1ULL << 20), "flags");
    static_cast<void>(f.call(cg, "_CGEventSetTimestamp", {key, 1234}));
    require(f.call(cg, "_CGEventGetTimestamp", {key}) == 1234, "timestamp");
    const auto units = f.block(8);
    f.engine.write64(units, 'h' | (std::uint64_t{'i'} << 16));   // UTF-16 "hi"
    static_cast<void>(f.call(cg, "_CGEventKeyboardSetUnicodeString", {key, 2, units}));
    const auto length = f.block(8), back = f.block(8);
    static_cast<void>(f.call(cg, "_CGEventKeyboardGetUnicodeString", {key, 4, length, back}));
    require(f.engine.read64(length) == 2 && (f.engine.read64(back) & 0xffffffff) == ('h' | (std::uint64_t{'i'} << 16)), "the event's characters");

    // Double fields, copies, a mouse event and a scroll event.
    alchemy::CallRegs set{};
    set.x[0] = key; set.x[1] = 12;
    set.set_d(0, 2.5);
    static_cast<void>(f.regs(cg, "_CGEventSetDoubleValueField", set));
    alchemy::CallRegs get{};
    get.x[0] = key; get.x[1] = 12;
    require(f.regs(cg, "_CGEventGetDoubleValueField", get).d(0) == 2.5, "a double field");
    const auto copy = f.call(cg, "_CGEventCreateCopy", {key});
    require(copy != key && f.call(cg, "_CGEventGetType", {copy}) == 10 && f.call(cg, "_CGEventGetFlags", {copy}) == (1ULL << 20), "a copy");
    alchemy::CallRegs mouse{};
    mouse.x[0] = 0; mouse.x[1] = 1; mouse.x[2] = 0;   // kCGEventLeftMouseDown, kCGMouseButtonLeft
    mouse.set_d(0, 40); mouse.set_d(1, 50);
    const auto click = f.regs(cg, "_CGEventCreateMouseEvent", mouse).x0;
    alchemy::CallRegs where{};
    where.x[0] = click;
    const auto location = f.regs(cg, "_CGEventGetLocation", where);
    require(f.call(cg, "_CGEventGetType", {click}) == 1 && location.d(0) == 40 && location.d(1) == 50, "a mouse event at its location");
    const auto scroll = f.call(cg, "_CGEventCreateScrollWheelEvent2", {0, 0, 2, 5, 7, 0});   // pixel units
    require(f.call(cg, "_CGEventGetType", {scroll}) == 22 && f.call(cg, "_CGEventGetIntegerValueField", {scroll, 96}) == 5 &&
                f.call(cg, "_CGEventGetIntegerValueField", {scroll, 97}) == 7 && f.call(cg, "_CGEventGetIntegerValueField", {scroll, 88}) == 1,
            "a continuous pixel scroll");
    require(f.call(cg, "_CGEventGetTypeID") != 0 && f.call(cg, "_CGEventSourceGetTypeID") != 0, "type IDs");

    // CGAffineTransformComponents round trip (48-byte structs by reference both ways).
    const auto components = f.block(48), transform = f.block(48), decomposed = f.block(48);
    const double values[6] = {2, 3, 0.5, 0.3, 10, 20};
    for (unsigned i = 0; i < 6; ++i) f.engine.write64(components + i * 8, std::bit_cast<std::uint64_t>(values[i]));
    alchemy::CallRegs make{};
    make.x[0] = components;
    make.indirect_result = transform;
    static_cast<void>(f.regs(cg, "_CGAffineTransformMakeWithComponents", make));
    alchemy::CallRegs decompose{};
    decompose.x[0] = transform;
    decompose.indirect_result = decomposed;
    static_cast<void>(f.regs(cg, "_CGAffineTransformDecompose", decompose));
    for (unsigned i = 0; i < 6; ++i)
        require(std::abs(std::bit_cast<double>(f.engine.read64(decomposed + i * 8)) - values[i]) < 1e-9, "transform components round trip");
    // Display capture is not available; releasing succeeds.
    require(static_cast<std::int32_t>(f.call(cg, "_CGDisplayCapture", {1})) == 1004 && f.call(cg, "_CGDisplayRelease", {1}) == 0 &&
                static_cast<std::int32_t>(f.call(cg, "_CGCaptureAllDisplays")) == 1004, "display capture is refused");
});

} // namespace
