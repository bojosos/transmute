#include "alchemy/darwin/media/avaudio.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"
#include "alchemy/darwin/media/avaudio_effects.hpp"
#include "alchemy/darwin/media/avaudio_sampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
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
// - Effect nodes (AVAudioUnitEQ, Delay, Reverb, Distortion, Varispeed, TimePitch: avaudio_effects.hpp)
//   process what their input renders, in the connection's channels at the engine's rate.
// - AVAudioUnitSampler (avaudio_sampler.hpp) plays SoundFont 2 presets or audio files by note, through
//   the AVAudioUnitMIDIInstrument messages (notes, controllers, pitch bend, program changes, raw
//   channel events); DLS banks and .aupreset/.exs instruments answer NO with an error.
// - AVAudioEnvironmentNode mixes its inputs like a mixer, placing mono ones (AVAudio3DMixing position)
//   around the listener: distance attenuation by the documented models (exponential, inverse, linear
//   between the reference and maximum distances) and equal-power stereo panning by azimuth (the
//   listener's yaw, or its forward vector; pitch and roll are kept but not applied), plus a reverb send
//   by each input's reverbBlend when the reverb is enabled. Inputs of more channels mix unplaced.
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

enum class Kind { output, input, mixer, player, source, sink, effect, sampler, environment, other };

// AVAudioEnvironmentNode's listener, distance attenuation and reverb.
struct Environment {
    std::array<double, 3> listener{};
    double yaw{}, pitch{}, roll{};     // degrees
    std::int64_t model{1};             // AVAudioEnvironmentDistanceAttenuationModel: exponential 1, inverse 2, linear 3
    double reference{1}, maximum{100000}, rolloff{1};
    bool reverb{};
    double reverb_level{};             // dB
    std::int64_t output_type{};
    Reverb reverb_unit;
};
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
    // Effect nodes.
    std::shared_ptr<Effect> effect;
    // AVAudioUnitSampler: its sampler, and the sound bank it loaded (for program changes).
    std::shared_ptr<Sampler> sampler;
    std::shared_ptr<std::vector<std::uint8_t>> sound_bank;
    int bank{}, program{};
    // AVAudioEnvironmentNode.
    std::shared_ptr<Environment> environment;
    // AVAudio3DMixing (as a source of an environment node).
    std::array<float, 3> position{};
    float reverb_blend{}, obstruction{}, occlusion{}, mixing_rate{1};
    std::int64_t rendering_algorithm{}, source_mode{}, point_source_in_head_mode{};
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
    void install_effects();
    void install_instruments();
    void install_environment();
    void render_environment(Node& n, double* out, std::uint32_t frames, std::uint32_t channels, double rate, double sample_time, unsigned depth,
                            std::vector<Callback>& callbacks, std::vector<TapDelivery>& taps);

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
        else if (d->kind == Kind::mixer || d->kind == Kind::environment) while (d->inputs.contains(bus)) ++bus;
        if (d->kind == Kind::player || d->kind == Kind::source || d->kind == Kind::input || d->kind == Kind::sampler)
            unsupported("-[AVAudioEngine connect:to:...] to a node with no input (a player, source, sampler or input node)");
        if (d->kind != Kind::mixer && d->kind != Kind::environment && bus != 0) unsupported("-[AVAudioEngine connect:to:...] to an input bus other than 0 of a node that is not a mixer");
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
    case Kind::effect: {
        // The input in this node's channels (a mono input feeds every channel).
        const auto input = [&](std::uint32_t count) {
            const auto found = n->inputs.find(0);
            if (found == n->inputs.end()) return std::vector<double>(static_cast<std::size_t>(count) * channels, 0.0);
            const auto known = n->input_channels.find(0);
            const auto source_channels = known != n->input_channels.end() ? known->second : channels;
            const auto data = pull(found->second, count, source_channels, rate, sample_time, depth + 1, callbacks, taps);
            if (source_channels == channels) return data;
            std::vector<double> mapped(static_cast<std::size_t>(count) * channels, 0.0);
            for (std::uint32_t f = 0; f < count; ++f)
                for (std::uint32_t c = 0; c < channels; ++c)
                    mapped[static_cast<std::size_t>(f) * channels + c] =
                        source_channels == 1 ? data[f] : c < source_channels ? data[static_cast<std::size_t>(f) * source_channels + c] : 0.0;
            return mapped;
        };
        if (n->effect) n->effect->render(out.data(), frames, channels, rate, input);
        break;
    }
    case Kind::sampler:
        if (n->sampler) n->sampler->render(out.data(), frames, channels, rate);
        break;
    case Kind::environment:
        render_environment(*n, out.data(), frames, channels, rate, sample_time, depth, callbacks, taps);
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

// The effect nodes: AVAudioUnitEffect (EQ, Delay, Reverb, Distortion) and AVAudioUnitTimeEffect
// (Varispeed, TimePitch), and AVAudioUnitEQFilterParameters. Parameters change under the engines' lock.
struct BandState final : ObjectPayload {
    std::shared_ptr<Equalizer> equalizer;
    std::size_t index{};
};

void Engines::install_effects() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    if (!support.objc.find_class("AVAudioUnit")) static_cast<void>(declare(support, "AVAudioUnit", "AVAudioNode"));
    const auto effect_class = declare(support, "AVAudioUnitEffect", "AVAudioUnit");
    const auto time_class = declare(support, "AVAudioUnitTimeEffect", "AVAudioUnit");
    const auto eq_class = declare(support, "AVAudioUnitEQ", "AVAudioUnitEffect");
    const auto band_class = declare(support, "AVAudioUnitEQFilterParameters", "NSObject");
    const auto delay_class = declare(support, "AVAudioUnitDelay", "AVAudioUnitEffect");
    const auto reverb_class = declare(support, "AVAudioUnitReverb", "AVAudioUnitEffect");
    const auto distortion_class = declare(support, "AVAudioUnitDistortion", "AVAudioUnitEffect");
    const auto varispeed_class = declare(support, "AVAudioUnitVarispeed", "AVAudioUnitTimeEffect");
    const auto time_pitch_class = declare(support, "AVAudioUnitTimePitch", "AVAudioUnitTimeEffect");
    const auto real32 = [](const CpuState& s) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])); };
    const auto return_real32 = [](CpuState& s, double value) { s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(value)), 0}; };

    // A node of `cls` around a new effect.
    const auto initializer = [sp, self](Address cls, auto make) {
        return [sp, self, cls, make](CpuState& s, GuestMemory&) {
            const auto object = sp->objc.send_super(s.x[0], cls, sp->objc.selector("init")).x0;
            if (object) {
                const auto n = sp->objc.host_state<Node>(object);
                n->kind = Kind::effect;
                n->effect = make(s);
            }
            s.x[0] = object;
        };
    };
    // The effect of `object` as T.
    const auto effect_of = [self]<class T>(Address object, T*) {
        const auto e = std::dynamic_pointer_cast<T>(self->node(object)->effect);
        if (!e) unsupported("an AVAudioUnit effect method on a node of another kind");
        return e;
    };
    // A float (or double, `wide`) property of effects of type T.
    const auto property = [&support, self, effect_of, real32, return_real32]<class T>(Address cls, const char* getter, const char* setter, double T::*member, T*,
                                                                                        bool wide = false) {
        support.method(cls, getter, [self, effect_of, return_real32, member, wide](CpuState& s, GuestMemory&) {
            const auto e = effect_of(s.x[0], static_cast<T*>(nullptr));
            std::lock_guard lock(self->mutex_);
            if (wide) s.v[0] = {std::bit_cast<std::uint64_t>((*e).*member), 0};
            else return_real32(s, (*e).*member);
        }, wide ? "d@:" : "f@:");
        support.method(cls, setter, [self, effect_of, real32, member, wide](CpuState& s, GuestMemory&) {
            const auto e = effect_of(s.x[0], static_cast<T*>(nullptr));
            std::lock_guard lock(self->mutex_);
            (*e).*member = wide ? std::bit_cast<double>(s.v[0][0]) : static_cast<double>(real32(s));
        }, wide ? "v@:d" : "v@:f");
    };
    // bypass, on both base classes.
    for (const auto cls : {effect_class, time_class}) {
        support.method(cls, "bypass", [self](CpuState& s, GuestMemory&) {
            const auto n = self->node(s.x[0]);
            std::lock_guard lock(self->mutex_);
            s.x[0] = n->effect && n->effect->bypass ? 1 : 0;
        }, "B@:");
        support.method(cls, "setBypass:", [self](CpuState& s, GuestMemory&) {
            const auto n = self->node(s.x[0]);
            std::lock_guard lock(self->mutex_);
            if (n->effect) n->effect->bypass = (s.x[2] & 0xff) != 0;
        }, "v@:B");
    }

    // ---- AVAudioUnitEQ: -initWithNumberOfBands:, -init (one band), bands, globalGain ----------------
    const auto make_eq = [](std::size_t count) { return std::make_shared<Equalizer>(count); };
    support.method(eq_class, "initWithNumberOfBands:", initializer(eq_class, [make_eq](CpuState& s) {
        return make_eq(static_cast<std::size_t>(std::min<std::uint64_t>(s.x[2], 64)));
    }), "@@:Q");
    support.method(eq_class, "init", initializer(eq_class, [make_eq](CpuState&) { return make_eq(1); }), "@@:");
    support.method(eq_class, "bands", [sp, self, effect_of, band_class](CpuState& s, GuestMemory&) {
        const auto eq = effect_of(s.x[0], static_cast<Equalizer*>(nullptr));
        std::size_t count{};
        {
            std::lock_guard lock(self->mutex_);
            count = eq->bands.size();
        }
        std::vector<Address> items;
        for (std::size_t i = 0; i < count; ++i) {
            const auto band = sp->send_id(sp->send_id(band_class, "alloc"), "init");
            const auto st = sp->objc.host_state<BandState>(band);
            st->equalizer = eq;
            st->index = i;
            items.push_back(band);
        }
        const auto array = self->bridge_->array(items);
        for (const auto item : items) sp->release(item);
        s.x[0] = sp->send_id(array, "autorelease");
    }, "@@:");
    property(eq_class, "globalGain", "setGlobalGain:", &Equalizer::global_gain, static_cast<Equalizer*>(nullptr));
    // AVAudioUnitEQFilterParameters: a view of one band.
    const auto band_of = [sp](Address object) {
        const auto st = sp->objc.find_host_state<BandState>(object);
        if (!st || !st->equalizer) unsupported("an AVAudioUnitEQFilterParameters not taken from -[AVAudioUnitEQ bands]");
        return st;
    };
    for (const auto& [getter, setter, field] : std::initializer_list<std::tuple<const char*, const char*, double Equalizer::Band::*>>{
             {"frequency", "setFrequency:", &Equalizer::Band::frequency}, {"bandwidth", "setBandwidth:", &Equalizer::Band::bandwidth},
             {"gain", "setGain:", &Equalizer::Band::gain}}) {
        const auto member = field;
        support.method(band_class, getter, [self, band_of, return_real32, member](CpuState& s, GuestMemory&) {
            const auto b = band_of(s.x[0]);
            std::lock_guard lock(self->mutex_);
            return_real32(s, b->equalizer->bands[b->index].*member);
        }, "f@:");
        support.method(band_class, setter, [self, band_of, real32, member](CpuState& s, GuestMemory&) {
            const auto b = band_of(s.x[0]);
            std::lock_guard lock(self->mutex_);
            b->equalizer->bands[b->index].*member = real32(s);
        }, "v@:f");
    }
    support.method(band_class, "filterType", [self, band_of](CpuState& s, GuestMemory&) {
        const auto b = band_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = static_cast<std::uint64_t>(b->equalizer->bands[b->index].type);
    }, "q@:");
    support.method(band_class, "setFilterType:", [self, band_of](CpuState& s, GuestMemory&) {
        const auto b = band_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        b->equalizer->bands[b->index].type = std::clamp<std::int64_t>(static_cast<std::int64_t>(s.x[2]), 0, 10);
    }, "v@:q");
    support.method(band_class, "bypass", [self, band_of](CpuState& s, GuestMemory&) {
        const auto b = band_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = b->equalizer->bands[b->index].bypass ? 1 : 0;
    }, "B@:");
    support.method(band_class, "setBypass:", [self, band_of](CpuState& s, GuestMemory&) {
        const auto b = band_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        b->equalizer->bands[b->index].bypass = (s.x[2] & 0xff) != 0;
    }, "v@:B");

    // ---- AVAudioUnitDelay, Reverb, Distortion ----------------------------------------------------
    support.method(delay_class, "init", initializer(delay_class, [](CpuState&) { return std::make_shared<Delay>(); }), "@@:");
    property(delay_class, "delayTime", "setDelayTime:", &Delay::delay_time, static_cast<Delay*>(nullptr), true);
    property(delay_class, "feedback", "setFeedback:", &Delay::feedback, static_cast<Delay*>(nullptr));
    property(delay_class, "lowPassCutoff", "setLowPassCutoff:", &Delay::low_pass_cutoff, static_cast<Delay*>(nullptr));
    property(delay_class, "wetDryMix", "setWetDryMix:", &Delay::wet_dry_mix, static_cast<Delay*>(nullptr));
    support.method(reverb_class, "init", initializer(reverb_class, [](CpuState&) { return std::make_shared<Reverb>(); }), "@@:");
    property(reverb_class, "wetDryMix", "setWetDryMix:", &Reverb::wet_dry_mix, static_cast<Reverb*>(nullptr));
    support.method(reverb_class, "loadFactoryPreset:", [self, effect_of](CpuState& s, GuestMemory&) {
        const auto e = effect_of(s.x[0], static_cast<Reverb*>(nullptr));
        std::lock_guard lock(self->mutex_);
        e->load_preset(static_cast<std::int64_t>(s.x[2]));
        e->reset();
    }, "v@:q");
    support.method(distortion_class, "init", initializer(distortion_class, [](CpuState&) { return std::make_shared<Distortion>(); }), "@@:");
    property(distortion_class, "preGain", "setPreGain:", &Distortion::pre_gain, static_cast<Distortion*>(nullptr));
    property(distortion_class, "wetDryMix", "setWetDryMix:", &Distortion::wet_dry_mix, static_cast<Distortion*>(nullptr));
    support.method(distortion_class, "loadFactoryPreset:", [self, effect_of](CpuState& s, GuestMemory&) {
        const auto e = effect_of(s.x[0], static_cast<Distortion*>(nullptr));
        std::lock_guard lock(self->mutex_);
        e->load_preset(static_cast<std::int64_t>(s.x[2]));
    }, "v@:q");

    // ---- AVAudioUnitVarispeed, TimePitch ---------------------------------------------------------
    support.method(varispeed_class, "init", initializer(varispeed_class, [](CpuState&) { return std::make_shared<Varispeed>(); }), "@@:");
    property(varispeed_class, "rate", "setRate:", &Varispeed::rate, static_cast<Varispeed*>(nullptr));
    support.method(time_pitch_class, "init", initializer(time_pitch_class, [](CpuState&) { return std::make_shared<TimePitch>(); }), "@@:");
    property(time_pitch_class, "rate", "setRate:", &TimePitch::rate, static_cast<TimePitch*>(nullptr));
    property(time_pitch_class, "pitch", "setPitch:", &TimePitch::pitch, static_cast<TimePitch*>(nullptr));
    property(time_pitch_class, "overlap", "setOverlap:", &TimePitch::overlap, static_cast<TimePitch*>(nullptr));
}

void Engines::render_environment(Node& n, double* out, std::uint32_t frames, std::uint32_t channels, double rate, double sample_time, unsigned depth,
                                 std::vector<Callback>& callbacks, std::vector<TapDelivery>& taps) {
    const auto& env = *n.environment;
    const double yaw = env.yaw * std::numbers::pi / 180;
    const std::array<double, 3> forward{std::sin(yaw), 0, -std::cos(yaw)}, right{std::cos(yaw), 0, std::sin(yaw)};
    std::vector<double> send(env.reverb ? static_cast<std::size_t>(frames) * channels : 0, 0.0);
    for (const auto& [bus, source] : n.inputs) {
        const auto known = n.input_channels.find(bus);
        const auto source_channels = known != n.input_channels.end() ? known->second : channels;
        const auto data = pull(source, frames, source_channels, rate, sample_time, depth + 1, callbacks, taps);
        const auto s = support_.objc.find_host_state<Node>(source);
        const double volume = s ? s->volume : 1.0;
        if (source_channels != 1 || !s) {
            mix_into(out, channels, data.data(), source_channels, frames, volume, s ? s->pan : 0.0);
            continue;
        }
        // Placed: distance gain and an equal-power pan by azimuth.
        const std::array<double, 3> rel{s->position[0] - env.listener[0], s->position[1] - env.listener[1], s->position[2] - env.listener[2]};
        const double x = rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2];
        const double z = rel[0] * forward[0] + rel[1] * forward[1] + rel[2] * forward[2];
        const double distance = std::clamp(std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]), env.reference, std::max(env.maximum, env.reference));
        double gain = 1;
        if (env.model == 1) gain = std::pow(distance / env.reference, -env.rolloff);
        else if (env.model == 2) gain = env.reference / (env.reference + env.rolloff * (distance - env.reference));
        else if (env.model == 3)
            gain = env.maximum > env.reference ? std::max(0.0, 1 - env.rolloff * (distance - env.reference) / (env.maximum - env.reference)) : 1.0;
        gain *= volume;
        const double pan = (x == 0 && z == 0) ? 0.0 : std::sin(std::atan2(x, z));
        const double theta = (pan + 1) * std::numbers::pi / 4;
        const double left = std::cos(theta) * gain, right_gain = std::sin(theta) * gain;
        for (std::uint32_t f = 0; f < frames; ++f) {
            const double v = data[f];
            double* o = out + static_cast<std::size_t>(f) * channels;
            if (channels == 1) o[0] += v * (left + right_gain) / 2;
            else { o[0] += v * left; o[1] += v * right_gain; }
            if (env.reverb && s->reverb_blend > 0) {
                double* r = send.data() + static_cast<std::size_t>(f) * channels;
                for (std::uint32_t c = 0; c < channels; ++c) r[c] += v * gain * s->reverb_blend;
            }
        }
    }
    if (env.reverb) {
        std::vector<double> wet(send.size());
        n.environment->reverb_unit.wet_dry_mix = 100;
        n.environment->reverb_unit.render(wet.data(), frames, channels, rate, [&send](std::uint32_t) { return send; });
        const double level = std::pow(10.0, env.reverb_level / 20.0);
        for (std::size_t i = 0; i < wet.size(); ++i) out[i] += wet[i] * level;
    }
    if (n.output_volume != 1.0f)
        for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * channels; ++i) out[i] *= n.output_volume;
}

// The parameter objects of an environment node: views of its Environment.
struct EnvironmentView final : ObjectPayload { std::shared_ptr<Environment> environment; };

void Engines::install_instruments() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    if (!support.objc.find_class("AVAudioUnit")) static_cast<void>(declare(support, "AVAudioUnit", "AVAudioNode"));
    const auto instrument_class = declare(support, "AVAudioUnitMIDIInstrument", "AVAudioUnit");
    const auto sampler_class = declare(support, "AVAudioUnitSampler", "AVAudioUnitMIDIInstrument");
    const auto real32 = [](const CpuState& s, unsigned i) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[i][0])); };
    const auto return_real32 = [](CpuState& s, double value) { s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(value)), 0}; };
    support.method(sampler_class, "init", [sp, sampler_class](CpuState& s, GuestMemory&) {
        const auto object = sp->objc.send_super(s.x[0], sampler_class, sp->objc.selector("init")).x0;
        if (object) {
            const auto n = sp->objc.host_state<Node>(object);
            n->kind = Kind::sampler;
            n->sampler = std::make_shared<Sampler>();
        }
        s.x[0] = object;
    }, "@@:");
    const auto sampler_of = [self](Address object) {
        const auto n = self->node(object);
        if (!n->sampler) unsupported("an AVAudioUnitMIDIInstrument message to a node that is not a sampler");
        return n;
    };
    // MIDI channel messages (status byte, two data bytes) to the sampler; program changes reload the
    // preset from the loaded sound bank when it has one.
    const auto channel_event = [self, sampler_of](Address object, unsigned status, unsigned data1, unsigned data2) {
        const auto n = sampler_of(object);
        std::lock_guard lock(self->mutex_);
        const int channel = static_cast<int>(status & 15);
        switch (status & 0xf0) {
        case 0x90: n->sampler->note_on(channel, static_cast<int>(data1 & 127), static_cast<int>(data2 & 127)); break;
        case 0x80: n->sampler->note_off(channel, static_cast<int>(data1 & 127)); break;
        case 0xb0: n->sampler->controller(channel, static_cast<int>(data1 & 127), static_cast<int>(data2 & 127)); break;
        case 0xe0: n->sampler->pitch_bend(channel, static_cast<int>((data1 & 127) | ((data2 & 127) << 7))); break;
        case 0xc0:
            if (n->sound_bank) {
                std::string why;
                if (auto instrument = load_sound_font(*n->sound_bank, n->bank, static_cast<int>(data1 & 127), why)) {
                    n->sampler->set_instrument(std::move(*instrument));
                    n->program = static_cast<int>(data1 & 127);
                }
            }
            break;
        default: break;   // pressure: no effect on the sampler's voices
        }
    };
    // startNote:withVelocity:onChannel: (note x2, velocity x3, channel x4) and the rest.
    support.method(instrument_class, "startNote:withVelocity:onChannel:", [channel_event](CpuState& s, GuestMemory&) {
        channel_event(s.x[0], 0x90 | static_cast<unsigned>(s.x[4] & 15), static_cast<unsigned>(s.x[2]), static_cast<unsigned>(s.x[3]));
    }, "v@:CCC");
    support.method(instrument_class, "stopNote:onChannel:", [channel_event](CpuState& s, GuestMemory&) {
        channel_event(s.x[0], 0x80 | static_cast<unsigned>(s.x[3] & 15), static_cast<unsigned>(s.x[2]), 0);
    }, "v@:CC");
    support.method(instrument_class, "sendController:withValue:onChannel:", [channel_event](CpuState& s, GuestMemory&) {
        channel_event(s.x[0], 0xb0 | static_cast<unsigned>(s.x[4] & 15), static_cast<unsigned>(s.x[2]), static_cast<unsigned>(s.x[3]));
    }, "v@:CCC");
    support.method(instrument_class, "sendPitchBend:onChannel:", [channel_event](CpuState& s, GuestMemory&) {
        const auto value = static_cast<unsigned>(s.x[2] & 0x3fff);
        channel_event(s.x[0], 0xe0 | static_cast<unsigned>(s.x[3] & 15), value & 127, value >> 7);
    }, "v@:SC");
    support.method(instrument_class, "sendProgramChange:onChannel:", [channel_event](CpuState& s, GuestMemory&) {
        channel_event(s.x[0], 0xc0 | static_cast<unsigned>(s.x[3] & 15), static_cast<unsigned>(s.x[2]), 0);
    }, "v@:CC");
    support.method(instrument_class, "sendProgramChange:bankMSB:bankLSB:onChannel:", [self, sampler_of, channel_event](CpuState& s, GuestMemory&) {
        {
            const auto n = sampler_of(s.x[0]);
            std::lock_guard lock(self->mutex_);
            n->bank = (s.x[3] & 0xff) == 0x78 ? 128 : static_cast<int>(s.x[4] & 127);
        }
        channel_event(s.x[0], 0xc0 | static_cast<unsigned>(s.x[5] & 15), static_cast<unsigned>(s.x[2]), 0);
    }, "v@:CCCC");
    for (const auto* selector : {"sendPressure:onChannel:", "sendPressureForKey:withValue:onChannel:", "sendMIDISysExEvent:"})
        support.method(instrument_class, selector, [](CpuState&, GuestMemory&) {}, "v@:CC");
    support.method(instrument_class, "sendMIDIEvent:data1:data2:", [channel_event](CpuState& s, GuestMemory&) {
        channel_event(s.x[0], static_cast<unsigned>(s.x[2] & 0xff), static_cast<unsigned>(s.x[3]), static_cast<unsigned>(s.x[4]));
    }, "v@:CCC");
    support.method(instrument_class, "sendMIDIEvent:data1:", [channel_event](CpuState& s, GuestMemory&) {
        channel_event(s.x[0], static_cast<unsigned>(s.x[2] & 0xff), static_cast<unsigned>(s.x[3]), 0);
    }, "v@:CC");

    // Loading. kAudioUnitErr_InvalidFile for what cannot be loaded.
    constexpr std::int32_t invalid_file = -10871;
    const auto fail = [self](Address error_out, std::string_view text) {
        if (error_out) self->bridge_->store_error(error_out, self->bridge_->os_status_error(invalid_file, text), true);
        return 0;
    };
    const auto file_bytes = [sp](Address url) -> std::shared_ptr<std::vector<std::uint8_t>> {
        const auto data = url ? sp->send_id(sp->objc.find_class("NSData"), "dataWithContentsOfURL:", {url}) : 0;
        if (!data) return nullptr;
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(sp->send(data, "length").x0));
        if (!bytes->empty()) sp->memory().read_bytes(sp->send_id(data, "bytes"), *bytes);
        return bytes;
    };
    const auto load_bank = [self, sampler_of, fail, file_bytes](Address object, Address url, int bank, int program, Address error_out) -> std::uint64_t {
        const auto n = sampler_of(object);
        auto bytes = file_bytes(url);
        if (!bytes) return fail(error_out, "AVAudioUnitSampler: the sound bank could not be read");
        std::string why;
        auto instrument = load_sound_font(*bytes, bank, program, why);
        if (!instrument) return fail(error_out, "AVAudioUnitSampler: " + why + " (SoundFont 2 banks only; DLS is not read)");
        std::lock_guard lock(self->mutex_);
        n->sampler->set_instrument(std::move(*instrument));
        n->sound_bank = std::move(bytes);
        n->bank = bank;
        n->program = program;
        return 1;
    };
    // -loadSoundBankInstrumentAtURL:program:bankMSB:bankLSB:error: (url x2, program x3, MSB x4, LSB x5, error x6):
    // kAUSampler_DefaultPercussionBankMSB (0x78) is the SoundFont's bank 128.
    support.method(sampler_class, "loadSoundBankInstrumentAtURL:program:bankMSB:bankLSB:error:", [load_bank](CpuState& s, GuestMemory&) {
        const int bank = (s.x[4] & 0xff) == 0x78 ? 128 : static_cast<int>(s.x[5] & 127);
        s.x[0] = load_bank(s.x[0], s.x[2], bank, static_cast<int>(s.x[3] & 127), s.x[6]);
    }, "B@:@CCC^@");
    // -loadInstrumentAtURL:error:: a .sf2 file's first melodic preset (bank 0, program 0).
    support.method(sampler_class, "loadInstrumentAtURL:error:", [sp, load_bank, fail](CpuState& s, GuestMemory&) {
        const auto extension = s.x[2] ? sp->cf.utf8(sp->send_id(s.x[2], "pathExtension")) : std::string{};
        if (extension != "sf2" && extension != "SF2") { s.x[0] = fail(s.x[3], "AVAudioUnitSampler: only SoundFont 2 instruments load (not DLS, .aupreset or .exs)"); return; }
        s.x[0] = load_bank(s.x[0], s.x[2], 0, 0, s.x[3]);
    }, "B@:@^@");
    // -loadAudioFilesAtURLs:error: (NSArray of NSURL): see from_samples.
    support.method(sampler_class, "loadAudioFilesAtURLs:error:", [self, sp, sampler_of, fail](CpuState& s, GuestMemory&) {
        const auto n = sampler_of(s.x[0]);
        std::vector<std::pair<std::vector<float>, double>> files;
        for (const auto url : s.x[2] ? sp->cf.array_values(s.x[2]) : std::vector<std::uint64_t>{}) {
            GuestAudio audio;
            if (open_guest_audio(*sp, sp->cf.url_path(url), audio)) { s.x[0] = fail(s.x[3], "AVAudioUnitSampler: an audio file could not be read"); return; }
            Decoded decoded;
            const auto status = decode_file(*sp, audio, decoded);
            close_guest_audio(*sp, audio);
            if (status || !decoded.channels) { s.x[0] = fail(s.x[3], "AVAudioUnitSampler: an audio file could not be decoded"); return; }
            std::vector<float> mono(static_cast<std::size_t>(decoded.frames()));
            for (std::size_t f = 0; f < mono.size(); ++f) {
                float sum = 0;
                for (std::uint32_t c = 0; c < decoded.channels; ++c) sum += decoded.samples[f * decoded.channels + c];
                mono[f] = sum / static_cast<float>(decoded.channels);
            }
            files.emplace_back(std::move(mono), decoded.rate);
        }
        if (files.empty()) { s.x[0] = fail(s.x[3], "AVAudioUnitSampler: no audio file to load"); return; }
        std::lock_guard lock(self->mutex_);
        n->sampler->set_instrument(from_samples(std::move(files)));
        n->sound_bank.reset();
        s.x[0] = 1;
    }, "B@:@^@");
    for (const auto& [getter, setter, field, low, high] : std::initializer_list<std::tuple<const char*, const char*, double Sampler::*, double, double>>{
             {"masterGain", "setMasterGain:", &Sampler::master_gain, -90.0, 12.0}, {"stereoPan", "setStereoPan:", &Sampler::stereo_pan, -1.0, 1.0},
             {"globalTuning", "setGlobalTuning:", &Sampler::global_tuning, -2400.0, 2400.0}}) {
        const auto member = field;
        const auto lo = low, hi = high;
        support.method(sampler_class, getter, [self, sampler_of, member, return_real32](CpuState& s, GuestMemory&) {
            const auto n = sampler_of(s.x[0]);
            std::lock_guard lock(self->mutex_);
            return_real32(s, (*n->sampler).*member);
        }, "f@:");
        support.method(sampler_class, setter, [self, sampler_of, member, real32, lo, hi](CpuState& s, GuestMemory&) {
            const auto n = sampler_of(s.x[0]);
            std::lock_guard lock(self->mutex_);
            (*n->sampler).*member = std::clamp(static_cast<double>(real32(s, 0)), lo, hi);
        }, "v@:f");
    }
}

void Engines::install_environment() {
    auto& support = support_;
    auto* sp = &support_;
    const auto self = shared_from_this();
    const auto environment_class = declare(support, "AVAudioEnvironmentNode", "AVAudioNode");
    const auto distance_class = declare(support, "AVAudioEnvironmentDistanceAttenuationParameters", "NSObject");
    const auto reverb_class = declare(support, "AVAudioEnvironmentReverbParameters", "NSObject");
    const auto real32 = [](const CpuState& s, unsigned i) { return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[i][0]))); };
    const auto put32 = [](CpuState& s, unsigned i, double value) { s.v[i] = {std::bit_cast<std::uint32_t>(static_cast<float>(value)), 0}; };
    support.method(environment_class, "init", [sp, environment_class](CpuState& s, GuestMemory&) {
        const auto object = sp->objc.send_super(s.x[0], environment_class, sp->objc.selector("init")).x0;
        if (object) {
            const auto n = sp->objc.host_state<Node>(object);
            n->kind = Kind::environment;
            n->environment = std::make_shared<Environment>();
        }
        s.x[0] = object;
    }, "@@:");
    const auto env_of = [self](Address object) {
        const auto n = self->node(object);
        if (!n->environment) unsupported("an AVAudioEnvironmentNode message to another node");
        return n;
    };
    // listenerPosition (AVAudio3DPoint in s0-s2) and listenerAngularOrientation (yaw, pitch, roll in s0-s2).
    support.method(environment_class, "listenerPosition", [self, env_of, put32](CpuState& s, GuestMemory&) {
        const auto n = env_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        for (unsigned i = 0; i < 3; ++i) put32(s, i, n->environment->listener[i]);
    }, "{AVAudio3DPoint=fff}@:");
    support.method(environment_class, "setListenerPosition:", [self, env_of, real32](CpuState& s, GuestMemory&) {
        const auto n = env_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        for (unsigned i = 0; i < 3; ++i) n->environment->listener[i] = real32(s, i);
    }, "v@:{AVAudio3DPoint=fff}");
    support.method(environment_class, "listenerAngularOrientation", [self, env_of, put32](CpuState& s, GuestMemory&) {
        const auto n = env_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        put32(s, 0, n->environment->yaw);
        put32(s, 1, n->environment->pitch);
        put32(s, 2, n->environment->roll);
    }, "{AVAudio3DAngularOrientation=fff}@:");
    support.method(environment_class, "setListenerAngularOrientation:", [self, env_of, real32](CpuState& s, GuestMemory&) {
        const auto n = env_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        n->environment->yaw = real32(s, 0);
        n->environment->pitch = real32(s, 1);
        n->environment->roll = real32(s, 2);
    }, "v@:{AVAudio3DAngularOrientation=fff}");
    // listenerVectorOrientation (AVAudio3DVectorOrientation {forward, up}: 24 bytes, returned through
    // x8 and passed by reference): the yaw follows the forward vector's direction in the x-z plane.
    support.method(environment_class, "listenerVectorOrientation", [self, env_of](CpuState& s, GuestMemory& m) {
        const auto n = env_of(s.x[0]);
        double yaw{};
        {
            std::lock_guard lock(self->mutex_);
            yaw = n->environment->yaw * std::numbers::pi / 180;
        }
        const std::array<float, 6> v{static_cast<float>(std::sin(yaw)), 0, static_cast<float>(-std::cos(yaw)), 0, 1, 0};
        for (unsigned i = 0; i < 6; ++i) m.write32(s.x[8] + 4 * i, std::bit_cast<std::uint32_t>(v[i]));
    }, "{AVAudio3DVectorOrientation={AVAudio3DPoint=fff}{AVAudio3DPoint=fff}}@:");
    support.method(environment_class, "setListenerVectorOrientation:", [self, env_of](CpuState& s, GuestMemory& m) {
        const auto n = env_of(s.x[0]);
        const double fx = std::bit_cast<float>(m.read32(s.x[2])), fz = std::bit_cast<float>(m.read32(s.x[2] + 8));
        std::lock_guard lock(self->mutex_);
        if (fx != 0 || fz != 0) n->environment->yaw = std::atan2(fx, -fz) * 180 / std::numbers::pi;
    }, "v@:{AVAudio3DVectorOrientation={AVAudio3DPoint=fff}{AVAudio3DPoint=fff}}");
    support.method(environment_class, "outputType", [self, env_of](CpuState& s, GuestMemory&) {
        const auto n = env_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = static_cast<std::uint64_t>(n->environment->output_type);
    }, "q@:");
    support.method(environment_class, "setOutputType:", [self, env_of](CpuState& s, GuestMemory&) {
        const auto n = env_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        n->environment->output_type = static_cast<std::int64_t>(s.x[2]);
    }, "v@:q");
    support.method(environment_class, "outputVolume", [self, env_of, put32](CpuState& s, GuestMemory&) {
        const auto n = env_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        put32(s, 0, n->output_volume);
    }, "f@:");
    support.method(environment_class, "setOutputVolume:", [self, env_of, real32](CpuState& s, GuestMemory&) {
        const auto n = env_of(s.x[0]);
        std::lock_guard lock(self->mutex_);
        n->output_volume = static_cast<float>(std::clamp(real32(s, 0), 0.0, 1.0));
    }, "v@:f");
    support.method(environment_class, "nextAvailableInputBus", [self](CpuState& s, GuestMemory&) {
        const auto n = self->node(s.x[0]);
        std::lock_guard lock(self->mutex_);
        std::uint64_t bus = 0;
        while (n->inputs.contains(static_cast<std::uint32_t>(bus))) ++bus;
        s.x[0] = bus;
    }, "Q@:");
    // AVAudio3DMixingRenderingAlgorithm: equal-power panning (0) is how every input is placed.
    support.method(environment_class, "applicableRenderingAlgorithms", [sp](CpuState& s, GuestMemory&) {
        const auto zero = sp->cf.integer(0);
        const std::array<std::uint64_t, 1> items{zero};
        const auto array = sp->cf.array(items);
        sp->release(zero);
        s.x[0] = sp->send_id(array, "autorelease");
    }, "@@:");
    // The parameter objects.
    const auto view = [sp](Address cls, std::shared_ptr<Environment> e) {
        const auto object = sp->send_id(sp->send_id(cls, "alloc"), "init");
        sp->objc.host_state<EnvironmentView>(object)->environment = std::move(e);
        return sp->send_id(object, "autorelease");
    };
    support.method(environment_class, "distanceAttenuationParameters", [env_of, view, distance_class](CpuState& s, GuestMemory&) {
        s.x[0] = view(distance_class, env_of(s.x[0])->environment);
    }, "@@:");
    support.method(environment_class, "reverbParameters", [env_of, view, reverb_class](CpuState& s, GuestMemory&) {
        s.x[0] = view(reverb_class, env_of(s.x[0])->environment);
    }, "@@:");
    const auto viewed = [sp](Address object) {
        const auto v = sp->objc.find_host_state<EnvironmentView>(object);
        if (!v || !v->environment) unsupported("an AVAudioEnvironment parameters object not taken from an AVAudioEnvironmentNode");
        return v->environment;
    };
    for (const auto& [getter, setter, field] : std::initializer_list<std::tuple<const char*, const char*, double Environment::*>>{
             {"referenceDistance", "setReferenceDistance:", &Environment::reference}, {"maximumDistance", "setMaximumDistance:", &Environment::maximum},
             {"rolloffFactor", "setRolloffFactor:", &Environment::rolloff}}) {
        const auto member = field;
        support.method(distance_class, getter, [self, viewed, put32, member](CpuState& s, GuestMemory&) {
            const auto e = viewed(s.x[0]);
            std::lock_guard lock(self->mutex_);
            put32(s, 0, (*e).*member);
        }, "f@:");
        support.method(distance_class, setter, [self, viewed, real32, member](CpuState& s, GuestMemory&) {
            const auto e = viewed(s.x[0]);
            std::lock_guard lock(self->mutex_);
            (*e).*member = std::max(0.0, real32(s, 0));
        }, "v@:f");
    }
    support.method(distance_class, "distanceAttenuationModel", [self, viewed](CpuState& s, GuestMemory&) {
        const auto e = viewed(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = static_cast<std::uint64_t>(e->model);
    }, "q@:");
    support.method(distance_class, "setDistanceAttenuationModel:", [self, viewed](CpuState& s, GuestMemory&) {
        const auto e = viewed(s.x[0]);
        std::lock_guard lock(self->mutex_);
        e->model = std::clamp<std::int64_t>(static_cast<std::int64_t>(s.x[2]), 1, 3);
    }, "v@:q");
    support.method(reverb_class, "enable", [self, viewed](CpuState& s, GuestMemory&) {
        const auto e = viewed(s.x[0]);
        std::lock_guard lock(self->mutex_);
        s.x[0] = e->reverb ? 1 : 0;
    }, "B@:");
    support.method(reverb_class, "setEnable:", [self, viewed](CpuState& s, GuestMemory&) {
        const auto e = viewed(s.x[0]);
        std::lock_guard lock(self->mutex_);
        e->reverb = (s.x[2] & 0xff) != 0;
    }, "v@:B");
    support.method(reverb_class, "level", [self, viewed, put32](CpuState& s, GuestMemory&) {
        const auto e = viewed(s.x[0]);
        std::lock_guard lock(self->mutex_);
        put32(s, 0, e->reverb_level);
    }, "f@:");
    support.method(reverb_class, "setLevel:", [self, viewed, real32](CpuState& s, GuestMemory&) {
        const auto e = viewed(s.x[0]);
        std::lock_guard lock(self->mutex_);
        e->reverb_level = std::clamp(real32(s, 0), -40.0, 40.0);
    }, "v@:f");
    support.method(reverb_class, "loadFactoryReverbPreset:", [self, viewed](CpuState& s, GuestMemory&) {
        const auto e = viewed(s.x[0]);
        std::lock_guard lock(self->mutex_);
        e->reverb_unit.load_preset(static_cast<std::int64_t>(s.x[2]));
        e->reverb_unit.reset();
    }, "v@:q");
    support.method(reverb_class, "filterParameters", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");   // no reverb filter

    // AVAudio3DMixing on the nodes that feed an environment (players, sources, samplers, mixers, input).
    const auto sampler_class = support.objc.find_class("AVAudioUnitSampler");
    for (const auto cls : {player_class_, source_class_, mixer_class_, input_class_, sampler_class}) {
        if (!cls) continue;
        support.method(cls, "position", [self, put32](CpuState& s, GuestMemory&) {
            const auto n = self->node(s.x[0]);
            std::lock_guard lock(self->mutex_);
            for (unsigned i = 0; i < 3; ++i) put32(s, i, n->position[i]);
        }, "{AVAudio3DPoint=fff}@:");
        support.method(cls, "setPosition:", [self, real32](CpuState& s, GuestMemory&) {
            const auto n = self->node(s.x[0]);
            std::lock_guard lock(self->mutex_);
            for (unsigned i = 0; i < 3; ++i) n->position[i] = static_cast<float>(real32(s, i));
        }, "v@:{AVAudio3DPoint=fff}");
        for (const auto& [getter, setter, field] : std::initializer_list<std::tuple<const char*, const char*, float Node::*>>{
                 {"reverbBlend", "setReverbBlend:", &Node::reverb_blend}, {"obstruction", "setObstruction:", &Node::obstruction},
                 {"occlusion", "setOcclusion:", &Node::occlusion}, {"rate", "setRate:", &Node::mixing_rate}}) {
            const auto member = field;
            support.method(cls, getter, [self, put32, member](CpuState& s, GuestMemory&) {
                const auto n = self->node(s.x[0]);
                std::lock_guard lock(self->mutex_);
                put32(s, 0, (*n).*member);
            }, "f@:");
            support.method(cls, setter, [self, real32, member](CpuState& s, GuestMemory&) {
                const auto n = self->node(s.x[0]);
                std::lock_guard lock(self->mutex_);
                (*n).*member = static_cast<float>(real32(s, 0));
            }, "v@:f");
        }
        for (const auto& [getter, setter, field] : std::initializer_list<std::tuple<const char*, const char*, std::int64_t Node::*>>{
                 {"renderingAlgorithm", "setRenderingAlgorithm:", &Node::rendering_algorithm}, {"sourceMode", "setSourceMode:", &Node::source_mode},
                 {"pointSourceInHeadMode", "setPointSourceInHeadMode:", &Node::point_source_in_head_mode}}) {
            const auto member = field;
            support.method(cls, getter, [self, member](CpuState& s, GuestMemory&) {
                const auto n = self->node(s.x[0]);
                std::lock_guard lock(self->mutex_);
                s.x[0] = static_cast<std::uint64_t>((*n).*member);
            }, "q@:");
            support.method(cls, setter, [self, member](CpuState& s, GuestMemory&) {
                const auto n = self->node(s.x[0]);
                std::lock_guard lock(self->mutex_);
                (*n).*member = static_cast<std::int64_t>(s.x[2]);
            }, "v@:q");
        }
    }
    // The sampler and environment take volume and pan too (AVAudioMixing).
    for (const auto cls : {sampler_class, environment_class}) {
        if (!cls) continue;
        for (const auto& [getter, setter, field, low, high] : std::initializer_list<std::tuple<const char*, const char*, float Node::*, float, float>>{
                 {"volume", "setVolume:", &Node::volume, 0.0f, 1.0f}, {"pan", "setPan:", &Node::pan, -1.0f, 1.0f}}) {
            const auto member = field;
            const auto lo = low, hi = high;
            support.method(cls, getter, [self, put32, member](CpuState& s, GuestMemory&) {
                const auto n = self->node(s.x[0]);
                std::lock_guard lock(self->mutex_);
                put32(s, 0, (*n).*member);
            }, "f@:");
            support.method(cls, setter, [self, real32, member, lo, hi](CpuState& s, GuestMemory&) {
                const auto n = self->node(s.x[0]);
                std::lock_guard lock(self->mutex_);
                (*n).*member = std::clamp(static_cast<float>(real32(s, 0)), lo, hi);
            }, "v@:f");
        }
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
        s.x[0] = n->kind == Kind::mixer || n->kind == Kind::environment ? (n->inputs.empty() ? 1 : n->inputs.rbegin()->first + 2)
               : n->kind == Kind::output || n->kind == Kind::sink || n->kind == Kind::effect ? 1 : 0;
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
        if (!s.x[0] && s.x[2] == 0 && (n->kind == Kind::player || n->kind == Kind::sampler || n->kind == Kind::environment)) {
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
    install_effects();
    install_instruments();
    install_environment();

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
