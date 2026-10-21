#pragma once

// The Quartz 2D unit-test fixture (quartz2d_tests.cpp, and CoreText's
// coretext/tests/coretext_tests.cpp): a reference-engine process with
// Foundation, AppKit and Quartz 2D installed, export and message calls with
// the Apple ARM64 register image, and an RGBA bitmap context to read pixels
// back from.
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/darwin/coregraphics/q2d_internal.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace q2d = alchemy::gui::quartz2d;
using Address = std::uint64_t;
using alchemy::CallRegs;
using alchemy::CallResult;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool close_to(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

constexpr std::string_view cg = q2d::core_graphics_library;
constexpr std::string_view ct = q2d::core_text_library;
constexpr std::string_view io = q2d::image_io_library;
constexpr std::string_view cf_library = alchemy::core_foundation_library;
// CGImageAlphaInfo / byte order
constexpr Address premultiplied_last = 1, premultiplied_first = 2, order_32_little = 0x2000, none_skip_last = 5, alpha_last = 3;

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices darwin{process};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    std::unique_ptr<alchemy::FoundationBridge> foundation;

    // with_quartzcore: also QuartzCore and the IOSurface provider (install_support_kit), in that order.
    explicit Fixture(bool with_quartzcore = false) {
        alchemy::install_darwin_numeric(process);
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_appkit_gui(context);
        if (with_quartzcore) alchemy::gui::install_quartzcore(context);
        alchemy::gui::install_quartz2d(context);
        if (with_quartzcore) alchemy::gui::install_support_kit(context);
    }

    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing export " + std::string(name));
        return found->address;
    }
    CallResult call(std::string_view library, std::string_view name, std::initializer_list<Address> x = {},
                    std::initializer_list<double> v = {}, Address indirect = 0) {
        CallRegs registers;
        std::size_t index = 0;
        for (const auto value : x) registers.x.at(index++) = value;
        index = 0;
        for (const auto value : v) registers.set_d(static_cast<unsigned>(index++), value);
        registers.indirect_result = indirect;
        return alchemy::engine_call_regs(engine, symbol(library, name), registers);
    }
    Address id(std::string_view library, std::string_view name, std::initializer_list<Address> x = {}, std::initializer_list<double> v = {}) {
        return call(library, name, x, v).x0;
    }
    Address allocate(std::size_t size) {
        const auto address = process.allocate_guest(std::max<std::size_t>(size, 16));
        engine.fill_bytes(address, 0, std::max<std::size_t>(size, 16));
        return address;
    }
    std::vector<std::uint8_t> read(Address address, std::size_t size) {
        std::vector<std::uint8_t> bytes(size);
        engine.read_bytes(address, bytes);
        return bytes;
    }
    double read_double(Address address) { return std::bit_cast<double>(engine.read64(address)); }
    Address cls(std::string_view name) {
        const auto value = objc.find_class(name);
        require(value != 0, "missing class " + std::string(name));
        return value;
    }
    CallResult send(Address receiver, std::string_view selector, std::initializer_list<Address> x = {}, std::initializer_list<double> v = {}) {
        CallRegs registers;
        std::size_t index = 2;
        for (const auto value : x) registers.x.at(index++) = value;
        index = 0;
        for (const auto value : v) registers.set_d(static_cast<unsigned>(index++), value);
        return objc.send(receiver, objc.selector(selector), registers);
    }
    Address str(std::string_view text) { return cf.string(text); }
};

// An RGBA (premultiplied last) bitmap context with its own memory.
struct Canvas {
    Fixture& f;
    Address context{}, data{};
    std::uint32_t width{}, height{};
    Canvas(Fixture& fixture, std::uint32_t w, std::uint32_t h, Address info = premultiplied_last) : f(fixture), width(w), height(h) {
        const auto space = f.id(cg, "_CGColorSpaceCreateDeviceRGB");
        context = f.id(cg, "_CGBitmapContextCreate", {0, w, h, 8, 0, space, info});
        f.call(cg, "_CGColorSpaceRelease", {space});
        require(context != 0, "CGBitmapContextCreate failed");
        data = f.id(cg, "_CGBitmapContextGetData", {context});
        require(data != 0, "CGBitmapContextGetData returned NULL");
    }
    ~Canvas() { try { f.call(cg, "_CGContextRelease", {context}); } catch (...) {} }
    Address bytes_per_row() { return f.id(cg, "_CGBitmapContextGetBytesPerRow", {context}); }
    // Pixel at (x, y) with y counted from the TOP row of the bitmap.
    std::array<std::uint8_t, 4> at(std::uint32_t x, std::uint32_t y) {
        const auto bytes = f.read(data + y * bytes_per_row() + x * 4, 4);
        return {bytes[0], bytes[1], bytes[2], bytes[3]};
    }
    // The same in CoreGraphics coordinates (y up from the bottom).
    std::array<std::uint8_t, 4> cg_at(std::uint32_t x, std::uint32_t y) { return at(x, height - 1 - y); }
    void rgb_fill(double r, double g, double b, double a) { f.call(cg, "_CGContextSetRGBFillColor", {context}, {r, g, b, a}); }
    void fill(double x, double y, double w, double h) { f.call(cg, "_CGContextFillRect", {context}, {x, y, w, h}); }
};
std::string describe(std::array<std::uint8_t, 4> p) {
    return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," + std::to_string(p[3]);
}
bool same(std::array<std::uint8_t, 4> a, std::array<std::uint8_t, 4> b, int tolerance = 2) {
    for (int index = 0; index < 4; ++index) if (std::abs(a[index] - b[index]) > tolerance) return false;
    return true;
}

} // namespace
