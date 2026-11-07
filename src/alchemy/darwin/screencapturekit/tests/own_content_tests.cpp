// ScreenCaptureKit and the CoreGraphics window lists over the process's own
// content (screencapturekit/install.cpp): an own window (red), an own window
// shared with nobody (NSWindowSharingNone, blue) and a window the process did
// not make (a host window: created on the window server directly, green
// pixels presented by nobody in the process). Listings hold the red window
// only; screenshots, window-list images and stream frames show red where it
// is, black elsewhere on the display, and never a green pixel. Every provider
// is installed (run_program's order); presentation goes to the in-memory
// window server.
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"
#include "alchemy/gui/layer_contents.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
namespace gui = alchemy::gui;
using fill_test::Fixture;
using fill_test::require;
using Address = std::uint64_t;
using alchemy::CallRegs;

constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view cm = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view cv = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
constexpr std::string_view iosurface = "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

void pump(double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    do static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0.01, false));
    while (std::chrono::steady_clock::now() < end);
}
// A compiler-emitted (global) block whose invoke runs `handler` (x0 = the block, then the arguments).
Address block(Fixture& f, std::string name, transmute::GuestCallHandler handler) {
    const auto invoke = f.process.install_function(std::move(name), std::move(handler), "alchemy.test");
    const auto value = f.process.allocate_guest(32), descriptor = f.process.allocate_guest(16);
    f.engine.write64(descriptor, 0);
    f.engine.write64(descriptor + 8, 32);
    f.engine.write64(value, f.symbol(libsystem, "__NSConcreteGlobalBlock"));
    f.engine.write32(value + 8, 1U << 28);
    f.engine.write32(value + 12, 0);
    f.engine.write64(value + 16, invoke);
    f.engine.write64(value + 24, descriptor);
    return value;
}
Address window(Fixture& f, double x, double y, double width, double height, double r, double g, double b) {
    CallRegs init;
    init.set_d(0, x); init.set_d(1, y); init.set_d(2, width); init.set_d(3, height);
    init.x[2] = gui::style::titled;
    init.x[3] = 2;
    const auto made = f.objc.send(f.send(f.cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    static_cast<void>(f.send(made, "setReleasedWhenClosed:", {0}));
    CallRegs rgba;
    rgba.set_d(0, r); rgba.set_d(1, g); rgba.set_d(2, b); rgba.set_d(3, 1);
    const auto color = f.objc.send(f.cls("NSColor"), "colorWithRed:green:blue:alpha:", rgba).x0;
    static_cast<void>(f.send(made, "setBackgroundColor:", {color}));
    // A view that draws (a custom NSBox filled with the colour): AppKit then draws the window in software.
    CallRegs bounds;
    bounds.set_d(0, 0); bounds.set_d(1, 0); bounds.set_d(2, width); bounds.set_d(3, height);
    const auto box = f.objc.send(f.send(f.cls("NSBox"), "alloc"), "initWithFrame:", bounds).x0;
    static_cast<void>(f.send(box, "setBoxType:", {4}));       // NSBoxCustom
    static_cast<void>(f.send(box, "setBorderType:", {0}));    // NSNoBorder
    static_cast<void>(f.send(box, "setFillColor:", {color}));
    static_cast<void>(f.send(made, "setContentView:", {box}));
    static_cast<void>(f.send(made, "makeKeyAndOrderFront:", {0}));
    return made;
}
// The pixels of a CGImage (BGRA, rows top first).
std::vector<std::uint8_t> image_pixels(Fixture& f, Address image, std::uint32_t& width, std::uint32_t& height) {
    width = static_cast<std::uint32_t>(f.call(cg, "_CGImageGetWidth", {image}));
    height = static_cast<std::uint32_t>(f.call(cg, "_CGImageGetHeight", {image}));
    const auto space = f.call(cg, "_CGColorSpaceCreateDeviceRGB");
    const auto context = f.call(cg, "_CGBitmapContextCreate", {0, width, height, 8, width * 4ULL, space, 2 | 0x2000});
    CallRegs draw;
    draw.x[0] = context; draw.x[1] = image;
    draw.set_d(2, width); draw.set_d(3, height);
    static_cast<void>(f.regs(cg, "_CGContextDrawImage", draw));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
    f.engine.read_bytes(f.call(cg, "_CGBitmapContextGetData", {context}), out);
    return out;
}
bool green(const std::uint8_t* p) { return p[1] > 150 && p[2] < 80 && p[0] < 80; }
bool red(const std::uint8_t* p) { return p[2] > 150 && p[1] < 80 && p[0] < 80; }
bool black(const std::uint8_t* p) { return p[0] < 30 && p[1] < 30 && p[2] < 30 && p[3] == 255; }

void own_content(Fixture& f, gui::testing::FakeWindowServer& server) {
    require(f.call(cg, "_CGPreflightScreenCaptureAccess") == 1 && f.call(cg, "_CGRequestScreenCaptureAccess") == 1, "granted for own content");
    const auto app = f.send(f.cls("NSApplication"), "sharedApplication");
    static_cast<void>(f.send(app, "finishLaunching"));
    // Cocoa coordinates on the 1440 x 900 display: red at (100, 500) 200 x 100; blue (shared with nobody) at (500, 500).
    const auto own = window(f, 100, 500, 200, 100, 1, 0, 0);
    const auto hidden = window(f, 500, 500, 100, 100, 0, 0, 1);
    static_cast<void>(f.send(hidden, "setSharingType:", {0}));
    // A host window over part of the red one and beyond it: green pixels the process never presented.
    gui::WindowDesc desc;
    desc.content_rect = {{250, 450}, {200, 200}};
    desc.visible = true;
    const auto host = server.create_window(desc, nullptr);
    server.z_order.insert(server.z_order.begin(), host);
    const auto host_scale = server.backing_scale(host);
    const auto hw = static_cast<std::uint32_t>(200 * host_scale), hh = static_cast<std::uint32_t>(200 * host_scale);
    std::vector<std::uint8_t> green_pixels(static_cast<std::size_t>(hw) * hh * 4);
    for (std::size_t i = 0; i < green_pixels.size(); i += 4) { green_pixels[i + 1] = 255; green_pixels[i + 3] = 255; }
    server.present_bitmap(host, {hw, hh, static_cast<std::size_t>(hw) * 4, green_pixels.data()}, std::nullopt);
    // AppKit displays the own windows while it takes events.
    for (const auto shown : {own, hidden}) static_cast<void>(f.send(shown, "display"));
    for (int i = 0; i < 3; ++i) static_cast<void>(f.send(app, "nextEventMatchingMask:untilDate:inMode:dequeue:", {~0ULL, 0, 0, 1}));
    pump(0.05);
    const auto own_id = f.send(own, "windowNumber"), hidden_id = f.send(hidden, "windowNumber");

    // Listings: the red window, the application, the display.
    Address content{};
    const auto handler = block(f, "sck_test_content", [&f, &content](transmute::CpuState& s, transmute::GuestMemory&) {
        content = f.send(s.x[1], "retain");
    });
    static_cast<void>(f.send(f.cls("SCShareableContent"), "getShareableContentWithCompletionHandler:", {handler}));
    for (int i = 0; i < 100 && !content; ++i) pump(0.01);
    require(content != 0, "shareable content delivered");
    const auto windows = f.send(content, "windows");
    require(f.cf.length(windows) == 1 && f.send(f.cf.element(windows, 0), "windowID") == own_id, "only the own shared window is listed");
    require(f.cf.length(f.send(content, "applications")) == 1 && f.cf.length(f.send(content, "displays")) == 1, "one application, one display");
    const auto listed_frame = f.objc.send(f.cf.element(windows, 0), "frame");
    require(listed_frame.d(0) == 100 && listed_frame.d(1) == 300 && listed_frame.d(2) == 200, "the window frame in CG coordinates");
    const auto info = f.call(cg, "_CGWindowListCopyWindowInfo", {0, 0});
    require(f.cf.length(info) == 1, "CGWindowListCopyWindowInfo: the own window only");
    std::array<Address, 3> asked{own_id, host, hidden_id};
    const auto asked_values = f.process.allocate_guest(24);
    for (unsigned i = 0; i < 3; ++i) f.engine.write64(asked_values + i * 8, asked[i]);
    const auto described = f.call(cg, "_CGWindowListCreateDescriptionFromArray", {f.call(cf, "_CFArrayCreate", {0, asked_values, 3, 0})});
    require(f.cf.length(described) == 1, "descriptions leave the host window and the unshared one out");

    // A display screenshot at one pixel per point.
    const auto display = f.cf.element(f.send(content, "displays"), 0);
    const auto filter = f.send(f.send(f.cls("SCContentFilter"), "alloc"), "initWithDisplay:excludingWindows:", {display, f.cf.array({})});
    const auto config = f.send(f.send(f.cls("SCStreamConfiguration"), "alloc"), "init");
    static_cast<void>(f.send(config, "setWidth:", {1440}));
    static_cast<void>(f.send(config, "setHeight:", {900}));
    Address shot{};
    const auto shot_handler = block(f, "sck_test_shot", [&f, &shot](transmute::CpuState& s, transmute::GuestMemory&) { shot = f.call(cf, "_CFRetain", {s.x[1]}); });
    static_cast<void>(f.send(f.cls("SCScreenshotManager"), "captureImageWithFilter:configuration:completionHandler:", {filter, config, shot_handler}));
    for (int i = 0; i < 100 && !shot; ++i) pump(0.01);
    require(shot != 0, "a screenshot");
    std::uint32_t width{}, height{};
    const auto pixels = image_pixels(f, shot, width, height);
    require(width == 1440 && height == 900, "the screenshot's size");
    const auto at = [&](int x, int y) { return &pixels[(static_cast<std::size_t>(y) * width + x) * 4]; };   // CG points (top-left)
    const auto describe = [&](int x, int y) {
        const auto* p = at(x, y);
        const auto found = server.screens_by_window.find(static_cast<gui::WindowId>(own_id));
        return std::string(" visible ") + std::to_string(f.send(own, "isVisible") & 0xff) + " server-visible " +
               std::to_string(server.is_visible(static_cast<gui::WindowId>(own_id))) +
               " (" + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," + std::to_string(p[3]) + "; presents " +
               std::to_string(found == server.screens_by_window.end() ? -1 : found->second.presents) + ")";
    };
    require(red(at(150, 350)), "the own window's pixels" + describe(150, 350));
    require(red(at(275, 320)), "the own window where the host window overlaps it");
    require(black(at(400, 400)), "the host window's area is black");
    require(black(at(550, 350)), "the unshared window's area is black");
    for (std::size_t i = 0; i < pixels.size(); i += 4) require(!green(&pixels[i]), "no host pixel anywhere in the screenshot");

    // CGWindowListCreateImage of the on-screen windows (CGRectNull: their bounds).
    CallRegs image_request;
    image_request.set_d(0, std::numeric_limits<double>::infinity()); image_request.set_d(1, std::numeric_limits<double>::infinity());
    image_request.set_d(2, 0); image_request.set_d(3, 0);
    image_request.x[0] = 1;   // kCGWindowListOptionOnScreenOnly
    const auto listed_image = f.regs(cg, "_CGWindowListCreateImage", image_request).x0;
    require(listed_image != 0, "CGWindowListCreateImage");
    std::uint32_t lw{}, lh{};
    const auto listed_pixels = image_pixels(f, listed_image, lw, lh);
    require(lw == 2 * lh && red(&listed_pixels[(static_cast<std::size_t>(lh / 2) * lw + lw / 2) * 4]), "the image is the own window");
    for (std::size_t i = 0; i < listed_pixels.size(); i += 4) require(!green(&listed_pixels[i]), "no host pixel in the window image");

    // A stream: frames of the display at 144 x 90 (ten points per pixel).
    Address sample{};
    const auto output_class = f.objc.register_native_class("SCKTestOutput", f.cls("NSObject"), 8);
    static_cast<void>(f.objc.add_native_method(output_class, "stream:didOutputSampleBuffer:ofType:", [&f, &sample](transmute::CpuState& s, transmute::GuestMemory&) {
        if (!sample) sample = f.call(cf, "_CFRetain", {s.x[3]});
    }, false, "v@:@^{opaqueCMSampleBuffer=}q"));
    const auto output = f.send(f.send(output_class, "alloc"), "init");
    static_cast<void>(f.send(config, "setWidth:", {144}));
    static_cast<void>(f.send(config, "setHeight:", {90}));
    const auto stream = f.send(f.send(f.cls("SCStream"), "alloc"), "initWithFilter:configuration:delegate:", {filter, config, 0});
    const auto error = f.process.allocate_guest(8);
    require(f.send(stream, "addStreamOutput:type:sampleHandlerQueue:error:", {output, 1, 0, error}) == 0 && f.engine.read64(error) != 0,
            "no audio output");
    require(f.send(stream, "addStreamOutput:type:sampleHandlerQueue:error:", {output, 0, 0, error}) == 1, "a screen output");
    static_cast<void>(f.send(stream, "startCaptureWithCompletionHandler:", {0}));
    for (int i = 0; i < 100 && !sample; ++i) pump(0.01);
    static_cast<void>(f.send(stream, "stopCaptureWithCompletionHandler:", {0}));
    require(sample != 0, "a stream frame");
    const auto attachments = f.call(cm, "_CMSampleBufferGetSampleAttachmentsArray", {sample, 0});
    const auto status_key = f.engine.read64(f.symbol("/System/Library/Frameworks/ScreenCaptureKit.framework/Versions/A/ScreenCaptureKit", "_SCStreamFrameInfoStatus"));
    require(attachments && f.cf.number_value(f.cf.dictionary_value(f.cf.element(attachments, 0), status_key)) == 0.0, "frame status complete");
    const auto surface = f.call(cv, "_CVPixelBufferGetIOSurface", {f.call(cm, "_CMSampleBufferGetImageBuffer", {sample})});
    require(surface && f.call(iosurface, "_IOSurfaceGetWidth", {surface}) == 144 && f.call(iosurface, "_IOSurfaceGetHeight", {surface}) == 90, "the frame's surface");
    const auto base = f.call(iosurface, "_IOSurfaceGetBaseAddress", {surface}), row = f.call(iosurface, "_IOSurfaceGetBytesPerRow", {surface});
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(row) * 90);
    f.engine.read_bytes(base, frame);
    require(red(&frame[35 * row + 15 * 4]), "the own window in the stream frame");
    for (std::uint32_t y = 0; y < 90; ++y)
        for (std::uint32_t x = 0; x < 144; ++x) require(!green(&frame[y * row + x * 4]), "no host pixel in the stream frame");
    std::cout << "own content: listings, screenshot, window image and stream ok\n";
}

} // namespace

int main() {
    try {
        gui::testing::FakeWindowServer server;
        gui::appkit::set_window_server_override(&server);
        gui::layer_contents::set_window_server_override(&server);
        {
            Fixture f;
            own_content(f, server);
        }
        gui::layer_contents::set_window_server_override(nullptr);
        gui::appkit::set_window_server_override(nullptr);
    } catch (const std::exception& error) {
        std::cerr << "ScreenCaptureKit own content test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "Alchemy GUI ScreenCaptureKit own content passed\n";
    return 0;
}
