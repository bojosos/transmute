#include "alchemy/darwin/kit/completion/graphics/providers.hpp"

#include "alchemy/darwin/quartzcore/ci_engine.hpp"
#include "alchemy/darwin/quartzcore/graphics_math.hpp"
#include "alchemy/darwin/quartzcore/graphics_support.hpp"

#include <cstdio>
#include <set>

// Core Image over the host evaluator (ci_engine.cpp):
//   CIImage            recipes from CGImages, image data/URLs (ImageIO),
//                      CVPixelBuffers, MTLTextures and colors; transforms,
//                      crops, clamping, nearest sampling, filters, compositing;
//   CIFilter           the built-in filters ci_engine implements (one class per
//                      filter, as Core Image registers them, so nibs decode
//                      them by name) with Core Image's key-value inputs,
//                      defaults and attributes; app subclasses keep their
//                      own key-value coding; +filterWithName: answers nil for
//                      a filter Alchemy does not implement (reported once),
//                      as for an unknown name;
//   CIContext          CPU rendering into CGImages, bitmaps, MTLTextures and
//                      CIRenderDestinations, drawing into its CGContext; the
//                      working space is linear sRGB unless the options set
//                      kCIContextWorkingColorSpace to NSNull (none) or a
//                      non-linear space; every other color space is treated
//                      as sRGB;
//   CIVector, CIColor  values with their string forms;
//   CIKernel           kernels compiled into Metal libraries (AIR) cannot run
//                      under Alchemy: creation answers nil with an NSError.
// Metal textures, as in Core Image, put the image's y = 0 on the texture's
// first row.
namespace alchemy::gui::completion_kit::graphics {
namespace {

using CiRgba = ci::Rgba;

constexpr std::string_view corevideo_library = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
constexpr std::string_view imageio_library = "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO";

// CIFormat values (opaque integers to the app, passed back to Core Image).
enum : std::int32_t {
    format_argb8 = 516, format_bgra8 = 778, format_rgba8 = 264, format_abgr8 = 1056, format_rgbah = 2056,
    format_rgbaf = 2312, format_rgba16 = 1803, format_a8 = 265, format_r8 = 267, format_l8 = 268
};

struct ContextValues { bool linear{true}; Address cg{}; Address device{}; };   // cg / device: +1 (bag-held)
struct DestinationValues {
    Address data{}; std::size_t width{}, height{}, row_bytes{}; std::int32_t format{};
    Address texture{}, command_buffer{};   // +1 (bag-held)
};

struct CoreImage {
    explicit CoreImage(Support& owner) : support(owner) {}
    Support& support;
    Address image_class{}, filter_class{}, vector_class{}, color_class{}, context_class{}, destination_class{}, task_class{}, info_class{};
    std::mutex reported_mutex;
    std::set<std::string> reported;

    Address key(const char* name) { return support.cf.constant_string(name); }

    // ---- CIImage objects ----
    ci::Image image_of(Address object) {
        if (!object) return nullptr;
        const auto b = support.objc.find_host_state<PropertyBag>(object);
        if (!b) fail("object is not a CIImage");
        std::lock_guard lock(b->mutex);
        const auto* value = std::any_cast<ci::Image>(&b->extra);
        if (!value) fail("CIImage without a recipe");
        return *value;
    }
    // +0 autoreleased CIImage (nil for a null recipe).
    Address make_image(const ci::Image& image, Address source_cg = 0) {
        if (!image) return 0;
        const auto object = support.objc.allocate(image_class);
        const auto b = bag(support, object);
        { std::lock_guard lock(b->mutex); b->extra = image; }
        if (source_cg) bag_set_object(support, object, "CGImage", source_cg);
        return support.send_id(object, "autorelease");
    }
    void set_image(Address object, const ci::Image& image) {
        const auto b = bag(support, object);
        std::lock_guard lock(b->mutex);
        b->extra = image;
    }

    // Decodes a CGImage into premultiplied sRGB RGBA8 rows (top to bottom).
    std::shared_ptr<ci::Bitmap> decode(Address cg_image) {
        const auto width = support.call(coregraphics_library, "_CGImageGetWidth", {cg_image});
        const auto height = support.call(coregraphics_library, "_CGImageGetHeight", {cg_image});
        auto bitmap = std::make_shared<ci::Bitmap>();
        bitmap->width = width;
        bitmap->height = height;
        if (!width || !height) return bitmap;
        const auto row = width * 4;
        const auto pixels = support.process.allocate_guest(row * height);
        const auto space = support.call(coregraphics_library, "_CGColorSpaceCreateWithName", {key("kCGColorSpaceSRGB")});
        CallRegs create;
        create.x = {pixels, width, height, 8, row, space, 1 /* premultiplied last */, 0};
        const auto context = support.call(coregraphics_library, "_CGBitmapContextCreate", create).x0;
        support.call(coregraphics_library, "_CGColorSpaceRelease", {space});
        if (!context) { support.process.release_guest(pixels); fail("Core Image: CGBitmapContextCreate failed"); }
        CallRegs draw;
        draw.x[0] = context; draw.x[1] = cg_image;
        draw.set_d(0, 0); draw.set_d(1, 0); draw.set_d(2, static_cast<double>(width)); draw.set_d(3, static_cast<double>(height));
        support.call(coregraphics_library, "_CGContextDrawImage", draw);
        support.call(coregraphics_library, "_CGContextRelease", {context});
        bitmap->rgba.resize(row * height);
        support.memory().read_bytes(pixels, bitmap->rgba);
        support.process.release_guest(pixels);
        return bitmap;
    }
    // +1 CGImage from premultiplied sRGB RGBA8 rows.
    Address cg_image(const std::vector<std::uint8_t>& rgba, std::size_t width, std::size_t height) {
        const auto row = width * 4;
        const auto pixels = support.process.allocate_guest(std::max<std::size_t>(row * height, 4));
        support.memory().write_bytes(pixels, rgba);
        const auto space = support.call(coregraphics_library, "_CGColorSpaceCreateWithName", {key("kCGColorSpaceSRGB")});
        CallRegs create;
        create.x = {pixels, width, height, 8, row, space, 1, 0};
        const auto context = support.call(coregraphics_library, "_CGBitmapContextCreate", create).x0;
        support.call(coregraphics_library, "_CGColorSpaceRelease", {space});
        if (!context) { support.process.release_guest(pixels); fail("Core Image: CGBitmapContextCreate failed"); }
        const auto image = support.call(coregraphics_library, "_CGBitmapContextCreateImage", {context});
        support.call(coregraphics_library, "_CGContextRelease", {context});
        support.process.release_guest(pixels);
        return image;
    }
    ci::Image from_cg(Address cg) { return cg ? ci::bitmap_image(decode(cg)) : nullptr; }
    // ImageIO source (+1, released here) -> recipe.
    std::pair<ci::Image, Address> from_source(Address source) {
        if (!source) return {nullptr, 0};
        const auto image = support.call(imageio_library, "_CGImageSourceCreateImageAtIndex", {source, 0, 0});
        const std::array<Address, 1> arguments{source};
        static_cast<void>(support.cf.invoke("CFRelease", arguments));
        if (!image) return {nullptr, 0};
        auto recipe = from_cg(image);
        return {recipe, image};
    }

    // ---- values ----
    std::vector<double> vector_of(Address object) {
        const auto b = bag(support, object);
        std::lock_guard lock(b->mutex);
        const auto* value = std::any_cast<std::vector<double>>(&b->extra);
        return value ? *value : std::vector<double>{};
    }
    Address make_vector(std::vector<double> values) {
        const auto object = support.send_id(support.send_id(vector_class, "alloc"), "init");
        const auto b = bag(support, object);
        { std::lock_guard lock(b->mutex); b->extra = std::move(values); }
        return support.send_id(object, "autorelease");
    }
    CiRgba color_of(Address object) {
        const auto b = bag(support, object);
        std::lock_guard lock(b->mutex);
        const auto* value = std::any_cast<CiRgba>(&b->extra);
        return value ? *value : CiRgba{0, 0, 0, 1};
    }
    Address make_color(const CiRgba& value) {
        const auto object = support.send_id(support.send_id(color_class, "alloc"), "init");
        const auto b = bag(support, object);
        { std::lock_guard lock(b->mutex); b->extra = value; }
        return support.send_id(object, "autorelease");
    }
    bool is_kind(Address object, Address cls) {
        if (!object || !cls) return false;
        return (support.send_id(object, "isKindOfClass:", {cls}) & 0xff) != 0;
    }
    // An Objective-C filter input -> an evaluator value.
    ci::Value to_value(Address object) {
        if (!object) return std::monostate{};
        if (is_kind(object, image_class)) return image_of(object);
        if (is_kind(object, vector_class)) return vector_of(object);
        if (is_kind(object, color_class)) return color_of(object);
        if (is_kind(object, support.objc.find_class("NSAffineTransform"))) {
            const auto buffer = support.process.allocate_guest(48);
            CallRegs registers;
            registers.indirect_result = buffer;
            support.send(object, "transformStruct", registers);
            std::array<double, 6> m{};
            for (unsigned i = 0; i < 6; ++i) m[i] = std::bit_cast<double>(support.memory().read64(buffer + i * 8));
            support.process.release_guest(buffer);
            return ci::Transform{m[0], m[1], m[2], m[3], m[4], m[5]};
        }
        if (const auto number = support.cf.number_value(object)) return *number;
        if (support.objc.responds(object, support.objc.selector("doubleValue"))) return support.send(object, "doubleValue").d(0);
        return std::monostate{};
    }
    // An evaluator default -> an Objective-C value (+0).
    Address from_value(const ci::Value& value) {
        if (const auto* number = std::get_if<double>(&value)) return support.send_id(support.cf.real(*number), "autorelease");
        if (const auto* values = std::get_if<std::vector<double>>(&value)) return make_vector(*values);
        if (const auto* color = std::get_if<CiRgba>(&value)) return make_color(*color);
        if (const auto* t = std::get_if<ci::Transform>(&value)) {
            const auto object = support.send_id(support.objc.find_class("NSAffineTransform"), "transform");
            const auto buffer = support.process.allocate_guest(48);
            const std::array<double, 6> m{t->a, t->b, t->c, t->d, t->tx, t->ty};
            for (unsigned i = 0; i < 6; ++i) support.memory().write64(buffer + i * 8, std::bit_cast<std::uint64_t>(m[i]));
            support.send_id(object, "setTransformStruct:", {buffer});
            support.process.release_guest(buffer);
            return object;
        }
        return 0;
    }

    // ---- filters ----
    const ci::FilterSpec* spec_of(Address filter) {
        // The first built-in class in the receiver's class chain.
        auto cls = support.objc.object_class(filter);
        for (int depth = 0; cls && depth < 64; ++depth) {
            const auto name = support.cf.utf8(support.call(foundation_library, "_NSStringFromClass", {cls}));
            if (const auto* spec = ci::find_filter(name)) return spec;
            cls = support.send_id(cls, "superclass");
        }
        return nullptr;
    }
    void apply_defaults(Address filter, const ci::FilterSpec& spec) {
        for (const auto& input : spec.inputs)
            if (!std::holds_alternative<std::monostate>(input.fallback))
                bag_set_object(support, filter, input.key, from_value(input.fallback));
    }
    Address output(Address filter) {
        const auto* spec = spec_of(filter);
        if (!spec) return 0;
        std::map<std::string, ci::Value> inputs;
        for (const auto& input : spec->inputs) {
            const auto value = bag_object(support, filter, input.key);
            inputs[input.key] = value ? to_value(value) : input.fallback;
        }
        return make_image(spec->output(inputs));
    }
    void report_missing_filter(const std::string& name) {
        std::lock_guard lock(reported_mutex);
        if (reported.insert(name).second)
            std::fprintf(stderr, "[alchemy completion] unsupported: Core Image filter %s is not implemented by Alchemy "
                                 "(+[CIFilter filterWithName:] answers nil)\n", name.c_str());
    }
    Address attributes(Address filter) {
        const auto* spec = spec_of(filter);
        std::vector<std::pair<Address, Address>> entries;
        const auto string = [this](const std::string& text) { return support.send_id(support.cf.string(text), "autorelease"); };
        if (!spec) return support.send_id(support.cf.dictionary(entries), "autorelease");
        entries.emplace_back(key("CIAttributeFilterName"), string(spec->name));
        entries.emplace_back(key("CIAttributeFilterDisplayName"), string(spec->display));
        std::vector<Address> categories;
        for (const auto& category : spec->categories) categories.push_back(string(category));
        entries.emplace_back(key("CIAttributeFilterCategories"), support.send_id(support.cf.array(categories), "autorelease"));
        for (const auto& input : spec->inputs) {
            std::vector<std::pair<Address, Address>> attributes;
            const char* class_name = "NSNumber";
            switch (input.type) {
            case ci::InputSpec::Type::image: class_name = "CIImage"; break;
            case ci::InputSpec::Type::vector: class_name = "CIVector"; break;
            case ci::InputSpec::Type::color: class_name = "CIColor"; break;
            case ci::InputSpec::Type::transform: class_name = "NSAffineTransform"; break;
            default: break;
            }
            attributes.emplace_back(key("CIAttributeClass"), string(class_name));
            attributes.emplace_back(key("CIAttributeDisplayName"), string(input.display));
            if (const auto value = from_value(input.fallback)) attributes.emplace_back(key("CIAttributeDefault"), value);
            if (input.minimum) attributes.emplace_back(key("CIAttributeSliderMin"), support.send_id(support.cf.real(*input.minimum), "autorelease"));
            if (input.maximum) attributes.emplace_back(key("CIAttributeSliderMax"), support.send_id(support.cf.real(*input.maximum), "autorelease"));
            entries.emplace_back(string(input.key), support.send_id(support.cf.dictionary(attributes), "autorelease"));
        }
        return support.send_id(support.cf.dictionary(entries), "autorelease");
    }

    // ---- rendering ----
    ci::RenderOptions options_of(Address context) {
        const auto b = bag(support, context);
        std::lock_guard lock(b->mutex);
        const auto* values = std::any_cast<ContextValues>(&b->extra);
        return {values ? values->linear : true};
    }
    std::vector<std::uint8_t> render(Address context, Address image, const ci::Rect& area, std::size_t width, std::size_t height) {
        return ci::render_rgba8(image_of(image), area, width, height, options_of(context));
    }
    // Writes RGBA8 rows in a CIFormat (8-bit formats only).
    static std::vector<std::uint8_t> convert(const std::vector<std::uint8_t>& rgba, std::int32_t format) {
        if (format == format_rgba8) return rgba;
        std::vector<std::uint8_t> out(rgba.size());
        for (std::size_t i = 0; i < rgba.size(); i += 4) {
            const auto r = rgba[i], g = rgba[i + 1], b = rgba[i + 2], a = rgba[i + 3];
            switch (format) {
            case format_bgra8: out[i] = b; out[i + 1] = g; out[i + 2] = r; out[i + 3] = a; break;
            case format_argb8: out[i] = a; out[i + 1] = r; out[i + 2] = g; out[i + 3] = b; break;
            case format_abgr8: out[i] = a; out[i + 1] = b; out[i + 2] = g; out[i + 3] = r; break;
            default: unsupported("Core Image render format " + std::to_string(format) + " (8-bit RGBA, BGRA, ARGB and ABGR only)");
            }
        }
        return out;
    }
    // Renders `area` into `texture` rows (row 0 = y area.y), through a blit in
    // `command_buffer` (when given or the texture is private) or replaceRegion.
    void render_texture(Address context, Address image, Address texture, Address command_buffer, const ci::Rect& area) {
        const auto width = static_cast<std::size_t>(std::llround(area.width)), height = static_cast<std::size_t>(std::llround(area.height));
        if (!width || !height) return;
        const auto format = support.send(texture, "pixelFormat").x0;
        std::int32_t ci_format{};
        if (format == 70 || format == 71) ci_format = format_rgba8;
        else if (format == 80 || format == 81) ci_format = format_bgra8;
        else unsupported("-[CIContext render:toMTLTexture:...] into pixel format " + std::to_string(format) + " (8-bit RGBA/BGRA only)");
        auto rgba = render(context, image, area, width, height);
        // Texture row r holds y = area.y + r: reverse the top-down rows.
        std::vector<std::uint8_t> rows(rgba.size());
        for (std::size_t r = 0; r < height; ++r)
            std::copy_n(rgba.begin() + (height - 1 - r) * width * 4, width * 4, rows.begin() + r * width * 4);
        rows = convert(rows, ci_format);
        const auto storage = support.send(texture, "storageMode").x0;
        const auto region = support.process.allocate_guest(48);
        const std::array<std::uint64_t, 6> values{0, 0, 0, width, height, 1};
        for (unsigned i = 0; i < 6; ++i) support.memory().write64(region + i * 8, values[i]);
        if (!command_buffer && storage != 2) {
            const auto pixels = support.process.allocate_guest(rows.size());
            support.memory().write_bytes(pixels, rows);
            CallRegs replace;
            replace.x[2] = region; replace.x[3] = 0; replace.x[4] = pixels; replace.x[5] = width * 4;
            support.send(texture, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", replace);
            support.process.release_guest(pixels);
            support.process.release_guest(region);
            return;
        }
        const auto device = support.send_id(texture, "device");
        const auto pixels = support.process.allocate_guest(rows.size());
        support.memory().write_bytes(pixels, rows);
        CallRegs make;
        make.x[2] = pixels; make.x[3] = rows.size(); make.x[4] = 0;
        const auto staging = support.send(device, "newBufferWithBytes:length:options:", make).x0;
        support.process.release_guest(pixels);
        Address queue{}, own_buffer{};
        auto buffer = command_buffer;
        if (!buffer) {
            queue = support.send_id(device, "newCommandQueue");
            buffer = own_buffer = support.send_id(queue, "commandBuffer");
        }
        const auto blit = support.send_id(buffer, "blitCommandEncoder");
        CallRegs copy;
        copy.x[2] = staging; copy.x[3] = 0; copy.x[4] = width * 4; copy.x[5] = rows.size();
        copy.x[6] = region + 24; copy.x[7] = texture;
        copy.push_stack<std::uint64_t>(0);
        copy.push_stack<std::uint64_t>(0);
        copy.push_stack<std::uint64_t>(region);
        support.send(blit, "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:", copy);
        support.send_id(blit, "endEncoding");
        if (own_buffer) {
            support.send_id(own_buffer, "commit");
            support.send_id(own_buffer, "waitUntilCompleted");
            support.release(queue);
        }
        support.release(staging);   // the command buffer keeps the copy's source alive
        support.process.release_guest(region);
    }
    // A texture's pixels as a recipe (row 0 at y = 0).
    ci::Image from_texture(Address texture) {
        const auto width = support.send(texture, "width").x0, height = support.send(texture, "height").x0;
        const auto format = support.send(texture, "pixelFormat").x0;
        const bool bgra = format == 80 || format == 81;
        if (!bgra && format != 70 && format != 71)
            unsupported("+[CIImage imageWithMTLTexture:options:] of pixel format " + std::to_string(format) + " (8-bit RGBA/BGRA only)");
        const auto size = width * height * 4;
        const auto pixels = support.process.allocate_guest(std::max<std::uint64_t>(size, 4));
        const auto region = support.process.allocate_guest(48);
        const std::array<std::uint64_t, 6> values{0, 0, 0, width, height, 1};
        for (unsigned i = 0; i < 6; ++i) support.memory().write64(region + i * 8, values[i]);
        if (support.send(texture, "storageMode").x0 == 2) {
            // Private: blit into a shared buffer first.
            const auto device = support.send_id(texture, "device");
            CallRegs make;
            make.x[2] = size; make.x[3] = 0;
            const auto staging = support.send(device, "newBufferWithLength:options:", make).x0;
            const auto queue = support.send_id(device, "newCommandQueue");
            const auto buffer = support.send_id(queue, "commandBuffer");
            const auto blit = support.send_id(buffer, "blitCommandEncoder");
            CallRegs copy;
            copy.x[2] = texture; copy.x[3] = 0; copy.x[4] = 0; copy.x[5] = region; copy.x[6] = region + 24; copy.x[7] = staging;
            copy.push_stack<std::uint64_t>(0);
            copy.push_stack<std::uint64_t>(width * 4);
            copy.push_stack<std::uint64_t>(size);
            support.send(blit, "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:", copy);
            support.send_id(blit, "endEncoding");
            support.send_id(buffer, "commit");
            support.send_id(buffer, "waitUntilCompleted");
            std::vector<std::uint8_t> bytes(size);
            support.memory().read_bytes(support.send_id(staging, "contents"), bytes);
            support.memory().write_bytes(pixels, bytes);
            support.release(staging);
            support.release(queue);
        } else {
            CallRegs read;
            read.x[2] = pixels; read.x[3] = width * 4; read.x[4] = region; read.x[5] = 0;
            support.send(texture, "getBytes:bytesPerRow:fromRegion:mipmapLevel:", read);
        }
        auto bitmap = std::make_shared<ci::Bitmap>();
        bitmap->width = width;
        bitmap->height = height;
        bitmap->rgba.resize(size);
        std::vector<std::uint8_t> bytes(size);
        support.memory().read_bytes(pixels, bytes);
        for (std::uint64_t r = 0; r < height; ++r)   // texture row r -> bitmap row height-1-r
            for (std::uint64_t c = 0; c < width; ++c) {
                const auto* in = bytes.data() + (r * width + c) * 4;
                auto* out = bitmap->rgba.data() + ((height - 1 - r) * width + c) * 4;
                out[0] = bgra ? in[2] : in[0]; out[1] = in[1]; out[2] = bgra ? in[0] : in[2]; out[3] = in[3];
            }
        support.process.release_guest(pixels);
        support.process.release_guest(region);
        return ci::bitmap_image(bitmap);
    }
    // A CVPixelBuffer's 32-bit BGRA / ARGB / RGBA pixels as a recipe.
    ci::Image from_pixel_buffer(Address buffer) {
        const auto cv = [this](const char* symbol, std::initializer_list<Address> arguments) { return support.call(corevideo_library, symbol, arguments); };
        const auto format = static_cast<std::uint32_t>(cv("_CVPixelBufferGetPixelFormatType", {buffer}));
        constexpr std::uint32_t bgra = 0x42475241, rgba = 0x52474241, argb = 0x20;
        if (format != bgra && format != rgba && format != argb)
            unsupported("+[CIImage imageWithCVPixelBuffer:] of pixel format " + std::to_string(format) + " (32-bit BGRA/RGBA/ARGB only)");
        cv("_CVPixelBufferLockBaseAddress", {buffer, 1});
        const auto width = cv("_CVPixelBufferGetWidth", {buffer}), height = cv("_CVPixelBufferGetHeight", {buffer});
        const auto row = cv("_CVPixelBufferGetBytesPerRow", {buffer});
        const auto base = cv("_CVPixelBufferGetBaseAddress", {buffer});
        auto bitmap = std::make_shared<ci::Bitmap>();
        bitmap->width = width;
        bitmap->height = height;
        bitmap->rgba.resize(width * height * 4);
        std::vector<std::uint8_t> line(width * 4);
        for (std::uint64_t r = 0; r < height && base; ++r) {
            support.memory().read_bytes(base + r * row, line);
            for (std::uint64_t c = 0; c < width; ++c) {
                const auto* in = line.data() + c * 4;
                auto* out = bitmap->rgba.data() + (r * width + c) * 4;
                if (format == bgra) { out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; out[3] = in[3]; }
                else if (format == argb) { out[0] = in[1]; out[1] = in[2]; out[2] = in[3]; out[3] = in[0]; }
                else std::copy_n(in, 4, out);
            }
        }
        cv("_CVPixelBufferUnlockBaseAddress", {buffer, 1});
        return ci::bitmap_image(bitmap);
    }
};

ci::Rect rect_of(const std::array<double, 4>& r) { return {r[0], r[1], r[2], r[3]}; }
void put_ci_rect(CpuState& s, const ci::Rect& r) {
    if (r.infinite()) {
        // CGRectInfinite.
        const auto big = 8.98846567431158e307;
        put_rect(s, {-big, -big, 2 * big, 2 * big});
        return;
    }
    put_rect(s, {r.x, r.y, r.width, r.height});
}

} // namespace

void install_core_image(Support& support) {
    auto* sp = &support;
    auto ci_state = std::make_shared<CoreImage>(support);
    auto& k = *ci_state;
    const auto key = [sp](const char* name) { return sp->cf.constant_string(name); };

    // ---- constants ----
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"kCIInputImageKey", "inputImage"}, {"kCIInputBackgroundImageKey", "inputBackgroundImage"},
             {"kCIInputBrightnessKey", "inputBrightness"}, {"kCIInputContrastKey", "inputContrast"},
             {"kCIInputSaturationKey", "inputSaturation"}, {"kCIInputRadiusKey", "inputRadius"}, {"kCIInputCenterKey", "inputCenter"},
             {"kCIInputAngleKey", "inputAngle"}, {"kCIInputIntensityKey", "inputIntensity"}, {"kCIInputColorKey", "inputColor"},
             {"kCIInputTransformKey", "inputTransform"}, {"kCIInputScaleKey", "inputScale"}, {"kCIInputAspectRatioKey", "inputAspectRatio"},
             {"kCIInputEVKey", "inputEV"}, {"kCIInputExtentKey", "inputExtent"}, {"kCIInputWidthKey", "inputWidth"},
             {"kCIInputSharpnessKey", "inputSharpness"}, {"kCIInputTimeKey", "inputTime"}, {"kCIInputMaskImageKey", "inputMaskImage"},
             {"kCIInputTargetImageKey", "inputTargetImage"}, {"kCIInputGradientImageKey", "inputGradientImage"},
             {"kCIInputShadingImageKey", "inputShadingImage"}, {"kCIInputVersionKey", "inputVersion"}, {"kCIOutputImageKey", "outputImage"},
             {"kCIAttributeFilterName", "CIAttributeFilterName"}, {"kCIAttributeFilterDisplayName", "CIAttributeFilterDisplayName"},
             {"kCIAttributeFilterCategories", "CIAttributeFilterCategories"}, {"kCIAttributeClass", "CIAttributeClass"},
             {"kCIAttributeType", "CIAttributeType"}, {"kCIAttributeDefault", "CIAttributeDefault"},
             {"kCIAttributeDisplayName", "CIAttributeDisplayName"}, {"kCIAttributeSliderMin", "CIAttributeSliderMin"},
             {"kCIAttributeSliderMax", "CIAttributeSliderMax"}, {"kCIAttributeMin", "CIAttributeMin"}, {"kCIAttributeMax", "CIAttributeMax"},
             {"kCIAttributeDescription", "CIAttributeDescription"}, {"kCIAttributeIdentity", "CIAttributeIdentity"},
             {"kCICategoryBuiltIn", "CICategoryBuiltIn"}, {"kCICategoryColorAdjustment", "CICategoryColorAdjustment"},
             {"kCICategoryColorEffect", "CICategoryColorEffect"}, {"kCICategoryCompositeOperation", "CICategoryCompositeOperation"},
             {"kCICategoryGeometryAdjustment", "CICategoryGeometryAdjustment"}, {"kCICategoryGenerator", "CICategoryGenerator"},
             {"kCICategoryVideo", "CICategoryVideo"}, {"kCICategoryStillImage", "CICategoryStillImage"},
             {"kCIContextWorkingColorSpace", "working_color_space"}, {"kCIContextOutputColorSpace", "output_color_space"},
             {"kCIContextWorkingFormat", "working_format"}, {"kCIContextUseSoftwareRenderer", "software_renderer"},
             {"kCIContextCacheIntermediates", "cache_intermediates"}, {"kCIContextPriorityRequestLow", "priority_request_low"},
             {"kCIContextHighQualityDownsample", "high_quality_downsample"}, {"kCIContextName", "name"},
             {"kCIImageColorSpace", "CIImageColorSpace"}, {"kCIImageProperties", "CIImageProperties"},
             {"kCIImageApplyOrientationProperty", "CIImageApplyOrientationProperty"}, {"kCIImageNearestSampling", "CIImageNearestSampling"},
             {"kCIImageCacheImmediately", "kCIImageCacheImmediately"}, {"kCIImageToneMapHDRtoSDR", "kCIImageToneMapHDRtoSDR"}}) {
        const auto name = "_" + std::string(symbol);
        if (!support.process.resolve(coreimage_library, name)) support.string_constant(coreimage_library, name, value);
    }
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, std::int32_t>>{
             {"_kCIFormatARGB8", format_argb8}, {"_kCIFormatBGRA8", format_bgra8}, {"_kCIFormatRGBA8", format_rgba8},
             {"_kCIFormatABGR8", format_abgr8}, {"_kCIFormatRGBAh", format_rgbah}, {"_kCIFormatRGBAf", format_rgbaf},
             {"_kCIFormatRGBA16", format_rgba16}, {"_kCIFormatA8", format_a8}, {"_kCIFormatR8", format_r8}, {"_kCIFormatL8", format_l8}}) {
        if (support.process.resolve(coreimage_library, symbol)) continue;
        const auto address = support.process.allocate_guest(8, 8);
        support.memory().write32(address, static_cast<std::uint32_t>(value));
        support.process.install_export(std::string(coreimage_library), symbol, {address, ExportKind::data});
    }

    // ---- CIVector ----
    k.vector_class = support.declare_class("CIVector", "NSObject", coreimage_library);
    Props(support, k.vector_class).lifecycle(false);   // -copyWithZone: below
    const auto vector_factory = [sp](const char* name, unsigned count, bool class_method, const char* types) {
        sp->method(sp->objc.find_class("CIVector"), name, [sp, count, class_method](CpuState& s, GuestMemory&) {
            std::vector<double> values;
            for (unsigned i = 0; i < count; ++i) values.push_back(d(s, i));
            const auto object = class_method ? sp->send_id(sp->send_id(s.x[0], "alloc"), "init") : s.x[0];
            const auto b = bag(*sp, object);
            { std::lock_guard lock(b->mutex); b->extra = values; }
            s.x[0] = class_method ? sp->send_id(object, "autorelease") : object;
        }, types, class_method);
    };
    vector_factory("vectorWithX:", 1, true, "@@:d");
    vector_factory("vectorWithX:Y:", 2, true, "@@:dd");
    vector_factory("vectorWithX:Y:Z:", 3, true, "@@:ddd");
    vector_factory("vectorWithX:Y:Z:W:", 4, true, "@@:dddd");
    vector_factory("vectorWithCGPoint:", 2, true, "@@:{CGPoint=dd}");
    vector_factory("vectorWithCGRect:", 4, true, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    vector_factory("initWithX:", 1, false, "@@:d");
    vector_factory("initWithX:Y:", 2, false, "@@:dd");
    vector_factory("initWithX:Y:Z:", 3, false, "@@:ddd");
    vector_factory("initWithX:Y:Z:W:", 4, false, "@@:dddd");
    vector_factory("initWithCGPoint:", 2, false, "@@:{CGPoint=dd}");
    vector_factory("initWithCGRect:", 4, false, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    // (const CGFloat *values, size_t count)
    const auto with_values = [sp](CpuState& s, GuestMemory& m, Address object) {
        std::vector<double> values;
        for (std::uint64_t i = 0; i < s.x[3] && i < 4096; ++i) values.push_back(std::bit_cast<double>(m.read64(s.x[2] + i * 8)));
        const auto b = bag(*sp, object);
        std::lock_guard lock(b->mutex);
        b->extra = values;
    };
    support.method(k.vector_class, "initWithValues:count:", [with_values](CpuState& s, GuestMemory& m) { with_values(s, m, s.x[0]); }, "@@:^dQ");
    support.method(k.vector_class, "vectorWithValues:count:", [sp, with_values](CpuState& s, GuestMemory& m) {
        const auto object = sp->send_id(sp->send_id(s.x[0], "alloc"), "init");
        with_values(s, m, object);
        s.x[0] = sp->send_id(object, "autorelease");
    }, "@@:^dQ", true);
    const auto vector_from_string = [ci_state](Address object, Address string) {
        const auto parsed = string ? parse_vector_string(ci_state->support.cf.utf8(string)) : std::nullopt;
        const auto b = bag(ci_state->support, object);
        std::lock_guard lock(b->mutex);
        b->extra = parsed.value_or(std::vector<double>{});
    };
    support.method(k.vector_class, "initWithString:", [vector_from_string](CpuState& s, GuestMemory&) { vector_from_string(s.x[0], s.x[2]); }, "@@:@");
    support.method(k.vector_class, "vectorWithString:", [sp, vector_from_string](CpuState& s, GuestMemory&) {
        const auto object = sp->send_id(sp->send_id(s.x[0], "alloc"), "init");
        vector_from_string(object, s.x[2]);
        s.x[0] = sp->send_id(object, "autorelease");
    }, "@@:@", true);
    support.method(k.vector_class, "count", [ci_state](CpuState& s, GuestMemory&) { s.x[0] = ci_state->vector_of(s.x[0]).size(); }, "Q@:");
    support.method(k.vector_class, "valueAtIndex:", [ci_state](CpuState& s, GuestMemory&) {
        const auto values = ci_state->vector_of(s.x[0]);
        put_d(s, 0, s.x[2] < values.size() ? values[s.x[2]] : 0.0);   // out of range: 0, as Core Image
    }, "d@:Q");
    for (unsigned index = 0; index < 4; ++index) {
        static constexpr const char* names[] = {"X", "Y", "Z", "W"};
        support.method(k.vector_class, names[index], [ci_state, index](CpuState& s, GuestMemory&) {
            const auto values = ci_state->vector_of(s.x[0]);
            put_d(s, 0, index < values.size() ? values[index] : 0.0);
        }, "d@:");
    }
    support.method(k.vector_class, "CGPointValue", [ci_state](CpuState& s, GuestMemory&) {
        auto v = ci_state->vector_of(s.x[0]);
        v.resize(2);
        put_d(s, 0, v[0]); put_d(s, 1, v[1]);
    }, "{CGPoint=dd}@:");
    support.method(k.vector_class, "CGRectValue", [ci_state](CpuState& s, GuestMemory&) {
        auto v = ci_state->vector_of(s.x[0]);
        v.resize(4);
        put_rect(s, {v[0], v[1], v[2], v[3]});
    }, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    support.method(k.vector_class, "stringRepresentation", [sp, ci_state](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased_string(*sp, vector_string(ci_state->vector_of(s.x[0])));
    }, "@@:");
    support.method(k.vector_class, "description", [sp, ci_state](CpuState& s, GuestMemory&) {
        s.x[0] = autoreleased_string(*sp, vector_string(ci_state->vector_of(s.x[0])));
    }, "@@:");
    support.method(k.vector_class, "isEqual:", [ci_state](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[2]) { s.x[0] = 1; return; }
        put_bool(s, s.x[2] && ci_state->is_kind(s.x[2], ci_state->vector_class) &&
                        ci_state->vector_of(s.x[0]) == ci_state->vector_of(s.x[2]));
    }, "B@:@");
    support.method(k.vector_class, "hash", [ci_state](CpuState& s, GuestMemory&) {
        std::uint64_t h = 1469598103934665603ULL;
        for (const auto value : ci_state->vector_of(s.x[0])) h = (h ^ std::bit_cast<std::uint64_t>(value)) * 1099511628211ULL;
        s.x[0] = h;
    }, "Q@:");
    support.method(k.vector_class, "copyWithZone:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->retain(s.x[0]); }, "@@:^v");

    // ---- CIColor ----
    k.color_class = support.declare_class("CIColor", "NSObject", coreimage_library);
    Props(support, k.color_class).lifecycle(false);   // -copyWithZone: below
    const auto set_color = [sp](Address object, const CiRgba& value) {
        const auto b = bag(*sp, object);
        std::lock_guard lock(b->mutex);
        b->extra = value;
    };
    const auto color_factory = [sp, set_color](const char* name, unsigned count, bool class_method, const char* types) {
        sp->method(sp->objc.find_class("CIColor"), name, [sp, set_color, count, class_method](CpuState& s, GuestMemory&) {
            CiRgba value{d(s, 0), d(s, 1), d(s, 2), count == 4 ? d(s, 3) : 1.0};
            const auto object = class_method ? sp->send_id(sp->send_id(s.x[0], "alloc"), "init") : s.x[0];
            set_color(object, value);
            s.x[0] = class_method ? sp->send_id(object, "autorelease") : object;
        }, types, class_method);
    };
    color_factory("colorWithRed:green:blue:alpha:", 4, true, "@@:dddd");
    color_factory("colorWithRed:green:blue:", 3, true, "@@:ddd");
    color_factory("initWithRed:green:blue:alpha:", 4, false, "@@:dddd");
    color_factory("initWithRed:green:blue:", 3, false, "@@:ddd");
    // From a CGColor / NSColor: its sRGB components.
    const auto components_of_cg = [sp](Address cg) -> CiRgba {
        const auto count = sp->call(coregraphics_library, "_CGColorGetNumberOfComponents", {cg});
        const auto values = sp->call(coregraphics_library, "_CGColorGetComponents", {cg});
        std::vector<double> c;
        for (std::uint64_t i = 0; i < count && values; ++i) c.push_back(std::bit_cast<double>(sp->memory().read64(values + i * 8)));
        if (c.size() == 2) return {c[0], c[0], c[0], c[1]};
        if (c.size() >= 4) return {c[0], c[1], c[2], c[3]};
        unsupported("CIColor from a CGColor with " + std::to_string(c.size()) + " components (gray or RGB only)");
    };
    support.method(k.color_class, "colorWithCGColor:", [sp, set_color, components_of_cg](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("+[CIColor colorWithCGColor:] with NULL");
        const auto object = sp->send_id(sp->send_id(s.x[0], "alloc"), "init");
        set_color(object, components_of_cg(s.x[2]));
        s.x[0] = sp->send_id(object, "autorelease");
    }, "@@:^{CGColor=}", true);
    support.method(k.color_class, "initWithCGColor:", [set_color, components_of_cg](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("-[CIColor initWithCGColor:] with NULL");
        set_color(s.x[0], components_of_cg(s.x[2]));
    }, "@@:^{CGColor=}");
    support.method(k.color_class, "initWithColor:", [sp, set_color](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { sp->release(s.x[0]); s.x[0] = 0; return; }
        const auto buffer = sp->process.allocate_guest(32);
        CallRegs registers;
        for (unsigned i = 0; i < 4; ++i) registers.x[2 + i] = buffer + i * 8;
        sp->send(s.x[2], "getRed:green:blue:alpha:", registers);
        CiRgba value{};
        for (unsigned i = 0; i < 4; ++i) value[i] = std::bit_cast<double>(sp->memory().read64(buffer + i * 8));
        sp->process.release_guest(buffer);
        set_color(s.x[0], value);
    }, "@@:@");
    support.method(k.color_class, "colorWithString:", [sp, set_color](CpuState& s, GuestMemory&) {
        const auto parsed = s.x[2] ? parse_vector_string(sp->cf.utf8(s.x[2])) : std::nullopt;
        if (!parsed || parsed->size() < 3) { s.x[0] = 0; return; }
        const auto object = sp->send_id(sp->send_id(s.x[0], "alloc"), "init");
        set_color(object, {(*parsed)[0], (*parsed)[1], (*parsed)[2], parsed->size() > 3 ? (*parsed)[3] : 1.0});
        s.x[0] = sp->send_id(object, "autorelease");
    }, "@@:@", true);
    for (unsigned index = 0; index < 4; ++index) {
        static constexpr const char* names[] = {"red", "green", "blue", "alpha"};
        support.method(k.color_class, names[index], [ci_state, index](CpuState& s, GuestMemory&) {
            put_d(s, 0, ci_state->color_of(s.x[0])[index]);
        }, "d@:");
    }
    support.method(k.color_class, "numberOfComponents", [](CpuState& s, GuestMemory&) { s.x[0] = 4; }, "Q@:");
    support.method(k.color_class, "stringRepresentation", [sp, ci_state](CpuState& s, GuestMemory&) {
        const auto c = ci_state->color_of(s.x[0]);
        s.x[0] = autoreleased_string(*sp, vector_string({c[0], c[1], c[2], c[3]}));
    }, "@@:");
    support.method(k.color_class, "colorSpace", [sp](CpuState& s, GuestMemory&) {
        const auto space = sp->call(coregraphics_library, "_CGColorSpaceCreateWithName", {sp->cf.constant_string("kCGColorSpaceSRGB")});
        s.x[0] = sp->send_id(space, "autorelease");
    }, "^{CGColorSpace=}@:");
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, CiRgba>>{
             {"blackColor", {0, 0, 0, 1}}, {"whiteColor", {1, 1, 1, 1}}, {"grayColor", {0.5, 0.5, 0.5, 1}}, {"redColor", {1, 0, 0, 1}},
             {"greenColor", {0, 1, 0, 1}}, {"blueColor", {0, 0, 1, 1}}, {"cyanColor", {0, 1, 1, 1}}, {"magentaColor", {1, 0, 1, 1}},
             {"yellowColor", {1, 1, 0, 1}}, {"clearColor", {0, 0, 0, 0}}}) {
        const auto rgba = value;
        support.method(k.color_class, name, [ci_state, rgba](CpuState& s, GuestMemory&) { s.x[0] = ci_state->make_color(rgba); }, "@@:", true);
    }
    support.method(k.color_class, "isEqual:", [ci_state](CpuState& s, GuestMemory&) {
        if (s.x[0] == s.x[2]) { s.x[0] = 1; return; }
        put_bool(s, s.x[2] && ci_state->is_kind(s.x[2], ci_state->color_class) && ci_state->color_of(s.x[0]) == ci_state->color_of(s.x[2]));
    }, "B@:@");
    support.method(k.color_class, "copyWithZone:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->retain(s.x[0]); }, "@@:^v");

    // ---- CIImage ----
    k.image_class = support.declare_class("CIImage", "NSObject", coreimage_library);
    Props(support, k.image_class).lifecycle(false);   // -copyWithZone: below
    support.method(k.image_class, "copyWithZone:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->retain(s.x[0]); }, "@@:^v");
    // Factories: +imageWith... (autoreleased) and -initWith... (the receiver).
    const auto factory = [sp, ci_state](const char* class_name, const char* init_name, const char* types,
                                        std::function<std::pair<ci::Image, Address>(CpuState&)> make) {
        const auto cls = ci_state->image_class;
        sp->method(cls, class_name, [ci_state, make](CpuState& s, GuestMemory&) {
            const auto [image, cg] = make(s);
            s.x[0] = ci_state->make_image(image, cg);
            if (cg) ci_state->support.call(coregraphics_library, "_CGImageRelease", {cg});
        }, types, true);
        if (init_name)
            sp->method(cls, init_name, [sp, ci_state, make](CpuState& s, GuestMemory&) {
                const auto self = s.x[0];
                const auto [image, cg] = make(s);
                if (!image) { sp->release(self); s.x[0] = 0; return; }
                ci_state->set_image(self, image);
                if (cg) {
                    bag_set_object(*sp, self, "CGImage", cg);
                    ci_state->support.call(coregraphics_library, "_CGImageRelease", {cg});
                }
                s.x[0] = self;
            }, types);
    };
    const auto retained_cg = [sp](Address cg) { return cg ? sp->retain(cg) : 0; };
    const auto from_cg = [ci_state, retained_cg](CpuState& s) -> std::pair<ci::Image, Address> {
        if (!s.x[2]) return {nullptr, 0};
        return {ci_state->from_cg(s.x[2]), retained_cg(s.x[2])};
    };
    factory("imageWithCGImage:", "initWithCGImage:", "@@:^{CGImage=}", from_cg);
    factory("imageWithCGImage:options:", "initWithCGImage:options:", "@@:^{CGImage=}@", from_cg);
    const auto from_data = [ci_state](CpuState& s) -> std::pair<ci::Image, Address> {
        if (!s.x[2]) return {nullptr, 0};
        return ci_state->from_source(ci_state->support.call(imageio_library, "_CGImageSourceCreateWithData", {s.x[2], 0}));
    };
    factory("imageWithData:", "initWithData:", "@@:@", from_data);
    factory("imageWithData:options:", "initWithData:options:", "@@:@@", from_data);
    const auto from_url = [ci_state](CpuState& s) -> std::pair<ci::Image, Address> {
        if (!s.x[2]) return {nullptr, 0};
        return ci_state->from_source(ci_state->support.call(imageio_library, "_CGImageSourceCreateWithURL", {s.x[2], 0}));
    };
    factory("imageWithContentsOfURL:", "initWithContentsOfURL:", "@@:@", from_url);
    factory("imageWithContentsOfURL:options:", "initWithContentsOfURL:options:", "@@:@@", from_url);
    const auto from_color = [ci_state](CpuState& s) -> std::pair<ci::Image, Address> {
        if (!s.x[2]) return {nullptr, 0};
        return {ci::constant_image(ci_state->color_of(s.x[2])), 0};
    };
    factory("imageWithColor:", "initWithColor:", "@@:@", from_color);
    const auto from_texture = [ci_state](CpuState& s) -> std::pair<ci::Image, Address> {
        if (!s.x[2]) return {nullptr, 0};
        return {ci_state->from_texture(s.x[2]), 0};
    };
    factory("imageWithMTLTexture:options:", "initWithMTLTexture:options:", "@@:@@", from_texture);
    const auto from_pixel_buffer = [ci_state](CpuState& s) -> std::pair<ci::Image, Address> {
        if (!s.x[2]) return {nullptr, 0};
        return {ci_state->from_pixel_buffer(s.x[2]), 0};
    };
    factory("imageWithCVPixelBuffer:", "initWithCVPixelBuffer:", "@@:^{__CVBuffer=}", from_pixel_buffer);
    factory("imageWithCVPixelBuffer:options:", "initWithCVPixelBuffer:options:", "@@:^{__CVBuffer=}@", from_pixel_buffer);
    factory("imageWithCVImageBuffer:", "initWithCVImageBuffer:", "@@:^{__CVBuffer=}", from_pixel_buffer);
    support.method(k.image_class, "emptyImage", [ci_state](CpuState& s, GuestMemory&) { s.x[0] = ci_state->make_image(ci::empty_image()); }, "@@:", true);
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, CiRgba>>{
             {"clearImage", {0, 0, 0, 0}}, {"blackImage", {0, 0, 0, 1}}, {"whiteImage", {1, 1, 1, 1}}, {"grayImage", {0.5, 0.5, 0.5, 1}},
             {"redImage", {1, 0, 0, 1}}, {"greenImage", {0, 1, 0, 1}}, {"blueImage", {0, 0, 1, 1}}, {"cyanImage", {0, 1, 1, 1}},
             {"magentaImage", {1, 0, 1, 1}}, {"yellowImage", {1, 1, 0, 1}}}) {
        const auto rgba = value;
        support.method(k.image_class, name, [ci_state, rgba](CpuState& s, GuestMemory&) { s.x[0] = ci_state->make_image(ci::constant_image(rgba)); }, "@@:", true);
    }
    support.method(k.image_class, "extent", [ci_state](CpuState& s, GuestMemory&) { put_ci_rect(s, ci_state->image_of(s.x[0])->extent); },
                   "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    support.method(k.image_class, "properties", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->cf.dictionary({}), "autorelease");
    }, "@@:");
    support.method(k.image_class, "colorSpace", [sp](CpuState& s, GuestMemory&) {
        const auto space = sp->call(coregraphics_library, "_CGColorSpaceCreateWithName", {sp->cf.constant_string("kCGColorSpaceSRGB")});
        s.x[0] = sp->send_id(space, "autorelease");
    }, "^{CGColorSpace=}@:");
    support.method(k.image_class, "CGImage", [sp](CpuState& s, GuestMemory&) { s.x[0] = bag_object(*sp, s.x[0], "CGImage"); }, "^{CGImage=}@:");
    for (const auto* name : {"url", "pixelBuffer", "metalTexture", "definition"})
        support.method(k.image_class, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    const auto derived = [sp, ci_state](const char* name, const char* types, std::function<ci::Image(const ci::Image&, CpuState&)> make) {
        sp->method(ci_state->image_class, name, [ci_state, make](CpuState& s, GuestMemory&) {
            s.x[0] = ci_state->make_image(make(ci_state->image_of(s.x[0]), s));
        }, types);
    };
    support.method(k.image_class, "imageByApplyingTransform:", [ci_state](CpuState& s, GuestMemory& m) {
        std::array<double, 6> t{};
        for (unsigned i = 0; i < 6; ++i) t[i] = std::bit_cast<double>(m.read64(s.x[2] + i * 8));
        s.x[0] = ci_state->make_image(ci::transformed(ci_state->image_of(s.x[0]), {t[0], t[1], t[2], t[3], t[4], t[5]}));
    }, "@@:{CGAffineTransform=dddddd}");
    derived("imageByCroppingToRect:", "@@:{CGRect={CGPoint=dd}{CGSize=dd}}",
            [](const ci::Image& image, CpuState& s) { return ci::cropped(image, rect_of(rect_arg(s))); });
    derived("imageBySamplingNearest", "@@:", [](const ci::Image& image, CpuState&) { return ci::sampling_nearest(image, true); });
    derived("imageBySamplingLinear", "@@:", [](const ci::Image& image, CpuState&) { return ci::sampling_nearest(image, false); });
    derived("imageByClampingToExtent", "@@:", [](const ci::Image& image, CpuState&) { return ci::clamped(image); });
    derived("imageByClampingToRect:", "@@:{CGRect={CGPoint=dd}{CGSize=dd}}",
            [](const ci::Image& image, CpuState& s) { return ci::clamped(ci::cropped(image, rect_of(rect_arg(s)))); });
    derived("imageByInsertingIntermediate", "@@:", [](const ci::Image& image, CpuState&) { return image; });
    derived("imageByInsertingIntermediate:", "@@:B", [](const ci::Image& image, CpuState&) { return image; });
    // Color matching: every color space is treated as sRGB, which is the
    // space sources and outputs are encoded in, so matching is the identity.
    derived("imageByColorMatchingWorkingSpaceToColorSpace:", "@@:^{CGColorSpace=}", [](const ci::Image& image, CpuState&) { return image; });
    derived("imageByColorMatchingColorSpaceToWorkingSpace:", "@@:^{CGColorSpace=}", [](const ci::Image& image, CpuState&) { return image; });
    support.method(k.image_class, "imageByCompositingOverImage:", [ci_state](CpuState& s, GuestMemory&) {
        const auto* spec = ci::find_filter("CISourceOverCompositing");
        s.x[0] = ci_state->make_image(spec->output({{"inputImage", ci_state->image_of(s.x[0])}, {"inputBackgroundImage", ci_state->image_of(s.x[2])}}));
    }, "@@:@");
    const auto apply_filter = [sp, ci_state](Address image, Address name, Address parameters) -> Address {
        const auto filter = sp->send_id(ci_state->filter_class, "filterWithName:", {name});
        if (!filter) return 0;
        sp->send_id(filter, "setValue:forKey:", {image, sp->cf.constant_string("inputImage")});
        if (parameters)
            for (const auto& [key, value] : sp->cf.dictionary_entries(parameters)) sp->send_id(filter, "setValue:forKey:", {value, key});
        return sp->send_id(filter, "outputImage");
    };
    support.method(k.image_class, "imageByApplyingFilter:", [apply_filter](CpuState& s, GuestMemory&) { s.x[0] = apply_filter(s.x[0], s.x[2], 0); }, "@@:@");
    support.method(k.image_class, "imageByApplyingFilter:withInputParameters:", [apply_filter](CpuState& s, GuestMemory&) {
        s.x[0] = apply_filter(s.x[0], s.x[2], s.x[3]);
    }, "@@:@@");
    // TIFF / EXIF orientations 1-8 about the image's extent.
    support.method(k.image_class, "imageByApplyingOrientation:", [ci_state](CpuState& s, GuestMemory&) {
        const auto image = ci_state->image_of(s.x[0]);
        const auto e = image->extent;
        const auto w = e.width, h = e.height;
        ci::Transform t;
        switch (static_cast<int>(s.x[2])) {
        case 1: break;
        case 2: t = {-1, 0, 0, 1, w, 0}; break;
        case 3: t = {-1, 0, 0, -1, w, h}; break;
        case 4: t = {1, 0, 0, -1, 0, h}; break;
        case 5: t = {0, -1, -1, 0, h, w}; break;
        case 6: t = {0, -1, 1, 0, 0, w}; break;
        case 7: t = {0, 1, 1, 0, 0, 0}; break;
        case 8: t = {0, 1, -1, 0, h, 0}; break;
        default: fail("-[CIImage imageByApplyingOrientation:] orientation " + std::to_string(s.x[2]));
        }
        auto moved = ci::transformed(ci::transformed(image, {1, 0, 0, 1, -e.x, -e.y}), t);
        s.x[0] = ci_state->make_image(moved);
    }, "@@:i");

    // ---- CIFilter ----
    k.filter_class = support.declare_class("CIFilter", "NSObject", coreimage_library);
    support.objc.add_native_protocol(k.filter_class, "NSCopying");
    support.objc.add_native_protocol(k.filter_class, "NSSecureCoding");
    Props(support, k.filter_class).lifecycle().boolean("isEnabled", "setEnabled:", true);
    const auto filter_class = k.filter_class;
    for (const auto& name : ci::filter_names()) {
        const auto cls = support.declare_class(name, "CIFilter", coreimage_library);
        // Built-in filters start with their inputs' defaults.
        support.method(cls, "init", [ci_state](CpuState& s, GuestMemory&) {
            if (const auto* spec = ci_state->spec_of(s.x[0])) ci_state->apply_defaults(s.x[0], *spec);
        }, "@@:");
        support.method(cls, "initWithCoder:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(s.x[0], "init"); }, "@@:@");
    }
    support.method(filter_class, "filterWithName:", [sp, ci_state](CpuState& s, GuestMemory&) {
        const auto name = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string();
        if (!ci::find_filter(name)) {
            if (name.rfind("CI", 0) == 0) ci_state->report_missing_filter(name);
            s.x[0] = 0;
            return;
        }
        const auto cls = sp->objc.find_class(name);
        s.x[0] = sp->send_id(sp->send_id(sp->send_id(cls, "alloc"), "init"), "autorelease");
    }, "@@:@", true);
    support.method(filter_class, "filterWithName:withInputParameters:", [sp](CpuState& s, GuestMemory&) {
        const auto filter = sp->send_id(s.x[0], "filterWithName:", {s.x[2]});
        if (filter && s.x[3])
            for (const auto& [key, value] : sp->cf.dictionary_entries(s.x[3])) sp->send_id(filter, "setValue:forKey:", {value, key});
        s.x[0] = filter;
    }, "@@:@@", true);
    // (name, key, value, key, value, ..., nil): variadic, 8-byte stack slots
    // after the named first key.
    support.method(filter_class, "filterWithName:keysAndValues:", [sp](CpuState& s, GuestMemory& m) {
        const auto filter = sp->send_id(s.x[0], "filterWithName:", {s.x[2]});
        auto key_object = s.x[3];
        auto slot = s.sp;
        for (int count = 0; key_object && count < 1024; ++count) {
            const auto value = m.read64(slot);
            if (filter) sp->send_id(filter, "setValue:forKey:", {value, key_object});
            key_object = m.read64(slot + 8);
            slot += 16;
        }
        s.x[0] = filter;
    }, "@@:@@", true);
    support.method(filter_class, "filterNamesInCategory:", [sp, ci_state](CpuState& s, GuestMemory&) {
        const auto category = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string();
        std::vector<Address> names;
        for (const auto& name : ci::filter_names()) {
            const auto* spec = ci::find_filter(name);
            if (category.empty() || std::find(spec->categories.begin(), spec->categories.end(), category) != spec->categories.end())
                names.push_back(autoreleased_string(*sp, name));
        }
        s.x[0] = sp->send_id(sp->cf.array(names), "autorelease");
        static_cast<void>(ci_state);
    }, "@@:@", true);
    support.method(filter_class, "filterNamesInCategories:", [sp](CpuState& s, GuestMemory&) {
        const auto categories = array_items(*sp, s.x[2]);
        std::vector<Address> names;
        for (const auto& name : ci::filter_names()) {
            const auto* spec = ci::find_filter(name);
            bool all = true;
            for (const auto category : categories)
                if (std::find(spec->categories.begin(), spec->categories.end(), sp->cf.utf8(category)) == spec->categories.end()) all = false;
            if (all) names.push_back(autoreleased_string(*sp, name));
        }
        s.x[0] = sp->send_id(sp->cf.array(names), "autorelease");
    }, "@@:@", true);
    support.method(filter_class, "localizedNameForFilterName:", [sp](CpuState& s, GuestMemory&) {
        const auto* spec = s.x[2] ? ci::find_filter(sp->cf.utf8(s.x[2])) : nullptr;
        s.x[0] = spec ? autoreleased_string(*sp, spec->display) : 0;
    }, "@@:@", true);
    support.method(filter_class, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);
    // Key-value coding: built-in filters keep their inputs; anything else
    // (an app subclass's own properties) goes through NSObject's.
    const auto builtin_key = [ci_state](Address filter, const std::string& name) {
        const auto* spec = ci_state->spec_of(filter);
        if (!spec) return false;
        for (const auto& input : spec->inputs) if (input.key == name) return true;
        return name.rfind("input", 0) == 0;
    };
    support.method(filter_class, "setValue:forKey:", [sp, builtin_key, filter_class](CpuState& s, GuestMemory&) {
        const auto name = s.x[3] ? sp->cf.utf8(s.x[3]) : std::string();
        if (builtin_key(s.x[0], name)) { bag_set_object(*sp, s.x[0], name, s.x[2]); return; }
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.x[3] = s.x[3];
        static_cast<void>(sp->objc.send_super(s.x[0], filter_class, sp->objc.selector("setValue:forKey:"), registers));
    }, "v@:@@");
    support.method(filter_class, "valueForKey:", [sp, builtin_key, filter_class](CpuState& s, GuestMemory&) {
        const auto name = s.x[2] ? sp->cf.utf8(s.x[2]) : std::string();
        if (name == "outputImage") { s.x[0] = sp->send_id(s.x[0], "outputImage"); return; }
        if (builtin_key(s.x[0], name)) { s.x[0] = bag_object(*sp, s.x[0], name); return; }
        CallRegs registers;
        registers.x[2] = s.x[2];
        s.x[0] = sp->objc.send_super(s.x[0], filter_class, sp->objc.selector("valueForKey:"), registers).x0;
    }, "@@:@");
    support.method(filter_class, "outputImage", [ci_state](CpuState& s, GuestMemory&) { s.x[0] = ci_state->output(s.x[0]); }, "@@:");
    support.method(filter_class, "setDefaults", [ci_state](CpuState& s, GuestMemory&) {
        if (const auto* spec = ci_state->spec_of(s.x[0])) ci_state->apply_defaults(s.x[0], *spec);
    }, "v@:");
    support.method(filter_class, "name", [sp, ci_state](CpuState& s, GuestMemory&) {
        if (const auto name = bag_object(*sp, s.x[0], "name")) { s.x[0] = name; return; }
        if (const auto* spec = ci_state->spec_of(s.x[0])) { s.x[0] = autoreleased_string(*sp, spec->name); return; }
        s.x[0] = sp->call(foundation_library, "_NSStringFromClass", {sp->objc.object_class(s.x[0])});
    }, "@@:");
    support.method(filter_class, "setName:", [sp](CpuState& s, GuestMemory&) { bag_set_object(*sp, s.x[0], "name", s.x[2], true); }, "v@:@");
    support.method(filter_class, "attributes", [ci_state](CpuState& s, GuestMemory&) { s.x[0] = ci_state->attributes(s.x[0]); }, "@@:");
    support.method(filter_class, "inputKeys", [sp, ci_state](CpuState& s, GuestMemory&) {
        std::vector<Address> keys;
        if (const auto* spec = ci_state->spec_of(s.x[0]))
            for (const auto& input : spec->inputs) keys.push_back(autoreleased_string(*sp, input.key));
        s.x[0] = sp->send_id(sp->cf.array(keys), "autorelease");
    }, "@@:");
    support.method(filter_class, "outputKeys", [sp](CpuState& s, GuestMemory&) {
        const std::array<Address, 1> keys{sp->cf.constant_string("outputImage")};
        s.x[0] = sp->send_id(sp->cf.array(keys), "autorelease");
    }, "@@:");
    support.method(filter_class, "initWithCoder:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(s.x[0], "init"); }, "@@:@");

    // ---- CIKernel ----
    const auto kernel = support.declare_class("CIKernel", "NSObject", coreimage_library);
    static_cast<void>(support.declare_class("CIColorKernel", "CIKernel", coreimage_library));
    static_cast<void>(support.declare_class("CIWarpKernel", "CIKernel", coreimage_library));
    static_cast<void>(support.declare_class("CIBlendKernel", "CIColorKernel", coreimage_library));
    const auto no_kernel = [sp, key](CpuState& s, Address out_error, const char* what) {
        std::fprintf(stderr, "[alchemy completion] unsupported: %s: Core Image kernels compiled into Metal libraries (AIR) cannot run under Alchemy\n", what);
        if (out_error) {
            const std::vector<std::pair<Address, Address>> entries{
                {key("NSLocalizedDescription"), autoreleased_string(*sp, std::string("unsupported: ") + what +
                                                                       ": Core Image kernels compiled into Metal libraries cannot run under Alchemy")}};
            CallRegs registers;
            registers.x[2] = key("CIKernel");
            registers.x[3] = 1;
            registers.x[4] = sp->send_id(sp->cf.dictionary(entries), "autorelease");
            sp->memory().write64(out_error, sp->send(sp->objc.find_class("NSError"), "errorWithDomain:code:userInfo:", registers).x0);
        }
        s.x[0] = 0;
    };
    support.method(kernel, "kernelWithFunctionName:fromMetalLibraryData:error:", [no_kernel](CpuState& s, GuestMemory&) {
        no_kernel(s, s.x[4], "+[CIKernel kernelWithFunctionName:fromMetalLibraryData:error:]");
    }, "@@:@@^@", true);
    support.method(kernel, "kernelWithFunctionName:fromMetalLibraryData:outputPixelFormat:error:", [no_kernel](CpuState& s, GuestMemory&) {
        no_kernel(s, s.x[5], "+[CIKernel kernelWithFunctionName:fromMetalLibraryData:outputPixelFormat:error:]");
    }, "@@:@@i^@", true);
    support.method(kernel, "kernelsWithMetalString:error:", [no_kernel](CpuState& s, GuestMemory&) {
        no_kernel(s, s.x[3], "+[CIKernel kernelsWithMetalString:error:]");
    }, "@@:@^@", true);
    // No kernel object can exist, so applying one is unreachable.
    support.method(kernel, "applyWithExtent:roiCallback:arguments:", [](CpuState&, GuestMemory&) {
        unsupported("-[CIKernel applyWithExtent:roiCallback:arguments:] (no Core Image kernel can be created under Alchemy)");
    }, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@?@");

    // ---- CIContext ----
    k.context_class = support.declare_class("CIContext", "NSObject", coreimage_library);
    Props(support, k.context_class).lifecycle();
    // Options: kCIContextWorkingColorSpace NSNull (no color management) or a
    // color space (linear when its name says so).
    const auto configure = [sp](Address context, Address options, Address cg, Address device) {
        ContextValues values;
        if (options) {
            const auto space = sp->send_id(options, "objectForKey:", {sp->cf.constant_string("working_color_space")});
            if (space) {
                const auto null_class = sp->objc.find_class("NSNull");
                if (null_class && (sp->send_id(space, "isKindOfClass:", {null_class}) & 0xff)) values.linear = false;
                else {
                    const auto name = sp->call(coregraphics_library, "_CGColorSpaceCopyName", {space});
                    values.linear = name && sp->cf.utf8(name).find("Linear") != std::string::npos;
                    if (name) { const std::array<Address, 1> a{name}; static_cast<void>(sp->cf.invoke("CFRelease", a)); }
                }
            }
        }
        if (cg) bag_set_object(*sp, context, "cg", cg);
        if (device) bag_set_object(*sp, context, "device", device);
        const auto b = bag(*sp, context);
        std::lock_guard lock(b->mutex);
        b->extra = values;
    };
    const auto context_factory = [sp, configure](const char* name, bool class_method, const char* types,
                                                 std::function<std::array<Address, 3>(const CpuState&)> args) {
        sp->method(sp->objc.find_class("CIContext"), name, [sp, configure, class_method, args](CpuState& s, GuestMemory&) {
            const auto [options, cg, device] = args(s);
            const auto object = class_method ? sp->send_id(sp->send_id(s.x[0], "alloc"), "init") : s.x[0];
            configure(object, options, cg, device);
            s.x[0] = class_method ? sp->send_id(object, "autorelease") : object;
        }, types, class_method);
    };
    context_factory("context", true, "@@:", [](const CpuState&) { return std::array<Address, 3>{}; });
    context_factory("contextWithOptions:", true, "@@:@", [](const CpuState& s) { return std::array<Address, 3>{s.x[2], 0, 0}; });
    context_factory("initWithOptions:", false, "@@:@", [](const CpuState& s) { return std::array<Address, 3>{s.x[2], 0, 0}; });
    context_factory("contextWithCGContext:options:", true, "@@:^{CGContext=}@", [](const CpuState& s) { return std::array<Address, 3>{s.x[3], s.x[2], 0}; });
    context_factory("contextWithMTLDevice:", true, "@@:@", [](const CpuState& s) { return std::array<Address, 3>{0, 0, s.x[2]}; });
    context_factory("contextWithMTLDevice:options:", true, "@@:@@", [](const CpuState& s) { return std::array<Address, 3>{s.x[3], 0, s.x[2]}; });
    context_factory("contextWithMTLCommandQueue:", true, "@@:@", [sp](const CpuState& s) {
        return std::array<Address, 3>{0, 0, s.x[2] ? sp->send_id(s.x[2], "device") : 0};
    });
    context_factory("contextWithMTLCommandQueue:options:", true, "@@:@@", [sp](const CpuState& s) {
        return std::array<Address, 3>{s.x[3], 0, s.x[2] ? sp->send_id(s.x[2], "device") : 0};
    });
    // +1 CGImage of `rect` (a finite rect; pixels follow units).
    const auto create_image = [ci_state](Address context, Address image, const ci::Rect& rect) -> Address {
        if (!image || rect.empty()) return 0;
        if (rect.infinite()) fail("-[CIContext createCGImage:fromRect:] with an infinite rect");
        const auto width = static_cast<std::size_t>(std::llround(rect.width)), height = static_cast<std::size_t>(std::llround(rect.height));
        if (!width || !height || width > 32768 || height > 32768) return 0;
        return ci_state->cg_image(ci_state->render(context, image, rect, width, height), width, height);
    };
    support.method(k.context_class, "createCGImage:fromRect:", [create_image](CpuState& s, GuestMemory&) {
        s.x[0] = create_image(s.x[0], s.x[2], rect_of(rect_arg(s)));
    }, "^{CGImage=}@:@{CGRect={CGPoint=dd}{CGSize=dd}}");
    support.method(k.context_class, "createCGImage:fromRect:format:colorSpace:", [create_image](CpuState& s, GuestMemory&) {
        s.x[0] = create_image(s.x[0], s.x[2], rect_of(rect_arg(s)));
    }, "^{CGImage=}@:@{CGRect={CGPoint=dd}{CGSize=dd}}i^{CGColorSpace=}");
    // render:toBitmap:rowBytes:bounds:format:colorSpace: (bounds d0-d3, format x5)
    support.method(k.context_class, "render:toBitmap:rowBytes:bounds:format:colorSpace:", [ci_state](CpuState& s, GuestMemory& m) {
        const auto bounds = rect_of(rect_arg(s));
        const auto width = static_cast<std::size_t>(std::llround(bounds.width)), height = static_cast<std::size_t>(std::llround(bounds.height));
        if (!s.x[2] || !s.x[3] || !width || !height) return;
        const auto pixels = ci_state->convert(ci_state->render(s.x[0], s.x[2], bounds, width, height), static_cast<std::int32_t>(s.x[5]));
        for (std::size_t row = 0; row < height; ++row)
            m.write_bytes(s.x[3] + row * s.x[4], std::span<const std::uint8_t>(pixels.data() + row * width * 4, width * 4));
    }, "v@:@^vQ{CGRect={CGPoint=dd}{CGSize=dd}}i^{CGColorSpace=}");
    support.method(k.context_class, "render:toMTLTexture:commandBuffer:bounds:colorSpace:", [ci_state](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) return;
        ci_state->render_texture(s.x[0], s.x[2], s.x[3], s.x[4], rect_of(rect_arg(s)));
    }, "v@:@@@{CGRect={CGPoint=dd}{CGSize=dd}}^{CGColorSpace=}");
    // -drawImage:inRect:fromRect: into the context's CGContext.
    support.method(k.context_class, "drawImage:inRect:fromRect:", [sp, create_image](CpuState& s, GuestMemory&) {
        const auto cg = bag_object(*sp, s.x[0], "cg");
        if (!cg) unsupported("-[CIContext drawImage:inRect:fromRect:] on a context without a CGContext (use +contextWithCGContext:options:)");
        const auto image = create_image(s.x[0], s.x[2], rect_of(rect_arg(s, 4)));
        if (!image) return;
        CallRegs registers;
        registers.x[0] = cg;
        registers.x[1] = image;
        for (unsigned i = 0; i < 4; ++i) registers.set_d(i, d(s, i));
        sp->call(coregraphics_library, "_CGContextDrawImage", registers);
        sp->call(coregraphics_library, "_CGImageRelease", {image});
    }, "v@:@{CGRect={CGPoint=dd}{CGSize=dd}}{CGRect={CGPoint=dd}{CGSize=dd}}");
    support.method(k.context_class, "drawImage:atPoint:fromRect:", [sp](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = s.x[2];
        registers.set_d(0, d(s, 0)); registers.set_d(1, d(s, 1)); registers.set_d(2, d(s, 4)); registers.set_d(3, d(s, 5));
        for (unsigned i = 0; i < 4; ++i) registers.set_d(4 + i, d(s, 2 + i));
        sp->send(s.x[0], "drawImage:inRect:fromRect:", registers);
    }, "v@:@{CGPoint=dd}{CGRect={CGPoint=dd}{CGSize=dd}}");
    support.method(k.context_class, "workingColorSpace", [sp, ci_state](CpuState& s, GuestMemory&) {
        const auto linear = ci_state->options_of(s.x[0]).linear_working_space;
        const auto space = sp->call(coregraphics_library, "_CGColorSpaceCreateWithName",
                                    {sp->cf.constant_string(linear ? "kCGColorSpaceLinearSRGB" : "kCGColorSpaceSRGB")});
        s.x[0] = space ? sp->send_id(space, "autorelease") : 0;
    }, "^{CGColorSpace=}@:");
    support.method(k.context_class, "workingFormat", [](CpuState& s, GuestMemory&) { s.x[0] = format_rgbaf; }, "i@:");
    support.method(k.context_class, "clearCaches", [](CpuState&, GuestMemory&) {}, "v@:");   // nothing is cached
    support.method(k.context_class, "reclaimResources", [](CpuState&, GuestMemory&) {}, "v@:");

    // ---- CIRenderDestination, CIRenderTask, CIRenderInfo ----
    k.destination_class = support.declare_class("CIRenderDestination", "NSObject", coreimage_library);
    Props(support, k.destination_class).lifecycle()
        .boolean("isFlipped", "setFlipped:", false)
        .boolean("isDithered", "setDithered:", false)
        .boolean("isClamped", "setClamped:", true)
        .integer("alphaMode", "setAlphaMode:", 1)   // CIRenderDestinationAlphaPremultiplied
        .boolean("blendsInDestinationColorSpace", "setBlendsInDestinationColorSpace:", false);
    const auto destination_values = [sp](Address object) {
        const auto b = bag(*sp, object);
        std::lock_guard lock(b->mutex);
        const auto* values = std::any_cast<DestinationValues>(&b->extra);
        return values ? *values : DestinationValues{};
    };
    support.method(k.destination_class, "initWithBitmapData:width:height:bytesPerRow:format:", [sp](CpuState& s, GuestMemory&) {
        DestinationValues values;
        values.data = s.x[2]; values.width = s.x[3]; values.height = s.x[4]; values.row_bytes = s.x[5];
        values.format = static_cast<std::int32_t>(s.x[6]);
        const auto b = bag(*sp, s.x[0]);
        std::lock_guard lock(b->mutex);
        b->extra = values;
    }, "@@:^vQQQi");
    support.method(k.destination_class, "initWithMTLTexture:commandBuffer:", [sp](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { sp->release(s.x[0]); s.x[0] = 0; return; }
        DestinationValues values;
        values.texture = s.x[2];
        values.command_buffer = s.x[3];
        values.width = sp->send(s.x[2], "width").x0;
        values.height = sp->send(s.x[2], "height").x0;
        bag_set_object(*sp, s.x[0], "texture", s.x[2]);
        if (s.x[3]) bag_set_object(*sp, s.x[0], "commandBuffer", s.x[3]);
        const auto b = bag(*sp, s.x[0]);
        std::lock_guard lock(b->mutex);
        b->extra = values;
    }, "@@:@@");
    support.method(k.destination_class, "width", [destination_values](CpuState& s, GuestMemory&) { s.x[0] = destination_values(s.x[0]).width; }, "Q@:");
    support.method(k.destination_class, "height", [destination_values](CpuState& s, GuestMemory&) { s.x[0] = destination_values(s.x[0]).height; }, "Q@:");
    Props(support, k.destination_class).object("colorSpace", "setColorSpace:");
    k.info_class = support.declare_class("CIRenderInfo", "NSObject", coreimage_library);
    Props(support, k.info_class).lifecycle().real("kernelExecutionTime", nullptr).integer("passCount", nullptr, 1)
        .integer("pixelsProcessed", nullptr).real("kernelCompileTime", nullptr);
    k.task_class = support.declare_class("CIRenderTask", "NSObject", coreimage_library);
    Props(support, k.task_class).lifecycle();
    support.method(k.task_class, "waitUntilCompletedAndReturnError:", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = bag_object(*sp, s.x[0], "info");   // the render already completed
    }, "@@:^@");
    // Renders `area` of the image into the destination (its [0, w] x [0, h]
    // units; flipped destinations put y = 0 on the first row).
    const auto render_to = [sp, ci_state, destination_values](Address context, Address image, Address destination, const ci::Rect& area) {
        const auto values = destination_values(destination);
        const bool flipped = (sp->send(destination, "isFlipped").x0 & 0xff) != 0;
        if (values.texture) {
            // Textures take y = 0 on row 0; a flipped destination is the other way.
            if (flipped) {
                const auto flipped_image = ci::transformed(ci_state->image_of(image), {1, 0, 0, -1, 0, area.y * 2 + area.height});
                const auto object = ci_state->make_image(flipped_image);
                ci_state->render_texture(context, object, values.texture, values.command_buffer, area);
            } else {
                ci_state->render_texture(context, image, values.texture, values.command_buffer, area);
            }
        } else if (values.data) {
            auto pixels = ci_state->render(context, image, area, values.width, values.height);   // top row first
            if (flipped) {
                std::vector<std::uint8_t> rows(pixels.size());
                for (std::size_t r = 0; r < values.height; ++r)
                    std::copy_n(pixels.begin() + (values.height - 1 - r) * values.width * 4, values.width * 4, rows.begin() + r * values.width * 4);
                pixels.swap(rows);
            }
            pixels = ci_state->convert(pixels, values.format);
            for (std::size_t r = 0; r < values.height; ++r)
                sp->memory().write_bytes(values.data + r * values.row_bytes,
                                         std::span<const std::uint8_t>(pixels.data() + r * values.width * 4, values.width * 4));
        } else {
            fail("CIRenderDestination without a target");
        }
        const auto info = sp->send_id(sp->send_id(ci_state->info_class, "alloc"), "init");
        bag_set_raw(*sp, info, "pixelsProcessed", {values.width * values.height, 0, 0, 0});
        const auto task = sp->send_id(sp->send_id(ci_state->task_class, "alloc"), "init");
        bag_set_object(*sp, task, "info", info);
        sp->release(info);
        return sp->send_id(task, "autorelease");
    };
    support.method(k.context_class, "startTaskToRender:toDestination:error:", [destination_values, render_to](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) { s.x[0] = 0; return; }
        const auto values = destination_values(s.x[3]);
        s.x[0] = render_to(s.x[0], s.x[2], s.x[3], {0, 0, static_cast<double>(values.width), static_cast<double>(values.height)});
    }, "@@:@@^@");
    // (image, fromRect d0-d3, destination, atPoint d4 d5, error)
    support.method(k.context_class, "startTaskToRender:fromRect:toDestination:atPoint:error:", [render_to](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3]) { s.x[0] = 0; return; }
        const auto from = rect_of(rect_arg(s));
        // The destination's point (d4, d5) shows the rect's origin.
        s.x[0] = render_to(s.x[0], s.x[2], s.x[3], {from.x - d(s, 4), from.y - d(s, 5), from.width + d(s, 4), from.height + d(s, 5)});
    }, "@@:@{CGRect={CGPoint=dd}{CGSize=dd}}@{CGPoint=dd}^@");
    support.method(k.context_class, "startTaskToClear:error:", [sp](CpuState& s, GuestMemory&) {
        const auto clear = sp->send_id(sp->objc.find_class("CIImage"), "clearImage");
        s.x[0] = sp->send_id(s.x[0], "startTaskToRender:toDestination:error:", {clear, s.x[2], s.x[3]});
    }, "@@:@^@");

    // CIImage drawing into the current NSGraphicsContext (AppKit additions).
    support.method(k.image_class, "drawInRect:fromRect:operation:fraction:", [sp, ci_state](CpuState& s, GuestMemory& m) {
        const auto gc_class = sp->objc.find_class("NSGraphicsContext");
        const auto gc = gc_class ? sp->send_id(gc_class, "currentContext") : 0;
        const auto cg = gc ? sp->send_id(gc, "CGContext") : 0;
        if (!cg) return;
        auto from = rect_of(rect_arg(s, 4));
        if (from.empty()) from = ci_state->image_of(s.x[0])->extent;
        if (from.infinite() || from.empty()) return;
        const auto context = sp->send_id(ci_state->context_class, "context");
        const auto width = static_cast<std::size_t>(std::llround(from.width)), height = static_cast<std::size_t>(std::llround(from.height));
        const auto image = ci_state->cg_image(ci_state->render(context, s.x[0], from, width, height), width, height);
        const auto fraction = std::bit_cast<double>(m.read64(s.sp));   // past v7: the first stack argument
        CallRegs alpha;
        alpha.x[0] = cg;
        alpha.set_d(0, std::clamp(fraction, 0.0, 1.0));
        sp->call(coregraphics_library, "_CGContextSaveGState", {cg});
        sp->call(coregraphics_library, "_CGContextSetAlpha", alpha);
        CallRegs draw;
        draw.x[0] = cg;
        draw.x[1] = image;
        for (unsigned i = 0; i < 4; ++i) draw.set_d(i, d(s, i));
        sp->call(coregraphics_library, "_CGContextDrawImage", draw);
        sp->call(coregraphics_library, "_CGContextRestoreGState", {cg});
        sp->call(coregraphics_library, "_CGImageRelease", {image});
    }, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}{CGRect={CGPoint=dd}{CGSize=dd}}Qd");
}

} // namespace alchemy::gui::completion_kit::graphics
