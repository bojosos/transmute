#include "alchemy/darwin/media/media.hpp"

#include "alchemy/darwin/media/cmtime.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

// CoreMedia (real): CMTime and CMTimeRange arithmetic (core:
// kit/completion/media/cmtime.cpp), CMBlockBuffer (one contiguous memory block),
// CMFormatDescription (video descriptions, H.264 parameter sets with the
// picture size read from the SPS, audio descriptions with their stream
// description and channel layout), CMSampleBuffer (data buffer, format,
// timing, sizes, per-sample attachment dictionaries, audio buffer lists,
// per-sample iteration) and the documented constant keys. Image buffers (CVPixelBuffer) cannot exist under Alchemy, so
// the image-buffer constructors report a missing required parameter.
namespace alchemy::gui::fill {

namespace {

// Bit reader over an RBSP (emulation-prevention bytes removed).
struct Bits {
    std::vector<std::uint8_t> bytes;
    std::size_t position{};
    bool failed{};
    std::uint32_t bit() {
        if (position >= bytes.size() * 8) { failed = true; return 0; }
        const auto value = (bytes[position / 8] >> (7 - position % 8)) & 1U;
        ++position;
        return value;
    }
    std::uint32_t read(unsigned count) { std::uint32_t value = 0; while (count--) value = (value << 1) | bit(); return value; }
    std::uint32_t ue() {
        unsigned zeros = 0;
        while (!bit()) { if (failed || ++zeros > 31) { failed = true; return 0; } }
        return (zeros ? ((1U << zeros) - 1 + read(zeros)) : 0);
    }
    std::int32_t se() { const auto k = ue(); return (k & 1) ? static_cast<std::int32_t>((k + 1) / 2) : -static_cast<std::int32_t>(k / 2); }
};

} // namespace

std::optional<std::pair<std::int32_t, std::int32_t>> h264_sps_dimensions(std::span<const std::uint8_t> nal) {
    if (nal.size() < 4 || (nal[0] & 0x1f) != 7) return std::nullopt;
    Bits b;
    for (std::size_t i = 1, zeros = 0; i < nal.size(); ++i) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        b.bytes.push_back(nal[i]);
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    const auto profile = b.read(8);
    b.read(16);   // constraint flags, level
    b.ue();       // seq_parameter_set_id
    std::uint32_t chroma_format = 1;
    bool separate_colour_plane = false;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 ||
        profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        chroma_format = b.ue();
        if (chroma_format == 3) separate_colour_plane = b.bit();
        b.ue(); b.ue();   // bit depths
        b.bit();          // qpprime_y_zero_transform_bypass_flag
        if (b.bit()) {    // seq_scaling_matrix_present_flag
            for (int list = 0; list < (chroma_format != 3 ? 8 : 12); ++list) {
                if (!b.bit()) continue;
                const int size = list < 6 ? 16 : 64;
                int last = 8, next = 8;
                for (int j = 0; j < size; ++j) {
                    if (next != 0) next = (last + b.se() + 256) % 256;
                    last = next == 0 ? last : next;
                }
            }
        }
    }
    b.ue();   // log2_max_frame_num_minus4
    const auto poc_type = b.ue();
    if (poc_type == 0) b.ue();
    else if (poc_type == 1) {
        b.bit(); b.se(); b.se();
        const auto cycle = b.ue();
        if (cycle > 255) return std::nullopt;
        for (std::uint32_t i = 0; i < cycle; ++i) b.se();
    }
    b.ue();   // max_num_ref_frames
    b.bit();  // gaps_in_frame_num_value_allowed_flag
    const auto width_mbs = b.ue() + 1, height_units = b.ue() + 1;
    const auto frame_mbs_only = b.bit();
    if (!frame_mbs_only) b.bit();
    b.bit();  // direct_8x8_inference_flag
    std::uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (b.bit()) { crop_left = b.ue(); crop_right = b.ue(); crop_top = b.ue(); crop_bottom = b.ue(); }
    if (b.failed || width_mbs > 1024 || height_units > 1024) return std::nullopt;
    const auto array_type = separate_colour_plane ? 0U : chroma_format;
    const std::uint32_t crop_x = array_type == 0 ? 1 : (chroma_format == 3 ? 1 : 2);
    const std::uint32_t crop_y = (array_type == 0 ? 1 : (chroma_format == 1 ? 2 : 1)) * (2 - frame_mbs_only);
    const auto width = static_cast<std::int64_t>(width_mbs) * 16 - static_cast<std::int64_t>(crop_x) * (crop_left + crop_right);
    const auto height = static_cast<std::int64_t>(height_units) * 16 * (2 - frame_mbs_only) -
        static_cast<std::int64_t>(crop_y) * (crop_top + crop_bottom);
    if (width <= 0 || height <= 0) return std::nullopt;
    return std::pair{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

namespace {

// <CoreMedia/CMBlockBuffer.h>, <CMFormatDescription.h>, <CMSampleBuffer.h>
constexpr std::int32_t bb_structure_allocation_failed = -12700, bb_block_allocation_failed = -12701,
    bb_bad_custom_block_source = -12702, bb_bad_offset = -12703, bb_bad_length = -12704, bb_bad_pointer = -12705;
constexpr std::int32_t fd_invalid_parameter = -12710;
constexpr std::int32_t sb_required_parameter_missing = -12731;
constexpr std::uint32_t cmtime_valid = 1, cmtime_positive_infinity = 4, cmtime_negative_infinity = 8, cmtime_indefinite = 16;

struct BlockBuffer final : ObjectPayload {
    std::mutex mutex;
    Address memory{};
    bool owned{};             // freed with free() when the buffer is deallocated
    std::uint64_t block_length{}, offset{}, data_length{};
    Address parent{};         // retained block buffer whose memory this one refers to
};
struct FormatDescription final : ObjectPayload {
    std::uint32_t media_type{}, subtype{};
    std::int32_t width{}, height{};
    Address extensions{};                     // retained CFDictionary or 0
    std::vector<std::vector<std::uint8_t>> parameter_sets;
    std::vector<Address> parameter_set_copies; // guest copies (process lifetime)
    std::int32_t nal_header_length{};
    Address stream_description{};             // audio: guest AudioStreamBasicDescription (40 bytes)
    Address channel_layout{};                 // audio: guest AudioChannelLayout copy or 0
    std::uint64_t channel_layout_size{};
};
struct SampleBuffer final : ObjectPayload {
    std::mutex mutex;
    Address data{}, format{}, image{};         // retained
    std::int64_t samples{};
    std::vector<std::uint8_t> timing;          // CMSampleTimingInfo entries (72 bytes each)
    std::vector<std::uint64_t> sizes;
    Address attachments{};                     // retained CFArray of mutable dictionaries
};

class CoreMedia final : public Provider {
public:
    using Provider::Provider;
    Address block_class{}, format_class{}, sample_class{};
    Address null_allocator{UINT64_MAX};

    template <class T>
    std::shared_ptr<T> state(Address object, const char* what) {
        const auto value = object ? objc.find_host_state<T>(object) : nullptr;
        if (!value) throw std::runtime_error(std::string("Alchemy: the object is not a ") + what);
        return value;
    }
    Address native_class(const char* name) {
        auto value = objc.find_class(name);
        if (!value) value = objc.register_native_class(name, objc.find_class("NSObject"), 8);
        return value;
    }
    template <class T>
    Address make(Address target, std::shared_ptr<T> value) {
        const auto object = objc.allocate(target);
        static_cast<void>(objc.host_state<T>(object, [&] { return value; }));
        return object;
    }
    static void write_time(GuestMemory& m, Address out, std::int64_t value, std::int32_t timescale, std::uint32_t flags, std::int64_t epoch) {
        m.write64(out, static_cast<std::uint64_t>(value));
        m.write32(out + 8, static_cast<std::uint32_t>(timescale));
        m.write32(out + 12, flags);
        m.write64(out + 16, static_cast<std::uint64_t>(epoch));
    }
    static completion_kit::media::CMTime read_cmtime(GuestMemory& m, Address in) {
        return {static_cast<std::int64_t>(m.read64(in)), static_cast<std::int32_t>(m.read32(in + 8)), m.read32(in + 12),
                static_cast<std::int64_t>(m.read64(in + 16))};
    }
    static void write_cmtime(GuestMemory& m, Address out, const completion_kit::media::CMTime& time) {
        write_time(m, out, time.value, time.timescale, time.flags, time.epoch);
    }
    void allocate_block(BlockBuffer& buffer) {
        if (buffer.memory || !buffer.block_length) return;
        buffer.memory = malloc(buffer.block_length);
        buffer.owned = true;
    }
    Address create_block(Address memory, std::uint64_t block_length, Address block_allocator, std::uint64_t offset,
                         std::uint64_t data_length, std::uint32_t flags, std::int32_t& error) {
        if (offset > block_length || data_length > block_length - offset) { error = bb_bad_length; return 0; }
        auto value = std::make_shared<BlockBuffer>();
        value->memory = memory;
        value->owned = memory && block_allocator != null_allocator;
        value->block_length = block_length;
        value->offset = offset;
        value->data_length = data_length;
        if (flags & 1U) allocate_block(*value);   // kCMBlockBufferAssureMemoryNowFlag
        error = 0;
        return make(block_class, value);
    }
    void install();
    void install_time();
    void install_block_buffers();
    void install_format_descriptions();
    void install_sample_buffers();
    void install_keys();
    void install_audio_formats();
    void install_sample_buffer_access();
    std::int32_t subdivide(Address buffer, std::int64_t index, Address& out);
};

void CoreMedia::install_time() {
    // CMTime CMTimeMake(int64_t value, int32_t timescale): returned through x8.
    raw(core_media, "_CMTimeMake", [](CpuState& s, GuestMemory& m) {
        const auto timescale = static_cast<std::int32_t>(s.x[1]);
        write_time(m, s.x[8], static_cast<std::int64_t>(s.x[0]), timescale, timescale > 0 ? cmtime_valid : 0, 0);
    });
    // Float64 CMTimeGetSeconds(CMTime): the 24-byte CMTime is passed by reference.
    raw(core_media, "_CMTimeGetSeconds", [](CpuState& s, GuestMemory& m) {
        const auto value = static_cast<std::int64_t>(m.read64(s.x[0]));
        const auto timescale = static_cast<std::int32_t>(m.read32(s.x[0] + 8));
        const auto flags = m.read32(s.x[0] + 12);
        double seconds = std::numeric_limits<double>::quiet_NaN();
        if (flags & cmtime_valid) {
            if (flags & cmtime_indefinite) seconds = std::numeric_limits<double>::quiet_NaN();
            else if (flags & cmtime_positive_infinity) seconds = std::numeric_limits<double>::infinity();
            else if (flags & cmtime_negative_infinity) seconds = -std::numeric_limits<double>::infinity();
            else if (timescale > 0) seconds = static_cast<double>(value) / timescale;
        }
        return_real(s, seconds);
    });
    // kCMTimeInvalid: all zero.
    const std::array<std::uint8_t, 24> invalid{};
    data(core_media, "_kCMTimeInvalid", invalid, 8);
    namespace cm = completion_kit::media;
    for (const auto& [symbol, time] : std::array<std::pair<const char*, cm::CMTime>, 4>{{
             {"_kCMTimeZero", cm::time_zero}, {"_kCMTimePositiveInfinity", cm::time_positive_infinity},
             {"_kCMTimeNegativeInfinity", cm::time_negative_infinity}, {"_kCMTimeIndefinite", cm::time_indefinite}}}) {
        std::array<std::uint8_t, 24> bytes{};
        std::memcpy(bytes.data(), &time.value, 8);
        std::memcpy(bytes.data() + 8, &time.timescale, 4);
        std::memcpy(bytes.data() + 12, &time.flags, 4);
        std::memcpy(bytes.data() + 16, &time.epoch, 8);
        data(core_media, symbol, bytes, 8);
    }
    // CMTime arguments (24 bytes) arrive by reference; CMTime/CMTimeRange results go through x8.
    raw(core_media, "_CMTimeAdd", [](CpuState& s, GuestMemory& m) {
        write_cmtime(m, s.x[8], cm::add(read_cmtime(m, s.x[0]), read_cmtime(m, s.x[1])));
    });
    raw(core_media, "_CMTimeSubtract", [](CpuState& s, GuestMemory& m) {
        write_cmtime(m, s.x[8], cm::subtract(read_cmtime(m, s.x[0]), read_cmtime(m, s.x[1])));
    });
    raw(core_media, "_CMTimeCompare", [](CpuState& s, GuestMemory& m) {
        s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(cm::compare(read_cmtime(m, s.x[0]), read_cmtime(m, s.x[1]))));
    });
    // CMTime CMTimeMakeWithSeconds(Float64 seconds, int32_t preferredTimescale)
    raw(core_media, "_CMTimeMakeWithSeconds", [](CpuState& s, GuestMemory& m) {
        write_cmtime(m, s.x[8], cm::make_with_seconds(real(s, 0), static_cast<std::int32_t>(s.x[0])));
    });
    raw(core_media, "_CMTimeMultiply", [](CpuState& s, GuestMemory& m) {
        write_cmtime(m, s.x[8], cm::multiply(read_cmtime(m, s.x[0]), static_cast<std::int32_t>(s.x[1])));
    });
    raw(core_media, "_CMTimeMultiplyByFloat64", [](CpuState& s, GuestMemory& m) {
        write_cmtime(m, s.x[8], cm::multiply_by_float64(read_cmtime(m, s.x[0]), real(s, 0)));
    });
    raw(core_media, "_CMTimeConvertScale", [](CpuState& s, GuestMemory& m) {
        // (time, int32_t newTimescale, CMTimeRoundingMethod): only the default rounding is provided.
        const auto method = static_cast<std::uint32_t>(s.x[2]);
        if (method != 0 && method != 1)
            throw std::runtime_error("Alchemy: CMTimeConvertScale rounding method " + std::to_string(method) + " is unsupported");
        write_cmtime(m, s.x[8], cm::convert_scale(read_cmtime(m, s.x[0]), static_cast<std::int32_t>(s.x[1])));
    });
    // CFStringRef CMTimeCopyDescription(CFAllocatorRef, CMTime): +1.
    raw(core_media, "_CMTimeCopyDescription", [this](CpuState& s, GuestMemory& m) {
        s.x[0] = cf.string(cm::description(read_cmtime(m, s.x[1])));
    });
    raw(core_media, "_CMTimeRangeMake", [](CpuState& s, GuestMemory& m) {
        const auto range = cm::range_make(read_cmtime(m, s.x[0]), read_cmtime(m, s.x[1]));
        write_cmtime(m, s.x[8], range.start);
        write_cmtime(m, s.x[8] + 24, range.duration);
    });
    // CMTime CMTimeRangeGetEnd(CMTimeRange): the 48-byte range arrives by reference.
    raw(core_media, "_CMTimeRangeGetEnd", [](CpuState& s, GuestMemory& m) {
        write_cmtime(m, s.x[8], cm::range_end({read_cmtime(m, s.x[0]), read_cmtime(m, s.x[0] + 24)}));
    });
}

void CoreMedia::install_block_buffers() {
    block_class = native_class("__AlchemyCMBlockBuffer");
    register_type(block_class, TypeId::block_buffer);
    if (!has(core_media, "_CMBlockBufferCreateWithMemoryBlock")) {
        static_cast<void>(objc.add_native_method(block_class, "dealloc", [self = shared_from_this(), this](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<BlockBuffer>(s.x[0])) {
                Address memory{};
                Address parent{};
                {
                    std::lock_guard lock(value->mutex);
                    if (value->owned) memory = std::exchange(value->memory, 0);
                    parent = std::exchange(value->parent, 0);
                }
                free(memory);
                release(parent);
            }
            static_cast<void>(objc.send_super(s.x[0], block_class, objc.selector("dealloc")));
        }, false, "v@:"));
    }
    // OSStatus CMBlockBufferCreateWithMemoryBlock(structureAllocator, memoryBlock, blockLength,
    //     blockAllocator, customBlockSource, offsetToData, dataLength, flags, CMBlockBufferRef *out [stack])
    raw(core_media, "_CMBlockBufferCreateWithMemoryBlock", [this](CpuState& s, GuestMemory& m) {
        const auto out = m.read64(s.sp);
        if (!out) { s.x[0] = status(bb_bad_pointer); return; }
        m.write64(out, 0);
        if (s.x[4]) { s.x[0] = status(bb_bad_custom_block_source); return; }   // custom block sources are unsupported
        std::int32_t error{};
        const auto buffer = create_block(s.x[1], s.x[2], s.x[3], s.x[5], s.x[6], static_cast<std::uint32_t>(s.x[7]), error);
        if (buffer) m.write64(out, buffer);
        s.x[0] = status(error);
    });
    raw(core_media, "_CMBlockBufferGetDataLength", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->data_length;
    });
    raw(core_media, "_CMBlockBufferAssureBlockMemory", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        allocate_block(*value);
        s.x[0] = value->memory || !value->block_length ? 0 : status(bb_block_allocation_failed);
    });
    // CMBlockBufferGetDataPointer(buffer, offset, size_t *lengthAtOffset, size_t *totalLength, char **dataPointer)
    raw(core_media, "_CMBlockBufferGetDataPointer", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        const auto offset = s.x[1];
        if (offset > value->data_length || (offset == value->data_length && value->data_length)) { s.x[0] = status(bb_bad_offset); return; }
        allocate_block(*value);
        if (s.x[2]) m.write64(s.x[2], value->data_length - offset);
        if (s.x[3]) m.write64(s.x[3], value->data_length);
        if (s.x[4]) m.write64(s.x[4], value->memory ? value->memory + value->offset + offset : 0);
        s.x[0] = 0;
    });
    raw(core_media, "_CMBlockBufferIsRangeContiguous", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        const auto offset = s.x[1], length = s.x[2] ? s.x[2] : value->data_length - std::min(offset, value->data_length);
        s.x[0] = offset <= value->data_length && length <= value->data_length - offset;   // one memory block
    });
    // CMBlockBufferReplaceDataBytes(sourceBytes, destinationBuffer, offsetIntoDestination, dataLength)
    raw(core_media, "_CMBlockBufferReplaceDataBytes", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<BlockBuffer>(s.x[1], "CMBlockBuffer");
        if (!s.x[0] && s.x[3]) { s.x[0] = status(bb_bad_pointer); return; }
        std::lock_guard lock(value->mutex);
        if (s.x[2] > value->data_length || s.x[3] > value->data_length - s.x[2]) { s.x[0] = status(bb_bad_length); return; }
        allocate_block(*value);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[3]));
        if (!bytes.empty()) { m.read_bytes(s.x[0], bytes); m.write_bytes(value->memory + value->offset + s.x[2], bytes); }
        s.x[0] = 0;
    });
    // CMBlockBufferCreateContiguous(structureAllocator, sourceBuffer, blockAllocator, customBlockSource,
    //     offsetToData, dataLength, flags, CMBlockBufferRef *out): a contiguous copy.
    raw(core_media, "_CMBlockBufferCreateContiguous", [this](CpuState& s, GuestMemory& m) {
        const auto out = s.x[7];
        if (!out) { s.x[0] = status(bb_bad_pointer); return; }
        m.write64(out, 0);
        if (s.x[3]) { s.x[0] = status(bb_bad_custom_block_source); return; }
        const auto source = state<BlockBuffer>(s.x[1], "CMBlockBuffer");
        std::vector<std::uint8_t> bytes;
        {
            std::lock_guard lock(source->mutex);
            const auto offset = s.x[4];
            if (offset > source->data_length) { s.x[0] = status(bb_bad_offset); return; }
            const auto length = s.x[5] ? s.x[5] : source->data_length - offset;
            if (length > source->data_length - offset) { s.x[0] = status(bb_bad_length); return; }
            allocate_block(*source);
            bytes.resize(static_cast<std::size_t>(length));
            if (length) m.read_bytes(source->memory + source->offset + offset, bytes);
        }
        const auto memory = bytes.empty() ? 0 : malloc(bytes.size());
        if (!bytes.empty()) m.write_bytes(memory, bytes);
        std::int32_t error{};
        const auto buffer = create_block(memory, bytes.size(), 0, 0, bytes.size(), 0, error);
        if (!buffer) { free(memory); s.x[0] = status(error ? error : bb_structure_allocation_failed); return; }
        m.write64(out, buffer);
        s.x[0] = 0;
    });
}

void CoreMedia::install_format_descriptions() {
    format_class = native_class("__AlchemyCMFormatDescription");
    register_type(format_class, TypeId::format_description);
    if (!has(core_media, "_CMVideoFormatDescriptionCreate")) {
        static_cast<void>(objc.add_native_method(format_class, "dealloc", [self = shared_from_this(), this](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<FormatDescription>(s.x[0])) {
                release(std::exchange(value->extensions, 0));
                for (const auto copy : value->parameter_set_copies) free(copy);
                value->parameter_set_copies.clear();
                free(std::exchange(value->stream_description, 0));
                free(std::exchange(value->channel_layout, 0));
            }
            static_cast<void>(objc.send_super(s.x[0], format_class, objc.selector("dealloc")));
        }, false, "v@:"));
    }
    // CMVideoFormatDescriptionCreate(allocator, codecType, width, height, extensions, CMVideoFormatDescriptionRef *out)
    raw(core_media, "_CMVideoFormatDescriptionCreate", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[5]) { s.x[0] = status(fd_invalid_parameter); return; }
        auto value = std::make_shared<FormatDescription>();
        value->media_type = fourcc("vide");
        value->subtype = static_cast<std::uint32_t>(s.x[1]);
        value->width = static_cast<std::int32_t>(s.x[2]);
        value->height = static_cast<std::int32_t>(s.x[3]);
        value->extensions = s.x[4] ? cf.copy(s.x[4]) : 0;
        m.write64(s.x[5], make(format_class, value));
        s.x[0] = 0;
    });
    // CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator, count, const uint8_t *const *sets,
    //     const size_t *sizes, int NALUnitHeaderLength, CMFormatDescriptionRef *out)
    raw(core_media, "_CMVideoFormatDescriptionCreateFromH264ParameterSets", [this](CpuState& s, GuestMemory& m) {
        const auto count = s.x[1];
        const auto header = static_cast<std::int32_t>(s.x[4]);
        if (s.x[5]) m.write64(s.x[5], 0);
        if (!s.x[5] || count < 2 || count > 64 || !s.x[2] || !s.x[3] || (header != 1 && header != 2 && header != 4)) {
            s.x[0] = status(fd_invalid_parameter);
            return;
        }
        auto value = std::make_shared<FormatDescription>();
        value->media_type = fourcc("vide");
        value->subtype = fourcc("avc1");
        value->nal_header_length = header;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto pointer = m.read64(s.x[2] + i * 8), size = m.read64(s.x[3] + i * 8);
            if (!pointer || !size || size > (1 << 16)) { s.x[0] = status(fd_invalid_parameter); return; }
            std::vector<std::uint8_t> set(static_cast<std::size_t>(size));
            m.read_bytes(pointer, set);
            value->parameter_sets.push_back(std::move(set));
        }
        const auto dimensions = h264_sps_dimensions(value->parameter_sets.front());
        if (!dimensions) { s.x[0] = status(fd_invalid_parameter); return; }
        value->width = dimensions->first;
        value->height = dimensions->second;
        for (const auto& set : value->parameter_sets) {
            const auto copy = malloc(set.size());
            m.write_bytes(copy, set);
            value->parameter_set_copies.push_back(copy);
        }
        m.write64(s.x[5], make(format_class, value));
        s.x[0] = 0;
    });
    // No image buffer can exist.
    raw(core_media, "_CMVideoFormatDescriptionCreateForImageBuffer", [](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        s.x[0] = status(fd_invalid_parameter);
    });
    raw(core_media, "_CMFormatDescriptionGetExtension", [this](CpuState& s, GuestMemory&) {
        const auto value = state<FormatDescription>(s.x[0], "CMFormatDescription");
        s.x[0] = value->extensions && s.x[1] ? cf.dictionary_value(value->extensions, s.x[1]) : 0;
    });
    raw(core_media, "_CMFormatDescriptionGetMediaSubType", [this](CpuState& s, GuestMemory&) {
        s.x[0] = state<FormatDescription>(s.x[0], "CMFormatDescription")->subtype;
    });
    // CMVideoDimensions {int32 width, int32 height} in x0.
    raw(core_media, "_CMVideoFormatDescriptionGetDimensions", [this](CpuState& s, GuestMemory&) {
        const auto value = state<FormatDescription>(s.x[0], "CMFormatDescription");
        s.x[0] = static_cast<std::uint32_t>(value->width) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(value->height)) << 32);
    });
    // CGRect CMVideoFormatDescriptionGetCleanAperture(desc, originIsAtTopLeft): the
    // full picture (no clean-aperture extension is interpreted).
    raw(core_media, "_CMVideoFormatDescriptionGetCleanAperture", [this](CpuState& s, GuestMemory&) {
        const auto value = state<FormatDescription>(s.x[0], "CMFormatDescription");
        return_real(s, 0, 0); return_real(s, 0, 1);
        return_real(s, value->width, 2); return_real(s, value->height, 3);
    });
    // CMVideoFormatDescriptionGetH264ParameterSetAtIndex(desc, index, const uint8_t **set,
    //     size_t *size, size_t *count, int *NALUnitHeaderLength)
    raw(core_media, "_CMVideoFormatDescriptionGetH264ParameterSetAtIndex", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<FormatDescription>(s.x[0], "CMFormatDescription");
        if (value->parameter_sets.empty()) { s.x[0] = status(fd_invalid_parameter); return; }
        if (s.x[4]) m.write64(s.x[4], value->parameter_sets.size());
        if (s.x[5]) m.write32(s.x[5], static_cast<std::uint32_t>(value->nal_header_length));
        if (s.x[2] || s.x[3]) {
            if (s.x[1] >= value->parameter_sets.size()) { s.x[0] = status(fd_invalid_parameter); return; }
            const auto index = static_cast<std::size_t>(s.x[1]);
            if (s.x[2]) m.write64(s.x[2], value->parameter_set_copies[index]);
            if (s.x[3]) m.write64(s.x[3], value->parameter_sets[index].size());
        }
        s.x[0] = 0;
    });
}

void CoreMedia::install_sample_buffers() {
    sample_class = native_class("__AlchemyCMSampleBuffer");
    register_type(sample_class, TypeId::sample_buffer);
    if (!has(core_media, "_CMSampleBufferCreate")) {
        static_cast<void>(objc.add_native_method(sample_class, "dealloc", [self = shared_from_this(), this](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<SampleBuffer>(s.x[0])) {
                release(std::exchange(value->data, 0));
                release(std::exchange(value->format, 0));
                release(std::exchange(value->image, 0));
                release(std::exchange(value->attachments, 0));
            }
            static_cast<void>(objc.send_super(s.x[0], sample_class, objc.selector("dealloc")));
        }, false, "v@:"));
    }
    const auto create = [this](GuestMemory& m, Address data_buffer, Address format, std::int64_t samples, std::uint64_t timing_count,
                               Address timing, std::uint64_t size_count, Address sizes, Address out) -> std::int32_t {
        if (!out) return sb_required_parameter_missing;
        m.write64(out, 0);
        if (samples < 0 || timing_count > 1'000'000 || size_count > 1'000'000) return -12738;   // kCMSampleBufferError_InvalidEntryCount
        auto value = std::make_shared<SampleBuffer>();
        value->data = retain(data_buffer);
        value->format = retain(format);
        value->samples = samples;
        value->timing.resize(static_cast<std::size_t>(timing_count * 72));
        if (timing_count && timing) m.read_bytes(timing, value->timing);
        for (std::uint64_t i = 0; i < size_count; ++i) value->sizes.push_back(m.read64(sizes + i * 8));
        m.write64(out, make(sample_class, value));
        return 0;
    };
    // CMSampleBufferCreate(allocator, dataBuffer, dataReady, makeDataReadyCallback, refcon, formatDescription,
    //     numSamples, numSampleTimingEntries, [stack] sampleTimingArray, numSampleSizeEntries, sampleSizeArray, sampleBufferOut)
    raw(core_media, "_CMSampleBufferCreate", [create](CpuState& s, GuestMemory& m) {
        s.x[0] = status(create(m, s.x[1], s.x[5], static_cast<std::int64_t>(s.x[6]), s.x[7], m.read64(s.sp), m.read64(s.sp + 8),
                               m.read64(s.sp + 16), m.read64(s.sp + 24)));
    });
    // CMSampleBufferCreateReady(allocator, dataBuffer, formatDescription, numSamples, numSampleTimingEntries,
    //     sampleTimingArray, numSampleSizeEntries, sampleSizeArray, [stack] sampleBufferOut)
    raw(core_media, "_CMSampleBufferCreateReady", [create](CpuState& s, GuestMemory& m) {
        s.x[0] = status(create(m, s.x[1], s.x[2], static_cast<std::int64_t>(s.x[3]), s.x[4], s.x[5], s.x[6], s.x[7], m.read64(s.sp)));
    });
    // CMSampleBufferCreateForImageBuffer(allocator, imageBuffer, dataReady, callback, refcon, formatDescription,
    //     sampleTiming, sampleBufferOut): no image buffer can exist.
    raw(core_media, "_CMSampleBufferCreateForImageBuffer", [](CpuState& s, GuestMemory& m) {
        if (s.x[7]) m.write64(s.x[7], 0);
        s.x[0] = status(sb_required_parameter_missing);
    });
    raw(core_media, "_CMSampleBufferGetDataBuffer", [this](CpuState& s, GuestMemory&) { s.x[0] = state<SampleBuffer>(s.x[0], "CMSampleBuffer")->data; });
    raw(core_media, "_CMSampleBufferGetFormatDescription", [this](CpuState& s, GuestMemory&) { s.x[0] = state<SampleBuffer>(s.x[0], "CMSampleBuffer")->format; });
    raw(core_media, "_CMSampleBufferGetImageBuffer", [this](CpuState& s, GuestMemory&) { s.x[0] = state<SampleBuffer>(s.x[0], "CMSampleBuffer")->image; });
    // CMTime CMSampleBufferGetPresentationTimeStamp(sbuf) through x8: the first timing entry's PTS.
    raw(core_media, "_CMSampleBufferGetPresentationTimeStamp", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<SampleBuffer>(s.x[0], "CMSampleBuffer");
        std::vector<std::uint8_t> time(24, 0);
        if (value->timing.size() >= 72) std::copy(value->timing.begin() + 24, value->timing.begin() + 48, time.begin());
        m.write_bytes(s.x[8], time);
    });
    // CFArrayRef CMSampleBufferGetSampleAttachmentsArray(sbuf, Boolean createIfNecessary)
    raw(core_media, "_CMSampleBufferGetSampleAttachmentsArray", [this](CpuState& s, GuestMemory&) {
        const auto value = state<SampleBuffer>(s.x[0], "CMSampleBuffer");
        std::lock_guard lock(value->mutex);
        if (!value->attachments && (s.x[1] & 0xff)) {
            std::vector<Address> dictionaries;
            for (std::int64_t i = 0; i < std::max<std::int64_t>(value->samples, 1); ++i)
                dictionaries.push_back(cf.dictionary({}, true));
            value->attachments = cf.array(dictionaries);
            for (const auto dictionary : dictionaries) release(dictionary);
        }
        s.x[0] = value->attachments;
    });
    // CMCopyDictionaryOfAttachments(allocator, target, mode): no buffer-level
    // attachment can be set (CMSetAttachment is not provided), so there is none.
    constant(core_media, "_CMCopyDictionaryOfAttachments", 0);
}

void CoreMedia::install_keys() {
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 26>{{
             {"_kCMFormatDescriptionColorPrimaries_DCI_P3", "DCI_P3"},
             {"_kCMFormatDescriptionColorPrimaries_ITU_R_2020", "ITU_R_2020"},
             {"_kCMFormatDescriptionColorPrimaries_P3_D65", "P3_D65"},
             {"_kCMFormatDescriptionExtension_ContentLightLevelInfo", "ContentLightLevelInfo"},
             {"_kCMFormatDescriptionExtension_Depth", "Depth"},
             {"_kCMFormatDescriptionExtension_FormatName", "FormatName"},
             {"_kCMFormatDescriptionExtension_FullRangeVideo", "FullRangeVideo"},
             {"_kCMFormatDescriptionExtension_MasteringDisplayColorVolume", "MasteringDisplayColorVolume"},
             {"_kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms", "SampleDescriptionExtensionAtoms"},
             {"_kCMFormatDescriptionTransferFunction_ITU_R_2020", "ITU_R_2020"},
             {"_kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG", "ITU_R_2100_HLG"},
             {"_kCMFormatDescriptionTransferFunction_Linear", "Linear"},
             {"_kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ", "SMPTE_ST_2084_PQ"},
             {"_kCMFormatDescriptionTransferFunction_SMPTE_ST_428_1", "SMPTE_ST_428_1"},
             {"_kCMFormatDescriptionTransferFunction_sRGB", "IEC_sRGB"},
             {"_kCMFormatDescriptionYCbCrMatrix_ITU_R_2020", "ITU_R_2020"},
             {"_kCMSampleAttachmentKey_DisplayImmediately", "DisplayImmediately"},
             {"_kCMSampleAttachmentKey_IsDependedOnByOthers", "IsDependedOnByOthers"},
             {"_kCMSampleAttachmentKey_NotSync", "NotSync"},
             {"_kCMSampleAttachmentKey_DependsOnOthers", "DependsOnOthers"},
             {"_kCMFormatDescriptionExtension_ContainsAlphaChannel", "ContainsAlphaChannel"},
             {"_kCMFormatDescriptionExtension_PixelAspectRatio", "CVPixelAspectRatio"},
             {"_kCMFormatDescriptionExtension_CleanAperture", "CVCleanAperture"},
             {"_kCMFormatDescriptionExtension_ColorPrimaries", "CVImageBufferColorPrimaries"},
             {"_kCMFormatDescriptionExtension_TransferFunction", "CVImageBufferTransferFunction"},
             {"_kCMFormatDescriptionExtension_YCbCrMatrix", "CVImageBufferYCbCrMatrix"}}})
        string(core_media, symbol, value);
}

void CoreMedia::install_audio_formats() {
    // CMAudioFormatDescriptionCreate(allocator, const AudioStreamBasicDescription *asbd, size_t layoutSize,
    //     const AudioChannelLayout *layout, size_t magicCookieSize, const void *magicCookie,
    //     CFDictionaryRef extensions, CMAudioFormatDescriptionRef *out)
    raw(core_media, "_CMAudioFormatDescriptionCreate", [this](CpuState& s, GuestMemory& m) {
        const auto out = s.x[7];
        if (out) m.write64(out, 0);
        // A layout holds at least its 12-byte header (tag, bitmap, description count).
        if (!out || !s.x[1] || (s.x[2] && (!s.x[3] || s.x[2] < 12 || s.x[2] > (1 << 16))) || (s.x[4] && !s.x[5])) {
            s.x[0] = status(fd_invalid_parameter);
            return;
        }
        // Magic cookies belong to compressed formats, which no Alchemy codec decodes.
        if (s.x[4]) throw std::runtime_error("Alchemy: CMAudioFormatDescriptionCreate with a magic cookie is unsupported (no audio codec exists)");
        auto value = std::make_shared<FormatDescription>();
        value->media_type = fourcc("soun");
        std::vector<std::uint8_t> asbd(40);
        m.read_bytes(s.x[1], asbd);
        std::uint32_t format_id{};
        std::memcpy(&format_id, asbd.data() + 8, 4);
        value->subtype = format_id;
        value->stream_description = malloc(40);
        m.write_bytes(value->stream_description, asbd);
        if (s.x[2]) {
            std::vector<std::uint8_t> layout(static_cast<std::size_t>(s.x[2]));
            m.read_bytes(s.x[3], layout);
            value->channel_layout = malloc(layout.size());
            m.write_bytes(value->channel_layout, layout);
            value->channel_layout_size = layout.size();
        }
        value->extensions = s.x[6] ? cf.copy(s.x[6]) : 0;
        m.write64(out, make(format_class, value));
        s.x[0] = 0;
    });
    const auto audio = [this](Address object) -> std::shared_ptr<FormatDescription> {
        const auto value = object ? objc.find_host_state<FormatDescription>(object) : nullptr;
        return value && value->media_type == fourcc("soun") ? value : nullptr;
    };
    // const AudioStreamBasicDescription *(desc): NULL when not an audio description.
    raw(core_media, "_CMAudioFormatDescriptionGetStreamBasicDescription", [audio](CpuState& s, GuestMemory&) {
        const auto value = audio(s.x[0]);
        s.x[0] = value ? value->stream_description : 0;
    });
    // const AudioChannelLayout *(desc, size_t *sizeOut)
    raw(core_media, "_CMAudioFormatDescriptionGetChannelLayout", [audio](CpuState& s, GuestMemory& m) {
        const auto value = audio(s.x[0]);
        if (s.x[1]) m.write64(s.x[1], value ? value->channel_layout_size : 0);
        s.x[0] = value ? value->channel_layout : 0;
    });
    raw(core_media, "_CMFormatDescriptionGetExtensions", [this](CpuState& s, GuestMemory&) {
        s.x[0] = state<FormatDescription>(s.x[0], "CMFormatDescription")->extensions;
    });
    raw(core_media, "_CMFormatDescriptionGetMediaType", [this](CpuState& s, GuestMemory&) {
        s.x[0] = state<FormatDescription>(s.x[0], "CMFormatDescription")->media_type;
    });
    // CGSize CMVideoFormatDescriptionGetPresentationDimensions(desc, Boolean usePixelAspectRatio,
    //     Boolean useCleanAperture): the coded size, widened by the pixel aspect ratio extension
    //     when asked. Clean-aperture extensions are not interpreted (as GetCleanAperture).
    raw(core_media, "_CMVideoFormatDescriptionGetPresentationDimensions", [this](CpuState& s, GuestMemory&) {
        const auto value = state<FormatDescription>(s.x[0], "CMFormatDescription");
        double width = value->width, height = value->height;
        if ((s.x[1] & 0xff) && value->extensions) {
            const auto key = cf.string("CVPixelAspectRatio");
            const auto ratio = cf.dictionary_value(value->extensions, key);
            release(key);
            if (ratio && bridge->is_dictionary(ratio)) {
                const auto horizontal_key = cf.string("HorizontalSpacing"), vertical_key = cf.string("VerticalSpacing");
                const auto horizontal = cf.dictionary_value(ratio, horizontal_key), vertical = cf.dictionary_value(ratio, vertical_key);
                release(horizontal_key);
                release(vertical_key);
                const auto h = horizontal ? bridge->real(horizontal) : std::nullopt, v = vertical ? bridge->real(vertical) : std::nullopt;
                if (h && v && *h > 0 && *v > 0) width = width * *h / *v;
            }
        }
        return_real(s, width, 0);
        return_real(s, height, 1);
    });
}

namespace {
completion_kit::media::CMTime timing_time(const std::vector<std::uint8_t>& timing, std::size_t at) {
    completion_kit::media::CMTime time{};
    std::memcpy(&time.value, timing.data() + at, 8);
    std::memcpy(&time.timescale, timing.data() + at + 8, 4);
    std::memcpy(&time.flags, timing.data() + at + 12, 4);
    std::memcpy(&time.epoch, timing.data() + at + 16, 8);
    return time;
}
void put_timing_time(std::vector<std::uint8_t>& timing, std::size_t at, const completion_kit::media::CMTime& time) {
    std::memcpy(timing.data() + at, &time.value, 8);
    std::memcpy(timing.data() + at + 8, &time.timescale, 4);
    std::memcpy(timing.data() + at + 12, &time.flags, 4);
    std::memcpy(timing.data() + at + 16, &time.epoch, 8);
}
} // namespace

// A sample buffer holding sample `index` of `buffer` (its timing, size and
// data range): CMSampleBufferCallForEachSample's temporary buffers.
std::int32_t CoreMedia::subdivide(Address buffer, std::int64_t index, Address& out) {
    namespace cm = completion_kit::media;
    const auto value = state<SampleBuffer>(buffer, "CMSampleBuffer");
    auto single = std::make_shared<SampleBuffer>();
    single->samples = 1;
    std::uint64_t offset{}, size{};
    Address data_buffer{};
    {
        std::lock_guard lock(value->mutex);
        if (value->sizes.size() == 1) {
            size = value->sizes[0];
            offset = size * static_cast<std::uint64_t>(index);
        } else if (value->sizes.size() > 1) {
            if (static_cast<std::uint64_t>(index) >= value->sizes.size()) return -12734;   // SampleIndexOutOfRange
            for (std::int64_t i = 0; i < index; ++i) offset += value->sizes[static_cast<std::size_t>(i)];
            size = value->sizes[static_cast<std::size_t>(index)];
        } else if (value->data) {
            return -12739;   // kCMSampleBufferError_CannotSubdivide: data without sample sizes
        }
        if (!value->sizes.empty()) single->sizes.push_back(size);
        const auto entries = value->timing.size() / 72;
        if (entries == 1) {
            single->timing = value->timing;
            const auto step = cm::multiply(timing_time(value->timing, 0), static_cast<std::int32_t>(index));
            for (const std::size_t at : {std::size_t{24}, std::size_t{48}})
                if (cm::is_valid(timing_time(value->timing, at))) put_timing_time(single->timing, at, cm::add(timing_time(value->timing, at), step));
        } else if (entries > 1) {
            if (static_cast<std::uint64_t>(index) >= entries) return -12734;
            const auto at = static_cast<std::ptrdiff_t>(index) * 72;
            single->timing.assign(value->timing.begin() + at, value->timing.begin() + at + 72);
        }
        data_buffer = value->data;
        single->format = retain(value->format);
    }
    if (data_buffer) {
        const auto source = state<BlockBuffer>(data_buffer, "CMBlockBuffer");
        std::lock_guard lock(source->mutex);
        if (offset > source->data_length || size > source->data_length - offset) {
            release(single->format);
            return -12742;   // kCMSampleBufferError_InvalidSampleData
        }
        allocate_block(*source);
        auto part = std::make_shared<BlockBuffer>();
        part->memory = source->memory;
        part->offset = source->offset + offset;
        part->block_length = source->block_length;
        part->data_length = size;
        part->parent = retain(data_buffer);
        single->data = make(block_class, part);
    }
    out = make(sample_class, single);
    return 0;
}

void CoreMedia::install_sample_buffer_access() {
    namespace cm = completion_kit::media;
    raw(core_media, "_CMSampleBufferGetNumSamples", [this](CpuState& s, GuestMemory&) {
        const auto value = state<SampleBuffer>(s.x[0], "CMSampleBuffer");
        std::lock_guard lock(value->mutex);
        s.x[0] = static_cast<std::uint64_t>(value->samples);
    });
    // Buffers are never invalidated (CMSampleBufferInvalidate is not provided).
    raw(core_media, "_CMSampleBufferIsValid", [this](CpuState& s, GuestMemory&) {
        static_cast<void>(state<SampleBuffer>(s.x[0], "CMSampleBuffer"));
        s.x[0] = 1;
    });
    // CMTime CMSampleBufferGetDuration(sbuf): a single timing entry covers every sample.
    raw(core_media, "_CMSampleBufferGetDuration", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<SampleBuffer>(s.x[0], "CMSampleBuffer");
        std::lock_guard lock(value->mutex);
        const auto entries = value->timing.size() / 72;
        cm::CMTime total = entries ? cm::time_zero : cm::time_invalid;
        for (std::size_t i = 0; i < entries; ++i) {
            const auto duration = timing_time(value->timing, i * 72);
            total = cm::add(total, entries == 1 ? cm::multiply(duration, static_cast<std::int32_t>(value->samples)) : duration);
        }
        write_cmtime(m, s.x[8], total);
    });
    // OSStatus CMSampleBufferCallForEachSample(sbuf, OSStatus (*callback)(CMSampleBufferRef, CMItemCount, void *), refcon)
    raw(core_media, "_CMSampleBufferCallForEachSample", [this](CpuState& s, GuestMemory&) {
        const auto buffer = s.x[0], callback = s.x[1], refcon = s.x[2];
        if (!callback) { s.x[0] = status(sb_required_parameter_missing); return; }
        const auto value = state<SampleBuffer>(buffer, "CMSampleBuffer");
        std::int64_t samples{};
        { std::lock_guard lock(value->mutex); samples = value->samples; }
        for (std::int64_t i = 0; i < samples; ++i) {
            Address single{};
            if (const auto error = subdivide(buffer, i, single)) { s.x[0] = status(error); return; }
            const std::array<Address, 3> arguments{single, static_cast<Address>(i), refcon};
            const auto result = static_cast<std::int32_t>(bridge->call_address(callback, arguments));
            release(single);
            if (result) { s.x[0] = status(result); return; }
        }
        s.x[0] = 0;
    });
    // OSStatus CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sbuf, size_t *bufferListSizeNeededOut,
    //     AudioBufferList *bufferListOut, size_t bufferListSize, CFAllocatorRef, CFAllocatorRef,
    //     uint32_t flags, CMBlockBufferRef *blockBufferOut)
    raw(core_media, "_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<SampleBuffer>(s.x[0], "CMSampleBuffer");
        const auto block_out = s.x[7];
        if (block_out) m.write64(block_out, 0);
        Address format{}, data_buffer{};
        { std::lock_guard lock(value->mutex); format = value->format; data_buffer = value->data; }
        const auto description = format ? objc.find_host_state<FormatDescription>(format) : nullptr;
        if (!description || description->media_type != fourcc("soun") || !description->stream_description) {
            s.x[0] = status(-12743);   // kCMSampleBufferError_InvalidMediaFormat
            return;
        }
        const auto asbd = description->stream_description;
        const auto flags = m.read32(asbd + 12), channels = m.read32(asbd + 28);
        const bool non_interleaved = (flags & (1U << 5)) != 0;   // kAudioFormatFlagIsNonInterleaved
        const std::uint32_t buffers = non_interleaved ? std::max<std::uint32_t>(channels, 1) : 1;
        const std::uint64_t needed = 8 + 16ULL * buffers;          // offsetof(mBuffers) + n * sizeof(AudioBuffer)
        if (s.x[1]) m.write64(s.x[1], needed);
        if (!s.x[2]) { s.x[0] = 0; return; }
        if (!block_out) { s.x[0] = status(sb_required_parameter_missing); return; }
        if (s.x[3] < needed) { s.x[0] = status(-12737); return; }  // kCMSampleBufferError_ArrayTooSmall
        if (!data_buffer) { s.x[0] = status(-12733); return; }     // kCMSampleBufferError_BufferNotReady (no data)
        const auto block = state<BlockBuffer>(data_buffer, "CMBlockBuffer");
        Address base{};
        std::uint64_t length{};
        {
            std::lock_guard lock(block->mutex);
            allocate_block(*block);
            base = block->memory + block->offset;
            length = block->data_length;
        }
        if (length % buffers) { s.x[0] = status(-12742); return; }   // kCMSampleBufferError_InvalidSampleData
        m.write32(s.x[2], buffers);
        m.write32(s.x[2] + 4, 0);
        const auto each = length / buffers;
        for (std::uint32_t i = 0; i < buffers; ++i) {
            const auto entry = s.x[2] + 8 + 16ULL * i;
            m.write32(entry, non_interleaved ? 1 : channels);
            m.write32(entry + 4, static_cast<std::uint32_t>(each));
            m.write64(entry + 8, base + each * i);
        }
        m.write64(block_out, retain(data_buffer));
        s.x[0] = 0;
    });
}

void CoreMedia::install() {
    if (const auto found = process.resolve(core_foundation, "_kCFAllocatorNull")) null_allocator = process.main_engine().read64(found->address);
    install_time();
    install_block_buffers();
    install_format_descriptions();
    install_sample_buffers();
    install_keys();
    install_audio_formats();
    install_sample_buffer_access();
}

} // namespace

void install_core_media(InstallContext& context) { std::make_shared<CoreMedia>(context)->install(); }

} // namespace alchemy::gui::fill
