#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/darwin/quartzcore/layer_tree.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// CARenderer: a layer tree rendered into an MTLTexture, frame by frame
// (+rendererWithMTLTexture:options:, -setDestination:). A frame is
// -beginFrameAtTime:timeStamp:, -updateBounds, -addUpdateRect:, -render,
// -endFrame. -render composites the layer and its sublayers as the window
// composition does (layer_tree.hpp; contents resolved by layer_contents.cpp:
// 'BGRA' IOSurfaces and CGImages) into the renderer's bounds, which fill the
// texture (the layer is placed in the bounds' space by its frame; bottom-left
// origin, the texture's first row is the bounds' top). The rendered area is
// cleared first, so pixels no layer covers are transparent.
// * The tree is the one QuartzCore shows now: model values with running
//   animations at the current media time (the frame time is kept for
//   -nextFrameTime, not used to sample the animations).
// * Change tracking is not kept: -updateBounds answers the whole bounds and
//   -render redraws all of it (-addUpdateRect: adds nothing to that).
// * -nextFrameTime is the frame time plus one 60 Hz frame while a layer of
//   the tree has animations, otherwise infinity.
// * Textures: BGRA8Unorm(_sRGB) and RGBA8Unorm(_sRGB) (others stop
//   explicitly); shared and managed storage are written with
//   -replaceRegion:..., private storage through a blit on the
//   kCARendererMetalCommandQueue option's queue (else one of the device's),
//   waited for before -render returns.
// * +rendererWithCGLContext:options: stops explicitly: no OpenGL context
//   exists (darwin/opengl/no_context.cpp).
namespace alchemy::gui::support_kit {
namespace {

constexpr std::string_view quartzcore_library = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";
namespace tree = quartzcore::tree;

constexpr std::uint64_t format_rgba8 = 70, format_rgba8_srgb = 71, format_bgra8 = 80, format_bgra8_srgb = 81;
constexpr std::uint64_t storage_private = 2;
constexpr std::uint64_t storage_shared_options = 0;   // MTLResourceStorageModeShared << 4

struct RendererState final : ObjectPayload {
    std::mutex mutex;
    Address layer{}, texture{}, options{}, queue{};   // +1 each
    Rect bounds;
    double frame_time{};
};

Rect rect_from(const CpuState& c) { return {{real(c, 0), real(c, 1)}, {real(c, 2), real(c, 3)}}; }
void return_rect(CpuState& c, const Rect& r) {
    return_real(c, 0, r.origin.x);
    return_real(c, 1, r.origin.y);
    return_real(c, 2, r.size.width);
    return_real(c, 3, r.size.height);
}

// Whether a layer of the tree at `layer` has animations.
bool animated(Support& s, Address layer, unsigned depth = 0) {
    if (!layer || depth > 256) return false;
    if (const auto keys = s.send_id(layer, "animationKeys"); keys && s.send(keys, "count").x0) return true;
    const auto sublayers = s.send_id(layer, "sublayers");
    for (Address i = 0, n = sublayers ? s.send(sublayers, "count").x0 : 0; i < n; ++i)
        if (animated(s, s.send_id(sublayers, "objectAtIndex:", {i}), depth + 1)) return true;
    return false;
}

void render(Support& s, RendererState& state) {
    Address layer{}, texture{}, options{};
    Rect bounds;
    {
        std::lock_guard lock(state.mutex);
        layer = s.retain(state.layer);
        texture = s.retain(state.texture);
        options = state.options;
        bounds = state.bounds;
    }
    struct Release { Support& s; Address a, b; ~Release() { s.release(a); s.release(b); } } held{s, layer, texture};
    if (!layer || !texture || !(bounds.size.width > 0) || !(bounds.size.height > 0)) return;
    const auto width = s.send(texture, "width").x0, height = s.send(texture, "height").x0;
    const auto format = s.send(texture, "pixelFormat").x0;
    const bool rgba = format == format_rgba8 || format == format_rgba8_srgb;
    if (!rgba && format != format_bgra8 && format != format_bgra8_srgb)
        unsupported("CARenderer into an MTLTexture of pixel format " + std::to_string(format) + " (BGRA8Unorm and RGBA8Unorm only)");
    if (width == 0 || height == 0 || width > 16384 || height > 16384) return;

    // The renderer's bounds hold the layer (placed by its frame) and fill the texture.
    auto root = quartzcore::snapshot_tree(s.objc, layer);
    const auto keep = resolve_layer_contents(s, root);
    tree::Layer space;
    space.bounds = bounds;
    space.position = {bounds.origin.x + bounds.size.width / 2, bounds.origin.y + bounds.size.height / 2};
    space.sublayers.push_back(std::move(root));
    const double scale = static_cast<double>(width) / bounds.size.width;
    const double points_high = static_cast<double>(height) / scale;
    const tree::Placement placement{{{0, 0}, {bounds.size.width, points_high}}, points_high, scale};
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    tree::Target target{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), static_cast<std::size_t>(width) * 4, pixels.data()};
    static_cast<void>(tree::compose(space, placement, target, {}));
    if (rgba)
        for (std::size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);

    // Into the texture: directly where the CPU can write it, through a blit for private storage.
    const auto row = width * 4;
    const auto bytes = s.process.allocate_guest(row * height, 64);
    const auto region = s.process.allocate_guest(48, 8);
    struct Free { Support& s; Address a, b; ~Free() { s.process.release_guest(a); s.process.release_guest(b); } } owned{s, bytes, region};
    s.memory().write_bytes(bytes, pixels);
    const std::array<std::uint64_t, 6> values{0, 0, 0, width, height, 1};   // MTLRegion: origin, size
    for (unsigned i = 0; i < 6; ++i) s.memory().write64(region + i * 8, values[i]);
    if (s.send(texture, "storageMode").x0 != storage_private) {
        CallRegs replace;
        replace.x[2] = region;
        replace.x[3] = 0;
        replace.x[4] = bytes;
        replace.x[5] = row;
        static_cast<void>(s.send(texture, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", replace));
        return;
    }
    Address queue{};
    if (options) {
        const auto key = s.cf.string("kCARendererMetalCommandQueue");
        queue = s.cf.dictionary_value(options, key);
        s.release(key);
    }
    if (!queue) {
        std::lock_guard lock(state.mutex);
        if (!state.queue) state.queue = s.send_id(s.send_id(texture, "device"), "newCommandQueue");
        queue = state.queue;
    }
    CallRegs buffer;
    buffer.x[2] = bytes;
    buffer.x[3] = row * height;
    buffer.x[4] = storage_shared_options;
    const auto staging = s.send(s.send_id(texture, "device"), "newBufferWithBytes:length:options:", buffer).x0;
    struct ReleaseOne { Support& s; Address a; ~ReleaseOne() { s.release(a); } } staged{s, staging};
    const auto commands = s.send_id(queue, "commandBuffer");
    const auto blit = s.send_id(commands, "blitCommandEncoder");
    CallRegs copy;
    copy.x[2] = staging;
    copy.x[3] = 0;
    copy.x[4] = row;
    copy.x[5] = row * height;
    copy.x[6] = region + 24;   // &size
    copy.x[7] = texture;
    copy.push_stack<std::uint64_t>(0);        // destinationSlice
    copy.push_stack<std::uint64_t>(0);        // destinationLevel
    copy.push_stack<std::uint64_t>(region);   // &destinationOrigin
    static_cast<void>(s.send(blit, "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:", copy));
    static_cast<void>(s.send_id(blit, "endEncoding"));
    static_cast<void>(s.send_id(commands, "commit"));
    static_cast<void>(s.send_id(commands, "waitUntilCompleted"));
}

} // namespace

void install_quartzcore_renderer(Support& support) {
    auto* const sp = &support;
    const auto cls = support.declare_class("CARenderer", "NSObject", quartzcore_library);
    const auto state_of = [sp](Address self) { return sp->objc.host_state<RendererState>(self); };
    for (const auto* name : {"kCARendererColorSpace", "kCARendererMetalCommandQueue"})
        if (!support.process.resolve(quartzcore_library, std::string("_") + name)) support.string_constant(quartzcore_library, std::string("_") + name, name);

    support.method(cls, "rendererWithMTLTexture:options:", [sp, cls, state_of](CpuState& c, GuestMemory&) {
        const auto texture = c.x[2], options = c.x[3];
        const auto made = sp->send_id(sp->objc.allocate(cls), "init");
        const auto state = state_of(made);
        state->texture = sp->retain(texture);
        state->options = sp->retain(options);
        // The bounds start as the texture's size (one point per pixel).
        if (texture) state->bounds = {{0, 0}, {static_cast<double>(sp->send(texture, "width").x0), static_cast<double>(sp->send(texture, "height").x0)}};
        c.x[0] = sp->send_id(made, "autorelease");
    }, "@@:@@", true);
    support.method(cls, "rendererWithCGLContext:options:", [](CpuState&, GuestMemory&) {
        unsupported("+[CARenderer rendererWithCGLContext:options:]: no OpenGL context exists");
    }, "@@:^v@", true);
    support.method(cls, "dealloc", [sp, cls](CpuState& c, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<RendererState>(c.x[0])) {
            for (auto* field : {&state->layer, &state->texture, &state->options, &state->queue}) sp->release(std::exchange(*field, 0));
        }
        static_cast<void>(sp->objc.send_super(c.x[0], cls, sp->objc.selector("dealloc")));
    }, "v@:");
    support.method(cls, "layer", [state_of](CpuState& c, GuestMemory&) {
        const auto state = state_of(c.x[0]);
        std::lock_guard lock(state->mutex);
        c.x[0] = state->layer;
    }, "@@:");
    support.method(cls, "setLayer:", [sp, state_of](CpuState& c, GuestMemory&) {
        const auto state = state_of(c.x[0]);
        const auto value = sp->retain(c.x[2]);
        Address old{};
        { std::lock_guard lock(state->mutex); old = std::exchange(state->layer, value); }
        sp->release(old);
    }, "v@:@");
    support.method(cls, "setDestination:", [sp, state_of](CpuState& c, GuestMemory&) {
        const auto state = state_of(c.x[0]);
        const auto value = sp->retain(c.x[2]);
        Address old{};
        { std::lock_guard lock(state->mutex); old = std::exchange(state->texture, value); }
        sp->release(old);
    }, "v@:@");
    support.method(cls, "bounds", [state_of](CpuState& c, GuestMemory&) {
        const auto state = state_of(c.x[0]);
        std::lock_guard lock(state->mutex);
        return_rect(c, state->bounds);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    support.method(cls, "setBounds:", [state_of](CpuState& c, GuestMemory&) {
        const auto state = state_of(c.x[0]);
        std::lock_guard lock(state->mutex);
        state->bounds = rect_from(c);
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    support.method(cls, "beginFrameAtTime:timeStamp:", [state_of](CpuState& c, GuestMemory&) {
        const auto state = state_of(c.x[0]);
        std::lock_guard lock(state->mutex);
        state->frame_time = real(c, 0);
    }, "v@:d^v");
    // No change tracking: the whole bounds need updating every frame.
    support.method(cls, "updateBounds", [state_of](CpuState& c, GuestMemory&) {
        const auto state = state_of(c.x[0]);
        std::lock_guard lock(state->mutex);
        return_rect(c, state->bounds);
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    support.method(cls, "addUpdateRect:", [](CpuState&, GuestMemory&) {}, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    support.method(cls, "render", [sp, state_of](CpuState& c, GuestMemory&) { render(*sp, *state_of(c.x[0])); }, "v@:");
    support.method(cls, "nextFrameTime", [sp, state_of](CpuState& c, GuestMemory&) {
        const auto state = state_of(c.x[0]);
        Address layer{};
        double frame_time{};
        { std::lock_guard lock(state->mutex); layer = sp->retain(state->layer); frame_time = state->frame_time; }
        const bool running = animated(*sp, layer);
        sp->release(layer);
        return_real(c, 0, running ? frame_time + 1.0 / 60 : std::numeric_limits<double>::infinity());
    }, "d@:");
    support.method(cls, "endFrame", [](CpuState&, GuestMemory&) {}, "v@:");
}

} // namespace alchemy::gui::support_kit
