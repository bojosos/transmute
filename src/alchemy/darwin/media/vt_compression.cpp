#include "alchemy/darwin/media/media.hpp"
#include "alchemy/darwin/media/video_encoder.hpp"
#include "alchemy/darwin/media/vt_session.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// VTCompressionSession (<VideoToolbox/VTCompressionSession.h>) for H.264 ('avc1') and HEVC ('hvc1'), over the
// host system's encoders (video_encoder.hpp; on Windows, Media Foundation's). No encoder is shipped with Alchemy:
// a machine without one answers kVTCouldNotFindVideoEncoderErr from VTCompressionSessionCreate.
// - Create picks the encoders of the codec its encoder specification allows: kVTVideoEncoderSpecification_EncoderID
//   (Alchemy's IDs, from VTCopyVideoEncoderList), RequireHardwareAcceleratedVideoEncoder, and
//   EnableHardwareAcceleratedVideoEncoder false (software encoders only); RequiredEncoderGPURegistryID names a
//   GPU no listed encoder is known to be on, so none qualifies. The size
//   must be even (the encoders take 4:2:0 pictures of whole chroma samples).
// - The encoder opens with the session's properties at the first frame or PrepareToEncodeFrames: AverageBitRate
//   (DataRateLimits as a ceiling on it), ExpectedFrameRate, MaxKeyFrameInterval and ...IntervalDuration,
//   AllowFrameReordering (B-frames; on by default), AllowTemporalCompression (off: every frame a keyframe),
//   ProfileLevel (H.264 Baseline, Main, High and HEVC Main, at a level or AutoLevel) and RealTime. Once it is
//   open those answer kVTPropertyReadOnlyErr: the running encoder is not retuned. Other compression keys answer
//   kVTPropertyNotSupportedErr (vt_session.hpp, pixel_transfer.cpp).
// - EncodeFrame takes any pixel buffer VTPixelTransferSession converts, scaled to the session's size as 4:2:0
//   video-range Y'CbCr; kVTEncodeFrameOptionKey_ForceKeyFrame makes a keyframe. Frames are encoded on the calling
//   thread and each compressed frame goes to the output callback (or the frame's handler) as soon as the encoder
//   gives it, in decode order, with its frame's sourceFrameRefCon, presentation time and duration (a decode time
//   when frames are reordered). An encoder may move its output times (Microsoft's software H.264 encoder shifts
//   them by its reordering delay and rounds them): the first packet of a stream is its first frame, so its offset
//   from that frame's time is the encoder's shift, and each packet is the waiting frame nearest its time less the
//   shift (within half a frame); its decode time, less the shift, is never after the frame's presentation time.
//   Its format description is made from the encoder's parameter sets (with the avcC
//   or hvcC atom, coremedia.cpp); frames other than keyframes carry NotSync and DependsOnOthers.
//   kVTEncodeInfo_Asynchronous is never set. CompleteFrames delivers what the encoder still holds; a frame it
//   never gave is reported with kVTEncodeInfo_FrameDropped.
// - Multi-pass encoding is not available: VTMultiPassStorageCreate answers kVTCouldNotCreateInstanceErr, so no
//   session has a multi-pass storage and the pass calls answer kVTParameterErr.
namespace alchemy::gui::fill {
namespace {

namespace enc = completion_kit::media::video_encoding;

// VTErrors.h.
constexpr std::int32_t vt_property_read_only = -12901, vt_parameter = -12902, vt_invalid_session = -12903, vt_could_not_create_instance = -12907,
    vt_could_not_find_encoder = -12908, vt_encoder_malfunction = -12912;
constexpr std::uint32_t info_frame_dropped = 2;   // kVTEncodeInfo_FrameDropped
constexpr std::uint32_t pixel_format_420v = fourcc("420v");

struct Pending {
    std::int64_t key{};                                // the presentation time in 100 ns (what the encoder echoes)
    std::array<std::uint8_t, 24> pts{}, duration{};    // the frame's CMTimes
    Address refcon{}, handler{};                       // sourceFrameRefCon; the frame's _Block_copy'd handler or 0
};

// The settings the session's properties give, or the status that refuses a value.
struct Tuning {
    std::optional<double> bit_rate, ceiling, frame_rate, key_interval, key_duration;
    bool reordering{true}, temporal{true}, real_time{};
    enc::Profile profile{};
    std::uint32_t level{};
};

struct Compressing {
    std::recursive_mutex mutex;   // held while frames go to the callbacks, which may ask the session for properties
    enc::Codec codec{};
    std::uint32_t codec_type{}, width{}, height{};
    std::string encoder_id;                            // the encoder specification's, or empty
    bool require_hardware{}, require_software{};
    std::optional<enc::EncoderInfo> expected;          // the encoder the specification selects first
    Address callback{}, refcon{};
    Address source_attributes{}, pool{};               // retained
    Address transfer{}, converted{};                   // VTPixelTransferSession and its 4:2:0 destination (retained)
    std::unique_ptr<enc::Encoder> encoder;
    bool started{};
    Address format{};                                  // retained CMVideoFormatDescription
    std::vector<std::vector<std::uint8_t>> format_sets;
    std::deque<Pending> pending;
    std::int64_t last_key{INT64_MIN};
    std::optional<std::int64_t> shift;                 // the encoder's output time less the frame's, for this stream
    double frame_rate{30};
    bool temporal{true};
};

std::int64_t time_value(const std::array<std::uint8_t, 24>& t) { std::int64_t v; std::memcpy(&v, t.data(), 8); return v; }
std::int32_t time_scale(const std::array<std::uint8_t, 24>& t) { std::int32_t v; std::memcpy(&v, t.data() + 8, 4); return v; }
std::uint32_t time_flags(const std::array<std::uint8_t, 24>& t) { std::uint32_t v; std::memcpy(&v, t.data() + 12, 4); return v; }
std::array<std::uint8_t, 24> make_time(std::int64_t value, std::int32_t scale) {
    std::array<std::uint8_t, 24> t{};
    const std::uint32_t valid = 1;   // kCMTimeFlags_Valid
    std::memcpy(t.data(), &value, 8);
    std::memcpy(t.data() + 8, &scale, 4);
    std::memcpy(t.data() + 12, &valid, 4);
    return t;
}

class Compression final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    std::shared_ptr<Compressing> compressing(Address session) {
        std::lock_guard lock(mutex_);
        const auto found = sessions_.find(session);
        return found == sessions_.end() ? nullptr : found->second;
    }
    std::int32_t check(Address session, std::shared_ptr<Compressing>& out) {
        if (!session) return vt_parameter;
        const auto state = objc.find_host_state<VTSessionState>(session);
        out = compressing(session);
        if (!state || state->kind != VTSessionState::Kind::compression || !out) return vt_invalid_session;
        return state->invalid ? vt_invalid_session : 0;
    }
    // The encoders an encoder specification allows for `codec`, in order.
    std::vector<enc::EncoderInfo> allowed(enc::Codec codec, Address specification, std::string& id, bool& hardware, bool& software, bool& gpu);
    std::int32_t create(GuestMemory& m, const CpuState& s);
    std::int32_t refuse(Compressing& c, const std::string& key, Address value);
    std::int32_t tuning(Address session, Tuning& t);
    std::int32_t start(Address session, Compressing& c);
    std::int32_t encode(Address session, Compressing& c, Address image, Address pts, Address duration, Address options, Address refcon, Address handler);
    bool picture(Compressing& c, Address image, std::vector<std::uint8_t>& luma, std::vector<std::uint8_t>& chroma);
    void deliver(Compressing& c, std::vector<enc::Packet>& packets);
    void send(const Compressing& c, const Pending& p, std::int32_t result, std::uint32_t flags, Address sample);
    Address sample_buffer(Compressing& c, const enc::Packet& packet, const Pending& p);
    bool refresh_format(Compressing& c);
    void drop_all(Compressing& c);
    Address pool(Compressing& c);
    void finish(Compressing& c);

    std::mutex mutex_;
    std::map<Address, std::shared_ptr<Compressing>> sessions_;
    Address class_{};
};

std::vector<enc::EncoderInfo> Compression::allowed(enc::Codec codec, Address specification, std::string& id, bool& hardware, bool& software, bool& gpu) {
    id.clear();
    hardware = software = gpu = false;
    if (specification && cf.kind(specification) == ObjectKind::dictionary) {
        for (const auto& [k, v] : cf.dictionary_entries(specification)) {
            const auto name = cf.utf8(k);
            if (name == "EncoderID" && v && cf.kind(v) == ObjectKind::string) id = cf.utf8(v);
            else if (name == "RequireHardwareAcceleratedVideoEncoder") hardware = v == cf.boolean(true);
            else if (name == "EnableHardwareAcceleratedVideoEncoder") software = v == cf.boolean(false);
            else if (name == "RequiredEncoderGPURegistryID") gpu = true;
        }
    }
    std::vector<enc::EncoderInfo> out;
    if (gpu) return out;
    for (auto& e : enc::encoders()) {
        if (e.codec != codec || (!id.empty() && e.id != id) || (hardware && !e.hardware) || (software && e.hardware)) continue;
        out.push_back(std::move(e));
    }
    return out;
}

// VTCompressionSessionCreate(allocator, width, height, codecType, encoderSpecification, sourceImageBufferAttributes,
//     compressedDataAllocator, outputCallback, [stack] outputCallbackRefCon, VTCompressionSessionRef *out)
std::int32_t Compression::create(GuestMemory& m, const CpuState& s) {
    const auto out = m.read64(s.sp + 8);
    if (!out) return vt_parameter;
    m.write64(out, 0);
    const auto width = static_cast<std::int32_t>(s.x[1]), height = static_cast<std::int32_t>(s.x[2]);
    const auto codec_type = static_cast<std::uint32_t>(s.x[3]);
    if (width <= 0 || height <= 0) return vt_parameter;
    if (codec_type != fourcc("avc1") && codec_type != fourcc("hvc1")) return vt_could_not_find_encoder;   // ProRes, JPEG, ...
    auto c = std::make_shared<Compressing>();
    c->codec = codec_type == fourcc("avc1") ? enc::Codec::h264 : enc::Codec::hevc;
    c->codec_type = codec_type;
    c->width = static_cast<std::uint32_t>(width);
    c->height = static_cast<std::uint32_t>(height);
    bool gpu = false;
    const auto candidates = allowed(c->codec, s.x[4], c->encoder_id, c->require_hardware, c->require_software, gpu);
    if (candidates.empty() || (c->width & 1) || (c->height & 1)) return vt_could_not_find_encoder;
    c->expected = candidates.front();
    c->source_attributes = s.x[5] ? retain(s.x[5]) : 0;
    c->callback = s.x[7];
    c->refcon = m.read64(s.sp);
    const auto session = objc.allocate(class_);
    const auto state = objc.host_state<VTSessionState>(session);
    state->kind = VTSessionState::Kind::compression;
    const std::weak_ptr<Compressing> weak = c;
    state->check_set = [this, weak](const std::string& key, Address value) -> std::int32_t {
        const auto locked = weak.lock();
        if (!locked) return vt_invalid_session;
        std::lock_guard lock(locked->mutex);
        return refuse(*locked, key, value);
    };
    state->computed = [this, weak](const std::string& key) -> Address {
        const auto locked = weak.lock();
        if (!locked || key != "UsingHardwareAcceleratedVideoEncoder") return 0;
        std::lock_guard lock(locked->mutex);
        const bool hardware = locked->encoder ? locked->encoder->info().hardware : locked->expected && locked->expected->hardware;
        return retain(cf.boolean(hardware));
    };
    {
        std::lock_guard lock(mutex_);
        sessions_[session] = std::move(c);
    }
    m.write64(out, session);
    return 0;
}

// Whether a property value is refused (the status) or kept (0).
std::int32_t Compression::refuse(Compressing& c, const std::string& key, Address value) {
    if (key == "UsingHardwareAcceleratedVideoEncoder" || c.started) return vt_property_read_only;
    if (!value) return 0;   // back to the default
    const auto kind = cf.kind(value);
    const auto number = [&] { return kind == ObjectKind::number ? cf.number_value(value) : std::nullopt; };
    if (key == "AllowFrameReordering" || key == "AllowTemporalCompression" || key == "RealTime")
        return kind == ObjectKind::boolean ? 0 : vt_parameter;
    if (key == "AverageBitRate" || key == "ExpectedFrameRate" || key == "MaxKeyFrameInterval" || key == "MaxKeyFrameIntervalDuration") {
        const auto v = number();
        return v && *v >= 0 ? 0 : vt_parameter;
    }
    if (key == "DataRateLimits") {
        // [bytes, seconds, ...] pairs of numbers.
        const auto values = kind == ObjectKind::array ? cf.array_values(value) : std::vector<Address>{};
        if (values.empty() || values.size() % 2) return vt_parameter;
        for (const auto v : values)
            if (!cf.number_value(v) || *cf.number_value(v) < 0) return vt_parameter;
        return 0;
    }
    if (key == "ProfileLevel") {
        // H264_{Baseline,Main,High}_{<major>_<minor>,AutoLevel}, HEVC_Main_AutoLevel. HEVC Main10 needs 10-bit
        // pictures, which the encoders here are not given.
        if (kind != ObjectKind::string) return vt_parameter;
        const auto name = cf.utf8(value);
        const bool h264 = name.starts_with("H264_Baseline_") || name.starts_with("H264_Main_") || name.starts_with("H264_High_");
        const bool hevc = name == "HEVC_Main_AutoLevel";
        if ((c.codec == enc::Codec::h264 && !h264) || (c.codec == enc::Codec::hevc && !hevc)) return vt_parameter;
        if (h264) {
            const auto level = name.substr(name.find('_', 5) + 1);
            static constexpr std::array<const char*, 17> levels{"AutoLevel", "1_3", "2_0", "2_1", "2_2", "3_0", "3_1", "3_2", "4_0", "4_1", "4_2",
                                                                "5_0", "5_1", "5_2", "1_1", "1_2", "1_0"};
            if (std::find(levels.begin(), levels.end(), level) == levels.end()) return vt_parameter;
        }
        return 0;
    }
    return 0;
}

// The encoder settings of the session's kept properties.
std::int32_t Compression::tuning(Address session, Tuning& t) {
    const auto state = objc.find_host_state<VTSessionState>(session);
    std::map<std::string, Address> kept;
    {
        std::lock_guard lock(state->mutex);
        kept = state->properties;
    }
    const auto number = [&](const char* key) -> std::optional<double> {
        const auto found = kept.find(key);
        return found != kept.end() && found->second ? cf.number_value(found->second) : std::nullopt;
    };
    const auto flag = [&](const char* key, bool fallback) {
        const auto found = kept.find(key);
        return found != kept.end() && found->second ? found->second == cf.boolean(true) : fallback;
    };
    t.bit_rate = number("AverageBitRate");
    t.frame_rate = number("ExpectedFrameRate");
    t.key_interval = number("MaxKeyFrameInterval");
    t.key_duration = number("MaxKeyFrameIntervalDuration");
    t.reordering = flag("AllowFrameReordering", true);
    t.temporal = flag("AllowTemporalCompression", true);
    t.real_time = flag("RealTime", false);
    if (const auto found = kept.find("DataRateLimits"); found != kept.end() && found->second) {
        const auto values = cf.array_values(found->second);
        for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
            const auto bytes = cf.number_value(values[i]).value_or(0), seconds = cf.number_value(values[i + 1]).value_or(0);
            if (bytes > 0 && seconds > 0) t.ceiling = std::min(t.ceiling.value_or(1e300), bytes * 8 / seconds);
        }
    }
    if (const auto found = kept.find("ProfileLevel"); found != kept.end() && found->second) {
        const auto name = cf.utf8(found->second);
        t.profile = name.starts_with("H264_Baseline_") ? enc::Profile::h264_baseline : name.starts_with("H264_Main_") ? enc::Profile::h264_main
                  : name.starts_with("H264_High_") ? enc::Profile::h264_high : enc::Profile::hevc_main;
        // H.264 levels: <major>_<minor> is level_idc 10 * major + minor.
        if (name.starts_with("H264_") && !name.ends_with("AutoLevel")) {
            const auto level = name.substr(name.find('_', 5) + 1);
            t.level = static_cast<std::uint32_t>((level[0] - '0') * 10 + (level[2] - '0'));
        }
    }
    return 0;
}

// Opens the encoder with the session's settings.
std::int32_t Compression::start(Address session, Compressing& c) {
    if (c.encoder) return 0;
    Tuning t;
    if (const auto refused = tuning(session, t)) return refused;
    enc::Settings settings;
    settings.codec = c.codec;
    settings.width = c.width;
    settings.height = c.height;
    settings.frame_rate = t.frame_rate && *t.frame_rate > 0 ? *t.frame_rate : 30;
    double bits = t.bit_rate && *t.bit_rate > 0 ? *t.bit_rate : 0;
    if (t.ceiling) bits = bits > 0 ? std::min(bits, *t.ceiling) : *t.ceiling;
    settings.bit_rate = static_cast<std::uint32_t>(std::min(bits, 4e9));
    std::optional<double> interval;
    if (t.key_interval && *t.key_interval > 0) interval = *t.key_interval;
    if (t.key_duration && *t.key_duration > 0) interval = std::min(interval.value_or(1e300), std::max(1.0, std::floor(*t.key_duration * settings.frame_rate)));
    settings.keyframe_interval = interval ? static_cast<std::uint32_t>(std::min(*interval, 1e9)) : 0;
    settings.profile = t.profile;
    settings.level = t.level;
    settings.real_time = t.real_time;
    settings.frame_reordering = t.reordering && t.temporal && t.profile != enc::Profile::h264_baseline;
    settings.require_hardware = c.require_hardware;
    settings.require_software = c.require_software;
    settings.encoder_id = c.encoder_id;
    enc::OpenError error{};
    c.encoder = enc::open(settings, error);
    if (!c.encoder) return error == enc::OpenError::not_found ? vt_could_not_find_encoder : vt_parameter;
    c.started = true;
    c.frame_rate = settings.frame_rate;
    c.temporal = t.temporal;
    return 0;
}

// The picture of `image` as 4:2:0 video-range Y'CbCr of the session's size: its planes as they are when it is
// one already, else through the session's pixel transfer.
bool Compression::picture(Compressing& c, Address image, std::vector<std::uint8_t>& luma, std::vector<std::uint8_t>& chroma) {
    auto& m = process.calling_engine();
    Address source = image;
    const auto format = static_cast<std::uint32_t>(call(core_video, "_CVPixelBufferGetPixelFormatType", {image}));
    if (format != pixel_format_420v || call(core_video, "_CVPixelBufferGetWidth", {image}) != c.width ||
        call(core_video, "_CVPixelBufferGetHeight", {image}) != c.height) {
        const auto out = process.allocate_guest(8, 8);
        if (!c.transfer) {
            m.write64(out, 0);
            if (call(video_toolbox, "_VTPixelTransferSessionCreate", {0, out}) == 0) c.transfer = m.read64(out);
        }
        if (!c.converted) {
            m.write64(out, 0);
            if (call(core_video, "_CVPixelBufferCreate", {0, c.width, c.height, pixel_format_420v, 0, out}) == 0) c.converted = m.read64(out);
        }
        process.release_guest(out);
        if (!c.transfer || !c.converted) return false;
        if (static_cast<std::int32_t>(call(video_toolbox, "_VTPixelTransferSessionTransferImage", {c.transfer, image, c.converted}))) return false;
        source = c.converted;
    }
    static_cast<void>(call(core_video, "_CVPixelBufferLockBaseAddress", {source, 1}));   // kCVPixelBufferLock_ReadOnly
    bool ok = call(core_video, "_CVPixelBufferGetPlaneCount", {source}) == 2;
    for (Address plane = 0; ok && plane < 2; ++plane) {
        const auto base = call(core_video, "_CVPixelBufferGetBaseAddressOfPlane", {source, plane});
        const auto row = call(core_video, "_CVPixelBufferGetBytesPerRowOfPlane", {source, plane});
        const auto rows = plane ? c.height / 2 : c.height;
        auto& out = plane ? chroma : luma;
        out.resize(static_cast<std::size_t>(c.width) * rows);
        ok = base && row >= c.width;
        for (std::uint32_t y = 0; ok && y < rows; ++y) m.read_bytes(base + y * row, std::span(out.data() + std::size_t(y) * c.width, c.width));
    }
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {source, 1}));
    return ok;
}

// A new format description when the encoder's parameter sets are not the current one's.
bool Compression::refresh_format(Compressing& c) {
    auto sets = c.encoder->parameter_sets();
    if (sets.empty()) return c.format != 0;
    if (c.format && sets == c.format_sets) return true;
    auto& m = process.calling_engine();
    const auto count = sets.size();
    const auto pointers = process.allocate_guest(count * 16, 8);
    std::vector<Address> copies;
    for (std::size_t i = 0; i < count; ++i) {
        const auto copy = process.allocate_guest(sets[i].size(), 8);
        m.write_bytes(copy, sets[i]);
        copies.push_back(copy);
        m.write64(pointers + 8 * i, copy);
        m.write64(pointers + 8 * count + 8 * i, sets[i].size());
    }
    const auto out = process.allocate_guest(8, 8);
    m.write64(out, 0);
    // CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator, count, sets, sizes, NALUnitHeaderLength, out);
    // the HEVC one takes extensions before out.
    const auto result = c.codec == enc::Codec::h264
        ? call(core_media, "_CMVideoFormatDescriptionCreateFromH264ParameterSets", {0, count, pointers, pointers + 8 * count, 4, out})
        : call(core_media, "_CMVideoFormatDescriptionCreateFromHEVCParameterSets", {0, count, pointers, pointers + 8 * count, 4, 0, out});
    const auto made = m.read64(out);
    process.release_guest(out);
    for (const auto copy : copies) process.release_guest(copy);
    process.release_guest(pointers);
    if (static_cast<std::int32_t>(result) || !made) return false;
    release(std::exchange(c.format, made));
    c.format_sets = std::move(sets);
    return true;
}

Address Compression::sample_buffer(Compressing& c, const enc::Packet& packet, const Pending& p) {
    if (!refresh_format(c)) return 0;
    auto& m = process.calling_engine();
    const auto size = packet.data.size();
    // CMBlockBufferCreateWithMemoryBlock(allocator, memoryBlock, blockLength, blockAllocator, customBlockSource, offset,
    //     dataLength, flags, [stack] CMBlockBufferRef *out)
    const auto block_out = process.allocate_guest(8, 8);
    m.write64(block_out, 0);
    CallRegs create{};
    create.x[2] = size;
    create.x[6] = size;
    create.x[7] = 1;   // kCMBlockBufferAssureMemoryNowFlag
    create.push_stack(block_out);
    const bool made_block = !static_cast<std::int32_t>(call_regs(core_media, "_CMBlockBufferCreateWithMemoryBlock", create).x0);
    const auto block = m.read64(block_out);
    process.release_guest(block_out);
    if (!made_block || !block) return 0;
    const auto staging = process.allocate_guest(size, 16);
    m.write_bytes(staging, packet.data);
    static_cast<void>(call(core_media, "_CMBlockBufferReplaceDataBytes", {staging, block, 0, size}));
    process.release_guest(staging);
    // One CMSampleTimingInfo {duration, presentationTimeStamp, decodeTimeStamp} and one size.
    const auto timing = process.allocate_guest(72 + 8, 8);
    auto duration = p.duration;
    if (!(time_flags(duration) & 1)) duration = make_time(packet.duration, 10000000);
    m.write_bytes(timing, duration);
    m.write_bytes(timing + 24, p.pts);
    // The decode time the encoder gives, in the presentation time's timescale.
    const auto scale = time_scale(p.pts);
    m.write_bytes(timing + 48, make_time(static_cast<std::int64_t>(std::llround(static_cast<double>(packet.dts) * scale / 1e7)), scale));
    m.write64(timing + 72, size);
    const auto sample_out = process.allocate_guest(8, 8);
    m.write64(sample_out, 0);
    // CMSampleBufferCreateReady(allocator, dataBuffer, formatDescription, numSamples, numSampleTimingEntries, timing,
    //     numSampleSizeEntries, sizes, [stack] CMSampleBufferRef *out)
    CallRegs ready{};
    ready.x[1] = block; ready.x[2] = c.format; ready.x[3] = 1; ready.x[4] = 1; ready.x[5] = timing; ready.x[6] = 1; ready.x[7] = timing + 72;
    ready.push_stack(sample_out);
    const auto result = static_cast<std::int32_t>(call_regs(core_media, "_CMSampleBufferCreateReady", ready).x0);
    const auto sample = m.read64(sample_out);
    process.release_guest(sample_out);
    process.release_guest(timing);
    release(block);
    if (result || !sample) return 0;
    // kCMSampleAttachmentKey_NotSync and _DependsOnOthers.
    const auto attachments = call(core_media, "_CMSampleBufferGetSampleAttachmentsArray", {sample, 1});
    if (const auto first = attachments ? cf.element(attachments, 0) : 0) {
        const auto not_sync = cf.string("NotSync"), depends = cf.string("DependsOnOthers");
        if (!packet.keyframe) static_cast<void>(call(core_foundation, "_CFDictionarySetValue", {first, not_sync, cf.boolean(true)}));
        static_cast<void>(call(core_foundation, "_CFDictionarySetValue", {first, depends, cf.boolean(!packet.keyframe)}));
        release(not_sync);
        release(depends);
    }
    return sample;
}

// One frame's result to its handler or the session's callback:
// VTCompressionOutputCallback(outputCallbackRefCon, sourceFrameRefCon, OSStatus, VTEncodeInfoFlags, CMSampleBufferRef);
// the handler block (OSStatus, VTEncodeInfoFlags, CMSampleBufferRef).
void Compression::send(const Compressing& c, const Pending& p, std::int32_t result, std::uint32_t flags, Address sample) {
    const auto code = static_cast<Address>(static_cast<std::int64_t>(result));
    if (p.handler) {
        static_cast<void>(bridge->invoke_block(p.handler, {code, flags, sample}));
        static_cast<void>(call(libsystem, "__Block_release", {p.handler}));
    } else if (c.callback) {
        static_cast<void>(EngineAdapter(process.calling_engine()).call(c.callback, std::array<Address, 5>{c.refcon, p.refcon, code, flags, sample}));
    }
}

void Compression::deliver(Compressing& c, std::vector<enc::Packet>& packets) {
    const auto tolerance = static_cast<std::int64_t>(5e6 / c.frame_rate);   // half a frame, in 100 ns
    for (auto packet : packets) {
        if (c.pending.empty()) continue;
        if (!c.shift) c.shift = packet.pts - c.pending.front().key;   // the stream's first packet is its first frame
        const auto target = packet.pts - *c.shift;
        auto found = c.pending.end();
        for (auto it = c.pending.begin(); it != c.pending.end(); ++it)
            if (std::llabs(it->key - target) <= tolerance && (found == c.pending.end() || std::llabs(it->key - target) < std::llabs(found->key - target)))
                found = it;
        if (found == c.pending.end()) continue;
        const auto p = *found;
        c.pending.erase(found);
        packet.pts = p.key;
        packet.dts = std::min(packet.dts - *c.shift, p.key);
        const auto sample = sample_buffer(c, packet, p);
        send(c, p, sample ? 0 : vt_encoder_malfunction, sample ? 0 : info_frame_dropped, sample);
        if (sample) release(sample);
    }
    packets.clear();
}

void Compression::drop_all(Compressing& c) {
    while (!c.pending.empty()) {
        const auto p = c.pending.front();
        c.pending.pop_front();
        send(c, p, 0, info_frame_dropped, 0);
    }
}

std::int32_t Compression::encode(Address session, Compressing& c, Address image, Address pts_at, Address duration_at, Address options, Address refcon,
                                 Address handler) {
    if (!image || !pts_at || call(core_foundation, "_CFGetTypeID", {image}) != call(core_video, "_CVPixelBufferGetTypeID", {})) return vt_parameter;
    auto& m = process.calling_engine();
    Pending p;
    m.read_bytes(pts_at, p.pts);
    if (duration_at) m.read_bytes(duration_at, p.duration);
    if (!(time_flags(p.pts) & 1) || time_scale(p.pts) <= 0) return vt_parameter;
    p.key = static_cast<std::int64_t>(std::llround(static_cast<double>(time_value(p.pts)) * 1e7 / time_scale(p.pts)));
    if (p.key <= c.last_key) return vt_parameter;   // presentation times increase
    if (const auto refused = start(session, c)) return refused;
    std::vector<std::uint8_t> luma, chroma;
    if (!picture(c, image, luma, chroma)) return vt_parameter;
    bool key = !c.temporal;
    if (options && cf.kind(options) == ObjectKind::dictionary) {
        const auto name = cf.string("ForceKeyFrame");
        key = key || cf.dictionary_value(options, name) == cf.boolean(true);
        release(name);
    }
    std::int64_t duration = time_flags(p.duration) & 1 && time_scale(p.duration) > 0
        ? static_cast<std::int64_t>(std::llround(static_cast<double>(time_value(p.duration)) * 1e7 / time_scale(p.duration)))
        : static_cast<std::int64_t>(std::llround(1e7 / c.frame_rate));
    p.refcon = refcon;
    p.handler = handler ? call(libsystem, "__Block_copy", {handler}) : 0;
    c.last_key = p.key;
    c.pending.push_back(p);
    const enc::Picture picture{c.width, c.height, luma.data(), c.width, chroma.data(), c.width, p.key, std::max<std::int64_t>(duration, 1)};
    std::vector<enc::Packet> packets;
    const bool ok = c.encoder->encode(picture, key, packets);
    deliver(c, packets);
    if (!ok) {
        // The frame (and any the encoder still held) did not come out.
        const auto found = std::find_if(c.pending.begin(), c.pending.end(), [&](const Pending& e) { return e.key == p.key; });
        if (found != c.pending.end()) {
            const auto failed = *found;
            c.pending.erase(found);
            send(c, failed, vt_encoder_malfunction, info_frame_dropped, 0);
        }
        return vt_encoder_malfunction;
    }
    return 0;
}

// VTCompressionSessionGetPixelBufferPool: a pool of the session's size in the source attributes' format (4:2:0
// video range when they name none), made once.
Address Compression::pool(Compressing& c) {
    if (c.pool) return c.pool;
    std::vector<std::pair<Address, Address>> entries;
    const auto width_key = cf.string("Width"), height_key = cf.string("Height"), format_key = cf.string("PixelFormatType");
    bool has_format = false;
    if (c.source_attributes && cf.kind(c.source_attributes) == ObjectKind::dictionary)
        for (const auto& entry : cf.dictionary_entries(c.source_attributes)) {
            if (cf.equal(entry.first, width_key) || cf.equal(entry.first, height_key)) continue;
            has_format = has_format || cf.equal(entry.first, format_key);
            entries.push_back(entry);
        }
    const auto width = cf.integer(c.width), height = cf.integer(c.height), format = cf.integer(pixel_format_420v);
    entries.emplace_back(width_key, width);
    entries.emplace_back(height_key, height);
    if (!has_format) entries.emplace_back(format_key, format);
    const auto attributes = cf.dictionary(entries);
    auto& m = process.calling_engine();
    const auto out = process.allocate_guest(8, 8);
    m.write64(out, 0);
    // CVPixelBufferPoolCreate(allocator, poolAttributes, pixelBufferAttributes, CVPixelBufferPoolRef *out)
    if (call(core_video, "_CVPixelBufferPoolCreate", {0, 0, attributes, out}) == 0) c.pool = m.read64(out);
    process.release_guest(out);
    for (const auto object : {width_key, height_key, format_key, width, height, format, attributes}) release(object);
    return c.pool;
}

void Compression::finish(Compressing& c) {
    for (auto& p : c.pending)
        if (p.handler) static_cast<void>(call(libsystem, "__Block_release", {std::exchange(p.handler, 0)}));
    c.pending.clear();
    c.encoder.reset();
    release(std::exchange(c.format, 0));
    release(std::exchange(c.converted, 0));
    if (const auto transfer = std::exchange(c.transfer, 0)) {
        static_cast<void>(call(video_toolbox, "_VTPixelTransferSessionInvalidate", {transfer}));
        release(transfer);
    }
}

void Compression::install() {
    class_ = objc.find_class("__AlchemyVTCompressionSession");
    if (!class_) class_ = objc.register_native_class("__AlchemyVTCompressionSession", objc.find_class("NSObject"), 8);
    register_type(class_, TypeId::vt_compression_session);
    const auto cls = class_;
    static_cast<void>(objc.add_native_method(cls, "dealloc", [self = shared_from_this(), this, cls](CpuState& s, GuestMemory&) {
        if (const auto value = objc.find_host_state<VTSessionState>(s.x[0])) {
            std::map<std::string, Address> properties;
            {
                std::lock_guard lock(value->mutex);
                properties.swap(value->properties);
            }
            for (const auto& [name, v] : properties) release(v);
            value->check_set = nullptr;
            value->computed = nullptr;
        }
        std::shared_ptr<Compressing> gone;
        {
            std::lock_guard lock(mutex_);
            if (const auto found = sessions_.find(s.x[0]); found != sessions_.end()) {
                gone = std::move(found->second);
                sessions_.erase(found);
            }
        }
        if (gone) {
            std::lock_guard lock(gone->mutex);
            finish(*gone);
            release(std::exchange(gone->source_attributes, 0));
            release(std::exchange(gone->pool, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], cls, objc.selector("dealloc")));
    }, false, "v@:"));

    constant(video_toolbox, "_VTCompressionSessionGetTypeID", static_cast<Address>(TypeId::vt_compression_session));
    raw(video_toolbox, "_VTCompressionSessionCreate", [this](CpuState& s, GuestMemory& m) { s.x[0] = status(create(m, s)); });
    // VTCompressionSessionEncodeFrame(session, imageBuffer, CMTime pts, CMTime duration, frameProperties, sourceFrameRefcon,
    //     VTEncodeInfoFlags *infoFlagsOut): the CMTimes by reference.
    raw(video_toolbox, "_VTCompressionSessionEncodeFrame", [this](CpuState& s, GuestMemory& m) {
        if (s.x[6]) m.write32(s.x[6], 0);
        std::shared_ptr<Compressing> c;
        if (const auto result = check(s.x[0], c)) { s.x[0] = status(result); return; }
        std::lock_guard lock(c->mutex);
        if (!c->callback) { s.x[0] = status(vt_parameter); return; }   // made for output handlers
        s.x[0] = status(encode(s.x[0], *c, s.x[1], s.x[2], s.x[3], s.x[4], s.x[5], 0));
    });
    // VTCompressionSessionEncodeFrameWithOutputHandler(session, imageBuffer, CMTime pts, CMTime duration, frameProperties,
    //     VTEncodeInfoFlags *infoFlagsOut, handler)
    raw(video_toolbox, "_VTCompressionSessionEncodeFrameWithOutputHandler", [this](CpuState& s, GuestMemory& m) {
        if (s.x[5]) m.write32(s.x[5], 0);
        std::shared_ptr<Compressing> c;
        if (const auto result = check(s.x[0], c)) { s.x[0] = status(result); return; }
        if (!s.x[6]) { s.x[0] = status(vt_parameter); return; }
        std::lock_guard lock(c->mutex);
        s.x[0] = status(encode(s.x[0], *c, s.x[1], s.x[2], s.x[3], s.x[4], 0, s.x[6]));
    });
    // VTCompressionSessionCompleteFrames(session, CMTime completeUntilPresentationTimeStamp): every frame given so far.
    raw(video_toolbox, "_VTCompressionSessionCompleteFrames", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Compressing> c;
        if (const auto result = check(s.x[0], c)) { s.x[0] = status(result); return; }
        std::lock_guard lock(c->mutex);
        if (!c->encoder) { s.x[0] = 0; return; }
        std::vector<enc::Packet> packets;
        const bool ok = c->encoder->drain(packets);
        deliver(*c, packets);
        drop_all(*c);
        c->shift.reset();   // the encoder starts a new stream
        s.x[0] = status(ok ? 0 : vt_encoder_malfunction);
    });
    raw(video_toolbox, "_VTCompressionSessionPrepareToEncodeFrames", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Compressing> c;
        if (const auto result = check(s.x[0], c)) { s.x[0] = status(result); return; }
        std::lock_guard lock(c->mutex);
        s.x[0] = status(start(s.x[0], *c));
    });
    raw(video_toolbox, "_VTCompressionSessionInvalidate", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Compressing> c;
        if (check(s.x[0], c)) return;
        objc.host_state<VTSessionState>(s.x[0])->invalid = true;
        std::lock_guard lock(c->mutex);
        finish(*c);
    });
    // The pool is the session's (not +1).
    raw(video_toolbox, "_VTCompressionSessionGetPixelBufferPool", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Compressing> c;
        if (check(s.x[0], c)) { s.x[0] = 0; return; }
        std::lock_guard lock(c->mutex);
        s.x[0] = pool(*c);
    });
    // Multi-pass: no storage can be made, so no session has one.
    raw(video_toolbox, "_VTMultiPassStorageCreate", [](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write64(s.x[4], 0);
        s.x[0] = status(vt_could_not_create_instance);
    });
    raw(video_toolbox, "_VTMultiPassStorageClose", [](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy: VTMultiPassStorageClose received a storage that cannot exist under Alchemy");
        s.x[0] = status(vt_parameter);
    });
    for (const auto* name : {"_VTCompressionSessionBeginPass", "_VTCompressionSessionEndPass", "_VTCompressionSessionGetTimeRangesForNextPass"})
        raw(video_toolbox, name, [this](CpuState& s, GuestMemory&) {
            std::shared_ptr<Compressing> c;
            const auto result = check(s.x[0], c);
            s.x[0] = status(result ? result : vt_parameter);
        });
    // VTCopyVideoEncoderList(options, CFArrayRef *out): Alchemy's encoders.
    raw(video_toolbox, "_VTCopyVideoEncoderList", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status(vt_parameter); return; }
        std::vector<Address> items;
        for (const auto& e : enc::encoders()) {
            const auto codec = e.codec == enc::Codec::h264 ? fourcc("avc1") : fourcc("hvc1");
            std::vector<std::pair<Address, Address>> entries{
                {cf.string("CodecType"), cf.integer(codec)},
                {cf.string("EncoderID"), cf.string(e.id)},
                {cf.string("CodecName"), cf.string(e.codec == enc::Codec::h264 ? "H.264" : "HEVC")},
                {cf.string("EncoderName"), cf.string(e.name)},
                {cf.string("DisplayName"), cf.string(e.name)},
                {cf.string("IsHardwareAccelerated"), retain(cf.boolean(e.hardware))}};
            items.push_back(cf.dictionary(entries));
            for (const auto& [k, v] : entries) { release(k); release(v); }
        }
        m.write64(s.x[1], cf.array(items));
        for (const auto item : items) release(item);
        s.x[0] = 0;
    });
    // VTCopySupportedPropertyDictionaryForEncoder(width, height, codecType, encoderSpecification, CFStringRef *encoderIDOut,
    //     CFDictionaryRef *supportedPropertiesOut): the encoder the specification selects and the keys a session honours.
    raw(video_toolbox, "_VTCopySupportedPropertyDictionaryForEncoder", [this](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write64(s.x[4], 0);
        if (s.x[5]) m.write64(s.x[5], 0);
        const auto codec_type = static_cast<std::uint32_t>(s.x[2]);
        if (codec_type != fourcc("avc1") && codec_type != fourcc("hvc1")) { s.x[0] = status(vt_could_not_find_encoder); return; }
        std::string id;
        bool hardware = false, software = false, gpu = false;
        const auto candidates = allowed(codec_type == fourcc("avc1") ? enc::Codec::h264 : enc::Codec::hevc, s.x[3], id, hardware, software, gpu);
        if (candidates.empty()) { s.x[0] = status(vt_could_not_find_encoder); return; }
        if (s.x[4]) m.write64(s.x[4], cf.string(candidates.front().id));
        if (s.x[5]) {
            std::vector<std::pair<Address, Address>> entries;
            for (const auto* key : vt_session_keys(VTSessionState::Kind::compression)) entries.emplace_back(cf.string(key), cf.dictionary({}));
            m.write64(s.x[5], cf.dictionary(entries));
            for (const auto& [k, v] : entries) { release(k); release(v); }
        }
        s.x[0] = 0;
    });
}

} // namespace

void install_vt_compression(InstallContext& context) { std::make_shared<Compression>(context)->install(); }

} // namespace alchemy::gui::fill
