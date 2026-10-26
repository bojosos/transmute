#include "alchemy/darwin/quartzcore/quartzcore.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/registration_audit.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/gui/window_server.hpp"
#include "alchemy/darwin/coregraphics/quartz2d.hpp"
#include "alchemy/darwin/quartzcore/layer_tree.hpp"
#include "alchemy/darwin/quartzcore/presenter.hpp"
#include "alchemy/darwin/quartzcore/quartzcore_vulkan.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// QuartzCore providers (owner: A7): CALayer geometry and hierarchy,
// CAMetalLayer with drawables presented to window-server HWNDs through
// Vulkan (presenter.hpp), CAMetalDrawable and CATransaction. They replace
// the headless CALayer/CAMetalLayer shells of darwin_frameworks.cpp by
// adding methods to the existing classes. Behaviour follows Apple's public
// QuartzCore documentation; there is no render tree: a layer has no backing
// store, animations are timed (delegate callbacks, removal) but never
// interpolated, and only a CAMetalLayer shows pixels.
namespace alchemy::gui {
namespace quartzcore {
namespace {
using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view quartz_core = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr auto rect_type = "{CGRect={CGPoint=dd}{CGSize=dd}}";
constexpr auto point_type = "{CGPoint=dd}";
constexpr auto size_type = "{CGSize=dd}";
constexpr double maximum_drawable_dimension = 16384;   // Metal's 2D texture limit
constexpr double drawable_timeout_seconds = 1.0;       // allowsNextDrawableTimeout

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Alchemy QuartzCore: " + what); }

double real(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }
float single(const CpuState& s, unsigned index) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[index][0])); }
void return_real(CpuState& s, unsigned index, double value) { s.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; }
void return_single(CpuState& s, float value) { s.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; }
Rect rect_argument(const CpuState& s) { return {{real(s, 0), real(s, 1)}, {real(s, 2), real(s, 3)}}; }
void return_rect(CpuState& s, const Rect& value) {
    return_real(s, 0, value.origin.x); return_real(s, 1, value.origin.y);
    return_real(s, 2, value.size.width); return_real(s, 3, value.size.height);
}
bool flag(const CpuState& s, unsigned index) { return (s.x[index] & 0xff) != 0; }
double finite_or_zero(double value) { return std::isfinite(value) ? value : 0.0; }

// ---- Metal device registry (quartzcore_vulkan.hpp) ------------------------------
std::mutex registry_mutex;
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
std::map<Address, std::shared_ptr<MetalVulkanDevice>>& registry() {
    static std::map<Address, std::shared_ptr<MetalVulkanDevice>> devices;
    return devices;
}
#endif

// ---- per-object state ---------------------------------------------------------------
struct MetalLayerState {
    Address device{};                  // retained
    Address colorspace{};              // retained CGColorSpaceRef
    Address edr_metadata{};            // retained
    std::uint64_t pixel_format{pixel_format::bgra8_unorm};
    std::uint32_t maximum_drawables{3};
    bool framebuffer_only{true}, display_sync{true}, presents_with_transaction{}, wants_edr{}, allows_timeout{true};
    std::optional<Size> drawable_size; // set explicitly; otherwise bounds * contentsScale
    bool linear_colorspace{};
    std::optional<LayerHost> host;
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
    std::shared_ptr<Presenter> presenter;
#endif
    std::uint64_t next_drawable_id{};
};

// CATransform3D: m11 ... m44, row-major (row vectors), as Core Animation lays it out.
using Transform3D = std::array<double, 16>;
constexpr Transform3D identity_transform() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

struct GradientLayerState {
    Address colors{}, locations{}, type{};   // retained copies (nil type: kCAGradientLayerAxial)
    Point start{0.5, 0}, end{0.5, 1};
};

// CAShapeLayer (CGPath / CGColor references CF-retained, strings retained copies).
struct ShapeLayerState {
    Address path{}, fill_color{}, stroke_color{};
    Address fill_rule{}, line_cap{}, line_join{}, dash_pattern{};
    double line_width{1}, miter_limit{10}, dash_phase{}, stroke_start{0}, stroke_end{1};
    bool fill_set{};                   // -setFillColor: was called (nil then means no fill)
};
// CATextLayer: the string (NSString or NSAttributedString, a retained copy),
// the font (CTFont, CGFont or a font name; CF-retained) and its size.
struct TextLayerState {
    Address string{}, truncation{}, alignment{};
    Address font{}, foreground{};
    double font_size{36};
    bool wrapped{}, foreground_set{}, subpixel_quantization{};
};
// CAReplicatorLayer.
struct ReplicatorLayerState {
    std::int64_t count{1};
    double delay{};
    Transform3D transform{identity_transform()};
    Address color{};                   // CF-retained CGColor (nil: opaque white)
    float red_offset{}, green_offset{}, blue_offset{}, alpha_offset{};
    bool preserves_depth{};
};
// CAScrollLayer / CATiledLayer.
struct ScrollLayerState {
    Address mode{};                    // retained copy (nil: kCAScrollBoth)
    std::uint64_t levels_of_detail{1}, levels_of_detail_bias{};
    Size tile_size{256, 256};
};

struct LayerState final : ObjectPayload {
    std::mutex mutex;
    Rect bounds;
    Point position;
    Point anchor{0.5, 0.5};
    double contents_scale{1}, z_position{};
    Rect contents_rect{{0, 0}, {1, 1}};   // unit coordinates of the contents (layer_tree.hpp)
    float opacity{1};
    bool opaque{}, hidden{}, masks_to_bounds{}, needs_display{}, needs_display_on_bounds_change{}, geometry_flipped{},
        needs_layout{};
    std::uint32_t edge_antialiasing_mask{0xf}, autoresizing_mask{};
    Address delegate{};                // not retained (CALayer delegates are weak)
    Address background_color{}, contents{}, name{};   // retained
    Address superlayer{};              // not retained
    std::vector<Address> sublayers;    // retained
    Transform3D transform{identity_transform()}, sublayer_transform{identity_transform()};
    Address corner_curve{};            // retained (nil: kCACornerCurveCircular)
    Address contents_gravity{};        // retained copy (nil: kCAGravityResize)
    Address preferred_dynamic_range{}; // retained copy (nil: CADynamicRangeAutomatic)
    // Animations added with -addAnimation:forKey: (copies, +1; keys copied,
    // +1 or nil), in the order they were added, with their timing timers.
    struct Animation {
        Address key{}, animation{};
        TimerId start_timer{}, stop_timer{};
        bool started{}, stopped{};
    };
    std::vector<Animation> animations;
    GradientLayerState gradient;       // CAGradientLayer
    MetalLayerState metal;
    // Appearance the composition draws (layer_tree.hpp).
    double corner_radius{}, border_width{}, shadow_radius{3};
    std::uint64_t masked_corners{tree::all_corners};
    Address border_color{}, shadow_color{}, shadow_path{};   // CF-retained
    float shadow_opacity{};
    Size shadow_offset{0, -3};
    Address style{};                   // retained copy
    // The layer's CAMediaTiming.
    double begin_time{}, time_offset{}, repeat_duration{}, duration{};
    float speed{1}, repeat_count{};
    bool autoreverses{};
    Address fill_mode{};               // retained copy (nil: kCAFillModeRemoved)
    // Constraint layout (CAConstraintLayoutManager).
    Address layout_manager{};          // retained
    std::vector<Address> constraints;  // retained CAConstraints
    ShapeLayerState shape;
    TextLayerState text;
    ReplicatorLayerState replicator;
    ScrollLayerState scroll;
};

struct DrawableState final : ObjectPayload {
    std::mutex mutex;
    Address layer{};                   // retained
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
    std::shared_ptr<Presenter> presenter;
    std::shared_ptr<DrawableImage> image;   // null once presented or returned
#endif
    Address texture{};
    std::uint64_t id{};
    double presented_time{};
    bool presented{};
    std::vector<Address> presented_handlers;   // _Block_copy'd
};

struct TransactionState {
    struct Level { bool disable_actions{}; double duration{0.25}; Address completion{}; };   // completion: _Block_copy'd
    std::vector<Level> levels;
    unsigned explicit_depth{};         // +begin minus +commit on this thread
    std::array<std::vector<Address>, 3> commit_handlers;   // by CATransactionPhase; _Block_copy'd
};
thread_local TransactionState transactions;

struct QuartzCore : std::enable_shared_from_this<QuartzCore> {
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;
    Address layer_class{}, metal_layer_class{}, drawable_class{}, transaction_class{};
    Address shape_layer_class{}, text_layer_class{}, replicator_layer_class{}, scroll_layer_class{}, gradient_layer_class{};
    std::mutex colors_mutex;
    Address black{}, white{};   // immortal opaque CGColors (the shape fill and text colour defaults)
    std::mutex completion_mutex;
    std::shared_ptr<CompletionThread> completion;
    // observe_layer_contents (quartzcore.hpp), told outside the lock.
    std::mutex contents_mutex;
    std::vector<std::pair<std::string, ContentsObserver>> contents_observers;
    void contents_changed(Address layer, Address contents) {
        std::vector<ContentsObserver> observers;
        {
            std::lock_guard lock(contents_mutex);
            for (const auto& entry : contents_observers) observers.push_back(entry.second);
        }
        for (const auto& observer : observers) observer(layer, contents);
    }
    // observe_layer_changes / observe_transaction_commits (quartzcore.hpp),
    // told outside every lock.
    std::vector<std::pair<std::string, ChangeObserver>> change_observers;
    std::vector<std::pair<std::string, CommitObserver>> commit_observers;
    std::atomic<bool> observed{false};
    void changed(Address layer) {
        if (!observed.load(std::memory_order_acquire) || !layer) return;
        std::vector<ChangeObserver> observers;
        {
            std::lock_guard lock(contents_mutex);
            for (const auto& entry : change_observers) observers.push_back(entry.second);
        }
        for (const auto& observer : observers) observer(layer);
    }
    void committed() {
        if (!observed.load(std::memory_order_acquire)) return;
        std::vector<CommitObserver> observers;
        {
            std::lock_guard lock(contents_mutex);
            for (const auto& entry : commit_observers) observers.push_back(entry.second);
        }
        for (const auto& observer : observers) observer();
    }

    QuartzCore(ProcessContext& context, ObjcRuntime& runtime, CoreFoundation& foundation)
        : process(context), objc(runtime), cf(foundation) {}

    GuestMemory& memory() { return process.calling_engine(); }
    Address call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments) {
        const auto found = process.resolve(library, symbol);
        if (!found) fail("missing runtime service " + std::string(symbol));
        return process.calling_engine().invoke_callback(found->address, std::span(arguments.begin(), arguments.size()));
    }
    CallResult call_regs(std::string_view library, std::string_view symbol, const CallRegs& registers) {
        const auto found = process.resolve(library, symbol);
        if (!found) fail("missing runtime service " + std::string(symbol));
        return EngineAdapter(process.calling_engine()).call_regs(found->address, registers);
    }
    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        unsigned index = 2;
        for (const auto value : arguments) registers.x.at(index++) = value;
        return objc.send(receiver, selector, std::move(registers)).x0;
    }
    Address retain(Address object) { return object ? send(object, "retain") : 0; }
    void release(Address object) { if (object) static_cast<void>(send(object, "release")); }
    Address cf_retain(Address object) {
        if (!object) return 0;
        const std::array<Address, 1> arguments{object};
        return cf.invoke("CFRetain", arguments);
    }
    void cf_release(Address object) {
        if (!object) return;
        const std::array<Address, 1> arguments{object};
        static_cast<void>(cf.invoke("CFRelease", arguments));
    }
    // object_setIvar-free class walk: true when `object`'s class is `cls` or inherits from it.
    bool is_kind(Address object, Address cls) {
        if (!object || !cls) return false;
        auto current = objc.object_class(object);
        for (int depth = 0; current && depth < 256; ++depth) {
            if (current == cls) return true;
            current = process.main_engine().read64(current + 8);
        }
        return false;
    }
    std::shared_ptr<LayerState> layer(Address object) { return objc.host_state<LayerState>(object); }
    std::shared_ptr<DrawableState> drawable(Address object) { return objc.host_state<DrawableState>(object); }
    std::shared_ptr<LayerState> metal_layer(Address object) {
        if (!is_kind(object, metal_layer_class)) fail("object is not a CAMetalLayer");
        return layer(object);
    }
    void method(Address cls, std::string name, transmute::GuestCallHandler handler, std::string types, bool class_method = false) {
        static_cast<void>(objc.add_native_method(cls, std::move(name),
            [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
            class_method, std::move(types)));
    }
    Address declare(std::string_view name, Address superclass, bool exported) {
        auto cls = objc.find_class(name);
        if (!cls) cls = objc.register_native_class(std::string(name), superclass, 8);
        if (exported) objc.publish_class(name, cls, quartz_core);
        return cls;
    }

    // ---- CALayer ------------------------------------------------------------------------
    static Rect frame_of(const LayerState& state) {
        const auto& size = state.bounds.size;
        const auto& t = state.transform;
        if (t == identity_transform())
            return {{state.position.x - state.anchor.x * size.width, state.position.y - state.anchor.y * size.height}, size};
        // The bounding box of the bounds transformed about the anchor point
        // (the 2D projection of the transform, as Core Animation reports).
        double min_x = std::numeric_limits<double>::infinity(), min_y = min_x, max_x = -min_x, max_y = -min_x;
        for (const auto& [u, v] : std::array<std::pair<double, double>, 4>{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}}) {
            const auto x = (u - state.anchor.x) * size.width, y = (v - state.anchor.y) * size.height;
            auto w = x * t[3] + y * t[7] + t[15];
            if (w == 0) w = 1;
            const auto px = (x * t[0] + y * t[4] + t[12]) / w, py = (x * t[1] + y * t[5] + t[13]) / w;
            min_x = std::min(min_x, px); max_x = std::max(max_x, px);
            min_y = std::min(min_y, py); max_y = std::max(max_y, py);
        }
        return {{state.position.x + min_x, state.position.y + min_y}, {max_x - min_x, max_y - min_y}};
    }
    void install_layer() {
        const auto L = layer_class;
        method(L, "layer", [this](CpuState& s, GuestMemory&) {
            const auto object = send(send(s.x[0], "alloc"), "init");
            s.x[0] = object ? send(object, "autorelease") : 0;
        }, "@@:", true);
        method(L, "init", [this](CpuState& s, GuestMemory&) { static_cast<void>(layer(s.x[0])); }, "@@:");
        method(L, "dealloc", [this, L](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            if (const auto state = objc.find_host_state<LayerState>(self)) {
                std::vector<Address> children, retained;
                bool had_contents{};
                {
                    std::lock_guard lock(state->mutex);
                    children = std::exchange(state->sublayers, {});
                    had_contents = state->contents != 0;
                    retained = {std::exchange(state->contents, 0), std::exchange(state->name, 0),
                                std::exchange(state->corner_curve, 0), std::exchange(state->contents_gravity, 0),
                                std::exchange(state->preferred_dynamic_range, 0),
                                std::exchange(state->gradient.colors, 0),
                                std::exchange(state->gradient.locations, 0), std::exchange(state->gradient.type, 0)};
                    // Timed animations keep their layer alive, so any left
                    // here have no timers.
                    for (const auto& a : std::exchange(state->animations, {})) { retained.push_back(a.key); retained.push_back(a.animation); }
                    state->delegate = 0;
                }
                for (const auto child : children) {
                    if (const auto child_state = objc.find_host_state<LayerState>(child)) {
                        std::lock_guard lock(child_state->mutex);
                        child_state->superlayer = 0;
                    }
                    release(child);
                }
                for (const auto object : retained) release(object);
                cf_release(std::exchange(state->background_color, 0));
                std::vector<Address> objects, cf_objects;
                {
                    std::lock_guard lock(state->mutex);
                    objects = {std::exchange(state->style, 0), std::exchange(state->fill_mode, 0), std::exchange(state->layout_manager, 0),
                               std::exchange(state->shape.fill_rule, 0), std::exchange(state->shape.line_cap, 0),
                               std::exchange(state->shape.line_join, 0), std::exchange(state->shape.dash_pattern, 0),
                               std::exchange(state->text.string, 0), std::exchange(state->text.truncation, 0),
                               std::exchange(state->text.alignment, 0), std::exchange(state->scroll.mode, 0)};
                    for (const auto constraint : std::exchange(state->constraints, {})) objects.push_back(constraint);
                    cf_objects = {std::exchange(state->border_color, 0), std::exchange(state->shadow_color, 0),
                                  std::exchange(state->shadow_path, 0), std::exchange(state->shape.path, 0),
                                  std::exchange(state->shape.fill_color, 0), std::exchange(state->shape.stroke_color, 0),
                                  std::exchange(state->text.font, 0), std::exchange(state->text.foreground, 0),
                                  std::exchange(state->replicator.color, 0)};
                }
                for (const auto object : objects) release(object);
                for (const auto object : cf_objects) cf_release(object);
                if (had_contents) contents_changed(self, 0);   // the contents observers drop what they hold for it
            }
            static_cast<void>(objc.send_super(self, L, objc.selector("dealloc")));
        }, "v@:");

        // Geometry. frame is derived from bounds, position, anchorPoint and
        // transform (the bounding box when the transform is not the identity).
        method(L, "bounds", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); return_rect(s, state->bounds);
        }, std::string(rect_type) + "@:");
        method(L, "setBounds:", [this](CpuState& s, GuestMemory&) {
            const auto value = rect_argument(s);
            const auto state = layer(s.x[0]);
            Size old;
            {
                std::lock_guard lock(state->mutex);
                old = state->bounds.size;
                if (old.width != value.size.width || old.height != value.size.height)
                    state->needs_display = state->needs_display || state->needs_display_on_bounds_change;
                state->bounds = value;
            }
            resized(s.x[0], old, value.size);
            changed(s.x[0]);
        }, std::string("v@:") + rect_type);
        method(L, "position", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex);
            return_real(s, 0, state->position.x); return_real(s, 1, state->position.y);
        }, std::string(point_type) + "@:");
        method(L, "setPosition:", [this](CpuState& s, GuestMemory&) {
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->position = {real(s, 0), real(s, 1)}; }
            changed(s.x[0]);
        }, std::string("v@:") + point_type);
        method(L, "anchorPoint", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex);
            return_real(s, 0, state->anchor.x); return_real(s, 1, state->anchor.y);
        }, std::string(point_type) + "@:");
        method(L, "setAnchorPoint:", [this](CpuState& s, GuestMemory&) {
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->anchor = {real(s, 0), real(s, 1)}; }
            changed(s.x[0]);
        }, std::string("v@:") + point_type);
        method(L, "frame", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); return_rect(s, frame_of(*state));
        }, std::string(rect_type) + "@:");
        method(L, "setFrame:", [this](CpuState& s, GuestMemory&) {
            const auto value = rect_argument(s);
            const auto state = layer(s.x[0]);
            Size old;
            {
                std::lock_guard lock(state->mutex);
                old = state->bounds.size;
                if (old.width != value.size.width || old.height != value.size.height)
                    state->needs_display = state->needs_display || state->needs_display_on_bounds_change;
                state->bounds.size = value.size;
                state->position = {value.origin.x + state->anchor.x * value.size.width,
                                   value.origin.y + state->anchor.y * value.size.height};
            }
            resized(s.x[0], old, value.size);
            changed(s.x[0]);
        }, std::string("v@:") + rect_type);
        method(L, "zPosition", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); return_real(s, 0, state->z_position);
        }, "d@:");
        method(L, "setZPosition:", [this](CpuState& s, GuestMemory&) {
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->z_position = real(s, 0); }
            changed(s.x[0]);
        }, "v@:d");
        method(L, "contentsScale", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); return_real(s, 0, state->contents_scale);
        }, "d@:");
        method(L, "setContentsScale:", [this](CpuState& s, GuestMemory&) {
            const auto value = real(s, 0);
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); if (std::isfinite(value) && value > 0) state->contents_scale = value; }
            changed(s.x[0]);
        }, "v@:d");
        // contentsRect: the part of the contents shown, in unit coordinates
        // ({0,0,1,1} by default); any finite rectangle is kept (outside the
        // unit square the edge pixels repeat, an empty one draws nothing).
        // Like every property here it applies at once (animations are timed,
        // not interpolated).
        method(L, "contentsRect", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); return_rect(s, state->contents_rect);
        }, std::string(rect_type) + "@:");
        method(L, "setContentsRect:", [this](CpuState& s, GuestMemory&) {
            const auto value = rect_argument(s);
            if (!std::isfinite(value.origin.x) || !std::isfinite(value.origin.y) || !std::isfinite(value.size.width) ||
                !std::isfinite(value.size.height))
                return;
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->contents_rect = value; }
            changed(s.x[0]);
        }, std::string("v@:") + rect_type);

        // Appearance flags and values.
        const auto boolean = [&](const char* getter, const char* setter, bool LayerState::*member, bool visual = true) {
            method(L, getter, [this, member](CpuState& s, GuestMemory&) {
                const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = (*state).*member ? 1 : 0;
            }, "B@:");
            method(L, setter, [this, member, visual](CpuState& s, GuestMemory&) {
                bool was{};
                { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); was = std::exchange((*state).*member, flag(s, 2)); }
                if (visual && was != flag(s, 2)) changed(s.x[0]);
            }, "v@:B");
        };
        boolean("isOpaque", "setOpaque:", &LayerState::opaque);
        boolean("isHidden", "setHidden:", &LayerState::hidden);
        boolean("masksToBounds", "setMasksToBounds:", &LayerState::masks_to_bounds);
        boolean("needsDisplayOnBoundsChange", "setNeedsDisplayOnBoundsChange:", &LayerState::needs_display_on_bounds_change, false);
        boolean("isGeometryFlipped", "setGeometryFlipped:", &LayerState::geometry_flipped);
        // -contentsAreFlipped: the layer's space is flipped relative to the
        // root of its tree (an odd number of geometry-flipped superlayers).
        method(L, "contentsAreFlipped", [this](CpuState& s, GuestMemory&) {
            bool flipped{};
            Address current{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); current = state->superlayer; }
            for (int depth = 0; current && depth < 4096; ++depth) {
                const auto state = layer(current);
                std::lock_guard lock(state->mutex);
                flipped ^= state->geometry_flipped;
                current = state->superlayer;
            }
            s.x[0] = flipped ? 1 : 0;
        }, "B@:");
        const auto mask = [&](const char* getter, const char* setter, std::uint32_t LayerState::*member) {
            method(L, getter, [this, member](CpuState& s, GuestMemory&) {
                const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = (*state).*member;
            }, "I@:");
            method(L, setter, [this, member](CpuState& s, GuestMemory&) {
                const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex);
                (*state).*member = static_cast<std::uint32_t>(s.x[2]);
            }, "v@:I");
        };
        mask("edgeAntialiasingMask", "setEdgeAntialiasingMask:", &LayerState::edge_antialiasing_mask);
        mask("autoresizingMask", "setAutoresizingMask:", &LayerState::autoresizing_mask);
        method(L, "opacity", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); return_single(s, state->opacity);
        }, "f@:");
        method(L, "setOpacity:", [this](CpuState& s, GuestMemory&) {
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->opacity = std::clamp(single(s, 0), 0.0F, 1.0F); }
            changed(s.x[0]);
        }, "v@:f");
        method(L, "delegate", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->delegate;
        }, "@@:");
        method(L, "setDelegate:", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->delegate = s.x[2];
        }, "v@:@");
        // CGColorRef (CF retain semantics).
        method(L, "backgroundColor", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->background_color;
        }, "^{CGColor=}@:");
        method(L, "setBackgroundColor:", [this](CpuState& s, GuestMemory&) {
            const auto value = cf_retain(s.x[2]);
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->background_color, value); }
            cf_release(old);
            changed(s.x[0]);
        }, "v@:^{CGColor=}");
        const auto object = [&](const char* getter, const char* setter, Address LayerState::*member, bool copy,
                                bool observed = false, bool visual = false, const char* fallback = nullptr) {
            method(L, getter, [this, member, fallback](CpuState& s, GuestMemory&) {
                Address value{};
                { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); value = (*state).*member; }
                s.x[0] = value || !fallback ? value : cf.constant_string(fallback);
            }, "@@:");
            method(L, setter, [this, member, copy, observed, visual](CpuState& s, GuestMemory&) {
                const auto value = s.x[2] ? (copy ? send(s.x[2], "copy") : retain(s.x[2])) : 0;
                const auto state = layer(s.x[0]);
                Address old{};
                { std::lock_guard lock(state->mutex); old = std::exchange((*state).*member, value); }
                release(old);
                if (observed) contents_changed(s.x[0], s.x[2]);
                if (visual) changed(s.x[0]);
            }, "v@:@");
        };
        object("contents", "setContents:", &LayerState::contents, false, true, true);
        object("name", "setName:", &LayerState::name, true);
        // contentsGravity: kCAGravityResize unless set.
        object("contentsGravity", "setContentsGravity:", &LayerState::contents_gravity, true, false, true, "resize");
        // preferredDynamicRange (CADynamicRange* strings, metal_display_link.cpp):
        // stored; the compositor shows standard dynamic range only. The
        // default CADynamicRangeAutomatic is unverified.
        object("preferredDynamicRange", "setPreferredDynamicRange:", &LayerState::preferred_dynamic_range, true, false, false,
               "automatic");
        // -setContentsChanged: the contents object (an IOSurface drawn into
        // again) changed in place; the next commit shows it again.
        method(L, "setContentsChanged", [this](CpuState& s, GuestMemory&) { changed(s.x[0]); }, "v@:");

        // Display and layout: needsDisplay marks the layer for -display (below).
        method(L, "setNeedsDisplay", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->needs_display = true;
        }, "v@:");
        method(L, "setNeedsDisplayInRect:", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->needs_display = true;
        }, std::string("v@:") + rect_type);
        method(L, "needsDisplay", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->needs_display ? 1 : 0;
        }, "B@:");
        // -display: the delegate's -displayLayer:, else -drawInContext: into a
        // new bitmap backing store of the bounds at contentsScale, which
        // becomes the contents.
        method(L, "display", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            Address delegate{};
            { const auto state = layer(self); std::lock_guard lock(state->mutex); state->needs_display = false; delegate = state->delegate; }
            if (delegate) {
                CallRegs registers;
                registers.x[2] = self;
                if (objc.send_if_responds(delegate, objc.selector("displayLayer:"), registers)) return;
            }
            draw_backing_store(self);
        }, "v@:");
        method(L, "drawInContext:", [this](CpuState& s, GuestMemory&) {
            Address delegate{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); delegate = state->delegate; }
            if (!delegate) return;
            CallRegs registers;
            registers.x[2] = s.x[0];
            registers.x[3] = s.x[2];
            static_cast<void>(objc.send_if_responds(delegate, objc.selector("drawLayer:inContext:"), registers));
        }, "v@:^{CGContext=}");
        method(L, "displayIfNeeded", [this](CpuState& s, GuestMemory&) {
            bool needed{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); needed = state->needs_display; }
            if (needed) static_cast<void>(send(s.x[0], "display"));
        }, "v@:");
        method(L, "setNeedsLayout", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->needs_layout = true;
        }, "v@:");
        method(L, "needsLayout", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->needs_layout ? 1 : 0;
        }, "B@:");
        method(L, "layoutIfNeeded", [this](CpuState& s, GuestMemory&) {
            bool needed{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); needed = std::exchange(state->needs_layout, false); }
            if (needed) static_cast<void>(send(s.x[0], "layoutSublayers"));
        }, "v@:");
        method(L, "layoutSublayers", [this](CpuState& s, GuestMemory&) {
            Address delegate{}, manager{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); delegate = state->delegate; manager = state->layout_manager; }
            if (manager) {
                CallRegs registers;
                registers.x[2] = s.x[0];
                static_cast<void>(objc.send_if_responds(manager, objc.selector("layoutSublayersOfLayer:"), registers));
            }
            CallRegs registers;
            registers.x[2] = s.x[0];
            if (delegate) static_cast<void>(objc.send_if_responds(delegate, objc.selector("layoutSublayersOfLayer:"), registers));
        }, "v@:");
        install_layer_animations();

        // Hierarchy.
        method(L, "superlayer", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->superlayer;
        }, "@@:");
        method(L, "sublayers", [this](CpuState& s, GuestMemory&) {
            std::vector<Address> values;
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); values = state->sublayers; }
            s.x[0] = values.empty() ? 0 : send(cf.array(values), "autorelease");
        }, "@@:");
        method(L, "addSublayer:", [this](CpuState& s, GuestMemory&) { insert(s.x[0], s.x[2], std::nullopt); }, "v@:@");
        method(L, "insertSublayer:atIndex:", [this](CpuState& s, GuestMemory&) {
            insert(s.x[0], s.x[2], static_cast<std::uint32_t>(s.x[3]));
        }, "v@:@I");
        method(L, "removeFromSuperlayer", [this](CpuState& s, GuestMemory&) { remove_from_parent(s.x[0]); }, "v@:");
        method(L, "resizeSublayersWithOldSize:", [this](CpuState& s, GuestMemory&) {
            const Size old{real(s, 0), real(s, 1)};
            std::vector<Address> children;
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); children = state->sublayers; }
            for (const auto child : children) {
                CallRegs registers;
                registers.set_d(0, old.width);
                registers.set_d(1, old.height);
                static_cast<void>(objc.send(child, objc.selector("resizeWithOldSuperlayerSize:"), registers));
            }
        }, std::string("v@:") + size_type);
        method(L, "resizeWithOldSuperlayerSize:", [this](CpuState& s, GuestMemory&) {
            autoresize(s.x[0], {real(s, 0), real(s, 1)});
        }, std::string("v@:") + size_type);
        // -insertSublayer:above: / below: (a nil sibling, or one that is not a
        // sublayer of the receiver: the top / the bottom).
        method(L, "insertSublayer:above:", [this](CpuState& s, GuestMemory&) {
            insert(s.x[0], s.x[2], sibling_index(s.x[0], s.x[2], s.x[3], true));
        }, "v@:@@");
        method(L, "insertSublayer:below:", [this](CpuState& s, GuestMemory&) {
            insert(s.x[0], s.x[2], sibling_index(s.x[0], s.x[2], s.x[3], false));
        }, "v@:@@");
        method(L, "replaceSublayer:with:", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0], old_layer = s.x[2], new_layer = s.x[3];
            std::optional<std::uint32_t> at;
            {
                const auto state = layer(self);
                std::lock_guard lock(state->mutex);
                const auto it = std::find(state->sublayers.begin(), state->sublayers.end(), old_layer);
                if (!old_layer || it == state->sublayers.end())
                    fail("-[CALayer replaceSublayer:with:] the old layer is not a sublayer of the receiver");
                at = static_cast<std::uint32_t>(it - state->sublayers.begin());
            }
            if (old_layer == new_layer) return;
            remove_from_parent(old_layer);
            if (new_layer) insert(self, new_layer, at);
        }, "v@:@@");

        // Transforms (CATransform3D: 128 bytes, passed and returned indirectly).
        const auto transform = [&](const char* getter, const char* setter, Transform3D LayerState::*member) {
            method(L, getter, [this, member](CpuState& s, GuestMemory& m) {
                Transform3D value;
                { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); value = (*state).*member; }
                write_transform(m, s.x[8], value);
            }, "{CATransform3D=dddddddddddddddd}@:");
            method(L, setter, [this, member](CpuState& s, GuestMemory& m) {
                const auto value = read_transform(m, s.x[2]);
                { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); (*state).*member = value; }
                changed(s.x[0]);
            }, "v@:{CATransform3D=dddddddddddddddd}");
        };
        transform("transform", "setTransform:", &LayerState::transform);
        transform("sublayerTransform", "setSublayerTransform:", &LayerState::sublayer_transform);
        // -affineTransform / -setAffineTransform: (CGAffineTransform: 48
        // bytes, indirect) over the transform's 2D part.
        method(L, "affineTransform", [this](CpuState& s, GuestMemory& m) {
            Transform3D t;
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); t = state->transform; }
            const std::array<double, 6> affine{t[0], t[1], t[4], t[5], t[12], t[13]};
            for (unsigned i = 0; i < 6; ++i) m.write64(s.x[8] + i * 8, std::bit_cast<std::uint64_t>(affine[i]));
        }, "{CGAffineTransform=dddddd}@:");
        method(L, "setAffineTransform:", [this](CpuState& s, GuestMemory& m) {
            std::array<double, 6> a{};
            for (unsigned i = 0; i < 6; ++i) a[i] = std::bit_cast<double>(m.read64(s.x[2] + i * 8));
            auto t = identity_transform();
            t[0] = a[0]; t[1] = a[1]; t[4] = a[2]; t[5] = a[3]; t[12] = a[4]; t[13] = a[5];
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->transform = t; }
            changed(s.x[0]);
        }, "v@:{CGAffineTransform=dddddd}");
        // cornerCurve: kCACornerCurveCircular unless set.
        method(L, "cornerCurve", [this](CpuState& s, GuestMemory&) {
            Address value{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); value = state->corner_curve; }
            s.x[0] = value ? value : cf.constant_string("circular");
        }, "@@:");
        method(L, "setCornerCurve:", [this](CpuState& s, GuestMemory&) {
            const auto value = s.x[2] ? send(s.x[2], "copy") : 0;
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->corner_curve, value); }
            release(old);
        }, "v@:@");
    }
    // The index for inserting `child` above / below `sibling` in `parent`.
    std::optional<std::uint32_t> sibling_index(Address parent, Address child, Address sibling, bool above) {
        const auto ends = above ? std::optional<std::uint32_t>() : std::optional<std::uint32_t>(0);
        if (!sibling || sibling == child) return ends;
        const auto state = layer(parent);
        std::lock_guard lock(state->mutex);
        auto list = state->sublayers;
        list.erase(std::remove(list.begin(), list.end(), child), list.end());   // the child moves
        const auto it = std::find(list.begin(), list.end(), sibling);
        if (it == list.end()) return ends;
        return static_cast<std::uint32_t>((it - list.begin()) + (above ? 1 : 0));
    }
    static Transform3D read_transform(GuestMemory& m, Address at) {
        if (!at) fail("CATransform3D argument without storage");
        Transform3D value{};
        for (unsigned i = 0; i < 16; ++i) value[i] = std::bit_cast<double>(m.read64(at + i * 8));
        return value;
    }
    static void write_transform(GuestMemory& m, Address at, const Transform3D& value) {
        if (!at) fail("CATransform3D result without storage (x8)");
        for (unsigned i = 0; i < 16; ++i) m.write64(at + i * 8, std::bit_cast<std::uint64_t>(value[i]));
    }

    // ---- CALayer animations -------------------------------------------------------------
    // -addAnimation:forKey: keeps a copy of the animation (a zero beginTime
    // becomes the current media time) and runs its timing on the main run
    // loop: animationDidStart: at its begin time, animationDidStop:finished:YES
    // when its active duration has elapsed (duration, 0.25 s when 0;
    // repeatCount or repeatDuration; autoreverses; speed; infinite repeats and
    // speed 0 never finish), then removal when removedOnCompletion. Removing
    // or replacing it earlier sends animationDidStop:finished:NO. Values are
    // never interpolated: there is no render tree, so a layer shows its model
    // values.
    // ---- geometry between layers (the composition's model, layer_tree.hpp) ----------
    static tree::Affine inverse(const tree::Affine& m) {
        const double det = m.a * m.d - m.b * m.c;
        if (std::abs(det) < 1e-12) return {};
        const double a = m.d / det, b = -m.b / det, c = -m.c / det, d = m.a / det;
        return {a, b, c, d, -(a * m.tx + c * m.ty), -(b * m.tx + d * m.ty)};
    }
    static tree::Affine about(const Rect& b, Point anchor_unit, const tree::Affine& m) {
        const Point anchor{b.origin.x + anchor_unit.x * b.size.width, b.origin.y + anchor_unit.y * b.size.height};
        return tree::Affine::translation(anchor.x, anchor.y).after(m).after(tree::Affine::translation(-anchor.x, -anchor.y));
    }
    // The layer's bounds space -> its superlayer's bounds space (nullopt parent: the root space).
    tree::Affine to_superlayer(Address object, Address* parent_out = nullptr) {
        tree::Affine local;
        Address parent{};
        {
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            const auto& b = state->bounds;
            const Point anchor{b.origin.x + state->anchor.x * b.size.width, b.origin.y + state->anchor.y * b.size.height};
            local = tree::Affine::translation(state->position.x, state->position.y)
                        .after(affine_of(state->transform))
                        .after(tree::Affine::translation(-anchor.x, -anchor.y));
            parent = state->superlayer;
        }
        if (parent_out) *parent_out = parent;
        if (!parent) return local;
        const auto state = layer(parent);
        std::lock_guard lock(state->mutex);
        const auto& b = state->bounds;
        tree::Affine inner;
        if (state->geometry_flipped) inner = inner.after({1, 0, 0, -1, 0, 2 * b.origin.y + b.size.height});
        if (state->sublayer_transform != identity_transform()) inner = inner.after(about(b, state->anchor, affine_of(state->sublayer_transform)));
        return inner.after(local);
    }
    tree::Affine to_root(Address object) {
        tree::Affine m;
        for (int depth = 0; object && depth < 4096; ++depth) {
            Address parent{};
            m = to_superlayer(object, &parent).after(m);
            object = parent;
        }
        return m;
    }
    // `from`'s space -> `to`'s space (nil: the root space).
    tree::Affine between(Address from, Address to) {
        return inverse(to ? to_root(to) : tree::Affine{}).after(from ? to_root(from) : tree::Affine{});
    }
    static Rect map_rect(const tree::Affine& m, const Rect& r) {
        double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
        for (const auto& [u, v] : std::array<std::pair<double, double>, 4>{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}}) {
            const auto p = m.apply({r.origin.x + u * r.size.width, r.origin.y + v * r.size.height});
            x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
        }
        return {{x0, y0}, {x1 - x0, y1 - y0}};
    }
    static bool contains(const Rect& r, Point p) {
        return p.x >= r.origin.x && p.y >= r.origin.y && p.x < r.origin.x + r.size.width && p.y < r.origin.y + r.size.height;
    }
    // A layer's time from its superlayer's, and back (CAMediaTiming).
    double local_time(Address object, double parent_time) {
        const auto state = layer(object);
        std::lock_guard lock(state->mutex);
        return (parent_time - state->begin_time) * state->speed + state->time_offset;
    }
    double parent_time(Address object, double local) {
        const auto state = layer(object);
        std::lock_guard lock(state->mutex);
        return state->speed != 0 ? (local - state->time_offset) / state->speed + state->begin_time : state->begin_time;
    }
    double to_root_time(Address object, double t) {
        for (int depth = 0; object && depth < 4096; ++depth) {
            t = parent_time(object, t);
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            object = state->superlayer;
        }
        return t;
    }
    double from_root_time(Address object, double t) {
        std::vector<Address> chain;
        for (int depth = 0; object && depth < 4096; ++depth) {
            chain.push_back(object);
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            object = state->superlayer;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) t = local_time(*it, t);
        return t;
    }
    Address hit_test(Address object, Point in_superlayer) {
        bool hidden{}, masks{};
        std::vector<Address> children;
        {
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            hidden = state->hidden;
            masks = state->masks_to_bounds;
            children = state->sublayers;
        }
        if (hidden) return 0;
        const auto point = inverse(to_superlayer(object)).apply(in_superlayer);
        CallRegs registers;
        registers.set_d(0, point.x);
        registers.set_d(1, point.y);
        const bool inside = objc.send(object, objc.selector("containsPoint:"), registers).x0 & 0xff;
        if (masks && !inside) return 0;
        // Front to back: the reverse of the drawing order.
        std::stable_sort(children.begin(), children.end(), [this](Address a, Address b) {
            const auto sa = layer(a), sb = layer(b);
            double za{}, zb{};
            { std::lock_guard lock(sa->mutex); za = sa->z_position; }
            { std::lock_guard lock(sb->mutex); zb = sb->z_position; }
            return za < zb;
        });
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            CallRegs child;
            child.set_d(0, point.x);
            child.set_d(1, point.y);
            if (const auto hit = objc.send(*it, objc.selector("hitTest:"), child).x0) return hit;
        }
        return inside ? object : 0;
    }
    Address closest_scroll_layer(Address object) {
        for (int depth = 0; object && depth < 4096; ++depth) {
            if (scroll_layer_class && is_kind(object, scroll_layer_class)) return object;
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            object = state->superlayer;
        }
        return 0;
    }
    // CF-retained property setters and getters (CGColor, CGPath, CTFont ...).
    void cf_property(Address cls, const char* getter, const char* setter, Address LayerState::*member, const char* type,
                     bool copy_path = false) {
        method(cls, getter, [this, member](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = (*state).*member;
        }, std::string(type) + "@:");
        method(cls, setter, [this, member, copy_path](CpuState& s, GuestMemory&) {
            const auto value = s.x[2] ? (copy_path ? call(core_graphics_library(), "_CGPathCreateCopy", {s.x[2]}) : cf_retain(s.x[2])) : 0;
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange((*state).*member, value); }
            cf_release(old);
            changed(s.x[0]);
        }, std::string("v@:") + type);
    }
    template <class T, class Member>
    void value_property(Address cls, const char* getter, const char* setter, Member member, const char* type, bool visual) {
        method(cls, getter, [this, member](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]);
            std::lock_guard lock(state->mutex);
            const T value = member(*state);
            if constexpr (std::is_same_v<T, double>) return_real(s, 0, value);
            else if constexpr (std::is_same_v<T, float>) return_single(s, value);
            else if constexpr (std::is_same_v<T, bool>) s.x[0] = value ? 1 : 0;
            else s.x[0] = static_cast<Address>(value);
        }, std::string(type) + "@:");
        method(cls, setter, [this, member, visual](CpuState& s, GuestMemory&) {
            {
                const auto state = layer(s.x[0]);
                std::lock_guard lock(state->mutex);
                T& slot = member(*state);
                if constexpr (std::is_same_v<T, double>) slot = real(s, 0);
                else if constexpr (std::is_same_v<T, float>) slot = single(s, 0);
                else if constexpr (std::is_same_v<T, bool>) slot = flag(s, 2);
                else slot = static_cast<T>(s.x[2]);
            }
            if (visual) changed(s.x[0]);
        }, std::string("v@:") + type);
    }
    void string_property(Address cls, const char* getter, const char* setter, std::function<Address&(LayerState&)> slot_of, const char* fallback,
                         bool visual) {
        method(cls, getter, [this, slot_of, fallback](CpuState& s, GuestMemory&) {
            Address value{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); value = slot_of(*state); }
            s.x[0] = value || !fallback ? value : cf.constant_string(fallback);
        }, "@@:");
        method(cls, setter, [this, slot_of, visual](CpuState& s, GuestMemory&) {
            const auto value = s.x[2] ? send(s.x[2], "copy") : 0;
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(slot_of(*state), value); }
            release(old);
            if (visual) changed(s.x[0]);
        }, "v@:@");
    }

    void install_layer_appearance() {
        const auto L = layer_class;
        value_property<double>(L, "cornerRadius", "setCornerRadius:", [](LayerState& v) -> double& { return v.corner_radius; }, "d", true);
        value_property<std::uint64_t>(L, "maskedCorners", "setMaskedCorners:", [](LayerState& v) -> std::uint64_t& { return v.masked_corners; }, "Q", true);
        value_property<double>(L, "borderWidth", "setBorderWidth:", [](LayerState& v) -> double& { return v.border_width; }, "d", true);
        cf_property(L, "borderColor", "setBorderColor:", &LayerState::border_color, "^{CGColor=}");
        cf_property(L, "shadowColor", "setShadowColor:", &LayerState::shadow_color, "^{CGColor=}");
        cf_property(L, "shadowPath", "setShadowPath:", &LayerState::shadow_path, "^{CGPath=}", true);
        value_property<float>(L, "shadowOpacity", "setShadowOpacity:", [](LayerState& v) -> float& { return v.shadow_opacity; }, "f", true);
        value_property<double>(L, "shadowRadius", "setShadowRadius:", [](LayerState& v) -> double& { return v.shadow_radius; }, "d", true);
        method(L, "shadowOffset", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex);
            return_real(s, 0, state->shadow_offset.width); return_real(s, 1, state->shadow_offset.height);
        }, std::string(size_type) + "@:");
        method(L, "setShadowOffset:", [this](CpuState& s, GuestMemory&) {
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->shadow_offset = {real(s, 0), real(s, 1)}; }
            changed(s.x[0]);
        }, std::string("v@:") + size_type);
        string_property(L, "style", "setStyle:", [](LayerState& v) -> Address& { return v.style; }, nullptr, false);
        // CAMediaTiming.
        value_property<double>(L, "beginTime", "setBeginTime:", [](LayerState& v) -> double& { return v.begin_time; }, "d", false);
        value_property<double>(L, "timeOffset", "setTimeOffset:", [](LayerState& v) -> double& { return v.time_offset; }, "d", false);
        value_property<double>(L, "duration", "setDuration:", [](LayerState& v) -> double& { return v.duration; }, "d", false);
        value_property<double>(L, "repeatDuration", "setRepeatDuration:", [](LayerState& v) -> double& { return v.repeat_duration; }, "d", false);
        value_property<float>(L, "speed", "setSpeed:", [](LayerState& v) -> float& { return v.speed; }, "f", false);
        value_property<float>(L, "repeatCount", "setRepeatCount:", [](LayerState& v) -> float& { return v.repeat_count; }, "f", false);
        value_property<bool>(L, "autoreverses", "setAutoreverses:", [](LayerState& v) -> bool& { return v.autoreverses; }, "B", false);
        string_property(L, "fillMode", "setFillMode:", [](LayerState& v) -> Address& { return v.fill_mode; }, "removed", false);
        // Hit testing and conversion between layers.
        method(L, "containsPoint:", [this](CpuState& s, GuestMemory&) {
            Rect bounds;
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); bounds = state->bounds; }
            s.x[0] = contains(bounds, {real(s, 0), real(s, 1)}) ? 1 : 0;
        }, std::string("B@:") + point_type);
        method(L, "hitTest:", [this](CpuState& s, GuestMemory&) { s.x[0] = hit_test(s.x[0], {real(s, 0), real(s, 1)}); }, std::string("@@:") + point_type);
        for (const bool from : {true, false}) {
            method(L, from ? "convertPoint:fromLayer:" : "convertPoint:toLayer:", [this, from](CpuState& s, GuestMemory&) {
                const auto m = from ? between(s.x[2], s.x[0]) : between(s.x[0], s.x[2]);
                const auto p = m.apply({real(s, 0), real(s, 1)});
                return_real(s, 0, p.x); return_real(s, 1, p.y);
            }, std::string(point_type) + "@:" + point_type + "@");
            method(L, from ? "convertRect:fromLayer:" : "convertRect:toLayer:", [this, from](CpuState& s, GuestMemory&) {
                const auto m = from ? between(s.x[2], s.x[0]) : between(s.x[0], s.x[2]);
                return_rect(s, map_rect(m, {{real(s, 0), real(s, 1)}, {real(s, 2), real(s, 3)}}));
            }, std::string(rect_type) + "@:" + rect_type + "@");
            method(L, from ? "convertTime:fromLayer:" : "convertTime:toLayer:", [this, from](CpuState& s, GuestMemory&) {
                const auto self = s.x[0], other = s.x[2];
                const double root = from ? (other ? to_root_time(other, real(s, 0)) : real(s, 0)) : to_root_time(self, real(s, 0));
                return_real(s, 0, from ? from_root_time(self, root) : (other ? from_root_time(other, root) : root));
            }, "d@:d@");
        }
        // The part of the layer its closest CAScrollLayer ancestor shows.
        method(L, "visibleRect", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            Rect bounds;
            Address parent{};
            { const auto state = layer(self); std::lock_guard lock(state->mutex); bounds = state->bounds; parent = state->superlayer; }
            if (const auto scroller = parent ? closest_scroll_layer(parent) : 0) {
                Rect shown;
                { const auto state = layer(scroller); std::lock_guard lock(state->mutex); shown = state->bounds; }
                const auto mapped = map_rect(between(scroller, self), shown);
                const double x0 = std::max(bounds.origin.x, mapped.origin.x), y0 = std::max(bounds.origin.y, mapped.origin.y);
                const double x1 = std::min(bounds.origin.x + bounds.size.width, mapped.origin.x + mapped.size.width);
                const double y1 = std::min(bounds.origin.y + bounds.size.height, mapped.origin.y + mapped.size.height);
                bounds = x1 > x0 && y1 > y0 ? Rect{{x0, y0}, {x1 - x0, y1 - y0}} : Rect{};
            }
            return_rect(s, bounds);
        }, std::string(rect_type) + "@:");
        // Scrolling asks the closest CAScrollLayer ancestor.
        method(L, "scrollPoint:", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto scroller = closest_scroll_layer(self);
            if (!scroller) return;
            const auto p = between(self, scroller).apply({real(s, 0), real(s, 1)});
            CallRegs registers;
            registers.set_d(0, p.x);
            registers.set_d(1, p.y);
            static_cast<void>(objc.send(scroller, objc.selector("scrollToPoint:"), registers));
        }, std::string("v@:") + point_type);
        method(L, "scrollRectToVisible:", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            const auto scroller = closest_scroll_layer(self);
            if (!scroller) return;
            const auto r = map_rect(between(self, scroller), {{real(s, 0), real(s, 1)}, {real(s, 2), real(s, 3)}});
            CallRegs registers;
            for (unsigned i = 0; i < 4; ++i) registers.set_d(i, i == 0 ? r.origin.x : i == 1 ? r.origin.y : i == 2 ? r.size.width : r.size.height);
            static_cast<void>(objc.send(scroller, objc.selector("scrollToRect:"), registers));
        }, std::string("v@:") + rect_type);
        // Replacing every sublayer.
        method(L, "setSublayers:", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            std::vector<Address> wanted;
            if (s.x[2]) for (std::uint64_t i = 0, n = cf.length(s.x[2]); i < n; ++i) wanted.push_back(retain(cf.element(s.x[2], i)));
            std::vector<Address> old;
            { const auto state = layer(self); std::lock_guard lock(state->mutex); old = state->sublayers; }
            for (const auto child : old) static_cast<void>(send(child, "removeFromSuperlayer"));
            for (const auto child : wanted) { insert(self, child, std::nullopt); release(child); }
        }, "v@:@");
        method(L, "preferredFrameSize", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            Address manager{};
            Size size;
            { const auto state = layer(self); std::lock_guard lock(state->mutex); manager = state->layout_manager; size = state->bounds.size; }
            if (manager) {
                CallRegs registers;
                registers.x[2] = self;
                if (const auto answer = objc.send_if_responds(manager, objc.selector("preferredSizeOfLayer:"), registers)) size = {answer->d(0), answer->d(1)};
            }
            return_real(s, 0, size.width); return_real(s, 1, size.height);
        }, std::string(size_type) + "@:");
        method(L, "layoutManager", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->layout_manager;
        }, "@@:");
        method(L, "setLayoutManager:", [this](CpuState& s, GuestMemory&) {
            const auto value = retain(s.x[2]);
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->layout_manager, value); state->needs_layout = true; }
            release(old);
        }, "v@:@");
        method(L, "constraints", [this](CpuState& s, GuestMemory&) {
            std::vector<Address> list;
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); list = state->constraints; }
            s.x[0] = list.empty() ? 0 : send(cf.array(list), "autorelease");
        }, "@@:");
        method(L, "setConstraints:", [this](CpuState& s, GuestMemory&) {
            std::vector<Address> list;
            if (s.x[2]) for (std::uint64_t i = 0, n = cf.length(s.x[2]); i < n; ++i) list.push_back(retain(cf.element(s.x[2], i)));
            const auto state = layer(s.x[0]);
            std::vector<Address> old;
            { std::lock_guard lock(state->mutex); old = std::exchange(state->constraints, list); }
            for (const auto value : old) release(value);
        }, "v@:@");
        method(L, "addConstraint:", [this](CpuState& s, GuestMemory&) {
            if (!s.x[2]) return;
            const auto value = retain(s.x[2]);
            const auto state = layer(s.x[0]);
            std::lock_guard lock(state->mutex);
            state->constraints.push_back(value);
        }, "v@:@");
        method(L, "defaultValueForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:@", true);
        method(L, "needsDisplayForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@", true);
        method(L, "shouldArchiveValueForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:@");
        // -renderInContext:: the layer tree composited at the context's scale and drawn over the bounds.
        method(L, "renderInContext:", [this](CpuState& s, GuestMemory&) { render_in_context(s.x[0], s.x[2]); }, "v@:^{CGContext=}");
    }

    // ---- CAShapeLayer, CATextLayer, CAReplicatorLayer, CAScrollLayer, CATiledLayer, CAConstraint ----
    void install_shape_and_text_layers() {
        shape_layer_class = declare("CAShapeLayer", layer_class, true);
        const auto S = shape_layer_class;
        method(S, "path", [this](CpuState& s, GuestMemory&) { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->shape.path; },
               "^{CGPath=}@:");
        method(S, "setPath:", [this](CpuState& s, GuestMemory&) {
            const auto value = s.x[2] ? call(core_graphics_library(), "_CGPathCreateCopy", {s.x[2]}) : 0;
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->shape.path, value); }
            cf_release(old);
            changed(s.x[0]);
        }, "v@:^{CGPath=}");
        // The fill is opaque black until set (nil then fills nothing).
        method(S, "fillColor", [this](CpuState& s, GuestMemory&) {
            bool set{};
            Address value{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); set = state->shape.fill_set; value = state->shape.fill_color; }
            s.x[0] = set ? value : opaque_black();
        }, "^{CGColor=}@:");
        method(S, "setFillColor:", [this](CpuState& s, GuestMemory&) {
            const auto value = cf_retain(s.x[2]);
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->shape.fill_color, value); state->shape.fill_set = true; }
            cf_release(old);
            changed(s.x[0]);
        }, "v@:^{CGColor=}");
        method(S, "strokeColor", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->shape.stroke_color;
        }, "^{CGColor=}@:");
        method(S, "setStrokeColor:", [this](CpuState& s, GuestMemory&) {
            const auto value = cf_retain(s.x[2]);
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->shape.stroke_color, value); }
            cf_release(old);
            changed(s.x[0]);
        }, "v@:^{CGColor=}");
        string_property(S, "fillRule", "setFillRule:", [](LayerState& v) -> Address& { return v.shape.fill_rule; }, "non-zero", true);
        string_property(S, "lineCap", "setLineCap:", [](LayerState& v) -> Address& { return v.shape.line_cap; }, "butt", true);
        string_property(S, "lineJoin", "setLineJoin:", [](LayerState& v) -> Address& { return v.shape.line_join; }, "miter", true);
        string_property(S, "lineDashPattern", "setLineDashPattern:", [](LayerState& v) -> Address& { return v.shape.dash_pattern; }, nullptr, true);
        value_property<double>(S, "lineWidth", "setLineWidth:", [](LayerState& v) -> double& { return v.shape.line_width; }, "d", true);
        value_property<double>(S, "miterLimit", "setMiterLimit:", [](LayerState& v) -> double& { return v.shape.miter_limit; }, "d", true);
        value_property<double>(S, "lineDashPhase", "setLineDashPhase:", [](LayerState& v) -> double& { return v.shape.dash_phase; }, "d", true);
        value_property<double>(S, "strokeStart", "setStrokeStart:", [](LayerState& v) -> double& { return v.shape.stroke_start; }, "d", true);
        value_property<double>(S, "strokeEnd", "setStrokeEnd:", [](LayerState& v) -> double& { return v.shape.stroke_end; }, "d", true);

        text_layer_class = declare("CATextLayer", layer_class, true);
        const auto T = text_layer_class;
        string_property(T, "string", "setString:", [](LayerState& v) -> Address& { return v.text.string; }, nullptr, true);
        string_property(T, "truncationMode", "setTruncationMode:", [](LayerState& v) -> Address& { return v.text.truncation; }, "none", true);
        string_property(T, "alignmentMode", "setAlignmentMode:", [](LayerState& v) -> Address& { return v.text.alignment; }, "natural", true);
        method(T, "font", [this](CpuState& s, GuestMemory&) { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->text.font; }, "@@:");
        method(T, "setFont:", [this](CpuState& s, GuestMemory&) {
            const auto value = cf_retain(s.x[2]);
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->text.font, value); }
            cf_release(old);
            changed(s.x[0]);
        }, "v@:@");
        value_property<double>(T, "fontSize", "setFontSize:", [](LayerState& v) -> double& { return v.text.font_size; }, "d", true);
        value_property<bool>(T, "isWrapped", "setWrapped:", [](LayerState& v) -> bool& { return v.text.wrapped; }, "B", true);
        value_property<bool>(T, "allowsFontSubpixelQuantization", "setAllowsFontSubpixelQuantization:",
                             [](LayerState& v) -> bool& { return v.text.subpixel_quantization; }, "B", false);
        // The text colour is opaque white until set.
        method(T, "foregroundColor", [this](CpuState& s, GuestMemory&) {
            bool set{};
            Address value{};
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); set = state->text.foreground_set; value = state->text.foreground; }
            s.x[0] = set ? value : opaque_white();
        }, "^{CGColor=}@:");
        method(T, "setForegroundColor:", [this](CpuState& s, GuestMemory&) {
            const auto value = cf_retain(s.x[2]);
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->text.foreground, value); state->text.foreground_set = true; }
            cf_release(old);
            changed(s.x[0]);
        }, "v@:^{CGColor=}");

        replicator_layer_class = declare("CAReplicatorLayer", layer_class, true);
        const auto R = replicator_layer_class;
        value_property<std::int64_t>(R, "instanceCount", "setInstanceCount:", [](LayerState& v) -> std::int64_t& { return v.replicator.count; }, "q", true);
        value_property<double>(R, "instanceDelay", "setInstanceDelay:", [](LayerState& v) -> double& { return v.replicator.delay; }, "d", false);
        value_property<bool>(R, "preservesDepth", "setPreservesDepth:", [](LayerState& v) -> bool& { return v.replicator.preserves_depth; }, "B", false);
        value_property<float>(R, "instanceRedOffset", "setInstanceRedOffset:", [](LayerState& v) -> float& { return v.replicator.red_offset; }, "f", false);
        value_property<float>(R, "instanceGreenOffset", "setInstanceGreenOffset:", [](LayerState& v) -> float& { return v.replicator.green_offset; }, "f", false);
        value_property<float>(R, "instanceBlueOffset", "setInstanceBlueOffset:", [](LayerState& v) -> float& { return v.replicator.blue_offset; }, "f", false);
        value_property<float>(R, "instanceAlphaOffset", "setInstanceAlphaOffset:", [](LayerState& v) -> float& { return v.replicator.alpha_offset; }, "f", true);
        method(R, "instanceColor", [this](CpuState& s, GuestMemory&) { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->replicator.color; },
               "^{CGColor=}@:");
        method(R, "setInstanceColor:", [this](CpuState& s, GuestMemory&) {
            const auto value = cf_retain(s.x[2]);
            const auto state = layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->replicator.color, value); }
            cf_release(old);
            changed(s.x[0]);
        }, "v@:^{CGColor=}");
        // CATransform3D is 128 bytes: returned through x8, passed by reference.
        method(R, "instanceTransform", [this](CpuState& s, GuestMemory& m) {
            Transform3D value;
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); value = state->replicator.transform; }
            for (unsigned i = 0; i < 16; ++i) m.write64(s.x[8] + i * 8, std::bit_cast<std::uint64_t>(value[i]));
        }, "{CATransform3D=dddddddddddddddd}@:");
        method(R, "setInstanceTransform:", [this](CpuState& s, GuestMemory& m) {
            Transform3D value;
            for (unsigned i = 0; i < 16; ++i) value[i] = std::bit_cast<double>(m.read64(s.x[2] + i * 8));
            { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->replicator.transform = value; }
            changed(s.x[0]);
        }, "v@:{CATransform3D=dddddddddddddddd}");

        scroll_layer_class = declare("CAScrollLayer", layer_class, true);
        const auto C = scroll_layer_class;
        string_property(C, "scrollMode", "setScrollMode:", [](LayerState& v) -> Address& { return v.scroll.mode; }, "both", false);
        // The bounds origin moves to the point, on the axes the scroll mode allows.
        method(C, "scrollToPoint:", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            Address mode{};
            { const auto state = layer(self); std::lock_guard lock(state->mutex); mode = state->scroll.mode; }
            const auto name = mode ? cf.utf8(mode) : std::string("both");
            if (name == "none") return;
            {
                const auto state = layer(self);
                std::lock_guard lock(state->mutex);
                if (name != "vertically") state->bounds.origin.x = real(s, 0);
                if (name != "horizontally") state->bounds.origin.y = real(s, 1);
            }
            changed(self);
        }, std::string("v@:") + point_type);
        method(C, "scrollToRect:", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            Rect bounds;
            { const auto state = layer(self); std::lock_guard lock(state->mutex); bounds = state->bounds; }
            const Rect wanted{{real(s, 0), real(s, 1)}, {real(s, 2), real(s, 3)}};
            auto origin = bounds.origin;
            const auto fit = [](double start, double length, double view_start, double view_length) {
                if (start < view_start) return start;
                if (start + length > view_start + view_length) return std::min(start, start + length - view_length);
                return view_start;
            };
            origin.x = fit(wanted.origin.x, wanted.size.width, bounds.origin.x, bounds.size.width);
            origin.y = fit(wanted.origin.y, wanted.size.height, bounds.origin.y, bounds.size.height);
            CallRegs registers;
            registers.set_d(0, origin.x);
            registers.set_d(1, origin.y);
            static_cast<void>(objc.send(self, objc.selector("scrollToPoint:"), registers));
        }, std::string("v@:") + rect_type);

        // CATiledLayer: drawn as a whole (tiles are not scheduled separately).
        const auto D = declare("CATiledLayer", layer_class, true);
        method(D, "fadeDuration", [](CpuState& s, GuestMemory&) { return_real(s, 0, 0.25); }, "d@:", true);
        value_property<std::uint64_t>(D, "levelsOfDetail", "setLevelsOfDetail:", [](LayerState& v) -> std::uint64_t& { return v.scroll.levels_of_detail; }, "Q", false);
        value_property<std::uint64_t>(D, "levelsOfDetailBias", "setLevelsOfDetailBias:",
                                      [](LayerState& v) -> std::uint64_t& { return v.scroll.levels_of_detail_bias; }, "Q", false);
        method(D, "tileSize", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex);
            return_real(s, 0, state->scroll.tile_size.width); return_real(s, 1, state->scroll.tile_size.height);
        }, std::string(size_type) + "@:");
        method(D, "setTileSize:", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); state->scroll.tile_size = {real(s, 0), real(s, 1)};
        }, std::string("v@:") + size_type);
        install_constraints();
    }

    // CAConstraint (a value object) and CAConstraintLayoutManager: kCAConstraintMinX 0,
    // MidX 1, MaxX 2, Width 3, MinY 4, MidY 5, MaxY 6, Height 7; the source is a
    // sibling by name, or "superlayer".
    struct ConstraintState final : ObjectPayload {
        std::int32_t attribute{}, source_attribute{};
        Address source_name{};   // retained copy
        double scale{1}, offset{};
    };
    void install_constraints() {
        const auto K = declare("CAConstraint", objc.find_class("NSObject"), true);
        const auto init = [this](Address self, std::int32_t attribute, Address name, std::int32_t source, double scale, double offset) {
            const auto state = objc.host_state<ConstraintState>(self);
            state->attribute = attribute;
            state->source_attribute = source;
            state->source_name = name ? send(name, "copy") : 0;
            state->scale = scale;
            state->offset = offset;
            return self;
        };
        method(K, "initWithAttribute:relativeTo:attribute:scale:offset:", [init](CpuState& s, GuestMemory&) {
            s.x[0] = init(s.x[0], static_cast<std::int32_t>(s.x[2]), s.x[3], static_cast<std::int32_t>(s.x[4]), real(s, 0), real(s, 1));
        }, "@@:i@idd");
        const auto make = [this, init](Address cls, std::int32_t attribute, Address name, std::int32_t source, double scale, double offset) {
            return send(init(send(cls, "alloc"), attribute, name, source, scale, offset), "autorelease");
        };
        method(K, "constraintWithAttribute:relativeTo:attribute:scale:offset:", [make](CpuState& s, GuestMemory&) {
            s.x[0] = make(s.x[0], static_cast<std::int32_t>(s.x[2]), s.x[3], static_cast<std::int32_t>(s.x[4]), real(s, 0), real(s, 1));
        }, "@@:i@idd", true);
        method(K, "constraintWithAttribute:relativeTo:attribute:offset:", [make](CpuState& s, GuestMemory&) {
            s.x[0] = make(s.x[0], static_cast<std::int32_t>(s.x[2]), s.x[3], static_cast<std::int32_t>(s.x[4]), 1, real(s, 0));
        }, "@@:i@id", true);
        method(K, "constraintWithAttribute:relativeTo:attribute:", [make](CpuState& s, GuestMemory&) {
            s.x[0] = make(s.x[0], static_cast<std::int32_t>(s.x[2]), s.x[3], static_cast<std::int32_t>(s.x[4]), 1, 0);
        }, "@@:i@i", true);
        method(K, "attribute", [this](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint32_t>(objc.host_state<ConstraintState>(s.x[0])->attribute); }, "i@:");
        method(K, "sourceAttribute", [this](CpuState& s, GuestMemory&) {
            s.x[0] = static_cast<std::uint32_t>(objc.host_state<ConstraintState>(s.x[0])->source_attribute);
        }, "i@:");
        method(K, "sourceName", [this](CpuState& s, GuestMemory&) { s.x[0] = objc.host_state<ConstraintState>(s.x[0])->source_name; }, "@@:");
        method(K, "scale", [this](CpuState& s, GuestMemory&) { return_real(s, 0, objc.host_state<ConstraintState>(s.x[0])->scale); }, "d@:");
        method(K, "offset", [this](CpuState& s, GuestMemory&) { return_real(s, 0, objc.host_state<ConstraintState>(s.x[0])->offset); }, "d@:");
        method(K, "dealloc", [this, K](CpuState& s, GuestMemory&) {
            if (const auto state = objc.find_host_state<ConstraintState>(s.x[0])) release(std::exchange(state->source_name, 0));
            static_cast<void>(objc.send_super(s.x[0], K, objc.selector("dealloc")));
        }, "v@:");

        const auto M = declare("CAConstraintLayoutManager", objc.find_class("NSObject"), true);
        method(M, "layoutManager", [this](CpuState& s, GuestMemory&) { s.x[0] = send(send(send(s.x[0], "alloc"), "init"), "autorelease"); }, "@@:", true);
        method(M, "layoutSublayersOfLayer:", [this](CpuState& s, GuestMemory&) { layout_constraints(s.x[2]); }, "v@:@");
    }
    // Each sublayer's frame from its constraints, sources laid out first
    // (two passes resolve one level of dependence on siblings).
    void layout_constraints(Address parent) {
        std::vector<Address> children;
        Rect parent_bounds;
        { const auto state = layer(parent); std::lock_guard lock(state->mutex); children = state->sublayers; parent_bounds = state->bounds; }
        const auto frame_of_layer = [this](Address object) {
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            return frame_of(*state);
        };
        const auto value_of = [](const Rect& r, std::int32_t attribute) {
            switch (attribute) {
            case 0: return r.origin.x;
            case 1: return r.origin.x + r.size.width / 2;
            case 2: return r.origin.x + r.size.width;
            case 3: return r.size.width;
            case 4: return r.origin.y;
            case 5: return r.origin.y + r.size.height / 2;
            case 6: return r.origin.y + r.size.height;
            case 7: return r.size.height;
            default: return 0.0;
            }
        };
        for (int pass = 0; pass < 2; ++pass)
            for (const auto child : children) {
                std::vector<Address> constraints;
                { const auto state = layer(child); std::lock_guard lock(state->mutex); constraints = state->constraints; }
                if (constraints.empty()) continue;
                auto frame = frame_of_layer(child);
                std::array<std::optional<double>, 8> wanted;
                for (const auto constraint : constraints) {
                    const auto info = objc.find_host_state<ConstraintState>(constraint);
                    if (!info || info->attribute < 0 || info->attribute > 7) continue;
                    const auto name = info->source_name ? cf.utf8(info->source_name) : std::string("superlayer");
                    Rect source;
                    if (name == "superlayer") {
                        source = {{0, 0}, parent_bounds.size};
                        source.origin = parent_bounds.origin;
                    } else {
                        Address sibling{};
                        for (const auto other : children) {
                            Address other_name{};
                            { const auto state = layer(other); std::lock_guard lock(state->mutex); other_name = state->name; }
                            if (other_name && cf.utf8(other_name) == name) { sibling = other; break; }
                        }
                        if (!sibling) continue;
                        source = frame_of_layer(sibling);
                    }
                    wanted[static_cast<std::size_t>(info->attribute)] = value_of(source, info->source_attribute) * info->scale + info->offset;
                }
                // Sizes first, then the edges and middles.
                for (const int axis : {0, 1}) {
                    const auto base = static_cast<std::size_t>(axis * 4);
                    auto origin = axis ? frame.origin.y : frame.origin.x;
                    auto length = axis ? frame.size.height : frame.size.width;
                    const auto& min_v = wanted[base], &mid_v = wanted[base + 1], &max_v = wanted[base + 2], &size_v = wanted[base + 3];
                    if (size_v) length = std::max(0.0, *size_v);
                    else if (min_v && max_v) length = std::max(0.0, *max_v - *min_v);
                    else if (min_v && mid_v) length = std::max(0.0, 2 * (*mid_v - *min_v));
                    else if (mid_v && max_v) length = std::max(0.0, 2 * (*max_v - *mid_v));
                    if (min_v) origin = *min_v;
                    else if (mid_v) origin = *mid_v - length / 2;
                    else if (max_v) origin = *max_v - length;
                    (axis ? frame.origin.y : frame.origin.x) = origin;
                    (axis ? frame.size.height : frame.size.width) = length;
                }
                CallRegs registers;
                registers.set_d(0, frame.origin.x);
                registers.set_d(1, frame.origin.y);
                registers.set_d(2, frame.size.width);
                registers.set_d(3, frame.size.height);
                static_cast<void>(objc.send(child, objc.selector("setFrame:"), registers));
            }
    }

    // ---- drawing into bitmaps (Quartz 2D) ------------------------------------------------
    Address opaque_color(double grey) {
        CallRegs registers;
        for (unsigned i = 0; i < 3; ++i) registers.set_d(i, grey);
        registers.set_d(3, 1);
        return call_regs(core_graphics_library(), "_CGColorCreateSRGB", registers).x0;
    }
    Address opaque_black() { std::lock_guard lock(colors_mutex); if (!black) black = opaque_color(0); return black; }
    Address opaque_white() { std::lock_guard lock(colors_mutex); if (!white) white = opaque_color(1); return white; }
    void context_double(Address context, std::string_view function, std::initializer_list<double> values) {
        CallRegs registers;
        registers.x[0] = context;
        unsigned index = 0;
        for (const auto value : values) registers.set_d(index++, value);
        static_cast<void>(call_regs(core_graphics_library(), function, registers));
    }
    struct Raster {
        std::shared_ptr<std::vector<std::uint8_t>> pixels;
        std::uint32_t width{}, height{};
        Rect rect;   // layer points the pixels cover
    };
    // Premultiplied BGRA pixels of `draw` over `rect` (layer points) at
    // `scale`, first row at the visual top: the layer's max y, or its min y
    // where the layer's space is flipped.
    std::optional<Raster> rasterize(Rect rect, double scale, bool flipped, const std::function<void(Address)>& draw) {
        if (!(rect.size.width > 0) || !(rect.size.height > 0) || !std::isfinite(rect.size.width) || !std::isfinite(rect.size.height)) return std::nullopt;
        if (!(scale > 0)) scale = 1;
        const auto width = static_cast<std::uint32_t>(std::ceil(rect.size.width * scale - 1e-9));
        const auto height = static_cast<std::uint32_t>(std::ceil(rect.size.height * scale - 1e-9));
        if (width == 0 || height == 0 || width > 8192 || height > 8192) return std::nullopt;
        const auto context = quartz2d::create_bitmap_context(width, height, scale);
        struct Release { QuartzCore& q; Address object; ~Release() { q.cf_release(object); } } held{*this, context};
        if (flipped) {
            context_double(context, "_CGContextTranslateCTM", {0, height / scale});
            context_double(context, "_CGContextScaleCTM", {1, -1});
        }
        context_double(context, "_CGContextTranslateCTM", {-rect.origin.x, -rect.origin.y});
        draw(context);
        const auto data = call(core_graphics_library(), "_CGBitmapContextGetData", {context});
        const auto stride = call(core_graphics_library(), "_CGBitmapContextGetBytesPerRow", {context});
        if (!data || stride < width * 4ULL) return std::nullopt;
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t row = 0; row < height; ++row)
            memory().read_bytes(data + row * stride, std::span(pixels->data() + static_cast<std::size_t>(row) * width * 4, static_cast<std::size_t>(width) * 4));
        return Raster{std::move(pixels), width, height, rect};
    }
    // The layer's own drawing into a new backing store (its contents).
    void draw_backing_store(Address self) {
        Rect bounds;
        double scale{};
        { const auto state = layer(self); std::lock_guard lock(state->mutex); bounds = state->bounds; scale = state->contents_scale; }
        if (!(bounds.size.width > 0) || !(bounds.size.height > 0)) return;
        if (!(scale > 0)) scale = 1;
        const auto width = static_cast<std::uint32_t>(std::ceil(bounds.size.width * scale - 1e-9));
        const auto height = static_cast<std::uint32_t>(std::ceil(bounds.size.height * scale - 1e-9));
        if (width > 16384 || height > 16384) fail("-[CALayer display] backing store larger than 16384 pixels");
        const auto context = quartz2d::create_bitmap_context(width, height, scale);
        struct Release { QuartzCore& q; Address object; ~Release() { q.cf_release(object); } } held{*this, context};
        // The layer's own coordinates; flipped where its contents are.
        if (send(self, "contentsAreFlipped") & 0xff) {
            context_double(context, "_CGContextTranslateCTM", {0, height / scale});
            context_double(context, "_CGContextScaleCTM", {1, -1});
        }
        context_double(context, "_CGContextTranslateCTM", {-bounds.origin.x, -bounds.origin.y});
        CallRegs registers;
        registers.x[2] = context;
        static_cast<void>(objc.send(self, objc.selector("drawInContext:"), registers));
        const auto image = call(core_graphics_library(), "_CGBitmapContextCreateImage", {context});
        static_cast<void>(send(self, "setContents:", {image}));
        cf_release(image);
    }
    // -renderInContext:: the tree composited over the bounds at the context's device scale.
    void render_in_context(Address self, Address context) {
        if (!context) return;
        auto tree_root = snapshot(self);
        Rect bounds = tree_root.bounds;
        if (!(bounds.size.width > 0) || !(bounds.size.height > 0)) return;
        CallRegs query;
        query.x[0] = context;
        const auto device = call_regs(core_graphics_library(), "_CGContextGetUserSpaceToDeviceSpaceTransform", query);
        double scale = std::hypot(device.d(0), device.d(1));
        if (!(scale > 0) || !std::isfinite(scale)) scale = 1;
        const auto width = static_cast<std::uint32_t>(std::ceil(bounds.size.width * scale - 1e-9));
        const auto height = static_cast<std::uint32_t>(std::ceil(bounds.size.height * scale - 1e-9));
        if (width == 0 || height == 0 || width > 8192 || height > 8192) return;
        resolve_contents(tree_root);
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
        tree::Target target{width, height, static_cast<std::size_t>(width) * 4, pixels.data()};
        const tree::Placement placement{{{0, 0}, {width / scale, height / scale}}, height / scale, scale};
        static_cast<void>(tree::compose(tree_root, placement, target, {}));
        const auto bitmap = quartz2d::create_bitmap_context(width, height, 1);
        struct Release { QuartzCore& q; Address object; ~Release() { q.cf_release(object); } } held{*this, bitmap};
        const auto data = call(core_graphics_library(), "_CGBitmapContextGetData", {bitmap});
        const auto stride = call(core_graphics_library(), "_CGBitmapContextGetBytesPerRow", {bitmap});
        for (std::uint32_t row = 0; row < height; ++row)
            memory().write_bytes(data + row * stride, std::span<const std::uint8_t>(pixels.data() + static_cast<std::size_t>(row) * width * 4, static_cast<std::size_t>(width) * 4));
        const auto image = call(core_graphics_library(), "_CGBitmapContextCreateImage", {bitmap});
        CallRegs draw;
        draw.x[0] = context;
        draw.x[1] = image;
        draw.set_d(0, bounds.origin.x); draw.set_d(1, bounds.origin.y); draw.set_d(2, bounds.size.width); draw.set_d(3, bounds.size.height);
        static_cast<void>(call_regs(core_graphics_library(), "_CGContextDrawImage", draw));
        cf_release(image);
    }
    // Contents objects of a snapshot resolved for an offscreen composite (renderInContext:).
    void resolve_contents(tree::Layer& value) {
        // CGImage contents through a bitmap context of their size (other
        // contents objects are left out of the offscreen composite).
        const auto image = value.contents_object;
        if (image && !value.contents &&
            cf.invoke("CFGetTypeID", std::array<Address, 1>{image}) == call(core_graphics_library(), "_CGImageGetTypeID", {})) {
            const auto width = call(core_graphics_library(), "_CGImageGetWidth", {image});
            const auto height = call(core_graphics_library(), "_CGImageGetHeight", {image});
            if (width && height && width <= 8192 && height <= 8192) {
                if (auto raster = rasterize({{0, 0}, {static_cast<double>(width), static_cast<double>(height)}}, 1, false, [&](Address context) {
                        CallRegs draw;
                        draw.x[0] = context;
                        draw.x[1] = image;
                        draw.set_d(2, static_cast<double>(width));
                        draw.set_d(3, static_cast<double>(height));
                        static_cast<void>(call_regs(core_graphics_library(), "_CGContextDrawImage", draw));
                    })) {
                    value.owned_contents = raster->pixels;
                    value.contents = tree::Image{raster->width, raster->height, static_cast<std::size_t>(raster->width) * 4, raster->pixels->data(), false};
                }
            }
        }
        for (auto& child : value.sublayers) resolve_contents(child);
    }

    void install_layer_animations() {
        const auto L = layer_class;
        method(L, "addAnimation:forKey:", [this](CpuState& s, GuestMemory&) { add_animation(s.x[0], s.x[2], s.x[3]); }, "v@:@@");
        method(L, "animationForKey:", [this](CpuState& s, GuestMemory&) {
            const auto self = s.x[0], key = s.x[2];
            s.x[0] = 0;
            if (!key) return;
            const auto state = layer(self);
            std::vector<std::pair<Address, Address>> entries;
            { std::lock_guard lock(state->mutex); for (const auto& a : state->animations) entries.emplace_back(a.key, a.animation); }
            for (const auto& [k, animation] : entries)
                if (k && cf.equal(k, key)) { s.x[0] = animation; return; }
        }, "@@:@");
        method(L, "animationKeys", [this](CpuState& s, GuestMemory&) {
            const auto state = layer(s.x[0]);
            std::vector<Address> keys;
            { std::lock_guard lock(state->mutex); for (const auto& a : state->animations) if (a.key) keys.push_back(a.key); }
            s.x[0] = keys.empty() ? 0 : send(cf.array(keys), "autorelease");
        }, "@@:");
        method(L, "removeAnimationForKey:", [this](CpuState& s, GuestMemory&) {
            if (s.x[2]) remove_animations(s.x[0], s.x[2]);
        }, "v@:@");
        method(L, "removeAllAnimations", [this](CpuState& s, GuestMemory&) { remove_animations(s.x[0], std::nullopt); }, "v@:");
    }
    void add_animation(Address self, Address animation, Address key) {
        if (!animation) fail("-[CALayer addAnimation:forKey:] with a nil animation");
        if (&process.calling_engine() != &process.main_engine() || !main_run_loop_available())
            fail("unsupported: -[CALayer addAnimation:forKey:] off the main thread (animation timing runs on the main run loop)");
        const auto copy = send(animation, "copy");
        const auto now = monotonic_seconds();
        auto begin = objc.send(copy, "beginTime").d(0);
        if (begin == 0) {
            CallRegs registers;
            registers.set_d(0, now);
            static_cast<void>(objc.send(copy, "setBeginTime:", registers));
            begin = now;
        }
        auto duration = objc.send(copy, "duration").d(0);
        if (!(duration > 0)) duration = 0.25;   // Core Animation's default
        const double repeat_count = objc.send(copy, "repeatCount").s(0);
        const auto repeat_duration = objc.send(copy, "repeatDuration").d(0);
        const double speed = objc.send(copy, "speed").s(0);
        const bool autoreverses = (send(copy, "autoreverses") & 0xff) != 0;
        double active = repeat_duration > 0 ? repeat_duration : duration * (repeat_count > 0 ? repeat_count : 1.0);
        if (autoreverses && !(repeat_duration > 0)) active *= 2;
        if (speed == 0 || !std::isfinite(active)) active = std::numeric_limits<double>::infinity();
        else active /= std::abs(speed);
        const auto key_copy = key ? send(key, "copy") : 0;

        if (key) remove_animations(self, key);   // a new animation replaces the one under its key
        static_cast<void>(retain(self));         // timed animations keep their layer
        auto& loop = main_run_loop();
        const auto start = loop.add_timer(begin, 0, common_modes, [this, self, copy](TimerId) { animation_started(self, copy); });
        TimerId stop{};
        if (std::isfinite(active))
            stop = loop.add_timer(begin + active, 0, common_modes, [this, self, copy](TimerId) { animation_finished(self, copy); });
        const auto state = layer(self);
        std::lock_guard lock(state->mutex);
        state->animations.push_back({key_copy, copy, start, stop});
    }
    static LayerState::Animation* find_animation(LayerState& state, Address animation) {
        for (auto& a : state.animations) if (a.animation == animation) return &a;
        return nullptr;
    }
    void animation_started(Address self, Address animation) {
        const auto state = layer(self);
        {
            std::lock_guard lock(state->mutex);
            auto* entry = find_animation(*state, animation);
            if (!entry || entry->started) return;
            entry->started = true;
            entry->start_timer = 0;
        }
        const auto delegate = send(animation, "delegate");
        CallRegs registers;
        registers.x[2] = animation;
        if (delegate) static_cast<void>(objc.send_if_responds(delegate, objc.selector("animationDidStart:"), registers));
    }
    void animation_finished(Address self, Address animation) {
        animation_started(self, animation);
        const auto state = layer(self);
        {
            std::lock_guard lock(state->mutex);
            auto* entry = find_animation(*state, animation);
            if (!entry || entry->stopped) return;
            entry->stopped = true;
            entry->stop_timer = 0;
        }
        static_cast<void>(retain(animation));
        stop_delegate(animation, true);
        if ((send(animation, "isRemovedOnCompletion") & 0xff) != 0) {
            std::optional<LayerState::Animation> removed;
            {
                std::lock_guard lock(state->mutex);
                const auto it = std::find_if(state->animations.begin(), state->animations.end(),
                                             [&](const LayerState::Animation& a) { return a.animation == animation; });
                if (it != state->animations.end()) { removed = *it; state->animations.erase(it); }
            }
            if (removed) { release(removed->key); release(removed->animation); release(self); }
        }
        release(animation);
    }
    void stop_delegate(Address animation, bool finished) {
        const auto delegate = send(animation, "delegate");
        if (!delegate) return;
        CallRegs registers;
        registers.x[2] = animation;
        registers.x[3] = finished ? 1 : 0;
        static_cast<void>(objc.send_if_responds(delegate, objc.selector("animationDidStop:finished:"), registers));
    }
    // Removes the animations under `key` (every one when nullopt); those not
    // yet stopped report animationDidStop:finished:NO.
    void remove_animations(Address self, std::optional<Address> key) {
        const auto state = layer(self);
        std::vector<LayerState::Animation> removed;
        {
            std::lock_guard lock(state->mutex);
            for (auto it = state->animations.begin(); it != state->animations.end();) {
                if (key && !(it->key && cf.equal(it->key, *key))) { ++it; continue; }
                removed.push_back(*it);
                it = state->animations.erase(it);
            }
        }
        for (const auto& entry : removed) {
            if (main_run_loop_available()) {
                if (entry.start_timer) main_run_loop().cancel_timer(entry.start_timer);
                if (entry.stop_timer) main_run_loop().cancel_timer(entry.stop_timer);
            }
            if (!entry.stopped) stop_delegate(entry.animation, false);
            release(entry.key);
            release(entry.animation);
            release(self);
        }
    }

    // ---- CAGradientLayer ---------------------------------------------------------------------
    // The gradient's description: colors (CGColorRefs), locations, start and
    // end points in the unit coordinate space, type. Alchemy composites
    // layer contents only, so the gradient itself is not drawn.
    void install_gradient_layer() {
        const auto G = declare("CAGradientLayer", layer_class, true);
        gradient_layer_class = G;
        const auto object = [&](const char* getter, const char* setter, Address GradientLayerState::*member, const char* fallback) {
            method(G, getter, [this, member, fallback](CpuState& s, GuestMemory&) {
                Address value{};
                { const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex); value = state->gradient.*member; }
                s.x[0] = value ? value : (fallback ? cf.constant_string(fallback) : 0);
            }, "@@:");
            method(G, setter, [this, member](CpuState& s, GuestMemory&) {
                const auto value = s.x[2] ? send(s.x[2], "copy") : 0;
                const auto state = layer(s.x[0]);
                Address old{};
                { std::lock_guard lock(state->mutex); old = std::exchange(state->gradient.*member, value); }
                release(old);
            }, "v@:@");
        };
        object("colors", "setColors:", &GradientLayerState::colors, nullptr);
        object("locations", "setLocations:", &GradientLayerState::locations, nullptr);
        object("type", "setType:", &GradientLayerState::type, "axial");
        const auto point = [&](const char* getter, const char* setter, Point GradientLayerState::*member) {
            method(G, getter, [this, member](CpuState& s, GuestMemory&) {
                const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex);
                return_real(s, 0, (state->gradient.*member).x); return_real(s, 1, (state->gradient.*member).y);
            }, std::string(point_type) + "@:");
            method(G, setter, [this, member](CpuState& s, GuestMemory&) {
                const auto state = layer(s.x[0]); std::lock_guard lock(state->mutex);
                state->gradient.*member = {real(s, 0), real(s, 1)};
            }, std::string("v@:") + point_type);
        };
        point("startPoint", "setStartPoint:", &GradientLayerState::start);
        point("endPoint", "setEndPoint:", &GradientLayerState::end);
    }
    void insert(Address parent, Address child, std::optional<std::uint32_t> index) {
        if (!child) fail("-[CALayer addSublayer:] with a nil layer");
        if (!is_kind(child, layer_class)) fail("-[CALayer addSublayer:] argument is not a CALayer");
        if (child == parent) fail("-[CALayer addSublayer:] would make a layer its own sublayer");
        static_cast<void>(retain(child));   // survives removal from its old parent
        remove_from_parent(child);
        const auto state = layer(parent);
        {
            std::lock_guard lock(state->mutex);
            const auto at = index ? std::min<std::size_t>(*index, state->sublayers.size()) : state->sublayers.size();
            state->sublayers.insert(state->sublayers.begin() + static_cast<std::ptrdiff_t>(at), child);
        }
        {
            const auto child_state = layer(child);
            std::lock_guard lock(child_state->mutex);
            child_state->superlayer = parent;
        }
        changed(parent);
    }
    void remove_from_parent(Address child) {
        const auto child_state = layer(child);
        Address parent{};
        { std::lock_guard lock(child_state->mutex); parent = std::exchange(child_state->superlayer, 0); }
        if (!parent) return;
        const auto state = layer(parent);
        bool found{};
        {
            std::lock_guard lock(state->mutex);
            const auto it = std::find(state->sublayers.begin(), state->sublayers.end(), child);
            if (it != state->sublayers.end()) { state->sublayers.erase(it); found = true; }
        }
        if (found) {
            changed(parent);
            release(child);
        }
    }
    // Autoresizing: a layer whose bounds size changed tells its sublayers
    // (-resizeSublayersWithOldSize:, which sends each one
    // -resizeWithOldSuperlayerSize:; both may be overridden).
    void resized(Address self, Size old, Size now) {
        if (old.width == now.width && old.height == now.height) return;
        { const auto state = layer(self); std::lock_guard lock(state->mutex); if (state->sublayers.empty()) return; }
        CallRegs registers;
        registers.set_d(0, old.width);
        registers.set_d(1, old.height);
        static_cast<void>(objc.send(self, objc.selector("resizeSublayersWithOldSize:"), registers));
    }
    // -resizeWithOldSuperlayerSize: applies autoresizingMask
    // (kCALayerMinXMargin 1, WidthSizable 2, MaxXMargin 4, MinYMargin 8,
    // HeightSizable 16, MaxYMargin 32): the superlayer's size change is shared
    // equally by the flexible margins and size of each axis.
    void autoresize(Address self, Size old) {
        Rect frame;
        std::uint32_t mask{};
        Address parent{};
        {
            const auto state = layer(self);
            std::lock_guard lock(state->mutex);
            mask = state->autoresizing_mask;
            parent = state->superlayer;
            frame = frame_of(*state);
        }
        if (!mask || !parent) return;
        Size now;
        { const auto state = layer(parent); std::lock_guard lock(state->mutex); now = state->bounds.size; }
        const auto share = [](double delta, double& origin, double& size, bool low, bool flexible, bool high) {
            const int parts = (low ? 1 : 0) + (flexible ? 1 : 0) + (high ? 1 : 0);
            if (parts == 0 || delta == 0) return;
            const double each = delta / parts;
            if (low) origin += each;
            if (flexible) size = std::max(0.0, size + each);
        };
        share(now.width - old.width, frame.origin.x, frame.size.width, mask & 1, mask & 2, mask & 4);
        share(now.height - old.height, frame.origin.y, frame.size.height, mask & 8, mask & 16, mask & 32);
        CallRegs registers;
        registers.set_d(0, frame.origin.x);
        registers.set_d(1, frame.origin.y);
        registers.set_d(2, frame.size.width);
        registers.set_d(3, frame.size.height);
        static_cast<void>(objc.send(self, objc.selector("setFrame:"), registers));
    }

    // ---- CAMetalLayer ---------------------------------------------------------------------
    static Size drawable_size(const LayerState& state) {
        if (state.metal.drawable_size) return *state.metal.drawable_size;
        return {state.bounds.size.width * state.contents_scale, state.bounds.size.height * state.contents_scale};
    }
    // The colorspace's name decides whether the stored values are linear.
    bool colorspace_is_linear(Address colorspace) {
        if (!colorspace) return false;
        const auto name = call(core_graphics_library(), "_CGColorSpaceCopyName", {colorspace});
        if (!name) return false;
        const auto text = cf.utf8(name);
        cf_release(name);
        return text.find("Linear") != std::string::npos;
    }
    static std::string_view core_graphics_library() {
        return "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
    }
    void install_metal_layer() {
        const auto M = metal_layer_class;
        method(M, "dealloc", [this, M](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            if (const auto state = objc.find_host_state<LayerState>(self)) {
                Address device{}, colorspace{}, metadata{};
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
                std::shared_ptr<Presenter> presenter;
#endif
                {
                    std::lock_guard lock(state->mutex);
                    device = std::exchange(state->metal.device, 0);
                    colorspace = std::exchange(state->metal.colorspace, 0);
                    metadata = std::exchange(state->metal.edr_metadata, 0);
                    state->metal.host.reset();
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
                    presenter = std::move(state->metal.presenter);
#endif
                }
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
                if (presenter) for (const auto texture : presenter->shutdown()) release(texture);
#endif
                release(device);
                cf_release(colorspace);
                release(metadata);
            }
            static_cast<void>(objc.send_super(self, M, objc.selector("dealloc")));
        }, "v@:");
        method(M, "device", [this](CpuState& s, GuestMemory&) {
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->metal.device;
        }, "@@:");
        method(M, "setDevice:", [this](CpuState& s, GuestMemory&) {
            const auto value = retain(s.x[2]);
            const auto state = metal_layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->metal.device, value); }
            release(old);
        }, "v@:@");
        method(M, "pixelFormat", [this](CpuState& s, GuestMemory&) {
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->metal.pixel_format;
        }, "Q@:");
        method(M, "setPixelFormat:", [this](CpuState& s, GuestMemory&) {
            if (const auto problem = layer_pixel_format_problem(s.x[2])) fail("-[CAMetalLayer setPixelFormat:]: " + *problem);
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); state->metal.pixel_format = s.x[2];
        }, "v@:Q");
        method(M, "drawableSize", [this](CpuState& s, GuestMemory&) {
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex);
            const auto size = drawable_size(*state);
            return_real(s, 0, size.width); return_real(s, 1, size.height);
        }, std::string(size_type) + "@:");
        method(M, "setDrawableSize:", [this](CpuState& s, GuestMemory&) {
            const Size size{std::clamp(finite_or_zero(real(s, 0)), 0.0, maximum_drawable_dimension),
                            std::clamp(finite_or_zero(real(s, 1)), 0.0, maximum_drawable_dimension)};
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); state->metal.drawable_size = size;
        }, std::string("v@:") + size_type);
        method(M, "maximumDrawableCount", [this](CpuState& s, GuestMemory&) {
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->metal.maximum_drawables;
        }, "Q@:");
        method(M, "setMaximumDrawableCount:", [this](CpuState& s, GuestMemory&) {
            if (s.x[2] < 2 || s.x[2] > 3) fail("-[CAMetalLayer setMaximumDrawableCount:] accepts only 2 or 3");
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex);
            state->metal.maximum_drawables = static_cast<std::uint32_t>(s.x[2]);
        }, "v@:Q");
        const auto boolean = [&](const char* getter, const char* setter, bool MetalLayerState::*member) {
            method(M, getter, [this, member](CpuState& s, GuestMemory&) {
                const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->metal.*member ? 1 : 0;
            }, "B@:");
            method(M, setter, [this, member](CpuState& s, GuestMemory&) {
                const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); state->metal.*member = flag(s, 2);
            }, "v@:B");
        };
        boolean("framebufferOnly", "setFramebufferOnly:", &MetalLayerState::framebuffer_only);
        boolean("displaySyncEnabled", "setDisplaySyncEnabled:", &MetalLayerState::display_sync);
        // Presentation is synchronous with -present, which already satisfies
        // a transaction-synchronized present (there is no separate commit).
        boolean("presentsWithTransaction", "setPresentsWithTransaction:", &MetalLayerState::presents_with_transaction);
        boolean("wantsExtendedDynamicRangeContent", "setWantsExtendedDynamicRangeContent:", &MetalLayerState::wants_edr);
        boolean("allowsNextDrawableTimeout", "setAllowsNextDrawableTimeout:", &MetalLayerState::allows_timeout);
        method(M, "colorspace", [this](CpuState& s, GuestMemory&) {
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->metal.colorspace;
        }, "^{CGColorSpace=}@:");
        method(M, "setColorspace:", [this](CpuState& s, GuestMemory&) {
            const auto value = cf_retain(s.x[2]);
            const bool linear = colorspace_is_linear(value);
            const auto state = metal_layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->metal.colorspace, value); state->metal.linear_colorspace = linear; }
            cf_release(old);
        }, "v@:^{CGColorSpace=}");
        method(M, "EDRMetadata", [this](CpuState& s, GuestMemory&) {
            const auto state = metal_layer(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->metal.edr_metadata;
        }, "@@:");
        method(M, "setEDRMetadata:", [this](CpuState& s, GuestMemory&) {
            const auto value = retain(s.x[2]);
            const auto state = metal_layer(s.x[0]);
            Address old{};
            { std::lock_guard lock(state->mutex); old = std::exchange(state->metal.edr_metadata, value); }
            release(old);
        }, "v@:@");
        method(M, "nextDrawable", [this](CpuState& s, GuestMemory&) { s.x[0] = next_drawable(s.x[0]); }, "@@:");
    }

    bool stopping() { return engine_stop_requested(process.calling_engine()); }
    void release_textures(const std::vector<Address>& textures) { for (const auto texture : textures) release(texture); }

    Address next_drawable(Address self) {
        alchemy::perf::Scope timed(alchemy::perf::next_drawable_ticks, alchemy::perf::next_drawable);
        alchemy::perf::phase("first nextDrawable");
        const auto state = metal_layer(self);
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
        DrawableConfig config;
        Address device{};
        std::optional<LayerHost> host;
        bool allows_timeout{};
        std::shared_ptr<Presenter> presenter, replaced;
        std::uint64_t id{};
        {
            std::lock_guard lock(state->mutex);
            device = state->metal.device;
            const auto size = drawable_size(*state);
            config.width = static_cast<std::uint32_t>(std::lround(size.width));
            config.height = static_cast<std::uint32_t>(std::lround(size.height));
            config.pixel_format = state->metal.pixel_format;
            config.maximum_drawable_count = state->metal.maximum_drawables;
            config.framebuffer_only = state->metal.framebuffer_only;
            config.linear_content = state->metal.linear_colorspace || state->metal.pixel_format == pixel_format::bgra8_unorm_srgb;
            host = state->metal.host;
            allows_timeout = state->metal.allows_timeout;
            presenter = state->metal.presenter;
        }
        // Documented nil results: no device, or an empty drawable size.
        if (!device || !config.width || !config.height) return 0;
        const auto bridge = find_metal_device(device);
        if (!bridge) fail("unsupported: -[CAMetalLayer nextDrawable] with a device that is not a Vulkan-backed Metal device");
        if (!host)
            fail("unsupported: -[CAMetalLayer nextDrawable] for a layer that is not shown in a window "
                 "(offscreen CAMetalLayer presentation is not implemented)");
        if (!presenter || presenter->device() != bridge) {
            auto created = std::make_shared<Presenter>(bridge);
            std::lock_guard lock(state->mutex);
            replaced = std::exchange(state->metal.presenter, created);
            presenter = created;
        }
        if (replaced) release_textures(replaced->shutdown());
        std::shared_ptr<DrawableImage> image;
        try {
            image = presenter->acquire(config, allows_timeout ? drawable_timeout_seconds : std::numeric_limits<double>::infinity(),
                                       [this] { return stopping(); });
        } catch (const PresenterStopped&) {
            EngineAdapter::stop_thread(process.calling_engine(), "-[CAMetalLayer nextDrawable]");
        }
        release_textures(presenter->take_retired_textures());
        if (!image) return 0;   // timed out
        Address texture{};
        try { texture = presenter->texture_for(image); }
        catch (...) { presenter->release(image); throw; }
        {
            std::lock_guard lock(state->mutex);
            id = state->metal.next_drawable_id++;
        }
        Address object{};
        try {
            object = objc.allocate(drawable_class);
            const auto value = drawable(object);
            value->layer = retain(self);
            value->presenter = presenter;
            value->image = std::move(image);
            value->texture = texture;
            value->id = id;
        } catch (...) {
            if (object) release(object); else presenter->release(image);
            throw;
        }
        return send(object, "autorelease");
#else
        {
            std::lock_guard lock(state->mutex);
            if (!state->metal.device) return 0;   // no device: nil, as in a Vulkan build
        }
        fail("unsupported: -[CAMetalLayer nextDrawable] (alchemy-fx was built without Vulkan headers)");
#endif
    }

    // ---- CAMetalDrawable ------------------------------------------------------------------
    void install_drawable() {
        const auto D = drawable_class;
        objc.add_native_protocol(D, "CAMetalDrawable");
        objc.add_native_protocol(D, "MTLDrawable");
        method(D, "texture", [this](CpuState& s, GuestMemory&) {
            const auto state = drawable(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->texture;
        }, "@@:");
        method(D, "layer", [this](CpuState& s, GuestMemory&) {
            const auto state = drawable(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->layer;
        }, "@@:");
        method(D, "drawableID", [this](CpuState& s, GuestMemory&) {
            const auto state = drawable(s.x[0]); std::lock_guard lock(state->mutex); s.x[0] = state->id;
        }, "Q@:");
        method(D, "presentedTime", [this](CpuState& s, GuestMemory&) {
            const auto state = drawable(s.x[0]); std::lock_guard lock(state->mutex); return_real(s, 0, state->presented_time);
        }, "d@:");
        method(D, "present", [this](CpuState& s, GuestMemory&) { present(s.x[0]); }, "v@:");
        // No display-timing extension is used: a timed present is queued now
        // and shown at the next vertical blank (displaySyncEnabled) like an
        // immediate one.
        method(D, "presentAtTime:", [this](CpuState& s, GuestMemory&) { present(s.x[0]); }, "v@:d");
        method(D, "presentAfterMinimumDuration:", [this](CpuState& s, GuestMemory&) { present(s.x[0]); }, "v@:d");
        method(D, "addPresentedHandler:", [this](CpuState& s, GuestMemory&) {
            if (!s.x[2]) fail("-[CAMetalDrawable addPresentedHandler:] with a nil block");
            const auto state = drawable(s.x[0]);
            {
                std::lock_guard lock(state->mutex);
                if (state->presented) fail("-[CAMetalDrawable addPresentedHandler:] after the drawable was presented");
            }
            static_cast<void>(completion_thread());   // must start on a guest thread, before any present
            const auto block = call(libsystem, "__Block_copy", {s.x[2]});
            std::lock_guard lock(state->mutex);
            state->presented_handlers.push_back(block);
        }, "v@:@?");
        method(D, "dealloc", [this, D](CpuState& s, GuestMemory&) {
            const auto self = s.x[0];
            if (const auto state = objc.find_host_state<DrawableState>(self)) {
                Address layer_object{};
                std::vector<Address> handlers;
                {
                    std::lock_guard lock(state->mutex);
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
                    if (state->image && state->presenter) state->presenter->release(state->image);
                    state->image.reset();
                    state->presenter.reset();
#endif
                    layer_object = std::exchange(state->layer, 0);
                    handlers = std::exchange(state->presented_handlers, {});
                }
                for (const auto block : handlers) static_cast<void>(call(libsystem, "__Block_release", {block}));
                release(layer_object);
            }
            static_cast<void>(objc.send_super(self, D, objc.selector("dealloc")));
        }, "v@:");
    }
    std::shared_ptr<CompletionThread> completion_thread() {
        std::lock_guard lock(completion_mutex);
        if (!completion) completion = start_completion_thread(process, "com.apple.coreanimation.presented");
        return completion;
    }
    void present(Address self) {
        const auto state = drawable(self);
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
        std::shared_ptr<Presenter> presenter;
        std::shared_ptr<DrawableImage> image;
        Address layer_object{};
        {
            std::lock_guard lock(state->mutex);
            if (state->presented) return;   // a drawable presents once
            state->presented = true;
            presenter = state->presenter;
            image = std::move(state->image);
            layer_object = state->layer;
        }
        if (!presenter || !image) return;
        PresentTarget target;
        {
            const auto layer_state = layer(layer_object);
            std::lock_guard lock(layer_state->mutex);
            target.display_sync = layer_state->metal.display_sync;
            if (const auto& host = layer_state->metal.host) {
                target.window = host->native_window;
                target.x = host->frame.origin.x * host->backing_scale;
                target.y = host->frame.origin.y * host->backing_scale;
                target.width = host->frame.size.width * host->backing_scale;
                target.height = host->frame.size.height * host->backing_scale;
                if (!target.window && host->window) target.window = window_server().native_handle(host->window);
            }
        }
        try {
            static_cast<void>(presenter->present(image, target, [this] { return stopping(); }));
        } catch (const PresenterStopped&) {
            EngineAdapter::stop_thread(process.calling_engine(), "-[CAMetalDrawable present]");
        }
        release_textures(presenter->take_retired_textures());
        std::vector<Address> handlers;
        {
            std::lock_guard lock(state->mutex);
            state->presented_time = monotonic_seconds();
            handlers = std::exchange(state->presented_handlers, {});
        }
        if (handlers.empty()) return;
        // Presented handlers receive the drawable, kept alive until the last ran.
        const auto thread = completion_thread();
        for (const auto block : handlers) {
            static_cast<void>(retain(self));
            thread->post_block(block, {self});
            thread->post([this, self] { release(self); });
        }
#else
        static_cast<void>(state);
        fail("unsupported: -[CAMetalDrawable present] (alchemy-fx was built without Vulkan headers)");
#endif
    }

    // ---- CATransaction ----------------------------------------------------------------------
    Address block_copy(Address block) { return call("/usr/lib/libSystem.B.dylib", "__Block_copy", {block}); }
    void block_release(Address block) { static_cast<void>(call("/usr/lib/libSystem.B.dylib", "__Block_release", {block})); }
    // Runs and releases copied blocks (no lock is held).
    void run_blocks(const std::vector<Address>& blocks) {
        for (const auto block : blocks) {
            struct Release { QuartzCore& self; Address block; ~Release() { try { self.block_release(block); } catch (...) {} } } owned{*this, block};
            const std::array<Address, 1> arguments{block};
            static_cast<void>(process.calling_engine().invoke_callback(memory().read64(block + 16), arguments));
        }
    }
    // The block at the next step of the calling thread's run loop (common modes).
    void perform_soon(Address block) {
        const auto core = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
        const auto modes = process.resolve(core, "_kCFRunLoopCommonModes");
        if (!modes) fail("CATransaction needs kCFRunLoopCommonModes");
        const auto loop = call(core, "_CFRunLoopGetCurrent", {});
        static_cast<void>(call(core, "_CFRunLoopPerformBlock", {loop, memory().read64(modes->address), block}));
        static_cast<void>(call(core, "_CFRunLoopWakeUp", {loop}));
    }
    // Layer properties change at once (there is no render server); the
    // composition of layer trees into windows (layer_contents.cpp) happens
    // when the outermost transaction commits, or for changes outside any
    // explicit transaction at the end of the run-loop step (the implicit
    // transaction).
    void install_transaction() {
        const auto T = transaction_class;
        const auto current = []() -> TransactionState::Level& {
            if (transactions.levels.empty()) transactions.levels.emplace_back();   // the implicit transaction
            return transactions.levels.back();
        };
        method(T, "begin", [current](CpuState&, GuestMemory&) {
            auto outer = current();
            outer.completion = 0;   // each transaction has its own completion block
            transactions.levels.push_back(outer);
            ++transactions.explicit_depth;
        }, "v@:", true);
        // The outermost commit of a thread (and +flush) tells the commit
        // observers (layer_contents.cpp composites the changed trees). Its
        // commit handlers run around that: pre-layout and pre-commit before,
        // post-commit after. A transaction's completion block runs at the next
        // step of the thread's run loop (there are no animations to wait for).
        method(T, "commit", [this](CpuState&, GuestMemory&) {
            Address completion{};
            if (!transactions.levels.empty()) {
                completion = transactions.levels.back().completion;
                transactions.levels.pop_back();
            }
            if (transactions.explicit_depth == 0) { if (completion) { perform_soon(completion); block_release(completion); } return; }
            if (--transactions.explicit_depth == 0) {
                auto handlers = std::exchange(transactions.commit_handlers, {});
                run_blocks(handlers[0]);   // kCATransactionPhasePreLayout
                run_blocks(handlers[1]);   // kCATransactionPhasePreCommit
                committed();
                run_blocks(handlers[2]);   // kCATransactionPhasePostCommit
            }
            if (completion) { perform_soon(completion); block_release(completion); }
        }, "v@:", true);
        // +addCommitHandler:forPhase: (macOS 10.15): queued for the outermost
        // explicit transaction; outside one, the implicit transaction commits
        // at the next step of the thread's run loop, so the handler runs then.
        method(T, "addCommitHandler:forPhase:", [this](CpuState& s, GuestMemory&) {
            if (!s.x[2] || s.x[3] > 2) fail("+[CATransaction addCommitHandler:forPhase:] needs a block and a phase 0-2");
            if (transactions.explicit_depth) transactions.commit_handlers[s.x[3]].push_back(block_copy(s.x[2]));
            else perform_soon(s.x[2]);
        }, "v@:@?q", true);
        method(T, "completionBlock", [current](CpuState& s, GuestMemory&) { s.x[0] = current().completion; }, "@?@:", true);
        method(T, "setCompletionBlock:", [this, current](CpuState& s, GuestMemory&) {
            if (transactions.explicit_depth == 0) { if (s.x[2]) perform_soon(s.x[2]); return; }   // the implicit transaction
            const auto previous = std::exchange(current().completion, s.x[2] ? block_copy(s.x[2]) : 0);
            if (previous) block_release(previous);
        }, "v@:@?", true);
        method(T, "flush", [this](CpuState&, GuestMemory&) { if (transactions.explicit_depth == 0) committed(); }, "v@:", true);
        method(T, "lock", [](CpuState&, GuestMemory&) {}, "v@:", true);
        method(T, "unlock", [](CpuState&, GuestMemory&) {}, "v@:", true);
        method(T, "disableActions", [current](CpuState& s, GuestMemory&) { s.x[0] = current().disable_actions ? 1 : 0; }, "B@:", true);
        method(T, "setDisableActions:", [current](CpuState& s, GuestMemory&) { current().disable_actions = flag(s, 2); }, "v@:B", true);
        method(T, "animationDuration", [current](CpuState& s, GuestMemory&) { return_real(s, 0, current().duration); }, "d@:", true);
        method(T, "setAnimationDuration:", [current](CpuState& s, GuestMemory&) { current().duration = real(s, 0); }, "v@:d", true);
    }

    // ---- composition snapshot (layer_tree.hpp) --------------------------------------------
    std::optional<std::array<float, 4>> color_components(Address color) {
        if (!color) return std::nullopt;
        const auto count = call(core_graphics_library(), "_CGColorGetNumberOfComponents", {color});
        const auto components = call(core_graphics_library(), "_CGColorGetComponents", {color});
        if (!components || (count != 2 && count != 4)) return std::nullopt;
        std::array<double, 4> values{};
        for (std::uint64_t index = 0; index < count; ++index)
            values[index] = std::bit_cast<double>(memory().read64(components + index * 8));
        if (count == 2) return std::array<float, 4>{static_cast<float>(values[0]), static_cast<float>(values[0]), static_cast<float>(values[0]),
                                                    static_cast<float>(values[1])};
        return std::array<float, 4>{static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2]),
                                    static_cast<float>(values[3])};
    }
    static tree::Affine affine_of(const Transform3D& t) { return {t[0], t[1], t[4], t[5], t[12], t[13]}; }
    // Layers marked for display that draw themselves (a delegate that draws,
    // or a class with its own -drawInContext:) draw before they are shown; a
    // view's backing layer is drawn by AppKit.
    void display_if_drawn(Address object) {
        bool needed{};
        Address delegate{};
        { const auto state = layer(object); std::lock_guard lock(state->mutex); needed = state->needs_display; delegate = state->delegate; }
        if (!needed) return;
        if (const auto view_class = objc.find_class("NSView"); delegate && view_class && is_kind(delegate, view_class)) return;
        const bool delegate_draws = delegate && (objc.responds(delegate, objc.selector("displayLayer:")) ||
                                                 objc.responds(delegate, objc.selector("drawLayer:inContext:")));
        const auto draw = objc.selector("drawInContext:");
        const auto own = call("/usr/lib/libobjc.A.dylib", "_class_getMethodImplementation", {objc.object_class(object), draw});
        const auto base = call("/usr/lib/libobjc.A.dylib", "_class_getMethodImplementation", {layer_class, draw});
        if (!delegate_draws && own == base) return;
        static_cast<void>(send(object, "displayIfNeeded"));
    }
    Rect path_box(Address path) {
        CallRegs registers;
        registers.x[0] = path;
        const auto r = call_regs(core_graphics_library(), "_CGPathGetPathBoundingBox", registers);
        return {{r.d(0), r.d(1)}, {r.d(2), r.d(3)}};
    }
    static void own_contents(tree::Layer& out, const Raster& raster) {
        out.owned_contents = raster.pixels;
        out.contents = tree::Image{raster.width, raster.height, static_cast<std::size_t>(raster.width) * 4, raster.pixels->data(), false};
        out.contents_frame = raster.rect;
        out.contents_object = 0;
    }
    // The shadow path's coverage in layer space.
    std::optional<tree::Mask> path_mask(Address path, double scale) {
        const auto box = path_box(path);
        auto raster = rasterize(box, scale, false, [&](Address context) {
            const auto cg = core_graphics_library();
            static_cast<void>(call(cg, "_CGContextAddPath", {context, path}));
            static_cast<void>(call(cg, "_CGContextSetFillColorWithColor", {context, opaque_black()}));
            static_cast<void>(call(cg, "_CGContextFillPath", {context}));
        });
        if (!raster) return std::nullopt;
        auto alpha = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(raster->width) * raster->height);
        for (std::size_t i = 0; i < alpha->size(); ++i) (*alpha)[i] = (*raster->pixels)[i * 4 + 3];
        return tree::Mask{raster->width, raster->height, raster->rect, std::move(alpha)};
    }
    // CAShapeLayer: the path filled and stroked; the pixels cover the path
    // (which need not lie in the bounds).
    void draw_shape(Address object, tree::Layer& out, bool flipped) {
        ShapeLayerState shape;
        {
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            shape = state->shape;
            for (const auto value : {shape.path, shape.fill_color, shape.stroke_color}) static_cast<void>(cf_retain(value));
            for (const auto value : {shape.fill_rule, shape.line_cap, shape.line_join, shape.dash_pattern}) static_cast<void>(retain(value));
        }
        struct Held {
            QuartzCore& q;
            ShapeLayerState& s;
            ~Held() {
                for (const auto value : {s.path, s.fill_color, s.stroke_color}) q.cf_release(value);
                for (const auto value : {s.fill_rule, s.line_cap, s.line_join, s.dash_pattern}) q.release(value);
            }
        } held{*this, shape};
        if (!shape.path) return;
        const auto fill = shape.fill_set ? shape.fill_color : opaque_black();
        const auto name = [this](Address value, const char* fallback) { return value ? cf.utf8(value) : std::string(fallback); };
        const bool even_odd = name(shape.fill_rule, "non-zero") == "even-odd";
        const auto cap = name(shape.line_cap, "butt"), join = name(shape.line_join, "miter");
        const bool stroking = shape.stroke_color && shape.line_width > 0;
        auto box = path_box(shape.path);
        if (!std::isfinite(box.origin.x) || !std::isfinite(box.size.width)) return;
        const double grow = (stroking ? shape.line_width * std::clamp(shape.miter_limit, 1.0, 10.0) / 2 : 0) + 1;
        box = {{box.origin.x - grow, box.origin.y - grow}, {box.size.width + 2 * grow, box.size.height + 2 * grow}};
        std::vector<double> dashes;
        if (shape.dash_pattern)
            for (std::uint64_t i = 0, n = cf.length(shape.dash_pattern); i < n; ++i)
                dashes.push_back(objc.send(cf.element(shape.dash_pattern, i), objc.selector("doubleValue"), {}).d(0));
        const auto raster = rasterize(box, out.contents_scale, flipped, [&](Address context) {
            const auto cg = core_graphics_library();
            if (fill) {
                static_cast<void>(call(cg, "_CGContextAddPath", {context, shape.path}));
                static_cast<void>(call(cg, "_CGContextSetFillColorWithColor", {context, fill}));
                static_cast<void>(call(cg, even_odd ? "_CGContextEOFillPath" : "_CGContextFillPath", {context}));
            }
            if (!stroking) return;
            Address trimmed{};
            if (shape.stroke_start > 0 || shape.stroke_end < 1)
                trimmed = quartz2d::trimmed_path(shape.path, shape.stroke_start, shape.stroke_end);
            static_cast<void>(call(cg, "_CGContextAddPath", {context, trimmed ? trimmed : shape.path}));
            static_cast<void>(call(cg, "_CGContextSetStrokeColorWithColor", {context, shape.stroke_color}));
            context_double(context, "_CGContextSetLineWidth", {shape.line_width});
            static_cast<void>(call(cg, "_CGContextSetLineCap", {context, cap == "round" ? 1u : cap == "square" ? 2u : 0u}));
            static_cast<void>(call(cg, "_CGContextSetLineJoin", {context, join == "round" ? 1u : join == "bevel" ? 2u : 0u}));
            context_double(context, "_CGContextSetMiterLimit", {shape.miter_limit});
            Address lengths{};
            if (!dashes.empty()) {
                lengths = process.allocate_guest(dashes.size() * 8, 8);
                for (std::size_t i = 0; i < dashes.size(); ++i) memory().write64(lengths + i * 8, std::bit_cast<std::uint64_t>(dashes[i]));
                CallRegs registers;
                registers.x[0] = context;
                registers.set_d(0, shape.dash_phase);
                registers.x[1] = lengths;
                registers.x[2] = dashes.size();
                static_cast<void>(call_regs(cg, "_CGContextSetLineDash", registers));
            }
            static_cast<void>(call(cg, "_CGContextStrokePath", {context}));
            if (lengths) process.release_guest(lengths);
            if (trimmed) cf_release(trimmed);
        });
        if (raster) own_contents(out, *raster);
    }
    // CAGradientLayer: the colours along the axis between the unit start and
    // end points ((0, 0) at the bounds' minimum corner), or as an ellipse
    // around the start point reaching the end point (radial). Conic
    // gradients are not drawn (reported once).
    void draw_gradient(Address object, tree::Layer& out, bool flipped) {
        Address colors{}, locations{}, type{};
        Point start, end;
        {
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            colors = retain(state->gradient.colors);
            locations = retain(state->gradient.locations);
            type = retain(state->gradient.type);
            start = state->gradient.start;
            end = state->gradient.end;
        }
        struct Held { QuartzCore& q; std::array<Address, 3> values; ~Held() { for (const auto v : values) q.release(v); } } held{*this, {colors, locations, type}};
        if (!colors || cf.length(colors) == 0) return;
        const auto kind = type ? cf.utf8(type) : std::string("axial");
        if (kind == "conic") {
            static std::once_flag reported;
            std::call_once(reported, [] { note_unsupported("CAGradientLayer conic gradients are not drawn"); });
            return;
        }
        const auto cg = core_graphics_library();
        const auto count = cf.length(colors);
        Address stops{};
        if (locations && cf.length(locations) >= count) {
            stops = process.allocate_guest(count * 8, 8);
            for (std::uint64_t i = 0; i < count; ++i)
                memory().write64(stops + i * 8, std::bit_cast<std::uint64_t>(objc.send(cf.element(locations, i), objc.selector("doubleValue"), {}).d(0)));
        }
        const auto space = call(cg, "_CGColorSpaceCreateDeviceRGB", {});
        const auto gradient = call(cg, "_CGGradientCreateWithColors", {space, colors, stops});
        if (stops) process.release_guest(stops);
        cf_release(space);
        if (!gradient) return;
        const auto& b = out.bounds;
        const Point p0{b.origin.x + start.x * b.size.width, b.origin.y + start.y * b.size.height};
        const Point p1{b.origin.x + end.x * b.size.width, b.origin.y + end.y * b.size.height};
        const auto raster = rasterize(b, out.contents_scale, flipped, [&](Address context) {
            CallRegs registers;
            registers.x[0] = context;
            registers.x[1] = gradient;
            if (kind == "radial") {
                const double rx = std::abs(p1.x - p0.x), ry = std::abs(p1.y - p0.y);
                if (!(rx > 0) || !(ry > 0)) return;
                context_double(context, "_CGContextTranslateCTM", {p0.x, p0.y});
                context_double(context, "_CGContextScaleCTM", {rx, ry});
                for (unsigned i = 0; i < 6; ++i) registers.set_d(i, i == 5 ? 1.0 : 0.0);
                registers.x[2] = 2;   // kCGGradientDrawsAfterEndLocation
                static_cast<void>(call_regs(cg, "_CGContextDrawRadialGradient", registers));
                return;
            }
            registers.set_d(0, p0.x); registers.set_d(1, p0.y); registers.set_d(2, p1.x); registers.set_d(3, p1.y);
            registers.x[2] = 3;       // before the start and after the end
            static_cast<void>(call_regs(cg, "_CGContextDrawLinearGradient", registers));
        });
        cf_release(gradient);
        if (raster) own_contents(out, *raster);
    }
    // A CTFont for CATextLayer's font (a CTFont / NSFont, a CGFont or a font
    // name; nil: Helvetica) at `size` (+1).
    Address text_font(Address font, double size) {
        const auto ct = core_text_library();
        CallRegs registers;
        registers.set_d(0, size);
        const auto by_name = [&](Address name) {
            registers.x[0] = name;
            registers.x[1] = 0;
            return call_regs(ct, "_CTFontCreateWithName", registers).x0;
        };
        if (font) {
            const auto type = cf.invoke("CFGetTypeID", std::array<Address, 1>{font});
            registers.x[0] = font;
            registers.x[1] = 0;
            registers.x[2] = 0;
            if (type == call(ct, "_CTFontGetTypeID", {})) return call_regs(ct, "_CTFontCreateCopyWithAttributes", registers).x0;
            if (type == call(core_graphics_library(), "_CGFontGetTypeID", {})) return call_regs(ct, "_CTFontCreateWithGraphicsFont", registers).x0;
            if (type == cf.invoke("CFStringGetTypeID", std::array<Address, 0>{})) return by_name(font);
        }
        const auto helvetica = cf.string("Helvetica");
        const auto made = by_name(helvetica);
        cf_release(helvetica);
        return made;
    }
    Address core_text_constant(std::string_view name) {
        const auto found = process.resolve(core_text_library(), name);
        if (!found) fail("missing CoreText constant " + std::string(name));
        return memory().read64(found->address);
    }
    // CATextLayer: the string drawn from the top of the bounds, wrapped or
    // on one line (truncated by the truncation mode), aligned by the
    // alignment mode. An attributed string keeps its own attributes.
    void draw_text(Address object, tree::Layer& out) {
        TextLayerState text;
        {
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            text = state->text;
            for (const auto value : {text.string, text.truncation, text.alignment}) static_cast<void>(retain(value));
            for (const auto value : {text.font, text.foreground}) static_cast<void>(cf_retain(value));
        }
        struct Held {
            QuartzCore& q;
            TextLayerState& t;
            ~Held() {
                for (const auto value : {t.string, t.truncation, t.alignment}) q.release(value);
                for (const auto value : {t.font, t.foreground}) q.cf_release(value);
            }
        } held{*this, text};
        if (!text.string) return;
        const auto ct = core_text_library();
        const auto align = text.alignment ? cf.utf8(text.alignment) : std::string("natural");
        const auto truncation = text.truncation ? cf.utf8(text.truncation) : std::string("none");
        Address attributed{};
        if (objc.responds(text.string, objc.selector("attributesAtIndex:effectiveRange:"))) {
            attributed = cf_retain(text.string);
        } else {
            const auto font = text_font(text.font, text.font_size);
            // CTParagraphStyleSetting {specifier, valueSize, value} with the alignment byte after it.
            const auto settings = process.allocate_guest(32, 8);
            memory().write32(settings, 0);   // kCTParagraphStyleSpecifierAlignment
            memory().write64(settings + 8, 1);
            memory().write64(settings + 16, settings + 24);
            memory().write8(settings + 24, align == "left" ? 0 : align == "right" ? 1 : align == "center" ? 2 : align == "justified" ? 3 : 4);
            const auto paragraph = call(ct, "_CTParagraphStyleCreate", {settings, 1});
            process.release_guest(settings);
            const std::vector<std::pair<Address, Address>> entries{{core_text_constant("_kCTFontAttributeName"), font},
                {core_text_constant("_kCTForegroundColorAttributeName"), text.foreground_set ? text.foreground : opaque_white()},
                {core_text_constant("_kCTParagraphStyleAttributeName"), paragraph}};
            std::vector<std::pair<Address, Address>> present;
            for (const auto& entry : entries) if (entry.second) present.push_back(entry);
            const auto attributes = cf.dictionary(present);
            attributed = call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFAttributedStringCreate",
                              {0, text.string, attributes});
            cf_release(attributes);
            cf_release(paragraph);
            cf_release(font);
        }
        if (!attributed) return;
        const auto& b = out.bounds;
        const auto raster = rasterize(b, out.contents_scale, false, [&](Address context) {
            if (text.wrapped) {
                const auto setter = call(ct, "_CTFramesetterCreateWithAttributedString", {attributed});
                CallRegs rect;
                rect.set_d(0, b.origin.x); rect.set_d(1, b.origin.y); rect.set_d(2, b.size.width); rect.set_d(3, b.size.height);
                rect.x[0] = 0;
                const auto path = call_regs(core_graphics_library(), "_CGPathCreateWithRect", rect).x0;
                const auto frame = call(ct, "_CTFramesetterCreateFrame", {setter, 0, 0, path, 0});
                if (frame) static_cast<void>(call(ct, "_CTFrameDraw", {frame, context}));
                for (const auto value : {frame, path, setter}) cf_release(value);
                return;
            }
            auto line = call(ct, "_CTLineCreateWithAttributedString", {attributed});
            if (!line) return;
            const auto metrics = process.allocate_guest(24, 8);
            CallRegs bounds_query;
            bounds_query.x[0] = line;
            bounds_query.x[1] = metrics; bounds_query.x[2] = metrics + 8; bounds_query.x[3] = metrics + 16;
            const double width = call_regs(ct, "_CTLineGetTypographicBounds", bounds_query).d(0);
            if (width > b.size.width && truncation != "none") {
                const auto ellipsis = cf.string("\xE2\x80\xA6");
                const auto first = call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFAttributedStringGetAttributes",
                                        {attributed, 0, 0});
                const auto token_string = call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
                                               "_CFAttributedStringCreate", {0, ellipsis, first});
                const auto token = call(ct, "_CTLineCreateWithAttributedString", {token_string});
                CallRegs truncate;
                truncate.x[0] = line;
                truncate.set_d(0, b.size.width);
                truncate.x[1] = truncation == "start" ? 0 : truncation == "middle" ? 2 : 1;
                truncate.x[2] = token;
                if (const auto shorter = call_regs(ct, "_CTLineCreateTruncatedLine", truncate).x0) { cf_release(line); line = shorter; }
                for (const auto value : {token, token_string, ellipsis}) cf_release(value);
                static_cast<void>(call_regs(ct, "_CTLineGetTypographicBounds", bounds_query));
            }
            const double ascent = std::bit_cast<double>(memory().read64(metrics));
            process.release_guest(metrics);
            CallRegs flush;
            flush.x[0] = line;
            flush.set_d(0, align == "center" ? 0.5 : align == "right" ? 1.0 : 0.0);
            flush.set_d(1, b.size.width);
            const double x = b.origin.x + call_regs(ct, "_CTLineGetPenOffsetForFlush", flush).d(0);
            context_double(context, "_CGContextSetTextPosition", {x, b.origin.y + b.size.height - ascent});
            static_cast<void>(call(ct, "_CTLineDraw", {line, context}));
            cf_release(line);
        });
        cf_release(attributed);
        if (raster) own_contents(out, *raster);
    }
    // CAReplicatorLayer: `count - 1` more copies of the sublayers, each one
    // instanceTransform (about the anchor point) after the previous, with
    // the alpha offset accumulated into its opacity (colour offsets and
    // instanceColor's colour components are not applied to the drawing).
    void replicate(Address object, tree::Layer& out) {
        ReplicatorLayerState replica;
        { const auto state = layer(object); std::lock_guard lock(state->mutex); replica = state->replicator; replica.color = 0; }
        if (replica.count <= 1 || out.sublayers.empty()) return;
        const auto originals = out.sublayers;
        const auto step = affine_of(replica.transform);
        tree::Affine accumulated;
        const auto& b = out.bounds;
        for (std::int64_t instance = 1; instance < std::min<std::int64_t>(replica.count, 1024); ++instance) {
            accumulated = step.after(accumulated);
            tree::Layer copy;
            copy.bounds = b;
            copy.anchor = out.anchor;
            copy.position = {b.origin.x + out.anchor.x * b.size.width, b.origin.y + out.anchor.y * b.size.height};
            copy.transform = accumulated;
            copy.opacity = std::clamp(1.0F + static_cast<float>(instance) * replica.alpha_offset, 0.0F, 1.0F);
            copy.sublayers = originals;
            out.sublayers.push_back(std::move(copy));
        }
    }
    static std::string_view core_text_library() { return "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText"; }
    static void note_unsupported(std::string_view what) {
        if (std::getenv("ALCHEMY_TRACE_LAYERS")) std::fprintf(stderr, "Alchemy QuartzCore: %.*s\n", static_cast<int>(what.size()), what.data());
    }
    // `flipped`: the layer's space is flipped relative to the root (an odd
    // number of geometry-flipped superlayers): its visual top is its min y.
    tree::Layer snapshot(Address object, unsigned depth = 0, bool flipped = false) {
        if (depth > 256) fail("CALayer tree deeper than 256 levels (a cycle?)");
        display_if_drawn(object);
        tree::Layer out;
        std::vector<Address> children;
        Address background{}, gravity{}, border{}, shadow_color{}, shadow_path{};
        {
            const auto state = layer(object);
            std::lock_guard lock(state->mutex);
            out.bounds = state->bounds;
            out.position = state->position;
            out.anchor = state->anchor;
            out.transform = affine_of(state->transform);
            out.sublayer_transform = affine_of(state->sublayer_transform);
            out.z_position = state->z_position;
            out.contents_scale = state->contents_scale;
            out.contents_rect = state->contents_rect;
            out.opacity = state->opacity;
            out.hidden = state->hidden;
            out.geometry_flipped = state->geometry_flipped;
            out.masks_to_bounds = state->masks_to_bounds;
            out.contents_object = state->contents;
            out.corner_radius = state->corner_radius;
            out.masked_corners = static_cast<std::uint32_t>(state->masked_corners);
            out.border_width = state->border_width;
            if (state->shadow_opacity > 0)
                out.shadow = tree::Shadow{{0, 0, 0, 1}, state->shadow_opacity, state->shadow_offset, state->shadow_radius, std::nullopt};
            background = state->background_color;
            gravity = state->contents_gravity;
            border = state->border_color;
            shadow_color = cf_retain(state->shadow_color);
            shadow_path = cf_retain(state->shadow_path);
            children = state->sublayers;
        }
        out.background = color_components(background);
        if (border) out.border = color_components(border);
        if (out.shadow) {
            if (shadow_color) if (const auto c = color_components(shadow_color)) out.shadow->color = *c;
            if (shadow_path) out.shadow->path = path_mask(shadow_path, out.contents_scale);
        }
        cf_release(shadow_color);
        cf_release(shadow_path);
        if (gravity) out.gravity = tree::gravity_named(cf.utf8(gravity)).value_or(tree::Gravity::resize);
        if (!out.hidden) {
            if (shape_layer_class && is_kind(object, shape_layer_class)) draw_shape(object, out, flipped);
            else if (text_layer_class && is_kind(object, text_layer_class)) draw_text(object, out);
            else if (gradient_layer_class && is_kind(object, gradient_layer_class)) draw_gradient(object, out, flipped);
        }
        out.sublayers.reserve(children.size());
        for (const auto child : children) out.sublayers.push_back(snapshot(child, depth + 1, flipped != out.geometry_flipped));
        if (replicator_layer_class && is_kind(object, replicator_layer_class)) replicate(object, out);
        return out;
    }
    Address root_of(Address object) {
        for (int depth = 0; object && depth < 4096; ++depth) {
            Address parent{};
            { const auto state = layer(object); std::lock_guard lock(state->mutex); parent = state->superlayer; }
            if (!parent) return object;
            object = parent;
        }
        return object;
    }

    void install() {
        const auto root = objc.find_class("NSObject");
        if (!root) fail("NSObject is missing");
        layer_class = objc.find_class("CALayer");
        metal_layer_class = objc.find_class("CAMetalLayer");
        if (!layer_class || !metal_layer_class) fail("the headless CALayer/CAMetalLayer classes are missing");
        drawable_class = declare("CAMetalDrawable", root, false);
        transaction_class = declare("CATransaction", root, true);
        install_layer();
        install_layer_appearance();
        install_gradient_layer();
        install_shape_and_text_layers();
        install_metal_layer();
        install_drawable();
        install_transaction();
        // CACurrentMediaTime: seconds on the mach_absolute_time clock.
        static_cast<void>(process.install_function("_CACurrentMediaTime",
            [owner = shared_from_this()](CpuState& s, GuestMemory&) { return_real(s, 0, monotonic_seconds()); },
            std::string(quartz_core)));
    }
};

std::mutex instance_mutex;
std::weak_ptr<QuartzCore> instance;   // the installed provider (for the C++ contract functions)

std::shared_ptr<QuartzCore> provider() {
    std::lock_guard lock(instance_mutex);
    auto value = instance.lock();
    if (!value) fail("QuartzCore is not installed");
    return value;
}
} // namespace

// ---- C++ contracts ---------------------------------------------------------------------
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
void register_metal_device(std::uint64_t mtl_device, std::shared_ptr<MetalVulkanDevice> device) {
    if (!mtl_device || !device) fail("register_metal_device needs a device and a bridge");
    std::lock_guard lock(registry_mutex);
    registry().insert_or_assign(mtl_device, std::move(device));
}
std::shared_ptr<MetalVulkanDevice> find_metal_device(std::uint64_t mtl_device) {
    std::lock_guard lock(registry_mutex);
    const auto found = registry().find(mtl_device);
    return found == registry().end() ? nullptr : found->second;
}
#endif
void unregister_metal_device(std::uint64_t mtl_device) {
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
    std::lock_guard lock(registry_mutex);
    registry().erase(mtl_device);
#else
    static_cast<void>(mtl_device);
#endif
}

void set_layer_host(ObjcRuntime& objc, std::uint64_t layer, std::optional<LayerHost> host) {
    const auto core = provider();
    const auto state = core->metal_layer(layer);
    static_cast<void>(objc);
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
    std::shared_ptr<Presenter> presenter;
#endif
    {
        std::lock_guard lock(state->mutex);
        const bool moved = !host || !state->metal.host || state->metal.host->window != host->window ||
            state->metal.host->native_window != host->native_window;
        state->metal.host = std::move(host);
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
        if (moved) presenter = state->metal.presenter;
#endif
    }
#ifdef ALCHEMY_HAVE_VULKAN_HEADERS
    if (presenter) presenter->detach();   // the next present attaches to the new window
#endif
}

std::optional<LayerHost> layer_host(ObjcRuntime&, std::uint64_t layer) {
    const auto state = provider()->metal_layer(layer);
    std::lock_guard lock(state->mutex);
    return state->metal.host;
}

bool is_metal_layer(ObjcRuntime&, std::uint64_t object) {
    const auto core = provider();
    return core->is_kind(object, core->metal_layer_class);
}

void observe_layer_contents(std::string_view domain, ContentsObserver observer, const std::source_location& where) {
    const auto core = provider();
    std::lock_guard lock(core->contents_mutex);
    for (const auto& entry : core->contents_observers)
        if (entry.first == domain) throw std::runtime_error("Alchemy QuartzCore: layer contents are already observed for " + std::string(domain));
    registration_audit::record_extension("-[CALayer setContents:]", domain, where);
    core->contents_observers.emplace_back(std::string(domain), std::move(observer));
}

void observe_layer_changes(std::string_view domain, ChangeObserver observer, const std::source_location& where) {
    const auto core = provider();
    std::lock_guard lock(core->contents_mutex);
    for (const auto& entry : core->change_observers)
        if (entry.first == domain) throw std::runtime_error("Alchemy QuartzCore: layer changes are already observed for " + std::string(domain));
    registration_audit::record_extension("CALayer changes", domain, where);
    core->change_observers.emplace_back(std::string(domain), std::move(observer));
    core->observed.store(true, std::memory_order_release);
}

void observe_transaction_commits(std::string_view domain, CommitObserver observer, const std::source_location& where) {
    const auto core = provider();
    std::lock_guard lock(core->contents_mutex);
    for (const auto& entry : core->commit_observers)
        if (entry.first == domain) throw std::runtime_error("Alchemy QuartzCore: commits are already observed for " + std::string(domain));
    registration_audit::record_extension("+[CATransaction commit]", domain, where);
    core->commit_observers.emplace_back(std::string(domain), std::move(observer));
    core->observed.store(true, std::memory_order_release);
}

bool in_explicit_transaction() noexcept { return transactions.explicit_depth > 0; }

std::uint64_t root_layer(ObjcRuntime&, std::uint64_t layer) { return provider()->root_of(layer); }

bool is_layer(ObjcRuntime&, std::uint64_t object) {
    const auto core = provider();
    return core->is_kind(object, core->layer_class);
}

tree::Layer snapshot_tree(ObjcRuntime&, std::uint64_t root) { return provider()->snapshot(root); }

void install_provider(InstallContext& context) {
    auto core = std::make_shared<QuartzCore>(context.process, context.objc, context.cf);
    core->install();
    std::lock_guard lock(instance_mutex);
    instance = core;
}

} // namespace quartzcore

} // namespace alchemy::gui
