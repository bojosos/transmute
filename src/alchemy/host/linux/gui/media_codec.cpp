// Linux host backend, area "media_codec" (include/alchemy/host/media_codec.hpp), over GStreamer and only the plugins
// the system already has: nothing is shipped with Alchemy, nothing is downloaded. libgstreamer, libgstvideo and
// libgstaudio are opened at run time (dlopen); only their headers are used to build. Without them (or built without
// their headers) every call answers Error::unsupported, as Windows does when Media Foundation does not start; with
// them, a codec without a plugin is listed empty and fails to open with Error::not_found, as on Windows without that
// Media Foundation transform.
// - Components are the element factories of the registry that encode raw video (or audio) to H.264, HEVC or AAC, or
//   decode them to raw (encoders of any rank, decoders of marginal rank or more), the highest rank first. Their ids are the factory names (x264enc,
//   avdec_h264, ...); "hardware" is the factory's Hardware class.
// - Each encoder or decoder is a short chain of elements (a converter, the codec, a parser where the codec does not
//   take or give Annex B in whole access units) driven from the calling thread: our own source pad pushes into the
//   first element and our own sink pad collects what the last one gives, so a forced keyframe or a drain is ordered
//   with the pictures around it. A drain is an end of stream; the next input flushes the chain and starts again.
// - Video: NV12 pictures in (videoconvert turns them into what the encoder takes), H.264 or HEVC Annex B out, split
//   into NAL units as on Windows: parameter sets kept aside, access-unit delimiters dropped, the rest length-prefixed.
//   Profile and level are asked of the encoder through the caps it negotiates with us and, for the encoders known
//   below, through their own properties. Decoding is the reverse, to NV12 or P010 (videoconvert).
// - AAC: 16-bit interleaved PCM in (audioconvert), raw AAC frames out with the AudioSpecificConfig from the caps'
//   codec_data; decoding to 16-bit interleaved PCM.
#include "alchemy/host/media_codec.hpp"

#if !defined(ALCHEMY_HOST_GSTREAMER)

namespace alchemy::host::media_codec {

namespace {
const Component none{};
} // namespace

// Built without GStreamer's headers: no host codecs.
Result<std::vector<Component>> encoders(Codec) { return Error::unsupported; }
Result<VideoEncoder*> open_video(const VideoSettings&) { return Error::unsupported; }
const Component& component(const VideoEncoder*) noexcept { return none; }
Status encode(VideoEncoder*, const Picture&, bool, std::vector<Sample>&) { return Error::unsupported; }
Status drain(VideoEncoder*, std::vector<Sample>&) { return Error::unsupported; }
std::vector<std::vector<std::uint8_t>> parameter_sets(const VideoEncoder*) { return {}; }
void close(VideoEncoder*) noexcept {}
Result<AudioEncoder*> open_audio(const AudioSettings&) { return Error::unsupported; }
const Component& component(const AudioEncoder*) noexcept { return none; }
std::uint32_t bit_rate(const AudioEncoder*) noexcept { return 0; }
std::uint32_t frames_per_packet(const AudioEncoder*) noexcept { return 0; }
std::vector<std::uint8_t> configuration(const AudioEncoder*) { return {}; }
Status encode(AudioEncoder*, std::span<const std::int16_t>, std::vector<Sample>&) { return Error::unsupported; }
Status drain(AudioEncoder*, std::vector<Sample>&) { return Error::unsupported; }
void close(AudioEncoder*) noexcept {}
Result<std::uint32_t> measure_delay(const AudioSettings&) { return Error::unsupported; }
Result<std::vector<Component>> decoders(Codec) { return Error::unsupported; }
Result<VideoDecoder*> open_video_decoder(const VideoDecoderSettings&) { return Error::unsupported; }
const Component& component(const VideoDecoder*) noexcept { return none; }
Status decode(VideoDecoder*, const Sample&, std::vector<DecodedPicture>&) { return Error::unsupported; }
Status drain(VideoDecoder*, std::vector<DecodedPicture>&) { return Error::unsupported; }
void flush(VideoDecoder*) noexcept {}
void close(VideoDecoder*) noexcept {}
Result<AudioDecoder*> open_audio_decoder(const AudioDecoderSettings&) { return Error::unsupported; }
const Component& component(const AudioDecoder*) noexcept { return none; }
Status decode(AudioDecoder*, std::span<const std::uint8_t>, std::vector<std::int16_t>&) { return Error::unsupported; }
Status drain(AudioDecoder*, std::vector<std::int16_t>&) { return Error::unsupported; }
void flush(AudioDecoder*) noexcept {}
void close(AudioDecoder*) noexcept {}

} // namespace alchemy::host::media_codec

#else

#include <gst/audio/audio.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>

namespace alchemy::host::media_codec {

namespace {

// ---- GStreamer, opened at run time -----------------------------------------------------------------------
// Only functions are called (never the headers' inline helpers, which would need the libraries at link time).

#define ALCHEMY_GST_CORE(X)                                                                                              \
    X(gst_init_check) X(gst_element_factory_list_get_elements) X(gst_element_factory_list_filter)                       \
    X(gst_plugin_feature_list_free) X(gst_element_factory_get_metadata) X(gst_plugin_feature_get_rank)                  \
    X(gst_element_factory_get_static_pad_templates) X(gst_static_pad_template_get_caps) X(gst_element_factory_find)     \
    X(gst_element_factory_create) X(gst_element_factory_make) X(gst_object_ref) X(gst_object_ref_sink)                   \
    X(gst_object_unref) X(gst_element_get_static_pad) X(gst_element_link) X(gst_element_set_state) X(gst_pad_new)         \
    X(gst_pad_set_active) X(gst_pad_link_full) X(gst_pad_unlink) X(gst_pad_set_chain_function_full)                      \
    X(gst_pad_set_event_function_full) X(gst_pad_set_query_function_full) X(gst_pad_set_element_private)                 \
    X(gst_pad_get_element_private) X(gst_pad_push) X(gst_pad_push_event) X(gst_pad_query_default)                       \
    X(gst_pad_peer_query_caps) X(gst_query_parse_caps) X(gst_query_set_caps_result) X(gst_query_parse_accept_caps)       \
    X(gst_query_set_accept_caps_result) X(gst_event_new_stream_start) X(gst_event_new_caps) X(gst_event_new_segment)     \
    X(gst_segment_init) X(gst_event_new_eos) X(gst_event_new_flush_start) X(gst_event_new_flush_stop)                    \
    X(gst_event_parse_caps) X(gst_event_copy_segment) X(gst_segment_to_stream_time_full) X(gst_caps_from_string) X(gst_caps_can_intersect) X(gst_caps_intersect_full)                 \
    X(gst_caps_get_structure) X(gst_caps_get_size) X(gst_caps_is_empty) X(gst_structure_get_value)                       \
    X(gst_mini_object_ref) X(gst_mini_object_unref) X(gst_buffer_new_allocate) X(gst_buffer_fill) X(gst_buffer_map)      \
    X(gst_buffer_unmap) X(gst_util_set_object_arg) X(g_object_class_find_property) X(g_value_get_boxed)
#define ALCHEMY_GST_VIDEO(X)                                                                                             \
    X(gst_video_info_set_format) X(gst_video_info_from_caps) X(gst_video_frame_map) X(gst_video_frame_unmap)             \
    X(gst_video_event_new_downstream_force_key_unit)
#define ALCHEMY_GST_AUDIO(X) X(gst_audio_channel_get_fallback_mask)

struct Api {
#define ALCHEMY_GST_MEMBER(name) decltype(&::name) name{};
    ALCHEMY_GST_CORE(ALCHEMY_GST_MEMBER)
    ALCHEMY_GST_VIDEO(ALCHEMY_GST_MEMBER)
    ALCHEMY_GST_AUDIO(ALCHEMY_GST_MEMBER)
#undef ALCHEMY_GST_MEMBER
};

// The libraries opened and GStreamer initialized, once; null when the system has no GStreamer.
const Api* load() {
    void* core = ::dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
    void* video = core ? ::dlopen("libgstvideo-1.0.so.0", RTLD_NOW | RTLD_LOCAL) : nullptr;
    void* audio = video ? ::dlopen("libgstaudio-1.0.so.0", RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!audio) return nullptr;
    static Api api;
    bool ok = true;
    // dlsym on a library's handle also finds its dependencies' symbols (GObject's).
#define ALCHEMY_GST_LOAD(handle)                                                                                         \
    [&](auto& member, const char* name) {                                                                                \
        member = reinterpret_cast<std::remove_reference_t<decltype(member)>>(::dlsym(handle, name));                     \
        ok = ok && member;                                                                                               \
    }
    const auto from_core = ALCHEMY_GST_LOAD(core);
    const auto from_video = ALCHEMY_GST_LOAD(video);
    const auto from_audio = ALCHEMY_GST_LOAD(audio);
#undef ALCHEMY_GST_LOAD
#define ALCHEMY_GST_FROM_CORE(name) from_core(api.name, #name);
#define ALCHEMY_GST_FROM_VIDEO(name) from_video(api.name, #name);
#define ALCHEMY_GST_FROM_AUDIO(name) from_audio(api.name, #name);
    ALCHEMY_GST_CORE(ALCHEMY_GST_FROM_CORE)
    ALCHEMY_GST_VIDEO(ALCHEMY_GST_FROM_VIDEO)
    ALCHEMY_GST_AUDIO(ALCHEMY_GST_FROM_AUDIO)
#undef ALCHEMY_GST_FROM_CORE
#undef ALCHEMY_GST_FROM_VIDEO
#undef ALCHEMY_GST_FROM_AUDIO
    if (!ok || !api.gst_init_check(nullptr, nullptr, nullptr)) return nullptr;
    return &api;
}

const Api* gst() {
    static const Api* api = load();
    return api;
}

// References held by this layer.
template <class T> void unref(T*& object) {
    if (object) gst()->gst_mini_object_unref(GST_MINI_OBJECT_CAST(object));
    object = nullptr;
}
void unref_object(gpointer object) {
    if (object) gst()->gst_object_unref(object);
}

std::int64_t to_gst(std::int64_t hns) { return hns * 100; }
std::int64_t duration_of(GstClockTime ns) { return GST_CLOCK_TIME_IS_VALID(ns) ? static_cast<std::int64_t>(ns / 100) : 0; }

// ---- components ---------------------------------------------------------------------------------------------

// The caps of a codec: Annex B in whole access units (video), raw frames (AAC).
const char* coded_caps(Codec codec) {
    switch (codec) {
    case Codec::h264: return "video/x-h264, stream-format=(string)byte-stream, alignment=(string)au";
    case Codec::hevc: return "video/x-h265, stream-format=(string)byte-stream, alignment=(string)au";
    case Codec::aac: break;
    }
    return "audio/mpeg, mpegversion=(int)4, stream-format=(string)raw";
}
const char* any_coded_caps(Codec codec) {
    return codec == Codec::h264 ? "video/x-h264" : codec == Codec::hevc ? "video/x-h265" : "audio/mpeg, mpegversion=(int)4";
}
const char* parser_of(Codec codec) { return codec == Codec::h264 ? "h264parse" : codec == Codec::hevc ? "h265parse" : "aacparse"; }

// The audio decoders' held-back frames (what comes out only at the drain of a decoded stream, counted by the host GUI
// test) and the audio encoders' delays (the frames their output starts with), by factory name, as measure_delay
// measured them with the system's decoder. The host GUI test measures them again and fails if they differ. A
// component not listed has an unknown delay (0) and is not used for files or AudioConverter.
struct KnownDelay { const char* factory; std::uint32_t frames; };
constexpr KnownDelay known_delays[] = {
    {"voaacenc", 1600},    // VisualOn's AAC encoder (48 kHz stereo, GStreamer 1.28.2, Ubuntu 26.04, 2026-11-10)
    {"avenc_aac", 1024},   // FFmpeg's AAC encoder through gst-libav (48 kHz stereo, Ubuntu 26.04, 2026-11-10)
    {"avdec_aac", 0},      // FFmpeg's AAC decoder through gst-libav: holds nothing back (Ubuntu 26.04, 2026-11-10)
};

// The first static pad template of a factory in `direction`, its caps (a reference) or null.
GstCaps* template_caps(GstElementFactory* factory, GstPadDirection direction) {
    for (const GList* l = gst()->gst_element_factory_get_static_pad_templates(factory); l; l = l->next) {
        auto* t = static_cast<GstStaticPadTemplate*>(l->data);
        if (t->direction == direction) return gst()->gst_static_pad_template_get_caps(t);
    }
    return nullptr;
}

bool template_takes(GstElementFactory* factory, GstPadDirection direction, const char* caps_text) {
    GstCaps* t = template_caps(factory, direction);
    GstCaps* c = gst()->gst_caps_from_string(caps_text);
    const bool takes = t && c && gst()->gst_caps_can_intersect(t, c);
    unref(t);
    unref(c);
    return takes;
}

bool have_element(const char* name) {
    GstElementFactory* f = gst()->gst_element_factory_find(name);
    unref_object(f);
    return f != nullptr;
}

struct Found {
    GstElementFactory* factory{};   // a reference
    Component component;
    Found() = default;
    Found(const Found&) = delete;
    Found& operator=(const Found&) = delete;
    Found(Found&& o) noexcept : factory(std::exchange(o.factory, nullptr)), component(std::move(o.component)) {}
    Found& operator=(Found&& o) noexcept {
        if (this != &o) { unref_object(factory); factory = std::exchange(o.factory, nullptr); component = std::move(o.component); }
        return *this;
    }
    ~Found() { unref_object(factory); }
};

// The encoders (or decoders) of `codec`, the highest rank first. Empty without the converter the chains need
// (videoconvert, audioconvert: GStreamer's base plugins).
std::vector<Found> enumerate(Codec codec, Direction direction) {
    std::vector<Found> out;
    const bool audio = codec == Codec::aac, decode = direction == Direction::decode;
    if (!have_element(audio ? "audioconvert" : "videoconvert")) return out;
    const auto type = (decode ? GST_ELEMENT_FACTORY_TYPE_DECODER : GST_ELEMENT_FACTORY_TYPE_ENCODER) |
                      (audio ? GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO : GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO);
    // As GStreamer's own bins choose: encoders of any rank (encodebin), decoders of marginal rank or more (decodebin).
    GList* all = gst()->gst_element_factory_list_get_elements(type, decode ? GST_RANK_MARGINAL : GST_RANK_NONE);
    GstCaps* coded = gst()->gst_caps_from_string(any_coded_caps(codec));
    GList* coding = gst()->gst_element_factory_list_filter(all, coded, decode ? GST_PAD_SINK : GST_PAD_SRC, FALSE);
    unref(coded);
    const char* raw = audio ? "audio/x-raw" : "video/x-raw";
    for (GList* l = coding; l; l = l->next) {
        auto* factory = static_cast<GstElementFactory*>(l->data);
        if (!template_takes(factory, decode ? GST_PAD_SRC : GST_PAD_SINK, raw)) continue;
        Found f;
        f.factory = static_cast<GstElementFactory*>(gst()->gst_object_ref(factory));
        f.component.id = GST_OBJECT_NAME(factory);
        const char* long_name = gst()->gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_LONGNAME);
        f.component.name = long_name ? long_name : f.component.id;
        f.component.codec = codec;
        f.component.direction = direction;
        const char* klass = gst()->gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
        f.component.hardware = klass && std::strstr(klass, GST_ELEMENT_FACTORY_KLASS_HARDWARE);
        for (const auto& known : known_delays)
            if (f.component.id == known.factory) f.component.delay = known.frames;
        out.push_back(std::move(f));
    }
    gst()->gst_plugin_feature_list_free(coding);
    gst()->gst_plugin_feature_list_free(all);
    std::stable_sort(out.begin(), out.end(), [](const Found& a, const Found& b) {
        return gst()->gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE_CAST(a.factory)) >
               gst()->gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE_CAST(b.factory));
    });
    return out;
}

GstElement* make(const char* factory) {
    GstElement* e = gst()->gst_element_factory_make(factory, nullptr);
    return e ? static_cast<GstElement*>(gst()->gst_object_ref_sink(e)) : nullptr;
}
GstElement* make(GstElementFactory* factory) {
    GstElement* e = gst()->gst_element_factory_create(factory, nullptr);
    return e ? static_cast<GstElement*>(gst()->gst_object_ref_sink(e)) : nullptr;
}

GParamSpec* property(GstElement* e, const char* name) {
    return gst()->g_object_class_find_property(G_OBJECT_GET_CLASS(e), name);
}
// Sets a property from its text (enums and flags by nick) when the element has it.
bool set(GstElement* e, const char* name, const std::string& value) {
    if (!property(e, name)) return false;
    gst()->gst_util_set_object_arg(reinterpret_cast<GObject*>(e), name, value.c_str());
    return true;
}
// An integer property's range and default, if it is one.
struct Range { std::int64_t min{}, max{}, value{}; };
bool int_range(GstElement* e, const char* name, Range& out) {
    GParamSpec* p = property(e, name);
    if (!p) return false;
    if (p->value_type == G_TYPE_INT) {
        const auto* i = reinterpret_cast<GParamSpecInt*>(p);
        out = {i->minimum, i->maximum, i->default_value};
        return true;
    }
    if (p->value_type == G_TYPE_UINT) {
        const auto* u = reinterpret_cast<GParamSpecUInt*>(p);
        out = {u->minimum, u->maximum, u->default_value};
        return true;
    }
    return false;
}

std::string hex(std::span<const std::uint8_t> bytes) {
    std::string out;
    char digits[3];
    for (const auto b : bytes) {
        std::snprintf(digits, sizeof digits, "%02x", b);
        out += digits;
    }
    return out;
}

std::vector<std::uint8_t> bytes_of(GstBuffer* buffer) {
    std::vector<std::uint8_t> out;
    GstMapInfo map{};
    if (buffer && gst()->gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        out.assign(map.data, map.data + map.size);
        gst()->gst_buffer_unmap(buffer, &map);
    }
    return out;
}

GstBuffer* buffer_of(std::span<const std::uint8_t> bytes) {
    GstBuffer* b = gst()->gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
    if (b && !bytes.empty()) gst()->gst_buffer_fill(b, 0, bytes.data(), bytes.size());
    return b;
}

// ---- a chain of elements driven from the caller -------------------------------------------------------------

class Chain {
public:
    // Called (under the chain's lock) for each buffer the last element gives, with the caps it came with.
    using Take = std::function<void(GstBuffer*, GstCaps*)>;

    Chain() = default;
    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;
    ~Chain() { stop(); }

    // Takes the elements (references); links them between our pads, starts them and sends the input caps.
    // Error::invalid_argument: the chain does not take `input` or cannot give `output`.
    Error start(std::vector<GstElement*> elements, const std::string& input, const std::string& output, Take take) {
        const auto* g = gst();
        elements_ = std::move(elements);
        take_ = std::move(take);
        for (auto* e : elements_)
            if (!e) return Error::not_found;
        g->gst_segment_init(&segment_, GST_FORMAT_TIME);
        accept_ = g->gst_caps_from_string(output.c_str());
        GstCaps* input_caps = g->gst_caps_from_string(input.c_str());
        if (!accept_ || !input_caps) { unref(input_caps); return Error::invalid_argument; }
        src_ = static_cast<GstPad*>(g->gst_object_ref_sink(g->gst_pad_new("src", GST_PAD_SRC)));
        sink_ = static_cast<GstPad*>(g->gst_object_ref_sink(g->gst_pad_new("sink", GST_PAD_SINK)));
        g->gst_pad_set_element_private(sink_, this);
        g->gst_pad_set_chain_function_full(sink_, on_chain, nullptr, nullptr);
        g->gst_pad_set_event_function_full(sink_, on_event, nullptr, nullptr);
        g->gst_pad_set_query_function_full(sink_, on_query, nullptr, nullptr);
        for (std::size_t i = 0; i + 1 < elements_.size(); ++i)
            if (!g->gst_element_link(elements_[i], elements_[i + 1])) { unref(input_caps); return Error::invalid_argument; }
        GstPad* first = g->gst_element_get_static_pad(elements_.front(), "sink");
        GstPad* last = g->gst_element_get_static_pad(elements_.back(), "src");
        const bool linked = first && last && g->gst_pad_link_full(src_, first, GST_PAD_LINK_CHECK_NOTHING) == GST_PAD_LINK_OK &&
                            g->gst_pad_link_full(last, sink_, GST_PAD_LINK_CHECK_NOTHING) == GST_PAD_LINK_OK;
        unref_object(first);
        unref_object(last);
        if (!linked) { unref(input_caps); return Error::invalid_argument; }
        g->gst_pad_set_active(sink_, TRUE);
        g->gst_pad_set_active(src_, TRUE);
        for (auto it = elements_.rbegin(); it != elements_.rend(); ++it)
            if (g->gst_element_set_state(*it, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) { unref(input_caps); return Error::io; }
        started_ = true;
        g->gst_pad_push_event(src_, g->gst_event_new_stream_start("alchemy"));
        const bool taken = g->gst_pad_push_event(src_, g->gst_event_new_caps(input_caps));
        unref(input_caps);
        if (!taken) return Error::invalid_argument;
        segment();
        // What the chain can give for that input, against what we take.
        GstCaps* can = g->gst_pad_peer_query_caps(sink_, accept_);
        const bool gives = can && !g->gst_caps_is_empty(can);
        unref(can);
        return gives ? Error::none : Error::invalid_argument;
    }

    // A time the last element gave, back in the input's terms (100 ns units): through the segment it sent, since an
    // encoder may offset its output (GstVideoEncoder starts at 1000 hours, so decode times never go negative).
    // Called from Take (the lock held). `fallback` when the time is not valid.
    std::int64_t time(GstClockTime t, std::int64_t fallback = 0) const {
        if (!GST_CLOCK_TIME_IS_VALID(t)) return fallback;
        guint64 stream{};
        const gint sign = gst()->gst_segment_to_stream_time_full(&segment_, GST_FORMAT_TIME, t, &stream);
        if (sign == 0) return fallback;
        const auto hns = static_cast<std::int64_t>(stream / 100);
        return sign > 0 ? hns : -hns;
    }

    // An event pushed in order with the buffers (takes it).
    bool event(GstEvent* e) {
        restart_if_drained();
        return gst()->gst_pad_push_event(src_, e);
    }

    // Takes the buffer.
    Error push(GstBuffer* buffer) {
        restart_if_drained();
        const auto flow = gst()->gst_pad_push(src_, buffer);
        return flow == GST_FLOW_OK ? Error::none : Error::io;
    }

    // The end of the stream, through the whole chain: everything held comes out.
    Error drain() {
        if (drained_) return Error::none;
        {
            std::lock_guard lock(mutex_);
            eos_ = false;
        }
        if (!gst()->gst_pad_push_event(src_, gst()->gst_event_new_eos())) return Error::io;
        std::unique_lock lock(mutex_);
        const bool done = cv_.wait_for(lock, std::chrono::seconds(30), [this] { return eos_; });
        drained_ = true;
        return done ? Error::none : Error::timed_out;
    }

    // Forgets what the elements hold.
    void flush() {
        if (!started_) return;
        gst()->gst_pad_push_event(src_, gst()->gst_event_new_flush_start());
        gst()->gst_pad_push_event(src_, gst()->gst_event_new_flush_stop(TRUE));
        drained_ = false;
        segment();
    }

    void stop() {
        const auto* g = gst();
        if (!g) return;
        for (auto* e : elements_)
            if (e) g->gst_element_set_state(e, GST_STATE_NULL);
        if (src_ && GST_PAD_PEER(src_)) g->gst_pad_unlink(src_, GST_PAD_PEER(src_));
        if (sink_ && GST_PAD_PEER(sink_)) g->gst_pad_unlink(GST_PAD_PEER(sink_), sink_);
        if (src_) g->gst_pad_set_active(src_, FALSE);
        if (sink_) g->gst_pad_set_active(sink_, FALSE);
        for (auto*& e : elements_) { unref_object(e); e = nullptr; }
        elements_.clear();
        unref_object(src_);
        unref_object(sink_);
        src_ = sink_ = nullptr;
        unref(accept_);
        unref(caps_);
        started_ = false;
    }

    std::mutex mutex_;   // held while Take runs

private:
    void segment() {
        GstSegment s;
        gst()->gst_segment_init(&s, GST_FORMAT_TIME);
        gst()->gst_pad_push_event(src_, gst()->gst_event_new_segment(&s));
    }
    void restart_if_drained() {
        if (drained_) flush();
    }

    static Chain* of(GstPad* pad) { return static_cast<Chain*>(gst()->gst_pad_get_element_private(pad)); }
    static GstFlowReturn on_chain(GstPad* pad, GstObject*, GstBuffer* buffer) {
        Chain* c = of(pad);
        {
            std::lock_guard lock(c->mutex_);
            if (c->take_) c->take_(buffer, c->caps_);
        }
        unref(buffer);
        return GST_FLOW_OK;
    }
    static gboolean on_event(GstPad* pad, GstObject*, GstEvent* event) {
        Chain* c = of(pad);
        if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
            GstCaps* caps = nullptr;
            gst()->gst_event_parse_caps(event, &caps);
            std::lock_guard lock(c->mutex_);
            unref(c->caps_);
            c->caps_ = caps ? reinterpret_cast<GstCaps*>(gst()->gst_mini_object_ref(GST_MINI_OBJECT_CAST(caps))) : nullptr;
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            std::lock_guard lock(c->mutex_);
            gst()->gst_event_copy_segment(event, &c->segment_);
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
            {
                std::lock_guard lock(c->mutex_);
                c->eos_ = true;
            }
            c->cv_.notify_all();
        }
        unref(event);
        return TRUE;
    }
    static gboolean on_query(GstPad* pad, GstObject* parent, GstQuery* query) {
        Chain* c = of(pad);
        if (GST_QUERY_TYPE(query) == GST_QUERY_CAPS) {
            GstCaps* filter = nullptr;
            gst()->gst_query_parse_caps(query, &filter);
            GstCaps* result = filter ? gst()->gst_caps_intersect_full(filter, c->accept_, GST_CAPS_INTERSECT_FIRST)
                                     : reinterpret_cast<GstCaps*>(gst()->gst_mini_object_ref(GST_MINI_OBJECT_CAST(c->accept_)));
            gst()->gst_query_set_caps_result(query, result);
            unref(result);
            return TRUE;
        }
        if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
            GstCaps* caps = nullptr;
            gst()->gst_query_parse_accept_caps(query, &caps);
            gst()->gst_query_set_accept_caps_result(query, caps && gst()->gst_caps_can_intersect(caps, c->accept_));
            return TRUE;
        }
        return gst()->gst_pad_query_default(pad, parent, query);
    }

    std::vector<GstElement*> elements_;
    GstPad* src_{};
    GstPad* sink_{};
    GstCaps* accept_{};
    GstCaps* caps_{};
    GstSegment segment_{};   // the last element's
    Take take_;
    std::condition_variable cv_;
    bool eos_{}, drained_{}, started_{};
};

// The NAL units of an Annex B stream (start codes removed, trailing zero bytes trimmed).
std::vector<std::pair<std::size_t, std::size_t>> nal_units(const std::vector<std::uint8_t>& b) {
    std::vector<std::pair<std::size_t, std::size_t>> units;
    std::size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= b.size()) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (start != SIZE_MAX) units.emplace_back(start, i);
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX) units.emplace_back(start, b.size());
    for (auto& [from, to] : units)
        while (to > from && b[to - 1] == 0) --to;
    return units;
}

// Annex B: a start code before each NAL unit of a length-prefixed sample.
void append_annex_b(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> sample) {
    for (std::size_t at = 0; at + 4 <= sample.size();) {
        const std::size_t n = (std::size_t(sample[at]) << 24) | (std::size_t(sample[at + 1]) << 16) | (std::size_t(sample[at + 2]) << 8) | sample[at + 3];
        at += 4;
        if (n > sample.size() - at) break;
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), sample.begin() + static_cast<std::ptrdiff_t>(at), sample.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
}

// "3.1" for a level_idc (H.264: ten times the level; HEVC: thirty times), as GStreamer's caps name levels.
std::string level_name(Codec codec, std::uint32_t idc) {
    const std::uint32_t tenths = codec == Codec::hevc ? idc / 3 : idc;
    return tenths % 10 ? std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) : std::to_string(tenths / 10);
}

// What the encoders known here call their settings (each element's own properties, from its documentation); other
// encoders get the caps alone.
struct Tuning {
    const char* factory;
    const char* bit_rate;          // property
    std::uint32_t bit_rate_unit;   // bits per second per unit
    const char* keyframe_interval; // property
    const char* b_frames;          // property (null: through the options)
    const char* options;           // the encoder library's option string property (null: none)
    const char* level_option;      // its name for the level there
    const char* b_frames_option;   // its name for the B-frame count there
    const char* tune;              // property for low latency ("zerolatency")
};
constexpr Tuning tunings[] = {
    {"x264enc", "bitrate", 1000, "key-int-max", "bframes", "option-string", "level", nullptr, "tune"},
    {"x265enc", "bitrate", 1000, "key-int-max", nullptr, "option-string", "level-idc", "bframes", "tune"},
    {"openh264enc", "bitrate", 1, "gop-size", nullptr, nullptr, nullptr, nullptr, nullptr},
};

} // namespace

// ---- video ------------------------------------------------------------------------------------------

class VideoEncoder {
public:
    Chain chain;
    Component component;
    VideoSettings settings;
    std::vector<std::vector<std::uint8_t>> parameter_sets;
    std::vector<Sample> ready;
    GstVideoInfo input{};

    // Splits an output buffer: parameter sets aside, delimiters dropped, the rest length-prefixed.
    void take(GstBuffer* out) {
        Sample s;
        s.pts = chain.time(GST_BUFFER_PTS(out));
        s.dts = chain.time(GST_BUFFER_DTS(out), s.pts);
        s.duration = duration_of(GST_BUFFER_DURATION(out));
        s.keyframe = !GST_BUFFER_FLAG_IS_SET(out, GST_BUFFER_FLAG_DELTA_UNIT);
        const auto bytes = bytes_of(out);
        const bool hevc = settings.codec == Codec::hevc;
        std::vector<std::vector<std::uint8_t>> sets;
        for (const auto& [from, to] : nal_units(bytes)) {
            if (to <= from) continue;
            const unsigned type = hevc ? (bytes[from] >> 1) & 0x3f : bytes[from] & 0x1f;
            const bool parameter = hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
            const bool delimiter = hevc ? type == 35 : type == 9;
            if (parameter) {
                sets.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(to));
                continue;
            }
            if (delimiter) continue;
            const auto n = static_cast<std::uint32_t>(to - from);
            for (int k = 3; k >= 0; --k) s.data.push_back(static_cast<std::uint8_t>(n >> (8 * k)));
            s.data.insert(s.data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.begin() + static_cast<std::ptrdiff_t>(to));
        }
        // The latest of each kind, in stream order: VPS, SPS, PPS (HEVC) or SPS, PPS (H.264).
        const auto key = [hevc](const std::vector<std::uint8_t>& u) { return hevc ? (u[0] >> 1) & 0x3f : u[0] & 0x1f; };
        for (auto& set : sets) {
            const auto same = std::find_if(parameter_sets.begin(), parameter_sets.end(), [&](const auto& kept) { return key(kept) == key(set); });
            if (same != parameter_sets.end()) *same = std::move(set);
            else parameter_sets.push_back(std::move(set));
        }
        std::stable_sort(parameter_sets.begin(), parameter_sets.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
        // Buffers of parameter sets alone (headers an encoder sends ahead of a keyframe) are no sample.
        if (!s.data.empty()) ready.push_back(std::move(s));
    }
};

Result<std::vector<Component>> encoders(Codec codec) {
    if (!gst()) return Error::unsupported;
    std::vector<Component> out;
    for (auto& f : enumerate(codec, Direction::encode)) {
        if (codec == Codec::aac) {
            // What it takes, from its sink template: the usual rates and one to eight channels. Its bit rates are a
            // range in GStreamer (the "bitrate" property, bits per second): listed as the usual rates in it, and its
            // default.
            for (const std::uint32_t rate : {8000u, 11025u, 12000u, 16000u, 22050u, 24000u, 32000u, 44100u, 48000u, 64000u, 88200u, 96000u})
                if (template_takes(f.factory, GST_PAD_SINK, ("audio/x-raw, rate=(int)" + std::to_string(rate)).c_str())) f.component.sample_rates.push_back(rate);
            for (std::uint32_t channels = 1; channels <= 8; ++channels)
                if (template_takes(f.factory, GST_PAD_SINK, ("audio/x-raw, channels=(int)" + std::to_string(channels)).c_str())) f.component.channel_counts.push_back(channels);
            GstElement* e = make(f.factory);
            Range r;
            if (e && int_range(e, "bitrate", r)) {
                for (const std::uint32_t bits : {32000u, 48000u, 64000u, 96000u, 128000u, 160000u, 192000u, 256000u, 320000u})
                    if (bits >= r.min && bits <= r.max) f.component.bit_rates.push_back(bits);
                if (r.value >= 8000 && std::find(f.component.bit_rates.begin(), f.component.bit_rates.end(), r.value) == f.component.bit_rates.end())
                    f.component.bit_rates.push_back(static_cast<std::uint32_t>(r.value));
                std::sort(f.component.bit_rates.begin(), f.component.bit_rates.end());
            }
            unref_object(e);
            if (f.component.sample_rates.empty() || f.component.channel_counts.empty() || f.component.bit_rates.empty()) continue;
        }
        out.push_back(std::move(f.component));
    }
    return out;
}

Result<VideoEncoder*> open_video(const VideoSettings& settings) {
    if (settings.codec == Codec::aac) return Error::invalid_argument;
    if (!settings.width || !settings.height || (settings.width & 1) || (settings.height & 1) || !(settings.frame_rate > 0)) return Error::invalid_argument;
    if (!gst()) return Error::unsupported;
    const auto& s = settings;
    const auto rate_num = static_cast<long>(std::lround(s.frame_rate * 1000));
    const std::uint32_t bits = s.bit_rate ? s.bit_rate : static_cast<std::uint32_t>(std::min(100e6, std::max(250e3, double(s.width) * s.height * s.frame_rate * 0.1)));
    const std::string input = "video/x-raw, format=(string)NV12, width=(int)" + std::to_string(s.width) + ", height=(int)" + std::to_string(s.height) +
                              ", framerate=(fraction)" + std::to_string(rate_num) + "/1000, pixel-aspect-ratio=(fraction)1/1, interlace-mode=(string)progressive";
    std::string output = coded_caps(s.codec);
    switch (s.profile) {
    case Profile::h264_baseline: output += ", profile=(string){ constrained-baseline, baseline }"; break;
    case Profile::h264_main: output += ", profile=(string)main"; break;
    case Profile::h264_high: output += ", profile=(string)high"; break;
    case Profile::hevc_main: output += ", profile=(string)main"; break;
    case Profile::hevc_main10: output += ", profile=(string)main-10"; break;
    default: break;
    }
    if (s.level) output += ", level=(string)" + level_name(s.codec, s.level);
    bool any = false;
    for (auto& f : enumerate(s.codec, Direction::encode)) {
        if (!s.component.empty() && f.component.id != s.component) continue;
        if (s.require_hardware && !f.component.hardware) continue;
        any = true;
        GstElement* codec = make(f.factory);
        if (!codec) continue;
        for (const auto& t : tunings) {
            if (f.component.id != t.factory) continue;
            set(codec, t.bit_rate, std::to_string(std::max<std::uint32_t>(1, bits / t.bit_rate_unit)));
            if (s.keyframe_interval) set(codec, t.keyframe_interval, std::to_string(s.keyframe_interval));
            if (t.b_frames) set(codec, t.b_frames, s.frame_reordering ? "2" : "0");
            if (s.real_time && t.tune) set(codec, t.tune, "zerolatency");
            std::string options;
            if (t.b_frames_option) options += std::string(t.b_frames_option) + "=" + (s.frame_reordering ? "2" : "0");
            if (s.level && t.level_option) options += (options.empty() ? "" : ":") + std::string(t.level_option) + "=" + level_name(s.codec, s.level);
            if (t.options && !options.empty()) set(codec, t.options, options);
        }
        // Annex B in whole access units from the encoder itself, or through the codec's parser.
        const bool annex_b = template_takes(f.factory, GST_PAD_SRC, coded_caps(s.codec));
        std::vector<GstElement*> elements{make("videoconvert"), codec};
        if (!annex_b) elements.push_back(make(parser_of(s.codec)));
        auto encoder = std::make_unique<VideoEncoder>();
        encoder->settings = s;
        auto* e = encoder.get();
        if (!gst()->gst_video_info_set_format(&e->input, GST_VIDEO_FORMAT_NV12, s.width, s.height)) { for (auto* x : elements) unref_object(x); return Error::invalid_argument; }
        if (e->chain.start(std::move(elements), input, output, [e](GstBuffer* b, GstCaps*) { e->take(b); }) != Error::none) continue;
        e->component = std::move(f.component);
        return encoder.release();
    }
    return any ? Error::invalid_argument : Error::not_found;
}

const Component& component(const VideoEncoder* encoder) noexcept { return encoder->component; }

Status encode(VideoEncoder* encoder, const Picture& picture, bool keyframe, std::vector<Sample>& out) {
    if (!encoder || picture.width != encoder->settings.width || picture.height != encoder->settings.height || !picture.luma || !picture.chroma)
        return Error::invalid_argument;
    const auto& info = encoder->input;
    GstBuffer* buffer = gst()->gst_buffer_new_allocate(nullptr, GST_VIDEO_INFO_SIZE(&info), nullptr);
    if (!buffer) return Error::no_memory;
    GstMapInfo map{};
    if (!gst()->gst_buffer_map(buffer, &map, GST_MAP_WRITE)) { unref(buffer); return Error::io; }
    for (std::uint32_t y = 0; y < picture.height; ++y)
        std::memcpy(map.data + GST_VIDEO_INFO_PLANE_OFFSET(&info, 0) + std::size_t(y) * GST_VIDEO_INFO_PLANE_STRIDE(&info, 0),
                    picture.luma + y * picture.luma_stride, picture.width);
    for (std::uint32_t y = 0; y < picture.height / 2; ++y)
        std::memcpy(map.data + GST_VIDEO_INFO_PLANE_OFFSET(&info, 1) + std::size_t(y) * GST_VIDEO_INFO_PLANE_STRIDE(&info, 1),
                    picture.chroma + y * picture.chroma_stride, picture.width);
    gst()->gst_buffer_unmap(buffer, &map);
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(to_gst(picture.pts));
    GST_BUFFER_DURATION(buffer) = picture.duration > 0 ? static_cast<GstClockTime>(to_gst(picture.duration)) : GST_CLOCK_TIME_NONE;
    // A keyframe asked for: GStreamer's force-key-unit event, ahead of the picture (with its headers).
    if (keyframe)
        encoder->chain.event(gst()->gst_video_event_new_downstream_force_key_unit(GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, TRUE, 0));
    const auto error = encoder->chain.push(buffer);
    std::lock_guard lock(encoder->chain.mutex_);
    for (auto& s : encoder->ready) out.push_back(std::move(s));
    encoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Status drain(VideoEncoder* encoder, std::vector<Sample>& out) {
    if (!encoder) return Error::invalid_argument;
    const auto error = encoder->chain.drain();
    std::lock_guard lock(encoder->chain.mutex_);
    for (auto& s : encoder->ready) out.push_back(std::move(s));
    encoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

std::vector<std::vector<std::uint8_t>> parameter_sets(const VideoEncoder* encoder) {
    if (!encoder) return {};
    std::lock_guard lock(const_cast<VideoEncoder*>(encoder)->chain.mutex_);
    return encoder->parameter_sets;
}

void close(VideoEncoder* encoder) noexcept { delete encoder; }

// ---- audio ------------------------------------------------------------------------------------------

class AudioEncoder {
public:
    Chain chain;
    Component component;
    AudioSettings settings;
    std::uint32_t bit_rate{};
    std::vector<std::uint8_t> configuration;
    std::uint64_t frames_in{};
    std::vector<Sample> ready;
    void take(GstBuffer* out, GstCaps* caps) {
        note(caps);
        Sample s;
        s.pts = s.dts = chain.time(GST_BUFFER_PTS(out));
        s.duration = duration_of(GST_BUFFER_DURATION(out));
        s.data = bytes_of(out);
        s.keyframe = true;
        if (!s.data.empty()) ready.push_back(std::move(s));
    }
    // The AudioSpecificConfig: the caps' codec_data.
    void note(GstCaps* caps) {
        if (!caps || !gst()->gst_caps_get_size(caps)) return;
        const GValue* v = gst()->gst_structure_get_value(gst()->gst_caps_get_structure(caps, 0), "codec_data");
        if (!v) return;
        auto config = bytes_of(static_cast<GstBuffer*>(gst()->g_value_get_boxed(v)));
        if (config.size() >= 2) configuration = std::move(config);
    }
};

namespace {
std::string raw_audio_caps(std::uint32_t rate, std::uint32_t channels) {
    std::string caps = "audio/x-raw, format=(string)S16LE, layout=(string)interleaved, rate=(int)" + std::to_string(rate) +
                       ", channels=(int)" + std::to_string(channels);
    // More than two channels: GStreamer's default positions, which follow WAVEFORMATEXTENSIBLE's order.
    if (channels > 2) {
        char mask[32];
        std::snprintf(mask, sizeof mask, "0x%llx", static_cast<unsigned long long>(gst()->gst_audio_channel_get_fallback_mask(static_cast<gint>(channels))));
        caps += std::string(", channel-mask=(bitmask)") + mask;
    }
    return caps;
}
} // namespace

Result<AudioEncoder*> open_audio(const AudioSettings& settings) {
    if (settings.codec != Codec::aac || !settings.sample_rate || !settings.channels) return Error::invalid_argument;
    if (!gst()) return Error::unsupported;
    const auto& s = settings;
    bool any = false;
    for (auto& f : enumerate(Codec::aac, Direction::encode)) {
        if (!s.component.empty() && f.component.id != s.component) continue;
        any = true;
        GstElement* codec = make(f.factory);
        if (!codec) continue;
        // The bit rate: the nearest it takes (its default for 0).
        Range r;
        std::uint32_t chosen = 0;
        if (int_range(codec, "bitrate", r)) {
            const std::int64_t want = s.bit_rate ? s.bit_rate : (r.value >= 8000 ? r.value : 64000 * std::min<std::int64_t>(s.channels, 2));
            chosen = static_cast<std::uint32_t>(std::clamp<std::int64_t>(want, std::max<std::int64_t>(r.min, 8000), r.max));
            set(codec, "bitrate", std::to_string(chosen));
        }
        if (!chosen) { unref_object(codec); continue; }
        auto encoder = std::make_unique<AudioEncoder>();
        encoder->settings = s;
        auto* e = encoder.get();
        const auto started = e->chain.start({make("audioconvert"), codec}, raw_audio_caps(s.sample_rate, s.channels), coded_caps(Codec::aac),
                                            [e](GstBuffer* b, GstCaps* caps) { e->take(b, caps); });
        if (started != Error::none) continue;
        {
            std::lock_guard lock(e->chain.mutex_);
            if (e->configuration.empty()) {
                // Not negotiated before the first packet: AAC LC (object type 2), the sampling frequency index and the
                // channel configuration (ISO/IEC 14496-3, 1.6.2.1) as the encoder was set up; the caps' codec_data
                // replaces it once the encoder gives it.
                static constexpr std::uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
                const auto found = std::find(std::begin(rates), std::end(rates), s.sample_rate);
                if (found != std::end(rates) && (s.channels <= 6 || s.channels == 8)) {
                    const auto index = static_cast<std::uint32_t>(found - std::begin(rates));
                    const auto channel_config = s.channels == 8 ? 7u : s.channels;
                    e->configuration = {static_cast<std::uint8_t>((2 << 3) | (index >> 1)), static_cast<std::uint8_t>(((index & 1) << 7) | (channel_config << 3))};
                }
            }
            if (e->configuration.empty()) continue;
        }
        e->bit_rate = chosen;
        e->component = std::move(f.component);
        return encoder.release();
    }
    return any ? Error::invalid_argument : Error::not_found;
}

const Component& component(const AudioEncoder* encoder) noexcept { return encoder->component; }
std::uint32_t bit_rate(const AudioEncoder* encoder) noexcept { return encoder ? encoder->bit_rate : 0; }
std::uint32_t frames_per_packet(const AudioEncoder* encoder) noexcept { return encoder ? 1024 : 0; }
std::vector<std::uint8_t> configuration(const AudioEncoder* encoder) {
    if (!encoder) return {};
    std::lock_guard lock(const_cast<AudioEncoder*>(encoder)->chain.mutex_);
    return encoder->configuration;
}

Status encode(AudioEncoder* encoder, std::span<const std::int16_t> samples, std::vector<Sample>& out) {
    if (!encoder || samples.size() % encoder->settings.channels) return Error::invalid_argument;
    if (samples.empty()) return {};
    GstBuffer* buffer = buffer_of(std::span(reinterpret_cast<const std::uint8_t*>(samples.data()), samples.size() * 2));
    if (!buffer) return Error::no_memory;
    const auto frames = samples.size() / encoder->settings.channels;
    const auto rate = encoder->settings.sample_rate;
    GST_BUFFER_PTS(buffer) = encoder->frames_in * GST_SECOND / rate;
    GST_BUFFER_DURATION(buffer) = frames * GST_SECOND / rate;
    encoder->frames_in += frames;
    const auto error = encoder->chain.push(buffer);
    std::lock_guard lock(encoder->chain.mutex_);
    for (auto& s : encoder->ready) out.push_back(std::move(s));
    encoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Status drain(AudioEncoder* encoder, std::vector<Sample>& out) {
    if (!encoder) return Error::invalid_argument;
    // The last block completed with silence: some encoders drop a partial one at the end of the stream (voaacenc), and
    // an AAC frame is 1024 frames whatever the input's length.
    Error error = Error::none;
    if (const auto partial = encoder->frames_in % 1024) {
        const std::vector<std::int16_t> silence((1024 - partial) * encoder->settings.channels, 0);
        std::vector<Sample> before;
        if (const auto padded = encode(encoder, silence, before); !padded) error = padded.error();
        std::lock_guard lock(encoder->chain.mutex_);
        encoder->ready.insert(encoder->ready.begin(), std::make_move_iterator(before.begin()), std::make_move_iterator(before.end()));
    }
    if (error == Error::none) error = encoder->chain.drain();
    std::lock_guard lock(encoder->chain.mutex_);
    for (auto& s : encoder->ready) out.push_back(std::move(s));
    encoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Result<std::uint32_t> measure_delay(const AudioSettings& settings) {
    const auto opened = open_audio(settings);
    if (!opened) return opened.error();
    auto* encoder = *opened;
    const auto rate = settings.sample_rate, channels = settings.channels;
    // One second of deterministic noise, encoded.
    std::vector<std::int16_t> input(std::size_t(rate) * channels);
    std::uint32_t seed = 1;
    for (auto& v : input) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<std::int16_t>(static_cast<std::int32_t>(seed >> 16) % 8000 - 4000);
    }
    std::vector<Sample> packets;
    for (std::size_t at = 0; at < input.size(); at += 1024ULL * channels)
        if (!encode(encoder, std::span(input).subspan(at, std::min<std::size_t>(1024ULL * channels, input.size() - at)), packets)) { close(encoder); return Error::io; }
    if (!drain(encoder, packets)) { close(encoder); return Error::io; }
    const auto config = configuration(encoder);
    close(encoder);
    // Decoded by the host's AAC decoder; what it gives before the drain, and what it held back.
    const auto opened_decoder = open_audio_decoder({Codec::aac, rate, channels, config, {}});
    if (!opened_decoder) return opened_decoder.error();
    auto* decoder = *opened_decoder;
    std::vector<std::int16_t> out;
    for (const auto& p : packets)
        if (!decode(decoder, p.data, out)) { close(decoder); return Error::io; }
    const std::size_t held = out.size();
    const bool drained = static_cast<bool>(drain(decoder, out));
    close(decoder);
    if (!drained) return Error::io;
    const std::size_t span = rate / 2;
    std::uint32_t best = 0;
    double best_score = -1;
    for (std::uint32_t shift = 0; shift <= 4096; ++shift) {
        if ((std::size_t(shift) + span) * channels > out.size()) break;
        double score = 0;
        for (std::size_t i = 0; i < span; ++i) score += double(out[(shift + i) * channels]) * input[i * channels];
        if (score > best_score) { best_score = score; best = shift; }
    }
    if (best_score <= 0) return Error::io;
    // Frames a decoder holds back until the drain are delay too for a player that trims by the stream's priming.
    return best + static_cast<std::uint32_t>((out.size() - held) / channels);
}

void close(AudioEncoder* encoder) noexcept { delete encoder; }

// ---- decoding ---------------------------------------------------------------------------------------

Result<std::vector<Component>> decoders(Codec codec) {
    if (!gst()) return Error::unsupported;
    std::vector<Component> out;
    for (auto& f : enumerate(codec, Direction::decode)) out.push_back(std::move(f.component));
    return out;
}

class VideoDecoder {
public:
    Chain chain;
    Component component;
    std::vector<std::uint8_t> sets;        // the parameter sets, Annex B
    bool started{};
    std::vector<DecodedPicture> ready;

    // A picture: NV12 or P010 of the displayed size, rows copied tight.
    void take(GstBuffer* out, GstCaps* caps) {
        GstVideoInfo info{};
        if (!caps || !gst()->gst_video_info_from_caps(&info, caps)) return;
        GstVideoFrame frame{};
        if (!gst()->gst_video_frame_map(&frame, &info, out, GST_MAP_READ)) return;
        DecodedPicture p;
        p.pts = chain.time(GST_BUFFER_PTS(out));
        p.duration = duration_of(GST_BUFFER_DURATION(out));
        p.bit_depth = GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_P010_10LE ? 10 : 8;
        const std::size_t bytes = p.bit_depth > 8 ? 2 : 1;
        p.width = static_cast<std::uint32_t>(GST_VIDEO_INFO_WIDTH(&info)) & ~1u;
        p.height = static_cast<std::uint32_t>(GST_VIDEO_INFO_HEIGHT(&info)) & ~1u;
        const std::size_t row = std::size_t(p.width) * bytes;
        p.luma_stride = p.chroma_stride = row;
        p.luma.resize(row * p.height);
        p.chroma.resize(row * (p.height / 2));
        const auto* luma = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        const auto* chroma = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
        const auto luma_pitch = static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
        const auto chroma_pitch = static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1));
        for (std::uint32_t r = 0; r < p.height; ++r) std::memcpy(p.luma.data() + r * row, luma + r * luma_pitch, row);
        for (std::uint32_t r = 0; r < p.height / 2; ++r) std::memcpy(p.chroma.data() + r * row, chroma + r * chroma_pitch, row);
        gst()->gst_video_frame_unmap(&frame);
        ready.push_back(std::move(p));
    }
};

Result<VideoDecoder*> open_video_decoder(const VideoDecoderSettings& settings) {
    if (settings.codec == Codec::aac) return Error::invalid_argument;
    if (!gst()) return Error::unsupported;
    std::vector<std::uint8_t> sets;
    for (const auto& set : settings.parameter_sets) {
        std::vector<std::uint8_t> prefixed{static_cast<std::uint8_t>(set.size() >> 24), static_cast<std::uint8_t>(set.size() >> 16),
                                           static_cast<std::uint8_t>(set.size() >> 8), static_cast<std::uint8_t>(set.size())};
        prefixed.insert(prefixed.end(), set.begin(), set.end());
        append_annex_b(sets, prefixed);
    }
    std::string input = coded_caps(settings.codec);
    if (settings.width && settings.height) input += ", width=(int)" + std::to_string(settings.width) + ", height=(int)" + std::to_string(settings.height);
    bool any = false;
    for (auto& f : enumerate(settings.codec, Direction::decode)) {
        if (!settings.component.empty() && f.component.id != settings.component) continue;
        any = true;
        // Annex B in whole access units to the decoder itself, or through the codec's parser.
        std::vector<GstElement*> elements;
        if (!template_takes(f.factory, GST_PAD_SINK, coded_caps(settings.codec))) elements.push_back(make(parser_of(settings.codec)));
        elements.push_back(make(f.factory));
        elements.push_back(make("videoconvert"));
        auto decoder = std::make_unique<VideoDecoder>();
        decoder->sets = sets;
        auto* d = decoder.get();
        if (d->chain.start(std::move(elements), input, "video/x-raw, format=(string){ NV12, P010_10LE }",
                           [d](GstBuffer* b, GstCaps* caps) { d->take(b, caps); }) != Error::none)
            continue;
        d->component = std::move(f.component);
        return decoder.release();
    }
    return any ? Error::invalid_argument : Error::not_found;
}

const Component& component(const VideoDecoder* decoder) noexcept { return decoder->component; }

Status decode(VideoDecoder* decoder, const Sample& sample, std::vector<DecodedPicture>& out) {
    if (!decoder) return Error::invalid_argument;
    std::vector<std::uint8_t> bytes;
    if (!decoder->started || sample.keyframe) bytes = decoder->sets;
    append_annex_b(bytes, sample.data);
    decoder->started = true;
    GstBuffer* buffer = buffer_of(bytes);
    if (!buffer) return Error::no_memory;
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(to_gst(sample.pts));
    GST_BUFFER_DURATION(buffer) = sample.duration > 0 ? static_cast<GstClockTime>(to_gst(sample.duration)) : GST_CLOCK_TIME_NONE;
    if (!sample.keyframe) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    const auto error = decoder->chain.push(buffer);
    std::lock_guard lock(decoder->chain.mutex_);
    for (auto& p : decoder->ready) out.push_back(std::move(p));
    decoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Status drain(VideoDecoder* decoder, std::vector<DecodedPicture>& out) {
    if (!decoder) return Error::invalid_argument;
    const auto error = decoder->chain.drain();
    std::lock_guard lock(decoder->chain.mutex_);
    for (auto& p : decoder->ready) out.push_back(std::move(p));
    decoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

void flush(VideoDecoder* decoder) noexcept {
    if (!decoder) return;
    decoder->chain.flush();
    decoder->started = false;
    std::lock_guard lock(decoder->chain.mutex_);
    decoder->ready.clear();
}

void close(VideoDecoder* decoder) noexcept { delete decoder; }

class AudioDecoder {
public:
    Chain chain;
    Component component;
    std::vector<std::int16_t> ready;
    void take(GstBuffer* out) {
        const auto bytes = bytes_of(out);
        const auto at = ready.size();
        ready.resize(at + bytes.size() / 2);
        std::memcpy(ready.data() + at, bytes.data(), bytes.size() / 2 * 2);
    }
};

Result<AudioDecoder*> open_audio_decoder(const AudioDecoderSettings& settings) {
    if (settings.codec != Codec::aac || !settings.sample_rate || !settings.channels || settings.configuration.size() < 2) return Error::invalid_argument;
    if (!gst()) return Error::unsupported;
    const std::string input = std::string(coded_caps(Codec::aac)) + ", framed=(boolean)true, rate=(int)" + std::to_string(settings.sample_rate) +
                              ", channels=(int)" + std::to_string(settings.channels) + ", codec_data=(buffer)" + hex(settings.configuration);
    const std::string output = "audio/x-raw, format=(string)S16LE, layout=(string)interleaved, channels=(int)" + std::to_string(settings.channels);
    bool any = false;
    for (auto& f : enumerate(Codec::aac, Direction::decode)) {
        if (!settings.component.empty() && f.component.id != settings.component) continue;
        any = true;
        std::vector<GstElement*> elements;
        if (!template_takes(f.factory, GST_PAD_SINK, input.c_str())) elements.push_back(make(parser_of(Codec::aac)));
        elements.push_back(make(f.factory));
        elements.push_back(make("audioconvert"));
        auto decoder = std::make_unique<AudioDecoder>();
        auto* d = decoder.get();
        if (d->chain.start(std::move(elements), input, output, [d](GstBuffer* b, GstCaps*) { d->take(b); }) != Error::none) continue;
        d->component = std::move(f.component);
        return decoder.release();
    }
    return any ? Error::invalid_argument : Error::not_found;
}

const Component& component(const AudioDecoder* decoder) noexcept { return decoder->component; }

Status decode(AudioDecoder* decoder, std::span<const std::uint8_t> packet, std::vector<std::int16_t>& out) {
    if (!decoder) return Error::invalid_argument;
    GstBuffer* buffer = buffer_of(packet);
    if (!buffer) return Error::no_memory;
    const auto error = decoder->chain.push(buffer);
    std::lock_guard lock(decoder->chain.mutex_);
    out.insert(out.end(), decoder->ready.begin(), decoder->ready.end());
    decoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

Status drain(AudioDecoder* decoder, std::vector<std::int16_t>& out) {
    if (!decoder) return Error::invalid_argument;
    const auto error = decoder->chain.drain();
    std::lock_guard lock(decoder->chain.mutex_);
    out.insert(out.end(), decoder->ready.begin(), decoder->ready.end());
    decoder->ready.clear();
    if (error != Error::none) return error;
    return {};
}

void flush(AudioDecoder* decoder) noexcept {
    if (!decoder) return;
    decoder->chain.flush();
    std::lock_guard lock(decoder->chain.mutex_);
    decoder->ready.clear();
}

void close(AudioDecoder* decoder) noexcept { delete decoder; }

} // namespace alchemy::host::media_codec

#endif
