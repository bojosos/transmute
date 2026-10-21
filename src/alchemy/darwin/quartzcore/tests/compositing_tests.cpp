// CALayer tree compositing (owner: w3): a view's layer tree with an
// IOSurface on a sublayer (Chrome's DisplayCALayerTree) composited into its
// window on CATransaction commit, contents changed in place shown again, the
// implicit transaction at the next run-loop step, autoresizing, the capture,
// IOSurface properties, lock seeds and Mach ports, CVDisplayLink timing, and
// (g3) IOSurfaceSetPurgeable and the GPU-write synchronization and page
// lifetime that IOSurface-backed MTLTextures rely on (quartzcore_vulkan.hpp).
// Every provider is installed (run_program's order); presentation goes to the
// in-memory window server.
#include "alchemy/darwin/appkit/appkit_internal.hpp"
#include "alchemy/darwin/appkit/tests/fake_window_server.hpp"
#include "alchemy/darwin/quartzcore/quartzcore_vulkan.hpp"
#include "alchemy/gui/layer_contents.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/mach_kernel_objects.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
namespace gui = alchemy::gui;
using fill_test::Fixture;
using fill_test::require;
using Address = std::uint64_t;
using Pixel = std::array<std::uint8_t, 4>;   // B, G, R, A

constexpr std::string_view iosurface = "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface";
constexpr std::string_view corevideo = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
constexpr std::string_view coregraphics = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr Pixel red{0, 0, 255, 255}, blue{255, 0, 0, 255}, green{0, 255, 0, 255}, white{255, 255, 255, 255};

std::string text(const Pixel& p) {
    return "bgra(" + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," + std::to_string(p[3]) + ")";
}

struct Scene {
    Fixture& f;
    gui::testing::FakeWindowServer& server;
    Address window{}, view{}, root{}, flipped{}, content{}, surface{};
    gui::WindowId id{};

    alchemy::CallResult send(Address receiver, std::string_view selector, alchemy::CallRegs registers = {}) {
        return f.objc.send(receiver, f.objc.selector(selector), std::move(registers));
    }
    Address id_of(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        alchemy::CallRegs registers;
        std::size_t index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return send(receiver, selector, registers).x0;
    }
    Address cls(std::string_view name) {
        const auto value = f.objc.find_class(name);
        require(value != 0, "missing class " + std::string(name));
        return value;
    }
    void with_doubles(Address receiver, std::string_view selector, std::initializer_list<double> values) {
        alchemy::CallRegs registers;
        unsigned index = 0;
        for (const auto value : values) registers.set_d(index++, value);
        static_cast<void>(send(receiver, selector, registers));
    }
    void begin() { static_cast<void>(id_of(cls("CATransaction"), "begin")); }
    void commit() { static_cast<void>(id_of(cls("CATransaction"), "commit")); }
    Pixel at(double x, double y) { return server.pixel(id, x, y); }
    void expect(double x, double y, const Pixel& want, const std::string& what) {
        const auto got = at(x, y);
        require(got == want, what + " at (" + std::to_string(x) + "," + std::to_string(y) + "): " + text(got) + " want " + text(want));
    }
    // Fills the surface: the top `split` rows `top`, the rest `bottom`.
    void paint(const Pixel& top, const Pixel& bottom, std::uint32_t split) {
        const auto base = f.call(iosurface, "_IOSurfaceGetBaseAddress", {surface});
        const auto row = f.call(iosurface, "_IOSurfaceGetBytesPerRow", {surface});
        const auto width = f.call(iosurface, "_IOSurfaceGetWidth", {surface});
        const auto height = f.call(iosurface, "_IOSurfaceGetHeight", {surface});
        require(f.status(iosurface, "_IOSurfaceLock", {surface, 0, 0}) == 0, "IOSurfaceLock");
        std::vector<std::uint8_t> line(row);
        for (std::uint64_t y = 0; y < height; ++y) {
            const auto& p = y < split ? top : bottom;
            for (std::uint64_t x = 0; x < width; ++x) std::copy(p.begin(), p.end(), line.begin() + static_cast<std::ptrdiff_t>(x * 4));
            f.engine.write_bytes(base + y * row, line);
        }
        require(f.status(iosurface, "_IOSurfaceUnlock", {surface, 0, 0}) == 0, "IOSurfaceUnlock");
    }
};

Address number(Fixture& f, std::int64_t value) { return f.cf.integer(value, 9 /* kCFNumberSInt64Type */); }

Address make_surface(Fixture& f, std::int64_t width, std::int64_t height) {
    const std::array<std::pair<Address, Address>, 4> entries{{
        {f.cf.string("IOSurfaceWidth"), number(f, width)},
        {f.cf.string("IOSurfaceHeight"), number(f, height)},
        {f.cf.string("IOSurfaceBytesPerElement"), number(f, 4)},
        {f.cf.string("IOSurfacePixelFormat"), number(f, 0x42475241)}}};   // 'BGRA'
    const auto properties = f.cf.dictionary(entries);
    const auto surface = f.call(iosurface, "_IOSurfaceCreate", {properties});
    require(surface != 0, "IOSurfaceCreate");
    return surface;
}

void iosurface_properties_seeds_and_ports(Fixture& f) {
    const auto surface = make_surface(f, 200, 160);
    // Rows align to 64 bytes: 800 -> 832; the allocation to pages.
    require(f.call(iosurface, "_IOSurfaceGetBytesPerRow", {surface}) == 832, "bytes per row aligned to 64");
    require(f.call(iosurface, "_IOSurfaceGetAllocSize", {surface}) % 4096 == 0, "page-aligned allocation");
    require(f.call(iosurface, "_IOSurfaceGetPropertyAlignment", {f.cf.string("IOSurfaceBytesPerRow")}) == 64, "row alignment");
    require(f.call(iosurface, "_IOSurfaceAlignProperty", {f.cf.string("IOSurfaceBytesPerRow"), 801}) == 832, "IOSurfaceAlignProperty");
    require(f.call(iosurface, "_IOSurfaceGetPixelFormat", {surface}) == 0x42475241, "pixel format");
    // Seeds: a writing unlock changes the seed, a read-only one does not.
    const auto seed = f.block(4);
    require(f.status(iosurface, "_IOSurfaceLock", {surface, 1, seed}) == 0, "read-only lock");
    const auto first = f.engine.read32(seed);
    require(f.status(iosurface, "_IOSurfaceUnlock", {surface, 1, seed}) == 0 && f.engine.read32(seed) == first, "read-only unlock keeps the seed");
    require(f.status(iosurface, "_IOSurfaceLock", {surface, 0, seed}) == 0, "lock");
    require(f.status(iosurface, "_IOSurfaceUnlock", {surface, 0, seed}) == 0 && f.engine.read32(seed) == first + 1, "a writing unlock bumps the seed");
    require(f.call(iosurface, "_IOSurfaceGetSeed", {surface}) == first + 1, "IOSurfaceGetSeed");
    require(f.status(iosurface, "_IOSurfaceUnlock", {surface, 0, seed}) == 4, "unlocking an unlocked surface: kIOReturn... invalid argument");
    // Mach ports: a send right that finds the surface again (+1).
    const auto port = f.call(iosurface, "_IOSurfaceCreateMachPort", {surface});
    require(port != 0, "IOSurfaceCreateMachPort gives a send right");
    const auto found = f.call(iosurface, "_IOSurfaceLookupFromMachPort", {port});
    require(found == surface, "IOSurfaceLookupFromMachPort finds the surface");
    f.process.objects().release(found);
    require(f.call(iosurface, "_IOSurfaceLookupFromMachPort", {port + 4}) == 0, "another name finds nothing");
    const auto task = f.engine.read32(f.symbol(libsystem, "_mach_task_self_"));
    // The port's object for another process: its description and its pixel
    // pages; the receiver there makes a local surface over the mapped pages
    // (here: a copy of them in new vm_allocate'd pages), one per remote surface.
    {
        const auto ports = alchemy::mach_kernel_objects(f.process);
        require(ports != nullptr, "the Mach provider offers kernel-object ports");
        const auto wire = ports->object(static_cast<std::uint32_t>(port))->wire();
        require(wire && wire->kind == "IOSurface" && wire->size == f.call(iosurface, "_IOSurfaceGetAllocSize", {surface}) &&
                wire->address == f.call(iosurface, "_IOSurfaceGetBaseAddress", {surface}), "an IOSurface port describes its surface and pages");
        const auto receiver = alchemy::mach_kernel_object_receiver(f.process, "IOSurface");
        require(static_cast<bool>(receiver), "IOSurface receives its ports from other processes");
        const auto mapped_copy = [&] {
            const auto out = f.block(8);
            require(f.status(libsystem, "_vm_allocate", {task, out, wire->size, 1}) == 0, "vm_allocate");
            const auto at = f.engine.read64(out);
            std::vector<std::uint8_t> bytes(wire->size);
            f.engine.read_bytes(wire->address, bytes);
            f.engine.write_bytes(at, bytes);
            return at;
        };
        f.engine.write32(wire->address, 0x11223344);
        // Its own process already has the surface with that ID: the same one.
        {
            const auto own = receiver(wire->data, mapped_copy(), wire->size);
            const auto own_port = ports->make_port(own);
            const auto found = f.call(iosurface, "_IOSurfaceLookupFromMachPort", {own_port});
            require(found == surface, "a surface that arrives where it lives is that surface");
            f.process.objects().release(found);
            require(f.status(libsystem, "_mach_port_deallocate", {task, own_port}) == 0, "release the port");
        }
        // Another process's surface (an ID unseen here): a local surface over the pages.
        auto remote_data = wire->data;
        const std::uint32_t remote_id = 0x7ffffff0;
        std::memcpy(remote_data.data() + 4, &remote_id, 4);   // version, then the IOSurfaceID
        const auto first = receiver(remote_data, mapped_copy(), wire->size);
        require(first != nullptr, "a well-formed surface is received");
        const auto first_port = ports->make_port(first);
        const auto local = f.call(iosurface, "_IOSurfaceLookupFromMachPort", {first_port});
        require(local != 0 && local != surface, "the received port names a local surface");
        require(f.call(iosurface, "_IOSurfaceGetID", {local}) == remote_id, "with the remote surface's IOSurfaceID");
        const auto by_id = f.call(iosurface, "_IOSurfaceLookup", {remote_id});
        require(by_id == local, "IOSurfaceLookup finds it by that ID");
        f.process.objects().release(by_id);
        require(f.call(iosurface, "_IOSurfaceGetWidth", {local}) == 200 && f.call(iosurface, "_IOSurfaceGetHeight", {local}) == 160 &&
                f.call(iosurface, "_IOSurfaceGetBytesPerRow", {local}) == 832 &&
                f.call(iosurface, "_IOSurfaceGetPixelFormat", {local}) == 0x42475241, "with the remote surface's geometry");
        require(f.engine.read32(f.call(iosurface, "_IOSurfaceGetBaseAddress", {local})) == 0x11223344, "over the shared pages");
        const auto second = receiver(remote_data, mapped_copy(), wire->size);
        const auto second_port = ports->make_port(second);
        const auto again = f.call(iosurface, "_IOSurfaceLookupFromMachPort", {second_port});
        require(again == local, "the same remote surface arrives as the same local surface");
        f.process.objects().release(again);
        std::vector<std::uint8_t> truncated(wire->data.begin(), wire->data.begin() + 6);
        require(receiver(truncated, mapped_copy(), wire->size) == nullptr, "a malformed description arrives as a dead port");
        f.process.objects().release(local);
        require(f.status(libsystem, "_mach_port_deallocate", {task, first_port}) == 0 &&
                f.status(libsystem, "_mach_port_deallocate", {task, second_port}) == 0, "release the received ports");
    }
    require(f.status(libsystem, "_mach_port_deallocate", {task, port}) == 0, "the port's send right is released");
    require(f.call(iosurface, "_IOSurfaceLookupFromMachPort", {port}) == 0, "a released port names no surface");
    static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0, false));   // the port's surface reference goes
    f.process.objects().release(surface);
    std::cout << "iosurface properties, seeds and ports ok\n";
}

void chrome_layer_tree(Fixture& f, gui::testing::FakeWindowServer& server, const std::filesystem::path& captures) {
    Scene s{f, server};
    const auto app = s.id_of(s.cls("NSApplication"), "sharedApplication");
    static_cast<void>(s.id_of(app, "finishLaunching"));
    alchemy::CallRegs init;
    init.set_d(0, 100); init.set_d(1, 100); init.set_d(2, 50); init.set_d(3, 40);
    init.x[2] = gui::style::titled;
    init.x[3] = 2;
    init.x[4] = 0;
    s.window = s.send(s.id_of(s.cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    static_cast<void>(s.id_of(s.window, "setReleasedWhenClosed:", {0}));
    alchemy::CallRegs frame;
    frame.set_d(0, 0); frame.set_d(1, 0); frame.set_d(2, 50); frame.set_d(3, 40);
    s.view = s.send(s.id_of(s.cls("NSView"), "alloc"), "initWithFrame:", frame).x0;
    static_cast<void>(s.id_of(s.view, "setWantsLayer:", {1}));
    static_cast<void>(s.id_of(s.window, "setContentView:", {s.view}));
    static_cast<void>(s.id_of(s.window, "makeKeyAndOrderFront:", {0}));
    s.id = static_cast<gui::WindowId>(s.id_of(s.window, "windowNumber"));
    s.root = s.id_of(s.view, "layer");
    require(s.root != 0, "a layer-backed view has its layer");
    {
        alchemy::CallRegs rgb;
        rgb.set_d(0, 0); rgb.set_d(1, 1); rgb.set_d(2, 0); rgb.set_d(3, 1);
        const auto color = f.regs(coregraphics, "_CGColorCreateGenericRGB", rgb).x0;
        static_cast<void>(s.id_of(s.root, "setBackgroundColor:", {color}));
        f.call(coregraphics, "_CGColorRelease", {color});
    }
    const auto before = gui::layer_contents::compositions();
    // DisplayCALayerTree: a flipped layer that follows the root's size.
    s.begin();
    s.flipped = s.id_of(s.id_of(s.cls("CALayer"), "alloc"), "init");
    static_cast<void>(s.id_of(s.flipped, "setGeometryFlipped:", {1}));
    static_cast<void>(s.id_of(s.flipped, "setAutoresizingMask:", {2 | 16}));   // width and height sizable
    s.with_doubles(s.flipped, "setAnchorPoint:", {0, 0});
    static_cast<void>(s.id_of(s.root, "addSublayer:", {s.flipped}));
    s.commit();
    require(gui::layer_contents::compositions() == before, "a tree without contents is not composited");
    // The window grows: autoresizing gives the flipped layer the root's growth.
    alchemy::CallRegs size;
    size.set_d(0, 100); size.set_d(1, 80);
    static_cast<void>(s.send(s.window, "setContentSize:", size));
    static_cast<void>(s.send(s.view, "setFrameSize:", size));
    const auto root_bounds = s.send(s.root, "bounds");
    require(root_bounds.d(2) == 100 && root_bounds.d(3) == 80, "the backing layer follows the view");
    const auto flipped_bounds = s.send(s.flipped, "bounds");
    require(flipped_bounds.d(2) == 50 && flipped_bounds.d(3) == 40, "autoresizing grew the sublayer by the root's growth (0 -> 50x40)");
    alchemy::CallRegs fill;
    fill.set_d(0, 0); fill.set_d(1, 0); fill.set_d(2, 100); fill.set_d(3, 80);
    static_cast<void>(s.send(s.flipped, "setFrame:", fill));
    // The IOSurface layer: 100x80 pixels at contentsScale 2 = 50x40 points, top-left.
    s.surface = make_surface(f, 100, 80);
    s.paint(red, blue, 40);
    s.begin();
    s.content = s.id_of(s.id_of(s.cls("CALayer"), "alloc"), "init");
    require(f.cf.utf8(s.id_of(s.content, "contentsGravity")) == "resize", "kCAGravityResize by default");
    static_cast<void>(s.id_of(s.content, "setContentsGravity:", {f.cf.string("topLeft")}));
    s.with_doubles(s.content, "setAnchorPoint:", {0, 0});
    s.with_doubles(s.content, "setContentsScale:", {2});
    s.with_doubles(s.content, "setBounds:", {0, 0, 50, 40});
    static_cast<void>(s.id_of(s.flipped, "addSublayer:", {s.content}));
    static_cast<void>(s.id_of(s.content, "setContents:", {s.surface}));
    require(gui::layer_contents::compositions() == before, "nothing is composited before the commit");
    s.commit();
    require(gui::layer_contents::compositions() == before + 1, "the commit composites the tree once");
    require((s.id_of(s.content, "contentsAreFlipped") & 0xff) == 1, "the IOSurface layer's space is flipped");
    // The window is 100x80 points at scale 2; the surface covers the top-left 50x40.
    s.expect(10, 75, red, "the surface's first rows at the window's top-left");
    s.expect(10, 45, blue, "its last rows");
    s.expect(75, 75, green, "beside it the root's background");
    s.expect(10, 20, green, "below it the root's background");

    // Chrome draws the next frame into the same surface: -setContentsChanged.
    s.paint(white, white, 80);
    s.begin();
    static_cast<void>(s.id_of(s.content, "setContentsChanged"));
    s.commit();
    require(gui::layer_contents::compositions() == before + 2, "contents changed in place are composited again");
    s.expect(10, 75, white, "the new frame");

    // g3: a GPU-process frame whose GPU write is still pending when the
    // browser commits (Chrome hands surfaces over once scheduled): the
    // composition waits for the write and shows its pixels.
    {
        auto pages = gui::quartzcore::surface_pages(s.surface);
        require(pages.has_value(), "surface_pages");
        const auto ticket = gui::quartzcore::begin_gpu_write(*pages);
        std::thread gpu([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::vector<std::uint8_t> line(pages->bytes_per_row);
            for (std::uint32_t x = 0; x < pages->width; ++x) std::copy(red.begin(), red.end(), line.begin() + x * 4);
            for (std::uint32_t y = 0; y < pages->height; ++y) f.engine.write_bytes(pages->base + y * pages->bytes_per_row, line);
            static_cast<void>(gui::quartzcore::end_gpu_write(*pages, ticket));
        });
        s.begin();
        static_cast<void>(s.id_of(s.content, "setContentsChanged"));
        s.commit();
        gpu.join();
        s.expect(10, 75, red, "the GPU write the composition waited for");
        s.expect(10, 45, red, "all of it");
        s.paint(white, white, 80);
        s.begin();
        static_cast<void>(s.id_of(s.content, "setContentsChanged"));
        s.commit();
        s.expect(10, 75, white, "a CPU frame again");
    }

    // g3: contentsRect, as Chrome's CALayerTreeCoordinator sets it: the
    // bottom half of the two-colour surface (y from the top in the flipped
    // tree), stretched over the layer (kCAGravityResize), in the window and
    // in the capture; then the top half.
    {
        s.paint(red, blue, 40);
        s.begin();
        static_cast<void>(s.id_of(s.content, "setContentsGravity:", {f.cf.string("resize")}));
        s.with_doubles(s.content, "setContentsRect:", {0, 0.5, 1, 0.5});
        static_cast<void>(s.id_of(s.content, "setContentsChanged"));
        s.commit();
        s.expect(10, 75, blue, "the surface's bottom half fills the layer (top)");
        s.expect(10, 45, blue, "the surface's bottom half fills the layer (bottom)");
        s.expect(75, 75, green, "beside the layer the root's background");
        for (int step = 0; step < 40; ++step) {   // the capture throttle (250 ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0, false));
        }
        const auto bmp = captures / ("window-" + std::to_string(s.id) + ".bmp");
        std::ifstream in(bmp, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        require(bytes.size() == 54 + 200 * 160 * 4, "the capture is 200x160");
        const auto pixel = [&](int x, int y) {
            const auto at = 54 + (static_cast<std::size_t>(y) * 200 + static_cast<std::size_t>(x)) * 4;
            return Pixel{bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
        };
        require(pixel(10, 5) == blue && pixel(10, 79) == blue, "the capture shows the sub-rectangle: " + text(pixel(10, 5)));
        require(pixel(150, 5) == green, "the capture: the background beside it");
        s.begin();
        s.with_doubles(s.content, "setContentsRect:", {0, 0, 1, 0.5});
        s.commit();
        s.expect(10, 45, red, "y 0..0.5 is the surface's top half in the flipped tree");
        s.begin();
        static_cast<void>(s.id_of(s.content, "setContentsGravity:", {f.cf.string("topLeft")}));
        s.with_doubles(s.content, "setContentsRect:", {0, 0, 1, 1});
        s.paint(white, white, 80);
        static_cast<void>(s.id_of(s.content, "setContentsChanged"));
        s.commit();
        s.expect(10, 75, white, "the unit rectangle again");
    }

    // Outside an explicit transaction: shown at the next run-loop step.
    static_cast<void>(s.id_of(s.content, "setHidden:", {1}));
    require(gui::layer_contents::compositions() == before + 7, "the implicit transaction waits for the run loop");
    static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0, false));
    require(gui::layer_contents::compositions() == before + 8, "the run-loop step composites the implicit transaction");
    s.expect(10, 75, green, "a hidden layer is gone");
    static_cast<void>(s.id_of(s.content, "setHidden:", {0}));
    static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0, false));
    s.expect(10, 75, white, "shown again");

    // The capture: window-<n>.bmp of the composite (200x160 pixels at scale 2),
    // the last frame written once the 250 ms throttle is over.
    for (int step = 0; step < 40; ++step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0, false));
    }
    const auto bmp = captures / ("window-" + std::to_string(s.id) + ".bmp");
    require(std::filesystem::exists(bmp), "ALCHEMY_CONTENTS_CAPTURE wrote " + bmp.string());
    std::ifstream in(bmp, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(bytes.size() == 54 + 200 * 160 * 4, "a 200x160 32-bit BMP");
    const Pixel top_left{bytes[54], bytes[55], bytes[56], bytes[57]};
    require(top_left == white, "the capture holds the last frame: " + text(top_left));

    // The tree shows no contents any more: its area goes back to AppKit.
    const auto composed = gui::layer_contents::compositions();
    server.pending.clear();
    s.begin();
    static_cast<void>(s.id_of(s.flipped, "removeFromSuperlayer"));
    s.commit();
    require(gui::layer_contents::compositions() == composed, "nothing is composited for a tree without contents");
    require(std::any_of(server.pending.begin(), server.pending.end(), [&](const gui::Event& e) {
        return e.notification == gui::Notification::expose && e.window == s.id; }), "AppKit is asked to draw the area again");
    static_cast<void>(s.id_of(s.window, "orderOut:", {0}));
    f.process.objects().release(s.surface);
    std::cout << "chrome layer tree ok\n";
}

// The window closes while a transaction is open and the app keeps the view's
// layer tree (Chrome's DisplayCALayerTree outlives its NSView): the view is
// freed, the layer's unretained delegate still names it, and the commit must
// not message the freed view or window (regression: headless Chrome stopped
// with "invalid or destroyed compatibility object" in +[CATransaction commit]
// during -[NSWindow close]).
void window_closes_under_a_pending_commit(Fixture& f, gui::testing::FakeWindowServer& server) {
    Scene s{f, server};
    alchemy::CallRegs init;
    init.set_d(0, 300); init.set_d(1, 100); init.set_d(2, 40); init.set_d(3, 30);
    init.x[2] = gui::style::titled;
    init.x[3] = 2;
    init.x[4] = 0;
    s.window = s.send(s.id_of(s.cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", init).x0;
    static_cast<void>(s.id_of(s.window, "setReleasedWhenClosed:", {0}));
    alchemy::CallRegs frame;
    frame.set_d(0, 0); frame.set_d(1, 0); frame.set_d(2, 40); frame.set_d(3, 30);
    s.view = s.send(s.id_of(s.cls("NSView"), "alloc"), "initWithFrame:", frame).x0;
    // A layer-hosting view: the app's own layer, kept by the app.
    s.root = s.id_of(s.id_of(s.cls("CALayer"), "alloc"), "init");
    static_cast<void>(s.id_of(s.view, "setLayer:", {s.root}));
    static_cast<void>(s.id_of(s.view, "setWantsLayer:", {1}));
    static_cast<void>(s.id_of(s.window, "setContentView:", {s.view}));
    static_cast<void>(s.id_of(s.window, "makeKeyAndOrderFront:", {0}));
    s.id = static_cast<gui::WindowId>(s.id_of(s.window, "windowNumber"));
    s.surface = make_surface(f, 40, 30);
    s.paint(red, red, 30);
    s.begin();
    s.content = s.id_of(s.id_of(s.cls("CALayer"), "alloc"), "init");
    s.with_doubles(s.content, "setAnchorPoint:", {0, 0});
    s.with_doubles(s.content, "setBounds:", {0, 0, 40, 30});
    static_cast<void>(s.id_of(s.root, "addSublayer:", {s.content}));
    static_cast<void>(s.id_of(s.content, "setContents:", {s.surface}));
    s.commit();
    s.expect(5, 5, red, "the hosted tree is composited");
    // Shown contents are in use (the window server's use), until dropped.
    require(f.call(iosurface, "_IOSurfaceIsInUse", {s.surface}) == 1 && f.call(iosurface, "_IOSurfaceGetUseCount", {s.surface}) == 1,
            "an IOSurface shown as layer contents is in use");
    const auto composed = gui::layer_contents::compositions();
    // Close and free the view inside an open transaction; the app then
    // changes its (still retained) tree and commits.
    s.begin();
    static_cast<void>(s.id_of(s.content, "setContents:", {0}));
    require(f.call(iosurface, "_IOSurfaceIsInUse", {s.surface}) == 0, "contents replaced: no longer in use");
    static_cast<void>(s.id_of(s.window, "close"));
    static_cast<void>(s.id_of(s.window, "setContentView:", {0}));
    f.process.objects().release(s.view);   // the view's dealloc releases its layer reference only
    require(!f.process.objects().find(s.view), "the view is freed");
    static_cast<void>(s.id_of(s.content, "setContents:", {s.surface}));
    s.commit();
    static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0, false));
    require(gui::layer_contents::compositions() == composed, "a tree whose view is gone is not composited");
    static_cast<void>(s.id_of(s.content, "removeFromSuperlayer"));
    require(f.call(iosurface, "_IOSurfaceIsInUse", {s.surface}) == 1, "shown again: in use");
    f.process.objects().release(s.content);
    require(f.call(iosurface, "_IOSurfaceIsInUse", {s.surface}) == 0, "a freed layer's contents are no longer in use");
    f.process.objects().release(s.root);
    f.process.objects().release(s.surface);
    f.process.objects().release(s.window);
    std::cout << "window closes under a pending commit ok\n";
}

// CVDisplayLink: the output callback on the link's thread at the display's
// refresh rate (the fake display: 120 Hz), with increasing timestamps.
std::mutex link_mutex;
std::vector<std::pair<std::uint64_t, std::uint64_t>> link_stamps;   // now.hostTime, output.hostTime
void display_link_cadence(Fixture& f) {
    const auto handler = f.process.install_function("_alchemy_test_display_link", [](transmute::CpuState& c, transmute::GuestMemory& m) {
        std::lock_guard lock(link_mutex);
        link_stamps.emplace_back(m.read64(c.x[1] + 16), m.read64(c.x[2] + 16));
        c.x[0] = 0;
    });
    const auto out = f.block(8);
    require(f.status(corevideo, "_CVDisplayLinkCreateWithCGDisplay", {7, out}) == -6670, "an unknown display: kCVReturnInvalidDisplay");
    require(f.status(corevideo, "_CVDisplayLinkCreateWithCGDisplay", {1, out}) == 0, "CVDisplayLinkCreateWithCGDisplay(main display)");
    const auto link = f.engine.read64(out);
    require(f.call(corevideo, "_CVDisplayLinkGetCurrentCGDisplay", {link}) == 1, "the link's display");
    require(f.status(corevideo, "_CVDisplayLinkStart", {link}) == -6673, "no callback: kCVReturnCallbacksNotSet... (-6673)");
    require(f.status(corevideo, "_CVDisplayLinkSetOutputCallback", {link, handler, 0}) == 0, "set the output callback");
    const auto period = f.regs(corevideo, "_CVDisplayLinkGetNominalOutputVideoRefreshPeriod", [&] { alchemy::CallRegs r; r.x[0] = link; return r; }());
    require(period.x0 == 8333333 && period.x1 == 1000000000, "120 Hz nominal period in nanoseconds");
    require(f.status(corevideo, "_CVDisplayLinkStart", {link}) == 0, "start");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    require(f.status(corevideo, "_CVDisplayLinkStop", {link}) == 0, "stop");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> stamps;
    {
        std::lock_guard lock(link_mutex);
        stamps = link_stamps;
    }
    require(stamps.size() >= 10 && stamps.size() <= 45, "about 36 callbacks in 300 ms at 120 Hz, got " + std::to_string(stamps.size()));
    for (std::size_t i = 0; i < stamps.size(); ++i) {
        // Also when the host wakes the link late (a loaded machine).
        require(stamps[i].second > stamps[i].first, "the output time is after now");
        if (i) require(stamps[i].first > stamps[i - 1].first, "now increases");
    }
    const auto spacing = static_cast<double>(stamps.back().first - stamps.front().first) / static_cast<double>(stamps.size() - 1);
    require(spacing > 5e6 && spacing < 30e6, "callbacks about 8.3 ms apart, got " + std::to_string(spacing));
    std::size_t after_stop{};
    {
        std::lock_guard lock(link_mutex);
        after_stop = link_stamps.size();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        std::lock_guard lock(link_mutex);
        require(link_stamps.size() == after_stop, "no callbacks after the stop");
    }
    f.call(corevideo, "_CVDisplayLinkRelease", {link});
    std::cout << "display link cadence ok (" << stamps.size() << " frames)\n";
}

} // namespace

// g3: IOSurfaceSetPurgeable (moved here from the Chrome kit's media gaps,
// now state of the surface), and the GPU-write protocol of
// quartzcore_vulkan.hpp: a pending GPU write holds IOSurfaceLock (not with
// kIOSurfaceLockAvoidSync) until it ends, its end advances the seed, and the
// pages outlive the surface while SurfacePages holds them.
// A planar '420v' (NV12) surface as Chrome's gfx::CreateIOSurface describes a video frame: a luma plane and an
// interleaved chroma plane at half size, both in one allocation.
void iosurface_planar(Fixture& f) {
    const auto plane = [&](std::int64_t width, std::int64_t height, std::int64_t element, std::int64_t row,
                           std::int64_t size, std::int64_t offset) {
        const std::array<std::pair<Address, Address>, 6> entries{{
            {f.cf.string("IOSurfacePlaneWidth"), number(f, width)}, {f.cf.string("IOSurfacePlaneHeight"), number(f, height)},
            {f.cf.string("IOSurfacePlaneBytesPerElement"), number(f, element)},
            {f.cf.string("IOSurfacePlaneBytesPerRow"), number(f, row)}, {f.cf.string("IOSurfacePlaneSize"), number(f, size)},
            {f.cf.string("IOSurfacePlaneOffset"), number(f, offset)}}};
        return f.cf.dictionary(entries);
    };
    const auto create = [&](Address planes) {
        const std::array<std::pair<Address, Address>, 5> entries{{
            {f.cf.string("IOSurfaceWidth"), number(f, 854)}, {f.cf.string("IOSurfaceHeight"), number(f, 480)},
            {f.cf.string("IOSurfacePixelFormat"), number(f, 0x34323076)},   // '420v'
            {f.cf.string("IOSurfacePlaneInfo"), planes}, {f.cf.string("IOSurfaceAllocSize"), number(f, 645120)}}};
        return f.call(iosurface, "_IOSurfaceCreate", {f.cf.dictionary(entries)});
    };
    const std::array<Address, 2> nv12{plane(854, 480, 1, 896, 430080, 0), plane(427, 240, 2, 896, 215040, 430080)};
    const auto surface = create(f.cf.array(nv12));
    require(surface != 0, "IOSurfaceCreate of a planar surface");
    require(f.call(iosurface, "_IOSurfaceGetPlaneCount", {surface}) == 2, "two planes");
    require(f.call(iosurface, "_IOSurfaceGetWidth", {surface}) == 854 && f.call(iosurface, "_IOSurfaceGetHeight", {surface}) == 480,
            "the surface's size");
    require(f.call(iosurface, "_IOSurfaceGetWidthOfPlane", {surface, 1}) == 427 &&
            f.call(iosurface, "_IOSurfaceGetHeightOfPlane", {surface, 1}) == 240, "chroma plane size");
    require(f.call(iosurface, "_IOSurfaceGetBytesPerElementOfPlane", {surface, 0}) == 1 &&
            f.call(iosurface, "_IOSurfaceGetBytesPerElementOfPlane", {surface, 1}) == 2, "bytes per element of each plane");
    require(f.call(iosurface, "_IOSurfaceGetBytesPerRowOfPlane", {surface, 1}) == 896, "chroma bytes per row");
    require(f.call(iosurface, "_IOSurfaceGetWidthOfPlane", {surface, 2}) == 0, "no third plane");
    require(f.call(iosurface, "_IOSurfaceGetAllocSize", {surface}) >= 645120, "the allocation covers both planes");
    require(f.status(iosurface, "_IOSurfaceLock", {surface, 0, 0}) == 0, "lock");
    const auto base = f.call(iosurface, "_IOSurfaceGetBaseAddress", {surface});
    const auto chroma = f.call(iosurface, "_IOSurfaceGetBaseAddressOfPlane", {surface, 1});
    require(f.call(iosurface, "_IOSurfaceGetBaseAddressOfPlane", {surface, 0}) == base && chroma == base + 430080,
            "plane base addresses");
    f.engine.write32(chroma + 896 * 239 + 852, 0x80407f3fU);   // the last chroma element pair
    require(f.engine.read32(base + 430080 + 896 * 239 + 852) == 0x80407f3fU, "chroma pixels inside the allocation");
    require(f.status(iosurface, "_IOSurfaceUnlock", {surface, 0, 0}) == 0, "unlock");
    f.process.objects().release(surface);
    // A plane whose rows do not fit its bytes per row: NULL, as IOSurfaceCreate answers impossible surfaces.
    const std::array<Address, 1> narrow{plane(854, 480, 1, 512, 245760, 0)};
    require(create(f.cf.array(narrow)) == 0, "impossible plane: NULL");
}

void iosurface_purgeable_and_gpu_writes(Fixture& f) {
    const auto surface = make_surface(f, 64, 32);
    const auto old = f.block(4);
    require(f.status(iosurface, "_IOSurfaceSetPurgeable", {surface, 1, old}) == 0 && f.engine.read32(old) == 0, "volatile");
    require(f.status(iosurface, "_IOSurfaceSetPurgeable", {surface, 3, old}) == 0 && f.engine.read32(old) == 1, "keep current");
    require(f.status(iosurface, "_IOSurfaceSetPurgeable", {surface, 2, old}) == 0 && f.engine.read32(old) == 1, "empty");
    require(f.status(iosurface, "_IOSurfaceSetPurgeable", {surface, 0, old}) == 0 && f.engine.read32(old) == 1,
            "an emptied surface is only volatile (its pages are kept)");
    require(f.call(iosurface, "_IOSurfaceSetPurgeable", {surface, 4, old}) == 0xE00002C2, "another state: kIOReturnBadArgument");
    require(f.call(iosurface, "_IOSurfaceSetPurgeable", {f.cf.string("not a surface"), 0, old}) == 0xE00002C2,
            "not a surface: kIOReturnBadArgument");

    auto pages = gui::quartzcore::surface_pages(surface);
    require(pages && pages->surface == surface && pages->base == f.call(iosurface, "_IOSurfaceGetBaseAddress", {surface}) &&
            pages->width == 64 && pages->height == 32 && pages->bytes_per_row == 256 && pages->bytes_per_element == 4 &&
            pages->id == f.call(iosurface, "_IOSurfaceGetID", {surface}), "surface_pages describes the surface");
    require(!gui::quartzcore::surface_pages(f.cf.string("not a surface")), "surface_pages of another object");
    const auto seed = static_cast<std::uint32_t>(f.call(iosurface, "_IOSurfaceGetSeed", {surface}));
    require(pages->seed() == seed, "SurfacePages::seed is the surface's");
    const auto ticket = gui::quartzcore::begin_gpu_write(*pages);
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    require(f.status(iosurface, "_IOSurfaceLock", {surface, 3 /* read-only, avoid sync */, 0}) == 0 &&
            f.status(iosurface, "_IOSurfaceUnlock", {surface, 1, 0}) == 0, "lock with kIOSurfaceLockAvoidSync");
    require(clock::now() - start < std::chrono::milliseconds(500), "kIOSurfaceLockAvoidSync does not wait for the GPU");
    std::thread finisher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        static_cast<void>(gui::quartzcore::end_gpu_write(*pages, ticket));
    });
    start = clock::now();
    const auto locked = f.status(iosurface, "_IOSurfaceLock", {surface, 1, 0});
    const auto waited = clock::now() - start;
    finisher.join();
    require(locked == 0 && f.status(iosurface, "_IOSurfaceUnlock", {surface, 1, 0}) == 0, "lock after a GPU write");
    require(waited >= std::chrono::milliseconds(120), "IOSurfaceLock waits for the pending GPU write");
    require(f.call(iosurface, "_IOSurfaceGetSeed", {surface}) == seed + 1, "the GPU write's end advances the seed");
    start = clock::now();
    require(f.status(iosurface, "_IOSurfaceLock", {surface, 1, 0}) == 0 && f.status(iosurface, "_IOSurfaceUnlock", {surface, 1, 0}) == 0 &&
            clock::now() - start < std::chrono::milliseconds(500), "no pending write: no wait");

    // The pages outlive the surface object while SurfacePages holds them,
    // and are freed (on the main run loop) after the last holder.
    const auto base = pages->base;
    f.engine.write32(base, 0xabcdef01);
    f.process.objects().release(surface);
    static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0, false));
    require(f.engine.read32(base) == 0xabcdef01, "the pages stay while SurfacePages holds them");
    pages.reset();
    static_cast<void>(gui::main_run_loop().run_once(gui::default_mode, 0, false));
    bool freed = false;
    try { static_cast<void>(f.engine.read32(base)); } catch (const std::exception&) { freed = true; }
    require(freed, "the pages are freed after the last holder");
    std::cout << "iosurface purgeable, GPU writes and page lifetime ok\n";
}

int main() {
    const auto captures = std::filesystem::temp_directory_path() /
        ("alchemy_compositing_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(captures);
    _putenv_s("ALCHEMY_CONTENTS_CAPTURE", captures.string().c_str());
    try {
        gui::testing::FakeWindowServer server;
        gui::appkit::set_window_server_override(&server);
        gui::layer_contents::set_window_server_override(&server);
        Fixture f;
        iosurface_properties_seeds_and_ports(f);
        iosurface_purgeable_and_gpu_writes(f);
        iosurface_planar(f);
        chrome_layer_tree(f, server, captures);
        window_closes_under_a_pending_commit(f, server);
        display_link_cadence(f);
        gui::layer_contents::set_window_server_override(nullptr);
        gui::appkit::set_window_server_override(nullptr);
    } catch (const std::exception& error) {
        std::cerr << "compositing test failed: " << error.what() << "\n";
        return 1;
    }
    std::filesystem::remove_all(captures);
    std::cout << "Alchemy GUI QuartzCore compositing passed\n";
    return 0;
}
