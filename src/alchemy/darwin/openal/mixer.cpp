#include "alchemy/darwin/openal/openal.hpp"

#include "alchemy/darwin/media/hal_io.hpp"
#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/registration_audit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

// OpenAL 1.1 (al.h, alc.h) as a software mixer on the default output device's I/O
// (media/hal_io.hpp), with AL_EXT_float32 and AL_EXT_MCFORMATS.
// - Devices and contexts: alcOpenDevice opens the default output ("Alchemy Default Output"); its
//   contexts mix while not suspended, each into the device's float output. A context has its own
//   listener, sources, distance model, Doppler and speed-of-sound values; buffers belong to the
//   device. Capture devices are not provided (no input device): alcCaptureOpenDevice answers NULL.
// - Mixing: each playing source steps through its buffers at (buffer rate / device rate) x pitch
//   with linear interpolation. Gain is the source gain x the listener gain x the distance model's
//   attenuation x the cone gain, clamped to the source's minimum and maximum. Mono sources are
//   placed by equal-power panning from their direction relative to the listener (on a device with
//   more than two channels, into the front two); multichannel buffers play their channels in order
//   (downmixed to stereo when the device has fewer channels). Doppler shift is not applied (the
//   Doppler values are kept).
// - Errors: the first AL error of a context sticks until alGetError; ALC errors per device and
//   for calls without a device.
namespace alchemy::openal {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;
using Address = std::uint64_t;
namespace hal_io = gui::completion_kit::media::hal_io;

// al.h / alc.h values.
constexpr std::int32_t al_none = 0, al_false = 0, al_true = 1;
enum : std::uint32_t {
    al_source_relative = 0x202, al_cone_inner_angle = 0x1001, al_cone_outer_angle = 0x1002, al_pitch = 0x1003, al_position = 0x1004,
    al_direction = 0x1005, al_velocity = 0x1006, al_looping = 0x1007, al_buffer = 0x1009, al_gain = 0x100a, al_min_gain = 0x100d,
    al_max_gain = 0x100e, al_orientation = 0x100f, al_source_state = 0x1010, al_initial = 0x1011, al_playing = 0x1012, al_paused = 0x1013,
    al_stopped = 0x1014, al_buffers_queued = 0x1015, al_buffers_processed = 0x1016, al_reference_distance = 0x1020,
    al_rolloff_factor = 0x1021, al_cone_outer_gain = 0x1022, al_max_distance = 0x1023, al_sec_offset = 0x1024, al_sample_offset = 0x1025,
    al_byte_offset = 0x1026, al_source_type = 0x1027, al_static = 0x1028, al_streaming = 0x1029, al_undetermined = 0x1030,
    al_frequency = 0x2001, al_bits = 0x2002, al_channels = 0x2003, al_size = 0x2004,
    al_no_error = 0, al_invalid_name = 0xa001, al_invalid_enum = 0xa002, al_invalid_value = 0xa003, al_invalid_operation = 0xa004,
    al_out_of_memory = 0xa005, al_vendor = 0xb001, al_version = 0xb002, al_renderer = 0xb003, al_extensions = 0xb004,
    al_doppler_factor = 0xc000, al_doppler_velocity = 0xc001, al_speed_of_sound = 0xc003, al_distance_model = 0xd000,
    al_inverse_distance = 0xd001, al_inverse_distance_clamped = 0xd002, al_linear_distance = 0xd003, al_linear_distance_clamped = 0xd004,
    al_exponent_distance = 0xd005, al_exponent_distance_clamped = 0xd006,
};
enum : std::uint32_t {
    alc_invalid_device = 0xa001, alc_invalid_context = 0xa002, alc_invalid_enum = 0xa003, alc_invalid_value = 0xa004, alc_out_of_memory = 0xa005,
    alc_major_version = 0x1000, alc_minor_version = 0x1001, alc_attributes_size = 0x1002, alc_all_attributes = 0x1003,
    alc_default_device_specifier = 0x1004, alc_device_specifier = 0x1005, alc_extensions = 0x1006, alc_frequency = 0x1007,
    alc_refresh = 0x1008, alc_sync = 0x1009, alc_mono_sources = 0x1010, alc_stereo_sources = 0x1011,
    alc_capture_device_specifier = 0x310, alc_capture_default_device_specifier = 0x311, alc_capture_samples = 0x312,
    alc_default_all_devices_specifier = 0x1012, alc_all_devices_specifier = 0x1013,
};

constexpr const char* device_name = "Alchemy Default Output";

// A buffer format: channels and sample kind.
struct Format { std::uint32_t channels{}; unsigned bytes{}; bool is_float{}; };
std::optional<Format> format_of(std::uint32_t format) {
    switch (format) {
    case 0x1100: return Format{1, 1, false};   // AL_FORMAT_MONO8
    case 0x1101: return Format{1, 2, false};   // AL_FORMAT_MONO16
    case 0x1102: return Format{2, 1, false};   // AL_FORMAT_STEREO8
    case 0x1103: return Format{2, 2, false};   // AL_FORMAT_STEREO16
    case 0x10010: return Format{1, 4, true};   // AL_FORMAT_MONO_FLOAT32
    case 0x10011: return Format{2, 4, true};   // AL_FORMAT_STEREO_FLOAT32
    case 0x1204: return Format{4, 1, false};   // AL_FORMAT_QUAD8
    case 0x1205: return Format{4, 2, false};   // AL_FORMAT_QUAD16
    case 0x1206: return Format{4, 4, true};    // AL_FORMAT_QUAD32
    case 0x1207: return Format{2, 1, false};   // AL_FORMAT_REAR8
    case 0x1208: return Format{2, 2, false};   // AL_FORMAT_REAR16
    case 0x1209: return Format{2, 4, true};    // AL_FORMAT_REAR32
    case 0x120A: return Format{6, 1, false};   // AL_FORMAT_51CHN8
    case 0x120B: return Format{6, 2, false};   // AL_FORMAT_51CHN16
    case 0x120C: return Format{6, 4, true};    // AL_FORMAT_51CHN32
    case 0x120D: return Format{7, 1, false};   // AL_FORMAT_61CHN8
    case 0x120E: return Format{7, 2, false};   // AL_FORMAT_61CHN16
    case 0x120F: return Format{7, 4, true};    // AL_FORMAT_61CHN32
    case 0x1210: return Format{8, 1, false};   // AL_FORMAT_71CHN8
    case 0x1211: return Format{8, 2, false};   // AL_FORMAT_71CHN16
    case 0x1212: return Format{8, 4, true};    // AL_FORMAT_71CHN32
    default: return std::nullopt;
    }
}

struct Buffer {
    std::uint32_t frequency{}, channels{1}, bits{16};
    std::vector<float> samples;        // interleaved
    std::uint64_t bytes{};             // the size it was given
    [[nodiscard]] std::uint64_t frames() const { return channels ? samples.size() / channels : 0; }
};

using Vec = std::array<float, 3>;

struct Source {
    float pitch{1}, gain{1}, min_gain{0}, max_gain{1}, reference{1}, rolloff{1}, max_distance{std::numeric_limits<float>::max()};
    float cone_inner{360}, cone_outer{360}, cone_outer_gain{0};
    Vec position{}, velocity{}, direction{};
    bool relative{}, looping{};
    std::uint32_t state{al_initial}, type{al_undetermined};
    std::deque<std::uint32_t> queue;   // buffer names, the current one first after `processed`
    std::size_t current{};             // index into queue of the playing buffer
    double cursor{};                   // frame position in the current buffer
};

struct Listener {
    float gain{1};
    Vec position{}, velocity{};
    Vec at{0, 0, -1}, up{0, 1, 0};
};

struct Device;

struct Context {
    Device* device{};
    Address handle{};
    Listener listener;
    std::map<std::uint32_t, Source> sources;
    std::uint32_t distance_model{al_inverse_distance_clamped};
    float doppler_factor{1}, doppler_velocity{1}, speed_of_sound{343.3f};
    bool suspended{};
    std::uint32_t error{al_no_error};
};

struct Device {
    Address handle{};
    std::map<std::uint32_t, Buffer> buffers;
    std::vector<std::shared_ptr<Context>> contexts;
    std::shared_ptr<hal_io::DeviceIo> io;
    bool playing{};
    std::uint32_t error{};
    std::uint32_t frequency{48000};
};

float length(const Vec& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
Vec minus(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec normalize(const Vec& v) {
    const auto l = length(v);
    return l > 0 ? Vec{v[0] / l, v[1] / l, v[2] / l} : Vec{};
}

// The distance model's attenuation (OpenAL 1.1, 3.4).
float attenuation(std::uint32_t model, const Source& s, float distance) {
    const bool clamped = model == al_inverse_distance_clamped || model == al_linear_distance_clamped || model == al_exponent_distance_clamped;
    if (clamped) distance = std::clamp(distance, s.reference, std::max(s.reference, s.max_distance));
    switch (model) {
    case al_inverse_distance: case al_inverse_distance_clamped: {
        const float d = s.reference + s.rolloff * (distance - s.reference);
        return d > 0 ? s.reference / d : 1.0f;
    }
    case al_linear_distance: case al_linear_distance_clamped: {
        const float span = s.max_distance - s.reference;
        return span > 0 ? std::max(0.0f, 1 - s.rolloff * (distance - s.reference) / span) : 1.0f;
    }
    case al_exponent_distance: case al_exponent_distance_clamped:
        return distance > 0 && s.reference > 0 ? std::pow(distance / s.reference, -s.rolloff) : 1.0f;
    default:
        return 1;
    }
}

class OpenAl : public std::enable_shared_from_this<OpenAl> {
public:
    explicit OpenAl(gui::InstallContext& context) : process_(context.process) {}
    void install();

private:
    template <class F>
    void fn(const char* name, F handler, const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(process_.install_function(name,
            [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); }, std::string(library)));
    }
    Address cstring(std::string_view text, bool list = false) {
        const auto size = text.size() + (list ? 2 : 1);
        const auto address = process_.allocate_guest(size, 1);
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        bytes.resize(size, 0);
        process_.main_engine().write_bytes(address, bytes);
        return address;
    }
    Address text(std::string_view value, bool list = false) {
        std::lock_guard lock(strings_mutex_);
        auto& slot = strings_[std::string(value) + (list ? "\x01" : "")];
        if (!slot) slot = cstring(value, list);
        return slot;
    }
    // The current context (under mutex_).
    Context* current() { return current_.get(); }
    void al_error(std::uint32_t code) {
        if (auto* c = current(); c && c->error == al_no_error) c->error = code;
    }
    Device* device(Address handle) {
        const auto found = devices_.find(handle);
        return found == devices_.end() ? nullptr : found->second.get();
    }
    std::shared_ptr<Context> context(Address handle) {
        for (auto& [h, d] : devices_)
            for (auto& c : d->contexts)
                if (c->handle == handle) return c;
        return nullptr;
    }
    void alc_error(Address device_handle, std::uint32_t code) {
        if (auto* d = device(device_handle)) d->error = code;
        else alc_error_ = code;
    }
    Source* source(std::uint32_t name) {
        auto* c = current();
        if (!c) return nullptr;
        const auto found = c->sources.find(name);
        return found == c->sources.end() ? nullptr : &found->second;
    }
    void render(Device& d, float* mix, std::uint32_t frames, std::uint32_t channels);
    void play(Source& s);
    void start_device(Device& d);

    ProcessContext& process_;
    std::mutex mutex_;                 // every device, context and source
    std::map<Address, std::unique_ptr<Device>> devices_;
    std::shared_ptr<Context> current_;
    std::atomic<std::uint32_t> alc_error_{};   // errors of calls without a device
    std::uint32_t next_name_{1};
    std::mutex strings_mutex_;
    std::map<std::string, Address> strings_;
};

void OpenAl::play(Source& s) {
    if (s.state == al_playing) {
        s.current = 0;
        s.cursor = 0;
    } else if (s.state != al_paused) {
        s.current = 0;
        s.cursor = 0;
    }
    s.state = s.queue.empty() ? al_stopped : al_playing;
}

// Called without mutex_ (the device's stop waits for a render, which takes it).
void OpenAl::start_device(Device& d) {
    std::shared_ptr<hal_io::DeviceIo> io;
    Address handle{};
    {
        std::lock_guard lock(mutex_);
        if (d.playing || !d.io) return;
        io = d.io;
        handle = d.handle;
    }
    const auto self = shared_from_this();
    Device* target = &d;
    const auto channels = io->channels();
    const bool started = io->start(handle, [self, target, channels](float* mix, std::uint32_t frames, const hal_io::Timing&) {
        self->render(*target, mix, frames, channels);
    }, gui::completion_kit::media::hal_buffer_frames(gui::completion_kit::media::hal_default_output())) == 0;
    std::lock_guard lock(mutex_);
    d.playing = d.playing || started;
}

void OpenAl::render(Device& d, float* mix, std::uint32_t frames, std::uint32_t channels) {
    std::lock_guard lock(mutex_);
    const double rate = d.io ? d.io->rate() : d.frequency;
    for (auto& c : d.contexts) {
        if (c->suspended) continue;
        const auto& l = c->listener;
        const Vec forward = normalize(l.at), right = normalize(cross(l.at, l.up));
        for (auto& [name, s] : c->sources) {
            if (s.state != al_playing || s.queue.empty()) continue;
            // Gain and pan for this block.
            float gain = s.gain * l.gain;
            float pan = 0;
            const auto& first = d.buffers[s.queue[std::min(s.current, s.queue.size() - 1)]];
            const bool spatial = first.channels == 1;
            if (spatial) {
                const Vec to = s.relative ? s.position : minus(s.position, l.position);
                const float distance = length(to);
                gain *= attenuation(c->distance_model, s, distance);
                if (distance > 0) {
                    const Vec dir = normalize(to);
                    pan = std::clamp(dot(dir, right), -1.0f, 1.0f);
                    static_cast<void>(forward);
                    // Cone: the angle between the source's direction and the listener.
                    if (length(s.direction) > 0 && (s.cone_inner < 360 || s.cone_outer < 360)) {
                        const float angle = std::acos(std::clamp(dot(normalize(s.direction), Vec{-dir[0], -dir[1], -dir[2]}), -1.0f, 1.0f)) *
                                            180.0f / std::numbers::pi_v<float> * 2;
                        if (angle > s.cone_outer) gain *= s.cone_outer_gain;
                        else if (angle > s.cone_inner && s.cone_outer > s.cone_inner)
                            gain *= 1 + (s.cone_outer_gain - 1) * (angle - s.cone_inner) / (s.cone_outer - s.cone_inner);
                    }
                }
            }
            gain = std::clamp(gain, s.min_gain, s.max_gain);
            const float left_gain = std::cos((pan + 1) * std::numbers::pi_v<float> / 4), right_gain = std::sin((pan + 1) * std::numbers::pi_v<float> / 4);
            for (std::uint32_t f = 0; f < frames && s.state == al_playing; ++f) {
                if (s.current >= s.queue.size()) {
                    if (s.looping && !s.queue.empty() && s.type == al_static) { s.current = 0; s.cursor = 0; }
                    else { s.state = al_stopped; break; }
                }
                const auto& b = d.buffers[s.queue[s.current]];
                const auto total = b.frames();
                if (!total || !b.frequency) { ++s.current; s.cursor = 0; continue; }
                const auto i0 = static_cast<std::uint64_t>(s.cursor);
                const double t = s.cursor - static_cast<double>(i0);
                const auto i1 = i0 + 1 < total ? i0 + 1 : (s.looping && s.queue.size() == 1 ? 0 : i0);
                float* out = mix + static_cast<std::size_t>(f) * channels;
                for (std::uint32_t ch = 0; ch < b.channels; ++ch) {
                    const float v = static_cast<float>(b.samples[i0 * b.channels + ch] * (1 - t) + b.samples[i1 * b.channels + ch] * t) * gain;
                    if (b.channels == 1) {
                        if (channels == 1) out[0] += v;
                        else { out[0] += v * left_gain; out[1] += v * right_gain; }
                    } else if (ch < channels && channels >= b.channels) {
                        out[ch] += v;
                    } else if (channels >= 2) {
                        // Fewer device channels: odd channels right, even left; the center and LFE (2, 3 of 5.1+) to both.
                        if (b.channels >= 6 && (ch == 2 || ch == 3)) { out[0] += v * 0.7071f; out[1] += v * 0.7071f; }
                        else out[ch % 2] += v * 0.7071f;
                    } else {
                        out[0] += v / b.channels;
                    }
                }
                s.cursor += static_cast<double>(b.frequency) / rate * std::clamp(s.pitch, 0.0001f, 8.0f);
                if (s.cursor >= static_cast<double>(total)) {
                    if (s.looping && s.queue.size() == 1) s.cursor = std::fmod(s.cursor, static_cast<double>(total));
                    else { s.cursor -= static_cast<double>(total); ++s.current; }
                }
            }
        }
    }
}

void OpenAl::install() {
    ProcessContext::add_provider_library(std::string(library));
    auto* self = this;
    const auto f32 = [](const CpuState& s, unsigned i) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[i][0])); };
    const auto ret_f32 = [](CpuState& s, float v) { s.v[0] = {std::bit_cast<std::uint32_t>(v), 0}; };

    // ---- devices ----
    fn("_alcOpenDevice", [self](CpuState& s, GuestMemory& m) {
        if (s.x[0]) {
            bool terminated{};
            const auto bytes = m.read_string(s.x[0], 1, 256, &terminated);
            const std::string name(bytes.begin(), bytes.end());
            if (name != device_name && name != "") { self->alc_error_ = alc_invalid_value; s.x[0] = 0; return; }
        }
        auto io = gui::completion_kit::media::hal_device_io(gui::completion_kit::media::hal_default_output());
        if (!io) { self->alc_error_ = alc_invalid_value; s.x[0] = 0; return; }
        auto d = std::make_unique<Device>();
        d->handle = self->process_.allocate_guest(16, 16);
        d->io = io;
        d->frequency = static_cast<std::uint32_t>(io->rate());
        const auto handle = d->handle;
        std::lock_guard lock(self->mutex_);
        self->devices_[handle] = std::move(d);
        s.x[0] = handle;
    });
    fn("_alcCloseDevice", [self](CpuState& s, GuestMemory&) {
        std::unique_ptr<Device> gone;
        {
            std::lock_guard lock(self->mutex_);
            auto* d = self->device(s.x[0]);
            if (!d) { self->alc_error_ = alc_invalid_device; s.x[0] = al_false; return; }
            if (!d->contexts.empty()) { d->error = alc_invalid_device; s.x[0] = al_false; return; }   // contexts left
            gone = std::move(self->devices_[s.x[0]]);
            self->devices_.erase(s.x[0]);
        }
        // Waits for a render in progress, which may still use the device.
        if (gone->playing && gone->io) gone->io->stop(gone->handle);
        s.x[0] = al_true;
    });
    fn("_alcGetError", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        if (auto* d = self->device(s.x[0])) s.x[0] = std::exchange(d->error, 0U);
        else if (s.x[0]) s.x[0] = alc_invalid_device;
        else s.x[0] = self->alc_error_.exchange(0U);
    });
    // ---- contexts ----
    fn("_alcCreateContext", [self](CpuState& s, GuestMemory&) {
        Device* d{};
        {
            std::lock_guard lock(self->mutex_);
            d = self->device(s.x[0]);
            if (!d) { self->alc_error_ = alc_invalid_device; s.x[0] = 0; return; }
            auto c = std::make_shared<Context>();
            c->device = d;
            c->handle = self->process_.allocate_guest(16, 16);
            d->contexts.push_back(c);
            s.x[0] = c->handle;
        }
        self->start_device(*d);
    });
    fn("_alcDestroyContext", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const auto c = self->context(s.x[0]);
        if (!c) { self->alc_error_ = alc_invalid_context; return; }
        if (self->current_ == c) self->current_.reset();
        std::erase(c->device->contexts, c);
    });
    fn("_alcMakeContextCurrent", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        if (!s.x[0]) { self->current_.reset(); s.x[0] = al_true; return; }
        const auto c = self->context(s.x[0]);
        if (!c) { self->alc_error_ = alc_invalid_context; s.x[0] = al_false; return; }
        self->current_ = c;
        s.x[0] = al_true;
    });
    fn("_alcGetCurrentContext", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        s.x[0] = self->current_ ? self->current_->handle : 0;
    });
    fn("_alcGetContextsDevice", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const auto c = self->context(s.x[0]);
        s.x[0] = c ? c->device->handle : 0;
    });
    for (const auto* name : {"_alcProcessContext", "_alcSuspendContext"}) {
        const bool suspend = std::string_view(name) == "_alcSuspendContext";
        fn(name, [self, suspend](CpuState& s, GuestMemory&) {
            std::lock_guard lock(self->mutex_);
            const auto c = self->context(s.x[0]);
            if (!c) { self->alc_error_ = alc_invalid_context; return; }
            c->suspended = suspend;
        });
    }
    fn("_alcIsExtensionPresent", [self](CpuState& s, GuestMemory& m) {
        bool terminated{};
        const auto bytes = s.x[1] ? m.read_string(s.x[1], 1, 256, &terminated) : std::vector<std::uint8_t>{};
        std::string name(bytes.begin(), bytes.end());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (!s.x[1]) { std::lock_guard lock(self->mutex_); self->alc_error(s.x[0], alc_invalid_value); }
        s.x[0] = name == "ALC_ENUMERATION_EXT" || name == "ALC_ENUMERATE_ALL_EXT" ? al_true : al_false;
    });
    fn("_alcGetProcAddress", [self](CpuState& s, GuestMemory& m) {
        bool terminated{};
        const auto bytes = s.x[1] ? m.read_string(s.x[1], 1, 256, &terminated) : std::vector<std::uint8_t>{};
        const std::string name(bytes.begin(), bytes.end());
        const auto found = self->process_.resolve(library, "_" + name);
        s.x[0] = found ? found->address : 0;
    });
    fn("_alcGetEnumValue", [](CpuState& s, GuestMemory& m) {
        bool terminated{};
        const auto bytes = s.x[1] ? m.read_string(s.x[1], 1, 256, &terminated) : std::vector<std::uint8_t>{};
        s.x[0] = enum_value(std::string(bytes.begin(), bytes.end()));
    });
    fn("_alcGetString", [self](CpuState& s, GuestMemory&) {
        const auto parameter = static_cast<std::uint32_t>(s.x[1]);
        switch (parameter) {
        case 0: s.x[0] = self->text("No Error"); return;
        case alc_invalid_device: s.x[0] = self->text("Invalid Device"); return;
        case alc_invalid_context: s.x[0] = self->text("Invalid Context"); return;
        case alc_invalid_enum: s.x[0] = self->text("Invalid Enum"); return;
        case alc_invalid_value: s.x[0] = self->text("Invalid Value"); return;
        case alc_out_of_memory: s.x[0] = self->text("Out of Memory"); return;
        case alc_default_device_specifier: case alc_default_all_devices_specifier: s.x[0] = self->text(device_name); return;
        case alc_device_specifier: case alc_all_devices_specifier:
            s.x[0] = s.x[0] ? self->text(device_name) : self->text(device_name, true);
            return;
        case alc_capture_device_specifier: s.x[0] = self->text("", true); return;
        case alc_capture_default_device_specifier: s.x[0] = self->text(""); return;
        case alc_extensions: s.x[0] = self->text("ALC_ENUMERATION_EXT ALC_ENUMERATE_ALL_EXT"); return;
        default: {
            std::lock_guard lock(self->mutex_);
            self->alc_error(s.x[0], alc_invalid_enum);
            s.x[0] = 0;
        }
        }
    });
    // alcGetIntegerv(device, param, size, ALCint *values)
    fn("_alcGetIntegerv", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        const auto parameter = static_cast<std::uint32_t>(s.x[1]);
        const auto size = static_cast<std::int32_t>(s.x[2]);
        if (!s.x[3] || size <= 0) { self->alc_error(s.x[0], alc_invalid_value); return; }
        auto* d = self->device(s.x[0]);
        const std::int32_t frequency = d ? static_cast<std::int32_t>(d->frequency) : 0;
        const std::vector<std::int32_t> attributes{static_cast<std::int32_t>(alc_frequency), frequency, static_cast<std::int32_t>(alc_refresh), 50,
                                                   static_cast<std::int32_t>(alc_sync), 0, static_cast<std::int32_t>(alc_mono_sources), 255,
                                                   static_cast<std::int32_t>(alc_stereo_sources), 255, 0};
        std::vector<std::int32_t> out;
        switch (parameter) {
        case alc_major_version: out = {1}; break;
        case alc_minor_version: out = {1}; break;
        case alc_frequency: case alc_refresh: case alc_sync: case alc_mono_sources: case alc_stereo_sources: case alc_attributes_size:
        case alc_all_attributes:
            if (!d) { self->alc_error(s.x[0], alc_invalid_device); return; }
            if (parameter == alc_attributes_size) out = {static_cast<std::int32_t>(attributes.size())};
            else if (parameter == alc_all_attributes) out = attributes;
            else for (std::size_t i = 0; i + 1 < attributes.size(); i += 2)
                if (static_cast<std::uint32_t>(attributes[i]) == parameter) out = {attributes[i + 1]};
            break;
        case alc_capture_samples: out = {0}; break;
        default: self->alc_error(s.x[0], alc_invalid_enum); return;
        }
        if (static_cast<std::size_t>(size) < out.size()) { self->alc_error(s.x[0], alc_invalid_value); return; }
        for (std::size_t i = 0; i < out.size(); ++i) m.write32(s.x[3] + 4 * i, static_cast<std::uint32_t>(out[i]));
    });
    // Capture: no input device.
    fn("_alcCaptureOpenDevice", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        self->alc_error_ = alc_invalid_value;
        s.x[0] = 0;
    });
    fn("_alcCaptureCloseDevice", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        self->alc_error_ = alc_invalid_device;
        s.x[0] = al_false;
    });
    for (const auto* name : {"_alcCaptureStart", "_alcCaptureStop", "_alcCaptureSamples"})
        fn(name, [self](CpuState&, GuestMemory&) {
            std::lock_guard lock(self->mutex_);
            self->alc_error_ = alc_invalid_device;
        });

    // ---- state ----
    fn("_alGetError", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        s.x[0] = c ? std::exchange(c->error, static_cast<std::uint32_t>(al_no_error)) : al_invalid_operation;
    });
    fn("_alIsExtensionPresent", [](CpuState& s, GuestMemory& m) {
        bool terminated{};
        const auto bytes = s.x[0] ? m.read_string(s.x[0], 1, 256, &terminated) : std::vector<std::uint8_t>{};
        std::string name(bytes.begin(), bytes.end());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        s.x[0] = name == "AL_EXT_FLOAT32" || name == "AL_EXT_MCFORMATS" || name == "AL_EXT_OFFSET" ? al_true : al_false;
    });
    fn("_alGetProcAddress", [self](CpuState& s, GuestMemory& m) {
        bool terminated{};
        const auto bytes = s.x[0] ? m.read_string(s.x[0], 1, 256, &terminated) : std::vector<std::uint8_t>{};
        const auto found = self->process_.resolve(library, "_" + std::string(bytes.begin(), bytes.end()));
        s.x[0] = found ? found->address : 0;
    });
    fn("_alGetEnumValue", [](CpuState& s, GuestMemory& m) {
        bool terminated{};
        const auto bytes = s.x[0] ? m.read_string(s.x[0], 1, 256, &terminated) : std::vector<std::uint8_t>{};
        s.x[0] = enum_value(std::string(bytes.begin(), bytes.end()));
    });
    fn("_alGetString", [self](CpuState& s, GuestMemory&) {
        switch (static_cast<std::uint32_t>(s.x[0])) {
        case al_vendor: s.x[0] = self->text("Alchemy"); return;
        case al_version: s.x[0] = self->text("1.1 Alchemy"); return;
        case al_renderer: s.x[0] = self->text("Alchemy Software Mixer"); return;
        case al_extensions: s.x[0] = self->text("AL_EXT_FLOAT32 AL_EXT_MCFORMATS AL_EXT_OFFSET"); return;
        case al_no_error: s.x[0] = self->text("No Error"); return;
        case al_invalid_name: s.x[0] = self->text("Invalid Name"); return;
        case al_invalid_enum: s.x[0] = self->text("Invalid Enum"); return;
        case al_invalid_value: s.x[0] = self->text("Invalid Value"); return;
        case al_invalid_operation: s.x[0] = self->text("Invalid Operation"); return;
        case al_out_of_memory: s.x[0] = self->text("Out of Memory"); return;
        default: {
            std::lock_guard lock(self->mutex_);
            self->al_error(al_invalid_enum);
            s.x[0] = 0;
        }
        }
    });
    // Global state (alGet*: AL_DOPPLER_FACTOR, AL_DOPPLER_VELOCITY, AL_SPEED_OF_SOUND, AL_DISTANCE_MODEL).
    const auto global = [self](std::uint32_t parameter, double& value) {
        auto* c = self->current();
        if (!c) return false;
        switch (parameter) {
        case al_doppler_factor: value = c->doppler_factor; return true;
        case al_doppler_velocity: value = c->doppler_velocity; return true;
        case al_speed_of_sound: value = c->speed_of_sound; return true;
        case al_distance_model: value = c->distance_model; return true;
        default: self->al_error(al_invalid_enum); return false;
        }
    };
    fn("_alGetInteger", [self, global](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        double value{};
        s.x[0] = global(static_cast<std::uint32_t>(s.x[0]), value) ? static_cast<std::uint32_t>(static_cast<std::int32_t>(value)) : 0;
    });
    fn("_alGetBoolean", [self, global](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        double value{};
        s.x[0] = global(static_cast<std::uint32_t>(s.x[0]), value) && value != 0 ? al_true : al_false;
    });
    fn("_alGetFloat", [self, global, ret_f32](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        double value{};
        ret_f32(s, global(static_cast<std::uint32_t>(s.x[0]), value) ? static_cast<float>(value) : 0.0f);
    });
    fn("_alGetDouble", [self, global](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        double value{};
        s.v[0] = {std::bit_cast<std::uint64_t>(global(static_cast<std::uint32_t>(s.x[0]), value) ? value : 0.0), 0};
    });
    for (const auto* name : {"_alGetIntegerv", "_alGetFloatv", "_alGetDoublev", "_alGetBooleanv"}) {
        const std::string kind = name;
        fn(name, [self, global, kind](CpuState& s, GuestMemory& m) {
            std::lock_guard lock(self->mutex_);
            double value{};
            if (!s.x[1]) { self->al_error(al_invalid_value); return; }
            if (!global(static_cast<std::uint32_t>(s.x[0]), value)) return;
            if (kind == "_alGetIntegerv") m.write32(s.x[1], static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
            else if (kind == "_alGetFloatv") m.write32(s.x[1], std::bit_cast<std::uint32_t>(static_cast<float>(value)));
            else if (kind == "_alGetDoublev") m.write64(s.x[1], std::bit_cast<std::uint64_t>(value));
            else m.write8(s.x[1], value != 0 ? 1 : 0);
        });
    }
    fn("_alDistanceModel", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        const auto model = static_cast<std::uint32_t>(s.x[0]);
        if (!c) return;
        if (model != al_none && (model < al_inverse_distance || model > al_exponent_distance_clamped)) { self->al_error(al_invalid_value); return; }
        c->distance_model = model;
    });
    for (const auto* name : {"_alDopplerFactor", "_alDopplerVelocity", "_alSpeedOfSound"}) {
        const std::string which = name;
        fn(name, [self, which, f32](CpuState& s, GuestMemory&) {
            std::lock_guard lock(self->mutex_);
            auto* c = self->current();
            const float value = f32(s, 0);
            if (!c) return;
            if (value < 0 || (which == "_alSpeedOfSound" && value <= 0)) { self->al_error(al_invalid_value); return; }
            (which == "_alDopplerFactor" ? c->doppler_factor : which == "_alDopplerVelocity" ? c->doppler_velocity : c->speed_of_sound) = value;
        });
    }
    // alEnable / alDisable / alIsEnabled: no capabilities exist in OpenAL 1.1.
    for (const auto* name : {"_alEnable", "_alDisable"})
        fn(name, [self](CpuState&, GuestMemory&) { std::lock_guard lock(self->mutex_); self->al_error(al_invalid_enum); });
    fn("_alIsEnabled", [self](CpuState& s, GuestMemory&) { std::lock_guard lock(self->mutex_); self->al_error(al_invalid_enum); s.x[0] = al_false; });

    // ---- listener ----
    const auto listener_set = [self](std::uint32_t parameter, const float* v, unsigned count) {
        auto* c = self->current();
        if (!c) return;
        auto& l = c->listener;
        switch (parameter) {
        case al_gain: if (count < 1 || v[0] < 0) { self->al_error(al_invalid_value); return; } l.gain = v[0]; return;
        case al_position: if (count < 3) break; l.position = {v[0], v[1], v[2]}; return;
        case al_velocity: if (count < 3) break; l.velocity = {v[0], v[1], v[2]}; return;
        case al_orientation: if (count < 6) break; l.at = {v[0], v[1], v[2]}; l.up = {v[3], v[4], v[5]}; return;
        default: break;
        }
        self->al_error(al_invalid_enum);
    };
    const auto listener_get = [self](std::uint32_t parameter, float* v) -> unsigned {
        auto* c = self->current();
        if (!c) return 0;
        const auto& l = c->listener;
        switch (parameter) {
        case al_gain: v[0] = l.gain; return 1;
        case al_position: std::copy(l.position.begin(), l.position.end(), v); return 3;
        case al_velocity: std::copy(l.velocity.begin(), l.velocity.end(), v); return 3;
        case al_orientation: std::copy(l.at.begin(), l.at.end(), v); std::copy(l.up.begin(), l.up.end(), v + 3); return 6;
        default: self->al_error(al_invalid_enum); return 0;
        }
    };
    fn("_alListenerf", [self, listener_set, f32](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const float v[1]{f32(s, 0)};
        listener_set(static_cast<std::uint32_t>(s.x[0]), v, 1);
    });
    fn("_alListener3f", [self, listener_set, f32](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const float v[3]{f32(s, 0), f32(s, 1), f32(s, 2)};
        listener_set(static_cast<std::uint32_t>(s.x[0]), v, 3);
    });
    fn("_alListeneri", [self, listener_set](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const float v[1]{static_cast<float>(static_cast<std::int32_t>(s.x[1]))};
        listener_set(static_cast<std::uint32_t>(s.x[0]), v, 1);
    });
    fn("_alListener3i", [self, listener_set](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const float v[3]{static_cast<float>(static_cast<std::int32_t>(s.x[1])), static_cast<float>(static_cast<std::int32_t>(s.x[2])),
                         static_cast<float>(static_cast<std::int32_t>(s.x[3]))};
        listener_set(static_cast<std::uint32_t>(s.x[0]), v, 3);
    });
    for (const auto* name : {"_alListenerfv", "_alListeneriv"}) {
        const bool integers = std::string_view(name) == "_alListeneriv";
        fn(name, [self, listener_set, integers](CpuState& s, GuestMemory& m) {
            std::lock_guard lock(self->mutex_);
            if (!s.x[1]) { self->al_error(al_invalid_value); return; }
            const auto parameter = static_cast<std::uint32_t>(s.x[0]);
            const unsigned count = parameter == al_orientation ? 6 : parameter == al_gain ? 1 : 3;
            float v[6]{};
            for (unsigned i = 0; i < count; ++i) {
                const auto bits = m.read32(s.x[1] + 4ULL * i);
                v[i] = integers ? static_cast<float>(static_cast<std::int32_t>(bits)) : std::bit_cast<float>(bits);
            }
            listener_set(parameter, v, count);
        });
    }
    fn("_alGetListenerf", [self, listener_get](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        float v[6]{};
        if (s.x[1] && listener_get(static_cast<std::uint32_t>(s.x[0]), v) >= 1) m.write32(s.x[1], std::bit_cast<std::uint32_t>(v[0]));
    });
    fn("_alGetListener3f", [self, listener_get](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        float v[6]{};
        if (listener_get(static_cast<std::uint32_t>(s.x[0]), v) >= 3)
            for (unsigned i = 0; i < 3; ++i)
                if (s.x[1 + i]) m.write32(s.x[1 + i], std::bit_cast<std::uint32_t>(v[i]));
    });
    fn("_alGetListenerfv", [self, listener_get](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        float v[6]{};
        const auto n = listener_get(static_cast<std::uint32_t>(s.x[0]), v);
        if (s.x[1]) for (unsigned i = 0; i < n; ++i) m.write32(s.x[1] + 4ULL * i, std::bit_cast<std::uint32_t>(v[i]));
    });
    fn("_alGetListeneri", [self, listener_get](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        float v[6]{};
        if (s.x[1] && listener_get(static_cast<std::uint32_t>(s.x[0]), v) >= 1) m.write32(s.x[1], static_cast<std::uint32_t>(static_cast<std::int32_t>(v[0])));
    });
    fn("_alGetListener3i", [self, listener_get](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        float v[6]{};
        if (listener_get(static_cast<std::uint32_t>(s.x[0]), v) >= 3)
            for (unsigned i = 0; i < 3; ++i)
                if (s.x[1 + i]) m.write32(s.x[1 + i], static_cast<std::uint32_t>(static_cast<std::int32_t>(v[i])));
    });
    fn("_alGetListeneriv", [self, listener_get](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        float v[6]{};
        const auto n = listener_get(static_cast<std::uint32_t>(s.x[0]), v);
        if (s.x[1]) for (unsigned i = 0; i < n; ++i) m.write32(s.x[1] + 4ULL * i, static_cast<std::uint32_t>(static_cast<std::int32_t>(v[i])));
    });

    // ---- sources ----
    fn("_alGenSources", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        const auto n = static_cast<std::int32_t>(s.x[0]);
        if (!c) return;
        if (n < 0 || (n && !s.x[1])) { self->al_error(al_invalid_value); return; }
        for (std::int32_t i = 0; i < n; ++i) {
            const auto name = self->next_name_++;
            c->sources[name] = Source{};
            m.write32(s.x[1] + 4ULL * i, name);
        }
    });
    fn("_alDeleteSources", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        const auto n = static_cast<std::int32_t>(s.x[0]);
        if (!c) return;
        if (n < 0 || (n && !s.x[1])) { self->al_error(al_invalid_value); return; }
        for (std::int32_t i = 0; i < n; ++i)
            if (!c->sources.contains(m.read32(s.x[1] + 4ULL * i))) { self->al_error(al_invalid_name); return; }
        for (std::int32_t i = 0; i < n; ++i) c->sources.erase(m.read32(s.x[1] + 4ULL * i));
    });
    fn("_alIsSource", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        s.x[0] = self->source(static_cast<std::uint32_t>(s.x[0])) ? al_true : al_false;
    });
    // Sets a source property from `count` values.
    const auto source_set = [self](std::uint32_t name, std::uint32_t parameter, const double* v, unsigned count) {
        auto* c = self->current();
        if (!c) return;
        auto* src = self->source(name);
        if (!src) { self->al_error(al_invalid_name); return; }
        auto& s = *src;
        const auto positive = [&](float& field) {
            if (v[0] < 0) self->al_error(al_invalid_value);
            else field = static_cast<float>(v[0]);
        };
        switch (parameter) {
        case al_pitch: if (v[0] <= 0) self->al_error(al_invalid_value); else s.pitch = static_cast<float>(v[0]); return;
        case al_gain: positive(s.gain); return;
        case al_min_gain: positive(s.min_gain); return;
        case al_max_gain: positive(s.max_gain); return;
        case al_reference_distance: positive(s.reference); return;
        case al_rolloff_factor: positive(s.rolloff); return;
        case al_max_distance: positive(s.max_distance); return;
        case al_cone_inner_angle: s.cone_inner = static_cast<float>(std::clamp(v[0], 0.0, 360.0)); return;
        case al_cone_outer_angle: s.cone_outer = static_cast<float>(std::clamp(v[0], 0.0, 360.0)); return;
        case al_cone_outer_gain: s.cone_outer_gain = static_cast<float>(std::clamp(v[0], 0.0, 1.0)); return;
        case al_position: if (count >= 3) { s.position = {float(v[0]), float(v[1]), float(v[2])}; return; } break;
        case al_velocity: if (count >= 3) { s.velocity = {float(v[0]), float(v[1]), float(v[2])}; return; } break;
        case al_direction: if (count >= 3) { s.direction = {float(v[0]), float(v[1]), float(v[2])}; return; } break;
        case al_source_relative: s.relative = v[0] != 0; return;
        case al_looping: s.looping = v[0] != 0; return;
        case al_buffer: {
            if (s.state == al_playing || s.state == al_paused) { self->al_error(al_invalid_operation); return; }
            const auto buffer = static_cast<std::uint32_t>(v[0]);
            if (buffer && !c->device->buffers.contains(buffer)) { self->al_error(al_invalid_value); return; }
            s.queue.clear();
            s.current = 0;
            s.cursor = 0;
            if (buffer) { s.queue.push_back(buffer); s.type = al_static; }
            else s.type = al_undetermined;
            return;
        }
        case al_sec_offset: case al_sample_offset: case al_byte_offset: {
            if (s.queue.empty()) { self->al_error(al_invalid_value); return; }
            const auto& b = c->device->buffers[s.queue[std::min(s.current, s.queue.size() - 1)]];
            double frames = v[0];
            if (parameter == al_sec_offset) frames = v[0] * b.frequency;
            else if (parameter == al_byte_offset) frames = v[0] / std::max(1U, b.channels * (b.bits / 8));
            if (frames < 0 || frames >= static_cast<double>(b.frames())) { self->al_error(al_invalid_value); return; }
            s.cursor = frames;
            return;
        }
        default: break;
        }
        self->al_error(al_invalid_enum);
    };
    // Reads a source property into `v` (count returned).
    const auto source_get = [self](std::uint32_t name, std::uint32_t parameter, double* v) -> unsigned {
        auto* c = self->current();
        if (!c) return 0;
        auto* src = self->source(name);
        if (!src) { self->al_error(al_invalid_name); return 0; }
        const auto& s = *src;
        const auto three = [&](const Vec& value) { v[0] = value[0]; v[1] = value[1]; v[2] = value[2]; return 3U; };
        switch (parameter) {
        case al_pitch: v[0] = s.pitch; return 1;
        case al_gain: v[0] = s.gain; return 1;
        case al_min_gain: v[0] = s.min_gain; return 1;
        case al_max_gain: v[0] = s.max_gain; return 1;
        case al_reference_distance: v[0] = s.reference; return 1;
        case al_rolloff_factor: v[0] = s.rolloff; return 1;
        case al_max_distance: v[0] = s.max_distance; return 1;
        case al_cone_inner_angle: v[0] = s.cone_inner; return 1;
        case al_cone_outer_angle: v[0] = s.cone_outer; return 1;
        case al_cone_outer_gain: v[0] = s.cone_outer_gain; return 1;
        case al_position: return three(s.position);
        case al_velocity: return three(s.velocity);
        case al_direction: return three(s.direction);
        case al_source_relative: v[0] = s.relative; return 1;
        case al_looping: v[0] = s.looping; return 1;
        case al_buffer: v[0] = s.queue.empty() ? 0 : s.queue[std::min(s.current, s.queue.size() - 1)]; return 1;
        case al_source_state: v[0] = s.state; return 1;
        case al_source_type: v[0] = s.type; return 1;
        case al_buffers_queued: v[0] = static_cast<double>(s.queue.size()); return 1;
        case al_buffers_processed:
            v[0] = s.type == al_static ? 0 : static_cast<double>(s.state == al_stopped ? s.queue.size() : std::min(s.current, s.queue.size()));
            return 1;
        case al_sec_offset: case al_sample_offset: case al_byte_offset: {
            if (s.queue.empty() || s.state == al_initial || s.state == al_stopped) { v[0] = 0; return 1; }
            const auto& b = c->device->buffers[s.queue[std::min(s.current, s.queue.size() - 1)]];
            const double frames = std::floor(s.cursor);
            v[0] = parameter == al_sec_offset ? (b.frequency ? s.cursor / b.frequency : 0)
                 : parameter == al_sample_offset ? frames : frames * b.channels * (b.bits / 8);
            return 1;
        }
        default: self->al_error(al_invalid_enum); return 0;
        }
    };
    fn("_alSourcef", [self, source_set, f32](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const double v[1]{f32(s, 0)};
        source_set(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), v, 1);
    });
    fn("_alSource3f", [self, source_set, f32](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const double v[3]{f32(s, 0), f32(s, 1), f32(s, 2)};
        source_set(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), v, 3);
    });
    fn("_alSourcei", [self, source_set](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const double v[1]{static_cast<double>(static_cast<std::int32_t>(s.x[2]))};
        // AL_BUFFER takes an unsigned name.
        const double value = static_cast<std::uint32_t>(s.x[1]) == al_buffer ? static_cast<double>(static_cast<std::uint32_t>(s.x[2])) : v[0];
        source_set(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), &value, 1);
    });
    fn("_alSource3i", [self, source_set](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        const double v[3]{static_cast<double>(static_cast<std::int32_t>(s.x[2])), static_cast<double>(static_cast<std::int32_t>(s.x[3])),
                          static_cast<double>(static_cast<std::int32_t>(s.x[4]))};
        source_set(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), v, 3);
    });
    for (const auto* name : {"_alSourcefv", "_alSourceiv"}) {
        const bool integers = std::string_view(name) == "_alSourceiv";
        fn(name, [self, source_set, integers](CpuState& s, GuestMemory& m) {
            std::lock_guard lock(self->mutex_);
            if (!s.x[2]) { self->al_error(al_invalid_value); return; }
            const auto parameter = static_cast<std::uint32_t>(s.x[1]);
            const unsigned count = parameter == al_position || parameter == al_velocity || parameter == al_direction ? 3 : 1;
            double v[3]{};
            for (unsigned i = 0; i < count; ++i) {
                const auto bits = m.read32(s.x[2] + 4ULL * i);
                v[i] = integers ? (parameter == al_buffer ? static_cast<double>(bits) : static_cast<double>(static_cast<std::int32_t>(bits)))
                                : static_cast<double>(std::bit_cast<float>(bits));
            }
            source_set(static_cast<std::uint32_t>(s.x[0]), parameter, v, count);
        });
    }
    const auto get_into = [self, source_get](CpuState& s, GuestMemory& m, bool integers, unsigned wanted) {
        double v[3]{};
        const auto n = source_get(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), v);
        if (n == 0) return;
        if (wanted == 3 && n != 3) { self->al_error(al_invalid_enum); return; }
        const auto write = [&](Address at, double value) {
            if (!at) return;
            m.write32(at, integers ? static_cast<std::uint32_t>(static_cast<std::int64_t>(value)) : std::bit_cast<std::uint32_t>(static_cast<float>(value)));
        };
        if (wanted == 3) for (unsigned i = 0; i < 3; ++i) write(s.x[2 + i], v[i]);
        else if (wanted == 0) for (unsigned i = 0; i < n; ++i) write(s.x[2] ? s.x[2] + 4ULL * i : 0, v[i]);
        else write(s.x[2], v[0]);
    };
    fn("_alGetSourcef", [self, get_into](CpuState& s, GuestMemory& m) { std::lock_guard lock(self->mutex_); get_into(s, m, false, 1); });
    fn("_alGetSource3f", [self, get_into](CpuState& s, GuestMemory& m) { std::lock_guard lock(self->mutex_); get_into(s, m, false, 3); });
    fn("_alGetSourcefv", [self, get_into](CpuState& s, GuestMemory& m) { std::lock_guard lock(self->mutex_); get_into(s, m, false, 0); });
    fn("_alGetSourcei", [self, get_into](CpuState& s, GuestMemory& m) { std::lock_guard lock(self->mutex_); get_into(s, m, true, 1); });
    fn("_alGetSource3i", [self, get_into](CpuState& s, GuestMemory& m) { std::lock_guard lock(self->mutex_); get_into(s, m, true, 3); });
    fn("_alGetSourceiv", [self, get_into](CpuState& s, GuestMemory& m) { std::lock_guard lock(self->mutex_); get_into(s, m, true, 0); });
    // Playback state changes, single and vector forms.
    enum class Action { play, stop, rewind, pause };
    const auto act = [self](std::uint32_t name, Action action) {
        auto* src = self->source(name);
        if (!src) { self->al_error(al_invalid_name); return; }
        auto& s = *src;
        switch (action) {
        case Action::play: self->play(s); break;
        case Action::stop: if (s.state != al_initial) { s.state = al_stopped; s.current = s.queue.size(); } break;
        case Action::rewind: s.state = al_initial; s.current = 0; s.cursor = 0; break;
        case Action::pause: if (s.state == al_playing) s.state = al_paused; break;
        }
    };
    for (const auto& [name, action] : std::initializer_list<std::pair<const char*, Action>>{
             {"_alSourcePlay", Action::play}, {"_alSourceStop", Action::stop}, {"_alSourceRewind", Action::rewind}, {"_alSourcePause", Action::pause}}) {
        const auto a = action;
        fn(name, [self, act, a](CpuState& s, GuestMemory&) {
            std::lock_guard lock(self->mutex_);
            if (self->current()) act(static_cast<std::uint32_t>(s.x[0]), a);
        });
    }
    for (const auto& [name, action] : std::initializer_list<std::pair<const char*, Action>>{
             {"_alSourcePlayv", Action::play}, {"_alSourceStopv", Action::stop}, {"_alSourceRewindv", Action::rewind}, {"_alSourcePausev", Action::pause}}) {
        const auto a = action;
        fn(name, [self, act, a](CpuState& s, GuestMemory& m) {
            std::lock_guard lock(self->mutex_);
            if (!self->current()) return;
            const auto n = static_cast<std::int32_t>(s.x[0]);
            if (n < 0 || (n && !s.x[1])) { self->al_error(al_invalid_value); return; }
            for (std::int32_t i = 0; i < n; ++i) act(m.read32(s.x[1] + 4ULL * i), a);
        });
    }
    // alSourceQueueBuffers(source, n, const ALuint *buffers)
    fn("_alSourceQueueBuffers", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        if (!c) return;
        auto* src = self->source(static_cast<std::uint32_t>(s.x[0]));
        if (!src) { self->al_error(al_invalid_name); return; }
        const auto n = static_cast<std::int32_t>(s.x[1]);
        if (n < 0 || (n && !s.x[2])) { self->al_error(al_invalid_value); return; }
        if (src->type == al_static) { self->al_error(al_invalid_operation); return; }
        std::vector<std::uint32_t> names;
        for (std::int32_t i = 0; i < n; ++i) {
            const auto buffer = m.read32(s.x[2] + 4ULL * i);
            if (buffer && !c->device->buffers.contains(buffer)) { self->al_error(al_invalid_name); return; }
            names.push_back(buffer);
        }
        for (const auto b : names) src->queue.push_back(b);
        if (n) src->type = al_streaming;
        // A stopped streaming source that ran out keeps its played buffers as processed.
    });
    // alSourceUnqueueBuffers(source, n, ALuint *buffers): processed buffers only.
    fn("_alSourceUnqueueBuffers", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        if (!self->current()) return;
        auto* src = self->source(static_cast<std::uint32_t>(s.x[0]));
        if (!src) { self->al_error(al_invalid_name); return; }
        const auto n = static_cast<std::int32_t>(s.x[1]);
        if (n < 0 || (n && !s.x[2])) { self->al_error(al_invalid_value); return; }
        const auto processed = src->state == al_stopped ? src->queue.size() : std::min(src->current, src->queue.size());
        if (static_cast<std::size_t>(n) > processed) { self->al_error(al_invalid_value); return; }
        for (std::int32_t i = 0; i < n; ++i) {
            m.write32(s.x[2] + 4ULL * i, src->queue.front());
            src->queue.pop_front();
            if (src->current) --src->current;
        }
        if (src->queue.empty()) src->type = al_undetermined;
    });

    // ---- buffers ----
    fn("_alGenBuffers", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        const auto n = static_cast<std::int32_t>(s.x[0]);
        if (!c) return;
        if (n < 0 || (n && !s.x[1])) { self->al_error(al_invalid_value); return; }
        for (std::int32_t i = 0; i < n; ++i) {
            const auto name = self->next_name_++;
            c->device->buffers[name] = Buffer{};
            m.write32(s.x[1] + 4ULL * i, name);
        }
    });
    fn("_alDeleteBuffers", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        const auto n = static_cast<std::int32_t>(s.x[0]);
        if (!c) return;
        if (n < 0 || (n && !s.x[1])) { self->al_error(al_invalid_value); return; }
        for (std::int32_t i = 0; i < n; ++i) {
            const auto name = m.read32(s.x[1] + 4ULL * i);
            if (name && !c->device->buffers.contains(name)) { self->al_error(al_invalid_name); return; }
            for (const auto& other : c->device->contexts)
                for (const auto& [id, src] : other->sources)
                    if (std::find(src.queue.begin(), src.queue.end(), name) != src.queue.end()) { self->al_error(al_invalid_operation); return; }
        }
        for (std::int32_t i = 0; i < n; ++i) c->device->buffers.erase(m.read32(s.x[1] + 4ULL * i));
    });
    fn("_alIsBuffer", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        const auto name = static_cast<std::uint32_t>(s.x[0]);
        s.x[0] = c && (name == 0 || c->device->buffers.contains(name)) ? al_true : al_false;
    });
    // alBufferData(buffer, format, const ALvoid *data, size, frequency)
    fn("_alBufferData", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        auto* c = self->current();
        if (!c) return;
        const auto found = c->device->buffers.find(static_cast<std::uint32_t>(s.x[0]));
        if (found == c->device->buffers.end()) { self->al_error(al_invalid_name); return; }
        const auto format = format_of(static_cast<std::uint32_t>(s.x[1]));
        const auto size = static_cast<std::int32_t>(s.x[3]), frequency = static_cast<std::int32_t>(s.x[4]);
        if (!format) { self->al_error(al_invalid_enum); return; }
        const auto frame = format->channels * format->bytes;
        if (size < 0 || frequency <= 0 || static_cast<std::uint32_t>(size) % frame || (size && !s.x[2])) { self->al_error(al_invalid_value); return; }
        for (const auto& other : c->device->contexts)
            for (const auto& [id, src] : other->sources)
                if ((src.state == al_playing || src.state == al_paused) &&
                    std::find(src.queue.begin(), src.queue.end(), found->first) != src.queue.end()) { self->al_error(al_invalid_operation); return; }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (size) m.read_bytes(s.x[2], bytes);
        auto& b = found->second;
        b.frequency = static_cast<std::uint32_t>(frequency);
        b.channels = format->channels;
        b.bits = format->bytes * 8;
        b.bytes = static_cast<std::uint64_t>(size);
        b.samples.resize(bytes.size() / format->bytes);
        for (std::size_t i = 0; i < b.samples.size(); ++i) {
            const auto* p = bytes.data() + i * format->bytes;
            if (format->is_float) b.samples[i] = std::bit_cast<float>(static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24)));
            else if (format->bytes == 2) b.samples[i] = static_cast<float>(static_cast<std::int16_t>(p[0] | (p[1] << 8))) / 32768.0f;
            else b.samples[i] = (static_cast<float>(p[0]) - 128.0f) / 128.0f;   // 8-bit is unsigned
        }
    });
    const auto buffer_get = [self](std::uint32_t name, std::uint32_t parameter, std::int32_t& value) {
        auto* c = self->current();
        if (!c) return false;
        const auto found = c->device->buffers.find(name);
        if (found == c->device->buffers.end()) { self->al_error(al_invalid_name); return false; }
        const auto& b = found->second;
        switch (parameter) {
        case al_frequency: value = static_cast<std::int32_t>(b.frequency); return true;
        case al_bits: value = static_cast<std::int32_t>(b.bits); return true;
        case al_channels: value = static_cast<std::int32_t>(b.channels); return true;
        case al_size: value = static_cast<std::int32_t>(b.bytes); return true;
        default: self->al_error(al_invalid_enum); return false;
        }
    };
    fn("_alGetBufferi", [self, buffer_get](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        std::int32_t value{};
        if (s.x[2] && buffer_get(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), value)) m.write32(s.x[2], static_cast<std::uint32_t>(value));
    });
    fn("_alGetBufferiv", [self, buffer_get](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->mutex_);
        std::int32_t value{};
        if (s.x[2] && buffer_get(static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), value)) m.write32(s.x[2], static_cast<std::uint32_t>(value));
    });
    // OpenAL 1.1 defines no settable buffer properties and no float ones.
    for (const auto* name : {"_alBufferf", "_alBuffer3f", "_alBufferfv", "_alBufferi", "_alBuffer3i", "_alBufferiv", "_alGetBufferf",
                             "_alGetBuffer3f", "_alGetBufferfv", "_alGetBuffer3i"})
        fn(name, [self](CpuState&, GuestMemory&) {
            std::lock_guard lock(self->mutex_);
            self->al_error(al_invalid_enum);
        });
}

} // namespace

void install_openal_gui(gui::InstallContext& context) { std::make_shared<OpenAl>(context)->install(); }

} // namespace alchemy::openal
