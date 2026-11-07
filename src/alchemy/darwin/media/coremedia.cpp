#include "alchemy/darwin/media/media.hpp"

#include "alchemy/darwin/media/cmtime.hpp"
#include "alchemy/darwin/media/video_config.hpp"

#include <algorithm>
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
// per-sample iteration, image buffers), the CMAttachmentBearer calls (sample
// buffers' buffer-level attachments here, CVBuffers' through CoreVideo) and the
// documented constant keys.
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

// H.265 sequence parameter set (NAL type 33): the conformance-window-cropped picture size.
std::optional<std::pair<std::int32_t, std::int32_t>> hevc_sps_dimensions(std::span<const std::uint8_t> nal) {
    if (nal.size() < 4 || ((nal[0] >> 1) & 0x3f) != 33) return std::nullopt;
    Bits b;
    for (std::size_t i = 2, zeros = 0; i < nal.size(); ++i) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        b.bytes.push_back(nal[i]);
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    b.read(4);                               // sps_video_parameter_set_id
    const auto sub_layers = b.read(3);       // sps_max_sub_layers_minus1
    b.bit();                                 // sps_temporal_id_nesting_flag
    // profile_tier_level(1, sub_layers): general profile space .. level (88 + 8 bits).
    b.read(8); b.read(32); b.read(32); b.read(16); b.read(8);
    std::vector<std::pair<bool, bool>> present;
    for (std::uint32_t i = 0; i < sub_layers; ++i) {
        const bool profile = b.bit(), level = b.bit();
        present.emplace_back(profile, level);
    }
    if (sub_layers > 0)
        for (std::uint32_t i = sub_layers; i < 8; ++i) b.read(2);
    for (const auto& [profile, level] : present) {
        if (profile) { b.read(32); b.read(32); b.read(24); }   // 88 bits
        if (level) b.read(8);
    }
    b.ue();                                  // sps_seq_parameter_set_id
    const auto chroma_format = b.ue();
    if (chroma_format == 3) b.bit();         // separate_colour_plane_flag
    const auto width = b.ue(), height = b.ue();
    std::uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (b.bit()) { left = b.ue(); right = b.ue(); top = b.ue(); bottom = b.ue(); }
    if (b.failed || width == 0 || height == 0 || width > 16384 || height > 16384) return std::nullopt;
    const std::uint32_t sub_width = chroma_format == 1 || chroma_format == 2 ? 2 : 1, sub_height = chroma_format == 1 ? 2 : 1;
    const auto w = static_cast<std::int64_t>(width) - static_cast<std::int64_t>(sub_width) * (left + right);
    const auto h = static_cast<std::int64_t>(height) - static_cast<std::int64_t>(sub_height) * (top + bottom);
    if (w <= 0 || h <= 0) return std::nullopt;
    return std::pair{static_cast<std::int32_t>(w), static_cast<std::int32_t>(h)};
}

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

// A CMBlockBuffer: its data is the concatenation of its segments. A segment is a range of a
// memory block (allocated when first needed, or the caller's), or of another buffer's memory
// (a buffer reference, which keeps that buffer).
struct Segment {
    Address memory{};
    bool owned{};             // freed with free() when the buffer is deallocated
    std::uint64_t block_length{}, offset{}, length{};
    Address parent{};         // retained block buffer whose memory this segment refers to
};
struct BlockBuffer final : ObjectPayload {
    std::mutex mutex;
    std::vector<Segment> segments;
    std::uint64_t data_length{};
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
    Address cookie{};                         // audio: guest copy of the magic cookie or 0
    std::uint64_t cookie_size{};
    Address format_list{};                    // audio: guest AudioFormatListItem (made on first use)
};
struct SampleBuffer final : ObjectPayload {
    std::mutex mutex;
    Address data{}, format{}, image{};         // retained
    std::int64_t samples{};
    std::vector<std::uint8_t> timing;          // CMSampleTimingInfo entries (72 bytes each)
    std::vector<std::uint64_t> sizes;
    Address attachments{};                     // retained CFArray of mutable dictionaries
    struct Attachment { Address key{}, value{}; std::uint32_t mode{}; };   // retained key and value
    std::vector<Attachment> buffer_attachments;   // CMSetAttachment's (the CMAttachmentBearer calls)
    bool invalidated{};
    std::optional<completion_kit::media::CMTime> output_pts;   // CMSampleBufferSetOutputPresentationTimeStamp
    std::vector<std::uint8_t> packet_descriptions;             // AudioStreamPacketDescription entries (16 bytes each)
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
    void allocate_segment(Segment& segment) {
        if (segment.memory || !segment.block_length) return;
        segment.memory = malloc(segment.block_length);
        segment.owned = true;
    }
    void allocate_block(BlockBuffer& buffer) {
        for (auto& segment : buffer.segments) allocate_segment(segment);
    }
    // A memory-block segment (the checks of CMBlockBufferCreateWithMemoryBlock / AppendMemoryBlock).
    std::optional<Segment> memory_segment(Address memory, std::uint64_t block_length, Address block_allocator, std::uint64_t offset,
                                          std::uint64_t data_length, std::uint32_t flags, std::int32_t& error) {
        if (offset > block_length || data_length > block_length - offset) { error = bb_bad_length; return std::nullopt; }
        Segment segment;
        segment.memory = memory;
        segment.owned = memory && block_allocator != null_allocator;
        segment.block_length = block_length;
        segment.offset = offset;
        segment.length = data_length;
        if (flags & 1U) allocate_segment(segment);   // kCMBlockBufferAssureMemoryNowFlag
        error = 0;
        return segment;
    }
    Address create_block(Address memory, std::uint64_t block_length, Address block_allocator, std::uint64_t offset,
                         std::uint64_t data_length, std::uint32_t flags, std::int32_t& error) {
        const auto segment = memory_segment(memory, block_length, block_allocator, offset, data_length, flags, error);
        if (!segment) return 0;
        auto value = std::make_shared<BlockBuffer>();
        value->segments.push_back(*segment);
        value->data_length = segment->length;
        return make(block_class, value);
    }
    // Segments covering [offset, offset + length) of `source` (memory allocated), each keeping `owner`.
    std::vector<Segment> reference(BlockBuffer& source, Address owner, std::uint64_t offset, std::uint64_t length) {
        allocate_block(source);
        std::vector<Segment> out;
        std::uint64_t at = 0;
        for (const auto& segment : source.segments) {
            const auto end = at + segment.length;
            if (length && offset < end && offset + length > at) {
                const auto from = std::max(offset, at), to = std::min(offset + length, end);
                Segment part;
                part.memory = segment.memory;
                part.block_length = segment.block_length;
                part.offset = segment.offset + (from - at);
                part.length = to - from;
                part.parent = retain(owner);
                out.push_back(part);
            }
            at = end;
        }
        return out;
    }
    // The segment holding byte `offset` (and the offset inside it); memory allocated.
    std::pair<Segment*, std::uint64_t> locate(BlockBuffer& buffer, std::uint64_t offset) {
        std::uint64_t at = 0;
        for (auto& segment : buffer.segments) {
            if (offset < at + segment.length) {
                allocate_segment(segment);
                return {&segment, offset - at};
            }
            at += segment.length;
        }
        return {nullptr, 0};
    }
    // Copies [offset, offset + bytes.size()) out of the buffer (or into it, with `write`).
    void transfer(GuestMemory& m, BlockBuffer& buffer, std::uint64_t offset, std::span<std::uint8_t> bytes, bool write) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto [segment, inner] = locate(buffer, offset + done);
            if (!segment) throw std::runtime_error("Alchemy: CMBlockBuffer range outside the data");
            const auto count = std::min<std::uint64_t>(segment->length - inner, bytes.size() - done);
            const auto address = segment->memory + segment->offset + inner;
            const auto part = bytes.subspan(done, static_cast<std::size_t>(count));
            if (write) m.write_bytes(address, part);
            else m.read_bytes(address, part);
            done += static_cast<std::size_t>(count);
        }
    }
    void install();
    void install_time();
    void install_block_buffers();
    void install_format_descriptions();
    void install_sample_buffers();
    void install_keys();
    void install_audio_formats();
    void install_sample_buffer_access();
    void install_attachments();
    void install_time_more();
    void install_sample_buffer_more();
    void install_format_description_more();
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
                std::vector<Segment> segments;
                {
                    std::lock_guard lock(value->mutex);
                    segments = std::move(value->segments);
                    value->segments.clear();
                }
                for (const auto& segment : segments) {
                    if (segment.owned) free(segment.memory);
                    release(segment.parent);
                }
            }
            static_cast<void>(objc.send_super(s.x[0], block_class, objc.selector("dealloc")));
        }, false, "v@:"));
    }
    constant(core_media, "_CMBlockBufferGetTypeID", static_cast<Address>(TypeId::block_buffer));
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
    // CMBlockBufferCreateEmpty(structureAllocator, subBlockCapacity, flags, CMBlockBufferRef *out)
    raw(core_media, "_CMBlockBufferCreateEmpty", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) { s.x[0] = status(bb_bad_pointer); return; }
        m.write64(s.x[3], make(block_class, std::make_shared<BlockBuffer>()));
        s.x[0] = 0;
    });
    // CMBlockBufferAppendMemoryBlock(buffer, memoryBlock, blockLength, blockAllocator, customBlockSource,
    //     offsetToData, dataLength, flags)
    raw(core_media, "_CMBlockBufferAppendMemoryBlock", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        if (s.x[4]) { s.x[0] = status(bb_bad_custom_block_source); return; }
        std::int32_t error{};
        const auto segment = memory_segment(s.x[1], s.x[2], s.x[3], s.x[5], s.x[6], static_cast<std::uint32_t>(s.x[7]), error);
        if (!segment) { s.x[0] = status(error); return; }
        std::lock_guard lock(value->mutex);
        value->segments.push_back(*segment);
        value->data_length += segment->length;
        s.x[0] = 0;
    });
    // CMBlockBufferAppendBufferReference(buffer, targetBuffer, offsetToData, dataLength (0: to the end), flags)
    raw(core_media, "_CMBlockBufferAppendBufferReference", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        const auto target = state<BlockBuffer>(s.x[1], "CMBlockBuffer");
        if (value == target) { s.x[0] = status(bb_bad_pointer); return; }
        std::vector<Segment> parts;
        {
            std::lock_guard lock(target->mutex);
            if (s.x[2] > target->data_length) { s.x[0] = status(bb_bad_offset); return; }
            const auto length = s.x[3] ? s.x[3] : target->data_length - s.x[2];
            if (length > target->data_length - s.x[2]) { s.x[0] = status(bb_bad_length); return; }
            parts = reference(*target, s.x[1], s.x[2], length);
        }
        std::lock_guard lock(value->mutex);
        for (const auto& part : parts) {
            value->segments.push_back(part);
            value->data_length += part.length;
        }
        s.x[0] = 0;
    });
    // CMBlockBufferCreateWithBufferReference(structureAllocator, targetBuffer, offsetToData, dataLength, flags, CMBlockBufferRef *out)
    raw(core_media, "_CMBlockBufferCreateWithBufferReference", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[5]) { s.x[0] = status(bb_bad_pointer); return; }
        m.write64(s.x[5], 0);
        const auto target = state<BlockBuffer>(s.x[1], "CMBlockBuffer");
        auto value = std::make_shared<BlockBuffer>();
        {
            std::lock_guard lock(target->mutex);
            if (s.x[2] > target->data_length) { s.x[0] = status(bb_bad_offset); return; }
            const auto length = s.x[3] ? s.x[3] : target->data_length - s.x[2];
            if (length > target->data_length - s.x[2]) { s.x[0] = status(bb_bad_length); return; }
            value->segments = reference(*target, s.x[1], s.x[2], length);
            value->data_length = length;
        }
        m.write64(s.x[5], make(block_class, value));
        s.x[0] = 0;
    });
    raw(core_media, "_CMBlockBufferGetDataLength", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->data_length;
    });
    raw(core_media, "_CMBlockBufferIsEmpty", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->data_length == 0;
    });
    raw(core_media, "_CMBlockBufferAssureBlockMemory", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        allocate_block(*value);
        const bool ok = std::all_of(value->segments.begin(), value->segments.end(), [](const Segment& g) { return g.memory || !g.block_length; });
        s.x[0] = ok ? 0 : status(bb_block_allocation_failed);
    });
    // CMBlockBufferGetDataPointer(buffer, offset, size_t *lengthAtOffset, size_t *totalLength, char **dataPointer):
    // lengthAtOffset is what is contiguous from the offset (the rest of its segment).
    raw(core_media, "_CMBlockBufferGetDataPointer", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        const auto offset = s.x[1];
        if (offset > value->data_length || (offset == value->data_length && value->data_length)) { s.x[0] = status(bb_bad_offset); return; }
        const auto [segment, inner] = locate(*value, offset);
        if (s.x[2]) m.write64(s.x[2], segment ? segment->length - inner : 0);
        if (s.x[3]) m.write64(s.x[3], value->data_length);
        if (s.x[4]) m.write64(s.x[4], segment && segment->memory ? segment->memory + segment->offset + inner : 0);
        s.x[0] = 0;
    });
    raw(core_media, "_CMBlockBufferIsRangeContiguous", [this](CpuState& s, GuestMemory&) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        const auto offset = s.x[1];
        if (offset > value->data_length) { s.x[0] = 0; return; }
        const auto length = s.x[2] ? s.x[2] : value->data_length - offset;
        if (length > value->data_length - offset) { s.x[0] = 0; return; }
        if (!length) { s.x[0] = 1; return; }
        const auto [segment, inner] = locate(*value, offset);
        s.x[0] = segment && length <= segment->length - inner;
    });
    // CMBlockBufferReplaceDataBytes(sourceBytes, destinationBuffer, offsetIntoDestination, dataLength)
    raw(core_media, "_CMBlockBufferReplaceDataBytes", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<BlockBuffer>(s.x[1], "CMBlockBuffer");
        if (!s.x[0] && s.x[3]) { s.x[0] = status(bb_bad_pointer); return; }
        std::lock_guard lock(value->mutex);
        if (s.x[2] > value->data_length || s.x[3] > value->data_length - s.x[2]) { s.x[0] = status(bb_bad_length); return; }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[3]));
        if (!bytes.empty()) {
            m.read_bytes(s.x[0], bytes);
            transfer(m, *value, s.x[2], bytes, true);
        }
        s.x[0] = 0;
    });
    // CMBlockBufferCopyDataBytes(buffer, offsetToData, dataLength, void *destination)
    raw(core_media, "_CMBlockBufferCopyDataBytes", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        if (!s.x[3] && s.x[2]) { s.x[0] = status(bb_bad_pointer); return; }
        std::lock_guard lock(value->mutex);
        if (s.x[1] > value->data_length || s.x[2] > value->data_length - s.x[1]) { s.x[0] = status(bb_bad_length); return; }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[2]));
        if (!bytes.empty()) {
            transfer(m, *value, s.x[1], bytes, false);
            m.write_bytes(s.x[3], bytes);
        }
        s.x[0] = 0;
    });
    // CMBlockBufferAccessDataBytes(buffer, offset, length, void *temporaryBlock, char **returnedPointer): the
    // data itself when contiguous, else a copy in the caller's temporary block.
    raw(core_media, "_CMBlockBufferAccessDataBytes", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<BlockBuffer>(s.x[0], "CMBlockBuffer");
        if (!s.x[4]) { s.x[0] = status(bb_bad_pointer); return; }
        std::lock_guard lock(value->mutex);
        if (s.x[1] > value->data_length || s.x[2] > value->data_length - s.x[1]) { s.x[0] = status(bb_bad_length); return; }
        const auto [segment, inner] = locate(*value, s.x[1]);
        if (segment && s.x[2] <= segment->length - inner) {
            m.write64(s.x[4], segment->memory + segment->offset + inner);
        } else {
            if (!s.x[3]) { s.x[0] = status(bb_bad_pointer); return; }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[2]));
            transfer(m, *value, s.x[1], bytes, false);
            if (!bytes.empty()) m.write_bytes(s.x[3], bytes);
            m.write64(s.x[4], s.x[3]);
        }
        s.x[0] = 0;
    });
    // CMBlockBufferFillDataBytes(char fillByte, buffer, offsetIntoDestination, dataLength)
    raw(core_media, "_CMBlockBufferFillDataBytes", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<BlockBuffer>(s.x[1], "CMBlockBuffer");
        std::lock_guard lock(value->mutex);
        if (s.x[2] > value->data_length || s.x[3] > value->data_length - s.x[2]) { s.x[0] = status(bb_bad_length); return; }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[3]), static_cast<std::uint8_t>(s.x[0]));
        if (!bytes.empty()) transfer(m, *value, s.x[2], bytes, true);
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
            bytes.resize(static_cast<std::size_t>(length));
            if (length) transfer(m, *source, offset, bytes, false);
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
    namespace vc = completion_kit::media::video_config;
    // The extensions `base` (or none) with `record` as the sample-description extension atom `atom` (+1).
    const auto with_atom = [this](Address base, const char* atom, const std::vector<std::uint8_t>& record) -> Address {
        const auto outer_key = cf.string("SampleDescriptionExtensionAtoms"), atom_key = cf.string(atom), data = bridge->data(record);
        std::vector<std::pair<Address, Address>> atoms;
        const auto existing = base ? cf.dictionary_value(base, outer_key) : 0;
        if (existing && cf.kind(existing) == ObjectKind::dictionary)
            for (const auto& entry : cf.dictionary_entries(existing))
                if (!cf.equal(entry.first, atom_key)) atoms.push_back(entry);
        atoms.emplace_back(atom_key, data);
        const auto inner = cf.dictionary(atoms);
        std::vector<std::pair<Address, Address>> entries;
        if (base)
            for (const auto& entry : cf.dictionary_entries(base))
                if (!cf.equal(entry.first, outer_key)) entries.push_back(entry);
        entries.emplace_back(outer_key, inner);
        const auto out = cf.dictionary(entries);
        for (const auto object : {outer_key, atom_key, data, inner}) release(object);
        return out;
    };
    // Guest copies of the parameter sets (GetH264/HEVCParameterSetAtIndex hand out their pointers).
    const auto copy_sets = [this](FormatDescription& value, GuestMemory& m) {
        for (const auto& set : value.parameter_sets) {
            const auto copy = malloc(set.size());
            m.write_bytes(copy, set);
            value.parameter_set_copies.push_back(copy);
        }
    };
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
    raw(core_media, "_CMVideoFormatDescriptionCreate", [this, copy_sets](CpuState& s, GuestMemory& m) {
        if (!s.x[5]) { s.x[0] = status(fd_invalid_parameter); return; }
        auto value = std::make_shared<FormatDescription>();
        value->media_type = fourcc("vide");
        value->subtype = static_cast<std::uint32_t>(s.x[1]);
        value->width = static_cast<std::int32_t>(s.x[2]);
        value->height = static_cast<std::int32_t>(s.x[3]);
        value->extensions = s.x[4] ? cf.copy(s.x[4]) : 0;
        // H.264 and HEVC: the parameter sets of the avcC or hvcC atom (for GetH264/HEVCParameterSetAtIndex).
        if (value->extensions && (value->subtype == fourcc("avc1") || value->subtype == fourcc("hvc1"))) {
            const auto outer_key = cf.string("SampleDescriptionExtensionAtoms"), atom_key = cf.string(value->subtype == fourcc("avc1") ? "avcC" : "hvcC");
            const auto atoms = cf.dictionary_value(value->extensions, outer_key);
            const auto record = atoms && cf.kind(atoms) == ObjectKind::dictionary ? cf.dictionary_value(atoms, atom_key) : 0;
            release(outer_key);
            release(atom_key);
            if (record && bridge->is_data(record)) {
                const auto bytes = bridge->bytes(record);
                const auto read = value->subtype == fourcc("avc1") ? vc::read_avc_configuration(bytes) : vc::read_hevc_configuration(bytes);
                if (read) {
                    value->parameter_sets = read->sets;
                    value->nal_header_length = static_cast<std::int32_t>(read->nal_length_size);
                    copy_sets(*value, m);
                }
            }
        }
        m.write64(s.x[5], make(format_class, value));
        s.x[0] = 0;
    });
    // CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator, count, const uint8_t *const *sets,
    //     const size_t *sizes, int NALUnitHeaderLength, CMFormatDescriptionRef *out)
    raw(core_media, "_CMVideoFormatDescriptionCreateFromH264ParameterSets", [this, with_atom, copy_sets](CpuState& s, GuestMemory& m) {
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
        // The avcC record of the sets as the sample-description extension atom, for writers and readers of it.
        if (const auto record = vc::avc_configuration(value->parameter_sets, static_cast<unsigned>(header)); !record.empty())
            value->extensions = with_atom(0, "avcC", record);
        copy_sets(*value, m);
        m.write64(s.x[5], make(format_class, value));
        s.x[0] = 0;
    });
    // CMVideoFormatDescriptionCreateFromHEVCParameterSets(allocator, count, sets, sizes, NALUnitHeaderLength,
    //     extensions, CMFormatDescriptionRef *out): the size from the SPS (NAL type 33) among the sets.
    raw(core_media, "_CMVideoFormatDescriptionCreateFromHEVCParameterSets", [this, with_atom, copy_sets](CpuState& s, GuestMemory& m) {
        const auto count = s.x[1];
        const auto header = static_cast<std::int32_t>(s.x[4]);
        if (s.x[6]) m.write64(s.x[6], 0);
        if (!s.x[6] || count < 1 || count > 64 || !s.x[2] || !s.x[3] || (header != 1 && header != 2 && header != 4)) {
            s.x[0] = status(fd_invalid_parameter);
            return;
        }
        auto value = std::make_shared<FormatDescription>();
        value->media_type = fourcc("vide");
        value->subtype = fourcc("hvc1");
        value->nal_header_length = header;
        std::optional<std::pair<std::int32_t, std::int32_t>> dimensions;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto pointer = m.read64(s.x[2] + i * 8), size = m.read64(s.x[3] + i * 8);
            if (!pointer || !size || size > (1 << 16)) { s.x[0] = status(fd_invalid_parameter); return; }
            std::vector<std::uint8_t> set(static_cast<std::size_t>(size));
            m.read_bytes(pointer, set);
            if (!dimensions) dimensions = hevc_sps_dimensions(set);
            value->parameter_sets.push_back(std::move(set));
        }
        if (!dimensions) { s.x[0] = status(fd_invalid_parameter); return; }
        value->width = dimensions->first;
        value->height = dimensions->second;
        // The extensions given, with the sets' hvcC record as the sample-description extension atom.
        if (const auto record = vc::hevc_configuration(value->parameter_sets, static_cast<unsigned>(header)); !record.empty())
            value->extensions = with_atom(s.x[5], "hvcC", record);
        else
            value->extensions = s.x[5] ? cf.copy(s.x[5]) : 0;
        copy_sets(*value, m);
        m.write64(s.x[6], make(format_class, value));
        s.x[0] = 0;
    });
    // CMVideoFormatDescriptionCreateForImageBuffer(allocator, CVImageBufferRef, CMFormatDescriptionRef *out):
    // the buffer's pixel format and size.
    raw(core_media, "_CMVideoFormatDescriptionCreateForImageBuffer", [this](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        if (!s.x[1] || !s.x[2] || call(core_foundation, "_CFGetTypeID", {s.x[1]}) != call(core_video, "_CVPixelBufferGetTypeID", {})) {
            s.x[0] = status(fd_invalid_parameter);
            return;
        }
        auto value = std::make_shared<FormatDescription>();
        value->media_type = fourcc("vide");
        value->subtype = static_cast<std::uint32_t>(call(core_video, "_CVPixelBufferGetPixelFormatType", {s.x[1]}));
        value->width = static_cast<std::int32_t>(call(core_video, "_CVPixelBufferGetWidth", {s.x[1]}));
        value->height = static_cast<std::int32_t>(call(core_video, "_CVPixelBufferGetHeight", {s.x[1]}));
        m.write64(s.x[2], make(format_class, value));
        s.x[0] = 0;
    });
    // CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(desc, index, const uint8_t **set, size_t *size,
    //     size_t *count, int *NALUnitHeaderLength)
    raw(core_media, "_CMVideoFormatDescriptionGetHEVCParameterSetAtIndex", [this](CpuState& s, GuestMemory& m) {
        const auto value = state<FormatDescription>(s.x[0], "CMFormatDescription");
        if (value->parameter_sets.empty() || value->subtype != fourcc("hvc1")) { s.x[0] = status(fd_invalid_parameter); return; }
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
                for (const auto& a : std::exchange(value->buffer_attachments, {})) { release(a.key); release(a.value); }
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
    //     const CMSampleTimingInfo *sampleTiming, CMSampleBufferRef *out): one sample, the image buffer.
    const auto for_image = [this, create](GuestMemory& m, Address image, Address format, Address timing, Address out) -> std::int32_t {
        if (!image || !format || !timing) {
            if (out) m.write64(out, 0);
            return sb_required_parameter_missing;
        }
        const auto result = create(m, 0, format, 1, 1, timing, 0, 0, out);
        if (result) return result;
        const auto value = state<SampleBuffer>(m.read64(out), "CMSampleBuffer");
        std::lock_guard lock(value->mutex);
        value->image = retain(image);
        return 0;
    };
    raw(core_media, "_CMSampleBufferCreateForImageBuffer", [for_image](CpuState& s, GuestMemory& m) {
        s.x[0] = status(for_image(m, s.x[1], s.x[5], s.x[6], s.x[7]));
    });
    // CMSampleBufferCreateReadyWithImageBuffer(allocator, imageBuffer, formatDescription, sampleTiming, out)
    raw(core_media, "_CMSampleBufferCreateReadyWithImageBuffer", [for_image](CpuState& s, GuestMemory& m) {
        s.x[0] = status(for_image(m, s.x[1], s.x[2], s.x[3], s.x[4]));
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
}

// The CMAttachmentBearer calls. A CMSampleBuffer keeps its buffer-level attachments here (the
// per-sample ones are CMSampleBufferGetSampleAttachmentsArray's); a CVBuffer's are CoreVideo's,
// reached through the CVBuffer calls with the same modes. Any other target stops the process.
void CoreMedia::install_attachments() {
    using Attachment = SampleBuffer::Attachment;
    const auto sample = [this](Address target) { return objc.find_host_state<SampleBuffer>(target); };
    const auto pixel_buffer = [this](Address target, const char* function) {
        if (call(core_foundation, "_CFGetTypeID", {target}) == call(core_video, "_CVPixelBufferGetTypeID", {})) return true;
        throw std::runtime_error(std::string("Alchemy: ") + function + ": the target is neither a CMSampleBuffer nor a CVBuffer");
    };
    // Removes the attachments `which` selects; releases them outside the lock.
    const auto remove = [this](SampleBuffer& b, const auto& which) {
        std::vector<Attachment> gone;
        {
            std::lock_guard lock(b.mutex);
            const auto split = std::stable_partition(b.buffer_attachments.begin(), b.buffer_attachments.end(), [&](const Attachment& a) { return !which(a); });
            gone.assign(split, b.buffer_attachments.end());
            b.buffer_attachments.erase(split, b.buffer_attachments.end());
        }
        for (const auto& a : gone) { release(a.key); release(a.value); }
    };
    // void CMSetAttachment(target, key, value, attachmentMode)
    const auto set = [this, sample, pixel_buffer](Address target, Address key, Address value, std::uint32_t mode) {
        if (!target || !key) return;
        const auto b = sample(target);
        if (!b) {
            if (pixel_buffer(target, "CMSetAttachment")) static_cast<void>(call(core_video, "_CVBufferSetAttachment", {target, key, value, mode}));
            return;
        }
        const Attachment added{retain(key), retain(value), mode};
        Attachment old{};
        {
            std::lock_guard lock(b->mutex);
            const auto found = std::find_if(b->buffer_attachments.begin(), b->buffer_attachments.end(), [&](const Attachment& a) { return cf.equal(a.key, key); });
            if (found != b->buffer_attachments.end()) old = std::exchange(*found, added);
            else b->buffer_attachments.push_back(added);
        }
        release(old.key);
        release(old.value);
    };
    raw(core_media, "_CMSetAttachment", [set](CpuState& s, GuestMemory&) {
        set(s.x[0], s.x[1], s.x[2], static_cast<std::uint32_t>(s.x[3]));
    });
    // CFTypeRef CMGetAttachment(target, key, CMAttachmentMode *attachmentModeOut): +0, NULL when absent.
    raw(core_media, "_CMGetAttachment", [this, sample, pixel_buffer](CpuState& s, GuestMemory& m) {
        const auto target = s.x[0], key = s.x[1], mode_out = s.x[2];
        s.x[0] = 0;
        if (!target || !key) return;
        const auto b = sample(target);
        if (!b) {
            if (pixel_buffer(target, "CMGetAttachment")) s.x[0] = call(core_video, "_CVBufferGetAttachment", {target, key, mode_out});
            return;
        }
        std::lock_guard lock(b->mutex);
        for (const auto& a : b->buffer_attachments)
            if (cf.equal(a.key, key)) {
                if (mode_out) m.write32(mode_out, a.mode);
                s.x[0] = a.value;
                return;
            }
    });
    // void CMRemoveAttachment(target, key)
    raw(core_media, "_CMRemoveAttachment", [this, sample, pixel_buffer, remove](CpuState& s, GuestMemory&) {
        const auto target = s.x[0], key = s.x[1];
        if (!target || !key) return;
        if (const auto b = sample(target)) remove(*b, [&](const Attachment& a) { return cf.equal(a.key, key); });
        else if (pixel_buffer(target, "CMRemoveAttachment")) static_cast<void>(call(core_video, "_CVBufferRemoveAttachment", {target, key}));
    });
    // void CMRemoveAllAttachments(target)
    raw(core_media, "_CMRemoveAllAttachments", [this, sample, pixel_buffer, remove](CpuState& s, GuestMemory&) {
        const auto target = s.x[0];
        if (!target) return;
        if (const auto b = sample(target)) remove(*b, [](const Attachment&) { return true; });
        else if (pixel_buffer(target, "CMRemoveAllAttachments")) static_cast<void>(call(core_video, "_CVBufferRemoveAllAttachments", {target}));
    });
    // CFDictionaryRef CMCopyDictionaryOfAttachments(allocator, target, attachmentMode): +1, the
    // attachments of that mode (an empty dictionary when there are none).
    raw(core_media, "_CMCopyDictionaryOfAttachments", [this, sample, pixel_buffer](CpuState& s, GuestMemory&) {
        const auto target = s.x[1];
        const auto mode = static_cast<std::uint32_t>(s.x[2]);
        s.x[0] = 0;
        if (!target) return;
        const auto b = sample(target);
        if (!b) {
            if (pixel_buffer(target, "CMCopyDictionaryOfAttachments")) s.x[0] = call(core_video, "_CVBufferCopyAttachments", {target, mode});
            return;
        }
        std::vector<std::pair<Address, Address>> pairs;
        {
            std::lock_guard lock(b->mutex);
            for (const auto& a : b->buffer_attachments)
                if (a.mode == mode) pairs.emplace_back(a.key, a.value);
        }
        s.x[0] = cf.dictionary(pairs);
    });
    // void CMSetAttachments(target, CFDictionaryRef theAttachments, attachmentMode)
    raw(core_media, "_CMSetAttachments", [this, set](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) return;
        for (const auto& [key, value] : cf.dictionary_entries(s.x[1])) set(s.x[0], key, value, static_cast<std::uint32_t>(s.x[2]));
    });
    // void CMPropagateAttachments(source, destination): the kCMAttachmentMode_ShouldPropagate ones.
    raw(core_media, "_CMPropagateAttachments", [this, set](CpuState& s, GuestMemory&) {
        const auto source = s.x[0], destination = s.x[1];
        if (!source || !destination) return;
        constexpr std::uint32_t should_propagate = 1;
        const auto copied = call(core_media, "_CMCopyDictionaryOfAttachments", {0, source, should_propagate});
        for (const auto& [key, value] : cf.dictionary_entries(copied)) set(destination, key, value, should_propagate);
        release(copied);
    });
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
        auto value = std::make_shared<FormatDescription>();
        if (s.x[4]) {   // the magic cookie of a compressed format, kept as given
            std::vector<std::uint8_t> cookie(static_cast<std::size_t>(s.x[4]));
            m.read_bytes(s.x[5], cookie);
            value->cookie = malloc(cookie.size());
            m.write_bytes(value->cookie, cookie);
            value->cookie_size = cookie.size();
        }
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
        auto part = std::make_shared<BlockBuffer>();
        part->segments = reference(*source, data_buffer, offset, size);
        part->data_length = size;
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
    // Valid until CMSampleBufferInvalidate.
    raw(core_media, "_CMSampleBufferIsValid", [this](CpuState& s, GuestMemory&) {
        const auto value = state<SampleBuffer>(s.x[0], "CMSampleBuffer");
        std::lock_guard lock(value->mutex);
        s.x[0] = value->invalidated ? 0 : 1;
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
        auto block = state<BlockBuffer>(data_buffer, "CMBlockBuffer");
        Address base{};
        std::uint64_t length{};
        bool contiguous{};
        {
            std::lock_guard lock(block->mutex);
            length = block->data_length;
            const auto [segment, inner] = locate(*block, 0);
            contiguous = !length || (segment && segment->length - inner >= length);
            if (segment) base = segment->memory + segment->offset + inner;
        }
        if (!contiguous) {
            // The audio buffer list points into one block: a contiguous copy of the data.
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            {
                std::lock_guard lock(block->mutex);
                transfer(m, *block, 0, bytes, false);
            }
            const auto memory = malloc(bytes.size());
            m.write_bytes(memory, bytes);
            std::int32_t error{};
            data_buffer = create_block(memory, bytes.size(), 0, 0, bytes.size(), 0, error);
            if (!data_buffer) { free(memory); s.x[0] = status(error); return; }
            base = memory;
            m.write64(block_out, data_buffer);   // the caller's reference (+1, created)
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
        if (contiguous) m.write64(block_out, retain(data_buffer));
        s.x[0] = 0;
    });
}

// CMTime and CMTimeRange beyond the arithmetic (<CoreMedia/CMTime.h>, <CoreMedia/CMTimeRange.h>): comparisons,
// ratios, ranges (union, intersection, containment, mapping, clamping), dictionaries and descriptions. CMTime and
// CMTimeRange arguments (24 and 48 bytes) arrive by reference; results go through x8.
void CoreMedia::install_time_more() {
    namespace cm = completion_kit::media;
    using cm::CMTime;
    const auto range_at = [](GuestMemory& m, Address at) { return cm::CMTimeRange{read_cmtime(m, at), read_cmtime(m, at + 24)}; };
    const auto write_range = [](GuestMemory& m, Address at, const cm::CMTimeRange& r) {
        write_cmtime(m, at, r.start);
        write_cmtime(m, at + 24, r.duration);
    };
    const auto valid_range = [](const cm::CMTimeRange& r) {
        return cm::is_valid(r.start) && cm::is_valid(r.duration) && r.duration.epoch == 0 && cm::compare(r.duration, cm::time_zero) >= 0;
    };
    // CMTimeRange kCMTimeRangeZero / kCMTimeRangeInvalid; CMTime dictionary keys; CMTimeRange dictionary keys.
    std::array<std::uint8_t, 48> zero_range{};
    {
        const auto zero = cm::time_zero;
        std::memcpy(zero_range.data(), &zero.value, 8);
        std::memcpy(zero_range.data() + 8, &zero.timescale, 4);
        std::memcpy(zero_range.data() + 12, &zero.flags, 4);
        std::memcpy(zero_range.data() + 24, &zero.value, 8);
        std::memcpy(zero_range.data() + 32, &zero.timescale, 4);
        std::memcpy(zero_range.data() + 36, &zero.flags, 4);
    }
    data(core_media, "_kCMTimeRangeZero", zero_range, 8);
    data(core_media, "_kCMTimeRangeInvalid", std::array<std::uint8_t, 48>{}, 8);
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 6>{{
             {"_kCMTimeValueKey", "value"}, {"_kCMTimeScaleKey", "timescale"}, {"_kCMTimeEpochKey", "epoch"}, {"_kCMTimeFlagsKey", "flags"},
             {"_kCMTimeRangeStartKey", "start"}, {"_kCMTimeRangeDurationKey", "duration"}}})
        string(core_media, symbol, value);
    raw(core_media, "_CMTimeMakeWithEpoch", [](CpuState& s, GuestMemory& m) {
        auto time = cm::make(static_cast<std::int64_t>(s.x[0]), static_cast<std::int32_t>(s.x[1]));
        if (cm::is_valid(time)) time.epoch = static_cast<std::int64_t>(s.x[2]);
        write_cmtime(m, s.x[8], time);
    });
    // CMTimeMultiplyByRatio(time, int32_t multiplier, int32_t divisor): exact when the product fits, else rounded.
    raw(core_media, "_CMTimeMultiplyByRatio", [](CpuState& s, GuestMemory& m) {
        const auto time = read_cmtime(m, s.x[0]);
        const auto multiplier = static_cast<std::int32_t>(s.x[1]), divisor = static_cast<std::int32_t>(s.x[2]);
        CMTime out = time;
        if (divisor == 0 || !cm::is_valid(time)) out = cm::time_invalid;
        else if (!cm::is_numeric(time)) out = (multiplier < 0) != (divisor < 0) && (time.flags & cm::cmtime_flags::positive_infinity) ? cm::time_negative_infinity
                                            : (multiplier < 0) != (divisor < 0) && (time.flags & cm::cmtime_flags::negative_infinity) ? cm::time_positive_infinity : time;
        else {
            const long double exact = static_cast<long double>(time.value) * multiplier / divisor;
            const auto rounded = std::llround(static_cast<double>(exact));
            out.value = rounded;
            if (static_cast<long double>(rounded) != exact) out.flags |= cm::cmtime_flags::rounded;
        }
        write_cmtime(m, s.x[8], out);
    });
    raw(core_media, "_CMTimeMinimum", [](CpuState& s, GuestMemory& m) {
        const auto a = read_cmtime(m, s.x[0]), b = read_cmtime(m, s.x[1]);
        write_cmtime(m, s.x[8], !cm::is_valid(a) || !cm::is_valid(b) ? cm::time_invalid : cm::compare(a, b) <= 0 ? a : b);
    });
    raw(core_media, "_CMTimeMaximum", [](CpuState& s, GuestMemory& m) {
        const auto a = read_cmtime(m, s.x[0]), b = read_cmtime(m, s.x[1]);
        write_cmtime(m, s.x[8], !cm::is_valid(a) || !cm::is_valid(b) ? cm::time_invalid : cm::compare(a, b) >= 0 ? a : b);
    });
    raw(core_media, "_CMTimeAbsoluteValue", [](CpuState& s, GuestMemory& m) {
        auto time = read_cmtime(m, s.x[0]);
        if (time.flags & cm::cmtime_flags::negative_infinity) time = cm::time_positive_infinity;
        else if (cm::is_numeric(time) && time.value < 0) time.value = -time.value;
        write_cmtime(m, s.x[8], time);
    });
    // CMTimeShow / CMTimeRangeShow: the description on stderr, as CFShow does.
    const auto show = [this](const std::string& text) {
        const auto line = text + "\n";
        static_cast<void>(process.output(2, std::span(reinterpret_cast<const std::uint8_t*>(line.data()), line.size())));
    };
    raw(core_media, "_CMTimeShow", [show](CpuState& s, GuestMemory& m) { show(cm::description(read_cmtime(m, s.x[0]))); });
    const auto range_text = [](const cm::CMTimeRange& r) { return "{" + cm::description(r.start) + ", " + cm::description(r.duration) + "}"; };
    raw(core_media, "_CMTimeRangeShow", [show, range_at, range_text](CpuState& s, GuestMemory& m) { show(range_text(range_at(m, s.x[0]))); });
    raw(core_media, "_CMTimeRangeCopyDescription", [this, range_at, range_text](CpuState& s, GuestMemory& m) { s.x[0] = cf.string(range_text(range_at(m, s.x[1]))); });
    // Dictionaries: {value, timescale, flags, epoch} as CFNumbers; ranges {start, duration} of those.
    const auto time_dictionary = [this](const CMTime& t) {
        const std::array<std::pair<const char*, Address>, 4> fields{{{"value", cf.integer(t.value, 4)}, {"timescale", cf.integer(t.timescale, 3)},
                                                                     {"flags", cf.integer(t.flags, 3)}, {"epoch", cf.integer(t.epoch, 4)}}};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
        for (const auto& [key, value] : fields) entries.emplace_back(cf.constant_string(key), value);
        const auto dictionary = cf.dictionary(entries);
        for (const auto& [key, value] : fields) release(value);
        return dictionary;
    };
    const auto time_from = [this](Address dictionary) {
        if (!dictionary || !bridge->is_dictionary(dictionary)) return cm::time_invalid;
        const auto field = [&](const char* key) { const auto v = cf.dictionary_value(dictionary, cf.constant_string(key)); return v ? bridge->integer(v) : std::nullopt; };
        const auto value = field("value"), timescale = field("timescale"), flags = field("flags"), epoch = field("epoch");
        if (!value || !timescale || !flags) return cm::time_invalid;
        return CMTime{*value, static_cast<std::int32_t>(*timescale), static_cast<std::uint32_t>(*flags), epoch.value_or(0)};
    };
    raw(core_media, "_CMTimeCopyAsDictionary", [time_dictionary](CpuState& s, GuestMemory& m) { s.x[0] = time_dictionary(read_cmtime(m, s.x[0])); });
    raw(core_media, "_CMTimeMakeFromDictionary", [time_from](CpuState& s, GuestMemory& m) { write_cmtime(m, s.x[8], time_from(s.x[0])); });
    raw(core_media, "_CMTimeRangeCopyAsDictionary", [this, range_at, time_dictionary](CpuState& s, GuestMemory& m) {
        const auto r = range_at(m, s.x[0]);
        const auto start = time_dictionary(r.start), duration = time_dictionary(r.duration);
        s.x[0] = cf.dictionary(std::vector<std::pair<std::uint64_t, std::uint64_t>>{{cf.constant_string("start"), start}, {cf.constant_string("duration"), duration}});
        release(start);
        release(duration);
    });
    raw(core_media, "_CMTimeRangeMakeFromDictionary", [this, time_from, write_range](CpuState& s, GuestMemory& m) {
        const auto d = s.x[0];
        const auto part = [&](const char* key) { return d && bridge->is_dictionary(d) ? time_from(cf.dictionary_value(d, cf.constant_string(key))) : cm::time_invalid; };
        write_range(m, s.x[8], {part("start"), part("duration")});
    });
    raw(core_media, "_CMTimeRangeFromTimeToTime", [write_range](CpuState& s, GuestMemory& m) {
        const auto start = read_cmtime(m, s.x[0]), end = read_cmtime(m, s.x[1]);
        write_range(m, s.x[8], {start, cm::subtract(end, start)});
    });
    raw(core_media, "_CMTimeRangeGetUnion", [range_at, write_range](CpuState& s, GuestMemory& m) {
        const auto a = range_at(m, s.x[0]), b = range_at(m, s.x[1]);
        const auto start = cm::compare(a.start, b.start) <= 0 ? a.start : b.start;
        const auto end_a = cm::range_end(a), end_b = cm::range_end(b);
        write_range(m, s.x[8], {start, cm::subtract(cm::compare(end_a, end_b) >= 0 ? end_a : end_b, start)});
    });
    raw(core_media, "_CMTimeRangeGetIntersection", [range_at, write_range](CpuState& s, GuestMemory& m) {
        const auto a = range_at(m, s.x[0]), b = range_at(m, s.x[1]);
        const auto start = cm::compare(a.start, b.start) >= 0 ? a.start : b.start;
        const auto end_a = cm::range_end(a), end_b = cm::range_end(b);
        const auto end = cm::compare(end_a, end_b) <= 0 ? end_a : end_b;
        write_range(m, s.x[8], cm::compare(end, start) > 0 ? cm::CMTimeRange{start, cm::subtract(end, start)} : cm::CMTimeRange{start, cm::time_zero});
    });
    // [start, start + duration): the end is not in the range.
    raw(core_media, "_CMTimeRangeContainsTime", [range_at, valid_range](CpuState& s, GuestMemory& m) {
        const auto r = range_at(m, s.x[0]);
        const auto t = read_cmtime(m, s.x[1]);
        s.x[0] = valid_range(r) && cm::is_valid(t) && cm::compare(t, r.start) >= 0 && cm::compare(t, cm::range_end(r)) < 0 ? 1 : 0;
    });
    raw(core_media, "_CMTimeRangeContainsTimeRange", [range_at, valid_range](CpuState& s, GuestMemory& m) {
        const auto a = range_at(m, s.x[0]), b = range_at(m, s.x[1]);
        s.x[0] = valid_range(a) && valid_range(b) && cm::compare(b.start, a.start) >= 0 && cm::compare(cm::range_end(b), cm::range_end(a)) <= 0 ? 1 : 0;
    });
    raw(core_media, "_CMTimeRangeEqual", [range_at](CpuState& s, GuestMemory& m) {
        const auto a = range_at(m, s.x[0]), b = range_at(m, s.x[1]);
        s.x[0] = cm::compare(a.start, b.start) == 0 && cm::compare(a.duration, b.duration) == 0 ? 1 : 0;
    });
    raw(core_media, "_CMTimeClampToRange", [range_at](CpuState& s, GuestMemory& m) {
        auto t = read_cmtime(m, s.x[0]);
        const auto r = range_at(m, s.x[1]);
        if (cm::compare(t, r.start) < 0) t = r.start;
        else if (cm::compare(t, cm::range_end(r)) > 0) t = cm::range_end(r);
        write_cmtime(m, s.x[8], t);
    });
    // start + (time - start) mod duration, for a time outside the range.
    raw(core_media, "_CMTimeFoldIntoRange", [range_at](CpuState& s, GuestMemory& m) {
        auto t = read_cmtime(m, s.x[0]);
        const auto r = range_at(m, s.x[1]);
        const auto span = cm::seconds(r.duration);
        if (cm::is_numeric(t) && span > 0 && (cm::compare(t, r.start) < 0 || cm::compare(t, cm::range_end(r)) >= 0)) {
            const auto offset = std::fmod(cm::seconds(t) - cm::seconds(r.start), span);
            t = cm::add(r.start, cm::make_with_seconds(offset < 0 ? offset + span : offset, std::max(t.timescale, r.start.timescale)));
        }
        write_cmtime(m, s.x[8], t);
    });
    // CMTimeMapTimeFromRangeToRange(t, fromRange, toRange): toStart + (t - fromStart) * toDuration / fromDuration.
    raw(core_media, "_CMTimeMapTimeFromRangeToRange", [range_at](CpuState& s, GuestMemory& m) {
        const auto t = read_cmtime(m, s.x[0]);
        const auto from = range_at(m, s.x[1]), to = range_at(m, s.x[2]);
        const auto from_span = cm::seconds(from.duration);
        CMTime out = cm::time_invalid;
        if (cm::is_numeric(t) && from_span > 0) {
            const auto scaled = (cm::seconds(t) - cm::seconds(from.start)) * cm::seconds(to.duration) / from_span;
            out = cm::add(to.start, cm::make_with_seconds(scaled, std::max(to.start.timescale, t.timescale)));
        }
        write_cmtime(m, s.x[8], out);
    });
    // CMTimeMapping CMTimeMappingMake(CMTimeRange source, CMTimeRange target): 96 bytes through x8.
    raw(core_media, "_CMTimeMappingMake", [range_at, write_range](CpuState& s, GuestMemory& m) {
        const auto source = range_at(m, s.x[0]), target = range_at(m, s.x[1]);
        write_range(m, s.x[8], source);
        write_range(m, s.x[8] + 48, target);
    });
}

// CMSampleBuffer beyond creation and access: timing and size queries, output timestamps, copies, invalidation,
// readiness (buffers are always ready: data is never provided later), audio sample buffers from packets or
// buffer lists, and PCM copies. Error codes are <CoreMedia/CMSampleBuffer.h>'s.
void CoreMedia::install_sample_buffer_more() {
    namespace cm = completion_kit::media;
    constexpr std::int32_t sb_already_has_data = -12732, sb_index_out_of_range = -12734, sb_no_sizes = -12735, sb_no_timing = -12736,
        sb_array_too_small = -12737, sb_invalid_media_type = -12741;
    constant(core_media, "_CMSampleBufferGetTypeID", static_cast<Address>(TypeId::sample_buffer));
    constant(core_media, "_CMFormatDescriptionGetTypeID", static_cast<Address>(TypeId::format_description));
    const auto sample = [this](Address object) { return state<SampleBuffer>(object, "CMSampleBuffer"); };
    // The timing of sample `index`: one entry covers every sample (each a duration later).
    const auto timing_of = [](const SampleBuffer& b, std::int64_t index, std::vector<std::uint8_t>& out) -> std::int32_t {
        const auto entries = static_cast<std::int64_t>(b.timing.size() / 72);
        if (!entries) return sb_no_timing;
        if (index < 0 || index >= std::max<std::int64_t>(b.samples, entries)) return sb_index_out_of_range;
        if (entries == 1) {
            out.assign(b.timing.begin(), b.timing.begin() + 72);
            const auto step = cm::multiply(timing_time(b.timing, 0), static_cast<std::int32_t>(index));
            for (const std::size_t at : {std::size_t{24}, std::size_t{48}}) {
                const auto time = timing_time(out, at);
                if (cm::is_numeric(time) && index) put_timing_time(out, at, cm::add(time, step));
            }
        } else {
            out.assign(b.timing.begin() + index * 72, b.timing.begin() + index * 72 + 72);
        }
        return 0;
    };
    raw(core_media, "_CMSampleBufferGetDecodeTimeStamp", [sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        write_cmtime(m, s.x[8], b->timing.size() >= 72 ? timing_time(b->timing, 48) : cm::time_invalid);
    });
    raw(core_media, "_CMSampleBufferGetOutputPresentationTimeStamp", [sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        write_cmtime(m, s.x[8], b->output_pts ? *b->output_pts : b->timing.size() >= 72 ? timing_time(b->timing, 24) : cm::time_invalid);
    });
    raw(core_media, "_CMSampleBufferSetOutputPresentationTimeStamp", [sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        b->output_pts = read_cmtime(m, s.x[1]);
        s.x[0] = 0;
    });
    // The output duration: the duration (no trimming or speed attachments are applied).
    raw(core_media, "_CMSampleBufferGetOutputDuration", [sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        const auto entries = b->timing.size() / 72;
        cm::CMTime total = entries ? cm::time_zero : cm::time_invalid;
        for (std::size_t i = 0; i < entries; ++i) {
            const auto duration = timing_time(b->timing, i * 72);
            total = cm::add(total, entries == 1 ? cm::multiply(duration, static_cast<std::int32_t>(b->samples)) : duration);
        }
        write_cmtime(m, s.x[8], total);
    });
    // OSStatus CMSampleBufferGetSampleTimingInfo(sbuf, CMItemIndex, CMSampleTimingInfo *out)
    raw(core_media, "_CMSampleBufferGetSampleTimingInfo", [sample, timing_of](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        if (!s.x[2]) { s.x[0] = status(sb_required_parameter_missing); return; }
        std::vector<std::uint8_t> entry;
        const auto error = timing_of(*b, static_cast<std::int64_t>(s.x[1]), entry);
        if (!error) m.write_bytes(s.x[2], entry);
        s.x[0] = status(error);
    });
    // OSStatus (sbuf, CMItemCount entries, CMSampleTimingInfo *array, CMItemCount *entriesNeededOut)
    raw(core_media, "_CMSampleBufferGetSampleTimingInfoArray", [sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        const auto entries = b->timing.size() / 72;
        if (s.x[3]) m.write64(s.x[3], entries);
        if (!entries) { s.x[0] = status(sb_no_timing); return; }
        if (s.x[2]) {
            if (s.x[1] < entries) { s.x[0] = status(sb_array_too_small); return; }
            m.write_bytes(s.x[2], b->timing);
        }
        s.x[0] = 0;
    });
    raw(core_media, "_CMSampleBufferGetSampleSize", [sample](CpuState& s, GuestMemory&) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        const auto index = static_cast<std::int64_t>(s.x[1]);
        s.x[0] = b->sizes.size() == 1 ? b->sizes[0] : index >= 0 && static_cast<std::size_t>(index) < b->sizes.size() ? b->sizes[static_cast<std::size_t>(index)] : 0;
    });
    raw(core_media, "_CMSampleBufferGetSampleSizeArray", [sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        if (s.x[3]) m.write64(s.x[3], b->sizes.size());
        if (b->sizes.empty()) { s.x[0] = status(sb_no_sizes); return; }
        if (s.x[2]) {
            if (s.x[1] < b->sizes.size()) { s.x[0] = status(sb_array_too_small); return; }
            for (std::size_t i = 0; i < b->sizes.size(); ++i) m.write64(s.x[2] + 8 * i, b->sizes[i]);
        }
        s.x[0] = 0;
    });
    raw(core_media, "_CMSampleBufferGetTotalSampleSize", [sample](CpuState& s, GuestMemory&) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        std::uint64_t total = 0;
        if (b->sizes.size() == 1) total = b->sizes[0] * static_cast<std::uint64_t>(std::max<std::int64_t>(b->samples, 0));
        else for (const auto size : b->sizes) total += size;
        s.x[0] = total;
    });
    raw(core_media, "_CMSampleBufferInvalidate", [sample](CpuState& s, GuestMemory&) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        b->invalidated = true;
        s.x[0] = 0;
    });
    raw(core_media, "_CMSampleBufferDataIsReady", [sample](CpuState& s, GuestMemory&) {
        static_cast<void>(sample(s.x[0]));
        s.x[0] = 1;
    });
    for (const auto* name : {"_CMSampleBufferSetDataReady", "_CMSampleBufferMakeDataReady"})
        raw(core_media, name, [sample](CpuState& s, GuestMemory&) {
            static_cast<void>(sample(s.x[0]));
            s.x[0] = 0;
        });
    raw(core_media, "_CMSampleBufferSetDataBuffer", [this, sample](CpuState& s, GuestMemory&) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        if (!s.x[1]) { s.x[0] = status(sb_required_parameter_missing); return; }
        if (b->data) { s.x[0] = status(sb_already_has_data); return; }
        b->data = retain(s.x[1]);
        s.x[0] = 0;
    });
    // A new buffer with the same data, format, image, sizes and buffer attachments (and timing, or the new timing).
    const auto copy = [this, sample](GuestMemory& m, Address source, const std::vector<std::uint8_t>* timing, Address out) -> std::int32_t {
        if (!out) return sb_required_parameter_missing;
        m.write64(out, 0);
        const auto b = sample(source);
        std::lock_guard lock(b->mutex);
        auto value = std::make_shared<SampleBuffer>();
        value->data = retain(b->data);
        value->format = retain(b->format);
        value->image = retain(b->image);
        value->samples = b->samples;
        value->timing = timing ? *timing : b->timing;
        value->sizes = b->sizes;
        value->packet_descriptions = b->packet_descriptions;
        for (const auto& a : b->buffer_attachments) value->buffer_attachments.push_back({retain(a.key), retain(a.value), a.mode});
        m.write64(out, make(sample_class, value));
        return 0;
    };
    raw(core_media, "_CMSampleBufferCreateCopy", [copy](CpuState& s, GuestMemory& m) { s.x[0] = status(copy(m, s.x[1], nullptr, s.x[2])); });
    // (allocator, original, CMItemCount numSampleTimingEntries, const CMSampleTimingInfo *array, out)
    raw(core_media, "_CMSampleBufferCreateCopyWithNewTiming", [copy](CpuState& s, GuestMemory& m) {
        std::vector<std::uint8_t> timing(static_cast<std::size_t>(std::min<std::uint64_t>(s.x[2], 1'000'000) * 72));
        if (!timing.empty() && s.x[3]) m.read_bytes(s.x[3], timing);
        s.x[0] = status(copy(m, s.x[1], &timing, s.x[4]));
    });
    // Audio sample buffers: one timing entry (a frame's duration at the format's rate, the given presentation
    // time); with packet descriptions, one size per packet; otherwise linear PCM's bytes per frame.
    const auto audio_buffer = [this](GuestMemory& m, Address data, Address format, std::int64_t samples, Address pts, Address descriptions,
                                     Address out) -> std::int32_t {
        if (!out || !format || !pts) {
            if (out) m.write64(out, 0);
            return sb_required_parameter_missing;
        }
        m.write64(out, 0);
        const auto f = objc.find_host_state<FormatDescription>(format);
        if (!f || f->media_type != fourcc("soun") || !f->stream_description) return sb_invalid_media_type;
        const auto rate = std::bit_cast<double>(m.read64(f->stream_description));
        const auto bytes_per_packet = m.read32(f->stream_description + 16);
        auto value = std::make_shared<SampleBuffer>();
        value->data = retain(data);
        value->format = retain(format);
        value->samples = samples;
        value->timing.assign(72, 0);
        put_timing_time(value->timing, 0, rate > 0 ? cm::make(1, static_cast<std::int32_t>(std::lround(rate))) : cm::time_invalid);
        put_timing_time(value->timing, 24, read_cmtime(m, pts));
        put_timing_time(value->timing, 48, cm::time_invalid);
        if (descriptions) {
            value->packet_descriptions.resize(static_cast<std::size_t>(samples) * 16);
            m.read_bytes(descriptions, value->packet_descriptions);
            for (std::int64_t i = 0; i < samples; ++i) value->sizes.push_back(m.read32(descriptions + 16 * i + 12));
        } else if (bytes_per_packet) {
            value->sizes.push_back(bytes_per_packet);
        }
        m.write64(out, make(sample_class, value));
        return 0;
    };
    // CMAudioSampleBufferCreateWithPacketDescriptions(allocator, dataBuffer, dataReady, callback, refcon, format,
    //     numSamples, const CMTime *pts (by reference), [stack] packetDescriptions, out)
    raw(core_media, "_CMAudioSampleBufferCreateWithPacketDescriptions", [audio_buffer](CpuState& s, GuestMemory& m) {
        s.x[0] = status(audio_buffer(m, s.x[1], s.x[5], static_cast<std::int64_t>(s.x[6]), s.x[7], m.read64(s.sp), m.read64(s.sp + 8)));
    });
    // CMAudioSampleBufferCreateReadyWithPacketDescriptions(allocator, dataBuffer, format, numSamples, pts, descriptions, out)
    raw(core_media, "_CMAudioSampleBufferCreateReadyWithPacketDescriptions", [audio_buffer](CpuState& s, GuestMemory& m) {
        s.x[0] = status(audio_buffer(m, s.x[1], s.x[2], static_cast<std::int64_t>(s.x[3]), s.x[4], s.x[5], s.x[6]));
    });
    // OSStatus (sbuf, size_t descriptionsSize, AudioStreamPacketDescription *out, size_t *sizeNeededOut)
    raw(core_media, "_CMSampleBufferGetAudioStreamPacketDescriptions", [sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        std::lock_guard lock(b->mutex);
        if (s.x[3]) m.write64(s.x[3], b->packet_descriptions.size());
        if (s.x[2] && !b->packet_descriptions.empty()) {
            if (s.x[1] < b->packet_descriptions.size()) { s.x[0] = status(sb_array_too_small); return; }
            m.write_bytes(s.x[2], b->packet_descriptions);
        }
        s.x[0] = 0;
    });
    // OSStatus CMSampleBufferSetDataBufferFromAudioBufferList(sbuf, allocator, allocator, flags, const AudioBufferList *):
    // the buffers' bytes, one after another, in a new block buffer.
    raw(core_media, "_CMSampleBufferSetDataBufferFromAudioBufferList", [this, sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        const auto list = s.x[4];
        if (!list) { s.x[0] = status(sb_required_parameter_missing); return; }
        std::vector<std::uint8_t> bytes;
        for (std::uint32_t i = 0, n = m.read32(list); i < n; ++i) {
            const auto at = list + 8 + 16ULL * i;
            std::vector<std::uint8_t> part(m.read32(at + 4));
            if (const auto from = m.read64(at + 8); from && !part.empty()) m.read_bytes(from, part);
            bytes.insert(bytes.end(), part.begin(), part.end());
        }
        std::lock_guard lock(b->mutex);
        if (b->data) { s.x[0] = status(sb_already_has_data); return; }
        std::int32_t error{};
        const auto block = create_block(0, bytes.size(), 0, 0, bytes.size(), 1, error);
        if (!block) { s.x[0] = status(error); return; }
        if (!bytes.empty()) transfer(m, *objc.find_host_state<BlockBuffer>(block), 0, bytes, true);
        b->data = block;
        s.x[0] = 0;
    });
    // OSStatus CMSampleBufferCopyPCMDataIntoAudioBufferList(sbuf, int32_t frameOffset, int32_t numFrames, AudioBufferList *):
    // linear PCM only; the data buffer holds the frames interleaved, or plane after plane when non-interleaved.
    raw(core_media, "_CMSampleBufferCopyPCMDataIntoAudioBufferList", [this, sample](CpuState& s, GuestMemory& m) {
        const auto b = sample(s.x[0]);
        const auto offset = static_cast<std::int32_t>(s.x[1]), frames = static_cast<std::int32_t>(s.x[2]);
        const auto list = s.x[3];
        std::lock_guard lock(b->mutex);
        const auto f = b->format ? objc.find_host_state<FormatDescription>(b->format) : nullptr;
        if (!list || !f || !f->stream_description || !b->data) { s.x[0] = status(sb_required_parameter_missing); return; }
        if (m.read32(f->stream_description + 8) != fourcc("lpcm")) { s.x[0] = status(sb_invalid_media_type); return; }
        const auto bytes_per_frame = m.read32(f->stream_description + 24);
        const bool non_interleaved = (m.read32(f->stream_description + 12) & 32) != 0;   // kAudioFormatFlagIsNonInterleaved
        const auto planes = non_interleaved ? m.read32(f->stream_description + 28) : 1U;
        if (offset < 0 || frames < 0 || offset + frames > b->samples || m.read32(list) < planes) { s.x[0] = status(-50); return; }
        const auto block = objc.find_host_state<BlockBuffer>(b->data);
        const auto plane_bytes = static_cast<std::uint64_t>(b->samples) * bytes_per_frame;
        for (std::uint32_t p = 0; p < planes; ++p) {
            const auto at = list + 8 + 16ULL * p;
            const auto wanted = static_cast<std::size_t>(frames) * bytes_per_frame;
            if (m.read32(at + 4) < wanted || !m.read64(at + 8)) { s.x[0] = status(-50); return; }
            std::vector<std::uint8_t> part(wanted);
            if (wanted) transfer(m, *block, p * plane_bytes + static_cast<std::uint64_t>(offset) * bytes_per_frame, part, false);
            m.write_bytes(m.read64(at + 8), part);
            m.write32(at + 4, static_cast<std::uint32_t>(wanted));
        }
        s.x[0] = 0;
    });
}

// CMFormatDescription comparison and generic creation; audio cookies and format lists; video/image matching.
void CoreMedia::install_format_description_more() {
    // CMFormatDescriptionCreate(allocator, mediaType, mediaSubType, extensions, out)
    raw(core_media, "_CMFormatDescriptionCreate", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[4]) { s.x[0] = status(fd_invalid_parameter); return; }
        auto value = std::make_shared<FormatDescription>();
        value->media_type = static_cast<std::uint32_t>(s.x[1]);
        value->subtype = static_cast<std::uint32_t>(s.x[2]);
        value->extensions = s.x[3] ? cf.copy(s.x[3]) : 0;
        m.write64(s.x[4], make(format_class, value));
        s.x[0] = 0;
    });
    const auto same = [this](GuestMemory& m, Address a_object, Address b_object, bool extensions) {
        if (a_object == b_object) return true;
        const auto a = a_object ? objc.find_host_state<FormatDescription>(a_object) : nullptr;
        const auto b = b_object ? objc.find_host_state<FormatDescription>(b_object) : nullptr;
        if (!a || !b || a->media_type != b->media_type || a->subtype != b->subtype || a->width != b->width || a->height != b->height) return false;
        if (a->parameter_sets != b->parameter_sets) return false;
        if ((a->stream_description != 0) != (b->stream_description != 0)) return false;
        if (a->stream_description) {
            std::vector<std::uint8_t> x(40), y(40);
            m.read_bytes(a->stream_description, x);
            m.read_bytes(b->stream_description, y);
            if (x != y) return false;
        }
        if (!extensions) return true;
        if (!a->extensions || !b->extensions) return !a->extensions && !b->extensions;
        return cf.equal(a->extensions, b->extensions);
    };
    raw(core_media, "_CMFormatDescriptionEqual", [same](CpuState& s, GuestMemory& m) { s.x[0] = same(m, s.x[0], s.x[1], true) ? 1 : 0; });
    // (a, b, CFTypeRef extensionKeysToIgnore, CFTypeRef atomKeysToIgnore): keys to ignore are not looked at
    // individually; extensions are compared only when neither list is given.
    raw(core_media, "_CMFormatDescriptionEqualIgnoringExtensionKeys", [same](CpuState& s, GuestMemory& m) {
        s.x[0] = same(m, s.x[0], s.x[1], !s.x[2] && !s.x[3]) ? 1 : 0;
    });
    // Boolean CMAudioFormatDescriptionEqual(a, b, CMAudioFormatDescriptionMask equalityMask, CMAudioFormatDescriptionMask *out)
    raw(core_media, "_CMAudioFormatDescriptionEqual", [this, same](CpuState& s, GuestMemory& m) {
        const auto equal = same(m, s.x[0], s.x[1], (s.x[2] & 8) != 0);   // kCMAudioFormatDescriptionMask_Extensions
        if (s.x[3]) m.write32(s.x[3], equal ? static_cast<std::uint32_t>(s.x[2]) : 0U);
        s.x[0] = equal ? 1 : 0;
    });
    const auto audio = [this](Address object) -> std::shared_ptr<FormatDescription> {
        const auto value = object ? objc.find_host_state<FormatDescription>(object) : nullptr;
        return value && value->media_type == fourcc("soun") ? value : nullptr;
    };
    raw(core_media, "_CMAudioFormatDescriptionGetMagicCookie", [audio](CpuState& s, GuestMemory& m) {
        const auto value = audio(s.x[0]);
        if (s.x[1]) m.write64(s.x[1], value ? value->cookie_size : 0);
        s.x[0] = value ? value->cookie : 0;
    });
    // AudioFormatListItem {AudioStreamBasicDescription; AudioChannelLayoutTag}: the one format of the description.
    const auto format_list = [this, audio](GuestMemory& m, Address object) -> Address {
        const auto value = audio(object);
        if (!value || !value->stream_description) return 0;
        if (!value->format_list) {
            value->format_list = malloc(48);
            std::vector<std::uint8_t> item(48, 0);
            m.read_bytes(value->stream_description, std::span(item).first(40));
            if (value->channel_layout) {
                const auto tag = m.read32(value->channel_layout);
                std::memcpy(item.data() + 40, &tag, 4);
            }
            m.write_bytes(value->format_list, item);
        }
        return value->format_list;
    };
    raw(core_media, "_CMAudioFormatDescriptionGetFormatList", [format_list](CpuState& s, GuestMemory& m) {
        const auto list = format_list(m, s.x[0]);
        if (s.x[1]) m.write64(s.x[1], list ? 48 : 0);
        s.x[0] = list;
    });
    for (const auto* name : {"_CMAudioFormatDescriptionGetMostCompatibleFormat", "_CMAudioFormatDescriptionGetRichestDecodableFormat"})
        raw(core_media, name, [format_list](CpuState& s, GuestMemory& m) { s.x[0] = format_list(m, s.x[0]); });
    // Boolean CMVideoFormatDescriptionMatchesImageBuffer(desc, imageBuffer): the size and (for an uncompressed
    // description) the pixel format.
    raw(core_media, "_CMVideoFormatDescriptionMatchesImageBuffer", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<FormatDescription>(s.x[0]) : nullptr;
        if (!value || !s.x[1] || value->media_type != fourcc("vide")) { s.x[0] = 0; return; }
        const auto width = call(core_video, "_CVPixelBufferGetWidth", {s.x[1]}), height = call(core_video, "_CVPixelBufferGetHeight", {s.x[1]});
        const auto format = static_cast<std::uint32_t>(call(core_video, "_CVPixelBufferGetPixelFormatType", {s.x[1]}));
        s.x[0] = static_cast<std::int64_t>(width) == value->width && static_cast<std::int64_t>(height) == value->height && format == value->subtype ? 1 : 0;
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
    install_attachments();
    install_time_more();
    install_sample_buffer_more();
    install_format_description_more();
}

} // namespace

void install_core_media(InstallContext& context) { std::make_shared<CoreMedia>(context)->install(); }

} // namespace alchemy::gui::fill
