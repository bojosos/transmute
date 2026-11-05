#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/cmtime.hpp"
#include "alchemy/darwin/media/packet_file.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// AVAssetWriter and AVAssetWriterInput for audio files: WAVE, AIFF, AIFC and CAF of linear PCM, and CAF, Apple M4A and
// MPEG-4 of Apple Lossless, written through ExtAudioFile (audio_file.cpp).
// - The file's format is the audio input's outputSettings (AVFormatIDKey linear PCM, AVSampleRateKey,
//   AVNumberOfChannelsKey, AVLinearPCMBitDepthKey, ...IsFloat, ...IsBigEndian; or AVFormatIDKey 'alac' with
//   AVEncoderBitDepthHintKey or AVLinearPCMBitDepthKey bits, 16 by default), or with nil settings the format of the
//   first appended buffer (linear PCM files only). The file is created by -startWriting (with settings) or at the first
//   append.
// - -appendSampleBuffer: writes a linear PCM CMSampleBuffer at the file's sample rate (converted between PCM layouts,
//   sample formats and channel counts by ExtAudioFile); buffers that end before the session's start time or begin at
//   or after its end time are dropped. Inputs are always ready for more data until -markAsFinished.
//   -requestMediaDataWhenReadyOnQueue:usingBlock: calls the block on the queue again while it keeps appending.
// - -finishWritingWithCompletionHandler: closes the file, sets the status to completed and calls the handler on a
//   thread of the writer; -cancelWriting deletes the file.
// - Unavailable, as documented answers: video inputs (no video encoder: -canAddInput: NO, -addInput: stops with
//   NSInvalidArgumentException), compressed audio settings other than Apple Lossless (no encoder:
//   -canApplyOutputSettings:forMediaType: NO), and the QuickTime movie and video file types (the initializer fails with
//   an error).
namespace alchemy::gui::completion_kit::media {
namespace {

namespace af = audio_file;

// The file types that hold linear PCM.
bool pcm_container(std::uint32_t container) {
    return container == af::four("WAVE") || container == af::four("AIFF") || container == af::four("AIFC") || container == af::four("caff");
}

constexpr std::string_view av_foundation = "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation";
constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view core_media = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// AVAssetWriterStatus
constexpr std::int64_t status_unknown = 0, status_writing = 1, status_completed = 2, status_failed = 3, status_cancelled = 4;
constexpr std::int64_t av_error_unknown = -11800, av_error_unsupported_output_settings = -11861;

struct WriterState final : ObjectPayload {
    std::mutex mutex;
    Address url{}, file_type{}, error{}, metadata{};   // retained
    std::uint32_t container{};                         // AudioFileTypeID
    std::int64_t status{};
    std::vector<Address> inputs;                       // retained
    Address ext{};                                     // ExtAudioFileRef once created
    std::vector<std::uint8_t> client;                  // the ASBD currently set as the client format
    std::optional<CMTime> start, end;
    bool optimize{};
    CMTime fragment_interval = time_invalid;
    std::shared_ptr<CompletionThread> thread;
};
struct WriterInput final : ObjectPayload {
    Address media_type{}, settings{}, hint{};          // retained
    Address writer{};                                  // not retained (the writer holds its inputs)
    bool finished{}, realtime{};
    std::array<double, 6> transform{1, 0, 0, 1, 0, 0};
    std::uint64_t appended{};
};

class Writers : public std::enable_shared_from_this<Writers> {
public:
    explicit Writers(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    // The file's AudioStreamBasicDescription for `settings` in `container`, or nullopt (a format the container does not
    // hold or nothing encodes, or incomplete settings).
    std::optional<std::vector<std::uint8_t>> settings_asbd(Address settings, std::uint32_t container);
    std::int32_t create_file(WriterState& w, const std::vector<std::uint8_t>& asbd);
    bool append(Address input_object, Address sample);
    void fail(WriterState& w, std::int64_t code, const std::string& text) {
        w.status = status_failed;
        if (!w.error) w.error = bridge_->error("AVFoundationErrorDomain", code, text);
    }

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    Address writer_class_{}, input_class_{};
};

std::optional<std::vector<std::uint8_t>> Writers::settings_asbd(Address settings, std::uint32_t container) {
    if (!settings || !bridge_->is_dictionary(settings)) return std::nullopt;
    const auto setting = [&](const char* key) -> std::optional<double> {
        const auto k = support_.cf.string(key);
        const auto value = bridge_->lookup(settings, k);
        support_.release(k);
        return value ? bridge_->real(value) : std::nullopt;
    };
    const auto format_id = static_cast<std::uint32_t>(setting("AVFormatIDKey").value_or(af::four("lpcm")));
    const auto rate = setting("AVSampleRateKey"), channels = setting("AVNumberOfChannelsKey");
    if (!rate || !(*rate > 0) || !channels || *channels < 1 || *channels > 64) return std::nullopt;
    if (format_id == af::four("alac")) {
        // kAppleLosslessFormatFlag_16/20/24/32BitSourceData, 4096 frames per packet.
        if (!packet_file::writable(container, format_id) || *channels > 8) return std::nullopt;
        const auto depth = setting("AVEncoderBitDepthHintKey").value_or(setting("AVLinearPCMBitDepthKey").value_or(16));
        const std::uint32_t flags = depth <= 16 ? 1 : depth <= 20 ? 2 : depth <= 24 ? 3 : 4, per_packet = 4096,
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

std::int32_t Writers::create_file(WriterState& w, const std::vector<std::uint8_t>& asbd) {
    auto& m = support_.memory();
    const auto scratch = support_.process.allocate_guest(48, 8);
    m.write_bytes(scratch, asbd);
    m.write64(scratch + 40, 0);
    // ExtAudioFileCreateWithURL(url, type, &format, NULL, kAudioFileFlags_EraseFile, &out)
    const auto status = static_cast<std::int32_t>(support_.call(audio_toolbox, "_ExtAudioFileCreateWithURL", {w.url, w.container, scratch, 0, 1, scratch + 40}));
    if (!status) w.ext = m.read64(scratch + 40);
    support_.process.release_guest(scratch);
    return status;
}

bool Writers::append(Address input_object, Address sample) {
    const auto in = support_.objc.find_host_state<WriterInput>(input_object);
    const auto w = in && in->writer ? support_.objc.find_host_state<WriterState>(in->writer) : nullptr;
    if (!w || !sample) return false;
    auto& m = support_.memory();
    std::lock_guard lock(w->mutex);
    if (w->status != status_writing || in->finished || !w->start) return false;
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
    // The session's time range.
    const auto times = support_.process.allocate_guest(48, 8);
    CallRegs r{};
    r.x[0] = sample;
    r.indirect_result = times;
    static_cast<void>(EngineAdapter(support_.process.calling_engine()).call_regs(bridge_->symbol(core_media, "_CMSampleBufferGetPresentationTimeStamp"), r));
    r.indirect_result = times + 24;
    static_cast<void>(EngineAdapter(support_.process.calling_engine()).call_regs(bridge_->symbol(core_media, "_CMSampleBufferGetDuration"), r));
    const auto read_time = [&](Address at) {
        return CMTime{static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12), static_cast<std::int64_t>(m.read64(at + 16))};
    };
    const auto pts = read_time(times), duration = read_time(times + 24);
    support_.process.release_guest(times);
    if (is_numeric(pts)) {
        const auto end_of_buffer = is_numeric(duration) ? add(pts, duration) : pts;
        if (is_numeric(*w->start) && compare(end_of_buffer, *w->start) <= 0) return true;   // before the session: dropped
        if (w->end && is_numeric(*w->end) && compare(pts, *w->end) >= 0) return true;      // after it
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
    const auto list = scratch + 40, data = list + 8 + 16ULL * planes;
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

void Writers::install() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    writer_class_ = support.declare_class("AVAssetWriter", "NSObject", av_foundation);
    input_class_ = support.declare_class("AVAssetWriterInput", "NSObject", av_foundation);
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_AVFileTypeWAVE", "com.microsoft.waveform-audio"}, {"_AVFileTypeAIFF", "public.aiff-audio"}, {"_AVFileTypeAIFC", "public.aifc-audio"},
             {"_AVFileTypeCoreAudioFormat", "com.apple.coreaudio-format"}, {"_AVFileTypeQuickTimeMovie", "com.apple.quicktime-movie"},
             {"_AVFileTypeMPEG4", "public.mpeg-4"}, {"_AVFileTypeAppleM4A", "com.apple.m4a-audio"}, {"_AVFileTypeAppleM4V", "com.apple.m4v-video"},
             {"_AVFileTypeMPEGLayer3", "public.mp3"}})
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
                                      : type == "com.apple.m4a-audio" ? af::four("m4af") : type == "public.mpeg-4" ? af::four("mp4f") : 0;
        if (!s.x[2] || !container) {
            if (s.x[4])
                self->bridge_->store_error(s.x[4], self->bridge_->error("AVFoundationErrorDomain", av_error_unknown,
                                                                        "AVAssetWriter writes WAVE, AIFF, AIFC, CAF, M4A and MPEG-4 audio files (no QuickTime movie writer exists under Alchemy)"),
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
    // Kept, not written: the audio containers carry no AVMetadataItems here.
    support.method(writer_class_, "setMetadata:", [writer_of, sp](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        sp->release(std::exchange(w->metadata, s.x[2] ? sp->cf.copy(s.x[2]) : 0));
    }, "v@:@");
    support.method(writer_class_, "shouldOptimizeForNetworkUse", [writer_of](CpuState& s, GuestMemory&) { s.x[0] = writer_of(s.x[0])->optimize ? 1 : 0; }, "B@:");
    support.method(writer_class_, "setShouldOptimizeForNetworkUse:", [writer_of](CpuState& s, GuestMemory&) { writer_of(s.x[0])->optimize = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(writer_class_, "movieFragmentInterval", [writer_of, write_time](CpuState& s, GuestMemory& m) { write_time(m, s.x[8], writer_of(s.x[0])->fragment_interval); }, "{?=qiIq}@:");
    support.method(writer_class_, "setMovieFragmentInterval:", [writer_of, read_time](CpuState& s, GuestMemory& m) { writer_of(s.x[0])->fragment_interval = read_time(m, s.x[2]); }, "v@:{?=qiIq}");
    support.method(writer_class_, "availableMediaTypes", [self, sp](CpuState& s, GuestMemory&) {
        s.x[0] = sp->send_id(self->bridge_->array({sp->cf.constant_string("soun")}), "autorelease");
    }, "@@:");
    support.method(writer_class_, "inputs", [writer_of, self, sp](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        s.x[0] = sp->send_id(self->bridge_->array(w->inputs), "autorelease");
    }, "@@:");
    // Linear PCM or Apple Lossless audio settings (or nil: passthrough); nothing else has an encoder.
    support.method(writer_class_, "canApplyOutputSettings:forMediaType:", [self, sp](CpuState& s, GuestMemory&) {
        const auto type = s.x[3] ? sp->cf.utf8(s.x[3]) : std::string{};
        s.x[0] = type == "soun" && (!s.x[2] || self->settings_asbd(s.x[2], af::four("caff"))) ? 1 : 0;
    }, "B@:@@");
    support.method(writer_class_, "canAddInput:", [self, sp, writer_of](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        const auto in = s.x[2] ? sp->objc.find_host_state<WriterInput>(s.x[2]) : nullptr;
        std::lock_guard lock(w->mutex);
        const bool audio = in && sp->cf.utf8(in->media_type) == "soun";
        // Passthrough (nil settings) writes linear PCM, which MPEG-4 files do not hold.
        const bool format_ok = in && (in->settings ? self->settings_asbd(in->settings, w->container).has_value() : pcm_container(w->container));
        s.x[0] = audio && w->status == status_unknown && w->inputs.empty() && !in->writer && format_ok ? 1 : 0;
    }, "B@:@");
    support.method(writer_class_, "addInput:", [sp, writer_of](CpuState& s, GuestMemory&) {
        if (!(sp->send(s.x[0], "canAddInput:", [&] { CallRegs r{}; r.x[2] = s.x[2]; return r; }()).x0 & 0xff))
            throw std::runtime_error("Alchemy: -[AVAssetWriter addInput:] cannot add this input (one linear PCM or Apple Lossless audio input "
                                     "the file type holds, before -startWriting; no video encoder exists under Alchemy) (NSInvalidArgumentException)");
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
        if (!w->inputs.empty()) {
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
    }, "v@:{?=qiIq}");
    support.method(writer_class_, "endSessionAtSourceTime:", [writer_of, read_time](CpuState& s, GuestMemory& m) {
        const auto w = writer_of(s.x[0]);
        std::lock_guard lock(w->mutex);
        w->end = read_time(m, s.x[2]);
    }, "v@:{?=qiIq}");
    // Closes the file; the status then says how it went.
    const auto finish = [self, sp](Address object) {
        const auto w = sp->objc.find_host_state<WriterState>(object);
        std::lock_guard lock(w->mutex);
        if (w->status != status_writing) return;
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
    support.method(writer_class_, "cancelWriting", [sp, writer_of](CpuState& s, GuestMemory&) {
        const auto w = writer_of(s.x[0]);
        Address url{};
        {
            std::lock_guard lock(w->mutex);
            if (w->status != status_writing && w->status != status_unknown) return;
            if (w->ext) static_cast<void>(sp->call(audio_toolbox, "_ExtAudioFileDispose", {std::exchange(w->ext, 0)}));
            w->status = status_cancelled;
            url = w->url;
        }
        const auto manager = sp->send_id(sp->objc.find_class("NSFileManager"), "defaultManager");
        if (manager && url) static_cast<void>(sp->send_id(manager, "removeItemAtURL:error:", {url, 0}));
    }, "v@:");
    support.method(writer_class_, "dealloc", [self, sp](CpuState& s, GuestMemory&) {
        if (const auto w = sp->objc.find_host_state<WriterState>(s.x[0])) {
            if (w->ext) static_cast<void>(sp->call(audio_toolbox, "_ExtAudioFileDispose", {std::exchange(w->ext, 0)}));
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
        if (const auto in = sp->objc.find_host_state<WriterInput>(s.x[0]))
            for (auto* object : {&in->media_type, &in->settings, &in->hint}) sp->release(std::exchange(*object, 0));
        static_cast<void>(sp->objc.send_super(s.x[0], self->input_class_, sp->objc.selector("dealloc")));
    }, "v@:");
}

} // namespace

void install_av_asset_writer(Support& support) { std::make_shared<Writers>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
