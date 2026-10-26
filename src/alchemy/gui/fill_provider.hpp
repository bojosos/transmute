#pragma once

// Gap-filling provider helper for the strict-mode framework providers
// (src/alchemy/media, devices, printing, machsys, fwgaps). Those installers run
// after every other GUI installer (include/alchemy/gui/install.hpp) and only
// add exports that no provider has yet, so the images Qt WebEngine links
// (a renderer plug-in's server) bind without ALCHEMY_PROBE_MISSING.
//
// Every export says what it does honestly:
// - real: the documented behaviour (CMTime math, vDSP kernels, MIDI packet
//   lists, CFSet, launchd data objects, ...);
// - an explicit failure value that the API documents (an OSStatus, NULL, nil,
//   an empty device list, KERN_NOT_SUPPORTED, ...), chosen per API;
// - an explicit stop (`unsupported`): calling it ends the process with a
//   message naming the symbol. Used only where the API has no failure value.
// No export reports success for work it did not do.
//
// The registration audit (alchemy/registration_audit.hpp) lists every name
// these helpers register (fill) or leave to another owner (fill skipped);
// each helper takes its caller's source location for that.
//
// The helper is built on the Security provider's process bridge (guest calls,
// blocks, NSError, CFGetTypeID registration), which install_security creates.

#include "alchemy/call_regs.hpp"
#include "alchemy/core_foundation.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/placeholder_exports.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/registration_audit.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::gui::fill {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;

// Framework identities (install names).
#define ALCHEMY_FRAMEWORK(name, version) "/System/Library/Frameworks/" name ".framework/Versions/" version "/" name
inline constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
inline constexpr std::string_view libobjc = "/usr/lib/libobjc.A.dylib";
inline constexpr std::string_view libbsm = "/usr/lib/libbsm.0.dylib";
inline constexpr std::string_view libpmenergy = "/usr/lib/libpmenergy.dylib";
inline constexpr std::string_view libpmsample = "/usr/lib/libpmsample.dylib";
inline constexpr std::string_view libcups = "/usr/lib/libcups.2.dylib";
inline constexpr std::string_view libncurses = "/usr/lib/libncurses.5.4.dylib";
inline constexpr std::string_view core_foundation = ALCHEMY_FRAMEWORK("CoreFoundation", "A");
inline constexpr std::string_view foundation = ALCHEMY_FRAMEWORK("Foundation", "C");
inline constexpr std::string_view appkit = ALCHEMY_FRAMEWORK("AppKit", "C");
inline constexpr std::string_view core_graphics = ALCHEMY_FRAMEWORK("CoreGraphics", "A");
inline constexpr std::string_view core_text = ALCHEMY_FRAMEWORK("CoreText", "A");
inline constexpr std::string_view core_services = ALCHEMY_FRAMEWORK("CoreServices", "A");
inline constexpr std::string_view application_services = ALCHEMY_FRAMEWORK("ApplicationServices", "A");
inline constexpr std::string_view carbon = ALCHEMY_FRAMEWORK("Carbon", "A");
inline constexpr std::string_view quartz_core = ALCHEMY_FRAMEWORK("QuartzCore", "A");
inline constexpr std::string_view opengl = ALCHEMY_FRAMEWORK("OpenGL", "A");
inline constexpr std::string_view security = ALCHEMY_FRAMEWORK("Security", "A");
inline constexpr std::string_view security_interface = ALCHEMY_FRAMEWORK("SecurityInterface", "A");
inline constexpr std::string_view system_configuration = ALCHEMY_FRAMEWORK("SystemConfiguration", "A");
inline constexpr std::string_view accelerate = ALCHEMY_FRAMEWORK("Accelerate", "A");
inline constexpr std::string_view iokit = ALCHEMY_FRAMEWORK("IOKit", "A");
inline constexpr std::string_view iosurface = ALCHEMY_FRAMEWORK("IOSurface", "A");
inline constexpr std::string_view core_video = ALCHEMY_FRAMEWORK("CoreVideo", "A");
inline constexpr std::string_view core_media = ALCHEMY_FRAMEWORK("CoreMedia", "A");
inline constexpr std::string_view video_toolbox = ALCHEMY_FRAMEWORK("VideoToolbox", "A");
inline constexpr std::string_view av_foundation = ALCHEMY_FRAMEWORK("AVFoundation", "A");
inline constexpr std::string_view audio_toolbox = ALCHEMY_FRAMEWORK("AudioToolbox", "A");
inline constexpr std::string_view audio_unit = ALCHEMY_FRAMEWORK("AudioUnit", "A");
inline constexpr std::string_view core_audio = ALCHEMY_FRAMEWORK("CoreAudio", "A");
inline constexpr std::string_view core_midi = ALCHEMY_FRAMEWORK("CoreMIDI", "A");
inline constexpr std::string_view media_accessibility = ALCHEMY_FRAMEWORK("MediaAccessibility", "A");
inline constexpr std::string_view image_capture_core = ALCHEMY_FRAMEWORK("ImageCaptureCore", "A");
inline constexpr std::string_view core_image = ALCHEMY_FRAMEWORK("CoreImage", "A");
inline constexpr std::string_view io_bluetooth = ALCHEMY_FRAMEWORK("IOBluetooth", "A");
inline constexpr std::string_view core_bluetooth = ALCHEMY_FRAMEWORK("CoreBluetooth", "A");
inline constexpr std::string_view game_controller = ALCHEMY_FRAMEWORK("GameController", "A");
inline constexpr std::string_view force_feedback = ALCHEMY_FRAMEWORK("ForceFeedback", "A");
inline constexpr std::string_view core_wlan = ALCHEMY_FRAMEWORK("CoreWLAN", "A");
inline constexpr std::string_view core_location = ALCHEMY_FRAMEWORK("CoreLocation", "A");
inline constexpr std::string_view local_authentication = ALCHEMY_FRAMEWORK("LocalAuthentication", "A");
inline constexpr std::string_view open_directory = ALCHEMY_FRAMEWORK("OpenDirectory", "A");
inline constexpr std::string_view disk_arbitration = ALCHEMY_FRAMEWORK("DiskArbitration", "A");
inline constexpr std::string_view quartz = ALCHEMY_FRAMEWORK("Quartz", "A");
#undef ALCHEMY_FRAMEWORK

// CFGetTypeID values of the CF types these providers define (distinct from
// the Security bridge's 0x400 range and CoreFoundation's 0x100 range).
enum class TypeId : std::uint64_t {
    block_buffer = 0x600, format_description, sample_buffer, cf_set, cf_bag, character_set, uuid, null_type,
    hid_manager, notification_port, power_blob, launch_data, cups_ipp, cg_event, sec_access_control,
    display_stream, cv_pixel_buffer, mach_port, notification_center, pm_object, da_session,
    binary_heap, bit_vector, tree,   // darwin/corefoundation/cf_collections.cpp
    cv_pixel_buffer_pool, vt_pixel_transfer_session,   // darwin/media/
};

inline std::int64_t sign32(std::uint64_t value) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }
inline std::uint64_t status(std::int32_t value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }
inline constexpr std::uint32_t fourcc(const char (&text)[5]) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8) | static_cast<unsigned char>(text[3]);
}
inline std::uint16_t read16(const GuestMemory& m, Address address) {
    return static_cast<std::uint16_t>(m.read8(address) | (m.read8(address + 1) << 8));
}
inline void write16(GuestMemory& m, Address address, std::uint16_t value) {
    m.write8(address, static_cast<std::uint8_t>(value));
    m.write8(address + 1, static_cast<std::uint8_t>(value >> 8));
}
inline double real(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }
inline float real32(const CpuState& s, unsigned index) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[index][0])); }
inline void return_real(CpuState& s, double value, unsigned index = 0) { s.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; }
inline void return_real32(CpuState& s, float value, unsigned index = 0) { s.v[index] = {std::bit_cast<std::uint32_t>(value), 0}; }

class Provider : public std::enable_shared_from_this<Provider> {
public:
    explicit Provider(InstallContext& context)
        : process(context.process), objc(context.objc), cf(context.cf), bridge(security::bridge()) {}
    virtual ~Provider() = default;

    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;
    std::shared_ptr<security::Bridge> bridge;

    GuestMemory& memory() const { return process.calling_engine(); }

    // Whether a provider already exports `name` for real (a placeholder counts
    // as missing: the caller replaces it).
    bool has(std::string_view library, std::string_view name) const { return bridge->exported(library, name); }

    // Raw handler (CpuState in, CpuState out); installed only when missing.
    // Handlers share ownership of the provider.
    void raw(std::string_view library, std::string_view name, transmute::GuestCallHandler handler,
             const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const bool skipped = has(library, name);
        registration_audit::record_fill(library, name, skipped, where);
        if (skipped) return;
        bridge->function(library, name, [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) {
            handler(s, m);
        });
    }
    // Handler returning x0.
    template <class F>
    void fn(std::string_view library, std::string_view name, F f, const std::source_location& where = std::source_location::current()) {
        raw(library, name, [f = std::move(f)](CpuState& s, GuestMemory& m) { s.x[0] = f(s, m); }, where);
    }
    // An OSStatus / kern_return_t / int result `code` (sign-extended). The
    // pointer arguments listed in `clear` are output parameters set to 0
    // (8 bytes) when non-null, as the API leaves them on failure.
    void failure(std::string_view library, std::string_view name, std::int32_t code, std::initializer_list<unsigned> clear = {},
                 const std::source_location& where = std::source_location::current()) {
        std::vector<unsigned> outputs(clear);
        raw(library, name, [code, outputs](CpuState& s, GuestMemory& m) {
            for (const auto index : outputs) {
                const auto out = index < 8 ? s.x[index] : m.read64(s.sp + (index - 8) * 8ULL);
                if (out) m.write64(out, 0);
            }
            s.x[0] = status(code);
        }, where);
    }
    // A constant integer/pointer result (NULL, false, 0 devices, ...).
    void constant(std::string_view library, std::string_view name, std::uint64_t value,
                  const std::source_location& where = std::source_location::current()) {
        raw(library, name, [value](CpuState& s, GuestMemory&) { s.x[0] = value; }, where);
    }
    // A constant double result in d0.
    void constant_real(std::string_view library, std::string_view name, double value,
                       const std::source_location& where = std::source_location::current()) {
        raw(library, name, [value](CpuState& s, GuestMemory&) { return_real(s, value); }, where);
    }
    // `const CFStringRef name` holding `value`.
    void string(std::string_view library, std::string_view name, std::string_view value,
                const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const bool skipped = has(library, name);
        registration_audit::record_fill(library, name, skipped, where);
        if (!skipped) static_cast<void>(bridge->export_string(library, name, value));
    }
    // `extern const <8-byte value> name`.
    void value(std::string_view library, std::string_view name, std::uint64_t word,
               const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const bool skipped = has(library, name);
        registration_audit::record_fill(library, name, skipped, where);
        if (!skipped) bridge->export_value(library, name, word);
    }
    // A data export holding `bytes` (the symbol is the bytes themselves).
    Address data(std::string_view library, std::string_view name, std::span<const std::uint8_t> bytes, std::uint64_t alignment = 16,
                 const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        if (const auto found = process.resolve(library, name); found && !take_placeholder_export(process, library, name)) {
            registration_audit::record_fill(library, name, true, where);
            return found->address;
        }
        registration_audit::record_fill(library, name, false, where);
        const auto address = process.allocate_guest(bytes.empty() ? 1 : bytes.size(), alignment);
        if (!bytes.empty()) process.main_engine().write_bytes(address, bytes);
        process.install_export(std::string(library), std::string(name), {address, ExportKind::data});
        return address;
    }
    // A C string constant in guest memory (immortal).
    Address cstring(std::string_view text) {
        const auto address = process.allocate_guest(text.size() + 1, 1);
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        bytes.push_back(0);
        process.main_engine().write_bytes(address, bytes);
        return address;
    }
    // Objective-C class `name` with superclass `superclass` (which must exist),
    // published under `library` when not exported yet. Returns the class.
    Address cls(std::string_view library, std::string_view name, std::string_view superclass,
                const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        auto existing = objc.find_class(name);
        if (!existing) {
            const auto parent = objc.find_class(superclass);
            if (!parent) throw std::runtime_error("Alchemy: superclass " + std::string(superclass) + " of " + std::string(name) + " is missing");
            existing = objc.register_native_class(std::string(name), parent, 8);
            created_.insert(existing);
        }
        const std::string class_symbol = "_OBJC_CLASS_$_" + std::string(name);
        if (created_.contains(existing)) {
            // register_native_class publishes a new class under libobjc and
            // Foundation itself: that is this registration, not a skip.
            const ProcessContext::RuntimeLookup lookup;
            if (const auto bound = process.resolve(library, class_symbol); bound && bound->address == existing) return existing;
        }
        const bool skipped = has(library, class_symbol);
        registration_audit::record_fill(library, class_symbol, skipped, where);
        if (!skipped) {
            process.install_export(std::string(library), class_symbol, {existing, ExportKind::data});
            process.install_export(std::string(library), "_OBJC_METACLASS_$_" + std::string(name),
                {process.main_engine().read64(existing), ExportKind::data});
        }
        return existing;
    }
    // A host method on `target`. Added only to classes this provider created
    // (cls() above), so a class another provider implements is never shadowed.
    void method(Address target, std::string_view selector, transmute::GuestCallHandler handler,
                bool class_method = false, std::string types = "@@:",
                const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        if (!created_.contains(target)) return;
        static_cast<void>(objc.add_native_method(target, std::string(selector),
            [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
            class_method, std::move(types)));
    }
    // Explicit stop: binding works, a call ends the process with a message.
    void unsupported(std::string_view library, std::string_view name, std::string_view why,
                     const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const bool skipped = has(library, name);
        registration_audit::record_fill(library, name, skipped, where);
        if (skipped) return;
        auto message = "Alchemy: " + std::string(name.substr(1)) + " is unsupported (" + std::string(why) + ")";
        bridge->function(library, name, [message = std::move(message)](CpuState&, GuestMemory&) {
            throw std::runtime_error(message);
        });
        mark_placeholder_export(process, library, name);
    }
    // A one-time stderr notice (for a void call whose effect Alchemy does not
    // produce, such as drawing a focus ring).
    void notice_once(const std::string& text) {
        std::lock_guard lock(notice_mutex_);
        if (notices_.insert(text).second) std::cerr << "alchemy: " << text << std::endl;
    }
    // Calls an exported guest function.
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments) {
        return bridge->call(library, name, arguments);
    }
    Address symbol(std::string_view library, std::string_view name) const { return bridge->symbol(library, name); }
    // A call with the full register image (floating-point or stack arguments, x8 results).
    CallResult call_regs(std::string_view library, std::string_view name, const CallRegs& registers) {
        return EngineAdapter(process.calling_engine()).call_regs(symbol(library, name), registers);
    }
    void register_type(Address target, TypeId id) { bridge->register_type_id(target, static_cast<security::TypeId>(id)); }
    void set_errno(std::uint32_t value) { process.set_errno(value); }

    // Guest memory helpers.
    std::string read_cstring(GuestMemory& m, Address address, std::size_t maximum = 1 << 16) const {
        if (!address) return {};
        bool terminated{};
        const auto bytes = m.read_string(address, 1, maximum, &terminated);
        return {bytes.begin(), bytes.end()};
    }
    Address malloc(std::uint64_t size) {
        const auto address = call(libsystem, "_malloc", {size ? size : 1});
        if (!address) throw std::bad_alloc();
        return address;
    }
    void free(Address address) { if (address) static_cast<void>(call(libsystem, "_free", {address})); }
    Address strdup(std::string_view text) {
        const auto address = malloc(text.size() + 1);
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        bytes.push_back(0);
        memory().write_bytes(address, bytes);
        return address;
    }
    // CFRetain/CFRelease through the guest exports (any CF or Objective-C object).
    Address retain(Address object) { return object ? call(core_foundation, "_CFRetain", {object}) : 0; }
    void release(Address object) { if (object) static_cast<void>(call(core_foundation, "_CFRelease", {object})); }

    bool created(Address target) const { return created_.contains(target); }

private:
    std::set<Address> created_;
    std::mutex notice_mutex_;
    std::set<std::string> notices_;
};

// Installers of the strict-mode providers (each fills gaps only).
void install_machsys(InstallContext& context);
void install_fwgaps(InstallContext& context);
void install_media(InstallContext& context);
void install_devices(InstallContext& context);
void install_printing(InstallContext& context);
void install_libsystem_leftovers(InstallContext& context);   // src/alchemy/darwin/libsystem/misc/leftovers.cpp

} // namespace alchemy::gui::fill
