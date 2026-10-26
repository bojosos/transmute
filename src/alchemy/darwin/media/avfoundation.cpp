#include "alchemy/darwin/media/audio_file.hpp"

#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

// AVFoundation assets, asset readers, audio formats and audio-unit classes,
// and CoreAudioKit's AUGenericView (QLab gap group G5). The capture classes
// are src/alchemy/darwin/media/video.cpp's.
//
// - AVURLAsset/AVAsset: file URLs whose audio Alchemy can read (linear PCM
//   WAVE, AIFF/AIFC, CAF: audio_file_core.hpp; MP3, FLAC, Ogg Opus and MP4
//   or Matroska audio in those codecs, decoded: audio_file.hpp) have one
//   audio track with its duration, time range and CMAudioFormatDescription
//   (of the decoded audio); other media (video, AAC, ALAC) has no track, an
//   invalid duration and AVKeyValueStatusFailed. Nothing is playable (there is no AVPlayer) and no
//   metadata is read (commonMetadata is empty). Keys load synchronously; the
//   completion handler runs on a global queue.
// - AVAssetReader with track and audio-mix outputs vends CMSampleBuffers of
//   linear PCM in the requested settings (same sample rate; see
//   audio_file_core.hpp for the conversions), honouring the time range.
// - AVAssetImageGenerator: no video decoder exists, so every request fails
//   with AVErrorDecoderNotFound (AVErrorOperationNotSupportedForAsset for an
//   audio-only asset).
// - AVAudioFormat and AVAudioChannelLayout are real value classes.
// - Audio units: the version 3 API exposes no component (the output units
//   are version 2 AudioComponents only, audio_units.cpp), so
//   AVAudioUnitComponentManager lists nothing, AVAudioUnit/AUAudioUnit
//   instantiation fails with invalidComponentID and their instance methods
//   (and AUGenericView's, which needs a unit) are unreachable: they stop
//   with an explicit message. AUAudioUnitBus is a real value class.
namespace alchemy::gui::completion_kit::media {
namespace {

constexpr std::string_view av_foundation = "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation";
constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view core_audio_kit = "/System/Library/Frameworks/CoreAudioKit.framework/Versions/A/CoreAudioKit";
constexpr std::string_view core_media = "/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// AVError codes (AVFoundationErrorDomain).
constexpr std::int64_t av_error_unknown = -11800, av_error_file_format_not_recognized = -11828, av_error_decoder_not_found = -11833,
    av_error_operation_not_supported_for_asset = -11838;
constexpr std::int32_t invalid_component_id = -3000;

[[noreturn]] void cannot_exist(const std::string& what) {
    unsupported(what + " (no instance can exist under Alchemy: no audio component, unit or capture device exists)");
}

struct Time { std::int64_t value{}; std::int32_t timescale{}; std::uint32_t flags{}; std::int64_t epoch{}; };
Time read_time(GuestMemory& m, Address at) {
    return {static_cast<std::int64_t>(m.read64(at)), static_cast<std::int32_t>(m.read32(at + 8)), m.read32(at + 12), static_cast<std::int64_t>(m.read64(at + 16))};
}
void write_time(GuestMemory& m, Address at, const Time& t) {
    m.write64(at, static_cast<std::uint64_t>(t.value));
    m.write32(at + 8, static_cast<std::uint32_t>(t.timescale));
    m.write32(at + 12, t.flags);
    m.write64(at + 16, static_cast<std::uint64_t>(t.epoch));
}
constexpr Time time_invalid{};
constexpr Time time_zero{0, 1, 1, 0};
constexpr Time time_positive_infinity{0, 0, 1 | 4, 0};

struct AssetState final : ObjectPayload {
    Address url{};                     // retained
    bool loaded{};
    std::int32_t status{};             // the open/parse OSStatus
    audio_file::Container container;
    Address track{};                   // retained AVAssetTrack (readable audio)
    [[nodiscard]] bool readable() const { return loaded && status == 0; }
    [[nodiscard]] std::uint64_t frames() const { return container.data_bytes / container.format.frame_bytes(); }
};
struct TrackState final : ObjectPayload {
    Address asset{};                   // not retained (the asset owns the track)
    Address format_description{};      // retained CMAudioFormatDescription
};
struct ReaderState final : ObjectPayload {
    Address asset{};                   // retained
    std::vector<Address> outputs;      // retained
    std::int64_t status{};             // AVAssetReaderStatus
    Address error{};                   // retained NSError
    Time start{time_zero}, duration{time_positive_infinity};
};
struct OutputState final : ObjectPayload {
    std::vector<Address> tracks;       // retained
    Address settings{};                // retained dictionary or 0
    Address reader{};                  // not retained (the reader owns its outputs)
    bool mix{}, always_copies{true};
    Address audio_mix{};               // retained
    audio_file::Pcm client;
    bool client_set{};
    GuestAudio file;
    std::uint64_t position{}, end{};
    Address format_description{};      // retained, for `client`
    bool finished{};
};
struct GeneratorState final : ObjectPayload {
    Address asset{};
    bool applies_transform{};
    double maximum_width{}, maximum_height{};
    Time before{time_positive_infinity}, after{time_positive_infinity};
};
struct MetadataState final : ObjectPayload { Address key{}, common_key{}, key_space{}, value{}, identifier{}; };
struct LayoutState final : ObjectPayload { std::vector<std::uint8_t> bytes; Address guest{}; };
struct FormatState final : ObjectPayload {
    std::vector<std::uint8_t> asbd;    // 40 bytes
    std::uint64_t common{};            // AVAudioCommonFormat
    Address layout{};                  // retained AVAudioChannelLayout or 0
    Address guest_asbd{};
    Address format_description{};      // retained, made on demand
};
struct BusState final : ObjectPayload { Address format{}, name{}; bool enabled{true}; std::uint32_t maximum_channels{}; bool allocate{true}; };

std::uint32_t le32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    std::uint32_t value{};
    if (at + 4 <= bytes.size()) std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

// The channel count an AudioChannelLayout describes.
std::uint32_t layout_channels(const std::vector<std::uint8_t>& layout) {
    const auto tag = le32(layout, 0);
    if (tag == 0) return le32(layout, 8);                                  // UseChannelDescriptions
    if (tag == 0x10000) return static_cast<std::uint32_t>(std::popcount(le32(layout, 4)));   // UseChannelBitmap
    return tag & 0xffff;
}

std::vector<std::uint8_t> asbd_for(std::uint64_t common, double rate, std::uint32_t channels, bool interleaved) {
    audio_file::Pcm f;
    f.rate = rate;
    f.channels = channels;
    f.non_interleaved = !interleaved;
    switch (common) {
    case 1: f.is_float = true; f.bits = 32; f.container = 4; break;    // AVAudioPCMFormatFloat32
    case 2: f.is_float = true; f.bits = 64; f.container = 8; break;    // Float64
    case 3: f.bits = 16; f.container = 2; break;                       // Int16
    case 4: f.bits = 32; f.container = 4; break;                       // Int32
    default: return {};
    }
    return audio_file::asbd_from_pcm(f);
}

std::uint64_t common_of(const std::vector<std::uint8_t>& asbd) {
    std::string why;
    const auto pcm = audio_file::pcm_from_asbd(asbd, why);
    if (!pcm || pcm->big_endian || pcm->bits != pcm->container * 8) return 0;
    if (pcm->is_float) return pcm->bits == 32 ? 1 : 2;
    if (!pcm->is_signed) return 0;
    return pcm->bits == 16 ? 3 : pcm->bits == 32 ? 4 : 0;
}

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

struct Av {
    Support& support;
    std::shared_ptr<security::Bridge> bridge = security::bridge();
    Address asset_class{}, url_asset_class{}, track_class{}, reader_class{}, output_class{}, track_output_class{}, mix_output_class{},
        generator_class{}, metadata_class{}, layout_class{}, format_class{}, bus_class{};

    explicit Av(Support& owner) : support(owner) {}

    template <class T> std::shared_ptr<T> state(Address self) { return support.objc.host_state<T>(self); }
    Address autorelease(Address object) { return object ? support.send_id(object, "autorelease") : 0; }
    Address retain(Address object) { return object ? support.send_id(object, "retain") : 0; }
    void release(Address object) { if (object) static_cast<void>(support.send_id(object, "release")); }
    Address alloc_init(Address cls) { return support.send_id(support.send_id(cls, "alloc"), "init"); }
    Address empty_array() { return autorelease(bridge->array({})); }
    Address string(std::string_view text) { return support.cf.constant_string(text); }
    void store_error(Address out, std::int64_t code, std::string_view text, std::string_view domain = "AVFoundationErrorDomain") {
        if (out) bridge->store_error(out, bridge->error(domain, code, text), true);
    }
    // A key of a settings dictionary, as a number.
    std::optional<double> setting(Address dictionary, std::string_view key) {
        if (!dictionary) return std::nullopt;
        const auto name = support.cf.string(key);
        const auto value = support.cf.dictionary_value(dictionary, name);
        support.release(name);
        return value ? support.cf.number_value(value) : std::nullopt;
    }

    // Opens and parses the asset's file once; makes its audio track.
    AssetState& load(Address asset) {
        auto s = state<AssetState>(asset);
        if (s->loaded) return *s;
        s->loaded = true;
        const auto scheme = s->url ? support.send_id(s->url, "scheme") : 0;
        if (!s->url || !scheme || support.cf.utf8(scheme) != "file") { s->status = audio_file::error::unsupported_file_type; return *s; }
        GuestAudio audio;
        s->status = open_guest_audio(support, support.cf.url_path(s->url), audio);
        if (s->status) return *s;
        close_guest_audio(support, audio);
        s->container = audio.container;
        // The track and its CMAudioFormatDescription.
        const auto track = alloc_init(track_class);
        auto t = state<TrackState>(track);
        t->asset = asset;
        t->format_description = make_format_description(audio_file::asbd_from_pcm(s->container.format),
                                                        audio_file::layout_for_mask(s->container.channel_mask, s->container.format.channels));
        s->track = track;
        return *s;
    }
    Address make_format_description(const std::vector<std::uint8_t>& asbd, const std::vector<std::uint8_t>& layout) {
        const auto asbd_at = support.process.allocate_guest(40, 8);
        support.memory().write_bytes(asbd_at, asbd);
        Address layout_at{};
        if (!layout.empty()) { layout_at = support.process.allocate_guest(layout.size(), 8); support.memory().write_bytes(layout_at, layout); }
        const auto out = support.process.allocate_guest(8, 8);
        support.memory().write64(out, 0);
        const auto result = support.call(core_media, "_CMAudioFormatDescriptionCreate", {0, asbd_at, layout.size(), layout_at, 0, 0, 0, out});
        const auto description = support.memory().read64(out);
        support.process.release_guest(out);
        support.process.release_guest(asbd_at);
        if (layout_at) support.process.release_guest(layout_at);
        if (static_cast<std::int32_t>(result)) fail("CMAudioFormatDescriptionCreate failed for an asset's format");
        return description;
    }
    Time duration_of(const AssetState& s) {
        if (!s.readable()) return time_invalid;
        const auto rate = s.container.format.rate;
        if (rate == std::floor(rate) && rate > 0 && rate <= 0x7fffffff) return {static_cast<std::int64_t>(s.frames()), static_cast<std::int32_t>(rate), 1, 0};
        return {static_cast<std::int64_t>(std::llround(static_cast<double>(s.frames()) / rate * 1e6)), 1000000, 1 | 2, 0};
    }

    // ---- asset reader --------------------------------------------------------------
    void finish_reader_if_done(Address reader) {
        auto r = state<ReaderState>(reader);
        if (r->status != 1) return;
        for (const auto output : r->outputs) if (!state<OutputState>(output)->finished) return;
        r->status = 2;   // AVAssetReaderStatusCompleted
    }
    void close_output(OutputState& o) {
        close_guest_audio(support, o.file);
        if (o.format_description) { support.release(o.format_description); o.format_description = 0; }
    }
    // The client format of an output from its settings (nil: the track's format, or float32 for a mix).
    bool client_format(OutputState& o, const AssetState& asset) {
        auto f = asset.container.format;
        if (!o.settings) {
            if (o.mix) { f.is_float = true; f.bits = 32; f.container = 4; f.big_endian = false; f.is_signed = true; f.aligned_high = false; }
            o.client = f;
            return true;
        }
        const auto format_id = setting(o.settings, "AVFormatIDKey");
        if (!format_id || static_cast<std::uint32_t>(*format_id) != audio_file::four("lpcm"))
            unsupported("AVAssetReader output settings other than linear PCM (no audio encoder exists)");
        if (const auto rate = setting(o.settings, "AVSampleRateKey")) f.rate = *rate;
        if (const auto channels = setting(o.settings, "AVNumberOfChannelsKey")) f.channels = static_cast<std::uint32_t>(*channels);
        if (const auto bits = setting(o.settings, "AVLinearPCMBitDepthKey")) { f.bits = static_cast<std::uint32_t>(*bits); f.container = (f.bits + 7) / 8; }
        f.is_float = setting(o.settings, "AVLinearPCMIsFloatKey").value_or(0) != 0;
        f.big_endian = setting(o.settings, "AVLinearPCMIsBigEndianKey").value_or(0) != 0;
        f.non_interleaved = setting(o.settings, "AVLinearPCMIsNonInterleaved").value_or(0) != 0;
        f.is_signed = true;
        f.aligned_high = false;
        if (f.is_float && f.bits != 32 && f.bits != 64) unsupported("AVAssetReader float output of " + std::to_string(f.bits) + " bits");
        if (!f.is_float && (f.bits < 8 || f.bits > 32)) unsupported("AVAssetReader integer output of " + std::to_string(f.bits) + " bits");
        if (!audio_file::convertible(asset.container.format, f))
            unsupported("AVAssetReader output settings that need sample-rate conversion or channel remapping");
        o.client = f;
        return true;
    }
    // copyNextSampleBuffer's work: a +1 CMSampleBuffer or 0 at the end.
    Address next_buffer(Address output) {
        auto o = state<OutputState>(output);
        if (o->finished || !o->reader) return 0;
        auto r = state<ReaderState>(o->reader);
        if (r->status != 1) return 0;
        const auto& asset = *state<AssetState>(r->asset);
        const auto frames = std::min<std::uint64_t>(o->end - std::min(o->end, o->position), 8192);
        if (!frames) {
            o->finished = true;
            close_output(*o);
            finish_reader_if_done(o->reader);
            return 0;
        }
        const auto& source = asset.container.format;
        std::vector<double> samples;
        const auto got = read_audio_frames(support, o->file, o->position, static_cast<std::size_t>(frames), samples);
        if (!got) {   // the file ended early
            o->finished = true;
            close_output(*o);
            finish_reader_if_done(o->reader);
            return 0;
        }
        const auto plane = got * o->client.frame_bytes();
        const std::uint32_t planes = o->client.non_interleaved ? o->client.channels : 1;
        std::vector<std::uint8_t> data(static_cast<std::size_t>(plane * planes));
        std::vector<std::span<std::uint8_t>> spans;
        for (std::uint32_t i = 0; i < planes; ++i) spans.emplace_back(data.data() + i * plane, static_cast<std::size_t>(plane));
        audio_file::encode(o->client, samples, source.channels, got, spans);
        auto& m = support.memory();
        // CMBlockBufferCreateWithMemoryBlock(NULL, NULL, length, NULL, NULL, 0, length, kCMBlockBufferAssureMemoryNowFlag, &out)
        const auto block_out = support.process.allocate_guest(8, 8);
        CallRegs create{};
        create.x[2] = data.size();
        create.x[6] = data.size();
        create.x[7] = 1;
        create.push_stack(block_out);
        if (static_cast<std::int32_t>(support.call(core_media, "_CMBlockBufferCreateWithMemoryBlock", create).x0)) fail("CMBlockBufferCreateWithMemoryBlock failed");
        const auto block = m.read64(block_out);
        const auto staging = support.process.allocate_guest(data.size(), 16);
        m.write_bytes(staging, data);
        static_cast<void>(support.call(core_media, "_CMBlockBufferReplaceDataBytes", {staging, block, 0, data.size()}));
        support.process.release_guest(staging);
        // One timing entry {duration 1/rate, PTS position/rate, DTS invalid} and one sample size.
        const auto timing = support.process.allocate_guest(72 + 8, 8);
        const auto rate = o->client.rate;
        const bool integral = rate == std::floor(rate) && rate <= 0x7fffffff;
        const auto timescale = integral ? static_cast<std::int32_t>(rate) : 1000000;
        write_time(m, timing, {integral ? 1 : static_cast<std::int64_t>(std::llround(1e6 / rate)), timescale, 1, 0});
        write_time(m, timing + 24, {integral ? static_cast<std::int64_t>(o->position) : static_cast<std::int64_t>(std::llround(static_cast<double>(o->position) * 1e6 / rate)), timescale, 1, 0});
        write_time(m, timing + 48, time_invalid);
        m.write64(timing + 72, o->client.frame_bytes());
        const auto sample_out = support.process.allocate_guest(8, 8);
        CallRegs ready{};
        ready.x[1] = block; ready.x[2] = o->format_description; ready.x[3] = got; ready.x[4] = 1; ready.x[5] = timing; ready.x[6] = 1; ready.x[7] = timing + 72;
        ready.push_stack(sample_out);
        const auto result = static_cast<std::int32_t>(support.call(core_media, "_CMSampleBufferCreateReady", ready).x0);
        const auto sample = m.read64(sample_out);
        support.release(block);
        support.process.release_guest(timing);
        support.process.release_guest(sample_out);
        support.process.release_guest(block_out);
        if (result) fail("CMSampleBufferCreateReady failed");
        o->position += got;
        return sample;
    }
};

} // namespace

void install_av_foundation(Support& support) {
    ProcessContext::add_provider_library(std::string(core_audio_kit));
    auto av = std::make_shared<Av>(support);
    auto* sp = &support;
    const auto ns_object = "NSObject";

    // ---- AVAsset / AVURLAsset ---------------------------------------------------------
    av->asset_class = support.declare_class("AVAsset", ns_object, av_foundation);
    av->url_asset_class = support.declare_class("AVURLAsset", "AVAsset", av_foundation);
    av->track_class = support.declare_class("AVAssetTrack", ns_object, av_foundation);
    const auto asset = av->asset_class, url_asset = av->url_asset_class, track = av->track_class;
    // +[AVAsset assetWithURL:] -> an AVURLAsset.
    support.method(asset, "assetWithURL:", [av](CpuState& s, GuestMemory&) {
        CallRegs r{};
        r.x[2] = s.x[2];
        const auto made = av->support.send(av->support.send_id(av->url_asset_class, "alloc"), "initWithURL:options:", r).x0;
        s.x[0] = av->autorelease(made);
    }, "@@:@", true);
    support.method(url_asset, "URLAssetWithURL:options:", [av](CpuState& s, GuestMemory&) {
        CallRegs r{};
        r.x[2] = s.x[2];
        r.x[3] = s.x[3];
        s.x[0] = av->autorelease(av->support.send(av->support.send_id(s.x[0], "alloc"), "initWithURL:options:", r).x0);
    }, "@@:@@", true);
    // -initWithURL:options: (AVURLAssetPreferPreciseDurationAndTimingKey: PCM timing is always precise).
    support.method(url_asset, "initWithURL:options:", [av](CpuState& s, GuestMemory&) {
        if (!s.x[2]) unsupported("-[AVURLAsset initWithURL:options:] with a nil URL");
        av->state<AssetState>(s.x[0])->url = av->retain(s.x[2]);
    }, "@@:@@");
    support.method(url_asset, "URL", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<AssetState>(s.x[0])->url; }, "@@:");
    support.method(asset, "dealloc", [av, asset](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<AssetState>(s.x[0])) {
            av->release(std::exchange(st->url, 0));
            av->release(std::exchange(st->track, 0));
        }
        static_cast<void>(av->support.objc.send_super(s.x[0], asset, av->support.objc.selector("dealloc")));
    }, "v@:");
    // CMTime -duration (x8).
    support.method(asset, "duration", [av](CpuState& s, GuestMemory& m) { write_time(m, s.x[8], av->duration_of(av->load(s.x[0]))); }, "{?=qiIq}@:");
    support.method(asset, "tracks", [av](CpuState& s, GuestMemory&) {
        const auto& st = av->load(s.x[0]);
        s.x[0] = st.track ? av->autorelease(av->bridge->array({st.track})) : av->empty_array();
    }, "@@:");
    support.method(asset, "tracksWithMediaType:", [av](CpuState& s, GuestMemory&) {
        const auto& st = av->load(s.x[0]);
        const bool audio = s.x[2] && av->support.cf.utf8(s.x[2]) == "soun";
        s.x[0] = st.track && audio ? av->autorelease(av->bridge->array({st.track})) : av->empty_array();
    }, "@@:@");
    support.method(asset, "trackWithTrackID:", [av](CpuState& s, GuestMemory&) {
        const auto& st = av->load(s.x[0]);
        s.x[0] = st.track && static_cast<std::int32_t>(s.x[2]) == 1 ? st.track : 0;
    }, "@@:i");
    for (const auto* name : {"commonMetadata", "metadata", "availableMetadataFormats", "availableChapterLocales", "chapterMetadataGroupsWithTitleLocale:containingItemsWithCommonKeys:"})
        support.method(asset, name, [av](CpuState& s, GuestMemory&) { s.x[0] = av->empty_array(); }, "@@:");
    for (const auto* name : {"hasProtectedContent", "isPlayable", "isExportable", "isComposable", "isCompatibleWithSavedPhotosAlbum"})
        support.method(asset, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    support.method(asset, "isReadable", [av](CpuState& s, GuestMemory&) { s.x[0] = av->load(s.x[0]).readable() ? 1 : 0; }, "B@:");
    support.method(asset, "providesPreciseDurationAndTiming", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    support.method(asset, "preferredRate", [](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint32_t>(1.0f), 0}; }, "f@:");
    support.method(asset, "preferredVolume", [](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint32_t>(1.0f), 0}; }, "f@:");
    // -statusOfValueForKey:error: AVKeyValueStatusLoaded (2) or Failed (3, with the reason).
    support.method(asset, "statusOfValueForKey:error:", [av](CpuState& s, GuestMemory&) {
        const auto& st = av->load(s.x[0]);
        if (st.readable()) { s.x[0] = 2; return; }
        av->store_error(s.x[3], av_error_file_format_not_recognized, "The media cannot be read under Alchemy (only linear PCM WAVE, AIFF and CAF files are)");
        s.x[0] = 3;
    }, "q@:@^@");
    // -loadValuesAsynchronouslyForKeys:completionHandler: loads now; the handler runs on a global queue.
    support.method(asset, "loadValuesAsynchronouslyForKeys:completionHandler:", [av](CpuState& s, GuestMemory&) {
        static_cast<void>(av->load(s.x[0]));
        if (!s.x[3]) return;
        const auto block = av->bridge->block_copy(s.x[3]);
        const auto queue = av->support.call(libsystem, "_dispatch_get_global_queue", {0, 0});
        auto bridge = av->bridge;
        bridge->dispatch_async(queue, [bridge, block] {
            static_cast<void>(bridge->invoke_block(block, {}));
            bridge->block_release(block);
        });
    }, "v@:@@?");
    support.method(asset, "cancelLoading", [](CpuState&, GuestMemory&) {}, "v@:");   // loading never runs in the background

    // AVAssetTrack: the one audio track of a readable asset.
    support.method(track, "dealloc", [av, track](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<TrackState>(s.x[0])) av->support.release(std::exchange(st->format_description, 0));
        static_cast<void>(av->support.objc.send_super(s.x[0], track, av->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(track, "asset", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<TrackState>(s.x[0])->asset; }, "@@:");
    support.method(track, "mediaType", [av](CpuState& s, GuestMemory&) { s.x[0] = av->string("soun"); }, "@@:");
    support.method(track, "trackID", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "i@:");
    support.method(track, "formatDescriptions", [av](CpuState& s, GuestMemory&) {
        s.x[0] = av->autorelease(av->bridge->array({av->state<TrackState>(s.x[0])->format_description}));
    }, "@@:");
    // CMTimeRange -timeRange (x8): from zero for the asset's duration.
    support.method(track, "timeRange", [av](CpuState& s, GuestMemory& m) {
        const auto owner = av->state<TrackState>(s.x[0])->asset;
        const auto duration = av->duration_of(av->load(owner));
        write_time(m, s.x[8], {0, duration.timescale, 1, 0});
        write_time(m, s.x[8] + 24, duration);
    }, "{?={?=qiIq}{?=qiIq}}@:");
    support.method(track, "naturalTimeScale", [av](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint32_t>(av->duration_of(av->load(av->state<TrackState>(s.x[0])->asset)).timescale);
    }, "i@:");
    support.method(track, "estimatedDataRate", [av](CpuState& s, GuestMemory&) {
        const auto& st = av->load(av->state<TrackState>(s.x[0])->asset);
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(st.container.format.rate * st.container.format.frame_bytes() * 8)), 0};
    }, "f@:");
    support.method(track, "totalSampleDataLength", [av](CpuState& s, GuestMemory&) {
        s.x[0] = av->load(av->state<TrackState>(s.x[0])->asset).container.data_bytes;
    }, "q@:");
    support.method(track, "nominalFrameRate", [](CpuState& s, GuestMemory&) { s.v[0] = {0, 0}; }, "f@:");
    support.method(track, "naturalSize", [](CpuState& s, GuestMemory&) { s.v[0] = {0, 0}; s.v[1] = {0, 0}; }, "{CGSize=dd}@:");
    for (const auto* name : {"isEnabled", "isDecodable", "isSelfContained"})
        support.method(track, name, [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:");
    support.method(track, "isPlayable", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");   // there is no AVPlayer
    for (const auto* name : {"commonMetadata", "metadata", "availableMetadataFormats"})
        support.method(track, name, [av](CpuState& s, GuestMemory&) { s.x[0] = av->empty_array(); }, "@@:");

    // ---- AVAssetReader ---------------------------------------------------------------------
    av->reader_class = support.declare_class("AVAssetReader", ns_object, av_foundation);
    av->output_class = support.declare_class("AVAssetReaderOutput", ns_object, av_foundation);
    av->track_output_class = support.declare_class("AVAssetReaderTrackOutput", "AVAssetReaderOutput", av_foundation);
    av->mix_output_class = support.declare_class("AVAssetReaderAudioMixOutput", "AVAssetReaderOutput", av_foundation);
    const auto reader = av->reader_class, output = av->output_class, track_output = av->track_output_class, mix_output = av->mix_output_class;
    support.method(reader, "assetReaderWithAsset:error:", [av](CpuState& s, GuestMemory&) {
        CallRegs r{};
        r.x[2] = s.x[2];
        r.x[3] = s.x[3];
        s.x[0] = av->autorelease(av->support.send(av->support.send_id(s.x[0], "alloc"), "initWithAsset:error:", r).x0);
    }, "@@:@^@", true);
    // -initWithAsset:error: nil (with AVErrorFileFormatNotRecognized) for media Alchemy cannot read.
    support.method(reader, "initWithAsset:error:", [av](CpuState& s, GuestMemory&) {
        if (!s.x[2]) unsupported("-[AVAssetReader initWithAsset:error:] with a nil asset");
        if (!av->load(s.x[2]).readable()) {
            av->store_error(s.x[3], av_error_file_format_not_recognized, "Cannot Open (Alchemy reads only linear PCM WAVE, AIFF and CAF media)");
            av->release(s.x[0]);
            s.x[0] = 0;
            return;
        }
        av->state<ReaderState>(s.x[0])->asset = av->retain(s.x[2]);
    }, "@@:@^@");
    support.method(reader, "dealloc", [av, reader](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<ReaderState>(s.x[0])) {
            for (const auto o : st->outputs) {
                auto os = av->state<OutputState>(o);
                av->close_output(*os);
                os->reader = 0;
                av->release(o);
            }
            st->outputs.clear();
            av->release(std::exchange(st->asset, 0));
            av->release(std::exchange(st->error, 0));
        }
        static_cast<void>(av->support.objc.send_super(s.x[0], reader, av->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(reader, "asset", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<ReaderState>(s.x[0])->asset; }, "@@:");
    support.method(reader, "status", [av](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>(av->state<ReaderState>(s.x[0])->status); }, "q@:");
    support.method(reader, "error", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<ReaderState>(s.x[0])->error; }, "@@:");
    support.method(reader, "outputs", [av](CpuState& s, GuestMemory&) { s.x[0] = av->autorelease(av->bridge->array(av->state<ReaderState>(s.x[0])->outputs)); }, "@@:");
    support.method(reader, "canAddOutput:", [av](CpuState& s, GuestMemory&) {
        const auto r = av->state<ReaderState>(s.x[0]);
        const auto o = s.x[2] ? av->support.objc.find_host_state<OutputState>(s.x[2]) : nullptr;
        s.x[0] = r->status == 0 && o && !o->reader ? 1 : 0;
    }, "B@:@");
    support.method(reader, "addOutput:", [av](CpuState& s, GuestMemory&) {
        const auto r = av->state<ReaderState>(s.x[0]);
        const auto o = s.x[2] ? av->support.objc.find_host_state<OutputState>(s.x[2]) : nullptr;
        if (r->status != 0 || !o || o->reader) fail("-[AVAssetReader addOutput:] cannot add this output (NSInvalidArgumentException)");
        for (const auto t : o->tracks)
            if (av->state<TrackState>(t)->asset != r->asset) fail("-[AVAssetReader addOutput:] the output's track belongs to another asset");
        o->reader = s.x[0];
        r->outputs.push_back(av->retain(s.x[2]));
    }, "v@:@");
    // CMTimeRange -timeRange (x8), -setTimeRange: (CMTimeRange by reference in x2): before reading only.
    support.method(reader, "timeRange", [av](CpuState& s, GuestMemory& m) {
        const auto r = av->state<ReaderState>(s.x[0]);
        write_time(m, s.x[8], r->start);
        write_time(m, s.x[8] + 24, r->duration);
    }, "{?={?=qiIq}{?=qiIq}}@:");
    support.method(reader, "setTimeRange:", [av](CpuState& s, GuestMemory& m) {
        const auto r = av->state<ReaderState>(s.x[0]);
        if (r->status != 0) fail("-[AVAssetReader setTimeRange:] after reading started (NSInternalInconsistencyException)");
        r->start = read_time(m, s.x[2]);
        r->duration = read_time(m, s.x[2] + 24);
    }, "v@:{?={?=qiIq}{?=qiIq}}");
    support.method(reader, "startReading", [av](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto r = av->state<ReaderState>(self);
        if (r->status != 0) { s.x[0] = 0; return; }
        const auto& asset = av->load(r->asset);
        const auto frames = asset.frames();
        const auto rate = asset.container.format.rate;
        const auto frame_at = [&](const Time& t) -> std::uint64_t {
            if ((t.flags & 4) || !(t.flags & 1)) return frames;   // +infinity / invalid
            if (t.timescale <= 0 || t.value <= 0) return 0;
            return std::min<std::uint64_t>(frames, static_cast<std::uint64_t>(std::llround(static_cast<double>(t.value) / t.timescale * rate)));
        };
        const auto first = frame_at(r->start);
        const auto last = (r->duration.flags & 4) ? frames : std::min<std::uint64_t>(frames, first + frame_at(r->duration));
        for (const auto o : r->outputs) {
            auto os = av->state<OutputState>(o);
            av->client_format(*os, asset);
            if (const auto result = open_guest_audio(av->support, av->support.cf.url_path(av->state<AssetState>(r->asset)->url), os->file)) {
                r->status = 3;   // AVAssetReaderStatusFailed
                r->error = av->bridge->os_status_error(result, "The media file could not be opened again for reading");
                s.x[0] = 0;
                return;
            }
            os->position = first;
            os->end = last;
            os->finished = false;
            os->format_description = av->make_format_description(audio_file::asbd_from_pcm(os->client), {});
        }
        r->status = 1;   // AVAssetReaderStatusReading
        s.x[0] = 1;
    }, "B@:");
    support.method(reader, "cancelReading", [av](CpuState& s, GuestMemory&) {
        const auto r = av->state<ReaderState>(s.x[0]);
        for (const auto o : r->outputs) { auto os = av->state<OutputState>(o); av->close_output(*os); os->finished = true; }
        if (r->status == 0 || r->status == 1) r->status = 4;   // AVAssetReaderStatusCancelled
    }, "v@:");

    // Outputs.
    support.method(output, "dealloc", [av, output](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<OutputState>(s.x[0])) {
            av->close_output(*st);
            for (const auto t : st->tracks) av->release(t);
            st->tracks.clear();
            av->release(std::exchange(st->settings, 0));
            av->release(std::exchange(st->audio_mix, 0));
        }
        static_cast<void>(av->support.objc.send_super(s.x[0], output, av->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(output, "copyNextSampleBuffer", [av](CpuState& s, GuestMemory&) {
        const auto o = av->state<OutputState>(s.x[0]);
        if (!o->reader || av->state<ReaderState>(o->reader)->status == 0)
            fail("-[AVAssetReaderOutput copyNextSampleBuffer] before -[AVAssetReader startReading] (NSInternalInconsistencyException)");
        s.x[0] = av->next_buffer(s.x[0]);
    }, "^{opaqueCMSampleBuffer=}@:");
    support.method(output, "mediaType", [av](CpuState& s, GuestMemory&) { s.x[0] = av->string("soun"); }, "@@:");
    support.method(output, "alwaysCopiesSampleData", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<OutputState>(s.x[0])->always_copies; }, "B@:");
    support.method(output, "setAlwaysCopiesSampleData:", [av](CpuState& s, GuestMemory&) { av->state<OutputState>(s.x[0])->always_copies = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(output, "supportsRandomAccess", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:");
    const auto init_output = [av](CpuState& s, std::vector<Address> tracks, Address settings, bool mix) {
        if (tracks.empty()) unsupported("an AVAssetReader output without tracks");
        auto st = av->state<OutputState>(s.x[0]);
        for (const auto t : tracks) {
            if (!av->support.objc.find_host_state<TrackState>(t)) unsupported("an AVAssetReader output for a track Alchemy did not make");
            st->tracks.push_back(av->retain(t));
        }
        st->settings = settings ? av->retain(settings) : 0;
        st->mix = mix;
    };
    support.method(track_output, "assetReaderTrackOutputWithTrack:outputSettings:", [av](CpuState& s, GuestMemory&) {
        CallRegs r{};
        r.x[2] = s.x[2]; r.x[3] = s.x[3];
        s.x[0] = av->autorelease(av->support.send(av->support.send_id(s.x[0], "alloc"), "initWithTrack:outputSettings:", r).x0);
    }, "@@:@@", true);
    support.method(track_output, "initWithTrack:outputSettings:", [init_output](CpuState& s, GuestMemory&) {
        init_output(s, {s.x[2]}, s.x[3], false);
    }, "@@:@@");
    support.method(track_output, "track", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<OutputState>(s.x[0])->tracks.front(); }, "@@:");
    support.method(track_output, "outputSettings", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<OutputState>(s.x[0])->settings; }, "@@:");
    support.method(mix_output, "assetReaderAudioMixOutputWithAudioTracks:audioSettings:", [av](CpuState& s, GuestMemory&) {
        CallRegs r{};
        r.x[2] = s.x[2]; r.x[3] = s.x[3];
        s.x[0] = av->autorelease(av->support.send(av->support.send_id(s.x[0], "alloc"), "initWithAudioTracks:audioSettings:", r).x0);
    }, "@@:@@", true);
    // A mix of the asset's audio tracks: every readable asset has one, so the mix is that track.
    support.method(mix_output, "initWithAudioTracks:audioSettings:", [av, init_output](CpuState& s, GuestMemory&) {
        init_output(s, s.x[2] ? av->support.cf.array_values(s.x[2]) : std::vector<Address>{}, s.x[3], true);
        if (av->state<OutputState>(s.x[0])->tracks.size() != 1) unsupported("AVAssetReaderAudioMixOutput over several tracks (Alchemy assets have one audio track)");
    }, "@@:@@");
    support.method(mix_output, "audioTracks", [av](CpuState& s, GuestMemory&) { s.x[0] = av->autorelease(av->bridge->array(av->state<OutputState>(s.x[0])->tracks)); }, "@@:");
    support.method(mix_output, "audioSettings", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<OutputState>(s.x[0])->settings; }, "@@:");
    support.method(mix_output, "audioMix", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<OutputState>(s.x[0])->audio_mix; }, "@@:");
    // An AVAudioMix sets per-track volume ramps; a one-track mix with input parameters is not applied.
    support.method(mix_output, "setAudioMix:", [av](CpuState& s, GuestMemory&) {
        if (s.x[2]) {
            const auto parameters = av->support.send_id(s.x[2], "inputParameters");
            if (parameters && av->support.cf.length(parameters)) unsupported("AVAudioMix input parameters (volume ramps) on an AVAssetReaderAudioMixOutput");
        }
        auto st = av->state<OutputState>(s.x[0]);
        const auto old = std::exchange(st->audio_mix, av->retain(s.x[2]));
        av->release(old);
    }, "v@:@");

    // ---- AVAssetImageGenerator -----------------------------------------------------------
    av->generator_class = support.declare_class("AVAssetImageGenerator", ns_object, av_foundation);
    const auto generator = av->generator_class;
    support.method(generator, "assetImageGeneratorWithAsset:", [av](CpuState& s, GuestMemory&) {
        s.x[0] = av->autorelease(av->support.send_id(av->support.send_id(s.x[0], "alloc"), "initWithAsset:", {s.x[2]}));
    }, "@@:@", true);
    support.method(generator, "initWithAsset:", [av](CpuState& s, GuestMemory&) { av->state<GeneratorState>(s.x[0])->asset = av->retain(s.x[2]); }, "@@:@");
    support.method(generator, "dealloc", [av, generator](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<GeneratorState>(s.x[0])) av->release(std::exchange(st->asset, 0));
        static_cast<void>(av->support.objc.send_super(s.x[0], generator, av->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(generator, "asset", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<GeneratorState>(s.x[0])->asset; }, "@@:");
    support.method(generator, "appliesPreferredTrackTransform", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<GeneratorState>(s.x[0])->applies_transform; }, "B@:");
    support.method(generator, "setAppliesPreferredTrackTransform:", [av](CpuState& s, GuestMemory&) { av->state<GeneratorState>(s.x[0])->applies_transform = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(generator, "maximumSize", [av](CpuState& s, GuestMemory&) {
        const auto st = av->state<GeneratorState>(s.x[0]);
        s.v[0] = {std::bit_cast<std::uint64_t>(st->maximum_width), 0};
        s.v[1] = {std::bit_cast<std::uint64_t>(st->maximum_height), 0};
    }, "{CGSize=dd}@:");
    support.method(generator, "setMaximumSize:", [av](CpuState& s, GuestMemory&) {
        const auto st = av->state<GeneratorState>(s.x[0]);
        st->maximum_width = std::bit_cast<double>(s.v[0][0]);
        st->maximum_height = std::bit_cast<double>(s.v[1][0]);
    }, "v@:{CGSize=dd}");
    support.method(generator, "setRequestedTimeToleranceBefore:", [av](CpuState& s, GuestMemory& m) { av->state<GeneratorState>(s.x[0])->before = read_time(m, s.x[2]); }, "v@:{?=qiIq}");
    support.method(generator, "setRequestedTimeToleranceAfter:", [av](CpuState& s, GuestMemory& m) { av->state<GeneratorState>(s.x[0])->after = read_time(m, s.x[2]); }, "v@:{?=qiIq}");
    // -copyCGImageAtTime:actualTime:error: (CMTime by reference in x2): no video decoder exists.
    support.method(generator, "copyCGImageAtTime:actualTime:error:", [av](CpuState& s, GuestMemory& m) {
        const auto st = av->state<GeneratorState>(s.x[0]);
        if (s.x[3]) write_time(m, s.x[3], time_invalid);
        const bool audio_only = st->asset && av->load(st->asset).readable();
        if (audio_only) av->store_error(s.x[4], av_error_operation_not_supported_for_asset, "The asset has no video track");
        else av->store_error(s.x[4], av_error_decoder_not_found, "No video decoder exists under Alchemy");
        s.x[0] = 0;
    }, "^{CGImage=}@:{?=qiIq}^{?=qiIq}^@");
    support.method(generator, "cancelAllCGImageGeneration", [](CpuState&, GuestMemory&) {}, "v@:");   // nothing is ever generating

    // ---- AVMetadataItem ---------------------------------------------------------------------
    av->metadata_class = support.declare_class("AVMetadataItem", ns_object, av_foundation);
    const auto metadata = av->metadata_class;
    support.method(metadata, "dealloc", [av, metadata](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<MetadataState>(s.x[0]))
            for (auto* slot : {&st->key, &st->common_key, &st->key_space, &st->value, &st->identifier}) av->release(std::exchange(*slot, 0));
        static_cast<void>(av->support.objc.send_super(s.x[0], metadata, av->support.objc.selector("dealloc")));
    }, "v@:");
    for (const auto& [name, field] : std::initializer_list<std::pair<const char*, Address MetadataState::*>>{
             {"key", &MetadataState::key}, {"commonKey", &MetadataState::common_key}, {"keySpace", &MetadataState::key_space},
             {"value", &MetadataState::value}, {"identifier", &MetadataState::identifier}}) {
        const auto member = field;
        support.method(metadata, name, [av, member](CpuState& s, GuestMemory&) { s.x[0] = (*av->state<MetadataState>(s.x[0])).*member; }, "@@:");
    }
    support.method(metadata, "stringValue", [av](CpuState& s, GuestMemory&) {
        const auto value = av->state<MetadataState>(s.x[0])->value;
        s.x[0] = value && av->bridge->is_string(value) ? value : 0;
    }, "@@:");
    support.method(metadata, "numberValue", [av](CpuState& s, GuestMemory&) {
        const auto value = av->state<MetadataState>(s.x[0])->value;
        s.x[0] = value && av->support.cf.kind(value) == ObjectKind::number ? value : 0;
    }, "@@:");
    support.method(metadata, "dataValue", [av](CpuState& s, GuestMemory&) {
        const auto value = av->state<MetadataState>(s.x[0])->value;
        s.x[0] = value && av->bridge->is_data(value) ? value : 0;
    }, "@@:");
    // +metadataItemsFromArray:withKey:keySpace: (nil key or key space matches any).
    support.method(metadata, "metadataItemsFromArray:withKey:keySpace:", [av](CpuState& s, GuestMemory&) {
        std::vector<Address> kept;
        for (const auto item : s.x[2] ? av->support.cf.array_values(s.x[2]) : std::vector<Address>{}) {
            const auto st = av->support.objc.find_host_state<MetadataState>(item);
            if (!st) continue;
            if (s.x[3] && !(st->key && av->support.cf.equal(st->key, s.x[3]))) continue;
            if (s.x[4] && !(st->key_space && av->support.cf.equal(st->key_space, s.x[4]))) continue;
            kept.push_back(item);
        }
        s.x[0] = av->autorelease(av->bridge->array(kept));
    }, "@@:@@@", true);

    // ---- AVAudioChannelLayout / AVAudioFormat -------------------------------------------------
    av->layout_class = support.declare_class("AVAudioChannelLayout", ns_object, av_foundation);
    av->format_class = support.declare_class("AVAudioFormat", ns_object, av_foundation);
    const auto layout = av->layout_class, format = av->format_class;
    const auto set_layout = [av](Address self, std::vector<std::uint8_t> bytes) {
        auto st = av->state<LayoutState>(self);
        st->bytes = std::move(bytes);
        st->guest = av->support.process.allocate_guest(st->bytes.size(), 8);
        av->support.memory().write_bytes(st->guest, st->bytes);
    };
    // -initWithLayoutTag: nil for the tags that need a full layout (descriptions, bitmap).
    support.method(layout, "initWithLayoutTag:", [av, set_layout](CpuState& s, GuestMemory&) {
        const auto tag = static_cast<std::uint32_t>(s.x[2]);
        if (tag == 0 || tag == 0x10000) { av->release(s.x[0]); s.x[0] = 0; return; }
        std::vector<std::uint8_t> bytes(12, 0);
        std::memcpy(bytes.data(), &tag, 4);
        set_layout(s.x[0], std::move(bytes));
    }, "@@:I");
    support.method(layout, "layoutWithLayoutTag:", [av](CpuState& s, GuestMemory&) {
        CallRegs r{};
        r.x[2] = s.x[2];
        s.x[0] = av->autorelease(av->support.send(av->support.send_id(s.x[0], "alloc"), "initWithLayoutTag:", r).x0);
    }, "@@:I", true);
    // -initWithLayout:(const AudioChannelLayout *): a copy (12 bytes + 20 per description).
    support.method(layout, "initWithLayout:", [av, set_layout](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) { av->release(s.x[0]); s.x[0] = 0; return; }
        const auto count = m.read32(s.x[2] + 8);
        if (count > 4096) unsupported("an AudioChannelLayout with more than 4096 channel descriptions");
        std::vector<std::uint8_t> bytes(12 + 20 * static_cast<std::size_t>(count));
        m.read_bytes(s.x[2], bytes);
        set_layout(s.x[0], std::move(bytes));
    }, "@@:^{AudioChannelLayout=}");
    support.method(layout, "layoutWithLayout:", [av](CpuState& s, GuestMemory&) {
        s.x[0] = av->autorelease(av->support.send_id(av->support.send_id(s.x[0], "alloc"), "initWithLayout:", {s.x[2]}));
    }, "@@:^{AudioChannelLayout=}", true);
    support.method(layout, "dealloc", [av, layout](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<LayoutState>(s.x[0]); st && st->guest) av->support.process.release_guest(std::exchange(st->guest, 0));
        static_cast<void>(av->support.objc.send_super(s.x[0], layout, av->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(layout, "layoutTag", [av](CpuState& s, GuestMemory&) { s.x[0] = le32(av->state<LayoutState>(s.x[0])->bytes, 0); }, "I@:");
    support.method(layout, "layout", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<LayoutState>(s.x[0])->guest; }, "^{AudioChannelLayout=}@:");
    support.method(layout, "channelCount", [av](CpuState& s, GuestMemory&) { s.x[0] = layout_channels(av->state<LayoutState>(s.x[0])->bytes); }, "I@:");
    support.method(layout, "isEqual:", [av](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? av->support.objc.find_host_state<LayoutState>(s.x[2]) : nullptr;
        s.x[0] = other && other->bytes == av->state<LayoutState>(s.x[0])->bytes ? 1 : 0;
    }, "B@:@");
    support.method(layout, "hash", [av](CpuState& s, GuestMemory&) { s.x[0] = le32(av->state<LayoutState>(s.x[0])->bytes, 0); }, "Q@:");
    support.method(layout, "copyWithZone:", [av](CpuState& s, GuestMemory&) { s.x[0] = av->retain(s.x[0]); }, "@@:^v");

    // AVAudioFormat: the stream description, its common format, interleaving and layout.
    const auto init_format = [av](CpuState& s, std::vector<std::uint8_t> asbd, Address channel_layout) {
        const auto self = s.x[0];
        std::uint32_t channels{};
        std::memcpy(&channels, asbd.data() + 28, 4);
        double rate{};
        std::memcpy(&rate, asbd.data(), 8);
        const bool layout_ok = !channel_layout || layout_channels(av->state<LayoutState>(channel_layout)->bytes) == channels;
        // More than two channels need a layout (as AVAudioFormat requires).
        if (asbd.size() < 40 || !(rate > 0) || channels == 0 || (channels > 2 && !channel_layout) || !layout_ok) {
            av->release(self);
            s.x[0] = 0;
            return;
        }
        auto st = av->state<FormatState>(self);
        st->asbd = std::move(asbd);
        st->common = common_of(st->asbd);
        st->layout = av->retain(channel_layout);
        st->guest_asbd = av->support.process.allocate_guest(40, 8);
        av->support.memory().write_bytes(st->guest_asbd, st->asbd);
    };
    // -initWithCommonFormat:sampleRate:interleaved:channelLayout: (format x2, rate d0, BOOL x3, layout x4)
    support.method(format, "initWithCommonFormat:sampleRate:interleaved:channelLayout:", [av, init_format](CpuState& s, GuestMemory&) {
        if (!s.x[4]) { av->release(s.x[0]); s.x[0] = 0; return; }
        auto asbd = asbd_for(s.x[2], std::bit_cast<double>(s.v[0][0]), layout_channels(av->state<LayoutState>(s.x[4])->bytes), (s.x[3] & 0xff) != 0);
        if (asbd.empty()) { av->release(s.x[0]); s.x[0] = 0; return; }
        init_format(s, std::move(asbd), s.x[4]);
    }, "@@:QdB@");
    // -initWithCommonFormat:sampleRate:channels:interleaved: (format x2, rate d0, channels x3, BOOL x4)
    support.method(format, "initWithCommonFormat:sampleRate:channels:interleaved:", [av, init_format](CpuState& s, GuestMemory&) {
        auto asbd = asbd_for(s.x[2], std::bit_cast<double>(s.v[0][0]), static_cast<std::uint32_t>(s.x[3]), (s.x[4] & 0xff) != 0);
        if (asbd.empty()) { av->release(s.x[0]); s.x[0] = 0; return; }
        init_format(s, std::move(asbd), 0);
    }, "@@:QdIB");
    // -initStandardFormatWithSampleRate:channels: float32, deinterleaved.
    support.method(format, "initStandardFormatWithSampleRate:channels:", [init_format](CpuState& s, GuestMemory&) {
        init_format(s, asbd_for(1, std::bit_cast<double>(s.v[0][0]), static_cast<std::uint32_t>(s.x[2]), false), 0);
    }, "@@:dI");
    support.method(format, "initStandardFormatWithSampleRate:channelLayout:", [av, init_format](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { av->release(s.x[0]); s.x[0] = 0; return; }
        init_format(s, asbd_for(1, std::bit_cast<double>(s.v[0][0]), layout_channels(av->state<LayoutState>(s.x[2])->bytes), false), s.x[2]);
    }, "@@:d@");
    support.method(format, "initWithStreamDescription:", [av, init_format](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) { av->release(s.x[0]); s.x[0] = 0; return; }
        std::vector<std::uint8_t> asbd(40);
        m.read_bytes(s.x[2], asbd);
        init_format(s, std::move(asbd), 0);
    }, "@@:^{AudioStreamBasicDescription=dIIIIIIII}");
    support.method(format, "initWithStreamDescription:channelLayout:", [av, init_format](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) { av->release(s.x[0]); s.x[0] = 0; return; }
        std::vector<std::uint8_t> asbd(40);
        m.read_bytes(s.x[2], asbd);
        init_format(s, std::move(asbd), s.x[3]);
    }, "@@:^{AudioStreamBasicDescription=dIIIIIIII}@");
    support.method(format, "dealloc", [av, format](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<FormatState>(s.x[0])) {
            av->release(std::exchange(st->layout, 0));
            av->support.release(std::exchange(st->format_description, 0));
            if (st->guest_asbd) av->support.process.release_guest(std::exchange(st->guest_asbd, 0));
        }
        static_cast<void>(av->support.objc.send_super(s.x[0], format, av->support.objc.selector("dealloc")));
    }, "v@:");
    const auto asbd_field = [av](Address self, std::size_t at) { return le32(av->state<FormatState>(self)->asbd, at); };
    support.method(format, "sampleRate", [av](CpuState& s, GuestMemory&) {
        double rate{};
        std::memcpy(&rate, av->state<FormatState>(s.x[0])->asbd.data(), 8);
        s.v[0] = {std::bit_cast<std::uint64_t>(rate), 0};
    }, "d@:");
    support.method(format, "channelCount", [asbd_field](CpuState& s, GuestMemory&) { s.x[0] = asbd_field(s.x[0], 28); }, "I@:");
    support.method(format, "commonFormat", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<FormatState>(s.x[0])->common; }, "Q@:");
    support.method(format, "isInterleaved", [asbd_field](CpuState& s, GuestMemory&) { s.x[0] = (asbd_field(s.x[0], 12) & 32) ? 0 : 1; }, "B@:");
    support.method(format, "isStandard", [av, asbd_field](CpuState& s, GuestMemory&) {
        s.x[0] = av->state<FormatState>(s.x[0])->common == 1 && (asbd_field(s.x[0], 12) & 32) ? 1 : 0;
    }, "B@:");
    support.method(format, "streamDescription", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<FormatState>(s.x[0])->guest_asbd; }, "^{AudioStreamBasicDescription=dIIIIIIII}@:");
    support.method(format, "channelLayout", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<FormatState>(s.x[0])->layout; }, "@@:");
    support.method(format, "magicCookie", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    support.method(format, "formatDescription", [av](CpuState& s, GuestMemory&) {
        auto st = av->state<FormatState>(s.x[0]);
        if (!st->format_description)
            st->format_description = av->make_format_description(st->asbd, st->layout ? av->state<LayoutState>(st->layout)->bytes : std::vector<std::uint8_t>{});
        s.x[0] = st->format_description;
    }, "^{opaqueCMFormatDescription=}@:");
    support.method(format, "isEqual:", [av](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? av->support.objc.find_host_state<FormatState>(s.x[2]) : nullptr;
        const auto self = av->state<FormatState>(s.x[0]);
        bool same = other && other->asbd == self->asbd && !other->layout == !self->layout;
        if (same && self->layout) same = av->state<LayoutState>(self->layout)->bytes == av->state<LayoutState>(other->layout)->bytes;
        s.x[0] = same ? 1 : 0;
    }, "B@:@");
    support.method(format, "hash", [asbd_field](CpuState& s, GuestMemory&) { s.x[0] = asbd_field(s.x[0], 28) * 31 + asbd_field(s.x[0], 12); }, "Q@:");
    support.method(format, "copyWithZone:", [av](CpuState& s, GuestMemory&) { s.x[0] = av->retain(s.x[0]); }, "@@:^v");
    // -settings: the AVAudioSettings dictionary of the format.
    support.method(format, "settings", [av, sp](CpuState& s, GuestMemory&) {
        const auto st = av->state<FormatState>(s.x[0]);
        const auto field = [&](std::size_t at) { return le32(st->asbd, at); };
        double rate{};
        std::memcpy(&rate, st->asbd.data(), 8);
        std::vector<std::pair<Address, Address>> entries{
            {sp->cf.string("AVFormatIDKey"), sp->cf.integer(field(8), 3)}, {sp->cf.string("AVSampleRateKey"), sp->cf.real(rate)},
            {sp->cf.string("AVNumberOfChannelsKey"), sp->cf.integer(field(28), 3)}};
        if (field(8) == audio_file::four("lpcm")) {
            entries.push_back({sp->cf.string("AVLinearPCMBitDepthKey"), sp->cf.integer(field(32), 3)});
            entries.push_back({sp->cf.string("AVLinearPCMIsFloatKey"), sp->cf.boolean((field(12) & 1) != 0)});
            entries.push_back({sp->cf.string("AVLinearPCMIsBigEndianKey"), sp->cf.boolean((field(12) & 2) != 0)});
            entries.push_back({sp->cf.string("AVLinearPCMIsNonInterleaved"), sp->cf.boolean((field(12) & 32) != 0)});
        }
        const auto dictionary = av->bridge->dictionary(entries);
        for (const auto& [key, value] : entries) {
            sp->release(key);
            if (sp->cf.kind(value) != ObjectKind::boolean) sp->release(value);   // kCFBooleanTrue/False are constants
        }
        s.x[0] = av->autorelease(dictionary);
    }, "@@:");

    // ---- audio units (version 3): none exists --------------------------------------------------------------
    const auto node = support.objc.find_class("AVAudioNode") ? support.objc.find_class("AVAudioNode") : support.declare_class("AVAudioNode", ns_object, av_foundation);
    static_cast<void>(node);
    const auto audio_unit = support.declare_class("AVAudioUnit", "AVAudioNode", av_foundation);
    const auto component = support.declare_class("AVAudioUnitComponent", ns_object, av_foundation);
    const auto manager = support.declare_class("AVAudioUnitComponentManager", ns_object, av_foundation);
    const auto au_audio_unit = support.declare_class("AUAudioUnit", ns_object, audio_toolbox);
    av->bus_class = support.declare_class("AUAudioUnitBus", ns_object, audio_toolbox);
    const auto bus = av->bus_class;
    const auto no_component_error = [av]() {
        return av->bridge->error("NSOSStatusErrorDomain", invalid_component_id, "No audio unit matches the description (no version 3 audio unit exists under Alchemy)");
    };
    // +instantiateWithComponentDescription:options:completionHandler: (AudioComponentDescription by
    // reference in x2): the handler gets (nil, error).
    for (const auto cls : {audio_unit, au_audio_unit})
        support.method(cls, "instantiateWithComponentDescription:options:completionHandler:", [av, no_component_error](CpuState& s, GuestMemory&) {
            if (!s.x[4]) return;
            const auto error = no_component_error();
            static_cast<void>(av->bridge->invoke_block(s.x[4], {0, error}));
            av->support.release(error);
        }, "v@:{AudioComponentDescription=IIIII}Q@?", true);
    for (const auto& [selector, error_at] : std::initializer_list<std::pair<const char*, unsigned>>{
             {"initWithComponentDescription:error:", 3}, {"initWithComponentDescription:options:error:", 4}}) {
        const auto out = error_at;
        support.method(au_audio_unit, selector, [av, no_component_error, out](CpuState& s, GuestMemory&) {
            if (s.x[out]) av->bridge->store_error(s.x[out], no_component_error(), true);
            av->release(s.x[0]);
            s.x[0] = 0;
        }, "@@:{AudioComponentDescription=IIIII}^@");
    }
    for (const auto* selector : {"allocateRenderResourcesAndReturnError:", "deallocateRenderResources", "outputBusses", "inputBusses", "setDeviceID:error:",
                                 "setInputEnabled:", "setInputHandler:", "setOutputEnabled:", "startHardwareAndReturnError:", "stopHardware",
                                 "renderBlock", "parameterTree", "componentDescription", "audioUnitName"}) {
        const std::string name = std::string("-[AUAudioUnit ") + selector + "]";
        support.method(au_audio_unit, selector, [name](CpuState&, GuestMemory&) { cannot_exist(name); }, "@@:");
    }
    for (const auto* selector : {"AUAudioUnit", "audioComponentDescription", "audioUnit", "name", "manufacturerName", "version"}) {
        const std::string name = std::string("-[AVAudioUnit ") + selector + "]";
        support.method(audio_unit, selector, [name](CpuState&, GuestMemory&) { cannot_exist(name); }, "@@:");
    }
    for (const auto* selector : {"audioComponentDescription", "audioComponent", "name", "typeName", "manufacturerName"}) {
        const std::string name = std::string("-[AVAudioUnitComponent ") + selector + "]";
        support.method(component, selector, [name](CpuState&, GuestMemory&) { cannot_exist(name); }, "@@:");
    }
    auto shared_manager = std::make_shared<Address>(0);
    support.method(manager, "sharedAudioUnitComponentManager", [av, manager, shared_manager](CpuState& s, GuestMemory&) {
        if (!*shared_manager) *shared_manager = av->alloc_init(manager);   // kept for the process
        s.x[0] = *shared_manager;
    }, "@@:", true);
    for (const auto* selector : {"componentsMatchingDescription:", "componentsMatchingPredicate:", "componentsPassingTest:", "tagNames", "standardLocalizedTagNames"})
        support.method(manager, selector, [av](CpuState& s, GuestMemory&) { s.x[0] = av->empty_array(); }, "@@:@");

    // AUAudioUnitBus: a value class (format, enabled, name, channel limit).
    support.method(bus, "initWithFormat:error:", [av](CpuState& s, GuestMemory&) {
        if (!s.x[2]) {
            av->store_error(s.x[3], -10868, "A bus needs a format", "NSOSStatusErrorDomain");   // kAudioUnitErr_FormatNotSupported
            av->release(s.x[0]);
            s.x[0] = 0;
            return;
        }
        av->state<BusState>(s.x[0])->format = av->retain(s.x[2]);
    }, "@@:@^@");
    support.method(bus, "dealloc", [av, bus](CpuState& s, GuestMemory&) {
        if (const auto st = av->support.objc.find_host_state<BusState>(s.x[0])) { av->release(std::exchange(st->format, 0)); av->release(std::exchange(st->name, 0)); }
        static_cast<void>(av->support.objc.send_super(s.x[0], bus, av->support.objc.selector("dealloc")));
    }, "v@:");
    support.method(bus, "format", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<BusState>(s.x[0])->format; }, "@@:");
    support.method(bus, "setFormat:error:", [av](CpuState& s, GuestMemory&) {
        auto st = av->state<BusState>(s.x[0]);
        const auto channels = s.x[2] ? av->support.send(s.x[2], "channelCount").x0 & 0xffffffffU : 0;
        if (!s.x[2] || (st->maximum_channels && channels > st->maximum_channels)) {
            av->store_error(s.x[3], -10868, "The format is not supported by the bus", "NSOSStatusErrorDomain");
            s.x[0] = 0;
            return;
        }
        const auto old = std::exchange(st->format, av->retain(s.x[2]));
        av->release(old);
        s.x[0] = 1;
    }, "B@:@^@");
    support.method(bus, "isEnabled", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<BusState>(s.x[0])->enabled; }, "B@:");
    support.method(bus, "setEnabled:", [av](CpuState& s, GuestMemory&) { av->state<BusState>(s.x[0])->enabled = (s.x[2] & 0xff) != 0; }, "v@:B");
    support.method(bus, "name", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<BusState>(s.x[0])->name; }, "@@:");
    support.method(bus, "setName:", [av](CpuState& s, GuestMemory&) {
        auto st = av->state<BusState>(s.x[0]);
        const auto old = std::exchange(st->name, s.x[2] ? av->support.send_id(s.x[2], "copy") : 0);
        av->release(old);
    }, "v@:@");
    support.method(bus, "maximumChannelCount", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<BusState>(s.x[0])->maximum_channels; }, "I@:");
    support.method(bus, "setMaximumChannelCount:", [av](CpuState& s, GuestMemory&) { av->state<BusState>(s.x[0])->maximum_channels = static_cast<std::uint32_t>(s.x[2]); }, "v@:I");
    support.method(bus, "shouldAllocateBuffer", [av](CpuState& s, GuestMemory&) { s.x[0] = av->state<BusState>(s.x[0])->allocate; }, "B@:");
    support.method(bus, "setShouldAllocateBuffer:", [av](CpuState& s, GuestMemory&) { av->state<BusState>(s.x[0])->allocate = (s.x[2] & 0xff) != 0; }, "v@:B");

    // ---- AUGenericView (CoreAudioKit): needs an audio unit -------------------------------------------
    if (const auto view = support.objc.find_class("NSView")) {
        static_cast<void>(view);
        const auto generic = support.declare_class("AUGenericView", "NSView", core_audio_kit);
        for (const auto& [selector, types] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"initWithAudioUnit:", "@@:^{ComponentInstanceRecord=}"}, {"initWithAudioUnit:displayFlags:", "@@:^{ComponentInstanceRecord=}I"}}) {
            const std::string name = std::string("-[AUGenericView ") + selector + "]";
            support.method(generic, selector, [av, name](CpuState& s, GuestMemory&) {
                if (s.x[2]) cannot_exist(name + " with an AudioUnit");
                av->release(s.x[0]);   // a NULL unit makes no view
                s.x[0] = 0;
            }, types);
        }
        for (const auto* selector : {"setShowsExpertParameters:", "showsExpertParameters", "audioUnit"}) {
            const std::string name = std::string("-[AUGenericView ") + selector + "]";
            support.method(generic, selector, [name](CpuState&, GuestMemory&) { cannot_exist(name); }, "v@:B");
        }
    }
}

} // namespace alchemy::gui::completion_kit::media
