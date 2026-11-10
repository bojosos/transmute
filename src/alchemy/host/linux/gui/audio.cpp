// Linux host backend, area "audio" (include/alchemy/host/audio.hpp), over PulseAudio's client library (libpulse;
// PipeWire's PulseAudio server and WSLg's both answer it). One threaded main loop and context per process, made at
// the first call and kept: when no server answers (none running, none reachable), every call fails with
// Error::unsupported, as a host without audio.
// - Endpoints: the server's sinks (output) and sources (input; a sink's monitor source is not a capture device)
//   with their descriptions, names as uids, sample specs and channel maps (as WAVEFORMATEXTENSIBLE speaker masks)
//   and the form factor their device.form_factor property names; the server's default sink and source.
// - Streams: a playback or record stream of 32-bit float samples in the rate and channels asked for (0: the
//   endpoint's), which the server converts to its own. Playback asks `render` for what the server requests, about
//   10 ms at a time, with the frames the server already holds ahead of them as `queued`; recording hands `capture`
//   each fragment the server delivers. Callbacks run on the main loop's thread with its lock held, so once a stop
//   (which takes the lock) returns, none is running and none starts until the next start.
// - The microphone's privacy setting: Linux desktops keep none a program can read (portals ask per use), so the
//   answer is unknown.
#include "alchemy/host/audio.hpp"
#include "alchemy/host/time.hpp"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::host::audio {

namespace {

// The process's connection to the server, or none.
class Server {
public:
    static Server* get() {
        static Server* server = [] {
            auto* made = new Server();   // kept for the process: its thread serves every stream
            if (!made->connect()) {
                delete made;
                return static_cast<Server*>(nullptr);
            }
            return made;
        }();
        return server;
    }
    pa_threaded_mainloop* loop{};
    pa_context* context{};

    // Runs `operation` (made under the lock) to completion; false when it failed or the context went away.
    bool wait(pa_operation* operation) {
        if (!operation) return false;
        while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) pa_threaded_mainloop_wait(loop);
        const bool done = pa_operation_get_state(operation) == PA_OPERATION_DONE;
        pa_operation_unref(operation);
        return done;
    }

private:
    bool connect() {
        loop = pa_threaded_mainloop_new();
        if (!loop) return false;
        auto* api = pa_threaded_mainloop_get_api(loop);
        pa_proplist* properties = pa_proplist_new();
        pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, "Alchemy");
        context = pa_context_new_with_proplist(api, "Alchemy", properties);
        pa_proplist_free(properties);
        if (!context) return false;
        pa_context_set_state_callback(context, [](pa_context*, void* self) {
            pa_threaded_mainloop_signal(static_cast<Server*>(self)->loop, 0);
        }, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0 || pa_threaded_mainloop_start(loop) < 0) {
            pa_context_unref(context);
            return false;
        }
        pa_threaded_mainloop_lock(loop);
        pa_context_state_t state;
        while ((state = pa_context_get_state(context)) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(state)) pa_threaded_mainloop_wait(loop);
        pa_threaded_mainloop_unlock(loop);
        if (state != PA_CONTEXT_READY) {
            pa_threaded_mainloop_stop(loop);
            pa_context_disconnect(context);
            pa_context_unref(context);
            pa_threaded_mainloop_free(loop);
            return false;
        }
        return true;
    }
};

class Lock {
public:
    explicit Lock(Server* s) : s_(s) { pa_threaded_mainloop_lock(s_->loop); }
    ~Lock() { pa_threaded_mainloop_unlock(s_->loop); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    Server* s_;
};

// WAVEFORMATEXTENSIBLE speaker bits of a channel map; 0 when a position has none.
std::uint32_t speaker_mask(const pa_channel_map& map) {
    std::uint32_t mask = 0;
    for (unsigned i = 0; i < map.channels; ++i) {
        std::uint32_t bit = 0;
        switch (map.map[i]) {
        case PA_CHANNEL_POSITION_FRONT_LEFT: bit = 0x1; break;
        case PA_CHANNEL_POSITION_FRONT_RIGHT: bit = 0x2; break;
        case PA_CHANNEL_POSITION_MONO:
        case PA_CHANNEL_POSITION_FRONT_CENTER: bit = 0x4; break;
        case PA_CHANNEL_POSITION_LFE: bit = 0x8; break;
        case PA_CHANNEL_POSITION_REAR_LEFT: bit = 0x10; break;
        case PA_CHANNEL_POSITION_REAR_RIGHT: bit = 0x20; break;
        case PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER: bit = 0x40; break;
        case PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER: bit = 0x80; break;
        case PA_CHANNEL_POSITION_REAR_CENTER: bit = 0x100; break;
        case PA_CHANNEL_POSITION_SIDE_LEFT: bit = 0x200; break;
        case PA_CHANNEL_POSITION_SIDE_RIGHT: bit = 0x400; break;
        case PA_CHANNEL_POSITION_TOP_CENTER: bit = 0x800; break;
        case PA_CHANNEL_POSITION_TOP_FRONT_LEFT: bit = 0x1000; break;
        case PA_CHANNEL_POSITION_TOP_FRONT_CENTER: bit = 0x2000; break;
        case PA_CHANNEL_POSITION_TOP_FRONT_RIGHT: bit = 0x4000; break;
        case PA_CHANNEL_POSITION_TOP_REAR_LEFT: bit = 0x8000; break;
        case PA_CHANNEL_POSITION_TOP_REAR_CENTER: bit = 0x10000; break;
        case PA_CHANNEL_POSITION_TOP_REAR_RIGHT: bit = 0x20000; break;
        default: return 0;
        }
        if (mask & bit) return 0;
        mask |= bit;
    }
    return mask;
}

// The EndpointFormFactor of a device.form_factor property (0 when it names none this maps).
std::uint32_t form_factor(const pa_proplist* properties) {
    const char* value = properties ? pa_proplist_gets(properties, PA_PROP_DEVICE_FORM_FACTOR) : nullptr;
    if (!value) return 0;
    const std::string v = value;
    return v == "speaker" ? 1 : v == "headphone" ? 3 : v == "microphone" ? 4 : v == "headset" ? 5 : v == "handset" ? 6 : 0;
}

Endpoint endpoint_of(const char* name, const char* description, bool input, const pa_sample_spec& spec, const pa_channel_map& map,
                     const pa_proplist* properties) {
    Endpoint e;
    e.uid = name ? name : "";
    e.name = description ? description : "";
    e.input = input;
    e.form_factor = form_factor(properties);
    e.has_format = spec.rate != 0 && spec.channels != 0;
    e.sample_rate = spec.rate;
    e.channels = spec.channels;
    e.channel_mask = speaker_mask(map);
    return e;
}

struct Listing {
    Server* server{};
    Endpoints* out{};
};

// The endpoint `uid` of a kind, as the server lists it; nothing when there is none (or no server).
std::optional<Endpoint> find(Server* s, std::string_view uid, bool input) {
    auto listed = endpoints();
    if (!listed) return std::nullopt;
    const auto& wanted = uid.empty() ? (input ? listed->default_input : listed->default_output) : std::string(uid);
    for (auto& e : listed->endpoints)
        if (e.input == input && e.uid == wanted) return e;
    static_cast<void>(s);
    return std::nullopt;
}

constexpr std::uint32_t chunk_ms = 10, ahead_ms = 40;

} // namespace

Result<Endpoints> endpoints() {
    auto* s = Server::get();
    if (!s) return Error::unsupported;
    Endpoints out;
    Listing listing{s, &out};
    Lock lock(s);
    const bool sinks = s->wait(pa_context_get_sink_info_list(s->context, [](pa_context*, const pa_sink_info* info, int eol, void* data) {
        auto* l = static_cast<Listing*>(data);
        if (eol) { pa_threaded_mainloop_signal(l->server->loop, 0); return; }
        l->out->endpoints.push_back(endpoint_of(info->name, info->description, false, info->sample_spec, info->channel_map, info->proplist));
    }, &listing));
    const bool sources = s->wait(pa_context_get_source_info_list(s->context, [](pa_context*, const pa_source_info* info, int eol, void* data) {
        auto* l = static_cast<Listing*>(data);
        if (eol) { pa_threaded_mainloop_signal(l->server->loop, 0); return; }
        if (info->monitor_of_sink != PA_INVALID_INDEX) return;   // a sink's monitor, not a capture device
        l->out->endpoints.push_back(endpoint_of(info->name, info->description, true, info->sample_spec, info->channel_map, info->proplist));
    }, &listing));
    const bool defaults = s->wait(pa_context_get_server_info(s->context, [](pa_context*, const pa_server_info* info, void* data) {
        auto* l = static_cast<Listing*>(data);
        if (info) {
            l->out->default_output = info->default_sink_name ? info->default_sink_name : "";
            l->out->default_input = info->default_source_name ? info->default_source_name : "";
        }
        pa_threaded_mainloop_signal(l->server->loop, 0);
    }, &listing));
    if (!sinks || !sources || !defaults) return Error::io;
    // A default that is a monitor (or gone) is no default here.
    const auto listed = [&](const std::string& uid, bool input) {
        return std::any_of(out.endpoints.begin(), out.endpoints.end(), [&](const Endpoint& e) { return e.input == input && e.uid == uid; });
    };
    if (!listed(out.default_output, false)) out.default_output.clear();
    if (!listed(out.default_input, true)) out.default_input.clear();
    return out;
}

// ---- streams ------------------------------------------------------------------------------------

namespace {

// What a playback or record stream shares: its stream, its state under the main loop's lock, and connecting.
struct StreamBase {
    Server* server{};
    pa_stream* stream{};
    bool running{};
    std::uint32_t rate{}, channels{}, mask{};

    // Connects `stream` and waits until it is ready (or failed).
    bool wait_ready() {
        pa_stream_state_t state;
        while ((state = pa_stream_get_state(stream)) != PA_STREAM_READY && PA_STREAM_IS_GOOD(state)) pa_threaded_mainloop_wait(server->loop);
        return state == PA_STREAM_READY;
    }
    // Corks or uncorks (under the lock) and waits for the server.
    bool cork(bool corked) {
        if (!server->wait(pa_stream_cork(stream, corked ? 1 : 0, [](pa_stream*, int, void* loop) {
                pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(loop), 0);
            }, server->loop)))
            return false;
        if (corked) static_cast<void>(server->wait(pa_stream_flush(stream, [](pa_stream*, int, void* loop) {
            pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(loop), 0);
        }, server->loop)));
        return true;
    }
    void release() {
        if (!stream) return;
        pa_stream_set_write_callback(stream, nullptr, nullptr);
        pa_stream_set_read_callback(stream, nullptr, nullptr);
        pa_stream_set_state_callback(stream, nullptr, nullptr);
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
        stream = nullptr;
    }
};

// The sample spec and channel map of a stream asked for in `rate` and `channels` on `endpoint` (0: its own).
bool spec_for(const Endpoint& endpoint, std::uint32_t rate, std::uint32_t channels, pa_sample_spec& spec, pa_channel_map& map) {
    spec.format = PA_SAMPLE_FLOAT32NE;
    spec.rate = rate ? rate : endpoint.sample_rate ? endpoint.sample_rate : 48000;
    spec.channels = static_cast<std::uint8_t>(channels ? channels : endpoint.channels ? endpoint.channels : 2);
    if (!pa_sample_spec_valid(&spec)) return false;
    return pa_channel_map_init_auto(&map, spec.channels, PA_CHANNEL_MAP_DEFAULT) != nullptr;
}

} // namespace

class OutputStream : public StreamBase {
public:
    Render render;
    OutputFormat format;

    static void on_write(pa_stream* stream, std::size_t bytes, void* self) {
        auto* o = static_cast<OutputStream*>(self);
        const std::size_t frame_bytes = sizeof(float) * o->channels;
        if (!o->running || frame_bytes == 0) return;
        // What the server holds ahead of these frames: its target buffer less what it asks for.
        const pa_buffer_attr* attr = pa_stream_get_buffer_attr(stream);
        const std::size_t target = attr ? attr->tlength : 0;
        const auto queued = static_cast<std::uint32_t>((target > bytes ? target - bytes : 0) / frame_bytes);
        while (bytes >= frame_bytes) {
            void* data = nullptr;
            std::size_t size = bytes;
            if (pa_stream_begin_write(stream, &data, &size) < 0 || !data) return;
            const auto frames = static_cast<std::uint32_t>(std::min(size, bytes) / frame_bytes);
            if (frames == 0) { pa_stream_cancel_write(stream); return; }
            o->render(static_cast<float*>(data), frames, queued);
            if (pa_stream_write(stream, data, std::size_t(frames) * frame_bytes, nullptr, 0, PA_SEEK_RELATIVE) < 0) return;
            bytes -= std::size_t(frames) * frame_bytes;
        }
    }
};

Result<OutputStream*> open_output(std::string_view uid, std::uint32_t sample_rate, std::uint32_t channels, Render render) {
    auto* s = Server::get();
    if (!s) return Error::unsupported;
    if (!render) return Error::invalid_argument;
    const auto endpoint = find(s, uid, false);
    if (!endpoint) return Error::not_found;
    pa_sample_spec spec{};
    pa_channel_map map{};
    if (!spec_for(*endpoint, sample_rate, channels, spec, map)) return Error::invalid_argument;
    auto* o = new OutputStream();
    o->server = s;
    o->render = std::move(render);
    o->rate = spec.rate;
    o->channels = spec.channels;
    o->mask = speaker_mask(map);
    Lock lock(s);
    o->stream = pa_stream_new(s->context, "Alchemy output", &spec, &map);
    if (!o->stream) {
        delete o;
        return Error::no_memory;
    }
    pa_stream_set_state_callback(o->stream, [](pa_stream*, void* loop) { pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(loop), 0); }, s->loop);
    pa_stream_set_write_callback(o->stream, &OutputStream::on_write, o);
    const std::uint32_t frame_bytes = sizeof(float) * spec.channels;
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<std::uint32_t>(-1);
    attr.tlength = spec.rate * ahead_ms / 1000 * frame_bytes;
    attr.prebuf = static_cast<std::uint32_t>(-1);
    attr.minreq = spec.rate * chunk_ms / 1000 * frame_bytes;
    attr.fragsize = static_cast<std::uint32_t>(-1);
    const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE);
    if (pa_stream_connect_playback(o->stream, endpoint->uid.c_str(), &attr, flags, nullptr, nullptr) < 0 || !o->wait_ready()) {
        o->release();
        delete o;
        return Error::io;
    }
    const pa_buffer_attr* got = pa_stream_get_buffer_attr(o->stream);
    o->format = {o->rate, o->channels, o->mask, got ? got->minreq / frame_bytes : attr.minreq / frame_bytes,
                 got ? got->tlength / frame_bytes : attr.tlength / frame_bytes};
    return o;
}

OutputFormat output_format(const OutputStream* stream) noexcept { return stream ? stream->format : OutputFormat{}; }

Status start_output(OutputStream* stream) {
    if (!stream) return Error::invalid_argument;
    Lock lock(stream->server);
    stream->running = true;
    if (!stream->cork(false)) {
        stream->running = false;
        return Error::io;
    }
    // The server asks for its whole buffer again after the uncork; ask now in case it already did while corked.
    if (const auto writable = pa_stream_writable_size(stream->stream); writable != static_cast<std::size_t>(-1) && writable > 0)
        OutputStream::on_write(stream->stream, writable, stream);
    return Error::none;
}

void stop_output(OutputStream* stream) noexcept {
    if (!stream) return;
    Lock lock(stream->server);
    stream->running = false;
    static_cast<void>(stream->cork(true));
}

void close_output(OutputStream* stream) noexcept {
    if (!stream) return;
    {
        Lock lock(stream->server);
        stream->running = false;
        stream->release();
    }
    delete stream;
}

class InputStream : public StreamBase {
public:
    Capture capture;
    InputFormat format;

    static void on_read(pa_stream* stream, std::size_t, void* self) {
        auto* in = static_cast<InputStream*>(self);
        const void* data = nullptr;
        std::size_t bytes = 0;
        while (pa_stream_readable_size(stream) > 0 && pa_stream_peek(stream, &data, &bytes) == 0 && bytes > 0) {
            const std::size_t frame_bytes = sizeof(float) * in->channels;
            // A hole (data NULL) is frames the server lost: nothing to hand on.
            if (data && in->running && frame_bytes) in->capture(static_cast<const float*>(data), static_cast<std::uint32_t>(bytes / frame_bytes), time::monotonic_ns());
            pa_stream_drop(stream);
        }
    }
};

Result<InputStream*> open_input(std::string_view uid, std::uint32_t sample_rate, std::uint32_t channels, Capture capture) {
    auto* s = Server::get();
    if (!s) return Error::unsupported;
    if (!capture) return Error::invalid_argument;
    const auto endpoint = find(s, uid, true);
    if (!endpoint) return Error::not_found;
    pa_sample_spec spec{};
    pa_channel_map map{};
    if (!spec_for(*endpoint, sample_rate, channels, spec, map)) return Error::invalid_argument;
    auto* in = new InputStream();
    in->server = s;
    in->capture = std::move(capture);
    in->rate = spec.rate;
    in->channels = spec.channels;
    in->mask = speaker_mask(map);
    in->format = {in->rate, in->channels, in->mask};
    Lock lock(s);
    in->stream = pa_stream_new(s->context, "Alchemy input", &spec, &map);
    if (!in->stream) {
        delete in;
        return Error::no_memory;
    }
    pa_stream_set_state_callback(in->stream, [](pa_stream*, void* loop) { pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(loop), 0); }, s->loop);
    pa_stream_set_read_callback(in->stream, &InputStream::on_read, in);
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<std::uint32_t>(-1);
    attr.fragsize = spec.rate * chunk_ms / 1000 * static_cast<std::uint32_t>(sizeof(float) * spec.channels);
    const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY);
    if (pa_stream_connect_record(in->stream, endpoint->uid.c_str(), &attr, flags) < 0 || !in->wait_ready()) {
        in->release();
        delete in;
        return Error::io;
    }
    return in;
}

InputFormat input_format(const InputStream* stream) noexcept { return stream ? stream->format : InputFormat{}; }

Status start_input(InputStream* stream) {
    if (!stream) return Error::invalid_argument;
    Lock lock(stream->server);
    stream->running = true;
    if (!stream->cork(false)) {
        stream->running = false;
        return Error::io;
    }
    return Error::none;
}

void stop_input(InputStream* stream) noexcept {
    if (!stream) return;
    Lock lock(stream->server);
    stream->running = false;
    static_cast<void>(stream->cork(true));
}

void close_input(InputStream* stream) noexcept {
    if (!stream) return;
    {
        Lock lock(stream->server);
        stream->running = false;
        stream->release();
    }
    delete stream;
}

Access microphone_access() { return Access::unknown; }

} // namespace alchemy::host::audio
