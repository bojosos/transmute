#pragma once

// Transmute fx engine: cross-component process API.
//
// Every function here has exactly one owning component (marked). Other
// components may call it. Signatures are additive-only: add new functions,
// never change or remove existing ones without integration sign-off.
//
// Core invariants (see docs/FX_ENGINE.md):
//  * Guest virtual address == host virtual address. Guest pointers are passed
//    to host APIs unchanged. There is no software MMU.
//  * Guest threads are host threads. There is no global execution lock.
//  * Guest code pages are never host-executable (PAGE_READONLY/READWRITE), so
//    a host jump into guest code faults and is turned into a callback.
//  * Imports from system DLLs bind once at load time to x64 thunks.

#include "transmute/fx/context.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace transmute::fx {

// ============================================================================
// Runtime (owner: runtime)  -- src/fx/runtime
// ============================================================================

struct RuntimeOptions {
    bool enable_translation_cache = true;   // persistent cache (cache component)
    bool profile = false;                   // counters; off for timing runs
    bool trace_exits = false;
    std::wstring cache_directory;           // empty = default
    // Install cached regions of loaded modules on a background thread, in
    // first-use order, before guest code asks for them. TRANSMUTE_FX_PRELOAD
    // (<threads>, 0 = off) overrides it either way. Alchemy's child processes set 0.
    bool preload_cached_regions = true;
};

// Initializes code arena, lookup table, stubs, vectored handler. Idempotent.
void runtime_initialize(const RuntimeOptions& options);

// Context of the calling host thread, or nullptr if it never ran guest code.
GuestContext* current_context() noexcept;

// Returns the calling thread's context, creating one (guest stack, shadow TEB,
// static TLS, DLL_THREAD_ATTACH) for a host thread that enters guest code for
// the first time (e.g. a host thread-pool callback).
GuestContext& ensure_thread_context();

// Runs guest function `entry` on the calling thread with integer arguments
// (x0..x7, further arguments on the guest stack) and returns x0. Re-entrant:
// may be called from a host callback while the same thread is inside a host
// call made by guest code. Guest callee-saved state is preserved.
std::uint64_t call_guest(std::uint64_t entry, std::span<const std::uint64_t> args);

// Like call_guest but with caller-prepared registers (x0..x7, v0..v7 already
// written into ctx). Runs until the guest returns to kReturnToHostPc.
void call_guest_prepared(GuestContext& ctx, std::uint64_t entry);

// ---- FP state at host-code boundaries ----------------------------------------
// Contract (docs/FX_ENGINE.md, FP): host code that receives a GuestContext&
// from fx -- the SVC handler (set_svc_handler), register_host_callback
// callbacks, request_interrupt requests, the guest fault hook, abi overrides
// and the seh entry points (RtlCaptureContext, RtlRestoreContext, longjmp,
// exception dispatch) -- sees ctx.fpsr and ctx.fpcr as the architectural
// FPSR and FPCR: the cumulative flags translated code accumulated in MXCSR
// are folded into ctx.fpsr first. Whatever the host code leaves in
// ctx.fpsr/ctx.fpcr is what the guest sees next: MRS FPSR/FPCR return it,
// cleared flags stay clear, and a new FPCR (rounding, FZ) applies to the
// next guest FP instruction. MXCSR's status flags while host code runs are
// not guest state (they may be stale copies), and flags the host code's own
// arithmetic raises there are dropped; abi overrides that run the host CRT's
// fenv functions on MXCSR take the result into ctx.fpsr themselves. A host
// callback, interrupt request or bracketed host entry that leaves by
// exit_to_dispatcher or a C++ exception keeps the contract too.
//
// Hand-written register_host_entry code is entered with the guest's FP state
// live in MXCSR (ctx.fpsr lags). Code that hands ctx to C++ brackets the call
// with these (ordinary Win64 functions; they clobber only volatile state):
//   fp_state_to_context: ctx.fpsr |= flags(MXCSR); from here ctx.fpsr is
//     authoritative (MXCSR is not written: ldmxcsr is expensive).
//   fp_state_from_context: MXCSR = rounding/FZ of ctx.fpcr (exceptions
//     masked) plus the flags of ctx.fpsr, which then keeps its other bits,
//     as translated code expects.
void fp_state_to_context(GuestContext& ctx) noexcept;
void fp_state_from_context(GuestContext& ctx) noexcept;

// Registers hand-written x64 code (e.g. an import thunk) as the translation
// of guest address `guest_pc`. Translated code reaching `guest_pc` jumps to
// `host_code` with the standard flushed-state entry convention. The guest FP
// state is live in MXCSR there (see fp_state_to_context above).
void register_host_entry(std::uint64_t guest_pc, const void* host_code);

// Native host-callback entry (e.g. libSystem calls of the macOS
// personality): like register_host_entry, but a guest branch to `guest_pc`
// (B/BL/BR/BLR, a linked exit, an RSB return, call_guest) calls
// `callback(ctx, user)` directly from translated code on the guest thread,
// without a dispatcher exit, with the call sequence of the abi override
// thunks. On entry the guest state is flushed into `ctx` and ctx.pc is the
// return address (x30). ctx.fpsr/ctx.fpcr are architectural (the FP contract
// above): the trampoline does what fp_state_to_context and
// fp_state_from_context do around the call, at about 2 ns per call and
// without ldmxcsr unless the callback changed FPCR or FPSR flags. The
// callback runs with the guest's MXCSR control bits (FPCR rounding/flush
// mode). The callback may read and write any guest state, call guest code
// (call_guest_prepared), raise guest exceptions, or leave by
// exit_to_dispatcher. When it returns, execution continues at ctx.x[30]
// (through the return-stack buffer), or at ctx.pc if the callback changed
// ctx.pc. Registering a pc again replaces the entry.
using HostCallback = void (*)(GuestContext& ctx, void* user);
void register_host_callback(std::uint64_t guest_pc, HostCallback callback, void* user);
// Removes a register_host_entry / register_host_callback entry (no-op if
// `guest_pc` has none). Exits linked to it are unlinked; the next branch to
// `guest_pc` resolves it again (e.g. translates guest code there).
// invalidate_guest_code drops translations only, never host entries.
void unregister_host_entry(std::uint64_t guest_pc);

// Drops translations overlapping [address, address + size). Safe to call
// from any thread at any time (VirtualProtect, VirtualFree, FlushInstruction
// Cache, write faults on translated pages, module unload).
void invalidate_guest_code(std::uint64_t address, std::uint64_t size);

// (additive) The guest says it rewrote code in [address, address + size)
// (Darwin sys_icache_invalidate): drops the translations whose guest bytes
// overlap the range. Translations that only share a page with it survive,
// and their page stays write-protected. Needed for writes fx cannot see (a
// file written into a shared view, another process, a mapping fx does not
// know); stores through mappings fx knows fault and invalidate by themselves.
// Counted in RuntimeStats::code_flushes. Safe from any thread at any time.
void flush_guest_code(std::uint64_t address, std::uint64_t size);

// (additive) The guest gave [address, address + size) a non-writable
// protection (already applied to the host pages): fx forgets the protection
// it saved for pages it write-protected there, so an SMC fault can never
// restore a writable protection over the guest's. Translations stay valid
// (the bytes cannot change through a non-writable page). A writable
// protection needs invalidate_guest_code instead.
void guest_protection_lowered(std::uint64_t address, std::uint64_t size);

// (additive) The page protection the guest set on the page at `address` when
// fx has write-protected it because it holds translated code (its host
// protection is then read-only), or 0 when fx has not.
std::uint32_t guest_page_protection(std::uint64_t address) noexcept;

struct GuestThreadStart {
    std::uint64_t entry;          // LPTHREAD_START_ROUTINE (guest)
    std::uint64_t argument;
    std::uint64_t stack_reserve;  // 0 = image default
    std::uint32_t creation_flags; // CREATE_SUSPENDED etc.
};
// Creates a host thread that runs guest code. Returns the host HANDLE (as an
// integer) and writes the thread id. Zero on failure (GetLastError set).
std::uint64_t create_guest_thread(const GuestThreadStart& start, std::uint32_t* thread_id);

// Ends the calling guest thread (DLL_THREAD_DETACH, TLS teardown) and the
// host thread with `exit_code`.
[[noreturn]] void exit_guest_thread(std::uint32_t exit_code);

// (additive) Shared guest memory: section-backed ranges that can be mapped at
// more than one guest address (e.g. Darwin vm_remap of writable memory).
// Ranges are 4 KiB aligned. map_shared_guest_memory maps new zero-filled
// memory at [base, base + size), which must be free address space (or fx
// shared-memory space freed by unmap_shared_guest_memory), as views of
// `granule` bytes each (a page multiple dividing size; 0 = one view): the unit
// in which it can later be unmapped or replaced. alias_guest_memory maps the
// pages of shared memory [source, source + size) a second time at
// `destination` (free space, or whole shared-memory views, which it replaces):
// writes through either address are seen through both. Pages live while any
// view maps them. `protect` is PAGE_READWRITE, PAGE_READONLY or PAGE_NOACCESS
// (VirtualProtect may change it later). Translated code on shared pages is
// write-protected at every address mapping them and invalidated by a write
// through any of them. Throw std::invalid_argument / std::runtime_error.
void map_shared_guest_memory(std::uint64_t base, std::uint64_t size, std::uint64_t granule, std::uint32_t protect);
void alias_guest_memory(std::uint64_t destination, std::uint64_t source, std::uint64_t size, std::uint32_t protect);
// (additive) Host sections at guest addresses: maps `view_size` bytes of the
// section `section` (a Windows file-mapping HANDLE; fx keeps a duplicate, the
// caller keeps its own) from `offset` (4 KiB aligned) at [base, base +
// view_size); view_size may end inside a page when the section (a file) ends
// there. The pages past it, up to base + size, stay an inaccessible fx
// placeholder (Darwin pages beyond the end of a mapped file). base == 0 picks
// free address space (64 KiB aligned). A section the caller created read-only
// (`writable_section` false) is viewed read-only and `protect` may not ask for
// write. Two processes viewing sections of one file (or one named section)
// see the same pages: Darwin mmap(MAP_SHARED). Returns the base; unmapped with
// unmap_shared_guest_memory(base, size). Throws like map_shared_guest_memory.
std::uint64_t map_section_guest_memory(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                                       std::uint64_t view_size, bool writable_section, std::uint32_t protect);
// (additive) A host section at [base, base + size) (free address space, or
// fx shared-memory space freed earlier) as views of `granule` bytes each (0 =
// one view) from `offset`: map_shared_guest_memory over a caller's section
// (fx keeps a duplicate). All views share one section record, so aliases made
// from them are known to each other. Memory another process shares with this
// one: writes there are seen here, but fx cannot see that process's stores to
// translated code.
void map_section_guest_pages(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                             std::uint64_t granule, bool writable_section, std::uint32_t protect);
// (additive) A read-only file section (`section`, a Windows file-mapping
// HANDLE; fx keeps a duplicate) viewed copy-on-write at [base, base + size)
// (free address space, 4 KiB aligned): the first `file_bytes` from `offset`
// (4 KiB aligned) are the file's pages, shared with every process viewing the
// file until a write here makes a private copy; the rest of the last file
// page and the pages after it read as zeros. Mach-O image segments, as dyld
// maps them. Writable protections are write-copy (host vm::protect). Not
// shared memory: alias_guest_memory refuses it as source or destination,
// duplicate_shared_guest_section returns null for it and a partial
// unmap_shared_guest_range of it throws. Unmapped with
// unmap_shared_guest_memory(base, size).
void map_file_copy_guest_memory(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                                std::uint64_t file_bytes);
// (additive) A duplicate of the section handle behind the shared view at
// `address` (the caller closes it) and the address's byte offset in it; the
// section's mapped size and writability. Null for other memory.
void* duplicate_shared_guest_section(std::uint64_t address, std::uint64_t* offset, std::uint64_t* size,
                                     bool* writable) noexcept;
// Unmaps whole views covering [base, base + size) (gaps that are fx
// placeholders are allowed); drops their translations.
void unmap_shared_guest_memory(std::uint64_t base, std::uint64_t size);
// (additive) Unmaps [base, base + size) (4 KiB aligned) from whatever shared
// views overlap it, keeping the parts of them outside the range mapped (with
// their protection and contents); placeholders inside it stay fx space.
// Darwin munmap of part of a file or shared mapping. Drops translations of
// the range.
void unmap_shared_guest_range(std::uint64_t base, std::uint64_t size);
bool is_shared_guest_memory(std::uint64_t address) noexcept;

// Guest-executable memory outside PE images (guest VirtualAlloc/VirtualProtect
// with PAGE_EXECUTE*). Host protection stays non-executable.
void register_guest_code_memory(std::uint64_t base, std::uint64_t size);
void unregister_guest_code_memory(std::uint64_t base, std::uint64_t size);
// True for addresses inside guest images' executable sections or registered
// guest code memory. Lock-free.
bool is_guest_code(std::uint64_t address) noexcept;

// (additive) Demand commit: reserved address space the personality has handed
// to the guest as accessible but not committed yet (Darwin commits anonymous
// memory on first touch). fx calls the hook when a host access to `address`
// faults (the fault handler, first chance, on any thread, after the SMC write
// branch) and when the JIT finds a guest code page uncommitted. `access` is
// the fault's kind: 0 read, 1 write, 8 execute. The hook returns true when it
// made the page accessible for that access (the access is retried), false
// otherwise (the fault takes its normal course). It runs inside the fault
// handler: it must not run guest code, must not throw, and must not take a
// lock that code touching guest memory may hold. One hook per process;
// nullptr removes it.
using DemandCommitHook = bool (*)(std::uint64_t address, std::uint32_t access, void* user);
void set_demand_commit_hook(DemandCommitHook hook, void* user);

// Allocates code memory reachable with rel32 from all translated code.
// Used by thunk/trampoline generators. Memory is RWX for the process lifetime.
void* allocate_code(std::size_t size, std::size_t alignment = 16);

// Sets GuestContext::helpers[id] (see HelperId ranges) in every existing and
// future context. Components register their helpers once at startup.
void register_helper(unsigned id, const void* function);

// (additive) Measurement window of the sampling profiler (TRANSMUTE_FX_SAMPLE,
// docs/FX_ENGINE.md "Profiling"); no-ops when it was not requested.
// profiler_reset drops every sample taken so far, so the report covers only
// later ones; profiler_set_enabled(false) pauses sampling (the report keeps
// what was sampled), true resumes it and cancels a pending `delay=` start.
void profiler_reset();
void profiler_set_enabled(bool enabled);

// ============================================================================
// Loader (owner: loader)  -- src/fx/loader
// ============================================================================

struct GuestModule {
    std::uint64_t base{};
    std::uint64_t size{};
    std::uint64_t entry{};              // 0 if none
    std::wstring path;                  // full path
    std::wstring name;                  // file name, original case
    bool is_dll{};
    std::uint32_t pdata_rva{};          // exception directory (.pdata) RVA
    std::uint32_t pdata_size{};
    std::uint32_t tls_index{};          // static TLS slot, UINT32_MAX if none
    std::uint64_t image_hash{};         // identity for the translation cache
    std::uint64_t text_begin{};         // union of executable sections
    std::uint64_t text_end{};
    std::uint64_t stack_reserve{};      // optional header SizeOfStackReserve
    std::uint64_t stack_commit{};       // optional header SizeOfStackCommit
};

// Lock-free lookup of the guest module containing `address`, or nullptr.
const GuestModule* find_guest_module(std::uint64_t address) noexcept;

// Snapshot of all loaded guest modules (load order; external modules last).
std::vector<const GuestModule*> guest_modules();

// Non-PE guest code modules (e.g. Mach-O images mapped by the macOS
// personality). A registered module is returned by find_guest_module and
// guest_modules as a GuestModule with these fields, is_dll = true,
// pdata_rva = pdata_size = 0, entry = 0 and tls_index = UINT32_MAX, so fault
// reports and the sampling profiler attribute addresses to it (module +
// offset; no .pdata function bounds) and the translation cache keys its
// regions by image_hash (0: not cached; it must identify the bytes, e.g. a
// hash of the file or its UUID load command). text_begin/text_end: the
// executable range ([base, base + size) if both are 0). This does not make
// the memory guest code: register it with register_guest_code_memory, as
// before. Throws std::invalid_argument for an empty range, a text range
// outside it, or an overlap with a PE guest module or another external
// module; registering the same base again replaces the module.
struct GuestCodeModule {
    std::uint64_t base{};
    std::uint64_t size{};
    std::uint64_t text_begin{};
    std::uint64_t text_end{};
    std::uint64_t image_hash{};
    std::wstring path;
    std::wstring name;
};
void register_external_module(const GuestCodeModule& module);
// Removes the external module at `base` (no-op if none) and drops the
// translations of its range. Pointers returned by find_guest_module stay valid.
void unregister_external_module(std::uint64_t base);

// LoadLibraryExW semantics for guest code. Resolves to an ARM64 guest DLL
// when one is found on the guest search path, otherwise loads the host DLL.
// Returns the HMODULE value (guest image base or host module) or 0.
std::uint64_t guest_load_library(std::wstring_view name, std::uint32_t flags);

// GetModuleHandleExW semantics over guest and host modules.
std::uint64_t guest_get_module_handle(std::wstring_view name);

// GetProcAddress for guest code. For guest modules returns the export
// address (resolving forwarders). For host modules returns a guest-callable
// thunk from bind_host_function. Ordinals: pass a value < 0x10000 cast to
// pointer, exactly as GetProcAddress does.
std::uint64_t guest_get_proc_address(std::uint64_t module, const char* name);

bool guest_free_library(std::uint64_t module);
// Keep a guest DLL mapped until process exit, including callbacks queued to
// native thread pools that cannot retain a guest HMODULE through Windows Ldr.
bool guest_pin_library(std::uint64_t module);

// Thread lifecycle notifications called by the runtime on the new thread
// before/after guest code runs. Allocate/free static TLS blocks in the
// shadow TEB and run TLS callbacks and DllMain(THREAD_ATTACH/DETACH).
void loader_thread_attach(GuestContext& ctx);
void loader_thread_detach(GuestContext& ctx);

// Guest process identity used by abi overrides (GetCommandLineW, CRT argv,
// GetModuleFileNameW(NULL)).
const std::wstring& guest_command_line();
const std::wstring& guest_image_path();

// Default guest stack size: used for the main guest thread and for threads
// created with stack_reserve == 0 (CreateThread dwStackSize == 0). The reserve
// is LaunchOptions::stack_reserve (transmute-fx --stack-size), else the
// TRANSMUTE_FX_STACK_SIZE environment variable, else the EXE's
// SizeOfStackReserve; the commit is the EXE's SizeOfStackCommit (capped at the
// reserve). launch_guest_process sets both before it calls
// ensure_thread_context. 0 until then (runtime default).
std::uint64_t guest_image_stack_reserve() noexcept;
std::uint64_t guest_image_stack_commit() noexcept;

// Process exit for guest ExitProcess/exit(): runs guest DLL_PROCESS_DETACH
// where safe, flushes the translation cache, then ends the host process.
[[noreturn]] void loader_process_exit(std::uint32_t exit_code);

// Process termination for guest TerminateProcess(GetCurrentProcess()) /
// NtTerminateProcess(NtCurrentProcess()): the same preparation (background
// work stopped, cache flushed, stats), then host TerminateProcess, so no
// DLL_PROCESS_DETACH runs, as natively.
[[noreturn]] void loader_process_terminate(std::uint32_t exit_code);

// Main program entry used by transmute-fx.exe.
struct LaunchOptions {
    std::wstring image_path;
    std::wstring command_line;            // guest command line (argv[0] = image path)
    std::vector<std::wstring> dll_directories;
    RuntimeOptions runtime;
    // Default guest stack reserve in bytes (main thread and default-sized
    // threads); 0 = TRANSMUTE_FX_STACK_SIZE or the EXE's SizeOfStackReserve.
    std::uint64_t stack_reserve = 0;
};
int launch_guest_process(const LaunchOptions& options);

// ============================================================================
// ABI / thunks (owner: abi)  -- src/fx/abi
// ============================================================================

// Guest-callable address for host function `function` exported by host DLL
// `dll` at `host_address`. The thunk marshals the ARM64 call (x0-x7, v0-v7,
// guest stack) to the Win64 convention using the signature database, and
// applies any registered override (CreateThread, GetProcAddress, CRT argv,
// callbacks, ...). Cached: same inputs return the same address.
std::uint64_t bind_host_function(std::string_view dll, std::string_view function,
                                 const void* host_address);

// Guest-callable thunk for a host code address of unknown signature (e.g. a
// function pointer returned by a host API). Positional integer marshalling.
std::uint64_t bind_unknown_host_target(const void* host_address);

// Host-callable (Win64) function pointer that runs guest function
// `guest_function` when called. `signature` uses the sigdb grammar
// (docs/FX_ENGINE.md); nullptr = positional integer arguments, returns x0
// in rax and d0 in xmm0.
const void* host_callable(std::uint64_t guest_function, const char* signature);

// Native replacement for an import that would otherwise bind to a guest DLL
// export (e.g. VCRUNTIME140!memcpy -> host memcpy thunk). The loader calls
// this before binding a guest export; 0 = no replacement.
std::uint64_t abi_guest_import_replacement(std::string_view dll, std::string_view function);

// True if `address` is a thunk created by this component.
bool is_abi_thunk(std::uint64_t address) noexcept;

// Host stack frame of abi-generated code (thunks and callback trampolines,
// all in allocate_code memory) for the runtime's code-arena unwinder.
// Guest-callable thunks are entered by jmp with rsp == ctx->host_rsp
// (entered_by_call == false); callback trampolines are Win64 functions
// ([entry rsp] = return address). Between begin + prolog_bytes and
// begin + epilog_offset, `frame_bytes` are allocated below the entry rsp;
// elsewhere in [begin, end) rsp equals the entry rsp. Returns false if
// `address` is not abi code.
struct AbiCodeFrame {
    std::uint64_t begin{};
    std::uint64_t end{};
    std::uint32_t prolog_bytes{};
    std::uint32_t epilog_offset{};
    std::uint32_t frame_bytes{};
    bool entered_by_call{};
};
bool abi_code_frame(std::uint64_t address, AbiCodeFrame& frame) noexcept;

// ============================================================================
// SEH (owner: seh)  -- src/fx/seh
// ============================================================================

struct GuestExceptionRecord {
    std::uint32_t code;
    std::uint32_t flags;
    std::uint64_t address;                 // guest PC
    std::uint32_t parameter_count;
    std::uint64_t parameters[15];
};

// Dispatches a guest exception for the current thread whose guest register
// state is in `ctx` (fully flushed, ctx.pc = faulting instruction). Runs guest
// vectored handlers, frame handlers (ARM64 .pdata/.xdata unwind) and the
// unhandled-exception filter. On return, `ctx` holds the state to resume
// (EXCEPTION_CONTINUE_EXECUTION or after an unwind to a handler). Never
// returns if the process must terminate.
void dispatch_guest_exception(GuestContext& ctx, const GuestExceptionRecord& record);

// How a guest-visible SEH entry point left the calling thread. The abi
// component's thunks use this to decide whether to return to the guest caller
// or resume translated execution at the (rewritten) ctx.pc.
enum class GuestResolution : std::uint32_t {
    continue_at_caller = 0,   // ordinary return to the guest caller (x0 = result)
    continue_at_pc = 1,       // resume translated execution at ctx.pc (state rewritten)
    terminated = 2,           // process is terminating; never actually observed
};

// A hardware fault reported by the runtime's vectored handler, translated
// into a guest exception. `kind` selects the NTSTATUS; access_* describe an
// access violation / in-page error.
struct GuestFault {
    enum class Kind : std::uint32_t {
        access_violation,
        in_page_error,
        illegal_instruction,
        privileged_instruction,
        stack_overflow,
        breakpoint,
        datatype_misalignment,
        guard_page,
        integer_overflow,
    };
    Kind kind{};
    std::uint64_t faulting_pc{};      // guest PC of the faulting instruction
    std::uint64_t access_address{};   // for access_violation / in_page_error
    std::uint32_t access_type{};      // 0 read, 1 write, 8 execute (as in EXCEPTION_RECORD)
    std::uint32_t nt_status{};        // for in_page_error, the underlying status
};

// Builds a GuestExceptionRecord from a hardware fault and dispatches it.
void dispatch_guest_fault(GuestContext& ctx, const GuestFault& fault);

// ---- Personality fault hook ------------------------------------------------
//
// Lets a personality (e.g. the macOS personality, which maps guest faults to
// Darwin signals) take guest exceptions that the guest itself does not
// handle, without going through guest SEH structures.
//
// When: last chance only. dispatch_guest_exception runs, in order, the guest
// vectored exception handlers, the frame-based handlers found through .pdata
// (none for images without .pdata, e.g. Mach-O images registered with
// register_external_module), and the guest's unhandled-exception filter
// (SetUnhandledExceptionFilter). If all of them decline, the hook is called,
// on the faulting thread, immediately before fx would report the unhandled
// exception and terminate the process. It sees every such exception:
// hardware faults (access violation, illegal instruction, stack overflow,
// ...) and software exceptions (RaiseException, BRK; __fastfail terminates
// without a dispatch).
// There is no first-chance mode: guest handlers always run first.
//
// ctx: the calling thread's GuestContext holding the exception context:
// x0..x30, SP (x[31]), pc, nzcv, FPCR/FPSR (architectural, including the
// flags raised since the last region entry) and v0..v31. For a hardware fault
// ctx.pc == fault.pc, the faulting instruction; for a software exception it
// is the continuation (the return address of the raising call). The hook may
// modify any of these (registers, pc, FPCR/FPSR) and return true: execution
// resumes at ctx (ctx.pc) as for EXCEPTION_CONTINUE_EXECUTION, also for
// non-continuable exceptions (the hook takes responsibility). It returns
// false if it does not handle the exception: changes to ctx are discarded,
// and fx reports the exception and terminates with the NTSTATUS as the exit
// code, as without a hook. The hook may also end the process or thread
// itself (it never returns then).
//
// The hook should not run guest code itself. To run guest code, point
// ctx.pc (and argument registers, x30, SP) at it and return true; faults in
// that code are dispatched normally (and reach the hook again).
//
// One hook per process. set_guest_fault_hook replaces the previous hook
// (nullptr removes it) and is thread-safe; the hook may run on several
// threads at once. `user` is passed through unchanged.
struct GuestFaultInfo {
    std::uint32_t code{};              // NTSTATUS (EXCEPTION_RECORD::ExceptionCode)
    std::uint32_t flags{};             // ExceptionFlags (1 = EXCEPTION_NONCONTINUABLE)
    std::uint64_t address{};           // data address of an access fault (access violation,
                                       // in-page error, guard page: ExceptionInformation[1]); else 0
    std::uint64_t pc{};                // ExceptionAddress: guest pc of the faulting instruction
    bool write{};                      // access fault on a write (ExceptionInformation[0] == 1)
    bool execute{};                    // access fault on an instruction fetch (ExceptionInformation[0] == 8)
    std::uint32_t parameter_count{};   // ExceptionInformation as dispatched to guest handlers
    std::uint64_t parameters[15]{};
};
using GuestFaultHook = bool (*)(GuestContext& ctx, const GuestFaultInfo& fault, void* user);
void set_guest_fault_hook(GuestFaultHook hook, void* user);

// Guest-visible implementations bound by the abi component for the
// corresponding kernel32/ntdll exports. Each takes the GuestContext of the
// calling guest thread (x0..x7 hold the ARM64 arguments) and reports whether
// execution continues at the guest caller or at a rewritten ctx.pc.

// RaiseException(code, flags, count, arguments). Convenience wrapper used by
// translated code and the abi thunk.
void guest_raise_exception(GuestContext& ctx, std::uint32_t code, std::uint32_t flags,
                           std::uint32_t count, const std::uint64_t* arguments);
GuestResolution guest_RaiseException(GuestContext& ctx);

// RtlCaptureContext(PCONTEXT): writes the caller's ARM64 context (x0 points at
// it) and returns to the caller.
GuestResolution guest_RtlCaptureContext(GuestContext& ctx);
// RtlRestoreContext(PCONTEXT, PEXCEPTION_RECORD): resumes at the given context
// (handles STATUS_UNWIND_CONSOLIDATE and STATUS_LONGJUMP records).
GuestResolution guest_RtlRestoreContext(GuestContext& ctx);

// RtlVirtualUnwind(HandlerType, ImageBase, ControlPc, FunctionEntry, Context,
// HandlerData*, EstablisherFrame*, ContextPointers): returns the handler in x0.
GuestResolution guest_RtlVirtualUnwind(GuestContext& ctx);
// RtlLookupFunctionEntry(ControlPc, ImageBase*, HistoryTable): returns the
// RUNTIME_FUNCTION address in x0, image base in *ImageBase.
GuestResolution guest_RtlLookupFunctionEntry(GuestContext& ctx);

// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue) and
// RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, Context,
// HistoryTable): run the second (unwinding) pass and transfer to the target.
GuestResolution guest_RtlUnwind(GuestContext& ctx);
GuestResolution guest_RtlUnwindEx(GuestContext& ctx);

// RtlAddFunctionTable / RtlDeleteFunctionTable / RtlInstallFunctionTable
// Callback / RtlAddGrowableFunctionTable / RtlGrowFunctionTable /
// RtlDeleteGrowableFunctionTable for guest-generated code.
GuestResolution guest_RtlAddFunctionTable(GuestContext& ctx);
GuestResolution guest_RtlDeleteFunctionTable(GuestContext& ctx);
GuestResolution guest_RtlInstallFunctionTableCallback(GuestContext& ctx);
GuestResolution guest_RtlAddGrowableFunctionTable(GuestContext& ctx);
GuestResolution guest_RtlGrowFunctionTable(GuestContext& ctx);
GuestResolution guest_RtlDeleteGrowableFunctionTable(GuestContext& ctx);

// SetUnhandledExceptionFilter / UnhandledExceptionFilter.
GuestResolution guest_SetUnhandledExceptionFilter(GuestContext& ctx);
GuestResolution guest_UnhandledExceptionFilter(GuestContext& ctx);

// AddVectoredExceptionHandler / AddVectoredContinueHandler /
// RemoveVectoredExceptionHandler.
GuestResolution guest_AddVectoredExceptionHandler(GuestContext& ctx);
GuestResolution guest_AddVectoredContinueHandler(GuestContext& ctx);
GuestResolution guest_RemoveVectoredExceptionHandler(GuestContext& ctx);

// longjmp(jmp_buf, value) with the ARM64 _JUMP_BUFFER layout produced by the
// guest's _setjmp/_setjmpex. Restores the saved registers and transfers to the
// saved PC, running intervening unwind handlers (setjmpex frame).
GuestResolution guest_longjmp(GuestContext& ctx);

// _resetstkoflw() for the guest stack (the host UCRT routine re-arms the
// host stack's guard): re-arms the runtime's guard region of the guest stack
// (rearm_guest_stack_guard) so the next overflow raises STATUS_STACK_OVERFLOW
// again. x0 = nonzero on success.
GuestResolution guest__resetstkoflw(GuestContext& ctx);

} // namespace transmute::fx
