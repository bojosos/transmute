#pragma once

// Registration helpers for the headless shells of the AppKit, CoreGraphics,
// CoreText, Metal, IOKit and media frameworks. A shell is that framework's
// own answer, in alchemy.exe, for names its GUI provider owns in alchemy-fx:
// <name>_headless.cpp is compiled into alchemy_core only
// (ALCHEMY_HEADLESS_ONLY_SOURCES) and its <name>_fx.cpp twin, in the GUI
// sources, defines the same install function empty, so every name has one
// owner in each build. install_darwin_frameworks (darwin/frameworks.cpp)
// calls the shells after the framework set, as the shell
// code used to run there.

#include "alchemy/core_foundation.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/placeholder_exports.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/registration_audit.hpp"

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace alchemy::darwin_shells {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;

inline double real(const CpuState& state, unsigned index) { return std::bit_cast<double>(state.v[index][0]); }
inline void return_real(CpuState& state, unsigned index, double value) {
    state.v[index] = {std::bit_cast<std::uint64_t>(value), 0};
}
inline std::uint64_t status(std::int32_t value) { return static_cast<std::uint32_t>(value); }

// Each helper takes its caller's location, which the registration audit
// reports as the owner.
struct ShellKit {
    ProcessContext& context;
    ObjcRuntime& objc;
    CoreFoundation& cf;

    GuestMemory& memory() { return context.main_engine(); }

    void function(std::string_view library, std::string symbol, transmute::GuestCallHandler handler,
                  const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(context.install_function(std::move(symbol), std::move(handler), std::string(library)));
    }
    // Explicit failure: the symbol binds, and a call stops the process. A
    // provider that implements the symbol later may replace it.
    void unsupported(std::string_view library, std::string_view symbol, std::string_view why,
                     const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        auto message = "Alchemy: " + std::string(symbol.substr(1)) + " is unsupported in a headless process (" +
            std::string(why) + ")";
        function(library, std::string(symbol), [message = std::move(message)](CpuState&, GuestMemory&) {
            throw std::runtime_error(message);
        });
        mark_placeholder_export(context, library, symbol);
    }
    void unsupported(std::string_view library, std::initializer_list<std::string_view> symbols, std::string_view why,
                     const std::source_location& where = std::source_location::current()) {
        for (const auto symbol : symbols) unsupported(library, symbol, why, where);
    }
    // A data symbol whose storage holds an immortal CFStringRef/NSString*.
    void string_symbol(std::string_view library, std::string symbol, std::string_view value,
                       const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const auto object = cf.string(value);
        if (const auto record = context.objects().find(object)) record->immortal = true;
        const auto slot = context.allocate_guest(8);
        memory().write64(slot, object);
        context.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
    }
    void data_symbol(std::string_view library, std::string symbol, std::span<const std::uint8_t> bytes,
                     const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        const auto size = std::max<std::size_t>(bytes.size(), 8);
        const auto slot = context.allocate_guest(size);
        memory().fill_bytes(slot, 0, size);
        memory().write_bytes(slot, bytes);
        context.install_export(std::string(library), std::move(symbol), {slot, ExportKind::data});
    }
    Address guest_call(std::string_view library, std::string_view symbol, std::initializer_list<Address> arguments) const {
        const auto found = context.resolve(library, symbol);
        if (!found) throw std::runtime_error("Alchemy headless shell: missing " + std::string(symbol));
        return EngineAdapter(context.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
    }
};

} // namespace alchemy::darwin_shells
