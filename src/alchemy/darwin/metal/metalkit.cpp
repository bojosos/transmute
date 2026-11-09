#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

// MetalKit: the framework identity (QLab links it), MTKView (below) and MTKTextureLoader
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

// ---- MTKView ----------------------------------------------------------------------------
// An NSView (AppKit's, so applications subclass it) backed by a CAMetalLayer
// (QuartzCore's, through -makeBackingLayer). Each frame, -draw sets the
// current drawable and render pass descriptor (with the depth/stencil and
// multisample textures the view owns), calls -drawRect: (MTKView's own
// forwards to the delegate's -drawInMTKView:; a subclass's override draws
// itself) and forgets the drawable; the application presents it. Frames
// come from a run-loop timer at preferredFramesPerSecond while the view is
// in a window, not paused and not in setNeedsDisplay mode; in that mode
// AppKit's -drawRect: calls draw. The drawable follows the view's bounds x
// the layer's contentsScale when autoResizeDrawable, telling the delegate
// (-mtkView:drawableSizeWillChange:). NSView's host state belongs to AppKit,
// so the view's own state lives in a table by object.
constexpr std::uint64_t pixel_format_invalid = 0, pixel_format_bgra8_unorm = 80;
constexpr std::uint64_t pixel_format_stencil8 = 253, pixel_format_depth24_stencil8 = 255,
                        pixel_format_depth32_stencil8 = 260, pixel_format_x32_stencil8 = 261, pixel_format_x24_stencil8 = 262;
constexpr std::uint64_t usage_render_target = 4, texture_type_2d_multisample = 4;
constexpr std::uint64_t load_clear = 2, store_dont_care = 0, store_store = 1, store_multisample_resolve = 2;

struct ViewState {
    Address device{};                            // +1
    std::uint64_t color_format{pixel_format_bgra8_unorm}, depth_format{pixel_format_invalid};
    std::uint64_t sample_count{1};
    std::array<double, 4> clear_color{0, 0, 0, 1};
    double clear_depth{1};
    std::uint32_t clear_stencil{};
    bool framebuffer_only{true}, auto_resize{true}, presents_with_transaction{}, set_needs_display{}, paused{};
    std::int64_t frames_per_second{60};
    Address delegate{};                          // weak
    Address timer{};                             // +1 while running
    Address drawable{}, pass{};                  // +1 during a frame
    Address depth_texture{}, msaa_texture{};     // +1, sized to the drawable
    double width{}, height{};                    // the drawable size those textures have
    double reported_width{}, reported_height{};  // the drawable size last told to the delegate
    bool drawing{};
};

struct Views {
    Support& support;
    Address cls{};
    std::mutex mutex;
    std::map<Address, std::shared_ptr<ViewState>> states;

    std::shared_ptr<ViewState> state(Address view) {
        std::lock_guard lock(mutex);
        auto& value = states[view];
        if (!value) value = std::make_shared<ViewState>();
        return value;
    }
    Address layer(Address view) { return support.send(view, "layer").x0; }
    void send_u64(Address object, std::string_view selector, std::uint64_t value) {
        CallRegs registers;
        registers.x[2] = value;
        static_cast<void>(support.send(object, selector, registers));
    }
    // Applies the view's settings to its CAMetalLayer.
    void configure(Address view, const ViewState& s, Address target) {
        if (!target) return;
        send_u64(target, "setDevice:", s.device);
        send_u64(target, "setPixelFormat:", s.color_format);
        send_u64(target, "setFramebufferOnly:", s.framebuffer_only ? 1 : 0);
        send_u64(target, "setPresentsWithTransaction:", s.presents_with_transaction ? 1 : 0);
        static_cast<void>(view);
    }
    std::pair<double, double> drawable_size(Address view) {
        const auto target = layer(view);
        if (!target) return {0, 0};
        const auto size = support.send(target, "drawableSize");
        return {size.d(0), size.d(1)};
    }
    void set_drawable_size(Address view, double width, double height) {
        const auto target = layer(view);
        if (!target) return;
        CallRegs registers;
        registers.set_d(0, width);
        registers.set_d(1, height);
        static_cast<void>(support.send(target, "setDrawableSize:", registers));
    }
    // bounds x contentsScale.
    std::pair<double, double> preferred_size(Address view) {
        const auto bounds = support.send(view, "bounds");
        const auto target = layer(view);
        const double scale = target ? support.send(target, "contentsScale").d(0) : 1;
        return {bounds.d(2) * (scale > 0 ? scale : 1), bounds.d(3) * (scale > 0 ? scale : 1)};
    }
    // autoResizeDrawable: the drawable follows the bounds; the delegate hears of a change.
    void track_size(Address view, ViewState& s) {
        if (!s.auto_resize) return;
        const auto [width, height] = preferred_size(view);
        if (width < 1 || height < 1) return;
        // Against the size last reported: the layer may follow its bounds itself.
        if (width == s.reported_width && height == s.reported_height) return;
        s.reported_width = width;
        s.reported_height = height;
        const auto [current_width, current_height] = drawable_size(view);
        if (width != current_width || height != current_height) set_drawable_size(view, width, height);
        if (s.delegate) {
            CallRegs registers;
            registers.x[2] = view;
            registers.set_d(0, width);
            registers.set_d(1, height);
            static_cast<void>(support.objc.send_if_responds(s.delegate, support.objc.selector("mtkView:drawableSizeWillChange:"),
                                                            registers));
        }
    }
    // A private texture the view owns for its pass (depth/stencil or multisample colour).
    Address attachment_texture(const ViewState& s, std::uint64_t format, std::uint64_t samples) {
        const auto descriptor_class = support.objc.find_class("MTLTextureDescriptor");
        if (!descriptor_class || !s.device) return 0;
        CallRegs make;
        make.x[2] = format;
        make.x[3] = static_cast<std::uint64_t>(s.width);
        make.x[4] = static_cast<std::uint64_t>(s.height);
        make.x[5] = 0;
        const auto descriptor = support.send(descriptor_class, "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", make).x0;
        send_u64(descriptor, "setUsage:", usage_render_target);
        send_u64(descriptor, "setStorageMode:", storage_private);
        if (samples > 1) {
            send_u64(descriptor, "setTextureType:", texture_type_2d_multisample);
            send_u64(descriptor, "setSampleCount:", samples);
        }
        return support.send_id(s.device, "newTextureWithDescriptor:", {descriptor});
    }
    void release_textures(ViewState& s) {
        support.release(std::exchange(s.depth_texture, 0));
        support.release(std::exchange(s.msaa_texture, 0));
        s.width = s.height = 0;
    }
    static bool has_stencil(std::uint64_t format) {
        return format == pixel_format_stencil8 || format == pixel_format_depth24_stencil8 ||
               format == pixel_format_depth32_stencil8 || format == pixel_format_x32_stencil8 ||
               format == pixel_format_x24_stencil8;
    }
    // The drawable for this frame (nextDrawable once).
    Address current_drawable(Address view, ViewState& s) {
        if (!s.drawable) {
            const auto target = layer(view);
            if (target) s.drawable = support.retain(support.send(target, "nextDrawable").x0);
        }
        return s.drawable;
    }
    Address current_pass(Address view, ViewState& s) {
        if (s.pass) return s.pass;
        const auto drawable = current_drawable(view, s);
        if (!drawable) return 0;
        const auto texture = support.send_id(drawable, "texture");
        const auto width = static_cast<double>(support.send(texture, "width").x0);
        const auto height = static_cast<double>(support.send(texture, "height").x0);
        if (width != s.width || height != s.height) {
            release_textures(s);
            s.width = width;
            s.height = height;
        }
        if (s.depth_format != pixel_format_invalid && !s.depth_texture)
            s.depth_texture = attachment_texture(s, s.depth_format, s.sample_count);
        if (s.sample_count > 1 && !s.msaa_texture) s.msaa_texture = attachment_texture(s, s.color_format, s.sample_count);
        const auto pass_class = support.objc.find_class("MTLRenderPassDescriptor");
        if (!pass_class) return 0;
        s.pass = support.retain(support.send_id(pass_class, "renderPassDescriptor"));
        const auto colors = support.send_id(s.pass, "colorAttachments");
        CallRegs index;
        index.x[2] = 0;
        const auto color = support.send(colors, "objectAtIndexedSubscript:", index).x0;
        if (s.sample_count > 1 && s.msaa_texture) {
            send_u64(color, "setTexture:", s.msaa_texture);
            send_u64(color, "setResolveTexture:", texture);
            send_u64(color, "setStoreAction:", store_multisample_resolve);
        } else {
            send_u64(color, "setTexture:", texture);
            send_u64(color, "setStoreAction:", store_store);
        }
        send_u64(color, "setLoadAction:", load_clear);
        CallRegs clear;
        for (unsigned i = 0; i < 4; ++i) clear.set_d(i, s.clear_color[i]);
        static_cast<void>(support.send(color, "setClearColor:", clear));
        if (s.depth_texture) {
            const auto depth = support.send_id(s.pass, "depthAttachment");
            send_u64(depth, "setTexture:", s.depth_texture);
            send_u64(depth, "setLoadAction:", load_clear);
            send_u64(depth, "setStoreAction:", store_dont_care);
            CallRegs value;
            value.set_d(0, s.clear_depth);
            static_cast<void>(support.send(depth, "setClearDepth:", value));
            if (has_stencil(s.depth_format)) {
                const auto stencil = support.send_id(s.pass, "stencilAttachment");
                send_u64(stencil, "setTexture:", s.depth_texture);
                send_u64(stencil, "setLoadAction:", load_clear);
                send_u64(stencil, "setStoreAction:", store_dont_care);
                send_u64(stencil, "setClearStencil:", s.clear_stencil);
            }
        }
        return s.pass;
    }
    // One frame: -drawRect: with the frame's drawable available, then the
    // drawable and descriptor are forgotten (the application presented it).
    void draw(Address view) {
        const auto s = state(view);
        if (s->drawing) return;
        track_size(view, *s);
        s->drawing = true;
        try {
            const auto bounds = support.send(view, "bounds");
            CallRegs rect;
            for (unsigned i = 0; i < 4; ++i) rect.set_d(i, bounds.d(i));
            static_cast<void>(support.send(view, "drawRect:", rect));
        } catch (...) {
            s->drawing = false;
            end_frame(*s);
            throw;
        }
        s->drawing = false;
        end_frame(*s);
    }
    void end_frame(ViewState& s) {
        support.release(std::exchange(s.drawable, 0));
        support.release(std::exchange(s.pass, 0));
    }
    // The run-loop timer runs while the view is in a window, not paused and
    // not in setNeedsDisplay mode.
    void update_timer(Address view, ViewState& s) {
        const bool wanted = !s.paused && !s.set_needs_display && s.frames_per_second > 0 && support.send(view, "window").x0;
        if (wanted == (s.timer != 0)) return;
        if (!wanted) {
            const auto timer = std::exchange(s.timer, 0);
            static_cast<void>(support.send(timer, "invalidate"));
            support.release(timer);
            return;
        }
        const auto timer_class = support.objc.find_class("NSTimer");
        if (!timer_class) unsupported("MTKView without NSTimer");
        CallRegs registers;
        registers.set_d(0, 1.0 / static_cast<double>(s.frames_per_second));
        registers.x[2] = view;
        registers.x[3] = support.objc.selector("_alchemyFrame:");
        registers.x[4] = 0;
        registers.x[5] = 1;
        s.timer = support.retain(support.send(timer_class, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:",
                                              registers).x0);
    }
    void set_device(Address view, ViewState& s, Address device) {
        if (s.device == device) return;
        release_textures(s);
        support.release(std::exchange(s.device, support.retain(device)));
        if (const auto target = layer(view)) send_u64(target, "setDevice:", device);
    }
    void destroy(Address view) {
        std::shared_ptr<ViewState> s;
        {
            std::lock_guard lock(mutex);
            const auto found = states.find(view);
            if (found == states.end()) return;
            s = found->second;
            states.erase(found);
        }
        end_frame(*s);
        release_textures(*s);
        if (const auto timer = std::exchange(s->timer, 0)) {
            static_cast<void>(support.send(timer, "invalidate"));
            support.release(timer);
        }
        support.release(std::exchange(s->device, 0));
    }
};

void install_mtkview(Support& support) {
    auto views = std::shared_ptr<Views>(new Views{support});
    const auto cls = support.declare_class("MTKView", "NSView", metalkit_library);
    views->cls = cls;
    auto* sp = &support;
    const auto init = [views, cls, sp](Address self, Address device) {
        const auto s = views->state(self);
        views->set_device(self, *s, device);
        CallRegs yes;
        yes.x[2] = 1;
        static_cast<void>(sp->send(self, "setWantsLayer:", yes));
        views->configure(self, *s, views->layer(self));
        static_cast<void>(cls);
    };
    support.method(cls, "initWithFrame:device:", [sp, cls, init](CpuState& s, GuestMemory&) {
        CallRegs frame;
        for (unsigned i = 0; i < 4; ++i) frame.v[i] = s.v[i];
        const auto device = s.x[2];
        const auto self = sp->objc.send_super(s.x[0], cls, sp->objc.selector("initWithFrame:"), frame).x0;
        if (self) init(self, device);
        s.x[0] = self;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@");
    support.method(cls, "initWithFrame:", [sp, cls, init](CpuState& s, GuestMemory&) {
        CallRegs frame;
        for (unsigned i = 0; i < 4; ++i) frame.v[i] = s.v[i];
        const auto self = sp->objc.send_super(s.x[0], cls, sp->objc.selector("initWithFrame:"), frame).x0;
        if (self) init(self, 0);
        s.x[0] = self;
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    support.method(cls, "initWithCoder:", [sp, cls, init](CpuState& s, GuestMemory&) {
        CallRegs coder;
        coder.x[2] = s.x[2];
        const auto self = sp->objc.send_super(s.x[0], cls, sp->objc.selector("initWithCoder:"), coder).x0;
        if (self) init(self, 0);
        s.x[0] = self;
    }, "@@:@");
    support.method(cls, "dealloc", [views, sp, cls](CpuState& s, GuestMemory&) {
        views->destroy(s.x[0]);
        static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("dealloc")));
    }, "v@:");
    support.method(cls, "makeBackingLayer", [views, sp](CpuState& s, GuestMemory&) {
        const auto layer_class = sp->objc.find_class("CAMetalLayer");
        if (!layer_class) unsupported("MTKView without QuartzCore's CAMetalLayer");
        const auto layer = sp->send(layer_class, "layer").x0;
        views->configure(s.x[0], *views->state(s.x[0]), layer);
        s.x[0] = layer;
    }, "@@:");

    // Properties.
    const auto getter = [&support, views](std::string name, auto read, std::string types) {
        support.method(views->cls, std::move(name), [views, read](CpuState& s, GuestMemory&) {
            read(*views->state(s.x[0]), s);
        }, std::move(types));
    };
    const auto setter = [&support, views](std::string name, auto write, std::string types) {
        support.method(views->cls, std::move(name), [views, write](CpuState& s, GuestMemory&) {
            const auto view = s.x[0];
            const auto state = views->state(view);
            write(view, *state, s);
            views->configure(view, *state, views->layer(view));
        }, std::move(types));
    };
    getter("device", [](ViewState& v, CpuState& s) { s.x[0] = v.device; }, "@@:");
    setter("setDevice:", [views](Address view, ViewState& v, CpuState& s) { views->set_device(view, v, s.x[2]); }, "v@:@");
    getter("preferredDevice", [](ViewState& v, CpuState& s) { s.x[0] = v.device; }, "@@:");
    getter("colorPixelFormat", [](ViewState& v, CpuState& s) { s.x[0] = v.color_format; }, "Q@:");
    setter("setColorPixelFormat:", [views](Address, ViewState& v, CpuState& s) {
        if (v.color_format != s.x[2]) views->release_textures(v);
        v.color_format = s.x[2];
    }, "v@:Q");
    getter("depthStencilPixelFormat", [](ViewState& v, CpuState& s) { s.x[0] = v.depth_format; }, "Q@:");
    setter("setDepthStencilPixelFormat:", [views](Address, ViewState& v, CpuState& s) {
        if (v.depth_format != s.x[2]) views->release_textures(v);
        v.depth_format = s.x[2];
    }, "v@:Q");
    getter("sampleCount", [](ViewState& v, CpuState& s) { s.x[0] = v.sample_count; }, "Q@:");
    setter("setSampleCount:", [views](Address, ViewState& v, CpuState& s) {
        const auto count = std::max<std::uint64_t>(1, s.x[2]);
        if (v.sample_count != count) views->release_textures(v);
        v.sample_count = count;
    }, "v@:Q");
    getter("clearColor", [](ViewState& v, CpuState& s) {
        for (unsigned i = 0; i < 4; ++i) s.v[i] = {std::bit_cast<std::uint64_t>(v.clear_color[i]), 0};
    }, "{MTLClearColor=dddd}@:");
    setter("setClearColor:", [](Address, ViewState& v, CpuState& s) {
        for (unsigned i = 0; i < 4; ++i) v.clear_color[i] = std::bit_cast<double>(s.v[i][0]);
    }, "v@:{MTLClearColor=dddd}");
    getter("clearDepth", [](ViewState& v, CpuState& s) { s.v[0] = {std::bit_cast<std::uint64_t>(v.clear_depth), 0}; }, "d@:");
    setter("setClearDepth:", [](Address, ViewState& v, CpuState& s) { v.clear_depth = std::bit_cast<double>(s.v[0][0]); }, "v@:d");
    getter("clearStencil", [](ViewState& v, CpuState& s) { s.x[0] = v.clear_stencil; }, "I@:");
    setter("setClearStencil:", [](Address, ViewState& v, CpuState& s) { v.clear_stencil = static_cast<std::uint32_t>(s.x[2]); }, "v@:I");
    getter("framebufferOnly", [](ViewState& v, CpuState& s) { s.x[0] = v.framebuffer_only ? 1 : 0; }, "B@:");
    setter("setFramebufferOnly:", [](Address, ViewState& v, CpuState& s) { v.framebuffer_only = (s.x[2] & 0xff) != 0; }, "v@:B");
    getter("presentsWithTransaction", [](ViewState& v, CpuState& s) { s.x[0] = v.presents_with_transaction ? 1 : 0; }, "B@:");
    setter("setPresentsWithTransaction:", [](Address, ViewState& v, CpuState& s) {
        v.presents_with_transaction = (s.x[2] & 0xff) != 0;
    }, "v@:B");
    getter("autoResizeDrawable", [](ViewState& v, CpuState& s) { s.x[0] = v.auto_resize ? 1 : 0; }, "B@:");
    setter("setAutoResizeDrawable:", [](Address, ViewState& v, CpuState& s) { v.auto_resize = (s.x[2] & 0xff) != 0; }, "v@:B");
    getter("delegate", [](ViewState& v, CpuState& s) { s.x[0] = v.delegate; }, "@@:");
    setter("setDelegate:", [](Address, ViewState& v, CpuState& s) { v.delegate = s.x[2]; }, "v@:@");
    getter("preferredFramesPerSecond", [](ViewState& v, CpuState& s) { s.x[0] = static_cast<std::uint64_t>(v.frames_per_second); }, "q@:");
    setter("setPreferredFramesPerSecond:", [views](Address view, ViewState& v, CpuState& s) {
        const auto fps = static_cast<std::int64_t>(s.x[2]);
        if (fps == v.frames_per_second) return;
        v.frames_per_second = fps;
        if (v.timer) {   // restart at the new rate
            v.paused = !v.paused;
            views->update_timer(view, v);
            v.paused = !v.paused;
            views->update_timer(view, v);
        }
    }, "v@:q");
    getter("enableSetNeedsDisplay", [](ViewState& v, CpuState& s) { s.x[0] = v.set_needs_display ? 1 : 0; }, "B@:");
    setter("setEnableSetNeedsDisplay:", [views](Address view, ViewState& v, CpuState& s) {
        v.set_needs_display = (s.x[2] & 0xff) != 0;
        views->update_timer(view, v);
    }, "v@:B");
    for (const auto* name : {"isPaused", "paused"})
        getter(name, [](ViewState& v, CpuState& s) { s.x[0] = v.paused ? 1 : 0; }, "B@:");
    setter("setPaused:", [views](Address view, ViewState& v, CpuState& s) {
        v.paused = (s.x[2] & 0xff) != 0;
        views->update_timer(view, v);
    }, "v@:B");
    support.method(cls, "drawableSize", [views](CpuState& s, GuestMemory&) {
        const auto [width, height] = views->drawable_size(s.x[0]);
        s.v[0] = {std::bit_cast<std::uint64_t>(width), 0};
        s.v[1] = {std::bit_cast<std::uint64_t>(height), 0};
    }, "{CGSize=dd}@:");
    support.method(cls, "setDrawableSize:", [views](CpuState& s, GuestMemory&) {
        views->set_drawable_size(s.x[0], std::bit_cast<double>(s.v[0][0]), std::bit_cast<double>(s.v[1][0]));
    }, "v@:{CGSize=dd}");
    support.method(cls, "preferredDrawableSize", [views](CpuState& s, GuestMemory&) {
        const auto [width, height] = views->preferred_size(s.x[0]);
        s.v[0] = {std::bit_cast<std::uint64_t>(width), 0};
        s.v[1] = {std::bit_cast<std::uint64_t>(height), 0};
    }, "{CGSize=dd}@:");
    for (const auto* name : {"colorspace", "setColorspace:"}) {
        const bool set = name[0] == 's';
        support.method(cls, name, [views, sp, name, set](CpuState& s, GuestMemory&) {
            const auto target = views->layer(s.x[0]);
            CallRegs registers;
            registers.x[2] = s.x[2];
            const auto result = target ? sp->objc.send_if_responds(target, sp->objc.selector(name), registers) : std::nullopt;
            if (!set) s.x[0] = result ? result->x0 : 0;
        }, set ? "v@:^{CGColorSpace=}" : "^{CGColorSpace=}@:");
    }

    // Frames.
    support.method(cls, "currentDrawable", [views](CpuState& s, GuestMemory&) {
        s.x[0] = views->current_drawable(s.x[0], *views->state(s.x[0]));
    }, "@@:");
    support.method(cls, "currentRenderPassDescriptor", [views](CpuState& s, GuestMemory&) {
        s.x[0] = views->current_pass(s.x[0], *views->state(s.x[0]));
    }, "@@:");
    support.method(cls, "depthStencilTexture", [views](CpuState& s, GuestMemory&) { s.x[0] = views->state(s.x[0])->depth_texture; }, "@@:");
    support.method(cls, "multisampleColorTexture", [views](CpuState& s, GuestMemory&) { s.x[0] = views->state(s.x[0])->msaa_texture; }, "@@:");
    support.method(cls, "draw", [views](CpuState& s, GuestMemory&) { views->draw(s.x[0]); }, "v@:");
    support.method(cls, "releaseDrawables", [views](CpuState& s, GuestMemory&) {
        const auto state = views->state(s.x[0]);
        views->end_frame(*state);
        views->release_textures(*state);
    }, "v@:");
    support.method(cls, "_alchemyFrame:", [views](CpuState& s, GuestMemory&) { views->draw(s.x[0]); }, "v@:@");
    // MTKView's own -drawRect:: the delegate draws inside -draw; AppKit's
    // display pass draws a frame only in setNeedsDisplay mode.
    support.method(cls, "drawRect:", [views, sp](CpuState& s, GuestMemory&) {
        const auto view = s.x[0];
        const auto state = views->state(view);
        if (!state->drawing) {
            if (state->set_needs_display) views->draw(view);
            return;
        }
        if (!state->delegate) return;
        CallRegs registers;
        registers.x[2] = view;
        static_cast<void>(sp->objc.send_if_responds(state->delegate, sp->objc.selector("drawInMTKView:"), registers));
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    support.method(cls, "viewDidMoveToWindow", [views, sp, cls](CpuState& s, GuestMemory&) {
        static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("viewDidMoveToWindow")));
        const auto state = views->state(s.x[0]);
        views->track_size(s.x[0], *state);
        views->update_timer(s.x[0], *state);
    }, "v@:");
    support.method(cls, "setFrameSize:", [views, sp, cls](CpuState& s, GuestMemory&) {
        CallRegs size;
        size.v[0] = s.v[0];
        size.v[1] = s.v[1];
        static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("setFrameSize:"), size));
        views->track_size(s.x[0], *views->state(s.x[0]));
    }, "v@:{CGSize=dd}");
}

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
    install_mtkview(support);
}

} // namespace alchemy::gui::completion_kit::graphics
