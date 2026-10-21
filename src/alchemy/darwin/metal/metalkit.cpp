#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// MetalKit: the framework identity (QLab links it) and MTKTextureLoader
// over Alchemy's Metal provider. An image (a CGImage, or the image an
// ImageIO source decodes from a URL or NSData) is drawn into an 8-bit RGBA
// bitmap context in sRGB (premultiplied alpha, the first row at the top,
// as MetalKit loads CGImages) and uploaded into a new 2D texture:
//   - pixel format RGBA8Unorm_sRGB, or RGBA8Unorm when
//     MTKTextureLoaderOptionSRGB is NO;
//   - MTKTextureLoaderOptionAllocateMipmaps / GenerateMipmaps allocate the
//     full mip chain; GenerateMipmaps also fills it with a blit encoder;
//   - MTKTextureLoaderOptionTextureUsage (default ShaderRead),
//     TextureStorageMode (default Private; private textures are filled
//     through a blit from a shared buffer) and TextureCPUCacheMode.
// Failures answer nil with an NSError (MTKTextureLoaderErrorDomain), as
// MetalKit does. Asynchronous loading, cube maps, KTX/PVR containers and
// asset catalogs are not provided (their selectors do not exist).
namespace alchemy::gui::completion_kit::graphics {
namespace {

constexpr std::string_view metalkit_library = "/System/Library/Frameworks/MetalKit.framework/Versions/A/MetalKit";
constexpr std::string_view coregraphics_library = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view imageio_library = "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO";

constexpr std::uint64_t pixel_format_rgba8_unorm = 70, pixel_format_rgba8_unorm_srgb = 71;
constexpr std::uint64_t storage_shared = 0, storage_private = 2;
constexpr std::uint64_t usage_shader_read = 1;
constexpr std::uint32_t alpha_premultiplied_last = 1;   // kCGImageAlphaPremultipliedLast (RGBA bytes)

struct LoaderState final : ObjectPayload { Address device{}; };   // +1

struct Loader {
    Support& support;
    Address cls{};

    Address key(std::string_view name) { return support.cf.constant_string(name); }
    // The option's value in the options dictionary (nil when absent).
    Address option(Address options, std::string_view name) {
        if (!options) return 0;
        return support.send_id(options, "objectForKey:", {key(name)});
    }
    std::optional<std::uint64_t> integer_option(Address options, std::string_view name) {
        const auto value = option(options, name);
        if (!value) return std::nullopt;
        return support.send(value, "unsignedIntegerValue").x0;
    }
    std::optional<bool> bool_option(Address options, std::string_view name) {
        const auto value = option(options, name);
        if (!value) return std::nullopt;
        return (support.send(value, "boolValue").x0 & 0xff) != 0;
    }
    Address error(std::int64_t code, std::string_view description) {
        const auto error_class = support.objc.find_class("NSError");
        if (!error_class) return 0;
        const std::vector<std::pair<Address, Address>> entries{
            {key("NSLocalizedDescription"), support.send_id(support.cf.string(description), "autorelease")}};
        CallRegs registers;
        registers.x[2] = key("MTKTextureLoaderErrorDomain");
        registers.x[3] = static_cast<std::uint64_t>(code);
        registers.x[4] = support.send_id(support.cf.dictionary(entries), "autorelease");
        return support.send(error_class, "errorWithDomain:code:userInfo:", registers).x0;
    }
    void report(Address out_error, std::string_view description) {
        if (out_error) support.memory().write64(out_error, error(0, description));
    }

    // +1 MTLTexture holding `image` (a CGImageRef), or 0 with *out_error set.
    Address texture_from_image(Address self, Address image, Address options, Address out_error) {
        const auto device = support.objc.host_state<LoaderState>(self)->device;
        if (!device) { report(out_error, "MTKTextureLoader has no device"); return 0; }
        const auto width = support.call(coregraphics_library, "_CGImageGetWidth", {image});
        const auto height = support.call(coregraphics_library, "_CGImageGetHeight", {image});
        if (!width || !height || width > 16384 || height > 16384) {
            report(out_error, "Image decoding failed: unsupported image dimensions");
            return 0;
        }
        const auto row = width * 4;
        const auto pixels = support.process.allocate_guest(row * height);
        try {
            // Decode: draw into an RGBA sRGB bitmap context.
            const auto space = support.call(coregraphics_library, "_CGColorSpaceCreateWithName", {key("kCGColorSpaceSRGB")});
            CallRegs create;
            create.x = {pixels, width, height, 8, row, space, alpha_premultiplied_last, 0};
            const auto context = support.call(coregraphics_library, "_CGBitmapContextCreate", create).x0;
            support.call(coregraphics_library, "_CGColorSpaceRelease", {space});
            if (!context) fail("MTKTextureLoader: CGBitmapContextCreate failed");
            CallRegs draw;
            draw.x[0] = context;
            draw.x[1] = image;
            draw.set_d(0, 0); draw.set_d(1, 0); draw.set_d(2, static_cast<double>(width)); draw.set_d(3, static_cast<double>(height));
            support.call(coregraphics_library, "_CGContextDrawImage", draw);
            support.call(coregraphics_library, "_CGContextRelease", {context});

            const bool srgb = bool_option(options, "MTKTextureLoaderOptionSRGB").value_or(true);
            const bool generate = bool_option(options, "MTKTextureLoaderOptionGenerateMipmaps").value_or(false);
            const bool mipmapped = generate || bool_option(options, "MTKTextureLoaderOptionAllocateMipmaps").value_or(false);
            const auto usage = integer_option(options, "MTKTextureLoaderOptionTextureUsage").value_or(usage_shader_read);
            const auto storage = integer_option(options, "MTKTextureLoaderOptionTextureStorageMode").value_or(storage_private);
            const auto cache = integer_option(options, "MTKTextureLoaderOptionTextureCPUCacheMode").value_or(0);

            const auto descriptor_class = support.objc.find_class("MTLTextureDescriptor");
            if (!descriptor_class) unsupported("MTKTextureLoader without the Metal provider");
            CallRegs make;
            make.x[2] = srgb ? pixel_format_rgba8_unorm_srgb : pixel_format_rgba8_unorm;
            make.x[3] = width;
            make.x[4] = height;
            make.x[5] = mipmapped ? 1 : 0;
            const auto descriptor = support.send(descriptor_class, "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", make).x0;
            support.send_id(descriptor, "setUsage:", {usage});
            support.send_id(descriptor, "setStorageMode:", {storage});
            support.send_id(descriptor, "setCpuCacheMode:", {cache});
            const auto texture = support.send_id(device, "newTextureWithDescriptor:", {descriptor});
            if (!texture) { support.process.release_guest(pixels); report(out_error, "Texture creation failed"); return 0; }

            // Level 0: directly for CPU-visible storage, through a blit for private textures.
            const auto region = support.process.allocate_guest(48);
            const std::array<std::uint64_t, 6> values{0, 0, 0, width, height, 1};
            for (unsigned i = 0; i < 6; ++i) support.memory().write64(region + i * 8, values[i]);
            Address queue{}, command_buffer{}, staging{};
            const auto commands = [&]() {
                if (!command_buffer) {
                    queue = support.send_id(device, "newCommandQueue");
                    command_buffer = support.send_id(queue, "commandBuffer");
                }
                return support.send_id(command_buffer, "blitCommandEncoder");
            };
            if (storage != storage_private) {
                CallRegs replace;
                replace.x[2] = region;
                replace.x[3] = 0;
                replace.x[4] = pixels;
                replace.x[5] = row;
                support.send(texture, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", replace);
            } else {
                CallRegs buffer;
                buffer.x[2] = pixels;
                buffer.x[3] = row * height;
                buffer.x[4] = storage_shared << 4;   // MTLResourceStorageModeShared
                staging = support.send(device, "newBufferWithBytes:length:options:", buffer).x0;
                const auto blit = commands();
                CallRegs copy;
                copy.x[2] = staging;
                copy.x[3] = 0;
                copy.x[4] = row;
                copy.x[5] = row * height;
                copy.x[6] = region + 24;   // &size
                copy.x[7] = texture;
                copy.push_stack<std::uint64_t>(0);       // destinationSlice
                copy.push_stack<std::uint64_t>(0);       // destinationLevel
                copy.push_stack<std::uint64_t>(region);  // &destinationOrigin
                support.send(blit, "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:", copy);
                support.send_id(blit, "endEncoding");
            }
            if (generate) {
                const auto blit = commands();
                support.send_id(blit, "generateMipmapsForTexture:", {texture});
                support.send_id(blit, "endEncoding");
            }
            if (command_buffer) {
                support.send_id(command_buffer, "commit");
                support.send_id(command_buffer, "waitUntilCompleted");
                support.release(queue);
            }
            support.release(staging);
            support.process.release_guest(region);
            support.process.release_guest(pixels);
            return texture;
        } catch (...) {
            support.process.release_guest(pixels);
            throw;
        }
    }

    // +1 texture from an ImageIO source (+1, released here).
    Address texture_from_source(Address self, Address source, Address options, Address out_error) {
        if (!source) { report(out_error, "Image decoding failed: the data is not an image"); return 0; }
        const auto image = support.call(imageio_library, "_CGImageSourceCreateImageAtIndex", {source, 0, 0});
        const std::array<Address, 1> release_arguments{source};
        static_cast<void>(support.cf.invoke("CFRelease", release_arguments));
        if (!image) { report(out_error, "Image decoding failed: the data is not an image"); return 0; }
        try {
            const auto texture = texture_from_image(self, image, options, out_error);
            support.call(coregraphics_library, "_CGImageRelease", {image});
            return texture;
        } catch (...) {
            support.call(coregraphics_library, "_CGImageRelease", {image});
            throw;
        }
    }
};

} // namespace

void install_metalkit(Support& support) {
    ProcessContext::add_provider_library(std::string(metalkit_library));
    for (const auto* name : {"MTKTextureLoaderOptionAllocateMipmaps", "MTKTextureLoaderOptionGenerateMipmaps",
             "MTKTextureLoaderOptionSRGB", "MTKTextureLoaderOptionTextureUsage", "MTKTextureLoaderOptionTextureCPUCacheMode",
             "MTKTextureLoaderOptionTextureStorageMode", "MTKTextureLoaderOptionCubeLayout", "MTKTextureLoaderOptionOrigin",
             "MTKTextureLoaderOptionLoadAsArray", "MTKTextureLoaderCubeLayoutVertical", "MTKTextureLoaderOriginTopLeft",
             "MTKTextureLoaderOriginBottomLeft", "MTKTextureLoaderOriginFlippedVertically", "MTKTextureLoaderErrorDomain",
             "MTKTextureLoaderErrorKey", "MTKModelErrorDomain", "MTKModelErrorKey"}) {
        const auto symbol = "_" + std::string(name);
        if (!support.process.resolve(metalkit_library, symbol)) support.string_constant(metalkit_library, symbol, name);
    }
    auto loader = std::make_shared<Loader>(Loader{support});
    const auto cls = support.declare_class("MTKTextureLoader", "NSObject", metalkit_library);
    loader->cls = cls;
    auto* sp = &support;
    support.method(cls, "initWithDevice:", [sp](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[MTKTextureLoader initWithDevice:] with a nil device");
        sp->objc.host_state<LoaderState>(s.x[0])->device = sp->retain(s.x[2]);
    }, "@@:@");
    support.method(cls, "init", [](CpuState&, GuestMemory&) {
        unsupported("-[MTKTextureLoader init] (use -initWithDevice:)");
    }, "@@:");
    support.method(cls, "device", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->objc.host_state<LoaderState>(s.x[0])->device; }, "@@:");
    support.method(cls, "dealloc", [sp, cls](CpuState& s, GuestMemory&) {
        if (const auto state = sp->objc.find_host_state<LoaderState>(s.x[0])) sp->release(std::exchange(state->device, 0));
        static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("dealloc")));
    }, "v@:");
    support.method(cls, "newTextureWithCGImage:options:error:", [loader](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[MTKTextureLoader newTextureWithCGImage:options:error:] with a NULL image");
        s.x[0] = loader->texture_from_image(s.x[0], s.x[2], s.x[3], s.x[4]);
    }, "@@:^{CGImage=}@^@");
    support.method(cls, "newTextureWithData:options:error:", [loader](CpuState& s, GuestMemory&) {
        const auto source = s.x[2] ? loader->support.call(imageio_library, "_CGImageSourceCreateWithData", {s.x[2], 0}) : 0;
        s.x[0] = loader->texture_from_source(s.x[0], source, s.x[3], s.x[4]);
    }, "@@:@@^@");
    support.method(cls, "newTextureWithContentsOfURL:options:error:", [loader](CpuState& s, GuestMemory&) {
        const auto source = s.x[2] ? loader->support.call(imageio_library, "_CGImageSourceCreateWithURL", {s.x[2], 0}) : 0;
        s.x[0] = loader->texture_from_source(s.x[0], source, s.x[3], s.x[4]);
    }, "@@:@@^@");
}

} // namespace alchemy::gui::completion_kit::graphics
