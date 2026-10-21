#include "alchemy/darwin/coregraphics/imageio/codec.hpp"
#include "alchemy/host/gpu.hpp"

#include <algorithm>
#include <cstring>

// Image codecs: the host's (host::gpu; WIC on win32) for the common formats,
// and an independently written
// .icns reader (layout from the public description of the format: a
// big-endian 'icns' header followed by typed entries; PNG payloads, 'ARGB'
// payloads and planar RGB payloads compressed with the icns run-length
// scheme, alpha in separate '*8mk' entries).
namespace alchemy::gui::quartz2d::codec {
namespace {

std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) << 24 | static_cast<std::uint32_t>(bytes[at + 1]) << 16 |
           static_cast<std::uint32_t>(bytes[at + 2]) << 8 | bytes[at + 3];
}
bool starts(std::span<const std::uint8_t> bytes, std::initializer_list<std::uint8_t> prefix) {
    if (bytes.size() < prefix.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

// ---- the host's codecs ---------------------------------------------------------------------------
std::optional<Frame> wic_frame(std::span<const std::uint8_t> bytes, std::size_t index, bool pixels) {
    auto decoded = host::gpu::image_frame(bytes, index, pixels);
    if (!decoded) return std::nullopt;
    Frame result;
    result.width = decoded->width;
    result.height = decoded->height;
    result.rgba = std::move(decoded->rgba);
    result.has_alpha = decoded->has_alpha;
    result.dpi_x = decoded->dpi_x;
    result.dpi_y = decoded->dpi_y;
    result.delay = decoded->delay;
    result.orientation = decoded->orientation;
    result.bits_per_component = decoded->bits_per_component;
    return result;
}

std::size_t wic_count(std::span<const std::uint8_t> bytes) { return host::gpu::image_frame_count(bytes).value_or(0); }

// ---- ICNS ----------------------------------------------------------------------------------------
struct IcnsEntry { std::string type; std::span<const std::uint8_t> data; };
std::vector<IcnsEntry> icns_entries(std::span<const std::uint8_t> bytes) {
    std::vector<IcnsEntry> entries;
    if (bytes.size() < 8 || !starts(bytes, {'i', 'c', 'n', 's'})) return entries;
    const auto total = std::min<std::size_t>(be32(bytes, 4), bytes.size());
    for (std::size_t at = 8; at + 8 <= total;) {
        const std::string type(reinterpret_cast<const char*>(bytes.data() + at), 4);
        const auto length = be32(bytes, at + 4);
        if (length < 8 || at + length > total) break;
        entries.push_back({type, bytes.subspan(at + 8, length - 8)});
        at += length;
    }
    return entries;
}
// icns run-length scheme: a control byte below 0x80 copies (control + 1)
// literal bytes; otherwise the next byte repeats (control - 125) times.
std::optional<std::vector<std::uint8_t>> icns_unpack(std::span<const std::uint8_t> data, std::size_t produce) {
    std::vector<std::uint8_t> out;
    out.reserve(produce);
    std::size_t at = 0;
    while (out.size() < produce && at < data.size()) {
        const auto control = data[at++];
        if (control < 0x80) {
            const std::size_t count = control + 1U;
            if (at + count > data.size()) return std::nullopt;
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + count));
            at += count;
        } else {
            if (at >= data.size()) return std::nullopt;
            out.insert(out.end(), static_cast<std::size_t>(control) - 125U, data[at++]);
        }
    }
    if (out.size() < produce) return std::nullopt;
    out.resize(produce);
    return out;
}
std::uint32_t rle_size(std::string_view type) {
    if (type == "is32") return 16;
    if (type == "il32") return 32;
    if (type == "ih32") return 48;
    if (type == "it32") return 128;
    return 0;
}
std::string mask_type(std::string_view type) {
    if (type == "is32") return "s8mk";
    if (type == "il32") return "l8mk";
    if (type == "ih32") return "h8mk";
    if (type == "it32") return "t8mk";
    return {};
}
bool icns_image_type(std::string_view type) {
    static constexpr std::string_view types[] = {"ic04", "ic05", "ic07", "ic08", "ic09", "ic10", "ic11", "ic12", "ic13", "ic14",
                                                  "icp4", "icp5", "icp6", "icsb", "icsB", "sb24", "SB24", "is32", "il32", "ih32", "it32"};
    return std::find(std::begin(types), std::end(types), type) != std::end(types);
}

std::optional<Frame> icns_decode(const std::vector<IcnsEntry>& entries, const IcnsEntry& entry, bool pixels) {
    const auto data = entry.data;
    if (starts(data, {0x89, 'P', 'N', 'G'})) {
        auto frame = wic_frame(data, 0, pixels);
        if (frame) frame->has_alpha = true;
        return frame;
    }
    if (starts(data, {0, 0, 0, 0x0c, 'j', 'P'}) || starts(data, {0xff, 0x4f, 0xff, 0x51})) return std::nullopt;   // JPEG 2000
    if (starts(data, {'A', 'R', 'G', 'B'})) {
        const std::uint32_t side = entry.type == "ic04" ? 16 : entry.type == "ic05" ? 32 : entry.type == "icsb" ? 18 : 0;
        if (!side) return std::nullopt;
        Frame frame;
        frame.width = frame.height = side;
        frame.has_alpha = true;
        if (!pixels) return frame;
        const auto planes = icns_unpack(data.subspan(4), static_cast<std::size_t>(side) * side * 4);
        if (!planes) return std::nullopt;
        const std::size_t n = static_cast<std::size_t>(side) * side;
        frame.rgba.resize(n * 4);
        for (std::size_t index = 0; index < n; ++index) {
            frame.rgba[index * 4 + 0] = (*planes)[n + index];
            frame.rgba[index * 4 + 1] = (*planes)[2 * n + index];
            frame.rgba[index * 4 + 2] = (*planes)[3 * n + index];
            frame.rgba[index * 4 + 3] = (*planes)[index];
        }
        return frame;
    }
    const auto side = rle_size(entry.type);
    if (!side) return std::nullopt;
    Frame frame;
    frame.width = frame.height = side;
    const auto mask_name = mask_type(entry.type);
    const auto mask = std::find_if(entries.begin(), entries.end(), [&](const auto& item) { return item.type == mask_name; });
    const std::size_t n = static_cast<std::size_t>(side) * side;
    frame.has_alpha = mask != entries.end() && mask->data.size() >= n;
    if (!pixels) return frame;
    auto payload = data;
    if (entry.type == "it32" && payload.size() >= 4) payload = payload.subspan(4);
    // Small entries may be stored uncompressed (exactly 3 * n bytes).
    std::optional<std::vector<std::uint8_t>> planes;
    if (payload.size() == 3 * n) planes = std::vector<std::uint8_t>(payload.begin(), payload.end());
    else planes = icns_unpack(payload, 3 * n);
    if (!planes) return std::nullopt;
    frame.rgba.resize(n * 4);
    for (std::size_t index = 0; index < n; ++index) {
        frame.rgba[index * 4 + 0] = (*planes)[index];
        frame.rgba[index * 4 + 1] = (*planes)[n + index];
        frame.rgba[index * 4 + 2] = (*planes)[2 * n + index];
        frame.rgba[index * 4 + 3] = frame.has_alpha ? mask->data[index] : 0xff;
    }
    return frame;
}
// Decodable image entries, in file order.
std::vector<IcnsEntry> icns_images(std::span<const std::uint8_t> bytes) {
    const auto entries = icns_entries(bytes);
    std::vector<IcnsEntry> images;
    for (const auto& entry : entries)
        if (icns_image_type(entry.type) && icns_decode(entries, entry, false)) images.push_back(entry);
    return images;
}

} // namespace

std::optional<std::string> sniff(std::span<const std::uint8_t> bytes) {
    if (starts(bytes, {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a})) return png;
    if (starts(bytes, {0xff, 0xd8, 0xff})) return jpeg;
    if (starts(bytes, {'G', 'I', 'F', '8'})) return gif;
    if (starts(bytes, {'B', 'M'})) return bmp;
    if (starts(bytes, {'I', 'I', 0x2a, 0}) || starts(bytes, {'M', 'M', 0, 0x2a})) return tiff;
    if (starts(bytes, {0, 0, 1, 0})) return ico;
    if (starts(bytes, {'i', 'c', 'n', 's'})) return icns;
    return std::nullopt;
}

std::size_t frame_count(std::span<const std::uint8_t> bytes) {
    const auto type = sniff(bytes);
    if (!type) return 0;
    if (*type == icns) return icns_images(bytes).size();
    return wic_count(bytes);
}

std::optional<Frame> decode(std::span<const std::uint8_t> bytes, std::size_t index) {
    const auto type = sniff(bytes);
    if (!type) return std::nullopt;
    if (*type == icns) {
        const auto entries = icns_entries(bytes);
        const auto images = icns_images(bytes);
        if (index >= images.size()) return std::nullopt;
        return icns_decode(entries, images[index], true);
    }
    return wic_frame(bytes, index, true);
}

std::optional<Frame> describe(std::span<const std::uint8_t> bytes, std::size_t index) {
    const auto type = sniff(bytes);
    if (!type) return std::nullopt;
    if (*type == icns) {
        const auto entries = icns_entries(bytes);
        const auto images = icns_images(bytes);
        if (index >= images.size()) return std::nullopt;
        return icns_decode(entries, images[index], false);
    }
    return wic_frame(bytes, index, false);
}

std::optional<std::vector<std::uint8_t>> encode(const std::string& type, const std::vector<Frame>& frames, double quality) {
    if (frames.empty()) return std::nullopt;
    host::gpu::ImageFormat format{};
    if (type == png) format = host::gpu::ImageFormat::png;
    else if (type == jpeg) format = host::gpu::ImageFormat::jpeg;
    else if (type == tiff) format = host::gpu::ImageFormat::tiff;
    else if (type == bmp) format = host::gpu::ImageFormat::bmp;
    else if (type == gif) format = host::gpu::ImageFormat::gif;
    else return std::nullopt;
    // TIFF and GIF keep every frame; the other formats write the first.
    const auto count = (type == tiff || type == gif) ? frames.size() : 1;
    std::vector<host::gpu::Pixels> pixels;
    for (std::size_t index = 0; index < count; ++index) pixels.push_back({frames[index].width, frames[index].height, frames[index].rgba});
    auto encoded = host::gpu::encode_image_frames(format, pixels, quality);
    if (!encoded) return std::nullopt;
    return std::move(*encoded);
}

std::vector<std::string> readable_types() { return {png, jpeg, tiff, gif, bmp, ico, icns}; }
std::vector<std::string> writable_types() { return {png, jpeg, tiff, gif, bmp}; }

} // namespace alchemy::gui::quartz2d::codec
