#pragma once

// Internal interfaces of the fx ABI component (owner: abi).

#include "fx/abi/signature.hpp"
#include "transmute/fx/context.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace transmute::fx::abi {

// ---- signature database ------------------------------------------------------

// Signature text for `name` from the built-in corrections or the generated
// table; nullptr if the function is not in the database.
const char* lookup_signature_text(std::string_view name) noexcept;
// Interned signature for `name`; unknown_signature() if absent.
const Signature* lookup_signature(std::string_view name);
std::size_t generated_signature_count() noexcept;
// GL/WGL entry points bound by the wglGetProcAddress override (for tests).
std::size_t opengl_signature_count() noexcept;
const char* opengl_signature_name(std::size_t i) noexcept;

// ---- overrides ---------------------------------------------------------------

struct OverrideBinding {
    std::string dll;        // lower-case file name as bound
    std::string name;
    const void* native;     // host implementation (may be nullptr)
    const Signature* sig;   // database signature (unknown_signature() if none)
};

enum class OverrideResult : std::uint32_t {
    return_to_caller = 0,   // results are in ctx; return to the guest x30
    resume = 1,             // ctx (including pc) was replaced; resume at ctx.pc
};

// A guest-aware implementation of an import. Arguments are in ctx (x0-x7,
// v0-v7, guest stack at ctx.x[31]); ctx.pc holds the guest return address.
using OverrideHandler = OverrideResult (*)(GuestContext& ctx, const OverrideBinding& binding);

OverrideHandler find_override(std::string_view dll, std::string_view name) noexcept;
std::size_t override_count() noexcept;

// Reads guest stack argument `index` (0 = first stack slot).
inline std::uint64_t guest_stack_arg(const GuestContext& ctx, unsigned index) {
    return *reinterpret_cast<const std::uint64_t*>(ctx.x[31] + 8ull * index);
}

// ---- code generation -----------------------------------------------------------

struct ThunkCode {
    std::vector<std::uint8_t> bytes;
    std::size_t native_imm{SIZE_MAX};   // offset of the imm64 holding the native target
    // Host frame: `frame_bytes` below the entry rsp from `prolog_bytes` up to
    // `epilog_offset` (see abi_code_frame).
    std::uint32_t prolog_bytes{};
    std::uint32_t frame_bytes{};
    std::size_t epilog_offset{};
};

// ---- diagnostics (TRANSMUTE_FX_TRACE_IMPORTS / TRANSMUTE_FX_TRACE_EXIT) ---------

enum class TraceMode : std::uint8_t { off, all, first };
// Read once from TRANSMUTE_FX_TRACE_IMPORTS ("1" = every call, "first" =
// first call of each import). Thunks bound while it is off carry no tracing
// code at all.
TraceMode import_trace_mode() noexcept;
bool exit_trace_enabled() noexcept;   // TRANSMUTE_FX_TRACE_EXIT=1

struct TraceInfo {
    std::string dll;
    std::string name;
    bool seen{};        // "first" mode
};
// Called by traced thunks on entry: "fx call <dll>!<name> from <module>+0x<off>".
void trace_import_call(TraceInfo* info, std::uint64_t caller) noexcept;
// "<module>+0x<offset>" for guest addresses, "0x<address>" otherwise.
std::string describe_guest_address(std::uint64_t address);
// Callback trace line (import tracing on): "fx callback <guest fn> <sig>".
void trace_callback(std::uint64_t guest_function, const char* sig, bool had_context) noexcept;
// Exit trace line (when enabled): "fx exit: <api> code=<n> from <caller>".
void trace_exit(const char* api, std::uint32_t code, std::uint64_t caller) noexcept;

// Guest-callable thunk (flushed entry state, returns to x30) calling `native`
// with the Win64 convention described by `sig` (or positional for "?").
// `trace` != nullptr adds a trace_import_call on entry.
ThunkCode generate_host_thunk(const Signature& sig, const void* native, TraceInfo* trace = nullptr);
// Guest-callable thunk that runs `handler(ctx, *binding)`.
std::vector<std::uint8_t> generate_override_thunk(OverrideHandler handler, const OverrideBinding* binding,
                                                  TraceInfo* trace = nullptr);

struct CallbackInfo;
// Win64-callable trampoline forwarding to callback_dispatch(info, ...).
std::vector<std::uint8_t> generate_callback_trampoline(const CallbackInfo* info, std::uint32_t* prolog_bytes = nullptr,
                                                       std::uint32_t* frame_bytes = nullptr);

// ---- runtime glue used by generated code ----------------------------------------

// Function-pointer argument of a host call: guest function -> host-callable
// trampoline, plain abi thunk -> its native target, anything else unchanged.
std::uint64_t wrap_callback_argument(std::uint64_t value, const Signature* sig);
// Function-pointer result of a host call: our trampoline -> its guest
// function, host code -> guest-callable thunk, anything else unchanged.
std::uint64_t unwrap_callback_result(std::uint64_t value, const Signature* sig);

struct CallbackInfo {
    std::uint64_t guest_function;
    const Signature* sig;           // never null ("?" for unknown)
    CallPlan plan;
    const void* trampoline{};
};

// Entered from a callback trampoline: `slots` = Win64 argument slots (home
// area followed by stack arguments), `xmm` = xmm0..xmm3 low halves (xmm[0]
// receives the floating-point result).
std::uint64_t callback_dispatch(const CallbackInfo* info, std::uint64_t* slots, std::uint64_t* xmm);

// ---- code memory and thunk registry ---------------------------------------------

// Copies `bytes` into code memory obtained from allocate_code(); returns the address.
std::uint8_t* emit_code(const std::vector<std::uint8_t>& bytes);

struct ThunkRecord {
    std::uint64_t address{};
    const void* native{};           // host target (plain thunks)
    const Signature* sig{};
    bool is_override{};
    std::string name;
};

// Registry lookups (thread-safe).
const ThunkRecord* find_thunk(std::uint64_t address) noexcept;
// Guest function of a host_callable trampoline, or 0.
std::uint64_t trampoline_guest_function(std::uint64_t address) noexcept;

// Guest-callable thunk for a host function pointer of signature `sig`
// (cached per (address, sig)).
std::uint64_t bind_host_pointer(const void* host_address, const Signature* sig);

// Name of the export at `host_address` in the host module that contains it,
// or empty (used to type unknown host targets and ordinal imports).
std::string host_export_name(const void* host_address);

// Guest processor identity (consistent across the processor overrides).
bool guest_processor_feature(std::uint32_t feature) noexcept;

// Synchronises ctx.fpcr (RMode, FZ, trap enables) with the host MXCSR after
// a host call that may have changed the floating-point environment, and
// keeps MXCSR exceptions masked.
void sync_fpcr_from_mxcsr(GuestContext& ctx) noexcept;

} // namespace transmute::fx::abi
