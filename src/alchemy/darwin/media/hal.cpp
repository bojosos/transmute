#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/hal_core.hpp"
#include "alchemy/darwin/media/hal_host.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>

// CoreAudio HAL over the HAL model (hal_core.hpp): the system object, the
// Windows audio endpoints as devices with one stream each, and their
// properties. Device I/O is not provided: creating or starting an IOProc on a
// device answers kAudioHardwareUnsupportedOperationError (a stderr notice
// says so once), and no device ever runs. Host time is mach_absolute_time's
// (nanoseconds; frequency 1e9).
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

struct Hal {
    Support& support;
    std::mutex mutex;
    std::optional<hal::Model> model;
    std::vector<Listener> listeners;
    bool io_notice{};

    explicit Hal(Support& owner) : support(owner) {}

    // The device list, read on first use.
    hal::Model& get() {
        if (!model) {
            std::unique_ptr<hal::Backend> backend;
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
    void notice_io() {
        if (std::exchange(io_notice, true)) return;
        std::cerr << "alchemy: CoreAudio device I/O is not provided (IOProcs answer kAudioHardwareUnsupportedOperationError)" << std::endl;
    }
};

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

} // namespace

void set_hal_backend_factory(std::function<std::unique_ptr<hal::Backend>()> factory) { hal_factory() = std::move(factory); }

void install_hal(Support& support) {
    auto state = std::make_shared<Hal>(support);
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
        if (!result) m.write32(s.x[4], value.string ? 8U : static_cast<std::uint32_t>(value.bytes.size()));
        status(s, result);
    });
    // (object, address, qualifierSize, qualifier, UInt32 *ioDataSize, void *outData): arrays are
    // truncated to the elements that fit; other values need their whole size. CFStrings are +1.
    support.function(core_audio, "_AudioObjectGetPropertyData", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1] || !s.x[4]) return status(s, hal::error::param);
        hal::Value value;
        {
            std::lock_guard lock(state->mutex);
            const auto address = Hal::address(m, s.x[1]);
            const auto qualifier = state->qualifier(m, static_cast<std::uint32_t>(s.x[2]), s.x[3]);
            if (address.selector == hal::four("uidd") && !qualifier) return status(s, hal::error::bad_size);
            if (const auto result = state->get().get(static_cast<std::uint32_t>(s.x[0]), address, qualifier.value_or(std::string()), value))
                return status(s, result);
        }
        const auto room = m.read32(s.x[4]);
        if (value.string) {
            if (room < 8 || !s.x[5]) return status(s, hal::error::bad_size);
            m.write64(s.x[5], state->support.cf.string(*value.string));
            m.write32(s.x[4], 8);
            return status(s, 0);
        }
        auto size = static_cast<std::uint32_t>(value.bytes.size());
        if (value.element_size) size = std::min(size, room / value.element_size * value.element_size);
        else if (room < size) return status(s, hal::error::bad_size);
        if (size && !s.x[5]) return status(s, hal::error::param);
        if (size) m.write_bytes(s.x[5], std::vector<std::uint8_t>(value.bytes.begin(), value.bytes.begin() + size));
        m.write32(s.x[4], size);
        status(s, 0);
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

    // ---- device I/O: not provided --------------------------------------------------------
    const auto device_check = [state](std::uint32_t device) {
        std::lock_guard lock(state->mutex);
        return state->get().kind(device) == hal::Kind::device;
    };
    // AudioDeviceCreateIOProcID(device, proc, clientData, AudioDeviceIOProcID *out)
    support.function(core_audio, "_AudioDeviceCreateIOProcID", [state, device_check](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write64(s.x[3], 0);
        if (!device_check(static_cast<std::uint32_t>(s.x[0]))) return status(s, hal::error::bad_device);
        if (!s.x[1] || !s.x[3]) return status(s, hal::error::param);
        state->notice_io();
        status(s, hal::error::unsupported);
    });
    // AudioDeviceCreateIOProcIDWithBlock(AudioDeviceIOProcID *out, device, queue, block)
    support.function(core_audio, "_AudioDeviceCreateIOProcIDWithBlock", [state, device_check](CpuState& s, GuestMemory& m) {
        if (s.x[0]) m.write64(s.x[0], 0);
        if (!device_check(static_cast<std::uint32_t>(s.x[1]))) return status(s, hal::error::bad_device);
        if (!s.x[0] || !s.x[3]) return status(s, hal::error::param);
        state->notice_io();
        status(s, hal::error::unsupported);
    });
    // Start with a NULL IOProc starts the hardware: unsupported too; with a proc, none can exist.
    support.function(core_audio, "_AudioDeviceStart", [state, device_check](CpuState& s, GuestMemory&) {
        if (!device_check(static_cast<std::uint32_t>(s.x[0]))) return status(s, hal::error::bad_device);
        state->notice_io();
        status(s, s.x[1] ? hal::error::illegal_operation : hal::error::unsupported);
    });
    // Stopping or destroying an IOProc that was never created.
    for (const auto* name : {"_AudioDeviceStop", "_AudioDeviceDestroyIOProcID"})
        support.function(core_audio, name, [device_check](CpuState& s, GuestMemory&) {
            if (!device_check(static_cast<std::uint32_t>(s.x[0]))) return status(s, hal::error::bad_device);
            status(s, s.x[1] ? hal::error::illegal_operation : 0);   // stopping hardware that is not running: done
        });
    // AudioDeviceGetCurrentTime(device, AudioTimeStamp *out): no device runs.
    support.function(core_audio, "_AudioDeviceGetCurrentTime", [device_check](CpuState& s, GuestMemory&) {
        if (!device_check(static_cast<std::uint32_t>(s.x[0]))) return status(s, hal::error::bad_device);
        status(s, hal::error::not_running);
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
