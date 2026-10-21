#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

#include <atomic>
#include <map>
#include <optional>
#include <span>

// OpenAL 1.1 with no output or capture device. No device or context can be
// created, so every handle a caller passes is invalid and AL calls have no
// current context.
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

constexpr std::string_view openal = darwin_framework_libraries[4];

// al.h / alc.h
constexpr std::uint32_t al_invalid_operation = 0xa004;
constexpr std::uint32_t alc_invalid_device = 0xa001, alc_invalid_context = 0xa002, alc_invalid_enum = 0xa003,
    alc_invalid_value = 0xa004, alc_out_of_memory = 0xa005;
constexpr std::uint32_t alc_major_version = 0x1000, alc_minor_version = 0x1001,
    alc_default_device_specifier = 0x1004, alc_device_specifier = 0x1005, alc_extensions = 0x1006,
    alc_capture_device_specifier = 0x310, alc_capture_default_device_specifier = 0x311;

constexpr std::pair<std::string_view, std::uint32_t> al_enums[] = {
    {"AL_NONE", 0}, {"AL_FALSE", 0}, {"AL_TRUE", 1}, {"AL_SOURCE_RELATIVE", 0x202},
    {"AL_CONE_INNER_ANGLE", 0x1001}, {"AL_CONE_OUTER_ANGLE", 0x1002}, {"AL_PITCH", 0x1003},
    {"AL_POSITION", 0x1004}, {"AL_DIRECTION", 0x1005}, {"AL_VELOCITY", 0x1006}, {"AL_LOOPING", 0x1007},
    {"AL_BUFFER", 0x1009}, {"AL_GAIN", 0x100a}, {"AL_MIN_GAIN", 0x100d}, {"AL_MAX_GAIN", 0x100e},
    {"AL_ORIENTATION", 0x100f}, {"AL_SOURCE_STATE", 0x1010}, {"AL_INITIAL", 0x1011}, {"AL_PLAYING", 0x1012},
    {"AL_PAUSED", 0x1013}, {"AL_STOPPED", 0x1014}, {"AL_BUFFERS_QUEUED", 0x1015},
    {"AL_BUFFERS_PROCESSED", 0x1016}, {"AL_REFERENCE_DISTANCE", 0x1020}, {"AL_ROLLOFF_FACTOR", 0x1021},
    {"AL_CONE_OUTER_GAIN", 0x1022}, {"AL_MAX_DISTANCE", 0x1023}, {"AL_SEC_OFFSET", 0x1024},
    {"AL_SAMPLE_OFFSET", 0x1025}, {"AL_BYTE_OFFSET", 0x1026}, {"AL_SOURCE_TYPE", 0x1027},
    {"AL_STATIC", 0x1028}, {"AL_STREAMING", 0x1029}, {"AL_UNDETERMINED", 0x1030},
    {"AL_FORMAT_MONO8", 0x1100}, {"AL_FORMAT_MONO16", 0x1101}, {"AL_FORMAT_STEREO8", 0x1102},
    {"AL_FORMAT_STEREO16", 0x1103}, {"AL_FREQUENCY", 0x2001}, {"AL_BITS", 0x2002}, {"AL_CHANNELS", 0x2003},
    {"AL_SIZE", 0x2004}, {"AL_UNUSED", 0x2010}, {"AL_PENDING", 0x2011}, {"AL_PROCESSED", 0x2012},
    {"AL_NO_ERROR", 0}, {"AL_INVALID_NAME", 0xa001}, {"AL_INVALID_ENUM", 0xa002},
    {"AL_INVALID_VALUE", 0xa003}, {"AL_INVALID_OPERATION", 0xa004}, {"AL_OUT_OF_MEMORY", 0xa005},
    {"AL_VENDOR", 0xb001}, {"AL_VERSION", 0xb002}, {"AL_RENDERER", 0xb003}, {"AL_EXTENSIONS", 0xb004},
    {"AL_DOPPLER_FACTOR", 0xc000}, {"AL_DOPPLER_VELOCITY", 0xc001}, {"AL_SPEED_OF_SOUND", 0xc003},
    {"AL_DISTANCE_MODEL", 0xd000}, {"AL_INVERSE_DISTANCE", 0xd001}, {"AL_INVERSE_DISTANCE_CLAMPED", 0xd002},
    {"AL_LINEAR_DISTANCE", 0xd003}, {"AL_LINEAR_DISTANCE_CLAMPED", 0xd004},
    {"AL_EXPONENT_DISTANCE", 0xd005}, {"AL_EXPONENT_DISTANCE_CLAMPED", 0xd006}};
constexpr std::pair<std::string_view, std::uint32_t> alc_enums[] = {
    {"ALC_FALSE", 0}, {"ALC_TRUE", 1}, {"ALC_FREQUENCY", 0x1007}, {"ALC_REFRESH", 0x1008},
    {"ALC_SYNC", 0x1009}, {"ALC_MONO_SOURCES", 0x1010}, {"ALC_STEREO_SOURCES", 0x1011},
    {"ALC_NO_ERROR", 0}, {"ALC_INVALID_DEVICE", 0xa001}, {"ALC_INVALID_CONTEXT", 0xa002},
    {"ALC_INVALID_ENUM", 0xa003}, {"ALC_INVALID_VALUE", 0xa004}, {"ALC_OUT_OF_MEMORY", 0xa005},
    {"ALC_MAJOR_VERSION", 0x1000}, {"ALC_MINOR_VERSION", 0x1001}, {"ALC_ATTRIBUTES_SIZE", 0x1002},
    {"ALC_ALL_ATTRIBUTES", 0x1003}, {"ALC_DEFAULT_DEVICE_SPECIFIER", 0x1004},
    {"ALC_DEVICE_SPECIFIER", 0x1005}, {"ALC_EXTENSIONS", 0x1006}, {"ALC_CAPTURE_DEVICE_SPECIFIER", 0x310},
    {"ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER", 0x311}, {"ALC_CAPTURE_SAMPLES", 0x312}};

class OpenAl final : public Provider {
public:
    using Provider::Provider;
    std::atomic<std::uint32_t> alc_error{0};
    Address empty_string{}, empty_list{};
    std::map<std::uint32_t, Address> alc_error_strings;
    void set_alc_error(std::uint32_t value) { alc_error.store(value); }

    void install() {
        empty_string = cstring("");
        empty_list = cstring("", true);
        for (const auto& [code, text] : {std::pair{0U, "No Error"}, std::pair{alc_invalid_device, "Invalid Device"},
                 std::pair{alc_invalid_context, "Invalid Context"}, std::pair{alc_invalid_enum, "Invalid Enum"},
                 std::pair{alc_invalid_value, "Invalid Value"}, std::pair{alc_out_of_memory, "Out of Memory"}})
            alc_error_strings.emplace(code, cstring(text));
        const auto lookup = [](const GuestMemory& m, Address name, std::span<const std::pair<std::string_view, std::uint32_t>> table) {
            if (!name) return std::optional<std::uint32_t>{};
            const auto text = guest_text(m, name, 256);
            for (const auto& [key, value] : table) if (key == text) return std::optional<std::uint32_t>{value};
            return std::optional<std::uint32_t>{0};
        };
        function(openal, "_alcOpenDevice", [this](CpuState& s, GuestMemory&) { set_alc_error(alc_invalid_value); s.x[0] = 0; });
        function(openal, "_alcCloseDevice", [this](CpuState& s, GuestMemory&) { set_alc_error(alc_invalid_device); s.x[0] = 0; });
        function(openal, "_alcCreateContext", [this](CpuState& s, GuestMemory&) { set_alc_error(alc_invalid_device); s.x[0] = 0; });
        function(openal, "_alcMakeContextCurrent", [this](CpuState& s, GuestMemory&) {
            if (s.x[0]) { set_alc_error(alc_invalid_context); s.x[0] = 0; } else s.x[0] = 1;
        });
        for (const auto* name : {"_alcDestroyContext", "_alcProcessContext", "_alcSuspendContext"})
            function(openal, name, [this](CpuState& s, GuestMemory&) { set_alc_error(alc_invalid_context); s.x[0] = 0; });
        function(openal, "_alcGetError", [this](CpuState& s, GuestMemory&) {
            s.x[0] = s.x[0] ? alc_invalid_device : alc_error.exchange(0);
        });
        function(openal, "_alcIsExtensionPresent", [this](CpuState& s, GuestMemory&) {
            if (s.x[0]) set_alc_error(alc_invalid_device);
            else if (!s.x[1]) set_alc_error(alc_invalid_value);
            s.x[0] = 0;
        });
        function(openal, "_alcGetEnumValue", [this, lookup](CpuState& s, GuestMemory& m) {
            const auto value = lookup(m, s.x[1], alc_enums);
            if (!value) set_alc_error(alc_invalid_value);
            s.x[0] = value.value_or(0);
        });
        function(openal, "_alcGetString", [this](CpuState& s, GuestMemory&) {
            const auto parameter = static_cast<std::uint32_t>(s.x[1]);
            if (const auto found = alc_error_strings.find(parameter); found != alc_error_strings.end()) { s.x[0] = found->second; return; }
            if (s.x[0]) { set_alc_error(alc_invalid_device); s.x[0] = 0; return; }
            switch (parameter) {
            case alc_device_specifier: case alc_capture_device_specifier: s.x[0] = empty_list; return;
            case alc_default_device_specifier: case alc_capture_default_device_specifier: case alc_extensions:
                s.x[0] = empty_string; return;
            default: set_alc_error(alc_invalid_enum); s.x[0] = 0;
            }
        });
        function(openal, "_alcGetIntegerv", [this](CpuState& s, GuestMemory& m) {
            const auto parameter = static_cast<std::uint32_t>(s.x[1]);
            const auto size = static_cast<std::int32_t>(s.x[2]);
            if (!s.x[3] || size <= 0) { set_alc_error(alc_invalid_value); s.x[0] = 0; return; }
            if (!s.x[0] && (parameter == alc_major_version || parameter == alc_minor_version))
                m.write32(s.x[3], 1); // OpenAL 1.1
            else set_alc_error(alc_invalid_device);
            s.x[0] = 0;
        });
        // AL entry points without a current context have no effect; outputs
        // are left unwritten and alGetError reports AL_INVALID_OPERATION.
        function(openal, "_alGetError", [](CpuState& s, GuestMemory&) { s.x[0] = al_invalid_operation; });
        function(openal, "_alGetEnumValue", [lookup](CpuState& s, GuestMemory& m) { s.x[0] = lookup(m, s.x[0], al_enums).value_or(0); });
        for (const auto* name : {"_alIsExtensionPresent", "_alGetInteger"})
            function(openal, name, [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        function(openal, "_alGetFloat", [](CpuState& s, GuestMemory&) { return_single(s, 0.0F); });
        for (const auto* name : {"_alBufferData", "_alDeleteBuffers", "_alDeleteSources", "_alDistanceModel",
                 "_alDopplerFactor", "_alGenBuffers", "_alGenSources", "_alGetListenerf", "_alGetListenerfv",
                 "_alGetSourcef", "_alGetSourcefv", "_alGetSourcei", "_alListenerf", "_alListenerfv",
                 "_alSourcePause", "_alSourcePlay", "_alSourceQueueBuffers", "_alSourceRewind", "_alSourceStop",
                 "_alSourceUnqueueBuffers", "_alSourcef", "_alSourcefv", "_alSourcei", "_alSpeedOfSound"})
            function(openal, name, [](CpuState&, GuestMemory&) {});
    }
};

} // namespace

void install_openal(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<OpenAl>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
