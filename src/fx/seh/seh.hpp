#pragma once

// fx SEH component internals (owner: seh). Shared by the seh sources and the
// seh tests. Other components use include/transmute/fx/process.hpp.
//
// ARM64 structures use the winnt.h layouts (ARM64_NT_CONTEXT,
// DISPATCHER_CONTEXT_ARM64, IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY), which the
// SDK defines for every target architecture.

#include "transmute/fx/process.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace transmute::fx::seh {

using Arm64Context = ARM64_NT_CONTEXT;
using Arm64RuntimeFunction = IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY;
using Arm64DispatcherContext = DISPATCHER_CONTEXT_ARM64;
using Arm64NonvolatileRegisters = DISPATCHER_CONTEXT_NONVOLREG_ARM64;
using Arm64ContextPointers = KNONVOLATILE_CONTEXT_POINTERS_ARM64;

static_assert(sizeof(Arm64Context) == 0x390);
static_assert(sizeof(Arm64DispatcherContext) == 0x58);
static_assert(sizeof(Arm64NonvolatileRegisters) == 0x98);
static_assert(sizeof(Arm64RuntimeFunction) == 8);
static_assert(sizeof(EXCEPTION_RECORD) == 0x98);

inline constexpr DWORD kCtxArm64 = 0x00400000u;
inline constexpr DWORD kCtxControl = kCtxArm64 | 0x1u;
inline constexpr DWORD kCtxInteger = kCtxArm64 | 0x2u;
inline constexpr DWORD kCtxFloatingPoint = kCtxArm64 | 0x4u;
inline constexpr DWORD kCtxFull = kCtxControl | kCtxInteger | kCtxFloatingPoint;
inline constexpr DWORD kCtxUnwoundToCall = 0x20000000u;

inline constexpr std::uint32_t kStatusUnwind = 0xC0000027u;
inline constexpr std::uint32_t kStatusBadStack = 0xC0000028u;
inline constexpr std::uint32_t kStatusInvalidUnwindTarget = 0xC0000029u;
inline constexpr std::uint32_t kStatusNoncontinuable = 0xC0000025u;
inline constexpr std::uint32_t kStatusInvalidDisposition = 0xC0000026u;
inline constexpr std::uint32_t kStatusLongjump = 0x80000026u;
inline constexpr std::uint32_t kStatusUnwindConsolidate = 0x80000029u;
inline constexpr std::uint32_t kStatusBreakpoint = 0x80000003u;
inline constexpr std::uint32_t kStatusAccessViolation = 0xC0000005u;
inline constexpr std::uint32_t kStatusInPageError = 0xC0000006u;
inline constexpr std::uint32_t kStatusIllegalInstruction = 0xC000001Du;
inline constexpr std::uint32_t kStatusStackOverflow = 0xC00000FDu;
inline constexpr std::uint32_t kStatusIntegerDivideByZero = 0xC0000094u;
inline constexpr std::uint32_t kStatusIntegerOverflow = 0xC0000095u;
inline constexpr std::uint32_t kStatusAssertionFailure = 0xC0000420u;
inline constexpr std::uint32_t kStatusStackBufferOverrun = 0xC0000409u;
inline constexpr std::uint32_t kStatusDatatypeMisalignment = 0x80000002u;
inline constexpr std::uint32_t kStatusGuardPageViolation = 0x80000001u;
inline constexpr std::uint32_t kStatusPrivilegedInstruction = 0xC0000096u;

inline constexpr std::uint32_t kFlagNoncontinuable = 0x01;
inline constexpr std::uint32_t kFlagUnwinding = 0x02;
inline constexpr std::uint32_t kFlagExitUnwind = 0x04;
inline constexpr std::uint32_t kFlagStackInvalid = 0x08;
inline constexpr std::uint32_t kFlagNestedCall = 0x10;
inline constexpr std::uint32_t kFlagTargetUnwind = 0x20;
inline constexpr std::uint32_t kFlagCollidedUnwind = 0x40;
inline constexpr std::uint32_t kFlagUnwindMask = 0x66;

enum Disposition : std::uint32_t {
    disposition_continue_execution = 0,
    disposition_continue_search = 1,
    disposition_nested_exception = 2,
    disposition_collided_unwind = 3,
};

inline constexpr std::uint32_t kHandlerTypeNone = 0;
inline constexpr std::uint32_t kHandlerTypeException = 1;  // UNW_FLAG_EHANDLER
inline constexpr std::uint32_t kHandlerTypeUnwind = 2;     // UNW_FLAG_UHANDLER

// ---------------------------------------------------------------------------
// Guarded guest memory access (guest VA == host VA). Return false instead of
// faulting on unmapped or protected memory.
// ---------------------------------------------------------------------------
bool read_guest(std::uint64_t address, void* out, std::size_t size) noexcept;
bool write_guest(std::uint64_t address, const void* data, std::size_t size) noexcept;
template <typename T>
bool read_guest_value(std::uint64_t address, T& out) noexcept {
    return read_guest(address, &out, sizeof(T));
}

// ---------------------------------------------------------------------------
// Unwind information
// ---------------------------------------------------------------------------

enum class UnwindStatus : std::uint32_t {
    ok = 0,
    unreadable,          // unwind data or stack memory could not be read
    bad_version,         // .xdata version != 0
    truncated,           // record runs past its code words
    bad_opcode,          // reserved or malformed unwind code
    unsupported_opcode,  // valid but unsupported here (SVE, trap frame, EC context)
    bad_packed,          // packed form that has no canonical prolog (Flag 3, ...)
    bad_epilog,          // epilog scope outside the function or its codes
};
const char* unwind_status_name(UnwindStatus status) noexcept;

inline constexpr std::size_t kMaxUnwindCodeBytes = 1024;
inline constexpr std::size_t kMaxSynthEpilogs = 1;

struct EpilogScope {
    std::uint32_t start;   // instruction index from the function start
    std::uint32_t index;   // byte index into the unwind codes
};

// Normalized description of one RUNTIME_FUNCTION. Packed entries are expanded
// into equivalent unwind codes so both forms share one interpreter.
struct FunctionUnwindInfo {
    std::uint32_t begin_rva{};
    std::uint32_t length{};           // instructions
    bool packed{};
    std::uint8_t packed_flag{};       // 1 or 2 for packed entries
    std::uint32_t code_size{};        // bytes in codes[]
    std::uint8_t codes[kMaxUnwindCodeBytes]{};
    std::uint32_t epilog_count{};
    std::uint64_t epilog_scopes_address{};  // guest address of xdata scope words (0 if synthesized)
    EpilogScope synthesized_epilog{};       // used when epilog_scopes_address == 0 && epilog_count == 1
    bool has_handler{};
    std::uint32_t handler_rva{};
    std::uint64_t handler_data{};     // absolute address of the language-specific data
    std::uint32_t xdata_size{};       // bytes before the handler data (0 for packed)
};

// Decodes the RUNTIME_FUNCTION `entry` of an image based at `image_base`.
UnwindStatus describe_function(std::uint64_t image_base, const Arm64RuntimeFunction& entry,
                               FunctionUnwindInfo& out) noexcept;

// Validates every unwind code of `info` (opcode lengths, register ranges,
// epilog scopes). Used by the corpus parse tests and RtlAddFunctionTable.
UnwindStatus validate_function(const FunctionUnwindInfo& info) noexcept;

// Function length in bytes (packed or xdata form). 0 if unreadable.
std::uint32_t function_length_bytes(std::uint64_t image_base, const Arm64RuntimeFunction& entry) noexcept;

// Size in bytes of the unwind code starting with `first_byte`.
unsigned unwind_code_size(std::uint8_t first_byte) noexcept;

struct VirtualUnwindResult {
    UnwindStatus status = UnwindStatus::ok;
    std::uint64_t handler = 0;        // language handler (absolute) or 0
    std::uint64_t handler_data = 0;   // absolute address of handler data or 0
    std::uint64_t establisher_frame = 0;
    bool in_prolog = false;
    bool in_epilog = false;
};

// RtlVirtualUnwind semantics. `context` holds the state at `control_pc`
// inside the function described by `entry`; on return it holds the caller's
// state (Pc = return address, CONTEXT_UNWOUND_TO_CALL set unless a custom
// frame code supplied the PC). The handler is reported only when the PC is
// in the body and the handler type matches `handler_type` (0 = any).
VirtualUnwindResult virtual_unwind(std::uint32_t handler_type, std::uint64_t image_base,
                                   std::uint64_t control_pc, const Arm64RuntimeFunction& entry,
                                   Arm64Context& context, Arm64ContextPointers* pointers) noexcept;

// Same with an already decoded description (saves re-parsing).
VirtualUnwindResult virtual_unwind_info(std::uint32_t handler_type, std::uint64_t image_base,
                                        std::uint64_t control_pc, const FunctionUnwindInfo& info,
                                        Arm64Context& context, Arm64ContextPointers* pointers) noexcept;

// Number of instructions covered by the prolog codes (from code index 0 up
// to end/end_c), and by the epilog that starts at code byte `index`.
std::uint32_t prolog_instruction_count(const FunctionUnwindInfo& info) noexcept;
std::uint32_t sequence_instruction_count(const FunctionUnwindInfo& info, std::uint32_t index) noexcept;
// Epilog scopes of `info` (reads xdata scope words). Returns the count written.
std::uint32_t epilog_scopes(const FunctionUnwindInfo& info, EpilogScope* out, std::uint32_t capacity) noexcept;

// ---------------------------------------------------------------------------
// Function tables
// ---------------------------------------------------------------------------

// RtlLookupFunctionEntry: guest modules (.pdata binary search) first, then
// dynamic tables and callbacks. Returns the guest address of the entry (in
// image or table memory) and the image base, or 0.
std::uint64_t lookup_function_entry(std::uint64_t pc, std::uint64_t* image_base);

// Binary search of a sorted table with `count` entries at `table`.
std::uint64_t search_function_table(std::uint64_t table, std::uint32_t count, std::uint64_t image_base,
                                     std::uint64_t pc, bool sorted) noexcept;

bool add_function_table(std::uint64_t table, std::uint32_t count, std::uint64_t base_address);
bool delete_function_table(std::uint64_t table);
bool install_function_table_callback(std::uint64_t identifier, std::uint64_t base_address,
                                     std::uint32_t length, std::uint64_t callback, std::uint64_t context);
std::uint32_t add_growable_function_table(std::uint64_t* handle, std::uint64_t table, std::uint32_t count,
                                          std::uint32_t maximum, std::uint64_t range_base,
                                          std::uint64_t range_end);
void grow_function_table(std::uint64_t handle, std::uint32_t count);
void delete_growable_function_table(std::uint64_t handle);
// Removes every dynamic table (tests).
void reset_dynamic_function_tables();

// ---------------------------------------------------------------------------
// Context conversion
// ---------------------------------------------------------------------------
void guest_to_arm64_context(const GuestContext& guest, Arm64Context& out, DWORD flags = kCtxFull);
// Applies the parts selected by `in.ContextFlags` (CONTROL, INTEGER,
// FLOATING_POINT); an ARM64 context with no section flags applies everything.
void arm64_to_guest_context(const Arm64Context& in, GuestContext& guest);
void fill_nonvolatile_registers(const Arm64Context& context, Arm64NonvolatileRegisters& out);

// ---------------------------------------------------------------------------
// Dispatcher internals
// ---------------------------------------------------------------------------

// Stack trace of `context` (no handlers are called).
std::string format_guest_stack(const Arm64Context& context, unsigned max_frames = 64);
std::string describe_guest_address(std::uint64_t address);

// Termination. The default hook prints `report` and ends the process with
// `exit_code` (TerminateProcess). Tests install a hook that throws.
using TerminationHook = void (*)(std::uint32_t exit_code, const std::string& report);
TerminationHook set_termination_hook(TerminationHook hook);
[[noreturn]] void terminate_process(std::uint32_t exit_code, const std::string& report);

// Per-thread dispatcher bookkeeping (tests).
std::size_t active_boundary_count();
void reset_thread_state();
void reset_process_state();   // vectored handlers, unhandled filter

// Statistics for tests.
struct DispatchStats {
    std::uint64_t dispatches;
    std::uint64_t handler_calls;
    std::uint64_t unwind_handler_calls;
    std::uint64_t consolidations;
    std::uint64_t nested;
    std::uint64_t collided;
};
DispatchStats dispatch_stats();

} // namespace transmute::fx::seh
