#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin/openal/openal.hpp"

#include <atomic>
#include <map>
#include <optional>
#include <span>

// OpenAL 1.1 with no output or capture device (alchemy.exe: no audio output;
// alchemy-fx has the real one, mixer.cpp, and openal_fx.cpp is this file's
// empty twin there). No device or context can be created, so every handle a
// caller passes is invalid and AL calls have no current context.
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
        const auto lookup = [](const GuestMemory& m, Address name) {
            if (!name) return std::optional<std::uint32_t>{};
            return std::optional<std::uint32_t>{openal::enum_value(guest_text(m, name, 256), false)};
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
            const auto value = lookup(m, s.x[1]);
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
        function(openal, "_alGetEnumValue", [lookup](CpuState& s, GuestMemory& m) { s.x[0] = lookup(m, s.x[0]).value_or(0); });
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
