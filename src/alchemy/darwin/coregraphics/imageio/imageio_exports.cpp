#include "alchemy/darwin/coregraphics/imageio/codec.hpp"
#include "alchemy/darwin/coregraphics/q2d_internal.hpp"

#include <algorithm>
#include <cmath>

// ImageIO: CGImageSource (PNG, JPEG, TIFF, BMP, GIF, ICO, ICNS) and
// CGImageDestination (PNG, JPEG, TIFF, BMP, GIF). Decoded images are 8-bit
// RGBA in sRGB: kCGImageAlphaLast (straight) for images with alpha,
// kCGImageAlphaNoneSkipLast otherwise.
namespace alchemy::gui::quartz2d {
namespace {

// CGImageSourceStatus
constexpr std::int32_t status_complete = 0, status_incomplete = -1, status_unknown_type = -3, status_invalid = -4;

struct SourceInfo final : ObjectPayload {
    std::mutex mutex;
    std::vector<std::uint8_t> bytes;
    std::optional<std::string> type;
    std::string type_hint;
    bool incremental{}, final_data{true};
    std::map<std::size_t, std::optional<codec::Frame>> frames;   // decoded cache
    std::optional<std::size_t> count;
    std::size_t frame_count() {
        if (!count) count = codec::frame_count(bytes);
        return *count;
    }
    const std::optional<codec::Frame>& frame(std::size_t index) {
        auto found = frames.find(index);
        if (found == frames.end()) found = frames.emplace(index, codec::decode(bytes, index)).first;
        return found->second;
    }
};

struct DestinationInfo final : ObjectPayload {
    std::string type;
    std::size_t capacity{};
    Address data{};                       // +1 CFMutableData target
    std::string path;                     // file target
    Address consumer{};                   // +1 CGDataConsumer target
    std::vector<codec::Frame> frames;
    double quality{0.85};
    bool finalized{};
};

constexpr std::string_view property_keys[] = {
    "kCGImagePropertyPixelWidth|PixelWidth", "kCGImagePropertyPixelHeight|PixelHeight", "kCGImagePropertyDPIWidth|DPIWidth",
    "kCGImagePropertyDPIHeight|DPIHeight", "kCGImagePropertyDepth|Depth", "kCGImagePropertyOrientation|Orientation",
    "kCGImagePropertyHasAlpha|HasAlpha", "kCGImagePropertyColorModel|ColorModel", "kCGImagePropertyColorModelRGB|RGB",
    "kCGImagePropertyColorModelGray|Gray", "kCGImagePropertyColorModelCMYK|CMYK", "kCGImagePropertyProfileName|ProfileName",
    "kCGImagePropertyFileSize|FileSize", "kCGImagePropertyIsFloat|IsFloat", "kCGImagePropertyIsIndexed|IsIndexed",
    "kCGImagePropertyPNGDictionary|{PNG}", "kCGImagePropertyTIFFDictionary|{TIFF}", "kCGImagePropertyGIFDictionary|{GIF}",
    "kCGImagePropertyJFIFDictionary|{JFIF}", "kCGImagePropertyExifDictionary|{Exif}", "kCGImagePropertyGIFDelayTime|DelayTime",
    "kCGImagePropertyGIFUnclampedDelayTime|UnclampedDelayTime", "kCGImagePropertyGIFLoopCount|LoopCount",
    "kCGImagePropertyTIFFOrientation|Orientation",
    "kCGImageSourceShouldCache|kCGImageSourceShouldCache", "kCGImageSourceShouldCacheImmediately|kCGImageSourceShouldCacheImmediately",
    "kCGImageSourceShouldAllowFloat|kCGImageSourceShouldAllowFloat", "kCGImageSourceTypeIdentifierHint|kCGImageSourceTypeIdentifierHint",
    "kCGImageSourceCreateThumbnailFromImageIfAbsent|kCGImageSourceCreateThumbnailFromImageIfAbsent",
    "kCGImageSourceCreateThumbnailFromImageAlways|kCGImageSourceCreateThumbnailFromImageAlways",
    "kCGImageSourceThumbnailMaxPixelSize|kCGImageSourceThumbnailMaxPixelSize",
    "kCGImageSourceCreateThumbnailWithTransform|kCGImageSourceCreateThumbnailWithTransform",
    "kCGImageSourceSubsampleFactor|kCGImageSourceSubsampleFactor",
    "kCGImageDestinationLossyCompressionQuality|kCGImageDestinationLossyCompressionQuality",
    "kCGImageDestinationBackgroundColor|kCGImageDestinationBackgroundColor",
    "kCGImageDestinationImageMaxPixelSize|kCGImageDestinationImageMaxPixelSize",
};

// EXIF orientation applied to straight RGBA.
codec::Frame orient(const codec::Frame& frame) {
    if (frame.orientation <= 1 || frame.orientation > 8) return frame;
    const bool swap = frame.orientation >= 5;
    codec::Frame out = frame;
    out.width = swap ? frame.height : frame.width;
    out.height = swap ? frame.width : frame.height;
    out.orientation = 1;
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            std::uint32_t sx{}, sy{};
            const auto w = frame.width, h = frame.height;
            switch (frame.orientation) {
            case 2: sx = w - 1 - x; sy = y; break;
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;
            case 4: sx = x; sy = h - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = h - 1 - x; break;
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;
            default: sx = w - 1 - y; sy = x; break;   // 8
            }
            std::memcpy(out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4,
                        frame.rgba.data() + (static_cast<std::size_t>(sy) * w + sx) * 4, 4);
        }
    return out;
}
// Box-filtered downscale so the longer side is at most `maximum`.
codec::Frame shrink(const codec::Frame& frame, double maximum) {
    const auto longest = std::max(frame.width, frame.height);
    if (maximum <= 0 || longest <= maximum) return frame;
    const auto scale = maximum / longest;
    codec::Frame out = frame;
    out.width = std::max(1U, static_cast<std::uint32_t>(std::lround(frame.width * scale)));
    out.height = std::max(1U, static_cast<std::uint32_t>(std::lround(frame.height * scale)));
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const auto x0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * frame.width / out.width);
            const auto x1 = std::max(x0 + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1) * frame.width / out.width));
            const auto y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * frame.height / out.height);
            const auto y1 = std::max(y0 + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1) * frame.height / out.height));
            double r{}, g{}, b{}, a{};
            for (auto sy = y0; sy < y1; ++sy)
                for (auto sx = x0; sx < x1; ++sx) {
                    const auto* p = frame.rgba.data() + (static_cast<std::size_t>(sy) * frame.width + sx) * 4;
                    const auto alpha = p[3] / 255.0;
                    r += p[0] * alpha; g += p[1] * alpha; b += p[2] * alpha; a += alpha;
                }
            const auto n = static_cast<double>((x1 - x0) * (y1 - y0));
            auto* o = out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4;
            if (a > 0) { o[0] = static_cast<std::uint8_t>(std::lround(r / a)); o[1] = static_cast<std::uint8_t>(std::lround(g / a)); o[2] = static_cast<std::uint8_t>(std::lround(b / a)); }
            o[3] = static_cast<std::uint8_t>(std::lround(a / n * 255));
        }
    return out;
}

} // namespace

void Quartz::install_imageio() {
    const auto io = image_io_library;
    const auto source_class = declare_class("_AlchemyCGImageSource");
    const auto destination_class = declare_class("_AlchemyCGImageDestination");
    for (const auto entry : property_keys) {
        const auto bar = entry.find('|');
        string_constant(io, "_" + std::string(entry.substr(0, bar)), entry.substr(bar + 1));
    }
    const auto source = [this](Address object) { return require<SourceInfo>(object, "CGImageSource"); };
    const auto option_bool = [this](Address options, std::string_view key) {
        const auto value = number(dictionary_get(options, key));
        return value && *value != 0;
    };
    const auto make_source = [this, source_class](std::vector<std::uint8_t> bytes, Address options) -> Address {
        auto info = std::make_shared<SourceInfo>();
        info->bytes = std::move(bytes);
        info->type = codec::sniff(info->bytes);
        if (const auto hint = dictionary_get(options, "kCGImageSourceTypeIdentifierHint"); hint && cf.kind(hint) == ObjectKind::string)
            info->type_hint = cf.utf8(hint);
        return create(source_class, info);
    };
    function(io, "_CGImageSourceCreateWithData", [this, make_source](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? make_source(data_bytes(s.x[0]), s.x[1]) : 0;
    });
    function(io, "_CGImageSourceCreateWithURL", [this, make_source](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        auto bytes = read_file(cf.url_path(s.x[0]));
        s.x[0] = bytes ? make_source(std::move(*bytes), s.x[1]) : 0;
    });
    function(io, "_CGImageSourceCreateWithDataProvider", [this, make_source](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return;
        const auto bytes = require<DataProviderInfo>(s.x[0], "CGDataProvider")->snapshot(m);
        s.x[0] = make_source(*bytes, s.x[1]);
    });
    function(io, "_CGImageSourceCreateIncremental", [this, make_source](CpuState& s, GuestMemory&) {
        const auto value = make_source({}, s.x[0]);
        const auto info = find<SourceInfo>(value);
        info->incremental = true;
        info->final_data = false;
        s.x[0] = value;
    });
    const auto update = [this, source](Address object, std::vector<std::uint8_t> bytes, bool final) {
        const auto info = source(object);
        std::lock_guard lock(info->mutex);
        info->bytes = std::move(bytes);
        info->type = codec::sniff(info->bytes);
        info->final_data = final;
        info->frames.clear();
        info->count.reset();
    };
    function(io, "_CGImageSourceUpdateData", [this, update](CpuState& s, GuestMemory&) { if (s.x[0]) update(s.x[0], data_bytes(s.x[1]), bool_arg(s, 2)); });
    function(io, "_CGImageSourceUpdateDataProvider", [this, update](CpuState& s, GuestMemory& m) {
        if (s.x[0] && s.x[1]) update(s.x[0], *require<DataProviderInfo>(s.x[1], "CGDataProvider")->snapshot(m), bool_arg(s, 2));
    });
    function(io, "_CGImageSourceGetType", [this, source](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = source(s.x[0]);
        std::lock_guard lock(info->mutex);
        s.x[0] = info->type ? cf.constant_string(*info->type) : 0;
    });
    function(io, "_CGImageSourceGetCount", [source](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = source(s.x[0]);
        std::lock_guard lock(info->mutex);
        s.x[0] = info->type && info->final_data ? info->frame_count() : 0;
    });
    function(io, "_CGImageSourceGetPrimaryImageIndex", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    const auto status = [source](Address object, std::optional<std::size_t> index) -> std::int32_t {
        const auto info = source(object);
        std::lock_guard lock(info->mutex);
        if (!info->final_data) return status_incomplete;
        if (!info->type) return info->bytes.empty() ? status_incomplete : status_unknown_type;
        const auto count = info->frame_count();
        if (count == 0) return status_invalid;
        if (index && *index >= count) return status_invalid;
        return status_complete;
    };
    function(io, "_CGImageSourceGetStatus", [status](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint32_t>(s.x[0] ? status(s.x[0], std::nullopt) : status_invalid);
    });
    function(io, "_CGImageSourceGetStatusAtIndex", [status](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint32_t>(s.x[0] ? status(s.x[0], s.x[1]) : status_invalid);
    });
    const auto frame_image = [this](const codec::Frame& frame, const std::string& type) {
        PixelLayout layout;
        layout.width = frame.width;
        layout.height = frame.height;
        layout.bytes_per_row = static_cast<std::uint64_t>(frame.width) * 4;
        layout.bitmap_info = frame.has_alpha ? alpha_info::last : alpha_info::none_skip_last;
        layout.straight_alpha = true;
        return make_image(layout, srgb(), std::make_shared<const std::vector<std::uint8_t>>(frame.rgba), type);
    };
    function(io, "_CGImageSourceCreateImageAtIndex", [source, frame_image](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = source(s.x[0]);
        std::lock_guard lock(info->mutex);
        if (!info->type || !info->final_data) { s.x[0] = 0; return; }
        const auto& frame = info->frame(static_cast<std::size_t>(s.x[1]));
        s.x[0] = frame ? frame_image(*frame, *info->type) : 0;
    });
    function(io, "_CGImageSourceCreateThumbnailAtIndex", [this, source, frame_image, option_bool](CpuState& s, GuestMemory&) {
        // No file here carries an embedded thumbnail: one is created only when asked.
        if (!s.x[0]) return;
        const auto options = s.x[2];
        if (!option_bool(options, "kCGImageSourceCreateThumbnailFromImageIfAbsent") &&
            !option_bool(options, "kCGImageSourceCreateThumbnailFromImageAlways")) { s.x[0] = 0; return; }
        const auto info = source(s.x[0]);
        std::lock_guard lock(info->mutex);
        if (!info->type || !info->final_data) { s.x[0] = 0; return; }
        const auto& frame = info->frame(static_cast<std::size_t>(s.x[1]));
        if (!frame) { s.x[0] = 0; return; }
        auto result = option_bool(options, "kCGImageSourceCreateThumbnailWithTransform") ? orient(*frame) : *frame;
        const auto maximum = number(dictionary_get(options, "kCGImageSourceThumbnailMaxPixelSize"));
        result = shrink(result, maximum.value_or(0));
        s.x[0] = frame_image(result, *info->type);
    });
    const auto properties = [this](const codec::Frame& frame, const std::string& type) {
        std::vector<std::pair<Address, Address>> pairs;
        const auto add = [&](std::string_view key, Address value) { pairs.emplace_back(cf.string(key), value); };
        add("PixelWidth", cf.integer(frame.width));
        add("PixelHeight", cf.integer(frame.height));
        add("DPIWidth", cf.real(frame.dpi_x));
        add("DPIHeight", cf.real(frame.dpi_y));
        add("Depth", cf.integer(frame.bits_per_component));
        add("ColorModel", cf.string("RGB"));
        add("HasAlpha", process.objects().retain(cf.boolean(frame.has_alpha)));
        add("ProfileName", cf.string("sRGB IEC61966-2.1"));
        if (frame.orientation != 1) add("Orientation", cf.integer(frame.orientation));
        if (type == codec::gif) {
            std::vector<std::pair<Address, Address>> gif{{cf.string("DelayTime"), cf.real(std::max(frame.delay, 0.1))},
                                                         {cf.string("UnclampedDelayTime"), cf.real(frame.delay)}};
            const auto dictionary = cf.dictionary(gif);
            for (const auto& [key, value] : gif) { release(key); release(value); }
            add("{GIF}", dictionary);
        }
        const auto dictionary = cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { release(key); release(value); }
        return dictionary;
    };
    function(io, "_CGImageSourceCopyPropertiesAtIndex", [source, properties](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = source(s.x[0]);
        std::lock_guard lock(info->mutex);
        if (!info->type || !info->final_data) { s.x[0] = 0; return; }
        const auto frame = codec::describe(info->bytes, static_cast<std::size_t>(s.x[1]));
        s.x[0] = frame ? properties(*frame, *info->type) : 0;
    });
    function(io, "_CGImageSourceCopyProperties", [this, source](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        const auto info = source(s.x[0]);
        std::lock_guard lock(info->mutex);
        std::vector<std::pair<Address, Address>> pairs{{cf.string("FileSize"), cf.integer(static_cast<std::int64_t>(info->bytes.size()))}};
        const auto dictionary = cf.dictionary(pairs);
        for (const auto& [key, value] : pairs) { release(key); release(value); }
        s.x[0] = dictionary;
    });
    const auto type_array = [this](const std::vector<std::string>& types) {
        std::vector<Address> values;
        for (const auto& type : types) values.push_back(cf.string(type));
        const auto array = cf.array(values);
        for (const auto value : values) release(value);
        return array;
    };
    function(io, "_CGImageSourceCopyTypeIdentifiers", [type_array](CpuState& s, GuestMemory&) { s.x[0] = type_array(codec::readable_types()); });
    function(io, "_CGImageSourceGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });

    // ---- CGImageDestination ----
    const auto make_destination = [this, destination_class](Address type, std::size_t count) -> std::shared_ptr<DestinationInfo> {
        if (!type || cf.kind(type) != ObjectKind::string) return nullptr;
        const auto name = cf.utf8(type);
        const auto writable = codec::writable_types();
        if (std::find(writable.begin(), writable.end(), name) == writable.end()) return nullptr;
        auto info = std::make_shared<DestinationInfo>();
        info->type = name;
        info->capacity = count;
        return info;
    };
    const auto finish_destination = [this, destination_class](std::shared_ptr<DestinationInfo> info) {
        return create(destination_class, info, [this, info](Address) {
            release(std::exchange(info->data, 0));
            release(std::exchange(info->consumer, 0));
        });
    };
    function(io, "_CGImageDestinationCreateWithData", [this, make_destination, finish_destination](CpuState& s, GuestMemory&) {
        // (CFMutableData, type, count, options)
        const auto info = s.x[0] ? make_destination(s.x[1], static_cast<std::size_t>(s.x[2])) : nullptr;
        if (!info) { s.x[0] = 0; return; }
        info->data = retain(s.x[0]);
        s.x[0] = finish_destination(info);
    });
    function(io, "_CGImageDestinationCreateWithURL", [this, make_destination, finish_destination](CpuState& s, GuestMemory&) {
        const auto info = s.x[0] ? make_destination(s.x[1], static_cast<std::size_t>(s.x[2])) : nullptr;
        if (!info) { s.x[0] = 0; return; }
        info->path = cf.url_path(s.x[0]);
        s.x[0] = finish_destination(info);
    });
    function(io, "_CGImageDestinationCreateWithDataConsumer", [this, make_destination, finish_destination](CpuState& s, GuestMemory&) {
        const auto info = s.x[0] ? make_destination(s.x[1], static_cast<std::size_t>(s.x[2])) : nullptr;
        if (!info) { s.x[0] = 0; return; }
        info->consumer = retain(s.x[0]);
        s.x[0] = finish_destination(info);
    });
    const auto destination = [this](Address object) { return require<DestinationInfo>(object, "CGImageDestination"); };
    const auto quality_from = [this](DestinationInfo& info, Address properties) {
        if (const auto quality = number(dictionary_get(properties, "kCGImageDestinationLossyCompressionQuality"))) info.quality = *quality;
    };
    const auto add_frame = [](DestinationInfo& info, codec::Frame frame) {
        if (info.finalized) fail("CGImageDestinationAddImage after CGImageDestinationFinalize");
        if (info.capacity && info.frames.size() >= info.capacity) {
            diagnostic("CGImageDestinationAddImage: image destination cannot have more than " + std::to_string(info.capacity) + " images");
            return;
        }
        info.frames.push_back(std::move(frame));
    };
    function(io, "_CGImageDestinationAddImage", [this, destination, quality_from, add_frame](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) return;
        const auto info = destination(s.x[0]);
        quality_from(*info, s.x[2]);
        const auto pixels = image_pixels(s.x[1]);
        codec::Frame frame;
        frame.width = pixels->width;
        frame.height = pixels->height;
        frame.rgba.resize(pixels->pixels.size());
        for (std::size_t at = 0; at < pixels->pixels.size(); at += 4) {
            const auto a = pixels->pixels[at + 3];
            const auto un = [a](std::uint8_t value) { return a ? static_cast<std::uint8_t>(std::min(255, (value * 255 + a / 2) / a)) : std::uint8_t{0}; };
            frame.rgba[at] = un(pixels->pixels[at + 2]);
            frame.rgba[at + 1] = un(pixels->pixels[at + 1]);
            frame.rgba[at + 2] = un(pixels->pixels[at]);
            frame.rgba[at + 3] = a;
            if (a != 255) frame.has_alpha = true;
        }
        add_frame(*info, std::move(frame));
    });
    function(io, "_CGImageDestinationAddImageFromSource", [this, destination, source, quality_from, add_frame](CpuState& s, GuestMemory&) {
        // (destination, source, index, properties)
        if (!s.x[0] || !s.x[1]) return;
        const auto info = destination(s.x[0]);
        quality_from(*info, s.x[3]);
        const auto from = source(s.x[1]);
        std::lock_guard lock(from->mutex);
        const auto& frame = from->frame(static_cast<std::size_t>(s.x[2]));
        if (frame) add_frame(*info, *frame);
    });
    function(io, "_CGImageDestinationSetProperties", [destination, quality_from](CpuState& s, GuestMemory&) {
        if (s.x[0]) quality_from(*destination(s.x[0]), s.x[1]);
    });
    function(io, "_CGImageDestinationFinalize", [this, destination](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { return_bool(s, false); return; }
        const auto info = destination(s.x[0]);
        if (info->finalized || info->frames.empty()) { return_bool(s, false); return; }
        info->finalized = true;
        const auto bytes = codec::encode(info->type, info->frames, info->quality);
        if (!bytes) { return_bool(s, false); return; }
        if (info->data) append_data(info->data, *bytes);
        else if (!info->path.empty()) { if (!write_file(info->path, *bytes)) { return_bool(s, false); return; } }
        else if (info->consumer) consumer_write(*this, info->consumer, *bytes);
        return_bool(s, true);
    });
    function(io, "_CGImageDestinationCopyTypeIdentifiers", [type_array](CpuState& s, GuestMemory&) { s.x[0] = type_array(codec::writable_types()); });
    function(io, "_CGImageDestinationGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = 0x100; });
}

} // namespace alchemy::gui::quartz2d
