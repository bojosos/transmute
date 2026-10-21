// QuartzCore Objective-C providers (owner: A7): CALayer, CAMetalLayer,
// CAMetalDrawable and CATransaction through the Objective-C runtime, with
// presentation backed by the Vulkan test device when one exists; (g3)
// CAMetalDisplayLink on the main run loop and the CADynamicRange keys.
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"
#include "alchemy/darwin/quartzcore/quartzcore_vulkan.hpp"
#include "alchemy/darwin/quartzcore/tests/vulkan_test_device.hpp"
#include "alchemy/gui/runloop.hpp"

#include <bit>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Address = std::uint64_t;
namespace qc = alchemy::gui::quartzcore;
constexpr std::string_view quartz = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";

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
    Fixture() {
        alchemy::install_darwin_numeric(process);
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::install_darwin_gui(process, objc, cf);
    }
    ~Fixture() { objc.cleanup_thread(*process.thread(engine)); }
    Address cls(std::string_view name) {
        const auto value = objc.find_class(name);
        require(value != 0, "missing class " + std::string(name));
        return value;
    }
    alchemy::CallResult send(Address receiver, std::string_view selector, alchemy::CallRegs registers = {}) {
        return objc.send(receiver, selector, std::move(registers));
    }
    Address id(Address receiver, std::string_view selector, Address argument = 0) {
        alchemy::CallRegs registers;
        registers.x[2] = argument;
        return send(receiver, selector, registers).x0;
    }
    Address make(std::string_view name) { return id(id(cls(name), "alloc"), "init"); }
    void set_rect(Address receiver, std::string_view selector, double x, double y, double w, double h) {
        alchemy::CallRegs registers;
        registers.set_d(0, x); registers.set_d(1, y); registers.set_d(2, w); registers.set_d(3, h);
        static_cast<void>(send(receiver, selector, registers));
    }
    void set_size(Address receiver, std::string_view selector, double w, double h) {
        alchemy::CallRegs registers;
        registers.set_d(0, w); registers.set_d(1, h);
        static_cast<void>(send(receiver, selector, registers));
    }
    void set_real(Address receiver, std::string_view selector, double value) {
        alchemy::CallRegs registers;
        registers.set_d(0, value);
        static_cast<void>(send(receiver, selector, registers));
    }
    std::array<double, 4> rect(Address receiver, std::string_view selector) {
        const auto result = send(receiver, selector);
        return {result.d(0), result.d(1), result.d(2), result.d(3)};
    }
    std::uint64_t retain_count(Address object) { return id(object, "retainCount"); }
    template <class F> bool fails(F&& action, std::string_view needle = {}) {
        try { action(); } catch (const std::exception& error) {
            return needle.empty() || std::string(error.what()).find(needle) != std::string::npos;
        }
        return false;
    }
};

void layers(Fixture& f) {
    const auto layer = f.make("CALayer");
    require(layer && f.objc.object_class(layer) == f.cls("CALayer"), "CALayer did not instantiate");
    f.set_rect(layer, "setFrame:", 10, 20, 300, 200);
    auto frame = f.rect(layer, "frame");
    require(frame == std::array<double, 4>{10, 20, 300, 200}, "frame did not round-trip");
    auto bounds = f.rect(layer, "bounds");
    require(bounds[2] == 300 && bounds[3] == 200 && bounds[0] == 0, "bounds do not follow the frame");
    const auto position = f.send(layer, "position");
    require(position.d(0) == 160 && position.d(1) == 120, "position is not frame origin + anchor * size");
    f.set_rect(layer, "setBounds:", 0, 0, 100, 50);
    frame = f.rect(layer, "frame");
    require(frame == std::array<double, 4>{110, 95, 100, 50}, "bounds change did not keep the position");
    require(f.send(layer, "contentsScale").d(0) == 1.0, "default contentsScale is not 1");
    f.set_real(layer, "setContentsScale:", 2.0);
    require(f.send(layer, "contentsScale").d(0) == 2.0, "contentsScale did not change");
    require(f.id(layer, "isOpaque") == 0, "layers are opaque by default");
    static_cast<void>(f.id(layer, "setOpaque:", 1));
    require(f.id(layer, "isOpaque") == 1, "setOpaque: lost");
    require(f.id(layer, "edgeAntialiasingMask") == 15, "default edge antialiasing mask is not all edges");
    static_cast<void>(f.id(layer, "setEdgeAntialiasingMask:", 0));
    require(f.id(layer, "edgeAntialiasingMask") == 0, "setEdgeAntialiasingMask: lost");
    static_cast<void>(f.id(layer, "setNeedsDisplay"));
    require(f.id(layer, "needsDisplay") == 1, "setNeedsDisplay did not mark the layer");
    static_cast<void>(f.id(layer, "displayIfNeeded"));   // no delegate: just clears the flag
    require(f.id(layer, "needsDisplay") == 0, "displayIfNeeded left the layer dirty");
    static_cast<void>(f.id(layer, "removeAllAnimations"));

    // Delegate is not retained; sublayers are.
    const auto delegate = f.make("NSObject");
    const auto before = f.retain_count(delegate);
    static_cast<void>(f.id(layer, "setDelegate:", delegate));
    require(f.id(layer, "delegate") == delegate && f.retain_count(delegate) == before, "delegate retained or lost");
    const auto child = f.make("CALayer");
    static_cast<void>(f.id(layer, "addSublayer:", child));
    require(f.id(child, "superlayer") == layer && f.retain_count(child) == 2, "addSublayer: did not retain/link");
    static_cast<void>(f.id(child, "removeFromSuperlayer"));
    require(f.id(child, "superlayer") == 0 && f.retain_count(child) == 1, "removeFromSuperlayer did not unlink/release");
    static_cast<void>(f.id(layer, "addSublayer:", child));
    const auto list = f.id(layer, "sublayers");   // autoreleased; retains its elements
    require(list && f.cf.length(list) == 1 && f.cf.element(list, 0) == child, "sublayers is wrong");
    static_cast<void>(f.id(child, "removeFromSuperlayer"));
    static_cast<void>(f.id(layer, "addSublayer:", child));
    static_cast<void>(f.id(child, "release"));   // the parent keeps it
    static_cast<void>(f.id(layer, "release"));   // releases the child too
    static_cast<void>(f.id(delegate, "release"));
    require(f.fails([&] { static_cast<void>(f.id(f.make("CALayer"), "addSublayer:", 0)); }), "nil sublayer accepted");

    // +layer returns an autoreleased instance of the receiver class.
    const auto metal = f.id(f.cls("CAMetalLayer"), "layer");
    require(metal && f.objc.object_class(metal) == f.cls("CAMetalLayer"), "+[CAMetalLayer layer] has the wrong class");
    std::cout << "layers ok\n";
}

// Transforms (indirect CATransform3D), sibling insertion, replacement,
// cornerCurve, CAGradientLayer and animation bookkeeping without a run loop.
void layer_additions(Fixture& f) {
    const auto layer = f.make("CALayer");
    const auto buffer = f.process.allocate_guest(128);
    const auto read_transform = [&](Address object, std::string_view selector) {
        alchemy::CallRegs registers;
        registers.indirect_result = buffer;
        static_cast<void>(f.send(object, selector, registers));
        std::array<double, 16> t{};
        for (unsigned i = 0; i < 16; ++i) t[i] = std::bit_cast<double>(f.engine.read64(buffer + i * 8));
        return t;
    };
    const std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    require(read_transform(layer, "transform") == identity, "default transform is not the identity");
    // A 90-degree rotation about z and a translation.
    std::array<double, 16> rotation = identity;
    rotation[0] = 0; rotation[1] = 1; rotation[4] = -1; rotation[5] = 0; rotation[12] = 5;
    for (unsigned i = 0; i < 16; ++i) f.engine.write64(buffer + i * 8, std::bit_cast<std::uint64_t>(rotation[i]));
    static_cast<void>(f.id(layer, "setTransform:", buffer));
    require(read_transform(layer, "transform") == rotation, "setTransform: did not round-trip");
    f.set_rect(layer, "setBounds:", 0, 0, 40, 20);
    alchemy::CallRegs position;
    position.set_d(0, 100); position.set_d(1, 100);
    static_cast<void>(f.send(layer, "setPosition:", position));
    const auto frame = f.rect(layer, "frame");
    require(std::abs(frame[0] - 95) < 1e-9 && std::abs(frame[1] - 80) < 1e-9 && std::abs(frame[2] - 20) < 1e-9 &&
            std::abs(frame[3] - 40) < 1e-9, "frame is not the rotated bounding box");
    require(f.cf.utf8(f.id(layer, "cornerCurve")) == "circular", "default cornerCurve is not circular");
    static_cast<void>(f.id(layer, "setCornerCurve:", f.cf.string("continuous")));
    require(f.cf.utf8(f.id(layer, "cornerCurve")) == "continuous", "setCornerCurve: lost");

    // insertSublayer:above: / below:, replaceSublayer:with:.
    const auto a = f.make("CALayer"), b = f.make("CALayer"), c = f.make("CALayer"), d = f.make("CALayer");
    static_cast<void>(f.id(layer, "addSublayer:", a));
    static_cast<void>(f.id(layer, "addSublayer:", b));
    alchemy::CallRegs above;
    above.x[2] = c; above.x[3] = a;
    static_cast<void>(f.send(layer, "insertSublayer:above:", above));
    alchemy::CallRegs below;
    below.x[2] = d; below.x[3] = a;
    static_cast<void>(f.send(layer, "insertSublayer:below:", below));
    auto list = f.id(layer, "sublayers");
    require(f.cf.length(list) == 4 && f.cf.element(list, 0) == d && f.cf.element(list, 1) == a && f.cf.element(list, 2) == c &&
            f.cf.element(list, 3) == b, "insertSublayer:above:/below: order is wrong");
    alchemy::CallRegs replace;
    replace.x[2] = c; replace.x[3] = f.make("CALayer");
    static_cast<void>(f.send(layer, "replaceSublayer:with:", replace));
    list = f.id(layer, "sublayers");
    require(f.cf.length(list) == 4 && f.cf.element(list, 2) == replace.x[3] && f.id(c, "superlayer") == 0,
            "replaceSublayer:with: did not swap in place");
    require(f.fails([&] { static_cast<void>(f.send(layer, "replaceSublayer:with:", replace)); }), "replacing a non-sublayer accepted");
    require(f.id(layer, "animationKeys") == 0, "a fresh layer has animations");
    static_cast<void>(f.id(layer, "removeAnimationForKey:", f.cf.string("none")));

    // CAGradientLayer.
    const auto gradient = f.make("CAGradientLayer");
    require(f.cf.utf8(f.id(gradient, "type")) == "axial", "default gradient type is not axial");
    const auto start = f.send(gradient, "startPoint"), end = f.send(gradient, "endPoint");
    require(start.d(0) == 0.5 && start.d(1) == 0 && end.d(0) == 0.5 && end.d(1) == 1, "default gradient points are wrong");
    alchemy::CallRegs point;
    point.set_d(0, 0); point.set_d(1, 0.5);
    static_cast<void>(f.send(gradient, "setStartPoint:", point));
    require(f.send(gradient, "startPoint").d(1) == 0.5, "setStartPoint: lost");
    const std::array<Address, 2> numbers{f.cf.real(0), f.cf.real(1)};
    static_cast<void>(f.id(gradient, "setLocations:", f.cf.array(numbers)));
    require(f.cf.length(f.id(gradient, "locations")) == 2, "setLocations: lost");
    static_cast<void>(f.id(gradient, "release"));
    static_cast<void>(f.id(layer, "release"));
    std::cout << "layer additions ok\n";
}

void metal_layers(Fixture& f) {
    const auto layer = f.make("CAMetalLayer");
    require(qc::is_metal_layer(f.objc, layer) && !qc::is_metal_layer(f.objc, f.make("CALayer")), "is_metal_layer is wrong");
    require(f.id(layer, "pixelFormat") == 80, "default pixelFormat is not BGRA8Unorm");
    require(f.id(layer, "framebufferOnly") == 1 && f.id(layer, "displaySyncEnabled") == 1 &&
            f.id(layer, "allowsNextDrawableTimeout") == 1 && f.id(layer, "presentsWithTransaction") == 0 &&
            f.id(layer, "wantsExtendedDynamicRangeContent") == 0 && f.id(layer, "maximumDrawableCount") == 3,
            "CAMetalLayer defaults are wrong");
    static_cast<void>(f.id(layer, "setPixelFormat:", 115));
    require(f.id(layer, "pixelFormat") == 115, "setPixelFormat: lost");
    require(f.fails([&] { static_cast<void>(f.id(layer, "setPixelFormat:", 70)); }, "not a valid"), "RGBA8Unorm accepted");
    require(f.fails([&] { static_cast<void>(f.id(layer, "setPixelFormat:", 552)); }, "unsupported"), "XR format accepted");
    require(f.fails([&] { static_cast<void>(f.id(layer, "setMaximumDrawableCount:", 4)); }), "4 drawables accepted");
    static_cast<void>(f.id(layer, "setMaximumDrawableCount:", 2));
    require(f.id(layer, "maximumDrawableCount") == 2, "setMaximumDrawableCount: lost");
    static_cast<void>(f.id(layer, "setPixelFormat:", 80));

    // drawableSize follows bounds * contentsScale until set explicitly.
    f.set_rect(layer, "setFrame:", 0, 0, 160, 120);
    f.set_real(layer, "setContentsScale:", 2.0);
    auto size = f.send(layer, "drawableSize");
    require(size.d(0) == 320 && size.d(1) == 240, "implicit drawableSize is not bounds * contentsScale");
    f.set_size(layer, "setDrawableSize:", 200, 100);
    f.set_rect(layer, "setFrame:", 0, 0, 10, 10);
    size = f.send(layer, "drawableSize");
    require(size.d(0) == 200 && size.d(1) == 100, "explicit drawableSize did not stick");

    // Colorspace (CF-retained CGColorSpace).
    const auto names = f.process.resolve("/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
                                         "_kCGColorSpaceExtendedLinearSRGB");
    require(names.has_value(), "kCGColorSpaceExtendedLinearSRGB missing");
    const auto create = f.process.resolve("/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
                                          "_CGColorSpaceCreateWithName");
    const std::array<Address, 1> name{f.engine.read64(names->address)};
    const auto space = f.engine.invoke_callback(create->address, name);
    require(space != 0, "CGColorSpaceCreateWithName failed");
    static_cast<void>(f.id(layer, "setColorspace:", space));
    require(f.id(layer, "colorspace") == space, "colorspace lost");
    f.process.objects().release(space);   // the layer keeps its own reference
    static_cast<void>(f.id(layer, "setColorspace:", 0));

    // nextDrawable: documented nil results and explicit failures.
    require(f.id(layer, "nextDrawable") == 0, "nextDrawable without a device is not nil");
    const auto fake_device = f.make("NSObject");
    static_cast<void>(f.id(layer, "setDevice:", fake_device));
    require(f.id(layer, "device") == fake_device && f.retain_count(fake_device) == 2, "device not retained");
    require(f.fails([&] { static_cast<void>(f.id(layer, "nextDrawable")); }, "unsupported"),
            "nextDrawable with a non-Vulkan device did not fail explicitly");
    static_cast<void>(f.id(layer, "setDevice:", 0));
    require(f.retain_count(fake_device) == 1, "replaced device not released");
    f.process.objects().release(fake_device);
    std::cout << "metal layers ok\n";
}

void transactions(Fixture& f) {
    const auto transaction = f.cls("CATransaction");
    require(f.process.resolve(quartz, "_OBJC_CLASS_$_CATransaction").has_value(), "CATransaction is not exported");
    static_cast<void>(f.id(transaction, "begin"));
    static_cast<void>(f.id(transaction, "setDisableActions:", 1));
    require(f.id(transaction, "disableActions") == 1, "setDisableActions: lost");
    static_cast<void>(f.id(transaction, "begin"));
    require(f.id(transaction, "disableActions") == 1, "nested transaction did not inherit disableActions");
    static_cast<void>(f.id(transaction, "setDisableActions:", 0));
    static_cast<void>(f.id(transaction, "commit"));
    require(f.id(transaction, "disableActions") == 1, "commit did not restore the outer transaction");
    static_cast<void>(f.id(transaction, "commit"));
    static_cast<void>(f.id(transaction, "flush"));
    const auto media = f.process.resolve(quartz, "_CACurrentMediaTime");
    require(media.has_value(), "CACurrentMediaTime missing");
    // Commit handlers run at the outermost commit in phase order; a completion
    // block runs at the next step of the run loop.
    static std::vector<int> ran;
    ran.clear();
    const auto global_block = [&](int tag) {
        const auto invoke = f.engine.register_guest_call([tag](auto& state, auto&) { ran.push_back(tag); state.x[0] = 0; });
        const auto descriptor = f.process.allocate_guest(16), block = f.process.allocate_guest(32);
        f.engine.write64(descriptor, 0); f.engine.write64(descriptor + 8, 32);
        f.engine.write64(block, f.process.resolve("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock")->address);
        f.engine.write32(block + 8, 1U << 28); f.engine.write32(block + 12, 0);
        f.engine.write64(block + 16, invoke); f.engine.write64(block + 24, descriptor);
        return block;
    };
    const auto add_handler = [&](int phase) {
        alchemy::CallRegs registers;
        registers.x[2] = global_block(phase); registers.x[3] = static_cast<Address>(phase);
        static_cast<void>(f.send(transaction, "addCommitHandler:forPhase:", registers));
    };
    static_cast<void>(f.id(transaction, "begin"));
    add_handler(2); add_handler(0); add_handler(1);
    static_cast<void>(f.id(transaction, "setCompletionBlock:", global_block(7)));
    static_cast<void>(f.id(transaction, "begin"));
    static_cast<void>(f.id(transaction, "commit"));
    require(ran.empty(), "commit handlers ran before the outermost commit");
    static_cast<void>(f.id(transaction, "commit"));
    require(ran == std::vector<int>{0, 1, 2}, "commit handlers run in phase order at the outermost commit");
    const auto core = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
    alchemy::CallRegs run;
    run.x[0] = f.engine.read64(f.process.resolve(core, "_kCFRunLoopDefaultMode")->address);
    run.set_d(0, 0.05);
    run.x[1] = 1;
    static_cast<void>(alchemy::engine_call_regs(f.engine, f.process.resolve(core, "_CFRunLoopRunInMode")->address, run));
    require(ran == std::vector<int>{0, 1, 2, 7}, "the completion block runs at the next run loop step");
    std::cout << "transactions ok\n";
}

float single(const alchemy::CallResult& result, unsigned index) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(result.v[index][0]));
}

// g3: -[CALayer contentsRect] defaults to the unit rectangle and keeps what
// is set (also outside the unit square); non-finite values are ignored.
void contents_rect(Fixture& f) {
    const auto layer = f.make("CALayer");
    const auto unit = f.rect(layer, "contentsRect");
    require(unit == std::array<double, 4>{0, 0, 1, 1}, "contentsRect defaults to {0,0,1,1}");
    f.set_rect(layer, "setContentsRect:", 0.25, 0.5, 0.5, 0.25);
    require(f.rect(layer, "contentsRect") == std::array<double, 4>{0.25, 0.5, 0.5, 0.25}, "setContentsRect:");
    f.set_rect(layer, "setContentsRect:", -0.5, 0, 2, 1);
    require(f.rect(layer, "contentsRect") == std::array<double, 4>{-0.5, 0, 2, 1}, "a rectangle outside the unit square is kept");
    f.set_rect(layer, "setContentsRect:", std::nan(""), 0, 1, 1);
    require(f.rect(layer, "contentsRect") == std::array<double, 4>{-0.5, 0, 2, 1}, "a NaN rectangle is ignored");
    static_cast<void>(f.id(layer, "release"));
    std::cout << "contents rect ok\n";
}

// g3: CAMetalDisplayLink's properties and the CADynamicRange keys (no device).
void metal_display_link_properties(Fixture& f) {
    const auto read_constant = [&](std::string_view name) {
        const auto symbol = f.process.resolve(quartz, name);
        require(symbol.has_value(), std::string("missing ") + std::string(name));
        return f.cf.utf8(f.engine.read64(symbol->address));
    };
    require(read_constant("_CADynamicRangeAutomatic") == "automatic" && read_constant("_CADynamicRangeStandard") == "standard" &&
            read_constant("_CADynamicRangeConstrainedHigh") == "constrainedHigh" && read_constant("_CADynamicRangeHigh") == "high",
            "CADynamicRange keys");
    const auto layer = f.make("CAMetalLayer");
    require(f.cf.utf8(f.id(layer, "preferredDynamicRange")) == "automatic", "preferredDynamicRange default");
    static_cast<void>(f.id(layer, "setPreferredDynamicRange:", f.cf.string("high")));
    require(f.cf.utf8(f.id(layer, "preferredDynamicRange")) == "high", "preferredDynamicRange set");
    const auto link = f.id(f.id(f.cls("CAMetalDisplayLink"), "alloc"), "initWithMetalLayer:", layer);
    require(link != 0 && f.id(link, "metalLayer") == layer, "initWithMetalLayer:");
    require(f.id(link, "delegate") == 0 && f.id(link, "isPaused") == 0, "delegate and paused defaults");
    require(single(f.send(link, "preferredFrameLatency"), 0) == 2.0F, "preferredFrameLatency default 2");
    alchemy::CallRegs range;
    range.set_s(0, 30); range.set_s(1, 120); range.set_s(2, 60);
    static_cast<void>(f.send(link, "setPreferredFrameRateRange:", range));
    const auto got = f.send(link, "preferredFrameRateRange");
    require(single(got, 0) == 30 && single(got, 1) == 120 && single(got, 2) == 60, "preferredFrameRateRange");
    static_cast<void>(f.id(link, "setPaused:", 1));
    require(f.id(link, "isPaused") == 1, "setPaused:");
    static_cast<void>(f.id(link, "invalidate"));
    static_cast<void>(f.id(link, "release"));
    static_cast<void>(f.id(layer, "release"));
    std::cout << "metal display link properties ok\n";
}

#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
// g3: a CAMetalDisplayLink added to the main run loop calls its delegate
// with drawables of its layer at the display rate, not while paused, and
// never after -invalidate.
void metal_display_link_frames(Fixture& f, Address layer) {
    struct Seen { int updates{}; Address drawable{}; double target{}, presentation{}; } seen;
    const auto delegate_class = f.objc.register_native_class("G3TestDisplayLinkDelegate", f.cls("NSObject"));
    static_cast<void>(f.objc.add_native_method(delegate_class, "metalDisplayLink:needsUpdate:", [&](transmute::CpuState& s, transmute::GuestMemory&) {
        const auto update = s.x[3];
        ++seen.updates;
        seen.drawable = f.id(update, "drawable");
        seen.target = f.send(update, "targetTimestamp").d(0);
        seen.presentation = f.send(update, "targetPresentationTimestamp").d(0);
        if (seen.drawable) static_cast<void>(f.id(seen.drawable, "present"));
    }, false, "v@:@@"));
    const auto delegate = f.make("G3TestDisplayLinkDelegate");
    const auto link = f.id(f.id(f.cls("CAMetalDisplayLink"), "alloc"), "initWithMetalLayer:", layer);
    static_cast<void>(f.id(link, "setDelegate:", delegate));
    const auto mode = f.engine.read64(f.process.resolve("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
                                                        "_kCFRunLoopDefaultMode")->address);
    alchemy::CallRegs add;
    add.x[2] = f.id(f.cls("NSRunLoop"), "currentRunLoop");
    add.x[3] = mode;
    static_cast<void>(f.send(link, "addToRunLoop:forMode:", add));
    const auto run = [&](int milliseconds) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < end)
            static_cast<void>(alchemy::gui::main_run_loop().run_once(alchemy::gui::default_mode, 0.01, false));
    };
    run(400);
    require(seen.updates >= 5, "CAMetalDisplayLink fired " + std::to_string(seen.updates) + " times in 400 ms");
    require(seen.drawable != 0 && f.id(seen.drawable, "layer") == layer, "the update's drawable is the layer's");
    require(seen.target > 0 && seen.presentation >= seen.target, "target timestamps");
    static_cast<void>(f.id(link, "setPaused:", 1));
    run(30);
    const auto paused = seen.updates;
    run(150);
    require(seen.updates == paused, "a paused link does not fire");
    static_cast<void>(f.id(link, "setPaused:", 0));
    run(150);
    require(seen.updates > paused, "an unpaused link fires again");
    static_cast<void>(f.id(link, "invalidate"));
    const auto stopped = seen.updates;
    run(150);
    require(seen.updates == stopped, "an invalidated link does not fire");
    static_cast<void>(f.id(link, "release"));
    static_cast<void>(f.id(delegate, "release"));
    f.objc.cleanup_thread(*f.process.thread(f.engine));
    std::cout << "metal display link frames ok (" << stopped << " updates)\n";
}

void drawables(Fixture& f) {
    std::string why;
    const auto device = qc::test::VulkanTestDevice::create(why);
    if (!device) { std::cout << "drawables skipped: " << why << '\n'; return; }
    const auto window = qc::test::TestWindow::create(256, 192, why);
    if (!window) { std::cout << "drawables skipped: " << why << '\n'; return; }
    std::vector<Address> textures;
    device->wrap = [&](const std::shared_ptr<qc::DrawableImage>&) {
        const auto texture = f.make("NSObject");
        textures.push_back(texture);
        return texture;
    };
    const auto mtl_device = f.make("NSObject");
    qc::register_metal_device(mtl_device, device);

    const auto layer = f.make("CAMetalLayer");
    static_cast<void>(f.id(layer, "setDevice:", mtl_device));
    require(f.id(layer, "nextDrawable") == 0, "nextDrawable with an empty drawableSize is not nil");
    f.set_size(layer, "setDrawableSize:", 256, 192);
    require(f.fails([&] { static_cast<void>(f.id(layer, "nextDrawable")); }, "not shown in a window"),
            "nextDrawable without a host did not fail explicitly");
    qc::LayerHost host;
    host.frame = {{0, 0}, {256, 192}};
    host.native_window = window->handle();
    qc::set_layer_host(f.objc, layer, host);
    require(qc::layer_host(f.objc, layer).has_value(), "layer host lost");

    for (int frame = 0; frame < 12; ++frame) {
        const auto drawable = f.id(layer, "nextDrawable");
        require(drawable != 0, "nextDrawable returned nil");
        static_cast<void>(f.id(drawable, "retain"));   // outlive the (absent) autorelease pool
        require(f.id(drawable, "layer") == layer, "drawable.layer is wrong");
        const auto texture = f.id(drawable, "texture");
        require(texture != 0 && std::find(textures.begin(), textures.end(), texture) != textures.end(),
                "drawable.texture is not the Metal-wrapped texture");
        require(f.id(drawable, "drawableID") == static_cast<Address>(frame), "drawableID is not sequential");
        static_cast<void>(f.id(drawable, "present"));
        static_cast<void>(f.id(drawable, "present"));   // a second present is ignored
        require(f.send(drawable, "presentedTime").d(0) > 0, "presentedTime not set");
        static_cast<void>(f.id(drawable, "release"));
        window->pump();
    }
    require(textures.size() <= 3, "more textures than maximumDrawableCount were wrapped");
    metal_display_link_frames(f, layer);
    // A drawable released unpresented returns its image.
    for (int index = 0; index < 5; ++index) {
        const auto drawable = f.id(layer, "nextDrawable");
        require(drawable != 0, "unpresented drawables were not recycled");
        static_cast<void>(f.id(drawable, "retain"));
        static_cast<void>(f.id(drawable, "release"));
        f.objc.cleanup_thread(*f.process.thread(f.engine));   // drains the autoreleased reference
    }
    qc::set_layer_host(f.objc, layer, std::nullopt);
    f.objc.cleanup_thread(*f.process.thread(f.engine));   // autoreleased drawables retain the layer
    static_cast<void>(f.id(layer, "release"));   // shuts the presenter down, releases the textures
    for (const auto texture : textures)
        require(!f.process.objects().find(texture), "a drawable texture outlived its layer");
    qc::unregister_metal_device(mtl_device);
    device->wait_idle();
    std::cout << "drawables ok (" << device->device_name << ")\n";
}
#endif
} // namespace

int main() {
    try {
        Fixture f;
        layers(f);
        layer_additions(f);
        metal_layers(f);
        transactions(f);
        metal_display_link_properties(f);
        contents_rect(f);
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
        drawables(f);
#endif
        std::cout << "Alchemy GUI QuartzCore Objective-C tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
