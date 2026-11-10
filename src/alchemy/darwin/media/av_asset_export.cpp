#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/aac_encoder.hpp"
#include "alchemy/darwin/media/audio_file.hpp"
#include "alchemy/darwin/media/cmtime.hpp"
#include "alchemy/darwin/media/media_container.hpp"
#include "alchemy/darwin/media/movie_file.hpp"
#include "alchemy/darwin/media/video_encoder.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// AVAssetExportSession (E5): an asset into a movie or an M4A file.
// - Presets:
//   - Passthrough: the source file's samples as they are (H.264, HEVC, AAC and Apple Lossless tracks of an MPEG-4
//     or QuickTime file), remuxed by the movie muxer (movie_file.hpp) into a QuickTime, MPEG-4 or M4V file. The
//     time range keeps the video from the sync sample before its start, and the audio from its priming's length
//     before it (the decoder's pre-roll), both trimmed exactly through the edit lists.
//   - AppleM4A: the asset's decodable audio (a file's audio track, or a composition's tracks with the audio mix)
//     encoded as AAC into an M4A file.
//   - The video presets encode the first decodable video track and the audio:
//     - LowQuality (within 320x240), MediumQuality (within 640x480), HighestQuality (the source's size), and
//       640x480, 960x540, 1280x720, 1920x1080, 3840x2160 (within those sizes, never larger than the source): H.264;
//     - HEVCHighestQuality, HEVC1920x1080 and HEVC3840x2160: HEVC;
//     - into QuickTime, MPEG-4 or M4V files.
//     The sizes are Alchemy's choices (the documentation names the presets, not their sizes); the encoders choose
//     the bit rates.
// - The pipeline is AVAssetReader (decoded pictures in 420v, the audio mix output in float) into AVAssetWriter (the
//   host's encoders through VTCompressionSession and the AAC encoder), on a thread of the session. Audio keeps its
//   rate where the AAC encoder takes it (else 48 kHz) and goes to at most two channels (the first two of more).
// - Discovery lists only what this machine can do for the asset: allExportPresets lists every preset;
//   exportPresetsCompatibleWithAsset: and determineCompatibility... need a decodable source track and an encoder
//   for it (Passthrough: remuxable samples), and supportedFileTypes follows the preset.
// - Errors: AVErrorEncoderNotFound without an encoder, AVErrorDecoderNotFound without a decodable track,
//   AVErrorFileAlreadyExists for an existing output file, AVErrorOperationNotSupportedForAsset for composition video
//   and Passthrough of what does not remux, AVErrorExportFailed for the rest. An AVVideoComposition stops with an
//   explicit message (not supported).
namespace alchemy::gui::completion_kit::media {
namespace {

namespace mf = movie_file;
namespace mc = container;

constexpr std::string_view av_foundation = "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation";
constexpr std::string_view core_media = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view core_foundation = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// AVAssetExportSessionStatus
constexpr std::int64_t export_unknown = 0, export_waiting = 1, export_exporting = 2, export_completed = 3, export_failed = 4, export_cancelled = 5;
// AVError codes (AVFoundationErrorDomain).
constexpr std::int64_t av_error_export_failed = -11820, av_error_file_already_exists = -11823, av_error_decoder_not_found = -11833,
                       av_error_encoder_not_found = -11834, av_error_operation_not_supported_for_asset = -11838;
constexpr std::uint64_t o_rdwr = 2, o_creat = 0x200, o_excl = 0x800;

constexpr std::uint32_t four(const char (&code)[5]) { return mc::fourcc(code); }

enum class Kind : std::uint8_t { passthrough, m4a, video };
struct Preset {
    const char* name;                  // the constant's value (unverified: the documentation gives the names only)
    Kind kind;
    std::uint32_t codec{};             // video presets: 'avc1' or 'hvc1'
    std::uint32_t long_side{}, short_side{};   // the box the picture fits in (0: the source's size)
};
constexpr Preset presets[] = {
    {"AVAssetExportPresetLowQuality", Kind::video, four("avc1"), 320, 240},
    {"AVAssetExportPresetMediumQuality", Kind::video, four("avc1"), 640, 480},
    {"AVAssetExportPresetHighestQuality", Kind::video, four("avc1"), 0, 0},
    {"AVAssetExportPreset640x480", Kind::video, four("avc1"), 640, 480},
    {"AVAssetExportPreset960x540", Kind::video, four("avc1"), 960, 540},
    {"AVAssetExportPreset1280x720", Kind::video, four("avc1"), 1280, 720},
    {"AVAssetExportPreset1920x1080", Kind::video, four("avc1"), 1920, 1080},
    {"AVAssetExportPreset3840x2160", Kind::video, four("avc1"), 3840, 2160},
    {"AVAssetExportPresetHEVCHighestQuality", Kind::video, four("hvc1"), 0, 0},
    {"AVAssetExportPresetHEVC1920x1080", Kind::video, four("hvc1"), 1920, 1080},
    {"AVAssetExportPresetHEVC3840x2160", Kind::video, four("hvc1"), 3840, 2160},
    {"AVAssetExportPresetAppleM4A", Kind::m4a},
    {"AVAssetExportPresetPassthrough", Kind::passthrough},
};
const Preset* preset_named(const std::string& name) {
    for (const auto& p : presets)
        if (name == p.name) return &p;
    return nullptr;
}

constexpr const char* type_mov = "com.apple.quicktime-movie";
constexpr const char* type_mp4 = "public.mpeg-4";
constexpr const char* type_m4v = "com.apple.m4v-video";
constexpr const char* type_m4a = "com.apple.m4a-audio";

struct ExportState final : ObjectPayload {
    std::mutex mutex;
    Address asset{}, preset{}, output_url{}, output_type{}, error{}, audio_mix{}, metadata{}, pitch{};   // retained
    const Preset* kind{};
    std::int64_t status{export_unknown};
    float progress{};
    bool optimize{};
    std::int64_t file_length_limit{};
    CMTimeRange range{time_zero, time_positive_infinity};
    std::atomic<bool> cancelled{false};
    std::shared_ptr<CompletionThread> thread;
};

// What the asset holds, as AVFoundation reports it.
struct Source {
    bool composition{};
    Address video{};                   // the first video track (not retained), or 0
    bool video_decodable{};
    std::uint32_t video_codec{};
    double width{}, height{};
    std::vector<Address> audio;        // the decodable audio tracks (not retained)
    bool any_audio{};
    CMTime duration{};
};

class Exports : public std::enable_shared_from_this<Exports> {
public:
    explicit Exports(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    Source inspect(Address asset);
    // Whether the preset can export the asset here; when not, the error code and why.
    bool compatible(const Preset& preset, Address asset, std::int64_t* code = nullptr, std::string* why = nullptr);
    std::vector<std::string> file_types(const Preset& preset) const;
    void run(Address session);
    bool passthrough(ExportState& e, std::string& why, std::int64_t& code);
    bool transcode(ExportState& e, const Source& source, std::string& why, std::int64_t& code);
    CMTime time_of(Address object, std::string_view selector);
    CMTime sample_time(Address sample, const char* function);
    Address text(std::string_view value) { return support_.cf.string(std::string(value)); }

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    Address class_{};
};

CMTime Exports::time_of(Address object, std::string_view selector) {
    auto& m = support_.memory();
    const auto at = support_.process.allocate_guest(24, 8);
    CallRegs r{};
    r.indirect_result = at;
    static_cast<void>(support_.send(object, selector, r));
    const CMTime t{static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12), static_cast<std::int64_t>(m.read64(at + 16))};
    support_.process.release_guest(at);
    return t;
}

CMTime Exports::sample_time(Address sample, const char* function) {
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

Source Exports::inspect(Address asset) {
    Source s;
    if (const auto composition = support_.objc.find_class("AVComposition"))
        s.composition = (support_.send(asset, "isKindOfClass:", [&] { CallRegs r{}; r.x[2] = composition; return r; }()).x0 & 0xff) != 0;
    const auto tracks = support_.send_id(asset, "tracks");
    const auto count = tracks ? support_.call(core_foundation, "_CFArrayGetCount", {tracks}) : 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto track = support_.call(core_foundation, "_CFArrayGetValueAtIndex", {tracks, i});
        const auto type = support_.cf.utf8(support_.send_id(track, "mediaType"));
        const bool decodable = (support_.send(track, "isDecodable").x0 & 0xff) != 0;
        if (type == "vide" && !s.video) {
            s.video = track;
            s.video_decodable = decodable;
            const auto descriptions = support_.send_id(track, "formatDescriptions");
            if (descriptions && support_.call(core_foundation, "_CFArrayGetCount", {descriptions}) > 0)
                s.video_codec = static_cast<std::uint32_t>(
                    support_.call(core_media, "_CMFormatDescriptionGetMediaSubType", {support_.call(core_foundation, "_CFArrayGetValueAtIndex", {descriptions, 0})}));
            const auto size = support_.send(track, "naturalSize");
            s.width = size.d(0);
            s.height = size.d(1);
        } else if (type == "soun") {
            s.any_audio = true;
            if (decodable) s.audio.push_back(track);
        }
    }
    s.duration = time_of(asset, "duration");
    return s;
}

bool Exports::compatible(const Preset& preset, Address asset, std::int64_t* code, std::string* why) {
    const auto refuse = [&](std::int64_t c, std::string text) {
        if (code) *code = c;
        if (why) *why = std::move(text);
        return false;
    };
    const auto source = inspect(asset);
    switch (preset.kind) {
    case Kind::passthrough: {
        if (source.composition) return refuse(av_error_operation_not_supported_for_asset, "Passthrough exports a file asset's samples (not a composition)");
        const auto url = support_.send_id(asset, "URL");
        if (!url) return refuse(av_error_operation_not_supported_for_asset, "Passthrough exports a file asset");
        std::uint64_t size = 0;
        std::int32_t status = 0;
        const auto fd = open_guest_read(support_, support_.cf.url_path(url), size, status);
        if (fd < 0) return refuse(av_error_export_failed, "the asset's file does not open");
        mc::Movie movie;
        std::string parse_why;
        const bool parsed = mc::parse_iso([&](std::uint64_t offset, std::span<std::uint8_t> out) { return read_guest(support_, fd, offset, out); }, size, movie,
                                          parse_why);
        close_guest_fd(support_, fd);
        if (!parsed) return refuse(av_error_operation_not_supported_for_asset, "Passthrough remuxes MPEG-4 and QuickTime files");
        bool any = false;
        for (const auto& t : movie.tracks) {
            if (t.kind == mc::Track::Kind::other) continue;
            const bool fits = t.kind == mc::Track::Kind::video ? (t.codec == four("avc1") || t.codec == four("hvc1")) : (t.codec == four("mp4a") || t.codec == four("alac"));
            if (!fits) return refuse(av_error_operation_not_supported_for_asset, "Passthrough remuxes H.264, HEVC, AAC and Apple Lossless tracks");
            any = true;
        }
        return any || refuse(av_error_operation_not_supported_for_asset, "the asset has no video or audio track");
    }
    case Kind::m4a:
        if (source.audio.empty()) return refuse(source.any_audio ? av_error_decoder_not_found : av_error_operation_not_supported_for_asset, "no decodable audio track");
        if (!aac::available()) return refuse(av_error_encoder_not_found, "no AAC encoder on this machine");
        return true;
    case Kind::video: {
        if (!source.video) return refuse(av_error_operation_not_supported_for_asset, "the asset has no video track");
        if (source.composition) return refuse(av_error_operation_not_supported_for_asset, "composition video is not read under Alchemy");
        if (!source.video_decodable) return refuse(av_error_decoder_not_found, "no decoder for the asset's video");
        const auto wanted = preset.codec == four("avc1") ? video_encoding::Codec::h264 : video_encoding::Codec::hevc;
        const auto encoders = video_encoding::encoders();
        if (std::none_of(encoders.begin(), encoders.end(), [&](const auto& e) { return e.codec == wanted; }))
            return refuse(av_error_encoder_not_found, "no encoder for the preset's codec on this machine");
        if (!source.audio.empty() && !aac::available()) return refuse(av_error_encoder_not_found, "no AAC encoder on this machine");
        return true;
    }
    }
    return false;
}

std::vector<std::string> Exports::file_types(const Preset& preset) const {
    if (preset.kind == Kind::m4a) return {type_m4a};
    return {type_mov, type_mp4, type_m4v};
}

// ---- Passthrough --------------------------------------------------------------------------------------------

bool Exports::passthrough(ExportState& e, std::string& why, std::int64_t& code) {
    code = av_error_export_failed;
    const auto url = support_.send_id(e.asset, "URL");
    std::uint64_t size = 0;
    std::int32_t status = 0;
    const auto in = open_guest_read(support_, support_.cf.url_path(url), size, status);
    if (in < 0) { why = "the asset's file does not open"; return false; }
    const mc::ReadAt read = [this, in](std::uint64_t offset, std::span<std::uint8_t> out) { return read_guest(support_, in, offset, out); };
    mc::Movie movie;
    if (!mc::parse_iso(read, size, movie, why)) { close_guest_fd(support_, in); code = av_error_operation_not_supported_for_asset; return false; }
    // The output, created new.
    const auto path = support_.cf.url_path(e.output_url);
    const auto name = support_.process.allocate_guest(path.size() + 1, 1);
    std::vector<std::uint8_t> bytes(path.begin(), path.end());
    bytes.push_back(0);
    support_.memory().write_bytes(name, bytes);
    CallRegs r{};
    r.x[0] = name;
    r.x[1] = o_rdwr | o_creat | o_excl;
    r.push_variadic(std::uint64_t{0644});
    const auto out = static_cast<std::int64_t>(support_.call(libsystem, "_open", r).x0);
    support_.process.release_guest(name);
    if (out < 0) { close_guest_fd(support_, in); why = "the output file cannot be created"; return false; }
    auto& m = support_.memory();
    const auto write = [&](const mf::Writes& writes) {
        for (const auto& [offset, data] : writes) {
            for (std::size_t done = 0; done < data.size();) {
                const auto n = std::min<std::size_t>(data.size() - done, 1 << 20);
                const auto buffer = support_.process.allocate_guest(n, 16);
                m.write_bytes(buffer, std::span(data).subspan(done, n));
                const auto wrote = static_cast<std::int64_t>(support_.call(libsystem, "_pwrite", {static_cast<Address>(out), buffer, n, offset + done}));
                support_.process.release_guest(buffer);
                if (wrote <= 0) return false;
                done += static_cast<std::size_t>(wrote);
            }
        }
        return true;
    };
    const auto type = support_.cf.utf8(e.output_type);
    mf::Writer writer(type == type_mov ? mf::Brand::quicktime : type == type_m4v ? mf::Brand::m4v : mf::Brand::mp4);
    bool ok = write({{0, writer.header()}});
    // The time range in seconds (the asset's whole length by default).
    const double range_start = is_numeric(e.range.start) ? static_cast<double>(e.range.start.value) / e.range.start.timescale : 0;
    const double range_length = is_numeric(e.range.duration) ? static_cast<double>(e.range.duration.value) / e.range.duration.timescale
                                                              : std::numeric_limits<double>::infinity();
    struct Pick { std::size_t source, track, first, last; std::int64_t shift; };   // the movie's track, the output's
    std::vector<Pick> picks;
    std::uint64_t total = 0;
    for (std::size_t i = 0; ok && i < movie.tracks.size(); ++i) {
        const auto& t = movie.tracks[i];
        if (t.kind == mc::Track::Kind::other || t.samples.empty()) continue;
        const bool video = t.kind == mc::Track::Kind::video;
        mf::TrackFormat format;
        format.video = video;
        format.codec = t.codec;
        format.timescale = t.timescale;
        format.width = t.width;
        format.height = t.height;
        format.sample_rate = t.sample_rate;
        format.channels = t.channels;
        format.bits = t.bits ? t.bits : 16;
        const auto track = writer.add_track(format);
        // The configuration as the muxer takes it: an esds payload without its version and flags.
        auto config = t.config;
        if (t.codec == four("mp4a") && config.size() > 4 && config[4] == 0x03) config.erase(config.begin(), config.begin() + 4);
        writer.set_config(track, std::move(config));
        // The samples covering the range, in media time: the presentation starts at media time `start`.
        const auto media = [&](double seconds) { return static_cast<std::int64_t>(std::llround(seconds * t.timescale)) + t.start; };
        const std::int64_t from = media(range_start), to = std::isinf(range_length) ? std::numeric_limits<std::int64_t>::max() : media(range_start + range_length);
        std::size_t first = 0, last = t.samples.size();
        if (video) {
            for (std::size_t k = 0; k < t.samples.size(); ++k)
                if (t.samples[k].keyframe && t.samples[k].pts <= from) first = k;
            last = first;
            for (std::size_t k = first; k < t.samples.size(); ++k)
                if (t.samples[k].pts < to) last = k + 1;
        } else {
            // The packets from the source's own priming before the range: the decoder's pre-roll, skipped by the edit list.
            const std::int64_t keep = std::max<std::int64_t>(0, from - t.start);
            while (first < t.samples.size() && t.samples[first].pts + t.samples[first].duration <= keep) ++first;
            last = first;
            while (last < t.samples.size() && t.samples[last].pts < to) ++last;
            if (first < last) {
                const auto priming = static_cast<std::uint64_t>(std::max<std::int64_t>(0, from - t.samples[first].pts));
                // The range's end, the last packet's, or the source's own presentation's (its remainder is padding).
                std::int64_t end = std::min<std::int64_t>(to, t.samples[last - 1].pts + t.samples[last - 1].duration);
                if (t.presented) end = std::min<std::int64_t>(end, t.start + t.presented);
                const std::uint64_t whole = t.presented ? static_cast<std::uint64_t>(t.presented) : 0;
                std::uint64_t presented = static_cast<std::uint64_t>(std::max<std::int64_t>(0, end - from));
                if (whole && range_start == 0 && std::isinf(range_length)) presented = whole;
                writer.set_trim(track, priming, presented);
            }
        }
        if (first >= last) continue;
        // Video: the frames' times from the range's start (those before it negative, cut by the edit list); audio: from
        // its first packet (the edit list skips to the start).
        picks.push_back({i, track, first, last, video ? from : t.samples[first].pts});
        total += last - first;
    }
    // The samples, interleaved by the muxer as they come; tracks are taken in turns of their decode times.
    std::vector<std::size_t> next(picks.size());
    for (std::size_t p = 0; p < picks.size(); ++p) next[p] = picks[p].first;
    std::uint64_t done = 0;
    while (ok && !e.cancelled) {
        std::optional<std::size_t> pick;
        long double earliest = std::numeric_limits<long double>::infinity();
        for (std::size_t p = 0; p < picks.size(); ++p) {
            if (next[p] >= picks[p].last) continue;
            const auto& track = movie.tracks[picks[p].source];
            const long double when = static_cast<long double>(track.samples[next[p]].dts) / track.timescale;
            if (when < earliest) { earliest = when; pick = p; }
        }
        if (!pick) break;
        const auto p = *pick;
        const auto& track = movie.tracks[picks[p].source];
        const auto& sample = track.samples[next[p]++];
        auto data = mc::read_sample(read, track, sample);
        if (data.size() != sample.size) { why = "the asset's file is shorter than its sample table"; ok = false; break; }
        ok = write(writer.add_sample(picks[p].track, std::move(data), sample.dts - picks[p].shift, sample.pts - picks[p].shift, sample.duration, sample.keyframe));
        ++done;
        std::lock_guard lock(e.mutex);
        e.progress = total ? static_cast<float>(done) / static_cast<float>(total) * 0.99f : 0;
    }
    close_guest_fd(support_, in);
    if (ok && !e.cancelled) {
        const auto finish = writer.finish(e.optimize);
        if (finish.move_by) {
            // The data moves up by the moov's size, the last chunk first.
            const auto end = static_cast<std::int64_t>(support_.call(libsystem, "_lseek", {static_cast<Address>(out), 0, 2}));
            const auto buffer = support_.process.allocate_guest(1 << 20, 16);
            for (std::uint64_t stop = end > 0 ? static_cast<std::uint64_t>(end) : 0; ok && stop > finish.move_from;) {
                const auto start = stop - finish.move_from > (1u << 20) ? stop - (1u << 20) : finish.move_from;
                const auto n = stop - start;
                ok = static_cast<std::int64_t>(support_.call(libsystem, "_pread", {static_cast<Address>(out), buffer, n, start})) == static_cast<std::int64_t>(n) &&
                     static_cast<std::int64_t>(support_.call(libsystem, "_pwrite", {static_cast<Address>(out), buffer, n, start + finish.move_by})) ==
                         static_cast<std::int64_t>(n);
                stop = start;
            }
            support_.process.release_guest(buffer);
        }
        ok = ok && write(finish.writes);
        if (!ok && why.empty()) why = "the output file could not be written";
    }
    static_cast<void>(support_.call(libsystem, "_close", {static_cast<Address>(out)}));
    return ok;
}

// ---- Re-encoding --------------------------------------------------------------------------------------------

bool Exports::transcode(ExportState& e, const Source& source, std::string& why, std::int64_t& code) {
    code = av_error_export_failed;
    auto* sp = &support_;
    auto& m = support_.memory();
    const bool video = e.kind->kind == Kind::video;
    const auto error_at = support_.process.allocate_guest(8, 8);
    m.write64(error_at, 0);
    std::vector<Address> owned;   // +1 objects released at the end
    const auto own = [&](Address object) { if (object) owned.push_back(object); return object; };
    const auto number = [&](std::int64_t value) { return own(bridge_->number(value)); };
    const auto finish = [&](bool ok) {
        bridge_->release_all(owned);
        support_.process.release_guest(error_at);
        return ok;
    };
    // The reader: decoded pictures in 420v, and the audio mix in float.
    const auto reader = own(sp->send_id(sp->send_id(sp->objc.find_class("AVAssetReader"), "alloc"), "initWithAsset:error:", {e.asset, error_at}));
    if (!reader) { why = "the asset cannot be read"; return finish(false); }
    if (is_numeric(e.range.start)) {
        const auto range = support_.process.allocate_guest(48, 8);
        const CMTime parts[2] = {e.range.start, e.range.duration};
        for (int i = 0; i < 2; ++i) {
            m.write64(range + 24 * i, static_cast<std::uint64_t>(parts[i].value));
            m.write32(range + 24 * i + 8, static_cast<std::uint32_t>(parts[i].timescale));
            m.write32(range + 24 * i + 12, parts[i].flags);
            m.write64(range + 24 * i + 16, static_cast<std::uint64_t>(parts[i].epoch));
        }
        CallRegs r{};
        r.x[2] = range;
        static_cast<void>(sp->send(reader, "setTimeRange:", r));
        support_.process.release_guest(range);
    }
    Address video_output{}, audio_output{};
    if (video && source.video) {
        const auto settings = own(bridge_->dictionary({{own(text("PixelFormatType")), number(four("420v"))}}));
        video_output = sp->send_id(sp->objc.find_class("AVAssetReaderTrackOutput"), "assetReaderTrackOutputWithTrack:outputSettings:", {source.video, settings});
        sp->send(reader, "addOutput:", [&] { CallRegs r{}; r.x[2] = video_output; return r; }());
    }
    if (!source.audio.empty()) {
        const auto tracks = own(bridge_->array(source.composition ? source.audio : std::vector<Address>{source.audio.front()}));
        audio_output = sp->send_id(sp->objc.find_class("AVAssetReaderAudioMixOutput"), "assetReaderAudioMixOutputWithAudioTracks:audioSettings:", {tracks, 0});
        if (e.audio_mix) sp->send(audio_output, "setAudioMix:", [&] { CallRegs r{}; r.x[2] = e.audio_mix; return r; }());
        sp->send(reader, "addOutput:", [&] { CallRegs r{}; r.x[2] = audio_output; return r; }());
    }
    if (!video_output && !audio_output) { why = "nothing to export"; code = av_error_operation_not_supported_for_asset; return finish(false); }
    // The audio's format: its rate where the AAC encoder takes it, at most two channels.
    std::uint32_t rate = 48000, channels = 2;
    if (audio_output) {
        const auto descriptions = sp->send_id(source.audio.front(), "formatDescriptions");
        if (descriptions && sp->call(core_foundation, "_CFArrayGetCount", {descriptions}) > 0) {
            const auto asbd = sp->call(core_media, "_CMAudioFormatDescriptionGetStreamBasicDescription", {sp->call(core_foundation, "_CFArrayGetValueAtIndex", {descriptions, 0})});
            if (asbd) {
                const auto source_rate = std::bit_cast<double>(m.read64(asbd));
                const auto source_channels = m.read32(asbd + 28);
                const auto caps = aac::capabilities();
                if (std::find(caps.sample_rates.begin(), caps.sample_rates.end(), static_cast<std::uint32_t>(source_rate)) != caps.sample_rates.end())
                    rate = static_cast<std::uint32_t>(source_rate);
                channels = std::clamp<std::uint32_t>(source_channels, 1, 2);
            }
        }
    }
    // The writer.
    const auto writer = own(sp->send_id(sp->send_id(sp->objc.find_class("AVAssetWriter"), "alloc"), "initWithURL:fileType:error:", {e.output_url, e.output_type, error_at}));
    if (!writer) { why = "the output file type cannot be written"; return finish(false); }
    Address video_input{}, audio_input{};
    if (video_output) {
        // The preset's box, never larger than the source; even sizes.
        double width = source.width, height = source.height;
        if (e.kind->long_side) {
            const bool landscape = width >= height;
            const double box_w = landscape ? e.kind->long_side : e.kind->short_side, box_h = landscape ? e.kind->short_side : e.kind->long_side;
            const double scale = std::min({1.0, box_w / width, box_h / height});
            width *= scale;
            height *= scale;
        }
        const auto even = [](double v) { return std::max<std::int64_t>(2, static_cast<std::int64_t>(std::lround(v / 2)) * 2); };
        const auto settings = own(bridge_->dictionary({{own(text("AVVideoCodecKey")), own(text(e.kind->codec == four("avc1") ? "avc1" : "hvc1"))},
                                                       {own(text("AVVideoWidthKey")), number(even(width))},
                                                       {own(text("AVVideoHeightKey")), number(even(height))}}));
        video_input = sp->send_id(sp->objc.find_class("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {own(text("vide")), settings});
        // The source track's transform.
        const auto transform = support_.process.allocate_guest(48, 8);
        CallRegs r{};
        r.indirect_result = transform;
        static_cast<void>(sp->send(source.video, "preferredTransform", r));
        sp->send(video_input, "setTransform:", [&] { CallRegs t{}; t.x[2] = transform; return t; }());
        support_.process.release_guest(transform);
        sp->send(writer, "addInput:", [&] { CallRegs a{}; a.x[2] = video_input; return a; }());
    }
    if (audio_output) {
        const auto settings = own(bridge_->dictionary({{own(text("AVFormatIDKey")), number(four("aac "))},
                                                       {own(text("AVSampleRateKey")), number(rate)},
                                                       {own(text("AVNumberOfChannelsKey")), number(channels)},
                                                       {own(text("AVEncoderBitRateKey")), number(channels == 1 ? 64000 : 128000)}}));
        audio_input = sp->send_id(sp->objc.find_class("AVAssetWriterInput"), "assetWriterInputWithMediaType:outputSettings:", {own(text("soun")), settings});
        if (!(sp->send(writer, "canAddInput:", [&] { CallRegs a{}; a.x[2] = audio_input; return a; }()).x0 & 0xff)) {
            why = "no AAC encoder takes the audio";
            code = av_error_encoder_not_found;
            return finish(false);
        }
        sp->send(writer, "addInput:", [&] { CallRegs a{}; a.x[2] = audio_input; return a; }());
    }
    sp->send(writer, "setShouldOptimizeForNetworkUse:", [&] { CallRegs a{}; a.x[2] = e.optimize ? 1 : 0; return a; }());
    if (!(sp->send(reader, "startReading").x0 & 0xff)) { why = "the asset cannot be read"; code = av_error_decoder_not_found; return finish(false); }
    if (!(sp->send(writer, "startWriting").x0 & 0xff)) {
        why = "the output cannot be written";
        const auto error = sp->send_id(writer, "error");
        if (error && sp->send(error, "code").x0 == static_cast<std::uint64_t>(av_error_encoder_not_found)) code = av_error_encoder_not_found;
        sp->send(reader, "cancelReading");
        return finish(false);
    }
    const auto start = is_numeric(e.range.start) ? e.range.start : time_zero;
    {
        const auto at = support_.process.allocate_guest(24, 8);
        m.write64(at, static_cast<std::uint64_t>(start.value));
        m.write32(at + 8, static_cast<std::uint32_t>(start.timescale));
        m.write32(at + 12, start.flags);
        m.write64(at + 16, 0);
        sp->send(writer, "startSessionAtSourceTime:", [&] { CallRegs a{}; a.x[2] = at; return a; }());
        support_.process.release_guest(at);
    }
    // The outputs in turns of their times, each buffer appended to its input.
    const double length = is_numeric(e.range.duration) ? static_cast<double>(e.range.duration.value) / e.range.duration.timescale
                          : is_numeric(source.duration) ? static_cast<double>(source.duration.value) / source.duration.timescale : 0;
    const double origin = static_cast<double>(start.value) / start.timescale;
    struct Lane { Address output, input; double at{}; bool done{}; };
    std::vector<Lane> lanes;
    if (video_output) lanes.push_back({video_output, video_input});
    if (audio_output) lanes.push_back({audio_output, audio_input});
    bool ok = true;
    while (ok && !e.cancelled) {
        Lane* lane = nullptr;
        for (auto& l : lanes)
            if (!l.done && (!lane || l.at < lane->at)) lane = &l;
        if (!lane) break;
        const auto sample = sp->send_id(lane->output, "copyNextSampleBuffer");
        if (!sample) {
            lane->done = true;
            sp->send(lane->input, "markAsFinished");
            continue;
        }
        const auto pts = sample_time(sample, "_CMSampleBufferGetPresentationTimeStamp");
        if (is_numeric(pts)) lane->at = static_cast<double>(pts.value) / pts.timescale;
        ok = (sp->send(lane->input, "appendSampleBuffer:", [&] { CallRegs a{}; a.x[2] = sample; return a; }()).x0 & 0xff) != 0;
        static_cast<void>(sp->call(core_foundation, "_CFRelease", {sample}));
        std::lock_guard lock(e.mutex);
        double low = std::numeric_limits<double>::infinity();
        for (const auto& l : lanes) low = std::min(low, l.done ? std::numeric_limits<double>::infinity() : l.at);
        if (length > 0 && std::isfinite(low)) e.progress = static_cast<float>(std::clamp((low - origin) / length, 0.0, 0.99));
    }
    if (e.cancelled) {
        sp->send(reader, "cancelReading");
        sp->send(writer, "cancelWriting");
        return finish(false);
    }
    if (!ok) {
        why = "the writer refused a sample";
        if (const auto error = sp->send_id(writer, "error")) {
            if (const auto description = sp->send_id(error, "localizedDescription")) why = sp->cf.utf8(description);
            if (sp->send(error, "code").x0 == static_cast<std::uint64_t>(av_error_encoder_not_found)) code = av_error_encoder_not_found;
        }
        sp->send(reader, "cancelReading");
        sp->send(writer, "cancelWriting");
        return finish(false);
    }
    if (sp->send(reader, "status").x0 == 3) {   // AVAssetReaderStatusFailed
        why = "the asset could not be decoded";
        code = av_error_decoder_not_found;
        sp->send(writer, "cancelWriting");
        return finish(false);
    }
    if (!(sp->send(writer, "finishWriting").x0 & 0xff)) {
        why = "the output could not be completed";
        return finish(false);
    }
    return finish(true);
}

void Exports::run(Address session) {
    const auto e = support_.objc.find_host_state<ExportState>(session);
    if (!e) return;
    std::string why;
    std::int64_t code = av_error_export_failed;
    bool ok = false;
    if (!e->cancelled) {
        if (!compatible(*e->kind, e->asset, &code, &why)) ok = false;
        else if (e->kind->kind == Kind::passthrough) ok = passthrough(*e, why, code);
        else ok = transcode(*e, inspect(e->asset), why, code);
    }
    std::lock_guard lock(e->mutex);
    if (e->cancelled) {
        e->status = export_cancelled;
        // A cancelled export leaves no file.
        const auto manager = support_.send_id(support_.objc.find_class("NSFileManager"), "defaultManager");
        if (manager && e->output_url) static_cast<void>(support_.send_id(manager, "removeItemAtURL:error:", {e->output_url, 0}));
    } else if (ok) {
        e->status = export_completed;
        e->progress = 1;
    } else {
        e->status = export_failed;
        if (!e->error) e->error = bridge_->error("AVFoundationErrorDomain", code, "AVAssetExportSession: " + why);
    }
}

void Exports::install() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    class_ = support.declare_class("AVAssetExportSession", "NSObject", av_foundation);
    for (const auto& p : presets) support.string_constant(av_foundation, std::string("_") + p.name, p.name);
    const auto session_of = [sp](Address object) {
        const auto e = sp->objc.find_host_state<ExportState>(object);
        if (!e) unsupported("an AVAssetExportSession Alchemy did not make");
        return e;
    };
    const auto strings = [self, sp](const std::vector<std::string>& values) {
        std::vector<Address> objects;
        for (const auto& v : values) objects.push_back(sp->cf.string(v));
        const auto array = self->bridge_->array(objects);
        for (const auto o : objects) sp->release(o);
        return sp->send_id(array, "autorelease");
    };
    const auto write_time = [](GuestMemory& m, Address at, const CMTime& t) {
        m.write64(at, static_cast<std::uint64_t>(t.value));
        m.write32(at + 8, static_cast<std::uint32_t>(t.timescale));
        m.write32(at + 12, t.flags);
        m.write64(at + 16, static_cast<std::uint64_t>(t.epoch));
    };
    const auto read_time = [](GuestMemory& m, Address at) {
        return CMTime{static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12), static_cast<std::int64_t>(m.read64(at + 16))};
    };

    support.method(class_, "allExportPresets", [strings](CpuState& s, GuestMemory&) {
        std::vector<std::string> names;
        for (const auto& p : presets) names.emplace_back(p.name);
        s.x[0] = strings(names);
    }, "@@:", true);
    support.method(class_, "exportPresetsCompatibleWithAsset:", [self, strings](CpuState& s, GuestMemory&) {
        std::vector<std::string> names;
        if (s.x[2])
            for (const auto& p : presets)
                if (self->compatible(p, s.x[2])) names.emplace_back(p.name);
        s.x[0] = strings(names);
    }, "@@:@", true);
    // + (void)determineCompatibilityOfExportPreset:withAsset:outputFileType:completionHandler: — block(BOOL), on a
    // thread of the session class.
    support.method(class_, "determineCompatibilityOfExportPreset:withAsset:outputFileType:completionHandler:", [self, sp](CpuState& s, GuestMemory&) {
        const auto* preset = s.x[2] ? preset_named(sp->cf.utf8(s.x[2])) : nullptr;
        bool ok = preset && s.x[3] && self->compatible(*preset, s.x[3]);
        if (ok && s.x[4]) {
            const auto types = self->file_types(*preset);
            ok = std::find(types.begin(), types.end(), sp->cf.utf8(s.x[4])) != types.end();
        }
        if (s.x[5]) self->bridge_->dispatch_async(self->bridge_->main_queue(), [b = self->bridge_, block = self->bridge_->block_copy(s.x[5]), ok] {
            static_cast<void>(b->invoke_block(block, {ok ? 1u : 0u}));
            b->block_release(block);
        });
    }, "v@:@@@@?", true);
    support.method(class_, "initWithAsset:presetName:", [sp](CpuState& s, GuestMemory&) {
        const auto* preset = s.x[3] ? preset_named(sp->cf.utf8(s.x[3])) : nullptr;
        if (!s.x[2] || !preset) {   // an unknown preset: nil
            sp->release(s.x[0]);
            s.x[0] = 0;
            return;
        }
        const auto e = sp->objc.host_state<ExportState>(s.x[0]);
        e->asset = sp->retain(s.x[2]);
        e->preset = sp->cf.copy(s.x[3]);
        e->kind = preset;
    }, "@@:@@");
    support.method(class_, "exportSessionWithAsset:presetName:", [sp](CpuState& s, GuestMemory&) {
        const auto made = sp->send_id(sp->send_id(s.x[0], "alloc"), "initWithAsset:presetName:", {s.x[2], s.x[3]});
        s.x[0] = made ? sp->send_id(made, "autorelease") : 0;
    }, "@@:@@", true);
    support.method(class_, "presetName", [session_of](CpuState& s, GuestMemory&) { s.x[0] = session_of(s.x[0])->preset; }, "@@:");
    support.method(class_, "asset", [session_of](CpuState& s, GuestMemory&) { s.x[0] = session_of(s.x[0])->asset; }, "@@:");
    support.method(class_, "outputURL", [session_of](CpuState& s, GuestMemory&) { s.x[0] = session_of(s.x[0])->output_url; }, "@@:");
    support.method(class_, "setOutputURL:", [sp, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        sp->release(std::exchange(e->output_url, sp->retain(s.x[2])));
    }, "v@:@");
    support.method(class_, "outputFileType", [session_of](CpuState& s, GuestMemory&) { s.x[0] = session_of(s.x[0])->output_type; }, "@@:");
    support.method(class_, "setOutputFileType:", [sp, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        sp->release(std::exchange(e->output_type, s.x[2] ? sp->cf.copy(s.x[2]) : 0));
    }, "v@:@");
    support.method(class_, "supportedFileTypes", [self, strings, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        s.x[0] = strings(self->compatible(*e->kind, e->asset) ? self->file_types(*e->kind) : std::vector<std::string>{});
    }, "@@:");
    // - (void)determineCompatibleFileTypesWithCompletionHandler: — block(NSArray *).
    support.method(class_, "determineCompatibleFileTypesWithCompletionHandler:", [self, sp, strings, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        const auto types = sp->retain(strings(self->compatible(*e->kind, e->asset) ? self->file_types(*e->kind) : std::vector<std::string>{}));
        if (!s.x[2]) { sp->release(types); return; }
        self->bridge_->dispatch_async(self->bridge_->main_queue(), [sp, b = self->bridge_, block = self->bridge_->block_copy(s.x[2]), types] {
            static_cast<void>(b->invoke_block(block, {types}));
            b->block_release(block);
            sp->release(types);
        });
    }, "v@:@?");
    support.method(class_, "status", [session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        std::lock_guard lock(e->mutex);
        s.x[0] = static_cast<std::uint64_t>(e->status);
    }, "q@:");
    support.method(class_, "error", [session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        std::lock_guard lock(e->mutex);
        s.x[0] = e->error;
    }, "@@:");
    support.method(class_, "progress", [session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        std::lock_guard lock(e->mutex);
        s.v[0] = {std::bit_cast<std::uint32_t>(e->progress), 0};
    }, "f@:");
    support.method(class_, "shouldOptimizeForNetworkUse", [session_of](CpuState& s, GuestMemory&) { s.x[0] = session_of(s.x[0])->optimize ? 1 : 0; }, "B@:");
    support.method(class_, "setShouldOptimizeForNetworkUse:", [session_of](CpuState& s, GuestMemory&) { session_of(s.x[0])->optimize = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(class_, "fileLengthLimit", [session_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(session_of(s.x[0])->file_length_limit); }, "q@:");
    support.method(class_, "setFileLengthLimit:", [session_of](CpuState& s, GuestMemory&) { session_of(s.x[0])->file_length_limit = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    // CMTimeRange (48 bytes: by reference, and returned through x8).
    support.method(class_, "timeRange", [session_of, write_time](CpuState& s, GuestMemory& m) {
        const auto e = session_of(s.x[0]);
        write_time(m, s.x[8], e->range.start);
        write_time(m, s.x[8] + 24, e->range.duration);
    }, "{?={?=qiIq}{?=qiIq}}@:");
    support.method(class_, "setTimeRange:", [session_of, read_time](CpuState& s, GuestMemory& m) {
        const auto e = session_of(s.x[0]);
        e->range = {read_time(m, s.x[2]), read_time(m, s.x[2] + 24)};
    }, "v@:{?={?=qiIq}{?=qiIq}}");
    support.method(class_, "audioMix", [session_of](CpuState& s, GuestMemory&) { s.x[0] = session_of(s.x[0])->audio_mix; }, "@@:");
    support.method(class_, "setAudioMix:", [sp, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        sp->release(std::exchange(e->audio_mix, sp->retain(s.x[2])));
    }, "v@:@");
    support.method(class_, "videoComposition", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(class_, "setVideoComposition:", [](CpuState& s, GuestMemory&) {
        if (s.x[2]) unsupported("-[AVAssetExportSession setVideoComposition:] (video compositions are not rendered under Alchemy)");
    }, "v@:@");
    // Kept, not written.
    support.method(class_, "metadata", [sp, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        s.x[0] = e->metadata ? e->metadata : sp->send_id(sp->objc.find_class("NSArray"), "array");
    }, "@@:");
    support.method(class_, "setMetadata:", [sp, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        sp->release(std::exchange(e->metadata, s.x[2] ? sp->cf.copy(s.x[2]) : 0));
    }, "v@:@");
    support.method(class_, "audioTimePitchAlgorithm", [session_of](CpuState& s, GuestMemory&) { s.x[0] = session_of(s.x[0])->pitch; }, "@@:");
    support.method(class_, "setAudioTimePitchAlgorithm:", [sp, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        sp->release(std::exchange(e->pitch, s.x[2] ? sp->cf.copy(s.x[2]) : 0));
    }, "v@:@");
    support.method(class_, "estimatedOutputFileLength", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "q@:");
    // Runs the export on the session's thread, then the handler there.
    support.method(class_, "exportAsynchronouslyWithCompletionHandler:", [self, sp, session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        if (!e->output_url) throw std::runtime_error("Alchemy: -[AVAssetExportSession exportAsynchronouslyWithCompletionHandler:] needs an outputURL (NSInvalidArgumentException)");
        if (!e->output_type) throw std::runtime_error("Alchemy: -[AVAssetExportSession exportAsynchronouslyWithCompletionHandler:] needs an outputFileType (NSInvalidArgumentException)");
        const auto types = self->file_types(*e->kind);
        if (std::find(types.begin(), types.end(), sp->cf.utf8(e->output_type)) == types.end())
            throw std::runtime_error("Alchemy: -[AVAssetExportSession exportAsynchronouslyWithCompletionHandler:] the output file type is not one the preset supports "
                                     "(NSInvalidArgumentException)");
        const auto session = sp->retain(s.x[0]);
        const auto handler = s.x[2] ? self->bridge_->block_copy(s.x[2]) : 0;
        {
            std::lock_guard lock(e->mutex);
            if (e->status != export_unknown) throw std::runtime_error("Alchemy: -[AVAssetExportSession exportAsynchronously...] called twice (NSInternalInconsistencyException)");
            e->status = export_waiting;
            // An existing file is not replaced.
            const auto manager = sp->send_id(sp->objc.find_class("NSFileManager"), "defaultManager");
            const auto path = sp->send_id(e->output_url, "path");
            if (manager && path && (sp->send(manager, "fileExistsAtPath:", [&] { CallRegs r{}; r.x[2] = path; return r; }()).x0 & 0xff)) {
                e->status = export_failed;
                e->error = self->bridge_->error("AVFoundationErrorDomain", av_error_file_already_exists, "AVAssetExportSession: the output file already exists");
            }
            if (!e->thread) e->thread = start_completion_thread(sp->process, "com.apple.avfoundation.exportsession");
        }
        e->thread->post([self, sp, e, session, handler] {
            {
                std::lock_guard lock(e->mutex);
                if (e->status == export_waiting) e->status = e->cancelled ? export_cancelled : export_exporting;
            }
            bool exporting = false;
            {
                std::lock_guard lock(e->mutex);
                exporting = e->status == export_exporting;
            }
            // An explicit stop inside the pipeline (something unsupported) fails the export with its message; the
            // handler still runs.
            if (exporting) {
                try {
                    self->run(session);
                } catch (const std::exception& error) {
                    std::lock_guard lock(e->mutex);
                    e->status = export_failed;
                    if (!e->error) e->error = self->bridge_->error("AVFoundationErrorDomain", av_error_export_failed, std::string("AVAssetExportSession: ") + error.what());
                }
            }
            if (handler) {
                static_cast<void>(self->bridge_->invoke_block(handler, {}));
                self->bridge_->block_release(handler);
            }
            sp->release(session);
        });
    }, "v@:@?");
    support.method(class_, "cancelExport", [session_of](CpuState& s, GuestMemory&) {
        const auto e = session_of(s.x[0]);
        e->cancelled = true;
        std::lock_guard lock(e->mutex);
        if (e->status == export_unknown) e->status = export_cancelled;
    }, "v@:");
    support.method(class_, "dealloc", [self, sp](CpuState& s, GuestMemory&) {
        if (const auto e = sp->objc.find_host_state<ExportState>(s.x[0])) {
            for (auto* object : {&e->asset, &e->preset, &e->output_url, &e->output_type, &e->error, &e->audio_mix, &e->metadata, &e->pitch})
                sp->release(std::exchange(*object, 0));
            if (e->thread) e->thread->shutdown();
        }
        static_cast<void>(sp->objc.send_super(s.x[0], self->class_, sp->objc.selector("dealloc")));
    }, "v@:");
}

} // namespace

void install_av_asset_export(Support& support) { std::make_shared<Exports>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
