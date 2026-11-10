#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/cmtime.hpp"
#include "alchemy/darwin/media/packet_file.hpp"
#include "alchemy/darwin/media/aac_encoder.hpp"
#include "alchemy/darwin/media/audio_file.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/movie_file.hpp"
#include "alchemy/darwin/media/resampler.hpp"
#include "alchemy/darwin/media/video_encoder.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// AVAssetWriter and AVAssetWriterInput.
// - Audio files: WAVE, AIFF, AIFC and CAF of linear PCM, and CAF and Apple M4A of Apple Lossless or AAC, written
//   through ExtAudioFile (audio_file.cpp). The file's format is the audio input's outputSettings (AVFormatIDKey linear
//   PCM, AVSampleRateKey, AVNumberOfChannelsKey, AVLinearPCMBitDepthKey, ...IsFloat, ...IsBigEndian; or AVFormatIDKey
//   'alac' with AVEncoderBitDepthHintKey or AVLinearPCMBitDepthKey bits, 16 by default; or 'aac ' at
//   AVEncoderBitRateKey, where the host's AAC encoder takes the rate and channels), or with nil settings the format of
//   the first appended buffer (linear PCM files only). One audio input; the file is created by -startWriting (with
//   settings) or at the first append.
// - Movies: MPEG-4, Apple M4V and QuickTime movie files of any number of video and audio inputs, through the muxer
//   (movie_file.hpp), written with the guest's own file calls.
//   - Video inputs with outputSettings (AVVideoCodecKey H.264 or HEVC, AVVideoWidthKey, AVVideoHeightKey, and
//     AVVideoCompressionPropertiesKey: average bit rate, key frame interval and its duration, profile level, expected
//     source frame rate, frame reordering) encode through a VTCompressionSession of their own (vt_compression.cpp,
//     the host's encoders), whose output handler gives the muxer its samples: the pixel buffers of
//     AVAssetWriterInputPixelBufferAdaptor (its pixelBufferPool is the session's) or of appended sample buffers.
//     -expectsMediaDataInRealTime asks the session for real time.
//   - Audio inputs with AAC or Apple Lossless settings encode linear PCM sample buffers (any layout, converted to the
//     settings' channels, and resampled to their rate) through the host's AAC encoder or Apple's ALAC encoder; AAC
//     tracks present the frames after the encoder's priming through their edit list.
//   - Inputs with nil settings take compressed sample buffers as they are: H.264 and HEVC (avcC or hvcC from the
//     format description's sample description atoms), AAC and Apple Lossless (the magic cookie), one sample or packet
//     a buffer, or several where the buffer lists their sizes.
//   - The input's transform is the track's matrix; shouldOptimizeForNetworkUse puts the moov ahead of the data.
// - -appendSampleBuffer: drops buffers that end before the session's start time or begin at or after its end time.
//   Inputs are always ready for more data until -markAsFinished. -requestMediaDataWhenReadyOnQueue:usingBlock: calls the
//   block on the queue again while it keeps appending.
// - -finishWritingWithCompletionHandler: completes the file, sets the status and calls the handler on a thread of the
//   writer; -cancelWriting deletes the file.
// - Unavailable, as documented answers: settings for a codec with no encoder on this machine (-canApplyOutputSettings:
//   NO, -canAddInput: NO, -addInput: stops with NSInvalidArgumentException); linear PCM, and uncompressed video without
//   settings, in movie files (the same); and the MPEG layer 3 file type (the initializer fails with an error).
namespace alchemy::gui::completion_kit::media {
namespace {

namespace af = audio_file;
namespace mf = movie_file;

// The file types that hold linear PCM.
bool pcm_container(std::uint32_t container) {
    return container == af::four("WAVE") || container == af::four("AIFF") || container == af::four("AIFC") || container == af::four("caff");
}
// The movie file types (written here, through movie_file).
bool movie_container(std::uint32_t container) {
    return container == af::four("mp4f") || container == af::four("M4V ") || container == af::four("qt  ");
}
mf::Brand brand_of(std::uint32_t container) {
    return container == af::four("qt  ") ? mf::Brand::quicktime : container == af::four("M4V ") ? mf::Brand::m4v : mf::Brand::mp4;
}

constexpr std::string_view av_foundation = "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation";
constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view core_media = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view video_toolbox = "/System/Library/Frameworks/VideoToolbox.framework/Versions/A/VideoToolbox";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// AVAssetWriterStatus
constexpr std::int64_t status_unknown = 0, status_writing = 1, status_completed = 2, status_failed = 3, status_cancelled = 4;
constexpr std::int64_t av_error_unknown = -11800, av_error_unsupported_output_settings = -11861, av_error_encoder_not_found = -11834;
constexpr std::uint64_t o_rdwr = 2, o_creat = 0x200, o_trunc = 0x400;

// The AVVideoCompressionPropertiesKey entries and the VTCompressionSession properties they set. The AVFoundation keys'
// values are the VideoToolbox keys' (unverified: the documentation gives the keys' names, not their values).
constexpr std::array<std::pair<const char*, const char*>, 6> compression_keys{{
    {"AverageBitRate", "AverageBitRate"},
    {"MaxKeyFrameInterval", "MaxKeyFrameInterval"},
    {"MaxKeyFrameIntervalDuration", "MaxKeyFrameIntervalDuration"},
    {"ProfileLevel", "ProfileLevel"},
    {"ExpectedFrameRate", "ExpectedFrameRate"},
    {"AllowFrameReordering", "AllowFrameReordering"},
}};

std::int64_t to_scale(const CMTime& t, std::int32_t scale) {
    if (!t.timescale) return 0;
    return static_cast<std::int64_t>(std::llround(static_cast<long double>(t.value) * scale / t.timescale));
}

// A movie input's track and its encoder.
struct MovieInput {
    std::size_t track{SIZE_MAX};
    bool video{}, ready{};                             // ready: the track's format is known
    std::int32_t timescale{};                          // the track's
    // Video encoded here: the session (+1) and its output handler.
    Address session{}, handler{};
    // Audio encoded here.
    std::uint32_t codec{}, rate{}, channels{}, bits{16}, bit_rate{};
    std::unique_ptr<aac::Encoder> aac;
    std::unique_ptr<codecs::AudioEncoder> alac;
    std::unique_ptr<Resampler> resampler;
    double source_rate{};
    std::deque<double> fifo;                           // source frames waiting for the resampler
    std::vector<std::int32_t> held;                    // ALAC: frames waiting for a whole packet
    std::uint64_t frames_in{}, frames_out{}, source_frames{}, packets{};
    std::uint32_t largest{};
    std::int64_t last_dts{};
};

struct WriterState final : ObjectPayload {
    std::mutex mutex;
    Address url{}, file_type{}, error{}, metadata{};   // retained
    std::uint32_t container{};                         // AudioFileTypeID, or 'mp4f', 'M4V ', 'qt  ' for movies
    std::int64_t status{};
    std::vector<Address> inputs;                       // retained
    Address ext{};                                     // ExtAudioFileRef once created
    std::vector<std::uint8_t> client;                  // the ASBD currently set as the client format
    std::optional<CMTime> start, end;
    bool optimize{};
    CMTime fragment_interval = time_invalid;
    std::shared_ptr<CompletionThread> thread;
    // Movies: the muxer and the file. `movie_mutex` guards them and the inputs' MovieInput; the encoders' output
    // handlers take it (never `mutex`).
    std::mutex movie_mutex;
    std::optional<mf::Writer> mux;
    std::int64_t fd{-1};
    std::optional<CMTime> movie_start;
    std::string movie_failure;
};
struct WriterInput final : ObjectPayload {
    Address media_type{}, settings{}, hint{};          // retained
    Address writer{};                                  // not retained (the writer holds its inputs)
    bool finished{}, realtime{};
    std::array<double, 6> transform{1, 0, 0, 1, 0, 0};
    std::uint64_t appended{};
    MovieInput movie;
};
struct AdaptorState final : ObjectPayload {
    Address input{}, attributes{};                     // retained
};

// The video outputSettings.
struct VideoOutput {
    std::uint32_t codec{};
    std::uint32_t width{}, height{};
    Address compression{};                             // AVVideoCompressionPropertiesKey's dictionary, or 0
};

class Writers : public std::enable_shared_from_this<Writers> {
public:
    explicit Writers(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    // The file's AudioStreamBasicDescription for `settings` in `container`, or nullopt (a format the container does not
    // hold or nothing encodes, or incomplete settings).
    std::optional<std::vector<std::uint8_t>> settings_asbd(Address settings, std::uint32_t container);
    std::optional<VideoOutput> video_settings(Address settings);
    std::optional<double> number(Address dictionary, const char* key);
    bool input_fits(const WriterState& w, const WriterInput& in);
    std::int32_t create_file(WriterState& w, const std::vector<std::uint8_t>& asbd);
    bool append(Address input_object, Address sample);
    void fail(WriterState& w, std::int64_t code, const std::string& text) {
        w.status = status_failed;
        if (!w.error) w.error = bridge_->error("AVFoundationErrorDomain", code, text);
    }

    // Movies
    bool start_movie(WriterState& w);
    bool start_movie_input(WriterState& w, Address input_object, WriterInput& in);
    bool append_movie(WriterState& w, Address input_object, WriterInput& in, Address sample);
    bool encode_image(WriterState& w, WriterInput& in, Address image, const CMTime& pts, const CMTime& duration);
    bool encode_pcm(WriterState& w, WriterInput& in, Address sample, const std::vector<std::uint8_t>& asbd);
    void audio_packets(WriterState& w, WriterInput& in, std::vector<std::vector<std::uint8_t>>& packets, std::uint32_t frames_each);
    void pump_audio(WriterState& w, WriterInput& in, bool final);
    void mux_compressed(WriterState& w, WriterInput& in, Address sample);   // movie_mutex held
    void add_sample(WriterState& w, WriterInput& in, std::vector<std::uint8_t> bytes, std::int64_t dts, std::int64_t pts, std::uint32_t duration, bool sync);
    bool write_file(WriterState& w, const mf::Writes& writes);
    bool move_data(WriterState& w, std::uint64_t from, std::uint64_t by);
    void finish_movie(WriterState& w);
    void close_movie(WriterState& w, bool remove);
    CMTime sample_time(Address sample, const char* function);

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    Address writer_class_{}, input_class_{}, adaptor_class_{};
};

std::optional<double> Writers::number(Address dictionary, const char* key) {
    if (!dictionary || !bridge_->is_dictionary(dictionary)) return std::nullopt;
    const auto k = support_.cf.string(key);
    const auto value = bridge_->lookup(dictionary, k);
    support_.release(k);
    if (!value) return std::nullopt;
    if (value == support_.cf.boolean(true)) return 1;
    if (value == support_.cf.boolean(false)) return 0;
    return bridge_->real(value);
}

std::optional<std::vector<std::uint8_t>> Writers::settings_asbd(Address settings, std::uint32_t container) {
    if (!settings || !bridge_->is_dictionary(settings)) return std::nullopt;
    const auto setting = [&](const char* key) { return number(settings, key); };
    const auto format_id = static_cast<std::uint32_t>(setting("AVFormatIDKey").value_or(af::four("lpcm")));
    const auto rate = setting("AVSampleRateKey"), channels = setting("AVNumberOfChannelsKey");
    if (!rate || !(*rate > 0) || !channels || *channels < 1 || *channels > 64) return std::nullopt;
    if (format_id == af::four("alac") || format_id == af::four("aac ")) {
        // ALAC: kAppleLosslessFormatFlag_16/20/24/32BitSourceData, 4096 frames per packet. AAC: AAC-LC, 1024 frames
        // per packet, at a rate and channel count the host's encoder takes.
        const bool is_aac = format_id == af::four("aac ");
        const auto file = movie_container(container) ? af::four("mp4f") : container;
        if (!packet_file::writable(file, format_id) || *channels > 8) return std::nullopt;
        if (is_aac) {
            const auto caps = aac::capabilities();
            const auto takes = [](const std::vector<std::uint32_t>& list, double v) { return std::find(list.begin(), list.end(), static_cast<std::uint32_t>(v)) != list.end(); };
            if (!takes(caps.sample_rates, *rate) || !takes(caps.channel_counts, *channels)) return std::nullopt;
        }
        const auto depth = setting("AVEncoderBitDepthHintKey").value_or(setting("AVLinearPCMBitDepthKey").value_or(16));
        const std::uint32_t flags = is_aac ? 2 : depth <= 16 ? 1 : depth <= 20 ? 2 : depth <= 24 ? 3 : 4, per_packet = is_aac ? 1024 : 4096,
                            count = static_cast<std::uint32_t>(*channels);
        const double sample_rate = *rate;
        std::vector<std::uint8_t> asbd(40, 0);
        std::memcpy(asbd.data(), &sample_rate, 8);
        std::memcpy(asbd.data() + 8, &format_id, 4);
        std::memcpy(asbd.data() + 12, &flags, 4);
        std::memcpy(asbd.data() + 20, &per_packet, 4);
        std::memcpy(asbd.data() + 28, &count, 4);
        return asbd;
    }
    if (format_id != af::four("lpcm") || !pcm_container(container)) return std::nullopt;
    af::Pcm pcm;
    pcm.rate = *rate;
    pcm.channels = static_cast<std::uint32_t>(*channels);
    pcm.is_float = setting("AVLinearPCMIsFloatKey").value_or(0) != 0;
    pcm.bits = static_cast<std::uint32_t>(setting("AVLinearPCMBitDepthKey").value_or(pcm.is_float ? 32 : 16));
    pcm.container = (pcm.bits + 7) / 8;
    pcm.big_endian = setting("AVLinearPCMIsBigEndianKey").value_or(container == af::four("AIFF") || container == af::four("AIFC")) != 0;
    pcm.is_signed = pcm.is_float || !(container == af::four("WAVE") && pcm.bits == 8);
    pcm.aligned_high = !pcm.is_float && pcm.bits < pcm.container * 8 && container != af::four("caff");
    return af::asbd_from_pcm(pcm);
}

// H.264 or HEVC at a size, with an encoder of the codec on this machine.
std::optional<VideoOutput> Writers::video_settings(Address settings) {
    if (!settings || !bridge_->is_dictionary(settings)) return std::nullopt;
    const auto key = support_.cf.string("AVVideoCodecKey");
    const auto codec = bridge_->lookup(settings, key);
    support_.release(key);
    const auto name = codec ? bridge_->maybe_text(codec) : std::nullopt;
    if (!name || (*name != "avc1" && *name != "hvc1")) return std::nullopt;
    const auto width = number(settings, "AVVideoWidthKey"), height = number(settings, "AVVideoHeightKey");
    if (!width || !height || !(*width >= 1) || !(*height >= 1) || *width > 16384 || *height > 16384) return std::nullopt;
    VideoOutput out;
    out.codec = af::four(*name == "avc1" ? "avc1" : "hvc1");
    out.width = static_cast<std::uint32_t>(*width);
    out.height = static_cast<std::uint32_t>(*height);
    const auto properties = support_.cf.string("AVVideoCompressionPropertiesKey");
    out.compression = bridge_->lookup(settings, properties);
    support_.release(properties);
    if (out.compression && !bridge_->is_dictionary(out.compression)) return std::nullopt;
    const auto wanted = out.codec == af::four("avc1") ? video_encoding::Codec::h264 : video_encoding::Codec::hevc;
    const auto encoders = video_encoding::encoders();
    if (std::none_of(encoders.begin(), encoders.end(), [&](const video_encoding::EncoderInfo& e) { return e.codec == wanted; })) return std::nullopt;
    return out;
}

// Whether the writer's file type takes the input as it is set up.
bool Writers::input_fits(const WriterState& w, const WriterInput& in) {
    const auto type = support_.cf.utf8(in.media_type);
    if (movie_container(w.container)) {
        if (type == "vide") return !in.settings || video_settings(in.settings).has_value();
        // Audio: AAC or Apple Lossless settings, or compressed buffers as they are.
        return type == "soun" && (!in.settings || settings_asbd(in.settings, w.container).has_value());
    }
    if (type != "soun" || !w.inputs.empty()) return false;
    // Passthrough (nil settings) writes linear PCM, which M4A files do not hold.
    return in.settings ? settings_asbd(in.settings, w.container).has_value() : pcm_container(w.container);
}

std::int32_t Writers::create_file(WriterState& w, const std::vector<std::uint8_t>& asbd) {
    auto& m = support_.memory();
    const auto scratch = support_.process.allocate_guest(48, 8);
    m.write_bytes(scratch, asbd);
    m.write64(scratch + 40, 0);
    // ExtAudioFileCreateWithURL(url, type, &format, NULL, kAudioFileFlags_EraseFile, &out)
    const auto status = static_cast<std::int32_t>(support_.call(audio_toolbox, "_ExtAudioFileCreateWithURL", {w.url, w.container, scratch, 0, 1, scratch + 40}));
    if (!status) w.ext = m.read64(scratch + 40);
    support_.process.release_guest(scratch);
    // AAC: the input's AVEncoderBitRateKey.
    std::uint32_t format_id{};
    std::memcpy(&format_id, asbd.data() + 8, 4);
    if (!status && format_id == af::four("aac ") && !w.inputs.empty())
        if (const auto in = support_.objc.find_host_state<WriterInput>(w.inputs.front()); in && in->settings && bridge_->is_dictionary(in->settings))
            if (const auto bits = number(in->settings, "AVEncoderBitRateKey")) set_encode_bit_rate(w.ext, static_cast<std::uint32_t>(*bits));
    return status;
}

CMTime Writers::sample_time(Address sample, const char* function) {
    auto& m = support_.memory();
    const auto at = support_.process.allocate_guest(24, 8);
    CallRegs r{};
    r.x[0] = sample;
    r.indirect_result = at;
    static_cast<void>(EngineAdapter(support_.process.calling_engine()).call_regs(bridge_->symbol(core_media, function), r));
    const CMTime t{static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12), static_cast<std::int64_t>(m.read64(at + 16))};
    support_.process.release_guest(at);
    return t;
}

bool Writers::append(Address input_object, Address sample) {
    const auto in = support_.objc.find_host_state<WriterInput>(input_object);
    const auto w = in && in->writer ? support_.objc.find_host_state<WriterState>(in->writer) : nullptr;
    if (!w || !sample) return false;
    auto& m = support_.memory();
    std::lock_guard lock(w->mutex);
    if (w->status != status_writing || in->finished || !w->start) return false;
    // The session's time range.
    const auto pts = sample_time(sample, "_CMSampleBufferGetPresentationTimeStamp"), duration = sample_time(sample, "_CMSampleBufferGetDuration");
    if (is_numeric(pts)) {
        const auto end_of_buffer = is_numeric(duration) ? add(pts, duration) : pts;
        if (is_numeric(*w->start) && compare(end_of_buffer, *w->start) <= 0 && compare(pts, *w->start) < 0) return true;   // before the session: dropped
        if (w->end && is_numeric(*w->end) && compare(pts, *w->end) >= 0) return true;                                        // after it
    }
    if (movie_container(w->container)) {
        const bool ok = append_movie(*w, input_object, *in, sample);
        if (ok) ++in->appended;
        return ok;
    }
    const auto description = support_.call(core_media, "_CMSampleBufferGetFormatDescription", {sample});
    const auto asbd_at = description ? support_.call(core_media, "_CMAudioFormatDescriptionGetStreamBasicDescription", {description}) : 0;
    if (!asbd_at) {
        fail(*w, av_error_unknown, "AVAssetWriterInput appends audio sample buffers only");
        return false;
    }
    std::vector<std::uint8_t> asbd(40);
    m.read_bytes(asbd_at, asbd);
    std::string why;
    const auto pcm = af::pcm_from_asbd(asbd, why);
    if (!pcm) {
        fail(*w, av_error_unsupported_output_settings, "AVAssetWriter writes linear PCM sample buffers (no audio decoder in the writer)");
        return false;
    }
    if (!w->ext) {
        const auto file = settings_asbd(in->settings, w->container);
        auto file_asbd = file ? *file : asbd;
        if (!file && (w->container == af::four("AIFF")) && !pcm->is_float) {   // AIFF integers are big-endian
            auto big = *pcm;
            big.big_endian = true;
            file_asbd = af::asbd_from_pcm(big);
        }
        if (const auto status = create_file(*w, file_asbd)) {
            fail(*w, status, "AVAssetWriter could not create the file in that format");
            return false;
        }
    }
    const auto frames = static_cast<std::uint32_t>(support_.call(core_media, "_CMSampleBufferGetNumSamples", {sample}));
    const auto block = support_.call(core_media, "_CMSampleBufferGetDataBuffer", {sample});
    const auto length = block ? support_.call(core_media, "_CMBlockBufferGetDataLength", {block}) : 0;
    if (!frames || !length) return true;
    const auto planes = pcm->non_interleaved ? pcm->channels : 1U;
    const auto scratch = support_.process.allocate_guest(40 + 8 + 16ULL * planes + length, 16);
    const std::uint64_t list = scratch + 40, data = list + 8 + 16ULL * planes;
    std::int32_t status = 0;
    if (w->client != asbd) {
        m.write_bytes(scratch, asbd);
        status = static_cast<std::int32_t>(support_.call(audio_toolbox, "_ExtAudioFileSetProperty", {w->ext, af::four("cfmt"), 40, scratch}));
        if (!status) w->client = asbd;
    }
    if (!status) status = static_cast<std::int32_t>(support_.call(core_media, "_CMBlockBufferCopyDataBytes", {block, 0, length, data}));
    if (!status) {
        const auto plane_bytes = length / planes;
        m.write32(list, planes);
        for (std::uint32_t p = 0; p < planes; ++p) {
            const auto entry = list + 8 + 16ULL * p;
            m.write32(entry, pcm->non_interleaved ? 1 : pcm->channels);
            m.write32(entry + 4, static_cast<std::uint32_t>(plane_bytes));
            m.write64(entry + 8, data + p * plane_bytes);
        }
        status = static_cast<std::int32_t>(support_.call(audio_toolbox, "_ExtAudioFileWrite", {w->ext, frames, list}));
    }
    support_.process.release_guest(scratch);
    if (status) {
        fail(*w, status, "AVAssetWriter could not write the sample buffer (another sample rate, or a format the file cannot take)");
        return false;
    }
    ++in->appended;
    return true;
}

// ---- movies ------------------------------------------------------------------------------------------------

bool Writers::write_file(WriterState& w, const mf::Writes& writes) {
    auto& m = support_.memory();
    for (const auto& [offset, bytes] : writes) {
        if (bytes.empty()) continue;
        const auto chunk = std::min<std::size_t>(bytes.size(), 1 << 20);
        const auto buffer = support_.process.allocate_guest(chunk, 16);
        bool ok = true;
        for (std::size_t done = 0; ok && done < bytes.size();) {
            const auto n = std::min(chunk, bytes.size() - done);
            m.write_bytes(buffer, std::span(bytes).subspan(done, n));
            const auto wrote = static_cast<std::int64_t>(support_.call(libsystem, "_pwrite", {static_cast<Address>(w.fd), buffer, n, offset + done}));
            if (wrote <= 0) ok = false;
            else done += static_cast<std::size_t>(wrote);
        }
        support_.process.release_guest(buffer);
        if (!ok) {
            if (w.movie_failure.empty()) w.movie_failure = "AVAssetWriter could not write the file";
            return false;
        }
    }
    return true;
}

// The bytes from `from` to the end move up by `by` (the last chunk first).
bool Writers::move_data(WriterState& w, std::uint64_t from, std::uint64_t by) {
    auto& m = support_.memory();
    const auto end = static_cast<std::int64_t>(support_.call(libsystem, "_lseek", {static_cast<Address>(w.fd), 0, 2}));
    if (end < 0) return false;
    constexpr std::uint64_t chunk = 1 << 20;
    const auto buffer = support_.process.allocate_guest(chunk, 16);
    bool ok = true;
    for (std::uint64_t stop = static_cast<std::uint64_t>(end); ok && stop > from;) {
        const auto start = stop - from > chunk ? stop - chunk : from;
        const auto n = stop - start;
        ok = static_cast<std::int64_t>(support_.call(libsystem, "_pread", {static_cast<Address>(w.fd), buffer, n, start})) == static_cast<std::int64_t>(n) &&
             static_cast<std::int64_t>(support_.call(libsystem, "_pwrite", {static_cast<Address>(w.fd), buffer, n, start + by})) == static_cast<std::int64_t>(n);
        stop = start;
    }
    static_cast<void>(m);
    support_.process.release_guest(buffer);
    return ok;
}

bool Writers::start_movie(WriterState& w) {
    const auto path = support_.cf.url_path(w.url);
    const auto text = support_.process.allocate_guest(path.size() + 1, 1);
    std::vector<std::uint8_t> name(path.begin(), path.end());
    name.push_back(0);
    support_.memory().write_bytes(text, name);
    CallRegs r{};
    r.x[0] = text;
    r.x[1] = o_rdwr | o_creat | o_trunc;
    r.push_variadic(std::uint64_t{0644});   // open's mode is variadic
    const auto fd = static_cast<std::int64_t>(support_.call(libsystem, "_open", r).x0);
    support_.process.release_guest(text);
    if (fd < 0) {
        fail(w, av_error_unknown, "AVAssetWriter could not create the file");
        return false;
    }
    std::lock_guard lock(w.movie_mutex);
    w.fd = fd;
    w.mux.emplace(brand_of(w.container));
    return write_file(w, {{0, w.mux->header()}});
}

bool Writers::start_movie_input(WriterState& w, Address input_object, WriterInput& in) {
    auto& mi = in.movie;
    mi.video = support_.cf.utf8(in.media_type) == "vide";
    mf::TrackFormat format;
    format.video = mi.video;
    format.transform = in.transform;
    if (mi.video) {
        mi.timescale = 600;
        if (const auto out = in.settings ? video_settings(in.settings) : std::nullopt) {
            format.codec = out->codec;
            format.width = out->width;
            format.height = out->height;
            // VTCompressionSessionCreate(NULL, width, height, codec, NULL, NULL, NULL, NULL, refcon, &session)
            const auto session_out = support_.process.allocate_guest(8, 8);
            support_.memory().write64(session_out, 0);
            CallRegs r{};
            r.x[1] = out->width;
            r.x[2] = out->height;
            r.x[3] = out->codec;
            r.push_stack(0);
            r.push_stack(session_out);
            const auto created = static_cast<std::int32_t>(support_.call(video_toolbox, "_VTCompressionSessionCreate", r).x0);
            mi.session = created ? 0 : support_.memory().read64(session_out);
            support_.process.release_guest(session_out);
            if (!mi.session) {
                fail(w, av_error_encoder_not_found, "AVAssetWriter found no video encoder for these settings (VTCompressionSessionCreate " + std::to_string(created) + ")");
                return false;
            }
            for (const auto& [av_key, vt_key] : compression_keys) {
                const auto k = support_.cf.string(av_key);
                const auto value = out->compression ? bridge_->lookup(out->compression, k) : 0;
                support_.release(k);
                if (!value) continue;
                const auto property = support_.cf.string(vt_key);
                static_cast<void>(support_.call(video_toolbox, "_VTSessionSetProperty", {mi.session, property, value}));
                support_.release(property);
            }
            if (in.realtime) {
                const auto property = support_.cf.string("RealTime");
                static_cast<void>(support_.call(video_toolbox, "_VTSessionSetProperty", {mi.session, property, support_.cf.boolean(true)}));
                support_.release(property);
            }
            // The output handler: block(OSStatus, VTEncodeInfoFlags, CMSampleBufferRef) gives the muxer each frame.
            const auto writer_object = in.writer;
            auto* sp = &support_;
            const std::weak_ptr<Writers> weak = shared_from_this();
            mi.handler = bridge_->make_block([weak, sp, writer_object, input_object](CpuState& s, GuestMemory&) {
                const auto self = weak.lock();
                const auto writer = self ? sp->objc.find_host_state<WriterState>(writer_object) : nullptr;
                const auto input = self ? sp->objc.find_host_state<WriterInput>(input_object) : nullptr;
                if (!writer || !input) return;
                std::lock_guard lock(writer->movie_mutex);
                if (static_cast<std::int32_t>(s.x[1]) != 0) {
                    if (writer->movie_failure.empty()) writer->movie_failure = "the video encoder failed (" + std::to_string(static_cast<std::int32_t>(s.x[1])) + ")";
                    return;
                }
                if (s.x[3]) self->mux_compressed(*writer, *input, s.x[3]);
            });
        }
    } else if (in.settings) {
        const auto asbd = settings_asbd(in.settings, w.container);
        if (!asbd) {
            fail(w, av_error_unsupported_output_settings, "AVAssetWriter has no encoder for these audio settings");
            return false;
        }
        std::memcpy(&mi.codec, asbd->data() + 8, 4);
        double rate{};
        std::memcpy(&rate, asbd->data(), 8);
        std::uint32_t flags{};
        std::memcpy(&flags, asbd->data() + 12, 4);
        std::memcpy(&mi.channels, asbd->data() + 28, 4);
        mi.rate = static_cast<std::uint32_t>(rate);
        mi.timescale = static_cast<std::int32_t>(mi.rate);
        format.codec = mi.codec == af::four("aac ") ? af::four("mp4a") : af::four("alac");
        format.sample_rate = rate;
        format.channels = mi.channels;
        format.timescale = mi.rate;
        std::string why;
        if (mi.codec == af::four("aac ")) {
            mi.bit_rate = static_cast<std::uint32_t>(number(in.settings, "AVEncoderBitRateKey").value_or(0));
            mi.aac = aac::open(mi.rate, mi.channels, mi.bit_rate, why);
        } else {
            mi.bits = flags == 1 ? 16 : flags == 2 ? 20 : flags == 3 ? 24 : 32;
            format.bits = mi.bits;
            mi.alac = codecs::make_audio_encoder(af::four("alac"), rate, mi.channels, mi.bits, 4096, why);
        }
        if (!mi.aac && !mi.alac) {
            fail(w, av_error_encoder_not_found, "AVAssetWriter could not open the audio encoder: " + why);
            return false;
        }
    }
    std::lock_guard lock(w.movie_mutex);
    mi.track = w.mux->add_track(format);
    mi.ready = mi.session || mi.aac || mi.alac;
    return true;
}

// A compressed sample buffer into its track (the encoders' output and inputs without settings). movie_mutex held.
void Writers::mux_compressed(WriterState& w, WriterInput& in, Address sample) {
    auto& m = support_.memory();
    auto& mi = in.movie;
    if (!w.mux || mi.track == SIZE_MAX || !w.movie_failure.empty()) return;
    const auto description = support_.call(core_media, "_CMSampleBufferGetFormatDescription", {sample});
    if (!description) { w.movie_failure = "AVAssetWriter needs sample buffers with a format description"; return; }
    const auto subtype = static_cast<std::uint32_t>(support_.call(core_media, "_CMFormatDescriptionGetMediaSubType", {description}));
    const auto pts = sample_time(sample, "_CMSampleBufferGetPresentationTimeStamp");
    auto dts = sample_time(sample, "_CMSampleBufferGetDecodeTimeStamp");
    const auto duration = sample_time(sample, "_CMSampleBufferGetDuration");
    if (!is_numeric(pts)) { w.movie_failure = "AVAssetWriter needs sample buffers with presentation times"; return; }
    if (!is_numeric(dts)) dts = pts;
    if (mi.video) {
        // The format: H.264 or HEVC, its size, and the avcC or hvcC of the sample description's extension atoms.
        if (subtype != af::four("avc1") && subtype != af::four("hvc1")) { w.movie_failure = "AVAssetWriter writes H.264 and HEVC video"; return; }
        const auto atoms_key = support_.cf.string("SampleDescriptionExtensionAtoms");
        const auto atoms = support_.call(core_media, "_CMFormatDescriptionGetExtension", {description, atoms_key});
        support_.release(atoms_key);
        const auto record_key = support_.cf.string(subtype == af::four("avc1") ? "avcC" : "hvcC");
        const auto record = atoms && bridge_->is_dictionary(atoms) ? bridge_->lookup(atoms, record_key) : 0;
        support_.release(record_key);
        if (!record || !bridge_->is_data(record)) { w.movie_failure = "AVAssetWriter needs the avcC or hvcC of the video's format description"; return; }
        if (!mi.ready || w.mux->sample_count(mi.track) == 0) {
            const auto dimensions = support_.call(core_media, "_CMVideoFormatDescriptionGetDimensions", {description});
            mf::TrackFormat format;
            format.video = true;
            format.codec = subtype;
            format.width = static_cast<std::uint32_t>(dimensions & 0xffffffff);
            format.height = static_cast<std::uint32_t>(dimensions >> 32);
            format.transform = in.transform;
            mi.timescale = pts.timescale >= 600 ? pts.timescale : 600;
            format.timescale = static_cast<std::uint32_t>(mi.timescale);
            w.mux->set_format(mi.track, format);
            mi.ready = true;
        }
        w.mux->set_config(mi.track, bridge_->bytes(record));
    } else {
        // AAC or Apple Lossless with its magic cookie, in the stream's own rate.
        if (subtype != af::four("aac ") && subtype != af::four("alac")) {
            w.movie_failure = "AVAssetWriter writes AAC and Apple Lossless audio into movies (linear PCM needs settings)";
            return;
        }
        const auto asbd_at = support_.call(core_media, "_CMAudioFormatDescriptionGetStreamBasicDescription", {description});
        if (!asbd_at) { w.movie_failure = "AVAssetWriter needs the audio's stream description"; return; }
        std::vector<std::uint8_t> asbd(40);
        m.read_bytes(asbd_at, asbd);
        double rate{};
        std::uint32_t per_packet{}, channels{}, flags{};
        std::memcpy(&rate, asbd.data(), 8);
        std::memcpy(&flags, asbd.data() + 12, 4);
        std::memcpy(&per_packet, asbd.data() + 20, 4);
        std::memcpy(&channels, asbd.data() + 28, 4);
        if (!mi.ready) {
            const auto size_at = support_.process.allocate_guest(8, 8);
            m.write64(size_at, 0);
            const auto cookie = support_.call(core_media, "_CMAudioFormatDescriptionGetMagicCookie", {description, size_at});
            std::vector<std::uint8_t> bytes(cookie ? m.read64(size_at) : 0);
            if (!bytes.empty()) m.read_bytes(cookie, bytes);
            support_.process.release_guest(size_at);
            mf::TrackFormat format;
            format.codec = subtype == af::four("aac ") ? af::four("mp4a") : af::four("alac");
            format.sample_rate = rate;
            format.channels = channels;
            format.bits = flags == 1 ? 16 : flags == 2 ? 20 : flags == 3 ? 24 : 32;
            format.timescale = static_cast<std::uint32_t>(rate);
            mi.timescale = static_cast<std::int32_t>(rate);
            mi.rate = static_cast<std::uint32_t>(rate);
            w.mux->set_format(mi.track, format);
            w.mux->set_config(mi.track, std::move(bytes));
            mi.ready = true;
        }
        // The packets: one, or each of the buffer's sizes.
        const auto count = static_cast<std::uint64_t>(support_.call(core_media, "_CMSampleBufferGetNumSamples", {sample}));
        const auto block = support_.call(core_media, "_CMSampleBufferGetDataBuffer", {sample});
        const auto length = block ? support_.call(core_media, "_CMBlockBufferGetDataLength", {block}) : 0;
        std::vector<std::uint64_t> sizes;
        if (count == 1) {
            sizes.push_back(length);
        } else {
            for (std::uint64_t i = 0; i < count; ++i) sizes.push_back(support_.call(core_media, "_CMSampleBufferGetSampleSize", {sample, i}));
            std::uint64_t total = 0;
            for (const auto s : sizes) total += s;
            if (total != length || std::find(sizes.begin(), sizes.end(), 0) != sizes.end()) {
                w.movie_failure = "AVAssetWriter needs the packet sizes of compressed audio buffers";
                return;
            }
        }
        std::vector<std::uint8_t> bytes(length);
        const auto staging = support_.process.allocate_guest(std::max<std::uint64_t>(length, 1), 16);
        const bool copied = length && support_.call(core_media, "_CMBlockBufferCopyDataBytes", {block, 0, length, staging}) == 0;
        if (copied) m.read_bytes(staging, bytes);
        support_.process.release_guest(staging);
        if (!copied) return;
        const auto frames = per_packet ? per_packet : (subtype == af::four("aac ") ? 1024u : 4096u);
        auto at = to_scale(subtract(pts, *w.movie_start), mi.timescale);
        std::size_t offset = 0;
        for (const auto size : sizes) {
            add_sample(w, in, std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)),
                       at, at, frames, true);
            offset += size;
            at += frames;
        }
        return;
    }
    // Video: one frame, sync unless its attachments say NotSync.
    const auto block = support_.call(core_media, "_CMSampleBufferGetDataBuffer", {sample});
    const auto length = block ? support_.call(core_media, "_CMBlockBufferGetDataLength", {block}) : 0;
    if (!length) return;
    std::vector<std::uint8_t> bytes(length);
    const auto staging = support_.process.allocate_guest(length, 16);
    const bool copied = support_.call(core_media, "_CMBlockBufferCopyDataBytes", {block, 0, length, staging}) == 0;
    if (copied) m.read_bytes(staging, bytes);
    support_.process.release_guest(staging);
    if (!copied) return;
    bool sync = true;
    if (const auto attachments = support_.call(core_media, "_CMSampleBufferGetSampleAttachmentsArray", {sample, 0}))
        if (support_.call(core_foundation, "_CFArrayGetCount", {attachments}) > 0) {
            const auto first = support_.call(core_foundation, "_CFArrayGetValueAtIndex", {attachments, 0});
            const auto key = support_.cf.string("NotSync");
            if (first && bridge_->lookup(first, key) == support_.cf.boolean(true)) sync = false;
            support_.release(key);
        }
    const auto start = *w.movie_start;
    const auto frame = is_numeric(duration) ? to_scale(duration, mi.timescale) : 0;
    add_sample(w, in, std::move(bytes), to_scale(subtract(dts, start), mi.timescale), to_scale(subtract(pts, start), mi.timescale),
               static_cast<std::uint32_t>(std::max<std::int64_t>(frame, 0)), sync);
}

void Writers::add_sample(WriterState& w, WriterInput& in, std::vector<std::uint8_t> bytes, std::int64_t dts, std::int64_t pts, std::uint32_t duration, bool sync) {
    auto& mi = in.movie;
    // A frame without a duration: until the next decode time (the muxer takes the steps between decode times).
    if (!duration) duration = static_cast<std::uint32_t>(std::max<std::int32_t>(1, mi.timescale / 30));
    mi.last_dts = dts;
    write_file(w, w.mux->add_sample(mi.track, std::move(bytes), dts, pts, duration, sync));
}

bool Writers::encode_image(WriterState& w, WriterInput& in, Address image, const CMTime& pts, const CMTime& duration) {
    auto& m = support_.memory();
    const auto times = support_.process.allocate_guest(48, 8);
    const auto put_time = [&](Address at, const CMTime& t) {
        m.write64(at, static_cast<std::uint64_t>(t.value));
        m.write32(at + 8, static_cast<std::uint32_t>(t.timescale));
        m.write32(at + 12, t.flags);
        m.write64(at + 16, static_cast<std::uint64_t>(t.epoch));
    };
    put_time(times, pts);
    put_time(times + 24, duration);
    // VTCompressionSessionEncodeFrameWithOutputHandler(session, image, pts, duration, NULL, NULL, handler)
    const auto status = static_cast<std::int32_t>(
        support_.call(video_toolbox, "_VTCompressionSessionEncodeFrameWithOutputHandler", {in.movie.session, image, times, times + 24, 0, 0, in.movie.handler}));
    support_.process.release_guest(times);
    if (status) {
        fail(w, status, "AVAssetWriter could not encode the frame (VTCompressionSessionEncodeFrame)");
        return false;
    }
    std::lock_guard lock(w.movie_mutex);
    if (!w.movie_failure.empty()) {
        fail(w, av_error_unknown, w.movie_failure);
        return false;
    }
    return true;
}

// Packets of `frames_each` frames each, in order, into the audio track.
void Writers::audio_packets(WriterState& w, WriterInput& in, std::vector<std::vector<std::uint8_t>>& packets, std::uint32_t frames_each) {
    auto& mi = in.movie;
    std::lock_guard lock(w.movie_mutex);
    for (auto& p : packets) {
        mi.largest = std::max<std::uint32_t>(mi.largest, static_cast<std::uint32_t>(p.size()));
        const auto at = static_cast<std::int64_t>(mi.frames_out);
        mi.frames_out += frames_each;
        ++mi.packets;
        add_sample(w, in, std::move(p), at, at, frames_each, true);
    }
    packets.clear();
}

// The FIFO's source frames through the resampler and the encoder (`final`: everything, the tail padded).
void Writers::pump_audio(WriterState& w, WriterInput& in, bool final) {
    auto& mi = in.movie;
    const auto channels = mi.channels;
    std::vector<double> out;
    // Output frames due: the source's so far at the output rate.
    const auto due = [&] { return static_cast<std::uint64_t>(std::llround(static_cast<long double>(mi.source_frames) * mi.rate / mi.source_rate)); };
    if (mi.resampler) {
        const auto available = mi.fifo.size() / channels;
        std::size_t frames = due() > mi.frames_in ? static_cast<std::size_t>(due() - mi.frames_in) : 0;
        while (frames && !final && mi.resampler->input_needed(frames) > available) --frames;
        if (frames) {
            out.resize(frames * channels);
            mi.resampler->render(out.data(), frames, [&](double* into, std::size_t n) {
                for (std::size_t i = 0; i < n * channels; ++i) {
                    if (mi.fifo.empty()) { into[i] = 0; continue; }
                    into[i] = mi.fifo.front();
                    mi.fifo.pop_front();
                }
            });
        }
    } else {
        out.assign(mi.fifo.begin(), mi.fifo.end());
        mi.fifo.clear();
    }
    const auto frames = out.size() / channels;
    mi.frames_in += frames;
    std::vector<std::vector<std::uint8_t>> packets;
    if (mi.aac) {
        std::vector<std::int16_t> samples(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) samples[i] = static_cast<std::int16_t>(std::lround(std::clamp(out[i], -1.0, 1.0) * 32767.0));
        if (!samples.empty() && !mi.aac->encode(samples, packets)) { fail(w, av_error_unknown, "the AAC encoder failed"); return; }
        if (final && !mi.aac->drain(packets)) { fail(w, av_error_unknown, "the AAC encoder failed"); return; }
        audio_packets(w, in, packets, 1024);
        return;
    }
    // ALAC: samples left-justified in 32 bits, 4096 frames a packet (the last may be shorter).
    for (const auto v : out) mi.held.push_back(static_cast<std::int32_t>(std::llround(std::clamp(v, -1.0, 1.0) * 2147483647.0)));
    std::string why;
    while (mi.held.size() >= 4096ULL * channels || (final && !mi.held.empty())) {
        const auto take = std::min<std::size_t>(mi.held.size(), 4096ULL * channels);
        std::vector<std::uint8_t> packet;
        if (!mi.alac->encode(std::span(mi.held).first(take), packet, why)) { fail(w, av_error_unknown, "the Apple Lossless encoder failed: " + why); return; }
        mi.held.erase(mi.held.begin(), mi.held.begin() + static_cast<std::ptrdiff_t>(take));
        std::vector<std::vector<std::uint8_t>> one;
        one.push_back(std::move(packet));
        audio_packets(w, in, one, static_cast<std::uint32_t>(take / channels));
    }
}

bool Writers::encode_pcm(WriterState& w, WriterInput& in, Address sample, const std::vector<std::uint8_t>& asbd) {
    auto& m = support_.memory();
    auto& mi = in.movie;
    std::string why;
    const auto pcm = af::pcm_from_asbd(asbd, why);
    if (!pcm) {
        fail(w, av_error_unsupported_output_settings, "AVAssetWriter encodes linear PCM sample buffers: " + why);
        return false;
    }
    const auto frames = static_cast<std::size_t>(support_.call(core_media, "_CMSampleBufferGetNumSamples", {sample}));
    const auto block = support_.call(core_media, "_CMSampleBufferGetDataBuffer", {sample});
    const auto length = block ? support_.call(core_media, "_CMBlockBufferGetDataLength", {block}) : 0;
    if (!frames || !length) return true;
    if (!mi.source_rate) {
        mi.source_rate = pcm->rate;
        if (pcm->rate != mi.rate) mi.resampler = std::make_unique<Resampler>(pcm->rate, mi.rate, mi.channels);
    } else if (pcm->rate != mi.source_rate) {
        fail(w, av_error_unsupported_output_settings, "AVAssetWriter takes one sample rate per audio input");
        return false;
    }
    std::vector<std::uint8_t> bytes(length);
    const auto staging = support_.process.allocate_guest(length, 16);
    const bool copied = support_.call(core_media, "_CMBlockBufferCopyDataBytes", {block, 0, length, staging}) == 0;
    if (copied) m.read_bytes(staging, bytes);
    support_.process.release_guest(staging);
    if (!copied) { fail(w, av_error_unknown, "AVAssetWriter could not read the sample buffer"); return false; }
    std::vector<std::span<const std::uint8_t>> buffers;
    const auto planes = pcm->non_interleaved ? pcm->channels : 1U;
    const auto plane_bytes = length / planes;
    for (std::uint32_t p = 0; p < planes; ++p) buffers.emplace_back(bytes.data() + p * plane_bytes, plane_bytes);
    const auto samples = af::decode(*pcm, buffers, frames);
    // Into the settings' channels: the same, mono to each, an average to mono, or the first channels.
    const auto from = pcm->channels, to = mi.channels;
    for (std::size_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < to; ++c) {
            double v = 0;
            if (from == to) v = samples[f * from + c];
            else if (from == 1) v = samples[f];
            else if (to == 1) { for (std::uint32_t k = 0; k < from; ++k) v += samples[f * from + k]; v /= from; }
            else v = c < from ? samples[f * from + c] : 0;
            mi.fifo.push_back(v);
        }
    mi.source_frames += frames;
    pump_audio(w, in, false);
    return w.status == status_writing;
}

bool Writers::append_movie(WriterState& w, Address input_object, WriterInput& in, Address sample) {
    static_cast<void>(input_object);
    auto& mi = in.movie;
    const auto image = support_.call(core_media, "_CMSampleBufferGetImageBuffer", {sample});
    if (mi.video && image) {
        if (!mi.session) {
            fail(w, av_error_unsupported_output_settings, "AVAssetWriter writes uncompressed video through outputSettings (no settings: compressed samples)");
            return false;
        }
        return encode_image(w, in, image, sample_time(sample, "_CMSampleBufferGetPresentationTimeStamp"), sample_time(sample, "_CMSampleBufferGetDuration"));
    }
    if (!mi.video && (mi.aac || mi.alac)) {
        const auto description = support_.call(core_media, "_CMSampleBufferGetFormatDescription", {sample});
        const auto asbd_at = description ? support_.call(core_media, "_CMAudioFormatDescriptionGetStreamBasicDescription", {description}) : 0;
        if (!asbd_at) {
            fail(w, av_error_unknown, "AVAssetWriterInput appends audio sample buffers only");
            return false;
        }
        std::vector<std::uint8_t> asbd(40);
        support_.memory().read_bytes(asbd_at, asbd);
        return encode_pcm(w, in, sample, asbd);
    }
    if (mi.session) {
        fail(w, av_error_unsupported_output_settings, "AVAssetWriterInput with video settings appends uncompressed frames");
        return false;
    }
    std::lock_guard lock(w.movie_mutex);
    mux_compressed(w, in, sample);
    if (!w.movie_failure.empty()) {
        fail(w, av_error_unsupported_output_settings, w.movie_failure);
        return false;
    }
    return true;
}

void Writers::finish_movie(WriterState& w) {
    // The encoders' last frames and packets.
    for (const auto input : w.inputs) {
        const auto in = support_.objc.find_host_state<WriterInput>(input);
        if (!in) continue;
        auto& mi = in->movie;
        if (mi.session) {
            const auto invalid = support_.process.allocate_guest(24, 8);
            for (int i = 0; i < 3; ++i) support_.memory().write64(invalid + 8 * i, 0);
            const auto status = static_cast<std::int32_t>(support_.call(video_toolbox, "_VTCompressionSessionCompleteFrames", {mi.session, invalid}));
            support_.process.release_guest(invalid);
            if (status) fail(w, status, "AVAssetWriter could not complete the video frames");
        }
        if ((mi.aac || mi.alac) && w.status == status_writing) {
            pump_audio(w, *in, true);
            std::lock_guard lock(w.movie_mutex);
            if (mi.aac && mi.track != SIZE_MAX) {
                const auto config = aac::es_descriptor(mi.aac->audio_specific_config(), mi.aac->bit_rate(),
                                                       static_cast<std::uint32_t>(std::uint64_t{mi.largest} * 8 * mi.rate / 1024), mi.largest);
                w.mux->set_config(mi.track, config);
                w.mux->set_trim(mi.track, mi.aac->priming(), mi.frames_in);
            } else if (mi.alac && mi.track != SIZE_MAX) {
                w.mux->set_config(mi.track, mi.alac->cookie());
            }
        }
    }
    std::lock_guard lock(w.movie_mutex);
    if (w.status != status_writing) return;
    if (!w.movie_failure.empty()) {
        fail(w, av_error_unknown, w.movie_failure);
        return;
    }
    const auto done = w.mux->finish(w.optimize);
    if ((done.move_by && !move_data(w, done.move_from, done.move_by)) || !write_file(w, done.writes)) {
        fail(w, av_error_unknown, "AVAssetWriter could not complete the file");
        return;
    }
}

// The sessions and the file closed (`remove`: the file deleted).
void Writers::close_movie(WriterState& w, bool remove) {
    for (const auto input : w.inputs) {
        const auto in = support_.objc.find_host_state<WriterInput>(input);
        if (!in) continue;
        auto& mi = in->movie;
        if (mi.session) {
            static_cast<void>(support_.call(video_toolbox, "_VTCompressionSessionInvalidate", {mi.session}));
            support_.release(std::exchange(mi.session, 0));
        }
        if (mi.handler) bridge_->forget_block(std::exchange(mi.handler, 0));
        mi.aac.reset();
        mi.alac.reset();
        mi.resampler.reset();
    }
    std::lock_guard lock(w.movie_mutex);
    if (w.fd >= 0) static_cast<void>(support_.call(libsystem, "_close", {static_cast<Address>(std::exchange(w.fd, -1))}));
    w.mux.reset();
    if (remove && w.url) {
        const auto manager = support_.send_id(support_.objc.find_class("NSFileManager"), "defaultManager");
        if (manager) static_cast<void>(support_.send_id(manager, "removeItemAtURL:error:", {w.url, 0}));
    }
}

void Writers::install() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    writer_class_ = support.declare_class("AVAssetWriter", "NSObject", av_foundation);
    input_class_ = support.declare_class("AVAssetWriterInput", "NSObject", av_foundation);
    adaptor_class_ = support.declare_class("AVAssetWriterInputPixelBufferAdaptor", "NSObject", av_foundation);
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_AVFileTypeWAVE", "com.microsoft.waveform-audio"}, {"_AVFileTypeAIFF", "public.aiff-audio"}, {"_AVFileTypeAIFC", "public.aifc-audio"},
             {"_AVFileTypeCoreAudioFormat", "com.apple.coreaudio-format"}, {"_AVFileTypeQuickTimeMovie", "com.apple.quicktime-movie"},
             {"_AVFileTypeMPEG4", "public.mpeg-4"}, {"_AVFileTypeAppleM4A", "com.apple.m4a-audio"}, {"_AVFileTypeAppleM4V", "com.apple.m4v-video"},
             {"_AVFileTypeMPEGLayer3", "public.mp3"}})
        support.string_constant(av_foundation, symbol, value);
    // The video settings' keys and values. Codec types are the codecs' four-char codes; the keys' values are their
    // names, and the compression properties' the VideoToolbox keys' (unverified: the documentation gives the names only).
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_AVVideoCodecTypeH264", "avc1"}, {"_AVVideoCodecTypeHEVC", "hvc1"}, {"_AVVideoCodecH264", "avc1"},
             {"_AVVideoWidthKey", "AVVideoWidthKey"}, {"_AVVideoHeightKey", "AVVideoHeightKey"},
             {"_AVVideoCompressionPropertiesKey", "AVVideoCompressionPropertiesKey"}, {"_AVVideoAverageBitRateKey", "AverageBitRate"},
             {"_AVVideoMaxKeyFrameIntervalKey", "MaxKeyFrameInterval"}, {"_AVVideoMaxKeyFrameIntervalDurationKey", "MaxKeyFrameIntervalDuration"},
             {"_AVVideoProfileLevelKey", "ProfileLevel"}, {"_AVVideoExpectedSourceFrameRateKey", "ExpectedFrameRate"},
             {"_AVVideoAllowFrameReorderingKey", "AllowFrameReordering"}, {"_AVVideoProfileLevelH264BaselineAutoLevel", "H264_Baseline_AutoLevel"},
             {"_AVVideoProfileLevelH264MainAutoLevel", "H264_Main_AutoLevel"}, {"_AVVideoProfileLevelH264HighAutoLevel", "H264_High_AutoLevel"},
             {"_AVEncoderBitRateKey", "AVEncoderBitRateKey"}, {"_AVEncoderBitDepthHintKey", "AVEncoderBitDepthHintKey"}})
        support.string_constant(av_foundation, symbol, value);
    const auto writer_of = [sp](Address object) {
        const auto w = sp->objc.find_host_state<WriterState>(object);
        if (!w) unsupported("an AVAssetWriter Alchemy did not make");
        return w;
    };
    const auto input_of = [sp](Address object) {
        const auto in = sp->objc.find_host_state<WriterInput>(object);
        if (!in) unsupported("an AVAssetWriterInput Alchemy did not make");
        return in;
    };
    const auto read_time = [](GuestMemory& m, Address at) {
        return CMTime{static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12), static_cast<std::int64_t>(m.read64(at + 16))};
    };
    const auto write_time = [](GuestMemory& m, Address at, const CMTime& t) {
        m.write64(at, static_cast<std::uint64_t>(t.value));
        m.write32(at + 8, static_cast<std::uint32_t>(t.timescale));
        m.write32(at + 12, t.flags);
        m.write64(at + 16, static_cast<std::uint64_t>(t.epoch));
    };

    // ---- AVAssetWriter ----
    support.method(writer_class_, "initWithURL:fileType:error:", [self, sp](CpuState& s, GuestMemory&) {
        const auto type = s.x[3] ? sp->cf.utf8(s.x[3]) : std::string{};
        const std::uint32_t container = type == "com.microsoft.waveform-audio" ? af::four("WAVE") : type == "public.aiff-audio" ? af::four("AIFF")
                                      : type == "public.aifc-audio" ? af::four("AIFC") : type == "com.apple.coreaudio-format" ? af::four("caff")
                                      : type == "com.apple.m4a-audio" ? af::four("m4af") : type == "public.mpeg-4" ? af::four("mp4f")
                                      : type == "com.apple.m4v-video" ? af::four("M4V ") : type == "com.apple.quicktime-movie" ? af::four("qt  ") : 0;
        if (!s.x[2] || !container) {
            if (s.x[4])
                self->bridge_->store_error(s.x[4], self->bridge_->error("AVFoundationErrorDomain", av_error_unknown,
                                                                        "AVAssetWriter writes WAVE, AIFF, AIFC, CAF, M4A, MPEG-4, M4V and QuickTime movie files"),
                                           true);
            sp->release(s.x[0]);
            s.x[0] = 0;
            return;
        }
        const auto w = sp->objc.host_state<WriterState>(s.x[0]);
        w->url = sp->retain(s.x[2]);
        w->file_type = sp->cf.copy(s.x[3]);
        w->container = container;
    }, "@@:@@^@");
    support.method(writer_class_, "assetWriterWithURL:fileType:error:", [sp](CpuState& s, GuestMemory&) {
        const auto made = sp->send_id(sp->send_id(s.x[0], "alloc"), "initWithURL:fileType:error:", {s.x[2], s.x[3], s.x[4]});
        s.x[0] = made ? sp->send_id(made, "autorelease") : 0;
    }, "@@:@@^@", true);
    support.method(writer_class_, "outputURL", [writer_of](CpuState& s, GuestMemory&) { s.x[0] = writer_of(s.x[0])->url; }, "@@:");
    support.method(writer_class_, "outputFileType", [writer_of](CpuState& s, GuestMemory&) { s.x[0] = writer_of(s.x[0])->file_type; }, "@@:");
    support.method(writer_class_, "status", [writer_of](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        s.x[0] = static_cast<std::uint64_t>(w->status);
    }, "q@:");
    support.method(writer_class_, "error", [writer_of](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        s.x[0] = w->error;
    }, "@@:");
    support.method(writer_class_, "metadata", [writer_of, sp](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        s.x[0] = w->metadata ? w->metadata : sp->send_id(sp->objc.find_class("NSArray"), "array");
    }, "@@:");
    // Kept, not written: the files carry no AVMetadataItems here.
    support.method(writer_class_, "setMetadata:", [writer_of, sp](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        sp->release(std::exchange(w->metadata, s.x[2] ? sp->cf.copy(s.x[2]) : 0));
    }, "v@:@");
    support.method(writer_class_, "shouldOptimizeForNetworkUse", [writer_of](CpuState& s, GuestMemory&) { s.x[0] = writer_of(s.x[0])->optimize ? 1 : 0; }, "B@:");
    support.method(writer_class_, "setShouldOptimizeForNetworkUse:", [writer_of](CpuState& s, GuestMemory&) { writer_of(s.x[0])->optimize = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(writer_class_, "movieFragmentInterval", [writer_of, write_time](CpuState& s, GuestMemory& m) { write_time(m, s.x[8], writer_of(s.x[0])->fragment_interval); }, "{?=qiIq}@:");
    support.method(writer_class_, "setMovieFragmentInterval:", [writer_of, read_time](CpuState& s, GuestMemory& m) { writer_of(s.x[0])->fragment_interval = read_time(m, s.x[2]); }, "v@:{?=qiIq}");
    support.method(writer_class_, "availableMediaTypes", [self, sp, writer_of](CpuState& s, GuestMemory&) {
        const bool movie = movie_container(writer_of(s.x[0])->container);
        std::vector<Address> types{sp->cf.constant_string("soun")};
        if (movie) types.push_back(sp->cf.constant_string("vide"));
        s.x[0] = sp->send_id(self->bridge_->array(types), "autorelease");
    }, "@@:");
    support.method(writer_class_, "inputs", [writer_of, self, sp](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        s.x[0] = sp->send_id(self->bridge_->array(w->inputs), "autorelease");
    }, "@@:");
    // Settings an encoder here takes (or nil: passthrough) for a media type the file holds.
    support.method(writer_class_, "canApplyOutputSettings:forMediaType:", [self, sp, writer_of](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        const auto type = s.x[3] ? sp->cf.utf8(s.x[3]) : std::string{};
        const bool movie = movie_container(w->container);
        if (type == "vide") s.x[0] = movie && (!s.x[2] || self->video_settings(s.x[2])) ? 1 : 0;
        else s.x[0] = type == "soun" && (!s.x[2] || self->settings_asbd(s.x[2], movie ? w->container : af::four("caff"))) ? 1 : 0;
    }, "B@:@@");
    support.method(writer_class_, "canAddInput:", [self, sp, writer_of](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        const auto in = s.x[2] ? sp->objc.find_host_state<WriterInput>(s.x[2]) : nullptr;
        std::lock_guard lock(w->mutex);
        s.x[0] = in && w->status == status_unknown && !in->writer && self->input_fits(*w, *in) ? 1 : 0;
    }, "B@:@");
    support.method(writer_class_, "addInput:", [sp, writer_of](CpuState& s, GuestMemory&) {
        if (!(sp->send(s.x[0], "canAddInput:", [&] { CallRegs r{}; r.x[2] = s.x[2]; return r; }()).x0 & 0xff))
            throw std::runtime_error("Alchemy: -[AVAssetWriter addInput:] cannot add this input (before -startWriting: in a movie, video or audio whose "
                                     "settings an encoder here takes, or compressed samples; in an audio file, one audio input the file type holds) "
                                     "(NSInvalidArgumentException)");
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        w->inputs.push_back(sp->retain(s.x[2]));
        sp->objc.find_host_state<WriterInput>(s.x[2])->writer = s.x[0];
    }, "v@:@");
    support.method(writer_class_, "startWriting", [self, sp, writer_of](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        if (w->status != status_unknown) throw std::runtime_error("Alchemy: -[AVAssetWriter startWriting] called twice (NSInternalInconsistencyException)");
        w->status = status_writing;
        if (movie_container(w->container)) {
            if (self->start_movie(*w))
                for (const auto input : w->inputs)
                    if (!self->start_movie_input(*w, input, *sp->objc.find_host_state<WriterInput>(input))) break;
            if (w->status != status_writing) self->close_movie(*w, true);
        } else if (!w->inputs.empty()) {
            const auto in = sp->objc.find_host_state<WriterInput>(w->inputs.front());
            if (const auto file = self->settings_asbd(in->settings, w->container))
                if (const auto status = self->create_file(*w, *file)) self->fail(*w, status, "AVAssetWriter could not create the file in that format");
        }
        s.x[0] = w->status == status_writing ? 1 : 0;
    }, "B@:");
    support.method(writer_class_, "startSessionAtSourceTime:", [writer_of, read_time](CpuState& s, GuestMemory& m) {
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        w->start = read_time(m, s.x[2]);
        std::lock_guard movie(w->movie_mutex);
        w->movie_start = is_numeric(*w->start) ? *w->start : time_zero;
    }, "v@:{?=qiIq}");
    support.method(writer_class_, "endSessionAtSourceTime:", [writer_of, read_time](CpuState& s, GuestMemory& m) {
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        w->end = read_time(m, s.x[2]);
    }, "v@:{?=qiIq}");
    // Completes the file; the status then says how it went.
    const auto finish = [self, sp](Address object) {
        const auto w = sp->objc.find_host_state<WriterState>(object);
        std::lock_guard lock(w->mutex);
        if (w->status != status_writing) return;
        if (movie_container(w->container)) {
            self->finish_movie(*w);
            self->close_movie(*w, w->status != status_writing);
            if (w->status == status_writing) w->status = status_completed;
            return;
        }
        if (w->ext) {
            const auto status = static_cast<std::int32_t>(sp->call(audio_toolbox, "_ExtAudioFileDispose", {std::exchange(w->ext, 0)}));
            if (status) {
                self->fail(*w, status, "AVAssetWriter could not finish the file");
                return;
            }
        }
        w->status = status_completed;
    };
    support.method(writer_class_, "finishWriting", [finish, writer_of](CpuState& s, GuestMemory&) {
        finish(s.x[0]);
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        s.x[0] = w->status == status_completed ? 1 : 0;
    }, "B@:");
    support.method(writer_class_, "finishWritingWithCompletionHandler:", [self, sp, finish, writer_of](CpuState& s, GuestMemory&) {
        finish(s.x[0]);
        if (!s.x[2]) return;
        const auto w = writer_of(s.x[0]);
        std::shared_ptr<CompletionThread> thread;
        {
            std::lock_guard lock(w->mutex);
            if (!w->thread) w->thread = start_completion_thread(sp->process, "com.apple.avfoundation.assetwriter");
            thread = w->thread;
        }
        thread->post_block(self->bridge_->block_copy(s.x[2]), {});
    }, "v@:@?");
    support.method(writer_class_, "cancelWriting", [self, sp, writer_of](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        Address url{};
        {
            std::lock_guard lock(w->mutex);
            if (w->status != status_writing && w->status != status_unknown) return;
            if (w->ext) static_cast<void>(sp->call(audio_toolbox, "_ExtAudioFileDispose", {std::exchange(w->ext, 0)}));
            if (movie_container(w->container)) self->close_movie(*w, false);
            w->status = status_cancelled;
            url = w->url;
        }
        const auto manager = sp->send_id(sp->objc.find_class("NSFileManager"), "defaultManager");
        if (manager && url) static_cast<void>(sp->send_id(manager, "removeItemAtURL:error:", {url, 0}));
    }, "v@:");
    support.method(writer_class_, "dealloc", [self, sp](CpuState& s, GuestMemory&) {
        if (const auto w = sp->objc.find_host_state<WriterState>(s.x[0])) {
            if (w->ext) static_cast<void>(sp->call(audio_toolbox, "_ExtAudioFileDispose", {std::exchange(w->ext, 0)}));
            if (movie_container(w->container)) self->close_movie(*w, w->status == status_writing);
            for (const auto input : std::exchange(w->inputs, {})) {
                if (const auto in = sp->objc.find_host_state<WriterInput>(input)) in->writer = 0;
                sp->release(input);
            }
            for (auto* object : {&w->url, &w->file_type, &w->error, &w->metadata}) sp->release(std::exchange(*object, 0));
            if (w->thread) w->thread->shutdown();
        }
        static_cast<void>(sp->objc.send_super(s.x[0], self->writer_class_, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- AVAssetWriterInput ----
    const auto init_input = [sp](Address object, Address type, Address settings, Address hint) {
        if (!type) throw std::runtime_error("Alchemy: -[AVAssetWriterInput initWithMediaType:outputSettings:] needs a media type (NSInvalidArgumentException)");
        const auto in = sp->objc.host_state<WriterInput>(object);
        in->media_type = sp->cf.copy(type);
        in->settings = settings ? sp->cf.copy(settings) : 0;
        in->hint = sp->retain(hint);
        return object;
    };
    support.method(input_class_, "initWithMediaType:outputSettings:", [init_input](CpuState& s, GuestMemory&) { s.x[0] = init_input(s.x[0], s.x[2], s.x[3], 0); }, "@@:@@");
    support.method(input_class_, "initWithMediaType:outputSettings:sourceFormatHint:", [init_input](CpuState& s, GuestMemory&) {
        s.x[0] = init_input(s.x[0], s.x[2], s.x[3], s.x[4]);
    }, "@@:@@^{opaqueCMFormatDescription=}");
    support.method(input_class_, "assetWriterInputWithMediaType:outputSettings:", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->send_id(sp->send_id(s.x[0], "alloc"), "initWithMediaType:outputSettings:", {s.x[2], s.x[3]}), "autorelease");
    }, "@@:@@", true);
    support.method(input_class_, "assetWriterInputWithMediaType:outputSettings:sourceFormatHint:", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->send_id(sp->send_id(s.x[0], "alloc"), "initWithMediaType:outputSettings:sourceFormatHint:", {s.x[2], s.x[3], s.x[4]}), "autorelease");
    }, "@@:@@^{opaqueCMFormatDescription=}", true);
    support.method(input_class_, "mediaType", [input_of](CpuState& s, GuestMemory&) { s.x[0] = input_of(s.x[0])->media_type; }, "@@:");
    support.method(input_class_, "outputSettings", [input_of](CpuState& s, GuestMemory&) { s.x[0] = input_of(s.x[0])->settings; }, "@@:");
    support.method(input_class_, "sourceFormatHint", [input_of](CpuState& s, GuestMemory&) { s.x[0] = input_of(s.x[0])->hint; }, "^{opaqueCMFormatDescription=}@:");
    support.method(input_class_, "isReadyForMoreMediaData", [sp, input_of](CpuState& s, GuestMemory&) {
        const auto in = input_of(s.x[0]);
        const auto w = in->writer ? sp->objc.find_host_state<WriterState>(in->writer) : nullptr;
        if (!w) { s.x[0] = 0; return; }
        std::lock_guard lock(w->mutex);
        s.x[0] = w->status == status_writing && !in->finished ? 1 : 0;
    }, "B@:");
    support.method(input_class_, "expectsMediaDataInRealTime", [input_of](CpuState& s, GuestMemory&) { s.x[0] = input_of(s.x[0])->realtime ? 1 : 0; }, "B@:");
    support.method(input_class_, "setExpectsMediaDataInRealTime:", [input_of](CpuState& s, GuestMemory&) { input_of(s.x[0])->realtime = (s.x[2] & 0xff) != 0; }, "v@:B");
    // CGAffineTransform (six doubles: not a homogeneous aggregate of at most four, so by reference / through x8).
    support.method(input_class_, "transform", [input_of](CpuState& s, GuestMemory& m) {
        const auto& t = input_of(s.x[0])->transform;
        for (std::size_t i = 0; i < t.size(); ++i) m.write64(s.x[8] + 8 * i, std::bit_cast<std::uint64_t>(t[i]));
    }, "{CGAffineTransform=dddddd}@:");
    support.method(input_class_, "setTransform:", [input_of](CpuState& s, GuestMemory& m) {
        auto& t = input_of(s.x[0])->transform;
        for (std::size_t i = 0; i < t.size(); ++i) t[i] = std::bit_cast<double>(m.read64(s.x[2] + 8 * i));
    }, "v@:{CGAffineTransform=dddddd}");
    support.method(input_class_, "appendSampleBuffer:", [self](CpuState& s, GuestMemory&) { s.x[0] = self->append(s.x[0], s.x[2]) ? 1 : 0; }, "B@:^{opaqueCMSampleBuffer=}");
    support.method(input_class_, "markAsFinished", [input_of](CpuState& s, GuestMemory&) { input_of(s.x[0])->finished = true; }, "v@:");
    // The block runs on the queue, and again after each call that appended data, until the input is finished.
    support.method(input_class_, "requestMediaDataWhenReadyOnQueue:usingBlock:", [self, sp, input_of](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !s.x[3])
            throw std::runtime_error("Alchemy: -[AVAssetWriterInput requestMediaDataWhenReadyOnQueue:usingBlock:] needs a queue and a block (NSInvalidArgumentException)");
        const auto in = input_of(s.x[0]);
        const auto input = sp->retain(s.x[0]);
        const auto queue = s.x[2];
        static_cast<void>(sp->call(libsystem, "_dispatch_retain", {queue}));
        const auto block = self->bridge_->block_copy(s.x[3]);
        const auto b = self->bridge_;
        // Each dispatched closure keeps the step alive while it runs; the step itself only refers to it weakly.
        auto step = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak = step;
        *step = [b, sp, in, input, queue, block, weak] {
            const auto before = in->appended;
            static_cast<void>(b->invoke_block(block, {}));
            if (!in->finished && in->appended != before && in->writer) {
                if (const auto again = weak.lock()) {
                    b->dispatch_async(queue, [again] { (*again)(); });
                    return;
                }
            }
            b->block_release(block);
            static_cast<void>(sp->call(libsystem, "_dispatch_release", {queue}));
            sp->release(input);
        };
        b->dispatch_async(queue, [step] { (*step)(); });
    }, "v@:@@?");
    support.method(input_class_, "dealloc", [self, sp](CpuState& s, GuestMemory&) {
        if (const auto in = sp->objc.find_host_state<WriterInput>(s.x[0])) {
            for (auto* object : {&in->media_type, &in->settings, &in->hint}) sp->release(std::exchange(*object, 0));
            if (in->movie.session) {
                static_cast<void>(sp->call(video_toolbox, "_VTCompressionSessionInvalidate", {in->movie.session}));
                sp->release(std::exchange(in->movie.session, 0));
            }
            if (in->movie.handler) self->bridge_->forget_block(std::exchange(in->movie.handler, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], self->input_class_, sp->objc.selector("dealloc")));
    }, "v@:");

    // ---- AVAssetWriterInputPixelBufferAdaptor ----
    // Pixel buffers into a video input with settings, through its compression session; the pool is the session's.
    const auto adaptor_of = [sp](Address object) {
        const auto a = sp->objc.find_host_state<AdaptorState>(object);
        if (!a) unsupported("an AVAssetWriterInputPixelBufferAdaptor Alchemy did not make");
        return a;
    };
    support.method(adaptor_class_, "initWithAssetWriterInput:sourcePixelBufferAttributes:", [sp](CpuState& s, GuestMemory&) {
        const auto in = s.x[2] ? sp->objc.find_host_state<WriterInput>(s.x[2]) : nullptr;
        if (!in || sp->cf.utf8(in->media_type) != "vide" || in->writer)
            throw std::runtime_error("Alchemy: -[AVAssetWriterInputPixelBufferAdaptor initWithAssetWriterInput:...] needs a video input not yet added to a writer "
                                     "(NSInvalidArgumentException)");
        const auto a = sp->objc.host_state<AdaptorState>(s.x[0]);
        a->input = sp->retain(s.x[2]);
        a->attributes = s.x[3] ? sp->cf.copy(s.x[3]) : 0;
    }, "@@:@@");
    support.method(adaptor_class_, "assetWriterInputPixelBufferAdaptorWithAssetWriterInput:sourcePixelBufferAttributes:", [sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(sp->send_id(sp->send_id(s.x[0], "alloc"), "initWithAssetWriterInput:sourcePixelBufferAttributes:", {s.x[2], s.x[3]}), "autorelease");
    }, "@@:@@", true);
    support.method(adaptor_class_, "assetWriterInput", [adaptor_of](CpuState& s, GuestMemory&) { s.x[0] = adaptor_of(s.x[0])->input; }, "@@:");
    support.method(adaptor_class_, "sourcePixelBufferAttributes", [adaptor_of](CpuState& s, GuestMemory&) { s.x[0] = adaptor_of(s.x[0])->attributes; }, "@@:");
    // nil until -startWriting made the session.
    support.method(adaptor_class_, "pixelBufferPool", [sp, adaptor_of](CpuState& s, GuestMemory&) {
        const auto in = sp->objc.find_host_state<WriterInput>(adaptor_of(s.x[0])->input);
        s.x[0] = in && in->movie.session ? sp->call(video_toolbox, "_VTCompressionSessionGetPixelBufferPool", {in->movie.session}) : 0;
    }, "^{__CVPixelBufferPool=}@:");
    support.method(adaptor_class_, "appendPixelBuffer:withPresentationTime:", [self, sp, adaptor_of, read_time](CpuState& s, GuestMemory& m) {
        const auto input = adaptor_of(s.x[0])->input;
        const auto in = sp->objc.find_host_state<WriterInput>(input);
        const auto w = in && in->writer ? sp->objc.find_host_state<WriterState>(in->writer) : nullptr;
        s.x[0] = 0;
        if (!w || !s.x[2]) return;
        std::lock_guard lock(w->mutex);
        if (w->status != status_writing || in->finished || !w->start || !in->movie.session) return;
        const auto pts = read_time(m, s.x[3]);
        if (w->end && is_numeric(*w->end) && is_numeric(pts) && compare(pts, *w->end) >= 0) { s.x[0] = 1; return; }
        if (self->encode_image(*w, *in, s.x[2], pts, time_invalid)) {
            ++in->appended;
            s.x[0] = 1;
        }
    }, "B@:^{__CVBuffer=}{?=qiIq}");
    support.method(adaptor_class_, "dealloc", [self, sp](CpuState& s, GuestMemory&) {
        if (const auto a = sp->objc.find_host_state<AdaptorState>(s.x[0]))
            for (auto* object : {&a->input, &a->attributes}) sp->release(std::exchange(*object, 0));
        static_cast<void>(sp->objc.send_super(s.x[0], self->adaptor_class_, sp->objc.selector("dealloc")));
    }, "v@:");
}

} // namespace

void install_av_asset_writer(Support& support) { std::make_shared<Writers>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
