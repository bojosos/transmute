// CARenderer (darwin/quartzcore/renderer.cpp): a layer tree rendered into an
// MTLTexture. A red root fills the texture, a blue sublayer rotated by 45
// degrees and a green band at the top are checked by pixel (bottom-left
// origin in layer space, top row first in the texture) in BGRA, RGBA and
// private-storage textures; the frame calls, -nextFrameTime and the CGL
// stop. The texture checks are skipped without a Metal device (no Vulkan 1.3
// GPU). Every provider is installed (run_program's order).
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace fill_test;

constexpr std::string_view metal = "/System/Library/Frameworks/Metal.framework/Versions/A/Metal";
constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";

Address color(Fixture& f, double r, double g, double b) {
    alchemy::CallRegs rgba;
    rgba.set_d(0, r); rgba.set_d(1, g); rgba.set_d(2, b); rgba.set_d(3, 1);
    return f.regs(cg, "_CGColorCreateGenericRGB", rgba).x0;
}
Address layer(Fixture& f, double x, double y, double width, double height, Address background) {
    const auto made = f.send(f.send(f.cls("CALayer"), "alloc"), "init");
    alchemy::CallRegs frame;
    frame.set_d(0, x); frame.set_d(1, y); frame.set_d(2, width); frame.set_d(3, height);
    static_cast<void>(f.objc.send(made, "setFrame:", frame));
    static_cast<void>(f.send(made, "setBackgroundColor:", {background}));
    return made;
}
Address texture(Fixture& f, Address device, std::uint64_t format, std::uint64_t storage) {
    alchemy::CallRegs make;
    make.x[2] = format; make.x[3] = 64; make.x[4] = 32; make.x[5] = 0;
    const auto descriptor = f.objc.send(f.cls("MTLTextureDescriptor"), "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", make).x0;
    static_cast<void>(f.send(descriptor, "setStorageMode:", {storage}));
    static_cast<void>(f.send(descriptor, "setUsage:", {0x1 | 0x2 | 0x4}));   // shader read / write, render target
    return f.send(device, "newTextureWithDescriptor:", {descriptor});
}
// The texture's pixels (64 x 32, 4 bytes each, top row first).
std::vector<std::uint8_t> pixels(Fixture& f, Address device, Address target, bool private_storage) {
    const auto region = f.block(48);
    const std::array<std::uint64_t, 6> values{0, 0, 0, 64, 32, 1};
    for (unsigned i = 0; i < 6; ++i) f.engine.write64(region + i * 8, values[i]);
    std::vector<std::uint8_t> out(64 * 32 * 4);
    if (!private_storage) {
        const auto bytes = f.block(out.size());
        static_cast<void>(f.send(target, "getBytes:bytesPerRow:fromRegion:mipmapLevel:", {bytes, 64 * 4, region, 0}));
        f.engine.read_bytes(bytes, out);
        return out;
    }
    const auto buffer = f.send(device, "newBufferWithLength:options:", {out.size(), 0});
    const auto queue = f.send(device, "newCommandQueue");
    const auto commands = f.send(queue, "commandBuffer");
    const auto blit = f.send(commands, "blitCommandEncoder");
    alchemy::CallRegs copy;
    copy.x[2] = target; copy.x[3] = 0; copy.x[4] = 0; copy.x[5] = region; copy.x[6] = region + 24; copy.x[7] = buffer;
    copy.push_stack<std::uint64_t>(0);
    copy.push_stack<std::uint64_t>(64 * 4);
    copy.push_stack<std::uint64_t>(out.size());
    static_cast<void>(f.objc.send(blit, "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:", copy));
    static_cast<void>(f.send(blit, "endEncoding"));
    static_cast<void>(f.send(commands, "commit"));
    static_cast<void>(f.send(commands, "waitUntilCompleted"));
    f.engine.read_bytes(f.send(buffer, "contents"), out);
    return out;
}

void renderer(Fixture& f) {
    // No OpenGL context exists.
    bool stopped = false;
    try { static_cast<void>(f.send(f.cls("CARenderer"), "rendererWithCGLContext:options:", {0, 0})); }
    catch (const std::exception& error) { stopped = std::string(error.what()).find("no OpenGL context") != std::string::npos; }
    require(stopped, "+rendererWithCGLContext:options: stops explicitly");

    const auto device = f.call(metal, "_MTLCreateSystemDefaultDevice");
    if (!device) { std::cout << "  no Metal device (no Vulkan 1.3 GPU): texture checks skipped\n"; return; }
    // Red root over the whole texture; a blue 16-point square at (8, 8) rotated by 45 degrees; a green band at the top.
    const auto root = layer(f, 0, 0, 64, 32, color(f, 1, 0, 0));
    const auto square = layer(f, 8, 8, 16, 16, color(f, 0, 0, 1));
    // (A CGAffineTransform is passed indirectly: 48 bytes.)
    const double angle = std::acos(-1.0) / 4;
    const std::array<double, 6> rotation{std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0, 0};
    const auto affine = f.block(48);
    for (unsigned i = 0; i < 6; ++i) f.engine.write64(affine + i * 8, std::bit_cast<std::uint64_t>(rotation[i]));
    static_cast<void>(f.send(square, "setAffineTransform:", {affine}));
    const auto band = layer(f, 40, 24, 16, 8, color(f, 0, 1, 0));
    static_cast<void>(f.send(root, "addSublayer:", {square}));
    static_cast<void>(f.send(root, "addSublayer:", {band}));

    for (const auto& [format, storage, label] : {std::tuple{std::uint64_t{80}, std::uint64_t{0}, "BGRA8 shared"},
                                                 std::tuple{std::uint64_t{70}, std::uint64_t{0}, "RGBA8 shared"},
                                                 std::tuple{std::uint64_t{80}, std::uint64_t{2}, "BGRA8 private"}}) {
        const auto target = texture(f, device, format, storage);
        require(target != 0, std::string("texture ") + label);
        const auto renderer = f.send(f.cls("CARenderer"), "rendererWithMTLTexture:options:", {target, 0});
        require(renderer != 0, "+rendererWithMTLTexture:options:");
        const auto bounds = f.objc.send(renderer, "bounds");
        require(bounds.d(2) == 64 && bounds.d(3) == 32, "the bounds start as the texture's size");
        static_cast<void>(f.send(renderer, "setLayer:", {root}));
        require(f.send(renderer, "layer") == root, "-layer");
        alchemy::CallRegs begin;
        begin.set_d(0, 10.0);
        static_cast<void>(f.objc.send(renderer, "beginFrameAtTime:timeStamp:", begin));
        const auto update = f.objc.send(renderer, "updateBounds");
        require(update.d(2) == 64 && update.d(3) == 32, "-updateBounds: the whole bounds");
        static_cast<void>(f.send(renderer, "render"));
        static_cast<void>(f.send(renderer, "endFrame"));
        require(std::isinf(f.objc.send(renderer, "nextFrameTime").d(0)), "no animations: no next frame");

        const auto data = pixels(f, device, target, storage == 2);
        const bool rgba = format == 70;
        // (red, green, blue) at texture pixel (x, y), y from the top.
        const auto at = [&](int x, int y) {
            const auto* p = &data[(static_cast<std::size_t>(y) * 64 + x) * 4];
            return rgba ? std::array<int, 3>{p[0], p[1], p[2]} : std::array<int, 3>{p[2], p[1], p[0]};
        };
        const auto is = [&](int x, int y, int channel) {
            const auto c = at(x, y);
            for (int i = 0; i < 3; ++i)
                if ((i == channel) != (c[i] > 160)) return false;
            return true;
        };
        const auto describe = [&](int x, int y) {
            const auto c = at(x, y);
            return std::string(label) + " (" + std::to_string(x) + "," + std::to_string(y) + ") = " + std::to_string(c[0]) + "," +
                   std::to_string(c[1]) + "," + std::to_string(c[2]);
        };
        require(is(60, 16, 0) && is(2, 30, 0), "the root's red: " + describe(60, 16));
        // The square's centre is (16, 16) in layer space: texture row 15.
        require(is(16, 15, 2), "the rotated square's centre: " + describe(16, 15));
        // The unrotated square's corner (9, 9) lies outside the rotated diamond.
        require(is(9, 22, 0), "rotation leaves the old corner red: " + describe(9, 22));
        // The diamond reaches further along the axes than the unrotated square (half-width 8, half-diagonal 11.3).
        require(is(16, 5, 2), "the diamond's tip: " + describe(16, 5));
        // The band (y 24..32 in layer space) is the texture's top rows.
        require(is(48, 2, 1) && is(48, 29, 0), "the green band at the top: " + describe(48, 2));
    }
}

} // namespace

int main() {
    return run("quartzcore_renderer", {{"renderer", renderer}});
}
