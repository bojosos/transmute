#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

// AudioComponent and the output audio units (<AudioToolbox/AudioComponent.h>,
// <AudioToolbox/AUComponent.h>, <AudioUnit/AudioUnitProperties.h>).
//
// - Components: Apple's three output units, kAudioUnitType_Output ('auou') from 'appl': the
//   default output ('def ') and the system output ('sys ') play to the default output device,
//   AUHAL ('ahal') to kAudioOutputUnitProperty_CurrentDevice. AudioComponentFindNext,
//   AudioComponentCount and the descriptions, names and versions list them; no other component
//   exists and AudioComponentRegister registers none.
// - An output unit plays through the device's I/O (hal_io.hpp) while it runs: on the device's
//   I/O thread it pulls its input with the render callback (kAudioUnitProperty_SetRenderCallback,
//   with the pre- and post-render notifications) in its input stream format (any linear PCM:
//   integer or float, interleaved or not, any rate), converts it to the device's float format,
//   maps its channels (kAudioOutputUnitProperty_ChannelMap, else mono to every channel and the
//   first channels in order), resamples to the device's rate (resampler.hpp), applies
//   kHALOutputParam_Volume and adds it to the device's output. Render calls never exceed
//   kAudioUnitProperty_MaximumFramesPerSlice frames. Sample times count the unit's input frames
//   from its start.
// - AudioUnitRender pulls the render callback into the caller's buffers (bus 0). Input (bus 1,
//   kAudioOutputUnitProperty_EnableIO on the input scope) is not provided: enabling it makes
//   AudioUnitInitialize fail with kAudioUnitErr_FailedInitialization, and pulling bus 1 answers
//   kAudioUnitErr_NoConnection.
// - Property listeners (AudioUnitAddPropertyListener) are called on the setting thread when a
//   property they watch is set, and for kAudioOutputUnitProperty_IsRunning on start and stop.
namespace alchemy::gui::completion_kit::media {
namespace {

namespace af = audio_file;

constexpr std::string_view audio_unit_library = "/System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit";

constexpr std::uint32_t four(const char (&code)[5]) { return af::four(code); }

// OSStatus values (AUComponent.h, AudioComponent.h, MacErrors.h).
namespace error {
inline constexpr std::int32_t invalid_property = -10879, invalid_parameter = -10878, invalid_element = -10877, no_connection = -10876,
    failed_initialization = -10875, too_many_frames = -10874, format_not_supported = -10868, uninitialized = -10867, invalid_scope = -10866,
    property_not_writable = -10865, invalid_property_value = -10851, initialized = -10849, param = -50,
    bad_component_instance = static_cast<std::int32_t>(0x80008001U), invalid_device = -10851;
}

// Scopes.
constexpr std::uint32_t scope_global = 0, scope_input = 1, scope_output = 2;
// Properties.
constexpr std::uint32_t prop_class_info = 0, prop_make_connection = 1, prop_sample_rate = 2, prop_parameter_list = 3,
    prop_stream_format = 8, prop_element_count = 11, prop_latency = 12, prop_supported_channel_layout_tags = 13,
    prop_maximum_frames_per_slice = 14, prop_audio_channel_layout = 19, prop_tail_time = 20, prop_bypass = 21,
    prop_last_render_error = 22, prop_set_render_callback = 23, prop_render_quality = 26, prop_should_allocate_buffer = 51,
    prop_current_device = 2000, prop_is_running = 2001, prop_channel_map = 2002, prop_enable_io = 2003, prop_start_time = 2004,
    prop_set_input_callback = 2005, prop_has_io = 2006, prop_start_timestamps_at_zero = 2007, prop_buffer_frame_size = four("fsiz");
// kHALOutputParam_Volume.
constexpr std::uint32_t param_volume = 14;
// AudioUnitRenderActionFlags.
constexpr std::uint32_t action_pre_render = 1U << 2, action_post_render = 1U << 3, action_output_is_silence = 1U << 4;

struct Component {
    std::uint32_t type{}, subtype{}, manufacturer{};
    const char* name{};
    bool follows_default{};            // the default and system output units
    std::uint64_t handle{};
};

struct Listener { std::uint32_t property{}; std::uint64_t proc{}, refcon{}; };
struct Notify { std::uint64_t proc{}, refcon{}; };

struct Unit {
    const Component* component{};
    std::uint64_t handle{};
    std::recursive_mutex mutex;
    bool initialized{}, running{};
    std::uint32_t device{};            // AUHAL's current device (0: the default output)
    std::vector<std::uint8_t> client_asbd;
    af::Pcm client;
    std::uint64_t render_proc{}, render_refcon{};
    std::uint64_t input_proc{}, input_refcon{};
    std::uint32_t max_frames{4096};
    std::vector<std::int32_t> channel_map;
    bool enable_output{true}, enable_input{};
    bool start_at_zero{true};
    double volume{1};
    std::int32_t last_error{};
    std::vector<Notify> notifies;
    std::vector<Listener> listeners;
    // While running:
    std::shared_ptr<hal_io::DeviceIo> io;
    std::uint32_t playing_device{};
    std::unique_ptr<Resampler> resampler;
    bool reset_pending{};              // AudioUnitReset: the resampler forgets its input
    double sample_time{};
    std::uint64_t memory{};            // the render buffers: a timestamp, a buffer list, the data
    std::size_t capacity{};
};

std::vector<std::uint8_t> float_asbd(double rate, std::uint32_t channels, bool non_interleaved) {
    af::Pcm f;
    f.rate = rate;
    f.channels = channels;
    f.bits = 32;
    f.container = 4;
    f.is_float = true;
    f.is_signed = true;
    f.non_interleaved = non_interleaved;
    return af::asbd_from_pcm(f);
}

class AudioUnits : public std::enable_shared_from_this<AudioUnits> {
public:
    explicit AudioUnits(Support& support) : support_(support) {}
    void install();

private:
    std::shared_ptr<Unit> unit(std::uint64_t handle) {
        std::lock_guard lock(mutex_);
        const auto found = units_.find(handle);
        return found == units_.end() ? nullptr : found->second;
    }
    const Component* component(std::uint64_t handle) const {
        for (const auto& c : components_)
            if (c.handle == handle) return &c;
        return nullptr;
    }
    std::uint32_t device_of(const Unit& u) const { return u.device ? u.device : hal_default_output(); }
    // The device's format (rate, channels), from its I/O.
    std::pair<double, std::uint32_t> device_format(std::uint32_t device) const {
        if (const auto io = hal_device_io(device)) return {io->rate(), io->channels()};
        return {48000.0, 2};
    }
    void notify_listeners(Unit& u, std::uint32_t property, std::uint32_t scope, std::uint32_t element);
    std::int32_t get_property(Unit& u, std::uint32_t id, std::uint32_t scope, std::uint32_t element, std::vector<std::uint8_t>& out, bool& writable);
    std::int32_t set_property(Unit& u, std::uint32_t id, std::uint32_t scope, std::uint32_t element, std::span<const std::uint8_t> data);
    std::int32_t start(Unit& u);
    void stop(Unit& u);
    // Pulls `frames` input frames through the render callback into the unit's guest buffers;
    // returns the status and whether the callback said the output is silence.
    std::int32_t pull(Unit& u, std::uint32_t frames, std::uint64_t host_time, std::uint64_t list, std::uint32_t& flags);
    std::uint64_t buffers(Unit& u, std::uint32_t frames);
    void render(Unit& u, float* mix, std::uint32_t frames, std::uint32_t device_channels, const hal_io::Timing& timing);
    std::uint64_t call(std::uint64_t function, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(support_.process.calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
    }

    Support& support_;
    std::mutex mutex_;
    std::vector<Component> components_;
    std::map<std::uint64_t, std::shared_ptr<Unit>> units_;
};

void AudioUnits::notify_listeners(Unit& u, std::uint32_t property, std::uint32_t scope, std::uint32_t element) {
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(u.mutex);
        for (const auto& l : u.listeners)
            if (l.property == property) listeners.push_back(l);
    }
    for (const auto& l : listeners) static_cast<void>(call(l.proc, {l.refcon, u.handle, property, scope, element}));
}

std::int32_t AudioUnits::get_property(Unit& u, std::uint32_t id, std::uint32_t scope, std::uint32_t element, std::vector<std::uint8_t>& out,
                                      bool& writable) {
    std::lock_guard lock(u.mutex);
    const auto u32 = [&](std::uint32_t value) { out.resize(4); std::memcpy(out.data(), &value, 4); };
    const auto f64 = [&](double value) { out.resize(8); std::memcpy(out.data(), &value, 8); };
    const bool hal = u.component->subtype == four("ahal");
    writable = false;
    if (scope > scope_output) return error::invalid_scope;
    switch (id) {
    case prop_stream_format:
        if (element > 1) return error::invalid_element;
        if (scope == scope_global) return error::invalid_scope;
        if (element == 0 && scope == scope_input) { out = u.client_asbd; writable = true; return 0; }
        if (element == 0 && scope == scope_output) {
            const auto [rate, channels] = device_format(device_of(u));
            out = float_asbd(rate, channels, false);
            return 0;
        }
        // Element 1, the input side: the input device's format (none here).
        out = float_asbd(device_format(device_of(u)).first, 2, true);
        writable = scope == scope_output;
        return 0;
    case prop_sample_rate:
        if (scope == scope_input) { f64(u.client.rate); writable = true; return 0; }
        f64(device_format(device_of(u)).first);
        return 0;
    case prop_element_count:
        u32(hal ? 2 : 1);
        return 0;
    case prop_latency:
        f64(0);
        return 0;
    case prop_tail_time:
        f64(0);
        return 0;
    case prop_maximum_frames_per_slice:
        u32(u.max_frames);
        writable = true;
        return 0;
    case prop_last_render_error:
        u32(static_cast<std::uint32_t>(u.last_error));
        return 0;
    case prop_set_render_callback:
        return error::invalid_property;   // write-only
    case prop_should_allocate_buffer:
        u32(1);
        writable = true;
        return 0;
    case prop_current_device:
        u32(device_of(u));
        writable = hal;
        return 0;
    case prop_is_running:
        u32(u.running ? 1 : 0);
        return 0;
    case prop_channel_map: {
        const auto channels = device_format(device_of(u)).second;
        out.resize(4ULL * channels);
        for (std::uint32_t i = 0; i < channels; ++i) {
            const std::int32_t value = i < u.channel_map.size() ? u.channel_map[i] : static_cast<std::int32_t>(i < u.client.channels ? i : -1);
            std::memcpy(out.data() + 4ULL * i, &value, 4);
        }
        writable = true;
        return 0;
    }
    case prop_enable_io:
        if (element == 1 && scope == scope_input) u32(u.enable_input ? 1 : 0);
        else if (element == 0 && scope == scope_output) u32(u.enable_output ? 1 : 0);
        else return error::invalid_element;
        writable = hal;
        return 0;
    case prop_has_io:
        if (element == 1 && scope == scope_input) u32(0);   // no input device
        else if (element == 0 && scope == scope_output) u32(1);
        else return error::invalid_element;
        return 0;
    case prop_start_timestamps_at_zero:
        u32(u.start_at_zero ? 1 : 0);
        writable = true;
        return 0;
    case prop_buffer_frame_size:
        u32(hal_buffer_frames(device_of(u)));
        writable = true;
        return 0;
    default:
        return error::invalid_property;
    }
}

std::int32_t AudioUnits::set_property(Unit& u, std::uint32_t id, std::uint32_t scope, std::uint32_t element, std::span<const std::uint8_t> data) {
    const auto u32 = [&](std::uint32_t& value) {
        if (data.size() < 4) return false;
        std::memcpy(&value, data.data(), 4);
        return true;
    };
    std::int32_t result = 0;
    {
        std::lock_guard lock(u.mutex);
        const bool hal = u.component->subtype == four("ahal");
        if (scope > scope_output) return error::invalid_scope;
        switch (id) {
        case prop_stream_format: {
            if (element > 1) return error::invalid_element;
            if (element == 1) return 0;   // the input side: no input device, the format is kept nowhere
            if (scope != scope_input) return error::property_not_writable;
            std::string why;
            const auto pcm = af::pcm_from_asbd(data, why);
            if (!pcm) return error::format_not_supported;
            if (u.initialized && u.running) return error::initialized;
            u.client = *pcm;
            u.client_asbd.assign(data.begin(), data.begin() + 40);
            break;
        }
        case prop_sample_rate: {
            if (scope != scope_input || data.size() < 8) return scope != scope_input ? error::property_not_writable : error::param;
            double rate{};
            std::memcpy(&rate, data.data(), 8);
            if (!(rate > 0)) return error::invalid_property_value;
            u.client.rate = rate;
            u.client_asbd = af::asbd_from_pcm(u.client);
            break;
        }
        case prop_maximum_frames_per_slice: {
            std::uint32_t value{};
            if (!u32(value) || value == 0) return error::invalid_property_value;
            u.max_frames = value;
            break;
        }
        case prop_set_render_callback:
            if (data.size() < 16) return error::param;
            std::memcpy(&u.render_proc, data.data(), 8);
            std::memcpy(&u.render_refcon, data.data() + 8, 8);
            break;
        case prop_set_input_callback:
            if (data.size() < 16) return error::param;
            std::memcpy(&u.input_proc, data.data(), 8);
            std::memcpy(&u.input_refcon, data.data() + 8, 8);
            break;
        case prop_should_allocate_buffer:
        case prop_render_quality:
        case prop_bypass:
            break;
        case prop_current_device: {
            std::uint32_t device{};
            if (!hal) return error::property_not_writable;
            if (!u32(device)) return error::param;
            if (device && !hal_device_io(device)) return error::invalid_device;
            if (u.running) return error::initialized;
            u.device = device;
            break;
        }
        case prop_channel_map: {
            u.channel_map.resize(data.size() / 4);
            for (std::size_t i = 0; i < u.channel_map.size(); ++i) std::memcpy(&u.channel_map[i], data.data() + 4 * i, 4);
            break;
        }
        case prop_enable_io: {
            std::uint32_t value{};
            if (!hal) return error::property_not_writable;
            if (!u32(value)) return error::param;
            if (u.initialized) return error::initialized;
            if (element == 1 && scope == scope_input) u.enable_input = value != 0;
            else if (element == 0 && scope == scope_output) u.enable_output = value != 0;
            else return error::invalid_element;
            break;
        }
        case prop_start_timestamps_at_zero: {
            std::uint32_t value{};
            if (!u32(value)) return error::param;
            u.start_at_zero = value != 0;
            break;
        }
        case prop_buffer_frame_size: {
            std::uint32_t value{};
            if (!u32(value)) return error::param;
            result = hal_set_buffer_frames(device_of(u), value);
            break;
        }
        case prop_make_connection:
            return error::invalid_property;   // no other audio unit exists to connect
        default:
            return error::invalid_property;
        }
    }
    if (!result) notify_listeners(u, id, scope, element);
    return result;
}

std::uint64_t AudioUnits::buffers(Unit& u, std::uint32_t frames) {
    // [AudioTimeStamp 64][flags 8][AudioBufferList 8 + 16 * buffers][data]
    const auto channel_buffers = u.client.non_interleaved ? u.client.channels : 1U;
    const auto data_bytes = static_cast<std::size_t>(frames) * u.client.frame_bytes() * channel_buffers;
    const auto header = 64 + 8 + 8 + 16ULL * channel_buffers;
    if (u.capacity < header + data_bytes) {
        if (u.memory) support_.process.release_guest(u.memory);
        u.memory = support_.process.allocate_guest(header + data_bytes, 16);
        u.capacity = header + data_bytes;
    }
    return u.memory;
}

std::int32_t AudioUnits::pull(Unit& u, std::uint32_t frames, std::uint64_t host_time, std::uint64_t list, std::uint32_t& flags) {
    auto& m = support_.process.calling_engine();
    const auto base = u.memory;
    const auto stamp = base, flags_at = base + 64;
    m.fill_bytes(stamp, 0, 64);
    m.write64(stamp, std::bit_cast<std::uint64_t>(u.sample_time));
    m.write64(stamp + 8, host_time);
    m.write64(stamp + 16, std::bit_cast<std::uint64_t>(1.0));
    m.write32(stamp + 56, 7);
    std::vector<Notify> notifies;
    std::uint64_t proc{}, refcon{};
    {
        std::lock_guard lock(u.mutex);
        notifies = u.notifies;
        proc = u.render_proc;
        refcon = u.render_refcon;
    }
    for (const auto& n : notifies) {
        m.write32(flags_at, action_pre_render);
        static_cast<void>(call(n.proc, {n.refcon, flags_at, stamp, 0, frames, list}));
    }
    m.write32(flags_at, 0);
    std::int32_t status = 0;
    if (proc) status = static_cast<std::int32_t>(call(proc, {refcon, flags_at, stamp, 0, frames, list}));
    else flags |= action_output_is_silence;   // no input: silence
    flags |= m.read32(flags_at);
    for (const auto& n : notifies) {
        m.write32(flags_at, action_post_render);
        static_cast<void>(call(n.proc, {n.refcon, flags_at, stamp, 0, frames, list}));
    }
    u.sample_time += frames;
    return status;
}

void AudioUnits::render(Unit& u, float* mix, std::uint32_t frames, std::uint32_t device_channels, const hal_io::Timing& timing) {
    auto& m = support_.process.calling_engine();
    const auto client = u.client;
    const auto channel_buffers = client.non_interleaved ? client.channels : 1U;
    std::vector<double> converted(static_cast<std::size_t>(frames) * device_channels);
    // Input frames in the client's channels -> device channels (the channel map, else mono to
    // every channel and the first channels in order).
    std::vector<std::int32_t> map(device_channels);
    {
        std::lock_guard lock(u.mutex);
        for (std::uint32_t i = 0; i < device_channels; ++i)
            map[i] = i < u.channel_map.size() ? u.channel_map[i]
                   : client.channels == 1 ? 0
                   : static_cast<std::int32_t>(i < client.channels ? i : -1);
    }
    const auto pull_frames = [&](double* out, std::size_t count) {
        std::size_t done = 0;
        while (done < count) {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::size_t>(count - done, u.max_frames));
            const auto base = buffers(u, chunk);
            const auto list = base + 72, data = list + 8 + 16ULL * channel_buffers;
            const auto buffer_bytes = static_cast<std::size_t>(chunk) * client.frame_bytes();
            m.write32(list, channel_buffers);
            for (std::uint32_t b = 0; b < channel_buffers; ++b) {
                const auto entry = list + 8 + 16ULL * b;
                m.write32(entry, client.non_interleaved ? 1 : client.channels);
                m.write32(entry + 4, static_cast<std::uint32_t>(buffer_bytes));
                m.write64(entry + 8, data + b * buffer_bytes);
            }
            m.fill_bytes(data, 0, buffer_bytes * channel_buffers);
            std::uint32_t flags = 0;
            const auto status = pull(u, chunk, timing.output_host, list, flags);
            if (status) u.last_error = status;
            std::vector<double> samples;
            if (status || (flags & action_output_is_silence)) {
                samples.assign(static_cast<std::size_t>(chunk) * client.channels, 0.0);
            } else {
                std::vector<std::vector<std::uint8_t>> storage(channel_buffers);
                std::vector<std::span<const std::uint8_t>> spans;
                for (std::uint32_t b = 0; b < channel_buffers; ++b) {
                    const auto entry = list + 8 + 16ULL * b;
                    const auto at = m.read64(entry + 8);
                    const auto size = std::min<std::size_t>(m.read32(entry + 4), buffer_bytes);
                    storage[b].assign(buffer_bytes, 0);
                    if (at && size) m.read_bytes(at, std::span(storage[b].data(), size));
                    spans.emplace_back(storage[b]);
                }
                samples = af::decode(client, spans, chunk);
            }
            for (std::uint32_t frame = 0; frame < chunk; ++frame)
                for (std::uint32_t c = 0; c < device_channels; ++c) {
                    const auto source = map[c];
                    out[(done + frame) * device_channels + c] =
                        source >= 0 && static_cast<std::uint32_t>(source) < client.channels ? samples[frame * client.channels + source] : 0.0;
                }
            done += chunk;
        }
    };
    {
        std::lock_guard lock(u.mutex);
        if (std::exchange(u.reset_pending, false) && u.resampler) u.resampler->reset();
    }
    if (!u.resampler) u.resampler = std::make_unique<Resampler>(client.rate, timing.rate, device_channels);
    u.resampler->render(converted.data(), frames, pull_frames);
    double volume{};
    {
        std::lock_guard lock(u.mutex);
        volume = u.volume;
    }
    for (std::size_t i = 0; i < converted.size(); ++i) mix[i] += static_cast<float>(converted[i] * volume);
}

std::int32_t AudioUnits::start(Unit& u) {
    std::shared_ptr<hal_io::DeviceIo> io;
    {
        std::lock_guard lock(u.mutex);
        if (!u.initialized) return error::uninitialized;
        if (u.running) return 0;
        u.playing_device = device_of(u);
        io = hal_device_io(u.playing_device);
        if (!io) return error::invalid_device;
        u.io = io;
        u.resampler.reset();
        if (u.start_at_zero) u.sample_time = 0;
    }
    auto self = shared_from_this();
    auto unit_ptr = this->unit(u.handle);
    const auto channels = io->channels();
    hal_io::Client client = [self, unit_ptr, channels](float* mix, std::uint32_t frames, const hal_io::Timing& timing) {
        self->render(*unit_ptr, mix, frames, channels, timing);
    };
    const auto status = io->start(u.handle, std::move(client), hal_buffer_frames(u.playing_device));
    if (status) return status;
    {
        std::lock_guard lock(u.mutex);
        u.running = true;
    }
    notify_listeners(u, prop_is_running, scope_global, 0);
    return 0;
}

void AudioUnits::stop(Unit& u) {
    std::shared_ptr<hal_io::DeviceIo> io;
    {
        std::lock_guard lock(u.mutex);
        if (!u.running) return;
        u.running = false;
        io = std::move(u.io);
    }
    if (io) io->stop(u.handle);
    notify_listeners(u, prop_is_running, scope_global, 0);
}

void AudioUnits::install() {
    components_ = {
        {four("auou"), four("def "), four("appl"), "Apple: DefaultOutputUnit", true},
        {four("auou"), four("sys "), four("appl"), "Apple: SystemOutputUnit", true},
        {four("auou"), four("ahal"), four("appl"), "Apple: AUHAL", false},
    };
    for (auto& c : components_) {
        c.handle = support_.process.allocate_guest(16, 16);
        support_.memory().fill_bytes(c.handle, 0, 16);
    }
    auto self = shared_from_this();
    const auto status = [](CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); };
    // AudioComponentDescription {type, subtype, manufacturer, flags, flagsMask}: zero fields match anything.
    const auto matches = [](GuestMemory& m, std::uint64_t desc, const Component& c) {
        if (!desc) return true;
        const auto type = m.read32(desc), subtype = m.read32(desc + 4), manufacturer = m.read32(desc + 8), mask = m.read32(desc + 16);
        if (type && type != c.type) return false;
        if (subtype && subtype != c.subtype) return false;
        if (manufacturer && manufacturer != c.manufacturer) return false;
        return (0 & mask) == (m.read32(desc + 12) & mask);
    };

    // ---- components ----
    support_.function(audio_unit_library, "_AudioComponentFindNext", [self, matches](CpuState& s, GuestMemory& m) {
        bool after = s.x[0] == 0;
        for (const auto& c : self->components_) {
            if (!after) {
                after = c.handle == s.x[0];
                continue;
            }
            if (matches(m, s.x[1], c)) {
                s.x[0] = c.handle;
                return;
            }
        }
        s.x[0] = 0;
    });
    support_.function(audio_unit_library, "_AudioComponentCount", [self, matches](CpuState& s, GuestMemory& m) {
        std::uint32_t count = 0;
        for (const auto& c : self->components_) count += matches(m, s.x[0], c) ? 1 : 0;
        s.x[0] = count;
    });
    support_.function(audio_unit_library, "_AudioComponentGetDescription", [self, status](CpuState& s, GuestMemory& m) {
        const auto* c = self->component(s.x[0]);
        if (!c || !s.x[1]) return status(s, error::param);
        m.write32(s.x[1], c->type);
        m.write32(s.x[1] + 4, c->subtype);
        m.write32(s.x[1] + 8, c->manufacturer);
        m.write32(s.x[1] + 12, 0);
        m.write32(s.x[1] + 16, 0);
        status(s, 0);
    });
    support_.function(audio_unit_library, "_AudioComponentCopyName", [self, status](CpuState& s, GuestMemory& m) {
        const auto* c = self->component(s.x[0]);
        if (s.x[1]) m.write64(s.x[1], 0);
        if (!c || !s.x[1]) return status(s, error::param);
        m.write64(s.x[1], self->support_.cf.string(c->name));
        status(s, 0);
    });
    support_.function(audio_unit_library, "_AudioComponentGetVersion", [self, status](CpuState& s, GuestMemory& m) {
        const auto* c = self->component(s.x[0]);
        if (!c || !s.x[1]) return status(s, error::param);
        m.write32(s.x[1], 0x00010000);
        status(s, 0);
    });
    // AudioComponentRegister(desc, name, version, factory): no component can be added.
    support_.function(audio_unit_library, "_AudioComponentRegister", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    support_.function(audio_unit_library, "_AudioComponentInstanceNew", [self, status](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        const auto* c = self->component(s.x[0]);
        if (!c || !s.x[1]) return status(s, error::param);
        auto made = std::make_shared<Unit>();
        made->component = c;
        made->handle = self->support_.process.allocate_guest(64, 16);
        m.fill_bytes(made->handle, 0, 64);
        const auto [rate, channels] = self->device_format(hal_default_output());
        made->client_asbd = float_asbd(rate, std::min(channels, 2U), true);
        std::string why;
        made->client = *af::pcm_from_asbd(made->client_asbd, why);
        {
            std::lock_guard lock(self->mutex_);
            self->units_[made->handle] = made;
        }
        m.write64(s.x[1], made->handle);
        status(s, 0);
    });
    support_.function(audio_unit_library, "_AudioComponentInstanceDispose", [self, status](CpuState& s, GuestMemory&) {
        std::shared_ptr<Unit> gone;
        {
            std::lock_guard lock(self->mutex_);
            const auto found = self->units_.find(s.x[0]);
            if (found == self->units_.end()) return status(s, error::bad_component_instance);
            gone = std::move(found->second);
            self->units_.erase(found);
        }
        self->stop(*gone);
        if (gone->memory) self->support_.process.release_guest(gone->memory);
        self->support_.process.release_guest(gone->handle);
        status(s, 0);
    });
    support_.function(audio_unit_library, "_AudioComponentInstanceGetComponent", [self](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        s.x[0] = u ? u->component->handle : 0;
    });
    support_.function(audio_unit_library, "_AudioComponentInstanceCanDo", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });

    // ---- units ----
    support_.function(audio_unit_library, "_AudioUnitInitialize", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        std::lock_guard lock(u->mutex);
        if (u->enable_input) return status(s, error::failed_initialization);   // no input device
        u->initialized = true;
        status(s, 0);
    });
    support_.function(audio_unit_library, "_AudioUnitUninitialize", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        self->stop(*u);
        std::lock_guard lock(u->mutex);
        u->initialized = false;
        status(s, 0);
    });
    support_.function(audio_unit_library, "_AudioOutputUnitStart", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        status(s, self->start(*u));
    });
    support_.function(audio_unit_library, "_AudioOutputUnitStop", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        self->stop(*u);
        status(s, 0);
    });
    // AudioUnitGetPropertyInfo(unit, id, scope, element, UInt32 *outDataSize, Boolean *outWritable)
    support_.function(audio_unit_library, "_AudioUnitGetPropertyInfo", [self, status](CpuState& s, GuestMemory& m) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        std::vector<std::uint8_t> value;
        bool writable{};
        auto result = self->get_property(*u, static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]),
                                         static_cast<std::uint32_t>(s.x[3]), value, writable);
        const auto id = static_cast<std::uint32_t>(s.x[1]);
        if (result == error::invalid_property && (id == prop_set_render_callback || id == prop_set_input_callback)) {
            value.resize(16);
            writable = true;
            result = 0;
        }
        if (result) return status(s, result);
        if (s.x[4]) m.write32(s.x[4], static_cast<std::uint32_t>(value.size()));
        if (s.x[5]) m.write8(s.x[5], writable ? 1 : 0);
        status(s, 0);
    });
    // AudioUnitGetProperty(unit, id, scope, element, void *outData, UInt32 *ioDataSize)
    support_.function(audio_unit_library, "_AudioUnitGetProperty", [self, status](CpuState& s, GuestMemory& m) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        if (!s.x[5]) return status(s, error::param);
        std::vector<std::uint8_t> value;
        bool writable{};
        if (const auto result = self->get_property(*u, static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]),
                                                   static_cast<std::uint32_t>(s.x[3]), value, writable))
            return status(s, result);
        const auto room = m.read32(s.x[5]);
        if (room < value.size() && static_cast<std::uint32_t>(s.x[1]) != prop_channel_map) return status(s, error::param);
        const auto size = std::min<std::size_t>(room, value.size());
        if (size && !s.x[4]) return status(s, error::param);
        if (size) m.write_bytes(s.x[4], std::span(value.data(), size));
        m.write32(s.x[5], static_cast<std::uint32_t>(size));
        status(s, 0);
    });
    // AudioUnitSetProperty(unit, id, scope, element, const void *inData, UInt32 inDataSize)
    support_.function(audio_unit_library, "_AudioUnitSetProperty", [self, status](CpuState& s, GuestMemory& m) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        std::vector<std::uint8_t> data(static_cast<std::uint32_t>(s.x[5]));
        if (!data.empty()) {
            if (!s.x[4]) return status(s, error::param);
            m.read_bytes(s.x[4], data);
        }
        status(s, self->set_property(*u, static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]),
                                     static_cast<std::uint32_t>(s.x[3]), data));
    });
    // AudioUnitAddPropertyListener(unit, id, proc, refcon)
    support_.function(audio_unit_library, "_AudioUnitAddPropertyListener", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        if (!s.x[2]) return status(s, error::param);
        std::lock_guard lock(u->mutex);
        u->listeners.push_back({static_cast<std::uint32_t>(s.x[1]), s.x[2], s.x[3]});
        status(s, 0);
    });
    for (const auto* name : {"_AudioUnitRemovePropertyListenerWithUserData", "_AudioUnitRemovePropertyListener"}) {
        const bool with_data = std::string_view(name) == "_AudioUnitRemovePropertyListenerWithUserData";
        support_.function(audio_unit_library, name, [self, status, with_data](CpuState& s, GuestMemory&) {
            const auto u = self->unit(s.x[0]);
            if (!u) return status(s, error::bad_component_instance);
            std::lock_guard lock(u->mutex);
            std::erase_if(u->listeners, [&](const Listener& l) {
                return l.property == static_cast<std::uint32_t>(s.x[1]) && l.proc == s.x[2] && (!with_data || l.refcon == s.x[3]);
            });
            status(s, 0);
        });
    }
    support_.function(audio_unit_library, "_AudioUnitAddRenderNotify", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        if (!s.x[1]) return status(s, error::param);
        std::lock_guard lock(u->mutex);
        u->notifies.push_back({s.x[1], s.x[2]});
        status(s, 0);
    });
    support_.function(audio_unit_library, "_AudioUnitRemoveRenderNotify", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        std::lock_guard lock(u->mutex);
        std::erase_if(u->notifies, [&](const Notify& n) { return n.proc == s.x[1] && n.refcon == s.x[2]; });
        status(s, 0);
    });
    // AudioUnitGetParameter(unit, id, scope, element, AudioUnitParameterValue *out)
    support_.function(audio_unit_library, "_AudioUnitGetParameter", [self, status](CpuState& s, GuestMemory& m) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        if (!s.x[4]) return status(s, error::param);
        if (static_cast<std::uint32_t>(s.x[1]) != param_volume) return status(s, error::invalid_parameter);
        std::lock_guard lock(u->mutex);
        m.write32(s.x[4], std::bit_cast<std::uint32_t>(static_cast<float>(u->volume)));
        status(s, 0);
    });
    // AudioUnitSetParameter(unit, id, scope, element, AudioUnitParameterValue value (s0), UInt32 offset)
    support_.function(audio_unit_library, "_AudioUnitSetParameter", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        if (static_cast<std::uint32_t>(s.x[1]) != param_volume) return status(s, error::invalid_parameter);
        const auto value = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        std::lock_guard lock(u->mutex);
        u->volume = std::clamp(static_cast<double>(value), 0.0, 1.0);
        status(s, 0);
    });
    // AudioUnitRender(unit, flags*, timestamp*, bus, frames, AudioBufferList *ioData): bus 0 pulls the
    // render callback into the caller's buffers; bus 1 (input) has no device.
    support_.function(audio_unit_library, "_AudioUnitRender", [self, status](CpuState& s, GuestMemory& m) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        const auto bus = static_cast<std::uint32_t>(s.x[3]), frames = static_cast<std::uint32_t>(s.x[4]);
        if (bus == 1) return status(s, error::no_connection);
        if (bus != 0) return status(s, error::invalid_element);
        if (!s.x[5]) return status(s, error::param);
        std::uint64_t proc{}, refcon{};
        {
            std::lock_guard lock(u->mutex);
            if (!u->initialized) return status(s, error::uninitialized);
            if (frames > u->max_frames) return status(s, error::too_many_frames);
            proc = u->render_proc;
            refcon = u->render_refcon;
        }
        if (!proc) return status(s, error::no_connection);
        status(s, static_cast<std::int32_t>(self->call(proc, {refcon, s.x[1], s.x[2], 0, frames, s.x[5]})));
        static_cast<void>(m);
    });
    support_.function(audio_unit_library, "_AudioUnitReset", [self, status](CpuState& s, GuestMemory&) {
        const auto u = self->unit(s.x[0]);
        if (!u) return status(s, error::bad_component_instance);
        std::lock_guard lock(u->mutex);
        u->reset_pending = true;
        status(s, 0);
    });
}

} // namespace

void install_audio_units(Support& support) { std::make_shared<AudioUnits>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
