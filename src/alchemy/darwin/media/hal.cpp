#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/hal_core.hpp"
#include "alchemy/darwin/media/hal_host.hpp"
#include "alchemy/darwin/media/hal_io.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <array>
#include <span>

// CoreAudio HAL over the HAL model (hal_core.hpp): the system object, the
// Windows audio endpoints as devices with one stream each, and their
// properties. Output devices play: IOProcs (function or block, and the old
// AudioDeviceAddIOProc form) run on the device's I/O thread through
// hal_io.hpp, with one interleaved float buffer of the device's I/O buffer
// frame size, and the device's output stream plays their sum (host::audio;
// silent in cloaked runs, hal_host.cpp). Input devices capture (hal_io.hpp
// InputIo): their IOProcs get one interleaved float input buffer of the
// device's I/O buffer frame size per call, on the device's input thread, and
// an empty output list (silence in cloaked runs). AVAudioEngine's input node
// and AVFoundation capture open their own input streams (hal_open_input). A block IOProc runs on the I/O thread, not on its queue. Host
// time is mach_absolute_time's
// (nanoseconds; frequency 1e9). The older AudioHardware, AudioDevice and
// AudioStream property calls answer the same properties, addressed by the
// direction (input or output scope) and channel (element).
namespace alchemy::gui::completion_kit::media {
namespace {

constexpr std::string_view core_audio = "/System/Library/Frameworks/CoreAudio.framework/Versions/A/CoreAudio";

std::function<std::unique_ptr<hal::Backend>()>& hal_factory() {
    static std::function<std::unique_ptr<hal::Backend>()> factory;
    return factory;
}

class NoDevices final : public hal::Backend {
public:
    hal::Snapshot snapshot() override { return {}; }
};

struct Listener { std::uint32_t object{}, selector{}, scope{}, element{}; Address proc{}, data{}; };

// An IOProc: a function (proc) or a block, and its guest buffers.
struct IoProc {
    std::uint32_t device{};
    Address proc{}, client_data{}, block{};
    Address memory{};                  // timestamps, buffer lists and the output data
    std::size_t capacity{};            // bytes of output data at memory + data_offset
    static constexpr std::size_t now_offset = 0, input_time_offset = 64, output_time_offset = 128, input_list_offset = 192,
        output_list_offset = 200, data_offset = 224;
};

struct Hal {
    Support& support;
    std::mutex mutex;
    std::unique_ptr<hal::Backend> backend;
    std::optional<hal::Model> model;
    std::vector<Listener> listeners;
    std::map<std::uint32_t, std::shared_ptr<hal_io::DeviceIo>> io;
    std::map<std::uint32_t, std::shared_ptr<hal_io::InputIo>> inputs;
    std::map<Address, std::shared_ptr<IoProc>> procs;   // by AudioDeviceIOProcID

    explicit Hal(Support& owner) : support(owner) {}

    // The device list, read on first use.
    hal::Model& get() {
        if (!model) {
            if (hal_factory()) backend = hal_factory()();
            else if (const char* value = std::getenv("ALCHEMY_AUDIO_DEVICES"); value && std::string_view(value) == "none") backend = std::make_unique<NoDevices>();
            else backend = hal::make_host_backend();
            model.emplace(backend->snapshot());
        }
        return *model;
    }
    static hal::Address address(GuestMemory& m, Address at) { return {m.read32(at), m.read32(at + 4), m.read32(at + 8)}; }
    // The qualifier of kAudioHardwarePropertyTranslateUIDToDevice: a CFStringRef.
    std::optional<std::string> qualifier(GuestMemory& m, std::uint64_t size, Address at) {
        if (size < 8 || !at) return std::nullopt;
        const auto string = m.read64(at);
        return string ? std::optional<std::string>(support.cf.utf8(string)) : std::nullopt;
    }
    // The input device's capture (nullptr for an output device). Under the mutex.
    std::shared_ptr<hal_io::InputIo> input_io(std::uint32_t id) {
        auto& slot = inputs[id];
        if (!slot) {
            const auto* device = get().device(id);
            if (!device || !device->input || !backend) return nullptr;
            slot = std::make_shared<hal_io::InputIo>(support, id, device->channels, device->sample_rate);
        }
        return slot;
    }
    // The output device's I/O (nullptr for an input device). Under the mutex.
    std::shared_ptr<hal_io::DeviceIo> device_io(std::uint32_t id) {
        auto& slot = io[id];
        if (!slot) {
            const auto* device = get().device(id);
            if (!device || device->input || !backend) return nullptr;
            slot = std::make_shared<hal_io::DeviceIo>(support, *backend, id, *device);
        }
        return slot;
    }
    // ALCHEMY_HAL_LOG=1: each unknown property (object, selector, scope, element) on stderr, once.
    void log_unknown(std::uint32_t object, const hal::Address& address, std::int32_t result) {
        static const bool enabled = [] { const char* v = std::getenv("ALCHEMY_HAL_LOG"); return v && *v && *v != '0'; }();
        if (!enabled || result != hal::error::unknown_property) return;
        const auto key = std::make_tuple(object, address.selector, address.scope, address.element);
        if (!logged.insert(key).second) return;
        const auto code = [](std::uint32_t v) { return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; };
        std::cerr << "alchemy: CoreAudio HAL: unknown property '" << code(address.selector) << "' scope '" << code(address.scope)
                  << "' element " << address.element << " of object " << object << std::endl;
    }
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> logged;
    // The device's I/O buffer frame size (kAudioDevicePropertyBufferFrameSize).
    std::uint32_t buffer_frames(std::uint32_t id) {
        hal::Value value;
        if (get().get(id, {hal::four("fsiz"), hal::scope_global, 0}, std::nullopt, value) || value.bytes.size() < 4) return 512;
        return value.bytes[0] | (value.bytes[1] << 8) | (value.bytes[2] << 16) | (static_cast<std::uint32_t>(value.bytes[3]) << 24);
    }

};

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

// An AudioTimeStamp (64 bytes) with valid sample time, host time and rate scalar 1.
void write_time_stamp(GuestMemory& m, Address at, double sample_time, std::uint64_t host_time) {
    m.fill_bytes(at, 0, 64);
    m.write64(at, std::bit_cast<std::uint64_t>(sample_time));
    m.write64(at + 8, host_time);
    m.write64(at + 16, std::bit_cast<std::uint64_t>(1.0));
    m.write32(at + 56, 7);   // kAudioTimeStampSampleTimeValid | HostTimeValid | RateScalarValid
}

// AudioObjectGetPropertyData's work (also the older AudioHardware/AudioDevice/AudioStream calls): the
// value into (UInt32 *ioSize, void *out). Arrays are truncated to the elements that fit; other values
// need their whole size. CFStrings are +1.
std::int32_t get_property(Hal& state, GuestMemory& m, std::uint32_t object, const hal::Address& address,
                          const std::optional<std::string>& qualifier, Address size_at, Address out) {
    hal::Value value;
    {
        std::lock_guard lock(state.mutex);
        if (address.selector == hal::four("uidd") && !qualifier) return hal::error::bad_size;
        if (const auto result = state.get().get(object, address, qualifier.value_or(std::string()), value)) {
            state.log_unknown(object, address, result);
            return result;
        }
    }
    const auto room = m.read32(size_at);
    if (value.string) {
        if (room < 8 || !out) return hal::error::bad_size;
        m.write64(out, state.support.cf.string(*value.string));
        m.write32(size_at, 8);
        return 0;
    }
    auto size = static_cast<std::uint32_t>(value.bytes.size());
    if (value.element_size) size = std::min(size, room / value.element_size * value.element_size);
    else if (room < size) return hal::error::bad_size;
    if (size && !out) return hal::error::param;
    if (size) m.write_bytes(out, std::vector<std::uint8_t>(value.bytes.begin(), value.bytes.begin() + size));
    m.write32(size_at, size);
    return 0;
}

// The device's clock now, while its output runs.
bool device_now(Hal& state, std::uint32_t device, hal_io::Timing& timing, double& rate) {
    std::shared_ptr<hal_io::DeviceIo> io;
    {
        std::lock_guard lock(state.mutex);
        if (const auto found = state.io.find(device); found != state.io.end()) io = found->second;
        const auto* d = state.get().device(device);
        rate = d && d->sample_rate > 0 ? d->sample_rate : 48000.0;
    }
    if (!io || !io->now(timing)) return false;
    if (timing.rate > 0) rate = timing.rate;
    return true;
}

// One buffer of an IOProc on the device's I/O thread: the proc gets an empty input list and
// one interleaved float output buffer of `frames` frames, filled with silence.
void render_io_proc(Hal& state, IoProc& proc, std::uint32_t device, std::uint32_t channels, float* mix, std::uint32_t frames,
                    const hal_io::Timing& timing) {
    auto& m = state.support.process.calling_engine();
    const auto bytes = static_cast<std::size_t>(frames) * channels * 4;
    if (proc.capacity < bytes) {
        if (proc.memory) state.support.process.release_guest(proc.memory);
        proc.memory = state.support.process.allocate_guest(IoProc::data_offset + bytes, 16);
        proc.capacity = bytes;
    }
    const auto base = proc.memory;
    write_time_stamp(m, base + IoProc::now_offset, timing.now_sample, timing.now_host);
    write_time_stamp(m, base + IoProc::input_time_offset, timing.output_sample, timing.now_host);
    write_time_stamp(m, base + IoProc::output_time_offset, timing.output_sample, timing.output_host);
    m.write64(base + IoProc::input_list_offset, 0);   // mNumberBuffers 0
    const auto list = base + IoProc::output_list_offset, data = base + IoProc::data_offset;
    m.write64(list, 1);   // mNumberBuffers 1 (and padding)
    m.write32(list + 8, channels);
    m.write32(list + 12, static_cast<std::uint32_t>(bytes));
    m.write64(list + 16, data);
    m.fill_bytes(data, 0, bytes);
    EngineAdapter adapter(m);
    if (proc.block) {
        const std::array<std::uint64_t, 6> arguments{proc.block, base + IoProc::now_offset, base + IoProc::input_list_offset,
                                                     base + IoProc::input_time_offset, list, base + IoProc::output_time_offset};
        static_cast<void>(adapter.call(m.read64(proc.block + 16), arguments));
    } else {
        const std::array<std::uint64_t, 7> arguments{device, base + IoProc::now_offset, base + IoProc::input_list_offset,
                                                     base + IoProc::input_time_offset, list, base + IoProc::output_time_offset, proc.client_data};
        static_cast<void>(adapter.call(proc.proc, arguments));
    }
    // The proc may point the buffer elsewhere or shorten it.
    const auto out = m.read64(list + 16);
    const auto size = std::min<std::size_t>(m.read32(list + 12), bytes);
    if (out && size) m.read_bytes(out, std::span(reinterpret_cast<std::uint8_t*>(mix), size));
}

// One input buffer of an IOProc on an input device's input thread: the proc gets one interleaved
// float input buffer of `frames` frames and an empty output list.
void render_input_proc(Hal& state, IoProc& proc, std::uint32_t device, std::uint32_t channels, const float* samples, std::uint32_t frames,
                       const hal_io::Timing& timing) {
    auto& m = state.support.process.calling_engine();
    const auto bytes = static_cast<std::size_t>(frames) * channels * 4;
    if (proc.capacity < bytes) {
        if (proc.memory) state.support.process.release_guest(proc.memory);
        proc.memory = state.support.process.allocate_guest(IoProc::data_offset + bytes, 16);
        proc.capacity = bytes;
    }
    const auto base = proc.memory;
    write_time_stamp(m, base + IoProc::now_offset, timing.now_sample + frames, host_time_now());
    write_time_stamp(m, base + IoProc::input_time_offset, timing.output_sample, timing.output_host);
    write_time_stamp(m, base + IoProc::output_time_offset, timing.output_sample, timing.output_host);
    const auto input = base + IoProc::output_list_offset, data = base + IoProc::data_offset;
    m.write64(base + IoProc::input_list_offset, 0);   // the output list: mNumberBuffers 0
    m.write64(input, 1);
    m.write32(input + 8, channels);
    m.write32(input + 12, static_cast<std::uint32_t>(bytes));
    m.write64(input + 16, data);
    m.write_bytes(data, std::span(reinterpret_cast<const std::uint8_t*>(samples), bytes));
    EngineAdapter adapter(m);
    const auto output_list = base + IoProc::input_list_offset;
    if (proc.block) {
        const std::array<std::uint64_t, 6> arguments{proc.block, base + IoProc::now_offset, input, base + IoProc::input_time_offset, output_list,
                                                     base + IoProc::output_time_offset};
        static_cast<void>(adapter.call(m.read64(proc.block + 16), arguments));
    } else {
        const std::array<std::uint64_t, 7> arguments{device, base + IoProc::now_offset, input, base + IoProc::input_time_offset, output_list,
                                                     base + IoProc::output_time_offset, proc.client_data};
        static_cast<void>(adapter.call(proc.proc, arguments));
    }
}

} // namespace

void set_hal_backend_factory(std::function<std::unique_ptr<hal::Backend>()> factory) { hal_factory() = std::move(factory); }

namespace {
std::mutex current_mutex;
std::weak_ptr<Hal> current_hal;
std::shared_ptr<Hal> current() {
    std::lock_guard lock(current_mutex);
    return current_hal.lock();
}
} // namespace

std::shared_ptr<hal_io::DeviceIo> hal_device_io(std::uint32_t device) {
    const auto state = current();
    if (!state) return nullptr;
    std::lock_guard lock(state->mutex);
    return state->device_io(device);
}

std::uint32_t hal_default_output() {
    const auto state = current();
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    hal::Value value;
    if (state->get().get(hal::system_object, {hal::four("dOut"), hal::scope_global, 0}, std::nullopt, value) || value.bytes.size() < 4) return 0;
    return value.bytes[0] | (value.bytes[1] << 8) | (value.bytes[2] << 16) | (static_cast<std::uint32_t>(value.bytes[3]) << 24);
}

std::shared_ptr<hal_io::InputIo> hal_input_io(std::uint32_t device) {
    const auto state = current();
    if (!state) return nullptr;
    std::lock_guard lock(state->mutex);
    return state->input_io(device);
}

std::uint32_t hal_default_input() {
    const auto state = current();
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    hal::Value value;
    if (state->get().get(hal::system_object, {hal::four("dIn "), hal::scope_global, 0}, std::nullopt, value) || value.bytes.size() < 4) return 0;
    return value.bytes[0] | (value.bytes[1] << 8) | (value.bytes[2] << 16) | (static_cast<std::uint32_t>(value.bytes[3]) << 24);
}

std::vector<HalInputDevice> hal_input_devices() {
    std::vector<HalInputDevice> out;
    const auto state = current();
    if (!state) return out;
    std::lock_guard lock(state->mutex);
    for (const auto id : state->get().devices())
        if (const auto* device = state->get().device(id); device && device->input)
            out.push_back({id, device->uid, device->name, std::max(device->channels, 1U), device->sample_rate > 0 ? device->sample_rate : 48000.0});
    return out;
}

std::unique_ptr<hal::InputStream> hal_open_input(std::uint32_t id, std::function<void(const float*, std::uint32_t, std::uint64_t)> capture) {
    const auto state = current();
    if (!state) return nullptr;
    std::lock_guard lock(state->mutex);
    const auto* device = state->get().device(id);
    if (!device || !device->input || !state->backend) return nullptr;
    return state->backend->open_input(*device, std::move(capture));
}

std::uint32_t hal_buffer_frames(std::uint32_t device) {
    const auto state = current();
    if (!state) return 512;
    std::lock_guard lock(state->mutex);
    return state->buffer_frames(device);
}

std::int32_t hal_set_buffer_frames(std::uint32_t device, std::uint32_t frames) {
    const auto state = current();
    if (!state) return hal::error::bad_device;
    std::lock_guard lock(state->mutex);
    const std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(frames), static_cast<std::uint8_t>(frames >> 8),
                                          static_cast<std::uint8_t>(frames >> 16), static_cast<std::uint8_t>(frames >> 24)};
    return state->get().set(device, {hal::four("fsiz"), hal::scope_global, 0}, bytes);
}

void install_hal(Support& support) {
    auto state = std::make_shared<Hal>(support);
    {
        std::lock_guard lock(current_mutex);
        current_hal = state;
    }
    support.function(core_audio, "_AudioObjectHasProperty", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = 0; return; }
        std::lock_guard lock(state->mutex);
        s.x[0] = state->get().has(static_cast<std::uint32_t>(s.x[0]), Hal::address(m, s.x[1])) ? 1 : 0;
    });
    // (object, address, Boolean *outIsSettable)
    support.function(core_audio, "_AudioObjectIsPropertySettable", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[2]) return status(s, hal::error::param);
        std::lock_guard lock(state->mutex);
        bool settable{};
        const auto result = state->get().settable(static_cast<std::uint32_t>(s.x[0]), Hal::address(m, s.x[1]), settable);
        if (!result) m.write8(s.x[2], settable ? 1 : 0);
        status(s, result);
    });
    // (object, address, UInt32 qualifierSize, const void *qualifier, UInt32 *outDataSize)
    support.function(core_audio, "_AudioObjectGetPropertyDataSize", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[4]) return status(s, hal::error::param);
        std::lock_guard lock(state->mutex);
        hal::Value value;
        const auto result = state->get().get(static_cast<std::uint32_t>(s.x[0]), Hal::address(m, s.x[1]),
                                             state->qualifier(m, static_cast<std::uint32_t>(s.x[2]), s.x[3]).value_or(std::string()), value);
        state->log_unknown(static_cast<std::uint32_t>(s.x[0]), Hal::address(m, s.x[1]), result);
        if (!result) m.write32(s.x[4], value.string ? 8U : static_cast<std::uint32_t>(value.bytes.size()));
        status(s, result);
    });
    // (object, address, qualifierSize, qualifier, UInt32 *ioDataSize, void *outData)
    support.function(core_audio, "_AudioObjectGetPropertyData", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[4]) return status(s, hal::error::param);
        const auto address = Hal::address(m, s.x[1]);
        const auto qualifier = state->qualifier(m, static_cast<std::uint32_t>(s.x[2]), s.x[3]);
        status(s, get_property(*state, m, static_cast<std::uint32_t>(s.x[0]), address, qualifier, s.x[4], s.x[5]));
    });
    // (object, address, qualifierSize, qualifier, UInt32 dataSize, const void *data)
    support.function(core_audio, "_AudioObjectSetPropertyData", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || (s.x[4] && !s.x[5])) return status(s, hal::error::param);
        std::vector<std::uint8_t> data(static_cast<std::uint32_t>(s.x[4]));
        if (!data.empty()) m.read_bytes(s.x[5], data);
        std::lock_guard lock(state->mutex);
        status(s, state->get().set(static_cast<std::uint32_t>(s.x[0]), Hal::address(m, s.x[1]), data));
    });
    // Listeners are kept per (object, address, proc, data). The device list never changes
    // after it is read, so they are never called.
    support.function(core_audio, "_AudioObjectAddPropertyListener", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[2]) return status(s, hal::error::param);
        std::lock_guard lock(state->mutex);
        if (!state->get().kind(static_cast<std::uint32_t>(s.x[0]))) return status(s, hal::error::bad_object);
        const auto address = Hal::address(m, s.x[1]);
        state->listeners.push_back({static_cast<std::uint32_t>(s.x[0]), address.selector, address.scope, address.element, s.x[2], s.x[3]});
        status(s, 0);
    });
    support.function(core_audio, "_AudioObjectRemovePropertyListener", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[2]) return status(s, hal::error::param);
        std::lock_guard lock(state->mutex);
        const auto address = Hal::address(m, s.x[1]);
        std::erase_if(state->listeners, [&](const Listener& l) {
            return l.object == static_cast<std::uint32_t>(s.x[0]) && l.selector == address.selector && l.scope == address.scope &&
                l.element == address.element && l.proc == s.x[2] && l.data == s.x[3];
        });
        status(s, 0);
    });
    // Block listeners (object, address, dispatch_queue_t, block): nothing changes, so the block is never called.
    for (const auto* name : {"_AudioObjectAddPropertyListenerBlock", "_AudioObjectRemovePropertyListenerBlock"})
        support.function(core_audio, name, [state](CpuState& s, GuestMemory&) {
            if (!s.x[1] || !s.x[3]) return status(s, hal::error::param);
            std::lock_guard lock(state->mutex);
            status(s, state->get().kind(static_cast<std::uint32_t>(s.x[0])) ? 0 : hal::error::bad_object);
        });

    // ---- the older property calls (AudioHardwareDeprecated.h) ---------------------------------
    // The system's properties are global; a device's are in the input or output scope and the
    // channel's element; a stream's are global, in the channel's element.
    const auto global = [](std::uint64_t selector) { return hal::Address{static_cast<std::uint32_t>(selector), hal::scope_global, 0}; };
    const auto directed = [](std::uint64_t channel, std::uint64_t is_input, std::uint64_t selector) {
        return hal::Address{static_cast<std::uint32_t>(selector), (is_input & 0xff) ? hal::scope_input : hal::scope_output, static_cast<std::uint32_t>(channel)};
    };
    const auto in_channel = [](std::uint64_t channel, std::uint64_t selector) {
        return hal::Address{static_cast<std::uint32_t>(selector), hal::scope_global, static_cast<std::uint32_t>(channel)};
    };
    // ...GetPropertyInfo: (UInt32 *outSize, Boolean *outWritable), either may be NULL.
    const auto info = [state](CpuState& s, GuestMemory& m, std::uint32_t object, const hal::Address& address, Address size_at, Address writable_at) {
        std::lock_guard lock(state->mutex);
        hal::Value value;
        const auto result = state->get().get(object, address, std::nullopt, value);
        state->log_unknown(object, address, result);
        if (result) return status(s, result);
        bool settable{};
        if (state->get().settable(object, address, settable)) settable = false;
        if (size_at) m.write32(size_at, value.string ? 8U : static_cast<std::uint32_t>(value.bytes.size()));
        if (writable_at) m.write8(writable_at, settable ? 1 : 0);
        status(s, 0);
    };
    // ...SetProperty: (UInt32 size, const void *data); the AudioTimeStamp of when is not used (now).
    const auto set = [state](CpuState& s, GuestMemory& m, std::uint32_t object, const hal::Address& address, std::uint64_t size, Address data) {
        if (size && !data) return status(s, hal::error::param);
        std::vector<std::uint8_t> bytes(static_cast<std::uint32_t>(size));
        if (!bytes.empty()) m.read_bytes(data, bytes);
        std::lock_guard lock(state->mutex);
        status(s, state->get().set(object, address, bytes));
    };
    const auto get = [state](CpuState& s, GuestMemory& m, std::uint32_t object, const hal::Address& address, Address size_at, Address out) {
        if (!size_at) return status(s, hal::error::param);
        status(s, get_property(*state, m, object, address, std::nullopt, size_at, out));
    };
    const auto object = [](std::uint64_t value) { return static_cast<std::uint32_t>(value); };
    // AudioHardwareGetPropertyInfo(id, outSize, outWritable), AudioHardwareGetProperty(id, ioSize, out),
    // AudioHardwareSetProperty(id, size, data)
    support.function(core_audio, "_AudioHardwareGetPropertyInfo", [info, global](CpuState& s, GuestMemory& m) {
        info(s, m, hal::system_object, global(s.x[0]), s.x[1], s.x[2]);
    });
    support.function(core_audio, "_AudioHardwareGetProperty", [get, global](CpuState& s, GuestMemory& m) {
        get(s, m, hal::system_object, global(s.x[0]), s.x[1], s.x[2]);
    });
    support.function(core_audio, "_AudioHardwareSetProperty", [set, global](CpuState& s, GuestMemory& m) {
        set(s, m, hal::system_object, global(s.x[0]), s.x[1], s.x[2]);
    });
    // AudioDeviceGetPropertyInfo(device, channel, isInput, id, outSize, outWritable),
    // AudioDeviceGetProperty(device, channel, isInput, id, ioSize, out),
    // AudioDeviceSetProperty(device, when, channel, isInput, id, size, data)
    support.function(core_audio, "_AudioDeviceGetPropertyInfo", [info, directed, object](CpuState& s, GuestMemory& m) {
        info(s, m, object(s.x[0]), directed(s.x[1], s.x[2], s.x[3]), s.x[4], s.x[5]);
    });
    support.function(core_audio, "_AudioDeviceGetProperty", [get, directed, object](CpuState& s, GuestMemory& m) {
        get(s, m, object(s.x[0]), directed(s.x[1], s.x[2], s.x[3]), s.x[4], s.x[5]);
    });
    support.function(core_audio, "_AudioDeviceSetProperty", [set, directed, object](CpuState& s, GuestMemory& m) {
        set(s, m, object(s.x[0]), directed(s.x[2], s.x[3], s.x[4]), s.x[5], s.x[6]);
    });
    // AudioStreamGetPropertyInfo(stream, channel, id, outSize, outWritable),
    // AudioStreamGetProperty(stream, channel, id, ioSize, out),
    // AudioStreamSetProperty(stream, when, channel, id, size, data)
    support.function(core_audio, "_AudioStreamGetPropertyInfo", [info, in_channel, object](CpuState& s, GuestMemory& m) {
        info(s, m, object(s.x[0]), in_channel(s.x[1], s.x[2]), s.x[3], s.x[4]);
    });
    support.function(core_audio, "_AudioStreamGetProperty", [get, in_channel, object](CpuState& s, GuestMemory& m) {
        get(s, m, object(s.x[0]), in_channel(s.x[1], s.x[2]), s.x[3], s.x[4]);
    });
    support.function(core_audio, "_AudioStreamSetProperty", [set, in_channel, object](CpuState& s, GuestMemory& m) {
        set(s, m, object(s.x[0]), in_channel(s.x[2], s.x[3]), s.x[4], s.x[5]);
    });
    // The older listeners, like AudioObjectAddPropertyListener's, are never called: nothing changes.
    // AudioHardwareAdd/RemovePropertyListener(id, proc[, client])
    for (const auto* name : {"_AudioHardwareAddPropertyListener", "_AudioHardwareRemovePropertyListener"})
        support.function(core_audio, name, [](CpuState& s, GuestMemory&) { status(s, s.x[1] ? 0 : hal::error::param); });
    // AudioDeviceAdd/RemovePropertyListener(device, channel, isInput, id, proc[, client]),
    // AudioStreamAdd/RemovePropertyListener(stream, channel, id, proc[, client])
    const auto listener = [state](CpuState& s, Address proc) {
        if (!proc) return status(s, hal::error::param);
        std::lock_guard lock(state->mutex);
        status(s, state->get().kind(static_cast<std::uint32_t>(s.x[0])) ? 0 : hal::error::bad_object);
    };
    for (const auto* name : {"_AudioDeviceAddPropertyListener", "_AudioDeviceRemovePropertyListener"})
        support.function(core_audio, name, [listener](CpuState& s, GuestMemory&) { listener(s, s.x[4]); });
    for (const auto* name : {"_AudioStreamAddPropertyListener", "_AudioStreamRemovePropertyListener"})
        support.function(core_audio, name, [listener](CpuState& s, GuestMemory&) { listener(s, s.x[3]); });
    // AudioHardwareUnload(): nothing is loaded that could be unloaded.
    support.function(core_audio, "_AudioHardwareUnload", [](CpuState& s, GuestMemory&) { status(s, 0); });
    // AudioObjectShow(object): a debugging aid that prints nothing here.
    support.function(core_audio, "_AudioObjectShow", [](CpuState&, GuestMemory&) {});

    // ---- device I/O ----------------------------------------------------------------------
    const auto device_check = [state](std::uint32_t device) {
        std::lock_guard lock(state->mutex);
        return state->get().kind(device) == hal::Kind::device;
    };
    // Registers an IOProc; its ID is a guest address of its own (the proc for AudioDeviceAddIOProc).
    const auto create = [state](std::uint32_t device, Address proc, Address data, Address block, Address id) {
        auto made = std::make_shared<IoProc>();
        made->device = device;
        made->proc = proc;
        made->client_data = data;
        made->block = block;
        if (!id) id = state->support.process.allocate_guest(16, 16);
        std::lock_guard lock(state->mutex);
        state->procs[id] = std::move(made);
        return id;
    };
    // AudioDeviceCreateIOProcID(device, proc, clientData, AudioDeviceIOProcID *out)
    support.function(core_audio, "_AudioDeviceCreateIOProcID", [device_check, create](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write64(s.x[3], 0);
        if (!device_check(static_cast<std::uint32_t>(s.x[0]))) return status(s, hal::error::bad_device);
        if (!s.x[1] || !s.x[3]) return status(s, hal::error::param);
        m.write64(s.x[3], create(static_cast<std::uint32_t>(s.x[0]), s.x[1], s.x[2], 0, 0));
        status(s, 0);
    });
    // AudioDeviceCreateIOProcIDWithBlock(AudioDeviceIOProcID *out, device, queue, block)
    support.function(core_audio, "_AudioDeviceCreateIOProcIDWithBlock", [state, device_check, create](CpuState& s, GuestMemory& m) {
        if (s.x[0]) m.write64(s.x[0], 0);
        if (!device_check(static_cast<std::uint32_t>(s.x[1]))) return status(s, hal::error::bad_device);
        if (!s.x[0] || !s.x[3]) return status(s, hal::error::param);
        const auto block = state->support.call(libsystem, "__Block_copy", {s.x[3]});
        m.write64(s.x[0], create(static_cast<std::uint32_t>(s.x[1]), 0, 0, block, 0));
        status(s, 0);
    });
    // AudioDeviceAddIOProc(device, proc, clientData): the proc is its own ID.
    support.function(core_audio, "_AudioDeviceAddIOProc", [state, device_check, create](CpuState& s, GuestMemory&) {
        if (!device_check(static_cast<std::uint32_t>(s.x[0]))) return status(s, hal::error::bad_device);
        if (!s.x[1]) return status(s, hal::error::param);
        {
            std::lock_guard lock(state->mutex);
            if (state->procs.contains(s.x[1])) return status(s, hal::error::illegal_operation);
        }
        static_cast<void>(create(static_cast<std::uint32_t>(s.x[0]), s.x[1], s.x[2], 0, s.x[1]));
        status(s, 0);
    });
    const auto destroy = [state](CpuState& s) {
        std::shared_ptr<IoProc> proc;
        std::shared_ptr<hal_io::DeviceIo> io;
        std::shared_ptr<hal_io::InputIo> input;
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->procs.find(s.x[1]);
            if (found == state->procs.end() || found->second->device != static_cast<std::uint32_t>(s.x[0]))
                return status(s, hal::error::illegal_operation);
            proc = std::move(found->second);
            state->procs.erase(found);
            if (const auto slot = state->io.find(proc->device); slot != state->io.end()) io = slot->second;
            if (const auto slot = state->inputs.find(proc->device); slot != state->inputs.end()) input = slot->second;
        }
        if (io) io->stop(s.x[1]);
        if (input) input->stop(s.x[1]);
        if (proc->block) static_cast<void>(state->support.call(libsystem, "__Block_release", {proc->block}));
        if (proc->memory) state->support.process.release_guest(proc->memory);
        status(s, 0);
    };
    for (const auto* name : {"_AudioDeviceDestroyIOProcID", "_AudioDeviceRemoveIOProc"})
        support.function(core_audio, name, [device_check, destroy](CpuState& s, GuestMemory&) {
            if (!device_check(static_cast<std::uint32_t>(s.x[0]))) return status(s, hal::error::bad_device);
            destroy(s);
        });
    // AudioDeviceStart(device, procID): a NULL proc starts the device alone (it plays silence).
    support.function(core_audio, "_AudioDeviceStart", [state, device_check](CpuState& s, GuestMemory&) {
        const auto device = static_cast<std::uint32_t>(s.x[0]);
        if (!device_check(device)) return status(s, hal::error::bad_device);
        std::shared_ptr<IoProc> proc;
        std::shared_ptr<hal_io::DeviceIo> io;
        std::shared_ptr<hal_io::InputIo> input;
        std::uint32_t frames{};
        {
            std::lock_guard lock(state->mutex);
            if (s.x[1]) {
                const auto found = state->procs.find(s.x[1]);
                if (found == state->procs.end() || found->second->device != device) return status(s, hal::error::illegal_operation);
                proc = found->second;
            }
            io = state->device_io(device);
            if (!io) input = state->input_io(device);
            frames = state->buffer_frames(device);
        }
        if (input) {
            hal_io::InputClient client = [](const float*, std::uint32_t, const hal_io::Timing&) {};
            if (proc)
                client = [state, proc, device, channels = input->channels()](const float* samples, std::uint32_t count, const hal_io::Timing& timing) {
                    render_input_proc(*state, *proc, device, channels, samples, count, timing);
                };
            return status(s, input->start(s.x[1], std::move(client), frames));
        }
        if (!io) return status(s, hal::error::unsupported);
        hal_io::Client client = [](float*, std::uint32_t, const hal_io::Timing&) {};
        if (proc)
            client = [state, proc, device, channels = io->channels()](float* mix, std::uint32_t count, const hal_io::Timing& timing) {
                render_io_proc(*state, *proc, device, channels, mix, count, timing);
            };
        status(s, io->start(s.x[1], std::move(client), frames));
    });
    support.function(core_audio, "_AudioDeviceStop", [state, device_check](CpuState& s, GuestMemory&) {
        const auto device = static_cast<std::uint32_t>(s.x[0]);
        if (!device_check(device)) return status(s, hal::error::bad_device);
        std::shared_ptr<hal_io::DeviceIo> io;
        std::shared_ptr<hal_io::InputIo> input;
        {
            std::lock_guard lock(state->mutex);
            if (s.x[1] && !state->procs.contains(s.x[1])) return status(s, hal::error::illegal_operation);
            if (const auto found = state->io.find(device); found != state->io.end()) io = found->second;
            if (const auto found = state->inputs.find(device); found != state->inputs.end()) input = found->second;
        }
        if (io) io->stop(s.x[1]);
        if (input) input->stop(s.x[1]);
        status(s, 0);
    });
    // AudioDeviceGetCurrentTime(device, AudioTimeStamp *out)
    support.function(core_audio, "_AudioDeviceGetCurrentTime", [state, device_check](CpuState& s, GuestMemory& m) {
        const auto device = static_cast<std::uint32_t>(s.x[0]);
        if (!device_check(device)) return status(s, hal::error::bad_device);
        if (!s.x[1]) return status(s, hal::error::param);
        std::shared_ptr<hal_io::DeviceIo> io;
        {
            std::lock_guard lock(state->mutex);
            if (const auto found = state->io.find(device); found != state->io.end()) io = found->second;
        }
        hal_io::Timing timing;
        if (!io || !io->now(timing)) return status(s, hal::error::not_running);
        write_time_stamp(m, s.x[1], timing.now_sample, timing.now_host);
        status(s, 0);
    });

    // AudioDeviceTranslateTime(device, const AudioTimeStamp *in, AudioTimeStamp *out): a sample time to
    // host time (or a host time to sample time) along the device's clock, while its output runs.
    support.function(core_audio, "_AudioDeviceTranslateTime", [state, device_check](CpuState& s, GuestMemory& m) {
        const auto device = static_cast<std::uint32_t>(s.x[0]);
        if (!device_check(device)) return status(s, hal::error::bad_device);
        if (!s.x[1] || !s.x[2]) return status(s, hal::error::param);
        hal_io::Timing timing;
        double rate{};
        if (!device_now(*state, device, timing, rate)) return status(s, hal::error::not_running);
        const auto flags = m.read32(s.x[1] + 56);
        double sample{};
        std::uint64_t host{};
        if (flags & 1) {   // kAudioTimeStampSampleTimeValid
            sample = std::bit_cast<double>(m.read64(s.x[1]));
            host = static_cast<std::uint64_t>(static_cast<std::int64_t>(timing.now_host) + std::llround((sample - timing.now_sample) / rate * 1e9));
        } else if (flags & 2) {   // kAudioTimeStampHostTimeValid
            host = m.read64(s.x[1] + 8);
            sample = timing.now_sample + static_cast<double>(static_cast<std::int64_t>(host - timing.now_host)) * rate / 1e9;
        } else {
            return status(s, hal::error::param);
        }
        write_time_stamp(m, s.x[2], sample, host);
        status(s, 0);
    });
    // AudioDeviceGetNearestStartTime(device, AudioTimeStamp *ioRequestedStartTime, UInt32 flags): any time
    // from now on can start I/O; an earlier one becomes now.
    support.function(core_audio, "_AudioDeviceGetNearestStartTime", [state, device_check](CpuState& s, GuestMemory& m) {
        const auto device = static_cast<std::uint32_t>(s.x[0]);
        if (!device_check(device)) return status(s, hal::error::bad_device);
        if (!s.x[1]) return status(s, hal::error::param);
        hal_io::Timing timing;
        double rate{};
        if (!device_now(*state, device, timing, rate)) return status(s, hal::error::not_running);
        const auto flags = m.read32(s.x[1] + 56);
        double sample = timing.now_sample;
        if (flags & 1) sample = std::max(sample, std::bit_cast<double>(m.read64(s.x[1])));
        else if (flags & 2) sample = std::max(sample, timing.now_sample + static_cast<double>(static_cast<std::int64_t>(m.read64(s.x[1] + 8) - timing.now_host)) * rate / 1e9);
        write_time_stamp(m, s.x[1], sample,
                         static_cast<std::uint64_t>(static_cast<std::int64_t>(timing.now_host) + std::llround((sample - timing.now_sample) / rate * 1e9)));
        status(s, 0);
    });

    // ---- host time ---------------------------------------------------------------------------
    support.function(core_audio, "_AudioGetCurrentHostTime", [](CpuState& s, GuestMemory&) { s.x[0] = host_time_now(); });
    support.function(core_audio, "_AudioGetHostClockFrequency", [](CpuState& s, GuestMemory&) { s.v[0] = {std::bit_cast<std::uint64_t>(1e9), 0}; });
    support.function(core_audio, "_AudioGetHostClockMinimumTimeDelta", [](CpuState& s, GuestMemory&) { s.x[0] = 100; });
    // Host time ticks are nanoseconds: both conversions are the identity.
    support.function(core_audio, "_AudioConvertHostTimeToNanos", [](CpuState&, GuestMemory&) {});
    support.function(core_audio, "_AudioConvertNanosToHostTime", [](CpuState&, GuestMemory&) {});
}

} // namespace alchemy::gui::completion_kit::media
