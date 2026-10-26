#pragma once

// CoreAudio HAL object model (host-only core, QLab gap group G5): the system
// object, one audio device per Windows audio endpoint (host::audio in alchemy-fx,
// a fake in tests) and one stream per device. Properties follow
// <CoreAudio/AudioHardware.h> and <AudioHardwareBase.h>: selectors, scopes
// (global/input/output), data sizes, array truncation and the
// kAudioHardware*Error codes.
//
// Windows endpoints carry one direction each, so a device has either an
// input or an output stream (as a Mac's built-in speakers and microphone are
// separate devices). The stream format is the endpoint's shared-mode format
// as 32-bit float, interleaved. The device list is read on first use and
// does not change afterwards (no hot-plug; listeners are recorded but have
// nothing to report). Default devices are Windows' console defaults and
// cannot be changed from here; nominal sample rates are the endpoint's own
// rate (shared mode cannot change it). The I/O buffer frame size is a
// per-process setting kept as set. Output devices play through a Backend
// output stream (hal_io.hpp runs the IOProcs); input devices have no I/O.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace alchemy::gui::completion_kit::media::hal {

constexpr std::uint32_t four(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

namespace error {
inline constexpr std::int32_t not_running = static_cast<std::int32_t>(four("stop")), unknown_property = static_cast<std::int32_t>(four("who?")),
    bad_size = static_cast<std::int32_t>(four("!siz")), illegal_operation = static_cast<std::int32_t>(four("nope")),
    bad_object = static_cast<std::int32_t>(four("!obj")), bad_device = static_cast<std::int32_t>(four("!dev")),
    unsupported = static_cast<std::int32_t>(four("unop")), param = -50;
}

inline constexpr std::uint32_t system_object = 1;
inline constexpr std::uint32_t scope_global = four("glob"), scope_input = four("inpt"), scope_output = four("outp");

struct Device {
    std::string uid, name, manufacturer;
    bool input{};                      // capture endpoint (else render)
    std::uint32_t channels{2};
    double sample_rate{48000};
    std::uint32_t channel_mask{};      // WAVEFORMATEXTENSIBLE dwChannelMask (0 = unknown)
    std::uint32_t form_factor{};       // EndpointFormFactor (0 = remote network device, 10 = unknown)
};

struct Snapshot {
    std::vector<Device> devices;
    std::string default_input, default_output;   // device UIDs (empty: none)
};

// Asks for `frames` interleaved float frames in the device's channels and rate; `queued` frames
// are already ahead of them in the host. Called on a thread of the stream.
using Render = std::function<void(float* samples, std::uint32_t frames, std::uint32_t queued)>;

// A device's output stream: render runs between start and stop (stop waits for it).
class OutputStream {
public:
    virtual ~OutputStream() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual Snapshot snapshot() = 0;
    // An output stream on `device` (an output device of the snapshot), or nullptr.
    virtual std::unique_ptr<OutputStream> open_output(const Device&, Render) { return nullptr; }
};

struct Address {
    std::uint32_t selector{}, scope{scope_global}, element{};
};

// A property value: raw bytes, or a CFString the guest layer creates (+1).
struct Value {
    std::vector<std::uint8_t> bytes;
    std::optional<std::string> string;
    std::uint32_t element_size{};      // > 0: an array that may be truncated to whole elements
};

enum class Kind { system, device, stream };

class Model {
public:
    explicit Model(Snapshot snapshot);

    [[nodiscard]] std::optional<Kind> kind(std::uint32_t object) const;
    [[nodiscard]] std::optional<std::uint32_t> device_for_uid(const std::string& uid) const;
    [[nodiscard]] const Device* device(std::uint32_t object) const;
    [[nodiscard]] std::vector<std::uint32_t> devices() const { return device_ids_; }

    // AudioObjectHasProperty / IsPropertySettable / GetPropertyDataSize / GetPropertyData /
    // SetPropertyData. `qualifier` is the UID string of kAudioHardwarePropertyTranslateUIDToDevice.
    [[nodiscard]] bool has(std::uint32_t object, const Address& address) const;
    std::int32_t settable(std::uint32_t object, const Address& address, bool& out) const;
    std::int32_t get(std::uint32_t object, const Address& address, const std::optional<std::string>& qualifier, Value& out) const;
    std::int32_t set(std::uint32_t object, const Address& address, const std::vector<std::uint8_t>& data);

private:
    struct Entry { std::uint32_t id{}, stream{}; Device device; std::uint32_t buffer_frames{512}; };
    [[nodiscard]] const Entry* entry_for_device(std::uint32_t object) const;
    [[nodiscard]] const Entry* entry_for_stream(std::uint32_t object) const;
    std::int32_t system_get(const Address& address, const std::optional<std::string>& qualifier, Value& out) const;
    std::int32_t device_get(const Entry& entry, const Address& address, Value& out) const;
    std::int32_t stream_get(const Entry& entry, const Address& address, Value& out) const;

    Snapshot snapshot_;
    std::uint64_t run_loop_{};          // kAudioHardwarePropertyRunLoop (NULL: the HAL's own thread)
    bool sleeping_allowed_{true};
    std::vector<Entry> entries_;
    std::vector<std::uint32_t> device_ids_;
};

// Bytes of an AudioStreamBasicDescription for interleaved 32-bit float.
std::vector<std::uint8_t> float_format(double sample_rate, std::uint32_t channels);

} // namespace alchemy::gui::completion_kit::media::hal
