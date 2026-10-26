// QuartzCore layer kinds and layer geometry (the frameworks wave, U2): the
// CALayer appearance properties feeding the composition, the backing store
// of layers that draw (-drawInContext: / -drawLayer:inContext:),
// CAShapeLayer / CATextLayer / CAGradientLayer pixels, CAReplicatorLayer
// copies, hit testing and conversion between layers, CAScrollLayer and the
// constraint layout manager. Every provider installed, through the
// Objective-C runtime; pixels are read from composition snapshots.
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/quartzcore/layer_tree.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Address = std::uint64_t;
namespace qc = alchemy::gui::quartzcore;
namespace tree = alchemy::gui::quartzcore::tree;
constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) < tolerance; }

std::vector<std::string> journal;

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
        return objc.send(receiver, objc.selector(selector), std::move(registers));
    }
    Address id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        alchemy::CallRegs registers;
        unsigned index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return send(receiver, selector, registers).x0;
    }
    Address make(std::string_view name) { return id(id(cls(name), "alloc"), "init"); }
    Address array(const std::vector<Address>& values) { return cf.array(values); }
    void reals(Address receiver, std::string_view selector, std::initializer_list<double> values, std::initializer_list<Address> arguments = {}) {
        alchemy::CallRegs registers;
        unsigned index = 0;
        for (const auto value : values) registers.set_d(index++, value);
        unsigned x = 2;
        for (const auto value : arguments) registers.x[x++] = value;
        static_cast<void>(send(receiver, selector, registers));
    }
    Address call(std::string_view symbol, std::initializer_list<Address> arguments = {}) {
        const auto found = process.resolve(cg, symbol);
        require(found.has_value(), "missing " + std::string(symbol));
        return engine.invoke_callback(found->address, std::span(arguments.begin(), arguments.size()));
    }
    Address call_regs(std::string_view symbol, const alchemy::CallRegs& registers) {
        const auto found = process.resolve(cg, symbol);
        require(found.has_value(), "missing " + std::string(symbol));
        return alchemy::EngineAdapter(engine).call_regs(found->address, registers).x0;
    }
    Address color(double r, double g, double b, double a) {
        alchemy::CallRegs registers;
        registers.set_d(0, r); registers.set_d(1, g); registers.set_d(2, b); registers.set_d(3, a);
        return call_regs("_CGColorCreateSRGB", registers);
    }
    Address rect_path(double x, double y, double w, double h) {
        alchemy::CallRegs registers;
        registers.set_d(0, x); registers.set_d(1, y); registers.set_d(2, w); registers.set_d(3, h);
        registers.x[0] = 0;
        return call_regs("_CGPathCreateWithRect", registers);
    }
    Address subclass(std::string name, std::string_view super) { return objc.register_native_class(std::move(name), cls(super), 8); }
    void add(Address target, std::string name, transmute::GuestCallHandler handler, std::string types) {
        static_cast<void>(objc.add_native_method(target, std::move(name), std::move(handler), false, std::move(types)));
    }
};

// The straight colour of a contents pixel at layer point (x, y), first row at the visual top.
std::array<std::uint8_t, 4> pixel_at(const tree::Layer& layer, double x, double y) {
    require(layer.contents && layer.contents->bgra, "the layer has no contents");
    const auto frame = layer.contents_frame.value_or(layer.bounds);
    const auto& image = *layer.contents;
    const auto col = static_cast<std::size_t>((x - frame.origin.x) / frame.size.width * image.width);
    const auto row = static_cast<std::size_t>((frame.origin.y + frame.size.height - y) / frame.size.height * image.height);
    require(col < image.width && row < image.height, "pixel outside the contents");
    const auto* p = image.bgra + row * image.stride + col * 4;
    return {p[0], p[1], p[2], p[3]};
}

void appearance(Fixture& f) {
    const auto layer = f.make("CALayer");
    f.reals(layer, "setBounds:", {0, 0, 40, 30});
    f.reals(layer, "setCornerRadius:", {6});
    f.reals(layer, "setBorderWidth:", {2});
    static_cast<void>(f.id(layer, "setBorderColor:", {f.color(0, 0, 1, 1)}));
    alchemy::CallRegs opacity;
    opacity.v[0] = {std::bit_cast<std::uint32_t>(0.5F), 0};
    static_cast<void>(f.send(layer, "setShadowOpacity:", opacity));
    f.reals(layer, "setShadowOffset:", {3, -4});
    f.reals(layer, "setShadowRadius:", {5});
    static_cast<void>(f.id(layer, "setMaskedCorners:", {1 | 8}));
    const auto snapshot = qc::snapshot_tree(f.objc, layer);
    require(near(snapshot.corner_radius, 6) && near(snapshot.border_width, 2) && snapshot.masked_corners == 9, "appearance in the snapshot");
    require(snapshot.border && near((*snapshot.border)[2], 1), "border colour in the snapshot");
    require(snapshot.shadow && near(snapshot.shadow->opacity, 0.5) && near(snapshot.shadow->offset.width, 3) &&
                near(snapshot.shadow->offset.height, -4) && near(snapshot.shadow->radius, 5), "shadow in the snapshot");
    require(near(f.send(layer, "cornerRadius").d(0), 6) && f.id(layer, "maskedCorners") == 9, "appearance getters");
    // The shadow path's coverage.
    static_cast<void>(f.id(layer, "setShadowPath:", {f.rect_path(0, 0, 10, 10)}));
    const auto with_path = qc::snapshot_tree(f.objc, layer);
    require(with_path.shadow && with_path.shadow->path && with_path.shadow->path->width >= 10, "the shadow path mask");
}

void drawn_layers(Fixture& f) {
    // A delegate drawing with -drawLayer:inContext: fills the backing store.
    const auto delegate_class = f.subclass("ApiLayerPainter", "NSObject");
    f.add(delegate_class, "drawLayer:inContext:", [&f](transmute::CpuState& s, transmute::GuestMemory&) {
        const auto context = s.x[3];
        static_cast<void>(f.call("_CGContextSetFillColorWithColor", {context, f.color(1, 0, 0, 1)}));
        alchemy::CallRegs fill;
        fill.x[0] = context;
        fill.set_d(0, 0); fill.set_d(1, 0); fill.set_d(2, 10); fill.set_d(3, 5);   // the bottom half
        static_cast<void>(f.call_regs("_CGContextFillRect", fill));
        journal.push_back("drawLayer");
    }, "v@:@^{CGContext=}");
    const auto painter = f.make("ApiLayerPainter");
    const auto layer = f.make("CALayer");
    f.reals(layer, "setBounds:", {0, 0, 10, 10});
    static_cast<void>(f.id(layer, "setDelegate:", {painter}));
    static_cast<void>(f.id(layer, "display"));
    require(!journal.empty() && journal.back() == "drawLayer", "-display asks the delegate to draw");
    require(f.id(layer, "contents") != 0, "-display makes contents");
    // Marked layers that draw are displayed before they are shown.
    journal.clear();
    static_cast<void>(f.id(layer, "setNeedsDisplay"));
    static_cast<void>(qc::snapshot_tree(f.objc, layer));
    require(!journal.empty(), "a layer that needs display draws before it is shown");
}

void shapes_text_gradients(Fixture& f) {
    const auto shape = f.make("CAShapeLayer");
    f.reals(shape, "setBounds:", {0, 0, 20, 20});
    static_cast<void>(f.id(shape, "setPath:", {f.rect_path(5, 5, 10, 10)}));
    static_cast<void>(f.id(shape, "setFillColor:", {f.color(0, 1, 0, 1)}));
    auto snapshot = qc::snapshot_tree(f.objc, shape);
    auto inside = pixel_at(snapshot, 10, 10);
    require(inside[1] == 255 && inside[3] == 255, "the shape's fill");
    require(pixel_at(snapshot, 4.6, 10)[3] == 0, "outside the path is clear");
    // A stroke trimmed to its first half.
    static_cast<void>(f.id(shape, "setFillColor:", {0}));
    static_cast<void>(f.id(shape, "setStrokeColor:", {f.color(1, 0, 0, 1)}));
    f.reals(shape, "setLineWidth:", {2});
    f.reals(shape, "setStrokeEnd:", {0.5});
    snapshot = qc::snapshot_tree(f.objc, shape);
    require(pixel_at(snapshot, 10, 5)[2] == 255, "the stroke's first half (the bottom edge)");
    require(pixel_at(snapshot, 10, 15)[3] == 0, "the stroke's second half is trimmed");
    require(f.id(shape, "fillRule") && f.id(shape, "lineCap"), "shape string defaults");

    const auto text = f.make("CATextLayer");
    f.reals(text, "setBounds:", {0, 0, 120, 40});
    static_cast<void>(f.id(text, "setString:", {f.cf.string("Hello")}));
    f.reals(text, "setFontSize:", {24});
    snapshot = qc::snapshot_tree(f.objc, text);
    require(snapshot.contents && snapshot.owned_contents, "text layers have contents");
    std::size_t lit{};
    for (std::size_t i = 3; i < snapshot.owned_contents->size(); i += 4) lit += (*snapshot.owned_contents)[i] > 0;
    require(lit > 30, "the text is drawn: " + std::to_string(lit) + " pixels");
    require(near(f.send(text, "fontSize").d(0), 24), "-fontSize");

    const auto gradient = f.make("CAGradientLayer");
    f.reals(gradient, "setBounds:", {0, 0, 10, 20});
    static_cast<void>(f.id(gradient, "setColors:", {f.array({f.color(1, 0, 0, 1), f.color(0, 0, 1, 1)})}));
    snapshot = qc::snapshot_tree(f.objc, gradient);
    const auto top = pixel_at(snapshot, 5, 19), bottom = pixel_at(snapshot, 5, 1);
    require(top[0] > 200 && bottom[2] > 200, "the default axis runs from red at the bottom to blue at the top");
}

void replicas_geometry_scrolling(Fixture& f) {
    const auto replicator = f.make("CAReplicatorLayer");
    f.reals(replicator, "setBounds:", {0, 0, 100, 20});
    const auto dot = f.make("CALayer");
    f.reals(dot, "setBounds:", {0, 0, 10, 10});
    static_cast<void>(f.id(replicator, "addSublayer:", {dot}));
    static_cast<void>(f.id(replicator, "setInstanceCount:", {4}));
    auto snapshot = qc::snapshot_tree(f.objc, replicator);
    require(snapshot.sublayers.size() == 4, "three replicated instances beside the original");

    // Geometry between layers.
    const auto root = f.make("CALayer");
    f.reals(root, "setBounds:", {0, 0, 200, 200});
    const auto child = f.make("CALayer");
    f.reals(child, "setFrame:", {50, 60, 20, 20});
    static_cast<void>(f.id(root, "addSublayer:", {child}));
    alchemy::CallRegs point;
    point.set_d(0, 55); point.set_d(1, 65);
    point.x[2] = root;
    const auto converted = f.send(child, "convertPoint:fromLayer:", point);
    require(near(converted.d(0), 5) && near(converted.d(1), 5), "convertPoint:fromLayer:");
    alchemy::CallRegs hit;
    hit.set_d(0, 55); hit.set_d(1, 65);
    require(f.send(root, "hitTest:", hit).x0 == child, "hitTest: finds the sublayer");
    hit.set_d(0, 150);
    require(f.send(root, "hitTest:", hit).x0 == root, "hitTest: outside the sublayer is the layer");

    // CAScrollLayer.
    const auto scroller = f.make("CAScrollLayer");
    f.reals(scroller, "setBounds:", {0, 0, 50, 50});
    f.reals(scroller, "scrollToPoint:", {30, 40});
    auto bounds = f.send(scroller, "bounds");
    require(near(bounds.d(0), 30) && near(bounds.d(1), 40), "-scrollToPoint: moves the bounds origin");
    static_cast<void>(f.id(scroller, "setScrollMode:", {f.cf.string("horizontally")}));
    f.reals(scroller, "scrollToPoint:", {10, 10});
    bounds = f.send(scroller, "bounds");
    require(near(bounds.d(0), 10) && near(bounds.d(1), 40), "a horizontal scroll mode keeps y");

    // CAConstraintLayoutManager: a sublayer centred in its superlayer, half its width.
    const auto host = f.make("CALayer");
    f.reals(host, "setBounds:", {0, 0, 100, 60});
    static_cast<void>(f.id(host, "setLayoutManager:", {f.id(f.cls("CAConstraintLayoutManager"), "layoutManager")}));
    const auto inner = f.make("CALayer");
    f.reals(inner, "setBounds:", {0, 0, 10, 10});
    static_cast<void>(f.id(host, "addSublayer:", {inner}));
    const auto constraint_class = f.cls("CAConstraint");
    const auto superlayer = f.cf.string("superlayer");
    static_cast<void>(f.id(inner, "addConstraint:", {f.id(constraint_class, "constraintWithAttribute:relativeTo:attribute:", {1, superlayer, 1})}));
    static_cast<void>(f.id(inner, "addConstraint:", {f.id(constraint_class, "constraintWithAttribute:relativeTo:attribute:", {5, superlayer, 5})}));
    alchemy::CallRegs half;
    half.x[2] = 3; half.x[3] = superlayer; half.x[4] = 3;
    half.set_d(0, 0.5); half.set_d(1, 0);
    static_cast<void>(f.id(inner, "addConstraint:", {f.send(constraint_class, "constraintWithAttribute:relativeTo:attribute:scale:offset:", half).x0}));
    static_cast<void>(f.id(host, "layoutSublayers"));
    const auto frame = f.send(inner, "frame");
    require(near(frame.d(0), 25) && near(frame.d(2), 50) && near(frame.d(1), 25), "constraint layout centres and sizes the sublayer");
}

} // namespace

int main() {
    try {
        Fixture f;
        for (const auto& [name, test] : std::initializer_list<std::pair<const char*, void (*)(Fixture&)>>{
                 {"appearance", appearance}, {"drawn_layers", drawn_layers}, {"shapes_text_gradients", shapes_text_gradients},
                 {"replicas_geometry_scrolling", replicas_geometry_scrolling}}) {
            std::cout << "[quartzcore layers] " << name << std::endl;
            test(f);
        }
    } catch (const std::exception& error) {
        std::cerr << "QuartzCore layer test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Alchemy QuartzCore layer tests passed\n";
    return 0;
}
