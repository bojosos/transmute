#pragma once

// Registration helpers of the frameworks' headless additions
// (darwin/headless_additions.hpp): answers that every build registers when
// the process starts (install_darwin_frameworks), and that a GUI provider of
// alchemy-fx may register over. Each framework's piece is a small Provider
// subclass in its directory; handlers share ownership of their provider, so a
// piece may keep state. Each helper takes its caller's location, which the
// registration audit reports as the owner. (The pieces were
// darwin/identities/installer_frameworks.cpp, the symbols an
// installer binds; its command-line mode links the whole wxOSX GUI.)

#include "alchemy/call_regs.hpp"
#include "alchemy/core_foundation.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/placeholder_exports.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/registration_audit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace alchemy::darwin_headless {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;

inline constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
inline constexpr std::string_view objc_library = "/usr/lib/libobjc.A.dylib";
inline constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
inline constexpr std::string_view cocoa = "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa";
inline constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
inline constexpr std::string_view core_graphics = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
inline constexpr std::string_view core_text = "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText";
inline constexpr std::string_view image_io = "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO";
inline constexpr std::string_view application_services =
    "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";
inline constexpr std::string_view carbon = "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon";
inline constexpr std::string_view core_services = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
inline constexpr std::string_view quartz_core = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";
inline constexpr std::string_view security = "/System/Library/Frameworks/Security.framework/Versions/A/Security";

inline double real(const CpuState& state, unsigned index) { return std::bit_cast<double>(state.v[index][0]); }
inline std::uint64_t status(std::int32_t value) { return static_cast<std::uint32_t>(value); }
inline std::string guest_text(const GuestMemory& memory, Address address, std::size_t maximum = 4096) {
    bool terminated{};
    const auto bytes = memory.read_string(address, 1, maximum, &terminated);
    if (!terminated) throw std::runtime_error("Alchemy frameworks: guest string exceeds " + std::to_string(maximum) + " bytes");
    return {bytes.begin(), bytes.end()};
}
inline float single(const CpuState& state, unsigned index) { return std::bit_cast<float>(static_cast<std::uint32_t>(state.v[index][0])); }
inline void return_single(CpuState& state, float value) { state.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; }
inline void return_real(CpuState& state, double value) { state.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; }
inline void return_real(CpuState& state, unsigned index, double value) { state.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; }

// Kinds of export, labelled where each is installed:
// - headless-correct: the full documented behaviour for a process with no
//   display, printer or pasteboard server;
// - class shells: Objective-C classes with their real superclass and no
//   methods of their own, so guest subclasses link; any message they do not
//   inherit fails with the runtime's explicit "unsupported message" error;
// - explicit failures: calling one stops the process with "unsupported in a
//   headless process" naming the symbol. They exist only so programs bind;
//   they never count as support.
struct Provider : std::enable_shared_from_this<Provider> {
    ProcessContext& context;
    ObjcRuntime& objc;
    CoreFoundation& cf;

    Provider(ProcessContext& process, ObjcRuntime& runtime, CoreFoundation& foundation_)
        : context(process), objc(runtime), cf(foundation_) {}
    GuestMemory& memory() { return context.main_engine(); }

    // The registration helpers take their caller's location for the
    // registration audit.
    void function(std::string_view library, std::string symbol, transmute::GuestCallHandler handler,
                  const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(context.install_function(std::move(symbol),
            [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
            std::string(library)));
    }
    // Explicit failure: the symbol binds, and a call stops the process.
    void unsupported(std::string_view library, std::string_view symbol, std::string_view why,
                     const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        auto message = "Alchemy: " + std::string(symbol.substr(1)) + " is unsupported in a headless process (" +
            std::string(why) + ")";
        function(library, std::string(symbol), [message = std::move(message)](CpuState&, GuestMemory&) {
            throw std::runtime_error(message);
        });
        // A provider that implements the symbol later may replace this one.
        mark_placeholder_export(context, library, symbol);
    }
    Address guest_call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments) {
        const auto found = context.resolve(library, symbol);
        if (!found) throw std::runtime_error("Alchemy headless frameworks: missing " + std::string(symbol));
        return EngineAdapter(context.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
    }
    Address constant_string(std::string_view text) {
        const auto object = cf.string(text);
        if (const auto record = context.objects().find(object)) record->immortal = true;
        return object;
    }
    void string_symbol(std::string_view library, std::string symbol, std::string_view value,
                       const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const auto slot = context.allocate_guest(8);
        memory().write64(slot, constant_string(value));
        context.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
    }
    void data_symbol(std::string_view library, std::string symbol, std::span<const std::uint8_t> bytes,
                     const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const auto slot = context.allocate_guest(std::max<std::size_t>(bytes.size(), 8));
        memory().fill_bytes(slot, 0, std::max<std::size_t>(bytes.size(), 8));
        memory().write_bytes(slot, bytes);
        context.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
    }
    // Class shell: the class with its real superclass, exported under each
    // library. An existing class (from another provider) is only exported.
    Address class_shell(std::initializer_list<std::string_view> libraries, std::string_view name,
                        std::string_view superclass, const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        auto cls = objc.find_class(name);
        if (!cls) {
            const auto parent = objc.find_class(superclass);
            if (!parent) throw std::runtime_error("Alchemy headless frameworks: missing superclass " + std::string(superclass));
            cls = objc.register_native_class(std::string(name), parent, 8);
        }
        for (const auto library : libraries) objc.publish_class(name, cls, library);
        return cls;
    }
    // Adds a method to an existing provider class. GUI providers installed
    // later replace it by adding the same selector again.
    void method(std::string_view class_name, std::string selector, transmute::GuestCallHandler handler,
                bool class_method = false, std::string types = "@@:", const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const auto cls = objc.find_class(class_name);
        if (!cls) throw std::runtime_error("Alchemy headless frameworks: missing class " + std::string(class_name));
        static_cast<void>(objc.add_native_method(cls, std::move(selector),
            [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
            class_method, std::move(types)));
    }

    // Strings, zeroed data, single-library classes and CF arrays.
    Address cstring(std::string_view text, bool list = false) {
        const auto size = text.size() + (list ? 2 : 1);
        const auto address = context.allocate_guest(size);
        memory().write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        memory().fill_bytes(address + text.size(), 0, size - text.size());
        return address;
    }
    void zero_symbol(std::string_view library, std::string symbol, std::uint64_t size,
                     const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const auto slot = context.allocate_guest(size);
        memory().fill_bytes(slot, 0, size);
        context.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
    }
    Address declare_class(std::string_view library, std::string_view name, std::string_view superclass,
                          const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        auto cls = objc.find_class(name);
        if (!cls) {
            const auto parent = objc.find_class(superclass);
            if (!parent) throw std::runtime_error("Alchemy frameworks: missing superclass " + std::string(superclass));
            cls = objc.register_native_class(std::string(name), parent, 8);
        }
        objc.publish_class(name, cls, library);
        return cls;
    }
    Address callbacks(std::string_view name) {
        const auto found = context.resolve(core_foundation_library, name);
        if (!found) throw std::runtime_error("Alchemy frameworks: CoreFoundation lacks " + std::string(name));
        return found->address;
    }
    Address empty_array() {
        const std::array<std::uint64_t, 4> arguments{0, 0, 0, callbacks("_kCFTypeArrayCallBacks")};
        return cf.invoke("CFArrayCreate", arguments);
    }

    // Messages and retains through the guest runtime.
    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        CallRegs registers;
        unsigned index = 2;
        for (const auto argument : arguments) registers.x.at(index++) = argument;
        return objc.send(receiver, selector, std::move(registers)).x0;
    }
    Address class_named(std::string_view name) {
        const auto cls = objc.find_class(name);
        if (!cls) throw std::runtime_error("Alchemy headless frameworks: missing class " + std::string(name));
        return cls;
    }
    Address retained(Address object) { return object ? guest_call(objc_library, "_objc_retain", {object}) : 0; }
    Address retain(Address object) { return object ? guest_call(objc_library, "_objc_retain", {object}) : 0; }
    void release(Address object) { if (object) static_cast<void>(guest_call(objc_library, "_objc_release", {object})); }
};

} // namespace alchemy::darwin_headless
