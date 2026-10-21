#pragma once

// Experimental fx execution backend for Alchemy (alchemy-fx).
//
// FxEngine exposes the subset of transmute::ReferenceEngine that Alchemy uses,
// implemented on the fx runtime: guest VA == host VA, guest threads are host
// threads, guest code is translated by fx. Alchemy's sources are compiled
// unchanged against it through alchemy_fx_prelude.hpp, which renames
// ReferenceEngine to FxEngine for that build only.
//
// Synthetic guest calls live at the reference engine's addresses
// (0x7100'0000'0000 + 16 * index) as `svc #0x7a1c` slots; Darwin system calls
// keep their own SVC immediates. Both reach Alchemy through fx's SVC hook.

#include "alchemy/call_regs.hpp"
#include "transmute/a64.hpp"
#include "transmute/jit.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace transmute {
namespace fx { struct GuestContext; }

class FxEngine : public GuestMemory {
public:
    explicit FxEngine(ExecutionStart start);
    ~FxEngine() override;
    FxEngine(const FxEngine&) = delete;
    FxEngine& operator=(const FxEngine&) = delete;

    // ---- GuestMemory (direct host memory at the guest address) -------------
    void map_memory(std::uint64_t base, std::size_t size) override;
    void reserve_memory(std::uint64_t base, std::uint64_t size) override;
    void map_external_memory(std::uint64_t base, std::uint8_t* bytes,
                             std::size_t size, bool writable) override;
    void unmap_memory(std::uint64_t base) override;
    [[nodiscard]] bool is_range_available(std::uint64_t base, std::uint64_t size) const override;
    [[nodiscard]] std::optional<RegionInfo> query_region(std::uint64_t address) const override;
    void set_memory_access(std::uint64_t base, bool readable, bool writable, bool executable) override;
    // Page-granular protection of [address, address + size) inside mapped
    // regions (Darwin partial mprotect). Region records keep their original
    // access; host faults enforce the new protection.
    // Reserves `size` bytes anywhere; committed now when readable/writable,
    // else on a later protect_range. Unmapped with unmap_memory(base).
    std::uint64_t reserve_anywhere(std::uint64_t size, std::uint64_t alignment, bool readable, bool writable);
    // Darwin VM operations on guest memory (darwin_vm_space.cpp):
    // reserve_at is reserve_anywhere at exactly `base` (page aligned, above
    // __PAGEZERO), false when any of the range is in use. unmap_range removes
    // a page range from the regions it overlaps, private or shared views
    // (their other parts stay), drops its translations and code registration,
    // decommits private pages and releases reservations left empty. replace_range is unmap_range then
    // reserve_at (mmap MAP_FIXED: fresh zero-filled pages with the new
    // protection). discard_range lets the host drop the contents of committed
    // pages (MADV_FREE: later reads see the old bytes or zeros). region_from
    // is the region containing `address` or the lowest above it; inside a
    // reservation, the run of pages with one protection.
    bool reserve_at(std::uint64_t base, std::uint64_t size, bool readable, bool writable);
    void unmap_range(std::uint64_t address, std::uint64_t size);
    void replace_range(std::uint64_t address, std::uint64_t size, bool readable, bool writable, bool executable);
    void discard_range(std::uint64_t address, std::uint64_t size);
    // A host section as `granule`-byte shared regions at [base, base + size)
    // (free space); duplicate_section returns a duplicate of the section
    // handle behind shared memory at `address` (cross_process_memory.hpp).
    void map_section_pages(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                           std::uint64_t granule, bool writable_section, bool readable, bool writable);
    void* duplicate_section(std::uint64_t address, std::uint64_t& offset, std::uint64_t& size, bool& writable) const;
    [[nodiscard]] std::optional<RegionInfo> region_from(std::uint64_t address) const;
    // Bytes of [address, address + size) the host has committed: Darwin's
    // resident pages, since anonymous memory commits on first touch (large
    // reservations made accessible commit page by page, from fx's fault handler).
    [[nodiscard]] std::uint64_t resident_bytes(std::uint64_t address, std::uint64_t size) const;
    // Shared guest memory (fx section-backed views): zero-filled
    // read/write memory at [base, base + size), one guest region per
    // `granule` bytes (0 = one region), that alias_memory can map a second
    // time at another address (Mach vm_remap of writable memory). The
    // destination of alias_memory is free or consists of whole shared regions,
    // which it replaces; writes through either address are seen through both,
    // and the pages live while any region maps them. unmap_memory and
    // set_memory_access work on these regions as on any other.
    void map_shared_memory(std::uint64_t base, std::uint64_t size, std::uint64_t granule);
    [[nodiscard]] bool is_shared_memory(std::uint64_t base, std::uint64_t size) const;
    void alias_memory(std::uint64_t destination, std::uint64_t source, std::uint64_t size, bool readable,
                      bool writable, bool executable);
    // A host section (Windows file-mapping HANDLE, duplicated) viewed at a
    // guest address: `view_size` bytes from `offset` at the start of a
    // `size`-byte region whose tail stays inaccessible (pages beyond the end
    // of a file). base == 0 picks the address. One shared region; unmap_memory
    // and set_memory_access work on it. Darwin mmap(MAP_SHARED): every
    // process viewing the same file or named section sees the same pages.
    std::uint64_t map_section(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                              std::uint64_t view_size, bool writable_section, bool readable, bool writable);
    // A Mach-O segment from its file (`section`, a read-only file-mapping
    // HANDLE) at [base, base + size): the first `file_bytes` from `offset`
    // copy-on-write, the rest zero (fx::map_file_copy_guest_memory). The file's
    // pages are shared with every process mapping the image until written
    // here. One region, readable and writable until set_memory_access; not
    // shared memory for is_shared_memory / alias_memory / duplicate_section.
    void map_file_copy(std::uint64_t base, std::uint64_t size, void* section, std::uint64_t offset,
                       std::uint64_t file_bytes);
    // Names a mapped Mach-O image for fault reports, the profiler and the
    // persistent translation cache (keyed by the image's LC_UUID).
    void register_image(std::uint64_t header, std::uint64_t begin, std::uint64_t end, std::uint64_t text_begin,
                        std::uint64_t text_end, const std::string& path);
    // Writes pending translations to the persistent cache (process exit).
    void flush_translations();
    // The guest rewrote code in [address, address + size)
    // (sys_icache_invalidate): drops the translations of those bytes
    // (fx::flush_guest_code; invalidate_engine_code calls it).
    void invalidate_code(std::uint64_t address, std::uint64_t size);
    // The calling thread opens or closes a JIT write window
    // (pthread_jit_write_protect_np; fx::guest_jit_write_window).
    void jit_write_window(bool open);
    // [copy, copy + size) holds an exact copy of the code at source
    // (fx::register_guest_code_copy).
    void code_copy(std::uint64_t copy, std::uint64_t source, std::uint64_t size);
    // Guest malloc heap: host heap memory is guest memory (VA == host VA).
    std::optional<std::uint64_t> heap_allocate(std::uint64_t size, std::uint64_t alignment);
    void heap_release(std::uint64_t address);
    void protect_range(std::uint64_t address, std::uint64_t size, bool readable, bool writable, bool executable);
    [[nodiscard]] std::uint32_t read32(std::uint64_t address) const override;
    [[nodiscard]] std::uint64_t read64(std::uint64_t address) const override;
    void write32(std::uint64_t address, std::uint32_t value) override;
    void write64(std::uint64_t address, std::uint64_t value) override;
    void read_bytes(std::uint64_t address, std::span<std::uint8_t> bytes) const override;
    void write_bytes(std::uint64_t address, std::span<const std::uint8_t> bytes) override;
    void fill_bytes(std::uint64_t address, std::uint8_t value, std::uint64_t count) override;
    void copy_bytes(std::uint64_t destination, std::uint64_t source,
                    std::uint64_t count, bool move = false) override;
    [[nodiscard]] std::vector<std::uint8_t> read_string(
        std::uint64_t address, unsigned character_bytes, std::size_t maximum_characters,
        bool* terminated = nullptr) const override;
    [[nodiscard]] std::size_t string_length(
        std::uint64_t address, unsigned character_bytes, std::size_t maximum_characters,
        bool* terminated = nullptr) const override;

    // ---- execution (ReferenceEngine subset used by Alchemy) ----------------
    void set_initial_register(unsigned index, std::uint64_t value);
    void set_initial_vector(unsigned index, std::array<std::uint64_t, 2> value);
    void set_initial_stack_pointer(std::uint64_t value);
    void set_supervisor_call_handler(SupervisorCallHandler handler);
    void set_instruction_limit(std::uint64_t) noexcept {}
    void set_jit_enabled(bool) noexcept {}
    void set_jit_profiling(bool) noexcept {}
    [[nodiscard]] JitStats jit_stats(std::size_t hottest = 10) const;
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> stack_bounds() const;
    std::uint64_t invoke_callback(std::uint64_t address, std::uint64_t argument);
    std::uint64_t invoke_callback(std::uint64_t address, std::span<const std::uint64_t> arguments);
    // Full-register host->guest call: x0..x7, x8 (indirect result), q0..q7
    // and stack arguments in, x0/x1 and q0..q3 out. Same stack placement,
    // nesting and process-stop rules as invoke_callback; the stack bytes are
    // copied below the guest SP (past its red zone) for the call's duration.
    // Used for Objective-C sends with NSRect/NSPoint/double arguments or
    // results (drawRect:, firstRectForCharacterRange:actualRange:, ...).
    alchemy::CallResult invoke_callback_regs(std::uint64_t address, const alchemy::CallRegs& registers);
    [[nodiscard]] std::uint64_t register_guest_call(GuestCallHandler handler);
    void unregister_guest_call(std::uint64_t address);
    [[nodiscard]] std::unique_ptr<FxEngine> fork_thread(
        std::uint64_t entry, std::uint64_t argument, std::uint64_t stack_size = 0);
    void resize_stack(std::uint64_t size);
    void set_default_worker_stack_size(std::uint64_t size);
    [[nodiscard]] std::uint64_t default_worker_stack_size() const noexcept;
    [[nodiscard]] static FxEngine* current() noexcept;
    // Reserves the main executable's load range (`load_base`, or the usual
    // 0x1'0000'0000) for the process, before anything else allocates: called
    // first thing in main. The first guest placement inside it takes it over.
    static void hold_load_range(std::optional<std::uint64_t> load_base) noexcept;
    // The base a position-independent main executable of `size` bytes should
    // load at instead of `preferred`, when host memory placed before main
    // (thread stacks, heap segments: Windows places them bottom-up from a
    // random base) sits in [preferred, preferred + size): the lowest held part
    // of the load range that fits it. nullopt: load at `preferred`.
    [[nodiscard]] static std::optional<std::uint64_t> slide_main_image(std::uint64_t preferred, std::uint64_t size) noexcept;
    RunResult run(std::uint64_t maximum_instructions = 0);
    [[nodiscard]] const CpuState& state() const noexcept { return state_; }
    // TPIDR_EL0 of the calling thread's guest context (engine_adapter.hpp).
    void set_thread_pointer(std::uint64_t value);
    // The synthetic call at `address` reads and writes no SIMD/FP register:
    // its entry copies only the integer state (engine_adapter.hpp).
    void set_guest_call_integer_only(std::uint64_t address);
    // The synthetic call at `address` is libSystem's `symbol`; for the pure
    // memory/string functions (memcpy, memmove, memset, bzero, bcopy, memcmp,
    // strlen) its native entry runs them on the guest registers directly and
    // takes the handler only when that faults (engine_adapter.hpp).
    void set_guest_call_leaf(std::uint64_t address, std::string_view symbol);

    // Called by the fx SVC hook on the thread running this engine.
    void handle_svc(void* guest_context, std::uint32_t immediate);
    // Runs the synthetic call at `pc` on guest state `guest_context` (from the
    // SVC slot or a native host-callback entry) and sets the resume pc.
    void invoke_synthetic(void* guest_context, std::uint64_t pc);
    // fx::HostCallback entry of a synthetic call (user = its guest address).
    static void host_callback(fx::GuestContext& ctx, void* user);
    // Gives the synthetic call at `pc` its native host-callback entry, once
    // (from its first call, which takes the SVC slot).
    void install_native_entry(std::uint64_t pc);

    // ---- process stop (Darwin exit and fatal signals) -----------------------
    // request_stop delivers fx::request_interrupt to every thread running
    // guest code; each unwinds at its next safe point and its run() returns.
    // Afterwards no guest code runs: run() returns at once and an outermost
    // invoke_callback returns 0. Idempotent; the first status is kept.
    struct ThreadUnwind {};   // not a std::exception: Alchemy handlers do not catch it
    struct Termination {
        std::uint32_t status{};
        int signal{};
        std::string reason;
        std::uint64_t pc{}, lr{}, fp{};
        bool main_thread{};
    };
    void request_stop(std::uint32_t status);
    [[nodiscard]] bool stop_requested() const noexcept;
    [[nodiscard]] std::optional<Termination> termination() const;
    void set_stop_observer(std::function<void(std::uint32_t)> observer);
    // Unwinds the calling thread's guest execution to its run() when it is
    // inside run(); returns when there is nothing to unwind (host code outside
    // guest execution). `process_stop` records why, for diagnostics.
    static void unwind_current_thread(bool process_stop);
    // Flushes host output and the fx profile, then ends the host process.
    [[noreturn]] void force_exit(std::uint32_t status, std::string_view diagnostic);

protected:
    [[nodiscard]] std::uint8_t read8(std::uint64_t address) const override;
    void write8(std::uint64_t address, std::uint8_t value) override;

public:
    struct Shared;

private:
    FxEngine(std::shared_ptr<Shared> shared, std::uint64_t entry, std::uint64_t argument,
             std::uint64_t stack_size);
    void ensure_stack();
    void enter_thread();
    bool enter_guest(void* guest_context);
    void leave_guest(void* guest_context) noexcept;
    void leave_thread() noexcept;
    void record_termination(int signal, std::uint32_t status, const std::string& reason);

    std::shared_ptr<Shared> shared_;
    CpuState state_{};
    std::uint64_t stack_size_{};
    bool main_{};
    bool stack_ready_{};
    bool initial_sp_set_{};
    unsigned depth_{};          // run/invoke_callback frames active on the owning thread
    std::mutex stack_lock_;     // stack_bounds() may run on another thread
    struct Stack;
    std::unique_ptr<Stack> stack_;
};

} // namespace transmute
