#include "alchemy/darwin/coregraphics/imageio/codec.hpp"
#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

#include <algorithm>
#include <cstring>

// CGDataProvider, CGDataConsumer and CGImage.
namespace alchemy::gui::quartz2d {

std::shared_ptr<const std::vector<std::uint8_t>> DataProviderInfo::snapshot(GuestMemory& memory) const {
    if (bytes) return bytes;
    auto copy = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(guest_size));
    if (guest_size) memory.read_bytes(guest_data, *copy);
    return copy;
}

void consumer_write(Quartz& q, Address consumer, std::span<const std::uint8_t> bytes) {
    const auto info = q.require<ConsumerInfo>(consumer, "CGDataConsumer");
    if (info->data) { q.append_data(info->data, bytes); return; }
    if (!info->path.empty()) { info->file_bytes.insert(info->file_bytes.end(), bytes.begin(), bytes.end()); return; }
    if (!info->put) return;
    Quartz::Scratch buffer(q, 65536);
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
        q.write(buffer.address(), bytes.subspan(offset, count));
        const auto written = q.call_guest(info->put, {info->info, buffer.address(), count});
        if (written == 0) return;
        offset += static_cast<std::size_t>(std::min<Address>(written, count));
    }
}

Address Quartz::make_provider(std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
    auto info = std::make_shared<DataProviderInfo>();
    info->bytes = std::move(bytes);
    return create(provider_class, info);
}

Address Quartz::make_image(const PixelLayout& layout, Address space, std::shared_ptr<const std::vector<std::uint8_t>> bytes,
                           std::string type_identifier) {
    auto info = std::make_shared<ImageInfo>();
    info->desc.layout = layout;
    info->space = retain(space);
    info->provider = make_provider(std::move(bytes));
    info->type_identifier = std::move(type_identifier);
    return create(image_class, info, [this, info](Address) {
        release(std::exchange(info->space, 0));
        release(std::exchange(info->provider, 0));
        release(std::exchange(info->mask_image, 0));
    });
}

std::shared_ptr<const Pbgra> Quartz::image_pixels(Address image) {
    const auto info = require<ImageInfo>(image, "CGImage");
    std::lock_guard lock(info->mutex);
    if (info->decoded) return info->decoded;
    const auto provider = require<DataProviderInfo>(info->provider, "CGDataProvider");
    const auto bytes = provider->snapshot(memory());
    auto desc = info->desc;
    if (desc.layout.model == ColorModel::indexed && info->space) {
        // Expand the palette to RGB once.
        const auto space = space_info(info->space);
        const auto base_count = space->base ? space_info(space->base)->components() : 3;
        for (std::uint32_t index = 0; index <= space->last_index; ++index) {
            std::vector<double> values(base_count);
            for (unsigned component = 0; component < base_count; ++component) {
                const auto offset = static_cast<std::size_t>(index) * base_count + component;
                values[component] = offset < space->table.size() ? space->table[offset] / 255.0 : 0;
            }
            const auto rgba = to_rgba(space->base, values, 1);
            desc.palette.push_back(static_cast<std::uint8_t>(std::lround(rgba.r * 255)));
            desc.palette.push_back(static_cast<std::uint8_t>(std::lround(rgba.g * 255)));
            desc.palette.push_back(static_cast<std::uint8_t>(std::lround(rgba.b * 255)));
        }
    }
    auto pixels = std::make_shared<Pbgra>(decode_samples(desc, *bytes));
    if (info->mask_image) {
        // CGImageCreateWithMask: mask samples are inverse alpha (1 masks out).
        const auto mask_info = require<ImageInfo>(info->mask_image, "CGImage");
        const auto mask_provider = require<DataProviderInfo>(mask_info->provider, "CGDataProvider");
        auto mask_desc = mask_info->desc;
        mask_desc.mask = true;   // decode sample s as alpha (1 - s)
        const auto mask = decode_samples(mask_desc, *mask_provider->snapshot(memory()));
        for (std::uint32_t y = 0; y < pixels->height; ++y)
            for (std::uint32_t x = 0; x < pixels->width; ++x) {
                const auto mx = std::min(mask.width - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * mask.width / pixels->width));
                const auto my = std::min(mask.height - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * mask.height / pixels->height));
                const auto a = mask.row(my)[mx * 4 + 3];
                auto* p = pixels->row(y) + x * 4;
                for (unsigned channel = 0; channel < 4; ++channel) p[channel] = static_cast<std::uint8_t>((p[channel] * a + 127) / 255);
            }
    }
    info->decoded = pixels;
    return pixels;
}

void Quartz::install_images() {
    const auto cg = core_graphics_library;
    image_class = declare_class("_AlchemyCGImage");
    provider_class = declare_class("_AlchemyCGDataProvider");
    consumer_class = declare_class("_AlchemyCGDataConsumer");
    const auto image = [this](Address object) { return require<ImageInfo>(object, "CGImage"); };
    const auto provider = [this](Address object) { return require<DataProviderInfo>(object, "CGDataProvider"); };

    // ---- CGDataProvider ----
    function(cg, "_CGDataProviderCreateWithData", [this](CpuState& s, GuestMemory&) {
        // (info, data, size, releaseData(info, data, size))
        auto info = std::make_shared<DataProviderInfo>();
        info->release_info = s.x[0];
        info->guest_data = s.x[1];
        info->guest_size = s.x[2];
        info->release_callback = s.x[3];
        if (!info->guest_data && info->guest_size) { s.x[0] = 0; return; }
        s.x[0] = create(provider_class, info, [this, info](Address) {
            if (info->release_callback) static_cast<void>(call_guest(info->release_callback, {info->release_info, info->guest_data, info->guest_size}));
        });
    });
    function(cg, "_CGDataProviderCreateWithCFData", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        auto info = std::make_shared<DataProviderInfo>();
        info->bytes = std::make_shared<const std::vector<std::uint8_t>>(data_bytes(s.x[0]));
        info->cf_data = retain(s.x[0]);
        s.x[0] = create(provider_class, info, [this, info](Address) { release(std::exchange(info->cf_data, 0)); });
    });
    function(cg, "_CGDataProviderCreateWithURL", [this](CpuState& s, GuestMemory&) {
        const auto bytes = s.x[0] ? read_file(cf.url_path(s.x[0])) : std::nullopt;
        s.x[0] = bytes ? make_provider(std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes))) : 0;
    });
    function(cg, "_CGDataProviderCreateWithFilename", [this](CpuState& s, GuestMemory& m) {
        bool terminated{};
        const auto raw = s.x[0] ? m.read_string(s.x[0], 1, 4096, &terminated) : std::vector<std::uint8_t>{};
        const auto bytes = s.x[0] && terminated ? read_file(std::string(raw.begin(), raw.end())) : std::nullopt;
        s.x[0] = bytes ? make_provider(std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes))) : 0;
    });
    function(cg, "_CGDataProviderCreateSequential", [this](CpuState& s, GuestMemory& m) {
        // (info, callbacks {u32 version; getBytes(info, buf, count); skipForward; rewind; releaseInfo})
        const auto info_pointer = s.x[0], callbacks = s.x[1];
        if (!callbacks) { s.x[0] = 0; return; }
        const auto get_bytes = m.read64(callbacks + 8), release_info = m.read64(callbacks + 32);
        auto bytes = std::make_shared<std::vector<std::uint8_t>>();
        Scratch buffer(*this, 65536);
        for (;;) {
            const auto count = call_guest(get_bytes, {info_pointer, buffer.address(), 65536});
            if (count == 0 || count > 65536) break;
            const auto chunk = read(buffer.address(), static_cast<std::size_t>(count));
            bytes->insert(bytes->end(), chunk.begin(), chunk.end());
            if (bytes->size() > (1ULL << 30)) fail("sequential data provider exceeds 1 GiB");
        }
        auto info = std::make_shared<DataProviderInfo>();
        info->bytes = bytes;
        info->release_info = info_pointer;
        s.x[0] = create(provider_class, info, [this, info_pointer, release_info](Address) {
            if (release_info) static_cast<void>(call_guest(release_info, {info_pointer}));
        });
    });
    function(cg, "_CGDataProviderCreateDirect", [this](CpuState& s, GuestMemory& m) {
        // (info, size, callbacks {u32 version; getBytePointer; releaseBytePointer; getBytesAtPosition; releaseInfo})
        const auto info_pointer = s.x[0], size = s.x[1], callbacks = s.x[2];
        if (!callbacks || size > (1ULL << 30)) { s.x[0] = 0; return; }
        const auto get_pointer = m.read64(callbacks + 8), release_pointer = m.read64(callbacks + 16);
        const auto get_at = m.read64(callbacks + 24), release_info = m.read64(callbacks + 32);
        auto bytes = std::make_shared<std::vector<std::uint8_t>>();
        if (get_pointer) {
            const auto pointer = call_guest(get_pointer, {info_pointer});
            if (pointer) *bytes = read(pointer, static_cast<std::size_t>(size));
            if (release_pointer) static_cast<void>(call_guest(release_pointer, {info_pointer, pointer}));
        } else if (get_at) {
            Scratch buffer(*this, 65536);
            for (Address position = 0; position < size;) {
                const auto want = std::min<Address>(65536, size - position);
                const auto count = call_guest(get_at, {info_pointer, buffer.address(), position, want});
                if (count == 0 || count > want) break;
                const auto chunk = read(buffer.address(), static_cast<std::size_t>(count));
                bytes->insert(bytes->end(), chunk.begin(), chunk.end());
                position += count;
            }
        }
        auto info = std::make_shared<DataProviderInfo>();
        info->bytes = bytes;
        s.x[0] = create(provider_class, info, [this, info_pointer, release_info](Address) {
            if (release_info) static_cast<void>(call_guest(release_info, {info_pointer}));
        });
    });
    function(cg, "_CGDataProviderCopyData", [this, provider](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return;
        s.x[0] = data_object(*provider(s.x[0])->snapshot(m));
    });
    function(cg, "_CGDataProviderGetInfo", [provider](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? provider(s.x[0])->release_info : 0; });
    function(cg, "_CGDataProviderRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGDataProviderRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGDataProviderGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CGDataConsumer ----
    function(cg, "_CGDataConsumerCreateWithCFData", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        auto info = std::make_shared<ConsumerInfo>();
        info->data = retain(s.x[0]);
        s.x[0] = create(consumer_class, info, [this, info](Address) { release(std::exchange(info->data, 0)); });
    });
    function(cg, "_CGDataConsumerCreateWithURL", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        auto info = std::make_shared<ConsumerInfo>();
        info->path = cf.url_path(s.x[0]);
        if (!write_file(info->path, {})) { s.x[0] = 0; return; }
        s.x[0] = create(consumer_class, info, [this, info](Address) { static_cast<void>(write_file(info->path, info->file_bytes)); });
    });
    function(cg, "_CGDataConsumerCreate", [this](CpuState& s, GuestMemory& m) {
        // (info, callbacks {putBytes(info, buffer, count), releaseConsumer(info)})
        if (!s.x[1]) { s.x[0] = 0; return; }
        auto info = std::make_shared<ConsumerInfo>();
        info->info = s.x[0];
        info->put = m.read64(s.x[1]);
        info->release = m.read64(s.x[1] + 8);
        s.x[0] = create(consumer_class, info, [this, info](Address) {
            if (info->release) static_cast<void>(call_guest(info->release, {info->info}));
        });
    });
    function(cg, "_CGDataConsumerRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGDataConsumerRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGDataConsumerGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CGImage ----
    const auto create_image = [this](const PixelLayout& layout, Address space, Address provider_object, Address decode, bool interpolate,
                                     std::int32_t intent, bool mask) -> Address {
        if (!provider_object || layout.width == 0 || layout.height == 0 || layout.bits_per_component == 0 ||
            layout.bits_per_pixel < layout.bits_per_component || layout.bytes_per_row * 8 < static_cast<std::uint64_t>(layout.width) * layout.bits_per_pixel)
            return 0;
        static_cast<void>(require<DataProviderInfo>(provider_object, "CGDataProvider"));
        auto info = std::make_shared<ImageInfo>();
        info->desc.layout = layout;
        info->desc.mask = mask;
        if (decode) {
            const auto count = mask ? 1U : layout.color_components + (layout.has_alpha() ? 1U : 0U);
            for (unsigned index = 0; index < 2 * count; ++index) info->desc.decode.push_back(read_double(decode + index * 8));
        }
        info->space = retain(space);
        info->provider = retain(provider_object);
        info->should_interpolate = interpolate;
        info->intent = intent;
        return create(image_class, info, [this, info](Address) {
            release(std::exchange(info->space, 0));
            release(std::exchange(info->provider, 0));
            release(std::exchange(info->mask_image, 0));
        });
    };
    function(cg, "_CGImageCreate", [this, create_image](CpuState& s, GuestMemory& m) {
        // (width, height, bpc, bpp, bytesPerRow, space, bitmapInfo, provider | decode, shouldInterpolate, intent)
        StackReader stack{s, m};
        const auto decode = stack.next<std::uint64_t>();
        const auto interpolate = stack.next<std::uint8_t>() != 0;
        const auto intent = stack.next<std::int32_t>();
        PixelLayout layout;
        layout.width = static_cast<std::uint32_t>(s.x[0]);
        layout.height = static_cast<std::uint32_t>(s.x[1]);
        layout.bits_per_component = static_cast<std::uint32_t>(s.x[2]);
        layout.bits_per_pixel = static_cast<std::uint32_t>(s.x[3]);
        layout.bytes_per_row = s.x[4];
        layout.bitmap_info = static_cast<std::uint32_t>(s.x[6]);
        layout.straight_alpha = true;
        if (!s.x[5]) { s.x[0] = 0; return; }
        const auto space = space_info(s.x[5]);
        layout.model = space->model;
        layout.color_components = space->model == ColorModel::indexed ? 1 : space->components();
        if (s.x[0] > 65536 || s.x[1] > 65536) { s.x[0] = 0; return; }
        s.x[0] = create_image(layout, s.x[5], s.x[7], decode, interpolate, intent, false);
    });
    function(cg, "_CGImageMaskCreate", [create_image](CpuState& s, GuestMemory&) {
        // (width, height, bpc, bpp, bytesPerRow, provider, decode, shouldInterpolate)
        PixelLayout layout;
        layout.width = static_cast<std::uint32_t>(s.x[0]);
        layout.height = static_cast<std::uint32_t>(s.x[1]);
        layout.bits_per_component = static_cast<std::uint32_t>(s.x[2]);
        layout.bits_per_pixel = static_cast<std::uint32_t>(s.x[3]);
        layout.bytes_per_row = s.x[4];
        layout.model = ColorModel::monochrome;
        layout.color_components = 1;
        if (s.x[0] > 65536 || s.x[1] > 65536) { s.x[0] = 0; return; }
        s.x[0] = create_image(layout, 0, s.x[5], s.x[6], bool_arg(s, 7), 0, true);
    });
    const auto copy_image = [this](const ImageInfo& source, Address space) {
        auto info = std::make_shared<ImageInfo>();
        info->desc = source.desc;
        info->space = retain(space);
        info->provider = retain(source.provider);
        info->mask_image = retain(source.mask_image);
        info->should_interpolate = source.should_interpolate;
        info->intent = source.intent;
        info->type_identifier = source.type_identifier;
        return create(image_class, info, [this, info](Address) {
            release(std::exchange(info->space, 0));
            release(std::exchange(info->provider, 0));
            release(std::exchange(info->mask_image, 0));
        });
    };
    function(cg, "_CGImageCreateCopy", [image, copy_image](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = image(s.x[0]);
        s.x[0] = copy_image(*info, info->space);
    });
    function(cg, "_CGImageCreateCopyWithColorSpace", [this, image, copy_image](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto info = image(s.x[0]);
        if (info->desc.mask || space_info(s.x[1])->components() != space_info(info->space)->components()) { s.x[0] = 0; return; }
        s.x[0] = copy_image(*info, s.x[1]);
    });
    function(cg, "_CGImageCreateWithMask", [this, image, copy_image](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto info = image(s.x[0]);
        static_cast<void>(image(s.x[1]));
        const auto result = copy_image(*info, info->space);
        const auto copied = this->image_class ? find<ImageInfo>(result) : nullptr;
        if (copied) { release(std::exchange(copied->mask_image, 0)); copied->mask_image = retain(s.x[1]); }
        s.x[0] = result;
    });
    function(cg, "_CGImageCreateWithMaskingColors", [](CpuState&, GuestMemory&) {
        unsupported("CGImageCreateWithMaskingColors (color-key masking)");
    });
    function(cg, "_CGImageCreateWithImageInRect", [this, image](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = image(s.x[0]);
        const auto& layout = info->desc.layout;
        const auto r = rect_intersection(rect_integral(rect_arg(s)),
                                         {{0, 0}, {static_cast<double>(layout.width), static_cast<double>(layout.height)}});
        if (rect_is_null(r) || rect_is_empty(r)) { s.x[0] = 0; return; }
        const auto x0 = static_cast<std::uint32_t>(r.origin.x), y0 = static_cast<std::uint32_t>(r.origin.y);
        const auto w = static_cast<std::uint32_t>(r.size.width), h = static_cast<std::uint32_t>(r.size.height);
        // Byte-aligned pixels are cropped in their own layout; others through RGBA.
        if (layout.bits_per_pixel % 8 == 0 && !info->desc.mask) {
            const auto provider_info = require<DataProviderInfo>(info->provider, "CGDataProvider");
            const auto bytes = provider_info->snapshot(memory());
            const auto bpp = layout.bits_per_pixel / 8;
            auto cropped = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * bpp * h);
            for (std::uint32_t y = 0; y < h; ++y) {
                const auto from = static_cast<std::size_t>(y0 + y) * layout.bytes_per_row + static_cast<std::size_t>(x0) * bpp;
                if (from + static_cast<std::size_t>(w) * bpp > bytes->size()) fail("CGImage data is shorter than its layout");
                std::memcpy(cropped->data() + static_cast<std::size_t>(y) * w * bpp, bytes->data() + from, static_cast<std::size_t>(w) * bpp);
            }
            auto cropped_layout = layout;
            cropped_layout.width = w;
            cropped_layout.height = h;
            cropped_layout.bytes_per_row = static_cast<std::uint64_t>(w) * bpp;
            const auto result = make_image(cropped_layout, info->space, cropped, info->type_identifier);
            const auto copied = find<ImageInfo>(result);
            copied->desc.decode = info->desc.decode;
            copied->should_interpolate = info->should_interpolate;
            s.x[0] = result;
            return;
        }
        const auto pixels = image_pixels(s.x[0]);
        auto cropped = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * 4 * h);
        for (std::uint32_t y = 0; y < h; ++y)
            std::memcpy(cropped->data() + static_cast<std::size_t>(y) * w * 4, pixels->row(y0 + y) + static_cast<std::size_t>(x0) * 4,
                        static_cast<std::size_t>(w) * 4);
        PixelLayout bgra;
        bgra.width = w;
        bgra.height = h;
        bgra.bytes_per_row = static_cast<std::uint64_t>(w) * 4;
        bgra.bitmap_info = alpha_info::premultiplied_first | bitmap_info::order_32_little;
        s.x[0] = make_image(bgra, info->space ? info->space : device_rgb(), cropped);
    });
    const auto from_file = [this](Address provider_object, std::string_view expected) -> Address {
        if (!provider_object) return 0;
        const auto bytes = require<DataProviderInfo>(provider_object, "CGDataProvider")->snapshot(memory());
        const auto type = codec::sniff(*bytes);
        if (!type || *type != expected) return 0;
        const auto frame = codec::decode(*bytes, 0);
        if (!frame) return 0;
        PixelLayout layout;
        layout.width = frame->width;
        layout.height = frame->height;
        layout.bytes_per_row = static_cast<std::uint64_t>(frame->width) * 4;
        layout.bitmap_info = frame->has_alpha ? alpha_info::last : alpha_info::none_skip_last;
        layout.straight_alpha = true;
        return make_image(layout, srgb(), std::make_shared<const std::vector<std::uint8_t>>(frame->rgba), std::string(expected));
    };
    function(cg, "_CGImageCreateWithPNGDataProvider", [from_file](CpuState& s, GuestMemory&) { s.x[0] = from_file(s.x[0], codec::png); });
    function(cg, "_CGImageCreateWithJPEGDataProvider", [from_file](CpuState& s, GuestMemory&) { s.x[0] = from_file(s.x[0], codec::jpeg); });
    function(cg, "_CGImageGetWidth", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->desc.layout.width : 0; });
    function(cg, "_CGImageGetHeight", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->desc.layout.height : 0; });
    function(cg, "_CGImageGetBitsPerComponent", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->desc.layout.bits_per_component : 0; });
    function(cg, "_CGImageGetBitsPerPixel", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->desc.layout.bits_per_pixel : 0; });
    function(cg, "_CGImageGetBytesPerRow", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->desc.layout.bytes_per_row : 0; });
    function(cg, "_CGImageGetColorSpace", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->space : 0; });
    function(cg, "_CGImageGetAlphaInfo", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->desc.layout.alpha() : 0; });
    function(cg, "_CGImageGetBitmapInfo", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->desc.layout.bitmap_info : 0; });
    function(cg, "_CGImageGetByteOrderInfo", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->desc.layout.byte_order() : 0; });
    function(cg, "_CGImageGetPixelFormatInfo", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });   // kCGImagePixelFormatPacked
    function(cg, "_CGImageGetDataProvider", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? image(s.x[0])->provider : 0; });
    function(cg, "_CGImageGetDecode", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    function(cg, "_CGImageGetShouldInterpolate", [image](CpuState& s, GuestMemory&) { return_bool(s, s.x[0] && image(s.x[0])->should_interpolate); });
    function(cg, "_CGImageGetRenderingIntent", [image](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? static_cast<std::uint32_t>(image(s.x[0])->intent) : 0; });
    function(cg, "_CGImageIsMask", [image](CpuState& s, GuestMemory&) { return_bool(s, s.x[0] && image(s.x[0])->desc.mask); });
    function(cg, "_CGImageGetUTType", [this, image](CpuState& s, GuestMemory&) {
        const auto type = s.x[0] ? image(s.x[0])->type_identifier : std::string();
        s.x[0] = type.empty() ? 0 : cf.constant_string(type);
    });
    function(cg, "_CGImageRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    function(cg, "_CGImageRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    function(cg, "_CGImageGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
}

} // namespace alchemy::gui::quartz2d
