#include "alchemy/darwin/media/avaudio.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// AVAudioEngine on the default output device's I/O (hal_io.hpp), with its output, input, mixer
// and player nodes.
// - The graph renders by pulling from the output node once per device buffer, at the device's
//   rate: a mixer sums its inputs (mono inputs into every channel, others channel by channel)
//   with each input's volume and pan, times its outputVolume; a player plays its schedule
//   resampled from the scheduled audio's rate. Nodes run at the engine's rate whatever their
//   connection formats' rates (players excepted).
// - AVAudioPlayerNode: buffers (copied when scheduled), files and file segments, with the loops,
//   interrupts and interrupts-at-loop options; completion handlers run on the engine's callback
//   thread once the audio is consumed, whatever the callback type asked for, and when the node is
//   stopped or reset. playAtTime: starts at an AVAudioTime's host time; node and player times
//   convert between the engine's frames and the player's.
// - Taps (installTapOnBus:...) deliver buffers of the node's output on the callback thread.
// - AVAudioSourceNode: its render block fills an AudioBufferList of its format (the connection's
//   without one) on the rendering thread, resampled to the engine's rate. AVAudioSinkNode: its
//   receiver block gets what its input renders, once per render cycle after the output's.
// - Manual rendering: offline (-renderOffline:toBuffer:error:) or realtime (manualRenderingBlock)
//   in the format given, with no device; the input node then takes its audio from the block given
//   to -setManualRenderingInputPCMFormat:inputBlock:.
// - Render and receiver blocks, and the input block, run under the engines' lock: one that calls
//   back into the engine or its nodes waits forever (Apple's documentation forbids such calls).
// - Unavailable, as documented: there is no input device, so -startAndReturnError: fails with an
//   error while the input node feeds the graph outside manual rendering, and the input node's
//   formats are nil there. Without an output device -startAndReturnError: fails with an error. A tap on a player whose format's rate
//   is not the engine's stops with a message.
namespace alchemy::gui::completion_kit::media::avaudio {
namespace {

namespace af = audio_file;

constexpr std::int32_t no_device = static_cast<std::int32_t>(0x21646576);   // '!dev' (kAudioHardwareBadDeviceError)
// AVAudioPlayerNodeBufferOptions
constexpr std::uint64_t option_loops = 1, option_interrupts = 2, option_interrupts_at_loop = 4;

enum class Kind { output, input, mixer, player, source, sink, other };
// AVAudioEngineManualRenderingStatus and AVAudioEngineManualRenderingError
constexpr std::int64_t manual_error = -1, manual_success = 0, manual_insufficient_input = 1;
constexpr std::int32_t manual_invalid_mode = -80800, manual_initialized = -80801, manual_not_running = -80802;
constexpr std::uint32_t timestamp_sample_host_valid = 3;   // kAudioTimeStampSampleHostTimeValid

// Interleaved samples in `pcm` from the AudioBufferList at `list` (missing or short buffers: silence).
std::vector<double> read_list(Support& support, const af::Pcm& pcm, Address list, std::uint32_t frames) {
    auto& m = support.memory();
    const auto planes = pcm.non_interleaved ? pcm.channels : 1;
    const auto bytes = static_cast<std::size_t>(frames) * pcm.frame_bytes();
    std::vector<std::vector<std::uint8_t>> storage(planes, std::vector<std::uint8_t>(bytes));
    const auto count = list ? m.read32(list) : 0;
    for (std::uint32_t i = 0; i < planes && i < count; ++i) {
        const auto at = list + 8 + 16ULL * i;
        const auto size = std::min<std::size_t>(m.read32(at + 4), bytes);
        if (const auto data = m.read64(at + 8); data && size) m.read_bytes(data, std::span(storage[i]).first(size));
    }
    std::vector<std::span<const std::uint8_t>> spans(storage.begin(), storage.end());
    return af::decode(pcm, spans, frames);
}

// Writes interleaved samples (in `channels`) into the AudioBufferList at `list`, in `pcm`, and sets
// its byte sizes.
void write_list(Support& support, const af::Pcm& pcm, Address list, std::span<const double> interleaved, std::uint32_t channels, std::uint32_t frames) {
    auto& m = support.memory();
    const auto planes = pcm.non_interleaved ? pcm.channels : 1;
    const auto bytes = static_cast<std::size_t>(frames) * pcm.frame_bytes();
    std::vector<std::vector<std::uint8_t>> storage(planes, std::vector<std::uint8_t>(bytes));
    std::vector<std::span<std::uint8_t>> spans(storage.begin(), storage.end());
    af::encode(pcm, interleaved, channels, frames, spans);
    const auto count = m.read32(list);
    for (std::uint32_t i = 0; i < planes && i < count; ++i) {
        const auto at = list + 8 + 16ULL * i;
        if (const auto data = m.read64(at + 8)) m.write_bytes(data, storage[i]);
        m.write32(at + 4, static_cast<std::uint32_t>(bytes));
    }
}

// Guest scratch for a block's buffers: an AudioBufferList over planes in `pcm`, and an
// AudioTimeStamp and a BOOL beside it.
struct GuestList {
    Support& support;
    af::Pcm pcm;
    std::uint32_t capacity{};
    Address list{}, data{}, stamp{};
    GuestList(Support& owner, const af::Pcm& format) : support(owner), pcm(format) { stamp = support.process.allocate_guest(72, 8); }
    GuestList(const GuestList&) = delete;
    GuestList& operator=(const GuestList&) = delete;
    ~GuestList() {
        for (auto* at : {&list, &data, &stamp})
            if (*at) support.process.release_guest(std::exchange(*at, 0));
    }
    [[nodiscard]] std::uint32_t planes() const { return pcm.non_interleaved ? pcm.channels : 1; }
    [[nodiscard]] std::uint64_t plane_bytes() const { return static_cast<std::uint64_t>(capacity) * pcm.frame_bytes(); }
    // Points the list at `frames` zeroed frames (the planes grow as needed); the time stamp at
    // `sample_time` now.
    void prepare(std::uint32_t frames, double sample_time) {
        auto& m = support.memory();
        if (frames > capacity) {
            for (auto* at : {&list, &data})
                if (*at) support.process.release_guest(std::exchange(*at, 0));
            capacity = std::max(frames, 512U);
            data = support.process.allocate_guest(std::max<std::uint64_t>(plane_bytes() * planes(), 16), 16);
            list = support.process.allocate_guest(8 + 16ULL * planes(), 8);
        }
        m.fill_bytes(data, 0, plane_bytes() * planes());
        m.write32(list, planes());
        m.write32(list + 4, 0);
        for (std::uint32_t i = 0; i < planes(); ++i) {
            const auto at = list + 8 + 16ULL * i;
            m.write32(at, pcm.non_interleaved ? 1 : pcm.channels);
            m.write32(at + 4, static_cast<std::uint32_t>(static_cast<std::uint64_t>(frames) * pcm.frame_bytes()));
            m.write64(at + 8, data + i * plane_bytes());
        }
        m.fill_bytes(stamp, 0, 72);
        m.write64(stamp, std::bit_cast<std::uint64_t>(sample_time));
        m.write64(stamp + 8, host_time_now());
        m.write64(stamp + 16, std::bit_cast<std::uint64_t>(1.0));
        m.write32(stamp + 56, timestamp_sample_host_valid);
    }
};

struct Scheduled {
    std::vector<float> samples;        // interleaved
    std::uint32_t channels{};
    double rate{};
    std::uint64_t frames{}, played{};
    bool loops{};
    Address block{};                   // _Block_copy'd completion handler or 0
    bool typed{};                      // the handler takes an AVAudioPlayerNodeCompletionCallbackType
    std::uint64_t callback_type{};
};

// A tap's block and format, shared by the deliveries in flight: released with the last one
// (never under the engines' lock).
struct TapHandle {
    Support& support;
    std::shared_ptr<security::Bridge> bridge;
    Address block{};                   // _Block_copy'd
    Address format{};                  // retained
    TapHandle(Support& owner, std::shared_ptr<security::Bridge> b, Address copied_block, Address retained_format)
        : support(owner), bridge(std::move(b)), block(copied_block), format(retained_format) {}
    TapHandle(const TapHandle&) = delete;
    TapHandle& operator=(const TapHandle&) = delete;
    ~TapHandle() {
        if (block) bridge->block_release(block);
        support.release(format);
    }
};

struct Tap {
    std::shared_ptr<TapHandle> handle;
    std::uint32_t frames{};
    std::uint32_t channels{};
    std::vector<float> pending;
};

struct Node final : ObjectPayload {
    Kind kind{Kind::other};
    Address engine{};                  // not retained: attached nodes are the engine's
    std::map<std::uint32_t, Address> inputs;            // bus -> source node
    std::map<std::uint32_t, Address> input_formats;     // bus -> retained AVAudioFormat
    std::map<std::uint32_t, std::uint32_t> input_channels;   // bus -> the connection format's channels
    Address output_format{};           // retained (output bus 0)
    float volume{1}, pan{}, output_volume{1};
    // Players.
    std::deque<Scheduled> schedule;
    bool playing{};
    std::uint64_t delay_frames{};
    double played_frames{};            // the player's frames since -play (player time)
    double start_node_sample{};        // the engine's sample time at -play
    std::unique_ptr<Resampler> resampler;
    double resampler_rate{};
    std::uint32_t resampler_channels{};
    std::optional<Tap> tap;
    // Source and sink nodes.
    Address block{};                   // _Block_copy'd render or receiver block
    Address source_format{};           // retained; 0: the connection's
    std::unique_ptr<GuestList> scratch;
};

struct EngineState final : ObjectPayload {
    Address output{}, mixer{}, input{};   // retained
    std::vector<Address> attached;     // retained (the output, mixer and input nodes too)
    bool running{};
    std::shared_ptr<hal_io::DeviceIo> io;
    std::shared_ptr<CompletionThread> thread;
    std::thread::id callback_thread;   // the thread's id, once a callback ran
    double sample_time{};              // frames rendered since the first start
    // Manual rendering.
    bool manual{};
    std::int64_t manual_mode{};        // AVAudioEngineManualRenderingMode
    Address manual_format{};           // retained
    af::Pcm manual_pcm;
    std::uint32_t manual_maximum{};
    Address manual_block{};            // the manualRenderingBlock (made once)
    Address input_format{}, input_block{};   // retained / _Block_copy'd (the input node's audio)
    af::Pcm input_pcm;
};

// Work to hand the callback thread after a render.
struct Callback { Address block{}; bool typed{}; std::uint64_t type{}; };
struct TapDelivery { std::shared_ptr<TapHandle> handle; std::vector<float> samples; std::uint32_t channels{}, frames{}; double sample_time{}, rate{}; };

class Engines : public std::enable_shared_from_this<Engines> {
public:
    explicit Engines(Support& support) : support_(support), bridge_(security::bridge()) {}
    void install();

private:
    std::shared_ptr<Node> node(Address self) {
        const auto n = support_.objc.find_host_state<Node>(self);
        if (!n) unsupported("an AVAudioNode Alchemy did not make");
        return n;
    }
    std::shared_ptr<EngineState> engine(Address self) { return support_.objc.host_state<EngineState>(self); }
    Address make_node(Address cls, Kind kind);
    Address output_node(Address engine);
    Address main_mixer(Address engine);
    // The device format (rate, channels) the engine renders in.
    std::pair<double, std::uint32_t> device_format(EngineState& e);
    std::uint32_t channels_of(Address format);
    double rate_of(Address format);
    void attach(Address engine, Address node);
    void detach(Address engine, Address node);
    void connect(Address engine, Address source, Address destination, std::uint32_t from_bus, std::optional<std::uint32_t> to_bus, Address format);
    void disconnect_input(Address node, std::optional<std::uint32_t> bus);
    void disconnect_output(Address engine, Address node);
    Address output_format(Address node);   // not retained; may be 0
    bool start(Address engine, Address error_out);
    void stop(Address engine);
    void stop_player(Address node, bool clear);
    void post(const std::shared_ptr<EngineState>& e, std::vector<Callback> callbacks, std::vector<TapDelivery> taps);
    void schedule(Address player, Scheduled item, Address when, std::uint64_t options);

    // Render (the lock is held).
    std::vector<double> pull(Address node, std::uint32_t frames, std::uint32_t channels, double rate, double sample_time, unsigned depth,
                             std::vector<Callback>& callbacks, std::vector<TapDelivery>& taps);
    void render_player(Node& n, double* out, std::uint32_t frames, std::uint32_t channels, double rate, std::vector<Callback>& callbacks);
    void render_source(Node& n, double* out, std::uint32_t frames, std::uint32_t channels, double rate, double sample_time);
    void render_input(Node& n, double* out, std::uint32_t frames, std::uint32_t channels);
    // One render cycle: the output node's frames, then the sink nodes' deliveries.
    std::vector<double> cycle(EngineState& e, std::uint32_t frames, std::uint32_t channels, double rate,
                              std::vector<Callback>& callbacks, std::vector<TapDelivery>& taps);
    void render(Address engine, float* mix, std::uint32_t frames, std::uint32_t channels, double rate);
    // A manual render of `frames` frames into an AudioBufferList of the manual format.
    std::int64_t render_manual(Address engine, std::uint32_t frames, Address list, Address error_out);
    // The rate the engine renders at (the device's, or the manual format's); 0 with no clock.
    double clock_rate(const EngineState& e) const { return e.manual ? e.manual_pcm.rate : e.io ? e.io->rate() : 0.0; }
    void set_manual(Address engine, bool manual);

    Support& support_;
    std::shared_ptr<security::Bridge> bridge_;
    std::mutex mutex_;                 // every engine and node
    Address output_class_{}, input_class_{}, mixer_class_{}, player_class_{}, source_class_{}, sink_class_{};
    bool input_short_{};               // the manual input block gave no audio this cycle (the lock is held)
};

// Adds `frames` frames of `source` (in `source_channels`) into `mix` (in `channels`).
void mix_into(double* mix, std::uint32_t channels, const double* source, std::uint32_t source_channels, std::uint32_t frames, double volume, double pan) {
    const double left = std::min(1.0, 1.0 - pan), right = std::min(1.0, 1.0 + pan);
    for (std::uint32_t f = 0; f < frames; ++f) {
        const double* in = source + static_cast<std::size_t>(f) * source_channels;
        double* out = mix + static_cast<std::size_t>(f) * channels;
        if (source_channels == 1) {
            for (std::uint32_t c = 0; c < channels; ++c) out[c] += in[0] * volume * (channels >= 2 ? (c == 0 ? left : c == 1 ? right : 1.0) : 1.0);
        } else {
            for (std::uint32_t c = 0; c < channels && c < source_channels; ++c)
                out[c] += in[c] * volume * (source_channels == 2 && c == 0 ? left : source_channels == 2 && c == 1 ? right : 1.0);
        }
    }
}

Address Engines::make_node(Address cls, Kind kind) {
    const auto object = support_.send_id(support_.send_id(cls, "alloc"), "init");
    support_.objc.host_state<Node>(object)->kind = kind;
    return object;
}

std::pair<double, std::uint32_t> Engines::device_format(EngineState& e) {
    if (e.manual) return {e.manual_pcm.rate, e.manual_pcm.channels};
    if (!e.io) e.io = hal_device_io(hal_default_output());
    if (e.io) return {e.io->rate(), std::max(1U, e.io->channels())};
    return {44100.0, 2};
}

std::uint32_t Engines::channels_of(Address format) {
    const auto pcm = format_pcm(support_, format);
    return pcm ? pcm->channels : 0;
}

double Engines::rate_of(Address format) {
    const auto pcm = format_pcm(support_, format);
    return pcm ? pcm->rate : 0;
}

Address Engines::output_node(Address engine_object) {
    auto e = engine(engine_object);
    if (!e->output) {
        const auto made = make_node(output_class_, Kind::output);
        const auto [rate, channels] = device_format(*e);
        const auto format = standard_format(support_, rate, channels);
        {
            std::lock_guard lock(mutex_);
            e->output = made;
            node(made)->output_format = format;
        }
        attach(engine_object, made);
    }
    return e->output;
}

Address Engines::main_mixer(Address engine_object) {
    auto e = engine(engine_object);
    if (!e->mixer) {
        const auto output = output_node(engine_object);
        const auto made = make_node(mixer_class_, Kind::mixer);
        const auto [rate, channels] = device_format(*e);
        const auto format = standard_format(support_, rate, channels);
        {
            std::lock_guard lock(mutex_);
            e->mixer = made;
            node(made)->output_format = format;
        }
        attach(engine_object, made);
        connect(engine_object, made, output, 0, 0, format);
    }
    return e->mixer;
}

void Engines::attach(Address engine_object, Address node_object) {
    if (!node_object) unsupported("-[AVAudioEngine attachNode:] with nil");
    const auto n = node(node_object);
    auto e = engine(engine_object);
    std::lock_guard lock(mutex_);
    if (n->engine == engine_object) return;
    if (n->engine) unsupported("-[AVAudioEngine attachNode:] with a node attached to another engine (an exception on macOS)");
    n->engine = engine_object;
    e->attached.push_back(support_.retain(node_object));
}

void Engines::detach(Address engine_object, Address node_object) {
    const auto n = node(node_object);
    if (n->engine != engine_object) return;
    disconnect_input(node_object, std::nullopt);
    disconnect_output(engine_object, node_object);
    auto e = engine(engine_object);
    {
        std::lock_guard lock(mutex_);
        n->engine = 0;
        std::erase(e->attached, node_object);
    }
    support_.release(node_object);
}

Address Engines::output_format(Address node_object) {
    const auto n = node(node_object);
    std::lock_guard lock(mutex_);
    return n->output_format;
}

void Engines::connect(Address engine_object, Address source, Address destination, std::uint32_t from_bus, std::optional<std::uint32_t> to_bus,
                      Address format) {
    if (!source || !destination) unsupported("-[AVAudioEngine connect:to:...] with a nil node (an exception on macOS)");
    const auto s = node(source), d = node(destination);
    if (s->engine != engine_object || d->engine != engine_object)
        unsupported("-[AVAudioEngine connect:to:...] with a node not attached to the engine (an exception on macOS)");
    if (from_bus != 0) unsupported("-[AVAudioEngine connect:to:...] from an output bus other than 0 (Alchemy nodes have one output)");
    if (s->kind == Kind::output || s->kind == Kind::sink) unsupported("-[AVAudioEngine connect:to:...] from a node with no output (the output node, a sink node)");
    // A nil format: the source's output format (a player's default is 44.1 kHz stereo).
    Address chosen = format;
    if (!chosen) chosen = output_format(source);
    Address made{};
    if (!chosen) chosen = made = standard_format(support_, 44100, 2);
    if (format && !format_pcm(support_, format)) unsupported("-[AVAudioEngine connect:to:...] with a format that is not linear PCM");
    const auto chosen_channels = std::max(1U, channels_of(chosen));
    support_.retain(chosen);
    support_.retain(chosen);
    std::vector<Address> released;
    {
        std::lock_guard lock(mutex_);
        // A source feeds one destination: an earlier connection goes.
        auto e = engine(engine_object);
        for (const auto other : e->attached) {
            const auto o = support_.objc.find_host_state<Node>(other);
            if (!o) continue;
            for (auto it = o->inputs.begin(); it != o->inputs.end();) {
                if (it->second != source) { ++it; continue; }
                if (const auto f = o->input_formats.find(it->first); f != o->input_formats.end()) {
                    released.push_back(f->second);
                    o->input_formats.erase(f);
                }
                o->input_channels.erase(it->first);
                it = o->inputs.erase(it);
            }
        }
        std::uint32_t bus = 0;
        if (to_bus) bus = *to_bus;
        else if (d->kind == Kind::mixer) while (d->inputs.contains(bus)) ++bus;
        if (d->kind == Kind::player || d->kind == Kind::source || d->kind == Kind::input)
            unsupported("-[AVAudioEngine connect:to:...] to a node with no input (a player, source or input node)");
        if (d->kind != Kind::mixer && bus != 0) unsupported("-[AVAudioEngine connect:to:...] to an input bus other than 0 of a node that is not a mixer");
        if (const auto f = d->input_formats.find(bus); f != d->input_formats.end()) released.push_back(f->second);
        d->inputs[bus] = source;
        d->input_formats[bus] = chosen;
        d->input_channels[bus] = chosen_channels;
        released.push_back(std::exchange(s->output_format, chosen));
    }
    for (const auto object : released) support_.release(object);
    support_.release(made);
}

void Engines::disconnect_input(Address node_object, std::optional<std::uint32_t> bus) {
    const auto n = node(node_object);
    std::vector<Address> released;
    {
        std::lock_guard lock(mutex_);
        for (auto it = n->inputs.begin(); it != n->inputs.end();) {
            if (bus && it->first != *bus) { ++it; continue; }
            if (const auto f = n->input_formats.find(it->first); f != n->input_formats.end()) {
                released.push_back(f->second);
                n->input_formats.erase(f);
            }
            n->input_channels.erase(it->first);
            it = n->inputs.erase(it);
        }
    }
    for (const auto object : released) support_.release(object);
}

void Engines::disconnect_output(Address engine_object, Address node_object) {
    std::vector<Address> released;
    {
        std::lock_guard lock(mutex_);
        const auto e = engine(engine_object);
        for (const auto other : e->attached) {
            const auto o = support_.objc.find_host_state<Node>(other);
            if (!o) continue;
            for (auto it = o->inputs.begin(); it != o->inputs.end();) {
                if (it->second != node_object) { ++it; continue; }
                if (const auto f = o->input_formats.find(it->first); f != o->input_formats.end()) {
                    released.push_back(f->second);
                    o->input_formats.erase(f);
                }
                o->input_channels.erase(it->first);
                it = o->inputs.erase(it);
            }
        }
    }
    for (const auto object : released) support_.release(object);
}

bool Engines::start(Address engine_object, Address error_out) {
    auto e = engine(engine_object);
    const auto fail_with = [&](std::int32_t status, std::string_view text) {
        if (error_out) bridge_->store_error(error_out, bridge_->os_status_error(status, text), true);
        return false;
    };
    {
        std::lock_guard lock(mutex_);
        if (e->running) return true;
        if (e->manual) {
            if (!e->thread) e->thread = start_completion_thread(support_.process, "com.apple.avfaudio.engine.callbacks");
            e->running = true;
            return true;
        }
        if (e->input)
            for (const auto other : e->attached)
                if (const auto o = support_.objc.find_host_state<Node>(other))
                    for (const auto& [bus, source] : o->inputs)
                        if (source == e->input) return fail_with(no_device, "AVAudioEngine: there is no audio input device under Alchemy");
    }
    static_cast<void>(output_node(engine_object));
    static_cast<void>(device_format(*e));
    if (!e->io) return fail_with(no_device, "AVAudioEngine: there is no audio output device");
    if (!e->thread) e->thread = start_completion_thread(support_.process, "com.apple.avfaudio.engine.callbacks");
    const auto owner = shared_from_this();
    const auto rate = e->io->rate();
    const auto channels = e->io->channels();
    {
        std::lock_guard lock(mutex_);
        e->running = true;
    }
    const auto status = e->io->start(engine_object, [owner, engine_object, channels, rate](float* mix, std::uint32_t frames, const hal_io::Timing&) {
        owner->render(engine_object, mix, frames, channels, rate);
    }, hal_buffer_frames(hal_default_output()));
    if (status) {
        std::lock_guard lock(mutex_);
        e->running = false;
        return fail_with(status, "AVAudioEngine: the output device did not start");
    }
    return true;
}

void Engines::stop(Address engine_object) {
    auto e = engine(engine_object);
    std::shared_ptr<hal_io::DeviceIo> io;
    {
        std::lock_guard lock(mutex_);
        e->running = false;
        io = e->io;
    }
    if (io) io->stop(engine_object);
}

void Engines::post(const std::shared_ptr<EngineState>& e, std::vector<Callback> callbacks, std::vector<TapDelivery> taps) {
    if (callbacks.empty() && taps.empty()) return;
    const auto thread = e->thread;
    auto* sp = &support_;
    const auto b = bridge_;
    const auto owner = shared_from_this();
    const auto run = [owner, e, sp, b, callbacks = std::move(callbacks), taps = std::move(taps)] {
        {
            std::lock_guard lock(owner->mutex_);
            e->callback_thread = std::this_thread::get_id();
        }
        for (const auto& c : callbacks) {
            if (c.typed) static_cast<void>(b->invoke_block(c.block, {c.type}));
            else static_cast<void>(b->invoke_block(c.block, {}));
            b->block_release(c.block);
        }
        for (const auto& t : taps) {
            const auto buffer = make_buffer(*sp, t.handle->format, t.frames);
            if (const auto state = buffer ? sp->objc.find_host_state<PcmBuffer>(buffer) : nullptr) {
                const std::vector<double> samples(t.samples.begin(), t.samples.end());
                fill_buffer(*sp, *state, samples, t.channels, t.frames);
            }
            const auto when = make_time(*sp, host_time_now(), t.sample_time, t.rate);
            static_cast<void>(b->invoke_block(t.handle->block, {buffer, when}));
            sp->release(when);
            sp->release(buffer);
        }
    };
    if (thread) thread->post(run);
}

void Engines::render_player(Node& n, double* out, std::uint32_t frames, std::uint32_t channels, double rate, std::vector<Callback>& callbacks) {
    std::fill(out, out + static_cast<std::size_t>(frames) * channels, 0.0);
    if (!n.playing) return;
    const auto skip = static_cast<std::uint32_t>(std::min<std::uint64_t>(n.delay_frames, frames));
    n.delay_frames -= skip;
    if (skip == frames) return;
    const double source_rate = n.schedule.empty() ? rate : n.schedule.front().rate;
    if (!n.resampler || n.resampler_rate != source_rate || n.resampler_channels != channels) {
        n.resampler = std::make_unique<Resampler>(source_rate, rate, channels);
        n.resampler_rate = source_rate;
        n.resampler_channels = channels;
    }
    const auto pull = [&](double* to, std::size_t count) {
        std::fill(to, to + count * channels, 0.0);
        std::size_t done = 0;
        while (done < count && !n.schedule.empty()) {
            auto& item = n.schedule.front();
            const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(count - done, item.frames - item.played));
            for (std::size_t f = 0; f < take; ++f) {
                const float* in = item.samples.data() + static_cast<std::size_t>(item.played + f) * item.channels;
                double* o = to + (done + f) * channels;
                for (std::uint32_t c = 0; c < channels; ++c) o[c] = in[item.channels == 1 ? 0 : std::min(c, item.channels - 1)] * (item.channels == 1 || c < item.channels ? 1.0 : 0.0);
            }
            item.played += take;
            done += take;
            if (item.played < item.frames) continue;
            if (item.loops && item.frames) {
                item.played = 0;
                continue;
            }
            if (item.block) callbacks.push_back({item.block, item.typed, item.callback_type});
            n.schedule.pop_front();
        }
        n.played_frames += static_cast<double>(done);
    };
    n.resampler->render(out + static_cast<std::size_t>(skip) * channels, frames - skip, pull);
}

std::vector<double> Engines::pull(Address node_object, std::uint32_t frames, std::uint32_t channels, double rate, double sample_time, unsigned depth,
                                  std::vector<Callback>& callbacks, std::vector<TapDelivery>& taps) {
    std::vector<double> out(static_cast<std::size_t>(frames) * channels, 0.0);
    const auto n = support_.objc.find_host_state<Node>(node_object);
    if (!n || depth > 32) return out;
    switch (n->kind) {
    case Kind::player:
        render_player(*n, out.data(), frames, channels, rate, callbacks);
        break;
    case Kind::source:
        render_source(*n, out.data(), frames, channels, rate, sample_time);
        break;
    case Kind::input:
        render_input(*n, out.data(), frames, channels);
        break;
    case Kind::mixer:
    case Kind::output:
        for (const auto& [bus, source] : n->inputs) {
            const auto known = n->input_channels.find(bus);
            const auto source_channels = known != n->input_channels.end() ? known->second : channels;
            const auto data = pull(source, frames, source_channels, rate, sample_time, depth + 1, callbacks, taps);
            const auto s = support_.objc.find_host_state<Node>(source);
            const double volume = n->kind == Kind::mixer && s ? s->volume : 1.0, pan = n->kind == Kind::mixer && s ? s->pan : 0.0;
            mix_into(out.data(), channels, data.data(), source_channels, frames, volume, pan);
        }
        if (n->kind == Kind::mixer && n->output_volume != 1.0f)
            for (auto& value : out) value *= n->output_volume;
        break;
    default:
        break;
    }
    if (n->tap) {
        auto& t = *n->tap;
        for (std::uint32_t f = 0; f < frames; ++f)
            for (std::uint32_t c = 0; c < t.channels; ++c) t.pending.push_back(static_cast<float>(c < channels ? out[static_cast<std::size_t>(f) * channels + c] : 0.0));
        while (t.pending.size() >= static_cast<std::size_t>(t.frames) * t.channels) {
            const auto count = static_cast<std::size_t>(t.frames) * t.channels;
            TapDelivery d{t.handle, std::vector<float>(t.pending.begin(), t.pending.begin() + static_cast<std::ptrdiff_t>(count)),
                          t.channels, t.frames, sample_time, rate};
            t.pending.erase(t.pending.begin(), t.pending.begin() + static_cast<std::ptrdiff_t>(count));
            taps.push_back(std::move(d));
        }
    }
    return out;
}

std::vector<double> Engines::cycle(EngineState& e, std::uint32_t frames, std::uint32_t channels, double rate,
                                   std::vector<Callback>& callbacks, std::vector<TapDelivery>& taps) {
    input_short_ = false;
    auto out = pull(e.output, frames, channels, rate, e.sample_time, 0, callbacks, taps);
    for (const auto object : e.attached) {
        const auto n = support_.objc.find_host_state<Node>(object);
        if (!n || n->kind != Kind::sink || !n->block || !n->inputs.contains(0)) continue;
        const auto source_channels = n->input_channels.contains(0) ? n->input_channels.at(0) : channels;
        const auto data = pull(n->inputs.at(0), frames, source_channels, rate, e.sample_time, 1, callbacks, taps);
        const auto format = n->input_formats.contains(0) ? format_pcm(support_, n->input_formats.at(0)) : std::nullopt;
        if (!format) continue;
        if (!n->scratch || n->scratch->pcm.channels != format->channels || n->scratch->pcm.frame_bytes() != format->frame_bytes() ||
            n->scratch->pcm.non_interleaved != format->non_interleaved || n->scratch->pcm.is_float != format->is_float)
            n->scratch = std::make_unique<GuestList>(support_, *format);
        n->scratch->prepare(frames, e.sample_time);
        write_list(support_, n->scratch->pcm, n->scratch->list, data, source_channels, frames);
        // OSStatus (^)(const AudioTimeStamp *timestamp, AVAudioFrameCount frameCount, const AudioBufferList *inputData)
        static_cast<void>(bridge_->invoke_block(n->block, {n->scratch->stamp, frames, n->scratch->list}));
    }
    e.sample_time += frames;
    return out;
}

void Engines::render(Address engine_object, float* mix, std::uint32_t frames, std::uint32_t channels, double rate) {
    std::vector<Callback> callbacks;
    std::vector<TapDelivery> taps;
    std::shared_ptr<EngineState> e;
    {
        std::lock_guard lock(mutex_);
        e = support_.objc.find_host_state<EngineState>(engine_object);
        if (!e || !e->running || !e->output || e->manual) return;
        const auto out = cycle(*e, frames, channels, rate, callbacks, taps);
        for (std::size_t i = 0; i < out.size(); ++i) mix[i] += static_cast<float>(out[i]);
    }
    post(e, std::move(callbacks), std::move(taps));
}

std::int64_t Engines::render_manual(Address engine_object, std::uint32_t frames, Address list, Address error_out) {
    const auto fail_with = [&](std::int32_t status, std::string_view text) {
        if (error_out) support_.memory().write32(error_out, static_cast<std::uint32_t>(status));
        static_cast<void>(text);
        return manual_error;
    };
    std::vector<Callback> callbacks;
    std::vector<TapDelivery> taps;
    std::shared_ptr<EngineState> e;
    bool short_input{};
    {
        std::lock_guard lock(mutex_);
        e = support_.objc.find_host_state<EngineState>(engine_object);
        if (!e || !e->manual) return fail_with(manual_invalid_mode, "not in manual rendering mode");
        if (!e->running || !e->output) return fail_with(manual_not_running, "the engine is not running");
        if (frames > e->manual_maximum) return fail_with(-50, "more frames than the maximum frame count");
        const auto channels = e->manual_pcm.channels;
        const auto out = cycle(*e, frames, channels, e->manual_pcm.rate, callbacks, taps);
        write_list(support_, e->manual_pcm, list, out, channels, frames);
        short_input = input_short_;
    }
    post(e, std::move(callbacks), std::move(taps));
    if (error_out) support_.memory().write32(error_out, 0);
    return short_input ? manual_insufficient_input : manual_success;
}

void Engines::render_source(Node& n, double* out, std::uint32_t frames, std::uint32_t channels, double rate, double sample_time) {
    std::fill(out, out + static_cast<std::size_t>(frames) * channels, 0.0);
    if (!n.block) return;
    const auto format = format_pcm(support_, n.source_format ? n.source_format : n.output_format);
    if (!format) return;
    if (!n.scratch || n.scratch->pcm.rate != format->rate || n.scratch->pcm.channels != format->channels ||
        n.scratch->pcm.frame_bytes() != format->frame_bytes() || n.scratch->pcm.non_interleaved != format->non_interleaved ||
        n.scratch->pcm.is_float != format->is_float)
        n.scratch = std::make_unique<GuestList>(support_, *format);
    auto& scratch = *n.scratch;
    // The block renders in its format's channels and rate; the resampler maps them to the engine's.
    const auto block_channels = format->channels;
    double block_time = sample_time * format->rate / rate;
    const auto call = [&](double* into, std::size_t count) {
        for (std::size_t done = 0; done < count;) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::size_t>(count - done, 4096));
            scratch.prepare(chunk, block_time);
            const auto silence = support_.process.allocate_guest(8, 8);
            support_.memory().write64(silence, 0);
            // OSStatus (^)(BOOL *isSilence, const AudioTimeStamp *timestamp, AVAudioFrameCount frameCount, AudioBufferList *outputData)
            const auto status = static_cast<std::int32_t>(bridge_->invoke_block(n.block, {silence, scratch.stamp, chunk, scratch.list}));
            const bool silent = (support_.memory().read8(silence) != 0) || status != 0;
            support_.process.release_guest(silence);
            const auto samples = silent ? std::vector<double>(static_cast<std::size_t>(chunk) * block_channels, 0.0)
                                        : read_list(support_, scratch.pcm, scratch.list, chunk);
            for (std::size_t f = 0; f < chunk; ++f)
                for (std::uint32_t c = 0; c < channels; ++c)
                    into[(done + f) * channels + c] = block_channels == 1 ? samples[f] : c < block_channels ? samples[f * block_channels + c] : 0.0;
            done += chunk;
            block_time += chunk;
        }
    };
    if (format->rate == rate) return call(out, frames);
    if (!n.resampler || n.resampler_rate != format->rate || n.resampler_channels != channels) {
        n.resampler = std::make_unique<Resampler>(format->rate, rate, channels);
        n.resampler_rate = format->rate;
        n.resampler_channels = channels;
    }
    n.resampler->render(out, frames, call);
}

void Engines::render_input(Node& n, double* out, std::uint32_t frames, std::uint32_t channels) {
    std::fill(out, out + static_cast<std::size_t>(frames) * channels, 0.0);
    const auto e = n.engine ? support_.objc.find_host_state<EngineState>(n.engine) : nullptr;
    if (!e || !e->manual || !e->input_block) return;
    // const AudioBufferList * (^)(AVAudioFrameCount inNumberOfFrames): nil when there is no input now.
    const auto list = bridge_->invoke_block(e->input_block, {frames});
    if (!list) { input_short_ = true; return; }
    const auto samples = read_list(support_, e->input_pcm, list, frames);
    const auto in_channels = e->input_pcm.channels;
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c)
            out[static_cast<std::size_t>(f) * channels + c] = in_channels == 1 ? samples[f] : c < in_channels ? samples[static_cast<std::size_t>(f) * in_channels + c] : 0.0;
}

void Engines::set_manual(Address engine_object, bool manual) {
    auto e = engine(engine_object);
    {
        std::lock_guard lock(mutex_);
        e->manual = manual;
    }
    // The output and main mixer render in the new format.
    const auto [rate, channels] = device_format(*e);
    std::vector<Address> released;
    {
        std::lock_guard lock(mutex_);
        for (const auto object : {e->output, e->mixer}) {
            const auto n = object ? support_.objc.find_host_state<Node>(object) : nullptr;
            if (!n) continue;
            released.push_back(std::exchange(n->output_format, standard_format(support_, rate, channels)));
        }
        if (const auto o = e->output ? support_.objc.find_host_state<Node>(e->output) : nullptr)
            for (auto& [bus, source] : o->inputs)
                if (source == e->mixer) {
                    o->input_channels[bus] = channels;
                    released.push_back(std::exchange(o->input_formats[bus], support_.retain(o->output_format)));
                }
    }
    for (const auto object : released) support_.release(object);
}

void Engines::stop_player(Address node_object, bool clear) {
    const auto n = node(node_object);
    std::vector<Callback> callbacks;
    std::shared_ptr<EngineState> e;
    {
        std::lock_guard lock(mutex_);
        n->playing = false;
        n->delay_frames = 0;
        n->played_frames = 0;
        if (n->resampler) n->resampler->reset();
        if (clear) {
            for (auto& item : n->schedule)
                if (item.block) callbacks.push_back({item.block, item.typed, item.callback_type});
            n->schedule.clear();
        }
        if (n->engine) e = support_.objc.find_host_state<EngineState>(n->engine);
    }
    if (callbacks.empty()) return;
    if (e && e->thread) return post(e, std::move(callbacks), {});
    // No engine thread yet: the handlers run here.
    for (const auto& c : callbacks) {
        if (c.typed) static_cast<void>(bridge_->invoke_block(c.block, {c.type}));
        else static_cast<void>(bridge_->invoke_block(c.block, {}));
        bridge_->block_release(c.block);
    }
}

void Engines::schedule(Address player, Scheduled item, Address when, std::uint64_t options) {
    if (when) {
        const auto t = support_.objc.find_host_state<TimeState>(when);
        if (t && (t->host_valid || t->sample_valid))
            unsupported("-[AVAudioPlayerNode schedule...:atTime:...] with a time (Alchemy plays scheduled audio in order, immediately)");
    }
    const auto n = node(player);
    std::vector<Callback> dropped;
    {
        std::lock_guard lock(mutex_);
        item.loops = (options & option_loops) != 0;
        if (options & option_interrupts) {
            for (auto& old : n->schedule)
                if (old.block) dropped.push_back({old.block, old.typed, old.callback_type});
            n->schedule.clear();
        } else if (options & option_interrupts_at_loop) {
            if (!n->schedule.empty()) {
                n->schedule.front().loops = false;
                for (std::size_t i = 1; i < n->schedule.size(); ++i)
                    if (n->schedule[i].block) dropped.push_back({n->schedule[i].block, n->schedule[i].typed, n->schedule[i].callback_type});
                n->schedule.resize(1);
            }
        }
        n->schedule.push_back(std::move(item));
    }
    for (const auto& c : dropped) {
        if (c.typed) static_cast<void>(bridge_->invoke_block(c.block, {c.type}));
        else static_cast<void>(bridge_->invoke_block(c.block, {}));
        bridge_->block_release(c.block);
    }
}

void Engines::install() {
    auto& support = support_;
    const auto self = shared_from_this();
    auto* sp = &support_;
    const auto real32 = [](const CpuState& s, unsigned i) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[i][0])); };
    const auto return_real32 = [](CpuState& s, float value) { s.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; };

    const auto node_class = declare(support, "AVAudioNode", "NSObject");
    const auto io_class = declare(support, "AVAudioIONode", "AVAudioNode");
    output_class_ = declare(support, "AVAudioOutputNode", "AVAudioIONode");
    input_class_ = declare(support, "AVAudioInputNode", "AVAudioIONode");
    mixer_class_ = declare(support, "AVAudioMixerNode", "AVAudioNode");
    player_class_ = declare(support, "AVAudioPlayerNode", "AVAudioNode");
    source_class_ = declare(support, "AVAudioSourceNode", "AVAudioNode");
    sink_class_ = declare(support, "AVAudioSinkNode", "AVAudioNode");
    const auto engine_class = declare(support, "AVAudioEngine", "NSObject");
    static_cast<void>(io_class);
    for (const auto library : {avf_audio, av_foundation})
        bridge_->export_string(library, "_AVAudioEngineConfigurationChangeNotification", "AVAudioEngineConfigurationChangeNotification");

    // ---- nodes ---------------------------------------------------------------------------------
    for (const auto& [cls, kind] : std::initializer_list<std::pair<Address, Kind>>{{mixer_class_, Kind::mixer}, {player_class_, Kind::player}}) {
        const auto target = cls;
        const auto node_kind = kind;
        support.method(target, "init", [sp, target, node_kind](CpuState& s, GuestMemory&) {
            const auto object = sp->objc.send_super(s.x[0], target, sp->objc.selector("init")).x0;
            if (object) sp->objc.host_state<Node>(object)->kind = node_kind;
            s.x[0] = object;
        }, "@@:");
    }
    support.method(node_class, "dealloc", [self, sp, node_class](CpuState& s, GuestMemory&) {
        if (const auto n = sp->objc.find_host_state<Node>(s.x[0])) {
            std::vector<Address> released{n->output_format};
            for (const auto& [bus, format] : n->input_formats) released.push_back(format);
            for (auto& item : n->schedule)
                if (item.block) self->bridge_->block_release(item.block);
            n->schedule.clear();
            n->input_formats.clear();
            n->output_format = 0;
            n->tap.reset();   // the tap's block and format go with its last delivery
            released.push_back(std::exchange(n->source_format, 0));
            if (n->block) self->bridge_->block_release(std::exchange(n->block, 0));
            n->scratch.reset();
            for (const auto object : released) sp->release(object);
        }
        static_cast<void>(sp->objc.send_super(s.x[0], node_class, sp->objc.selector("dealloc")));
    }, "v@:");
    support.method(node_class, "engine", [self](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = n->engine;
    }, "@@:");
    support.method(node_class, "numberOfInputs", [self](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = n->kind == Kind::mixer ? (n->inputs.empty() ? 1 : n->inputs.rbegin()->first + 2) : n->kind == Kind::output || n->kind == Kind::sink ? 1 : 0;
    }, "Q@:");
    support.method(node_class, "numberOfOutputs", [self](CpuState& s, GuestMemory&) {
        const auto kind = self->node(s.x[0])->kind;
        s.x[0] = kind == Kind::output || kind == Kind::sink ? 0 : 1;
    }, "Q@:");
    support.method(node_class, "outputFormatForBus:", [self, sp](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        if (n->kind == Kind::input) {   // no input device: the manual rendering input's format, if any
            const auto e = n->engine ? sp->objc.find_host_state<EngineState>(n->engine) : nullptr;
            s.x[0] = s.x[2] == 0 && e && e->manual ? e->input_format : 0;
            return;
        }
        s.x[0] = s.x[2] == 0 ? n->output_format : 0;
        if (!s.x[0] && s.x[2] == 0 && n->kind == Kind::player) {
            n->output_format = standard_format(self->support_, 44100, 2);
            s.x[0] = n->output_format;
        }
    }, "@@:Q");
    support.method(node_class, "inputFormatForBus:", [self](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        const auto bus = static_cast<std::uint32_t>(s.x[2]);
        if (n->kind == Kind::input || n->kind == Kind::player || n->kind == Kind::source) { s.x[0] = 0; return; }
        if (const auto f = n->input_formats.find(bus); f != n->input_formats.end()) { s.x[0] = f->second; return; }
        s.x[0] = n->output_format;   // an unconnected bus: the node's own format
    }, "@@:Q");
    support.method(node_class, "nameForInputBus:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:Q");
    support.method(node_class, "nameForOutputBus:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:Q");
    support.method(node_class, "latency", [](CpuState& s, GuestMemory&) { s.v[0] = {0, 0}; }, "d@:");
    support.method(node_class, "outputPresentationLatency", [](CpuState& s, GuestMemory&) { s.v[0] = {0, 0}; }, "d@:");
    support.method(node_class, "reset", [self](CpuState& s, GuestMemory&) {
        if (self->node(s.x[0])->kind == Kind::player) self->stop_player(s.x[0], true);
    }, "v@:");
    // -lastRenderTime: the engine's sample time now (nil while the engine is not running).
    support.method(node_class, "lastRenderTime", [self, sp](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        double sample{}, rate{};
        {
            std::lock_guard lock(self->mutex_);
            const auto e = n->engine ? sp->objc.find_host_state<EngineState>(n->engine) : nullptr;
            if (!e || !e->running || !self->clock_rate(*e)) { s.x[0] = 0; return; }
            sample = e->sample_time;
            rate = self->clock_rate(*e);
        }
        s.x[0] = sp->send_id(make_time(*sp, host_time_now(), sample, rate), "autorelease");
    }, "@@:");
    // -installTapOnBus:bufferSize:format:block: (bus x2, size x3, format x4, block x5)
    support.method(node_class, "installTapOnBus:bufferSize:format:block:", [self, sp](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        if (s.x[2] != 0) unsupported("-[AVAudioNode installTapOnBus:...] on a bus other than 0");
        if (!s.x[5]) unsupported("-[AVAudioNode installTapOnBus:...] without a block");
        Address format = s.x[4];
        if (!format) format = sp->send_id(s.x[0], "outputFormatForBus:", {0});
        if (!format) unsupported("-[AVAudioNode installTapOnBus:...] on a node without an output format (the input node: no input device)");
        double engine_rate{};
        {
            std::lock_guard lock(self->mutex_);
            const auto e = n->engine ? sp->objc.find_host_state<EngineState>(n->engine) : nullptr;
            if (n->tap) unsupported("-[AVAudioNode installTapOnBus:...] on a bus with a tap (an exception on macOS)");
            engine_rate = e ? self->device_format(*e).first : 0;
        }
        if (engine_rate > 0 && self->rate_of(format) != engine_rate)
            unsupported("-[AVAudioNode installTapOnBus:...] with a format whose rate is not the engine's (Alchemy nodes render at the engine's rate)");
        Tap t;
        t.handle = std::make_shared<TapHandle>(*sp, self->bridge_, self->bridge_->block_copy(s.x[5]), sp->retain(format));
        t.frames = std::max<std::uint32_t>(static_cast<std::uint32_t>(s.x[3]), 64);
        t.channels = std::max(1U, self->channels_of(format));
        std::lock_guard lock(self->mutex_);
        n->tap = std::move(t);
    }, "v@:QI@@?");
    support.method(node_class, "removeTapOnBus:", [self](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::optional<Tap> removed;
        {
            std::lock_guard lock(self->mutex_);
            removed = std::exchange(n->tap, std::nullopt);
        }
        // The tap goes here, unlocked (its block and format with it, unless a delivery is in flight).
    }, "v@:Q");
    // AVAudioMixing (players, mixers, the input node): applied by the destination mixer.
    for (const auto cls : {player_class_, mixer_class_, input_class_, source_class_}) {
        for (const auto& [getter, setter, field, low, high] : std::initializer_list<std::tuple<const char*, const char*, float Node::*, float, float>>{
                 {"volume", "setVolume:", &Node::volume, 0.0f, 1.0f}, {"pan", "setPan:", &Node::pan, -1.0f, 1.0f}}) {
            const auto member = field;
            const auto lo = low, hi = high;
            support.method(cls, getter, [self, member, return_real32](CpuState& s, GuestMemory&) {
                const auto n = self->node(s.x[0]);
                std::lock_guard lock(self->mutex_);
                return_real32(s, (*n).*member);
            }, "f@:");
            support.method(cls, setter, [self, member, real32, lo, hi](CpuState& s, GuestMemory&) {
                const auto n = self->node(s.x[0]);
                std::lock_guard lock(self->mutex_);
                (*n).*member = std::clamp(real32(s, 0), lo, hi);
            }, "v@:f");
        }
    }
    support.method(mixer_class_, "outputVolume", [self, return_real32](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        return_real32(s, n->output_volume);
    }, "f@:");
    support.method(mixer_class_, "setOutputVolume:", [self, real32](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        n->output_volume = std::clamp(real32(s, 0), 0.0f, 1.0f);
    }, "v@:f");
    support.method(mixer_class_, "nextAvailableInputBus", [self](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        std::uint64_t bus = 0;
        while (n->inputs.contains(static_cast<std::uint32_t>(bus))) ++bus;
        s.x[0] = bus;
    }, "Q@:");

    // ---- AVAudioPlayerNode ---------------------------------------------------------------------
    const auto player = player_class_;
    // The audio of a buffer, file or segment, ready to schedule.
    const auto from_buffer = [sp](Address buffer) {
        const auto b = buffer ? sp->objc.find_host_state<PcmBuffer>(buffer) : nullptr;
        if (!b) unsupported("-[AVAudioPlayerNode scheduleBuffer:...] with an object that is not an AVAudioPCMBuffer");
        Scheduled item;
        item.samples = buffer_samples(*sp, *b);
        item.channels = b->pcm.channels;
        item.rate = b->pcm.rate;
        item.frames = b->length;
        return item;
    };
    const auto from_file = [sp](Address file, std::uint64_t start, std::uint64_t count) {
        const auto f = file ? sp->objc.find_host_state<AudioFileState>(file) : nullptr;
        if (!f || !f->open) unsupported("-[AVAudioPlayerNode scheduleFile:...] with a file that is not open for reading");
        Decoded decoded;
        if (decode_file(*sp, f->audio, decoded, start, count)) unsupported("-[AVAudioPlayerNode scheduleFile:...] with a file too large to decode");
        Scheduled item;
        item.channels = decoded.channels;
        item.rate = decoded.rate;
        item.frames = decoded.frames();
        item.samples = std::move(decoded.samples);
        return item;
    };
    const auto with_handler = [self](Scheduled item, Address block, bool typed, std::uint64_t type) {
        item.block = block ? self->bridge_->block_copy(block) : 0;
        item.typed = typed;
        item.callback_type = type;
        return item;
    };
    // scheduleBuffer:completionHandler: (buffer x2, handler x3)
    support.method(player, "scheduleBuffer:completionHandler:", [self, from_buffer, with_handler](CpuState& s, GuestMemory&) {
        self->schedule(s.x[0], with_handler(from_buffer(s.x[2]), s.x[3], false, 0), 0, 0);
    }, "v@:@@?");
    // scheduleBuffer:atTime:options:completionHandler: (buffer x2, time x3, options x4, handler x5)
    support.method(player, "scheduleBuffer:atTime:options:completionHandler:", [self, from_buffer, with_handler](CpuState& s, GuestMemory&) {
        self->schedule(s.x[0], with_handler(from_buffer(s.x[2]), s.x[5], false, 0), s.x[3], s.x[4]);
    }, "v@:@@Q@?");
    // scheduleBuffer:completionCallbackType:completionHandler: (buffer x2, type x3, handler x4)
    support.method(player, "scheduleBuffer:completionCallbackType:completionHandler:", [self, from_buffer, with_handler](CpuState& s, GuestMemory&) {
        self->schedule(s.x[0], with_handler(from_buffer(s.x[2]), s.x[4], true, s.x[3]), 0, 0);
    }, "v@:@q@?");
    // scheduleBuffer:atTime:options:completionCallbackType:completionHandler: (x2..x6)
    support.method(player, "scheduleBuffer:atTime:options:completionCallbackType:completionHandler:", [self, from_buffer, with_handler](CpuState& s, GuestMemory&) {
        self->schedule(s.x[0], with_handler(from_buffer(s.x[2]), s.x[6], true, s.x[5]), s.x[3], s.x[4]);
    }, "v@:@@Qq@?");
    // scheduleFile:atTime:completionHandler: (file x2, time x3, handler x4)
    support.method(player, "scheduleFile:atTime:completionHandler:", [self, from_file, with_handler](CpuState& s, GuestMemory&) {
        self->schedule(s.x[0], with_handler(from_file(s.x[2], 0, std::numeric_limits<std::uint64_t>::max()), s.x[4], false, 0), s.x[3], 0);
    }, "v@:@@@?");
    support.method(player, "scheduleFile:atTime:completionCallbackType:completionHandler:", [self, from_file, with_handler](CpuState& s, GuestMemory&) {
        self->schedule(s.x[0], with_handler(from_file(s.x[2], 0, std::numeric_limits<std::uint64_t>::max()), s.x[5], true, s.x[4]), s.x[3], 0);
    }, "v@:@@q@?");
    // scheduleSegment:startingFrame:frameCount:atTime:completionHandler: (file x2, start x3, count x4, time x5, handler x6)
    support.method(player, "scheduleSegment:startingFrame:frameCount:atTime:completionHandler:", [self, from_file, with_handler](CpuState& s, GuestMemory&) {
        self->schedule(s.x[0], with_handler(from_file(s.x[2], s.x[3], static_cast<std::uint32_t>(s.x[4])), s.x[6], false, 0), s.x[5], 0);
    }, "v@:@qI@@?");
    support.method(player, "scheduleSegment:startingFrame:frameCount:atTime:completionCallbackType:completionHandler:",
                   [self, from_file, with_handler](CpuState& s, GuestMemory&) {
        self->schedule(s.x[0], with_handler(from_file(s.x[2], s.x[3], static_cast<std::uint32_t>(s.x[4])), s.x[7], true, s.x[6]), s.x[5], 0);
    }, "v@:@qI@q@?");
    const auto play = [self, sp](Address player_object, Address when) {
        const auto n = self->node(player_object);
        std::lock_guard lock(self->mutex_);
        if (n->playing) return;
        const auto e = n->engine ? sp->objc.find_host_state<EngineState>(n->engine) : nullptr;
        if (!e || !e->running) unsupported("-[AVAudioPlayerNode play] while its engine is not running (an exception on macOS)");
        n->playing = true;
        n->played_frames = 0;
        n->start_node_sample = e->sample_time;
        n->delay_frames = 0;
        if (const auto t = when ? sp->objc.find_host_state<TimeState>(when) : nullptr; t && t->host_valid && self->clock_rate(*e)) {
            const auto now = host_time_now();
            if (t->host > now) n->delay_frames = static_cast<std::uint64_t>(static_cast<double>(t->host - now) / 1e9 * self->clock_rate(*e));
        } else if (t && t->sample_valid && self->clock_rate(*e)) {
            if (t->sample > e->sample_time) n->delay_frames = static_cast<std::uint64_t>(t->sample - e->sample_time);
        }
        n->start_node_sample += static_cast<double>(n->delay_frames);
    };
    support.method(player, "play", [play](CpuState& s, GuestMemory&) { play(s.x[0], 0); }, "v@:");
    support.method(player, "playAtTime:", [play](CpuState& s, GuestMemory&) { play(s.x[0], s.x[2]); }, "v@:@");
    support.method(player, "pause", [self](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        n->playing = false;
    }, "v@:");
    support.method(player, "stop", [self](CpuState& s, GuestMemory&) { self->stop_player(s.x[0], true); }, "v@:");
    support.method(player, "isPlaying", [self](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = n->playing ? 1 : 0;
    }, "B@:");
    support.method(player, "prepareWithFrameCount:", [](CpuState&, GuestMemory&) {}, "v@:I");
    // Player time = the player's frames since -play (its first scheduled audio's rate).
    support.method(player, "playerTimeForNodeTime:", [self, sp](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        const auto t = s.x[2] ? sp->objc.find_host_state<TimeState>(s.x[2]) : nullptr;
        double sample{}, rate{};
        {
            std::lock_guard lock(self->mutex_);
            const auto e = n->engine ? sp->objc.find_host_state<EngineState>(n->engine) : nullptr;
            if (!t || !t->sample_valid || !n->playing || !e || !self->clock_rate(*e)) { s.x[0] = 0; return; }
            const double engine_rate = self->clock_rate(*e);
            rate = n->resampler_rate > 0 ? n->resampler_rate : engine_rate;
            sample = (t->sample - n->start_node_sample) * rate / engine_rate;
        }
        s.x[0] = sp->send_id(make_time(*sp, t->host_valid ? std::optional(t->host) : std::nullopt, sample, rate), "autorelease");
    }, "@@:@");
    support.method(player, "nodeTimeForPlayerTime:", [self, sp](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        const auto t = s.x[2] ? sp->objc.find_host_state<TimeState>(s.x[2]) : nullptr;
        double sample{}, engine_rate{};
        {
            std::lock_guard lock(self->mutex_);
            const auto e = n->engine ? sp->objc.find_host_state<EngineState>(n->engine) : nullptr;
            if (!t || !t->sample_valid || !n->playing || !e || !self->clock_rate(*e)) { s.x[0] = 0; return; }
            engine_rate = self->clock_rate(*e);
            const double rate = n->resampler_rate > 0 ? n->resampler_rate : engine_rate;
            sample = n->start_node_sample + t->sample * engine_rate / rate;
        }
        s.x[0] = sp->send_id(make_time(*sp, std::nullopt, sample, engine_rate), "autorelease");
    }, "@@:@");

    // ---- AVAudioEngine -------------------------------------------------------------------------
    const auto engine_object = engine_class;
    support.method(engine_object, "dealloc", [self, sp, engine_object](CpuState& s, GuestMemory&) {
        if (const auto e = sp->objc.find_host_state<EngineState>(s.x[0])) {
            self->stop(s.x[0]);
            std::vector<Address> attached;
            {
                std::lock_guard lock(self->mutex_);
                attached = e->attached;
            }
            for (const auto object : attached) self->detach(s.x[0], object);
            for (auto* object : {&e->output, &e->mixer, &e->input, &e->manual_format, &e->input_format}) sp->release(std::exchange(*object, 0));
            if (e->input_block) self->bridge_->block_release(std::exchange(e->input_block, 0));
            if (e->manual_block) self->bridge_->forget_block(std::exchange(e->manual_block, 0));
            std::shared_ptr<CompletionThread> thread;
            bool own_thread{};
            {
                std::lock_guard lock(self->mutex_);
                thread = std::move(e->thread);
                own_thread = e->callback_thread == std::this_thread::get_id();
            }
            // Released from one of its own callbacks: the thread cannot join itself; the main queue does.
            if (thread && own_thread) self->bridge_->dispatch_async(self->bridge_->main_queue(), [thread] { thread->shutdown(); });
            else if (thread) thread->shutdown();
        }
        static_cast<void>(sp->objc.send_super(s.x[0], engine_object, sp->objc.selector("dealloc")));
    }, "v@:");
    support.method(engine_object, "outputNode", [self](CpuState& s, GuestMemory&) { s.x[0] = self->output_node(s.x[0]); }, "@@:");
    support.method(engine_object, "mainMixerNode", [self](CpuState& s, GuestMemory&) { s.x[0] = self->main_mixer(s.x[0]); }, "@@:");
    support.method(engine_object, "inputNode", [self](CpuState& s, GuestMemory&) {
        auto e = self->engine(s.x[0]);
        if (!e->input) {
            const auto made = self->make_node(self->input_class_, Kind::input);
            {
                std::lock_guard lock(self->mutex_);
                e->input = made;
            }
            self->attach(s.x[0], made);
        }
        s.x[0] = e->input;
    }, "@@:");
    support.method(engine_object, "attachNode:", [self](CpuState& s, GuestMemory&) { self->attach(s.x[0], s.x[2]); }, "v@:@");
    support.method(engine_object, "detachNode:", [self](CpuState& s, GuestMemory&) { if (s.x[2]) self->detach(s.x[0], s.x[2]); }, "v@:@");
    support.method(engine_object, "attachedNodes", [self, sp](CpuState& s, GuestMemory&) {
        std::vector<Address> nodes;
        {
            std::lock_guard lock(self->mutex_);
            nodes = self->engine(s.x[0])->attached;
        }
        const auto array = self->bridge_->array(nodes);
        s.x[0] = sp->send_id(sp->objc.find_class("NSSet"), "setWithArray:", {array});
        sp->release(array);
    }, "@@:");
    // connect:to:format: (x2, x3, format x4): the destination's next free input bus.
    support.method(engine_object, "connect:to:format:", [self](CpuState& s, GuestMemory&) {
        self->connect(s.x[0], s.x[2], s.x[3], 0, std::nullopt, s.x[4]);
    }, "v@:@@@");
    // connect:to:fromBus:toBus:format: (x2, x3, from x4, to x5, format x6)
    support.method(engine_object, "connect:to:fromBus:toBus:format:", [self](CpuState& s, GuestMemory&) {
        self->connect(s.x[0], s.x[2], s.x[3], static_cast<std::uint32_t>(s.x[4]), static_cast<std::uint32_t>(s.x[5]), s.x[6]);
    }, "v@:@@QQ@");
    support.method(engine_object, "disconnectNodeInput:", [self](CpuState& s, GuestMemory&) { if (s.x[2]) self->disconnect_input(s.x[2], std::nullopt); }, "v@:@");
    support.method(engine_object, "disconnectNodeInput:bus:", [self](CpuState& s, GuestMemory&) {
        if (s.x[2]) self->disconnect_input(s.x[2], static_cast<std::uint32_t>(s.x[3]));
    }, "v@:@Q");
    support.method(engine_object, "disconnectNodeOutput:", [self](CpuState& s, GuestMemory&) { if (s.x[2]) self->disconnect_output(s.x[0], s.x[2]); }, "v@:@");
    support.method(engine_object, "disconnectNodeOutput:bus:", [self](CpuState& s, GuestMemory&) {
        if (s.x[2] && s.x[3] == 0) self->disconnect_output(s.x[0], s.x[2]);
    }, "v@:@Q");
    support.method(engine_object, "prepare", [self](CpuState& s, GuestMemory&) { static_cast<void>(self->output_node(s.x[0])); }, "v@:");
    support.method(engine_object, "startAndReturnError:", [self](CpuState& s, GuestMemory&) { s.x[0] = self->start(s.x[0], s.x[2]) ? 1 : 0; }, "B@:^@");
    support.method(engine_object, "stop", [self](CpuState& s, GuestMemory&) { self->stop(s.x[0]); }, "v@:");
    support.method(engine_object, "pause", [self](CpuState& s, GuestMemory&) { self->stop(s.x[0]); }, "v@:");
    support.method(engine_object, "reset", [self](CpuState& s, GuestMemory&) {
        std::vector<Address> players;
        {
            std::lock_guard lock(self->mutex_);
            for (const auto object : self->engine(s.x[0])->attached)
                if (const auto n = self->support_.objc.find_host_state<Node>(object); n && n->kind == Kind::player) players.push_back(object);
        }
        for (const auto object : players) self->stop_player(object, true);
    }, "v@:");
    support.method(engine_object, "isRunning", [self](CpuState& s, GuestMemory&) {
        const auto e = self->engine(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = e->running ? 1 : 0;
    }, "B@:");
    // ---- manual rendering ------------------------------------------------------------------------
    const auto manual_state = [self](Address object) {
        const auto e = self->engine(object);
        return e;
    };
    support.method(engine_object, "isInManualRenderingMode", [self, manual_state](CpuState& s, GuestMemory&) {
        const auto e = manual_state(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = e->manual ? 1 : 0;
    }, "B@:");
    // -enableManualRenderingMode:format:maximumFrameCount:error: (mode x2, format x3, count x4, error x5)
    support.method(engine_object, "enableManualRenderingMode:format:maximumFrameCount:error:", [self, sp, manual_state](CpuState& s, GuestMemory&) {
        const auto e = manual_state(s.x[0]);
        const auto fail_with = [&](std::int32_t status, std::string_view text) {
            if (s.x[5]) self->bridge_->store_error(s.x[5], self->bridge_->os_status_error(status, text), true);
            s.x[0] = 0;
        };
        const auto pcm = s.x[3] ? format_pcm(*sp, s.x[3]) : std::nullopt;
        const auto mode = static_cast<std::int64_t>(s.x[2]);
        if (!pcm || (mode != 0 && mode != 1) || s.x[4] == 0) return fail_with(manual_invalid_mode, "AVAudioEngine: manual rendering needs a PCM format, a mode and a frame count");
        {
            std::lock_guard lock(self->mutex_);
            if (e->running) return fail_with(manual_initialized, "AVAudioEngine: stop the engine before changing its rendering mode");
            sp->release(std::exchange(e->manual_format, sp->retain(s.x[3])));
            e->manual_pcm = *pcm;
            e->manual_mode = mode;
            e->manual_maximum = static_cast<std::uint32_t>(s.x[4]);
        }
        self->set_manual(s.x[0], true);
        s.x[0] = 1;
    }, "B@:q@I^@");
    support.method(engine_object, "disableManualRenderingMode", [self, manual_state](CpuState& s, GuestMemory&) {
        const auto e = manual_state(s.x[0]);
        {
            std::lock_guard lock(self->mutex_);
            if (!e->manual) return;
            e->running = false;
        }
        self->set_manual(s.x[0], false);
    }, "v@:");
    support.method(engine_object, "manualRenderingMode", [self, manual_state](CpuState& s, GuestMemory&) {
        const auto e = manual_state(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = static_cast<std::uint64_t>(e->manual_mode);
    }, "q@:");
    support.method(engine_object, "manualRenderingFormat", [self, manual_state](CpuState& s, GuestMemory&) {
        const auto e = manual_state(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = e->manual ? e->manual_format : 0;
    }, "@@:");
    support.method(engine_object, "manualRenderingMaximumFrameCount", [self, manual_state](CpuState& s, GuestMemory&) {
        const auto e = manual_state(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = e->manual ? e->manual_maximum : 0;
    }, "I@:");
    support.method(engine_object, "manualRenderingSampleTime", [self, manual_state](CpuState& s, GuestMemory&) {
        const auto e = manual_state(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(e->sample_time));
    }, "q@:");
    // -renderOffline:(AVAudioFrameCount)numberOfFrames toBuffer:(AVAudioPCMBuffer *)buffer error:(NSError **)
    support.method(engine_object, "renderOffline:toBuffer:error:", [self, sp, manual_state](CpuState& s, GuestMemory&) {
        const auto e = manual_state(s.x[0]);
        const auto frames = static_cast<std::uint32_t>(s.x[2]);
        const auto buffer = s.x[3] ? sp->objc.find_host_state<PcmBuffer>(s.x[3]) : nullptr;
        const auto fail_with = [&](std::int32_t status, std::string_view text) {
            if (s.x[4]) self->bridge_->store_error(s.x[4], self->bridge_->os_status_error(status, text), true);
            s.x[0] = static_cast<std::uint64_t>(manual_error);
        };
        bool offline{};
        {
            std::lock_guard lock(self->mutex_);
            offline = e->manual && e->manual_mode == 0;
        }
        if (!offline) return fail_with(manual_invalid_mode, "AVAudioEngine: -renderOffline:toBuffer:error: needs offline manual rendering");
        if (!buffer || frames > buffer->capacity) return fail_with(-50, "AVAudioEngine: the buffer cannot hold the frames asked for");
        const auto status_cell = sp->process.allocate_guest(8, 8);
        sp->memory().write32(status_cell, 0);
        const auto status = self->render_manual(s.x[0], frames, buffer->list, status_cell);
        const auto os_status = static_cast<std::int32_t>(sp->memory().read32(status_cell));
        sp->process.release_guest(status_cell);
        if (status == manual_error) return fail_with(os_status, "AVAudioEngine: the engine could not render");
        sp->send(s.x[3], "setFrameLength:", [frames] { CallRegs r{}; r.x[2] = frames; return r; }());
        s.x[0] = static_cast<std::uint64_t>(status);
    }, "q@:I@^@");
    // manualRenderingBlock: AVAudioEngineManualRenderingStatus (^)(AVAudioFrameCount numberOfFrames,
    //     AudioBufferList *outBuffer, OSStatus *outError), for realtime and offline modes alike.
    support.method(engine_object, "manualRenderingBlock", [self, manual_state](CpuState& s, GuestMemory&) {
        const auto engine_object = s.x[0];
        const auto e = manual_state(engine_object);
        std::lock_guard lock(self->mutex_);
        if (!e->manual) { s.x[0] = 0; return; }
        if (!e->manual_block)
            e->manual_block = self->bridge_->make_block([self, engine_object](CpuState& b, GuestMemory&) {
                b.x[0] = static_cast<std::uint64_t>(self->render_manual(engine_object, static_cast<std::uint32_t>(b.x[1]), b.x[2], b.x[3]));
            });
        s.x[0] = e->manual_block;
    }, "@?@:");
    // -[AVAudioInputNode setManualRenderingInputPCMFormat:inputBlock:] (format x2, block x3)
    support.method(input_class_, "setManualRenderingInputPCMFormat:inputBlock:", [self, sp](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        const auto pcm = s.x[2] ? format_pcm(*sp, s.x[2]) : std::nullopt;
        const auto e = n->engine ? sp->objc.find_host_state<EngineState>(n->engine) : nullptr;
        if (!pcm || !e) { s.x[0] = 0; return; }
        Address old_block{}, old_format{};
        {
            std::lock_guard lock(self->mutex_);
            if (!e->manual) { s.x[0] = 0; return; }
            old_format = std::exchange(e->input_format, sp->retain(s.x[2]));
            old_block = std::exchange(e->input_block, s.x[3] ? self->bridge_->block_copy(s.x[3]) : 0);
            e->input_pcm = *pcm;
            sp->release(std::exchange(n->output_format, sp->retain(s.x[2])));
        }
        sp->release(old_format);
        if (old_block) self->bridge_->block_release(old_block);
        s.x[0] = 1;
    }, "B@:@@?");

    // ---- AVAudioSourceNode and AVAudioSinkNode ---------------------------------------------------
    // -initWithRenderBlock:, -initWithFormat:renderBlock: and -initWithReceiverBlock:
    const auto init_with_block = [sp, self](CpuState& s, Address cls, Kind kind, Address format, Address block) {
        const auto object = sp->objc.send_super(s.x[0], cls, sp->objc.selector("init")).x0;
        if (!object || !block || (format && !format_pcm(*sp, format))) {
            sp->release(object);
            s.x[0] = 0;
            return;
        }
        const auto n = sp->objc.host_state<Node>(object);
        n->kind = kind;
        n->block = self->bridge_->block_copy(block);
        n->source_format = sp->retain(format);
        if (format) n->output_format = sp->retain(format);
        s.x[0] = object;
    };
    support.method(source_class_, "initWithRenderBlock:", [init_with_block, cls = source_class_](CpuState& s, GuestMemory&) {
        init_with_block(s, cls, Kind::source, 0, s.x[2]);
    }, "@@:@?");
    support.method(source_class_, "initWithFormat:renderBlock:", [init_with_block, cls = source_class_](CpuState& s, GuestMemory&) {
        init_with_block(s, cls, Kind::source, s.x[2], s.x[3]);
    }, "@@:@@?");
    support.method(sink_class_, "initWithReceiverBlock:", [init_with_block, cls = sink_class_](CpuState& s, GuestMemory&) {
        init_with_block(s, cls, Kind::sink, 0, s.x[2]);
    }, "@@:@?");
}

} // namespace

void install_avaudio_engine(Support& support) { std::make_shared<Engines>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media::avaudio
