#pragma once

#include "transmute/pe_image.hpp"

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace transmute {

struct CpuState {
    std::array<std::uint64_t, 31> x{};
    std::array<std::array<std::uint64_t, 2>, 32> v{};
    std::uint64_t sp{};
    std::uint64_t pc{};
    std::uint32_t nzcv{};
    std::uint32_t fpcr{}; // Guest rounding, flush, and default-NaN control state.
    std::uint32_t fpsr{}; // Sticky guest floating-point status bits.
};

struct RunResult {
    std::uint32_t exit_code;
    std::uint64_t instructions;
};

struct ExecutionStart {
    std::uint64_t entry_pc;
    // Main guest stack size in bytes; 0 selects the 1 MiB default. Rounded up
    // to 64 KiB; at most 1 GiB.
    std::uint64_t stack_size{};
};

struct SupervisorCall {
    std::uint64_t pc;
    std::uint16_t immediate;
};

struct NativeConcurrencyStats {
    std::uint64_t shared_read_leases{};
    unsigned peak_shared_read_leases{};
    std::uint64_t profiled_worker_caches{};
    std::uint64_t inherited_worker_native_regions{};
};

struct NativeTopologyStats {
    std::uint64_t selective_refreshes{};
    std::uint64_t replayed_changes{};
    std::uint64_t retained_read_entries{};
    std::uint64_t retained_write_entries{};
    std::uint64_t evicted_read_entries{};
    std::uint64_t evicted_write_entries{};
    std::uint64_t full_flushes{};
};

class ReferenceEngine;
class X64BlockCache;
class ExecutionGate;
class NativeMemoryContext;
class NativeTopologyHistory;

enum class NativeSpanMissKind {
    empty, stale, collision, range, covered, count
};

enum class NativeSpanRefillRejection {
    none, unsupported_width, unmapped, permissions, external, boundary,
    image, executable, count
};

// Optional cold-path diagnosis. No host pointer or region token is inspected
// before the lease proves the current epoch; classification reads metadata only.
struct NativeSpanRefillDiagnostic {
    NativeSpanMissKind cause{NativeSpanMissKind::empty};
    NativeSpanRefillRejection rejection{NativeSpanRefillRejection::none};
};

// The backend reads this table only during a synchronous, lock-owned lease.
// Epoch and complete bounds must be checked before host_begin is dereferenced.
// page_tag identifies the fill/index page; a valid span may cover other pages.
struct NativeReadEntry {
    std::uint64_t page_tag{};
    std::uint64_t topology_epoch{};
    std::uint64_t guest_begin{};
    std::uint64_t guest_size{};
    const std::uint8_t* host_begin{};
};

struct NativeReadTable {
    static constexpr std::size_t entry_count = 1024;
    std::uint64_t current_epoch{1};
    std::array<NativeReadEntry, entry_count> entries{};
};

class NativeReadLease final {
public:
    NativeReadLease(const NativeReadLease&) = delete;
    NativeReadLease& operator=(const NativeReadLease&) = delete;
    NativeReadLease(NativeReadLease&&) = delete;
    NativeReadLease& operator=(NativeReadLease&&) = delete;
    [[nodiscard]] const NativeReadTable& table() const;
    // Shared native accesses must use naturally aligned guest and host
    // addresses. The cache passes this lease-owned mode to generated code.
    [[nodiscard]] bool requires_atomic_access() const noexcept;
    // Metadata-only refill after generated code has returned. Failure has no
    // effects and leaves permission/boundary behavior to checked guest access.
    [[nodiscard]] bool refill_read(std::uint64_t address, unsigned bytes,
        NativeSpanRefillDiagnostic* diagnostic = nullptr);

private:
    friend class ReferenceEngine;
    NativeReadLease(ReferenceEngine& engine,
                    const NativeMemoryContext& context);
    ReferenceEngine& engine_;
    const NativeMemoryContext& context_;
};

struct NativeWriteEntry {
    std::uint64_t page_tag{};
    std::uint64_t topology_epoch{};
    std::uint64_t guest_begin{};
    std::uint64_t guest_size{};
    std::uint8_t* host_begin{};
    // Opaque engine-issued target. Its low bit distinguishes published PE
    // data from an owned mapping; generated code only copies this value.
    void* region_token{};
};

struct NativeWriteTable {
    static constexpr std::size_t entry_count = 1024;
    std::uint64_t current_epoch{1};
    std::array<NativeWriteEntry, entry_count> entries{};
};

struct NativeWriteRecord {
    std::uint64_t guest_address;
    void* region_token; // Opaque target copied from the validated write entry.
    std::uint32_t bytes;
    std::uint32_t reserved;
};

struct NativeWriteJournal {
    static constexpr std::size_t capacity = 64;
    std::uint64_t count{};
    // Only the initialized prefix [0,count) is read. Avoid clearing the whole
    // journal on every scoped instruction/region entry.
    std::array<NativeWriteRecord, capacity> entries;
};

class NativeWriteLease final {
public:
    ~NativeWriteLease();
    NativeWriteLease(const NativeWriteLease&) = delete;
    NativeWriteLease& operator=(const NativeWriteLease&) = delete;
    NativeWriteLease(NativeWriteLease&&) = delete;
    NativeWriteLease& operator=(NativeWriteLease&&) = delete;
    [[nodiscard]] const NativeWriteTable& table() const;
    [[nodiscard]] NativeWriteJournal& journal();
    [[nodiscard]] bool refill_write(std::uint64_t address, unsigned bytes,
        NativeSpanRefillDiagnostic* diagnostic = nullptr);
    // Must run immediately after every generated-code return and before any
    // refill, checked memory operation, guest observer or lock release.
    void commit() noexcept;

private:
    friend class ReferenceEngine;
    NativeWriteLease(ReferenceEngine& engine,
                     const NativeMemoryContext& context);
    ReferenceEngine& engine_;
    const NativeMemoryContext& context_;
    NativeWriteJournal journal_;
};

struct GuestScalarWrite {
    std::uint64_t address;
    std::uint64_t value;
    unsigned width;
};

class GuestMemory {
public:
    struct RegionInfo {
        std::uint64_t base;
        std::uint64_t size;
        bool readable;
        bool writable;
        bool executable;
        bool image;
        bool mapped;
    };
    virtual ~GuestMemory() = default;
    [[nodiscard]] virtual std::uint8_t read8(std::uint64_t address) const = 0;
    virtual void write8(std::uint64_t address, std::uint8_t value) = 0;
    virtual void map_memory(std::uint64_t base, std::size_t size) = 0;
    // Reserve an inaccessible address range without allocating guest backing.
    virtual void reserve_memory(std::uint64_t base, std::uint64_t size);
    virtual void map_external_memory(std::uint64_t base, std::uint8_t* bytes,
                                     std::size_t size, bool writable) = 0;
    virtual void unmap_memory(std::uint64_t base) = 0;
    [[nodiscard]] virtual bool is_range_available(std::uint64_t base,
                                                   std::uint64_t size) const = 0;
    [[nodiscard]] virtual std::optional<RegionInfo> query_region(
        std::uint64_t address) const = 0;
    virtual void set_memory_access(std::uint64_t base, bool readable,
                                   bool writable, bool executable) = 0;
    [[nodiscard]] virtual std::uint32_t read32(std::uint64_t address) const;
    [[nodiscard]] virtual std::uint64_t read64(std::uint64_t address) const;
    virtual void write32(std::uint64_t address, std::uint32_t value);
    virtual void write64(std::uint64_t address, std::uint64_t value);
    // An ordered batch retains each checked scalar write's width, revision
    // and partial-fault behavior. Engines may pin backing once for the batch.
    virtual void write_scalar_fields(std::span<const GuestScalarWrite> fields);
    // Optional whole-width access to one mapped region. Returning no value or
    // false leaves boundary, image, and permission behavior to the checked API.
    [[nodiscard]] virtual std::optional<std::uint64_t> try_read_mapped(
        std::uint64_t address, unsigned bytes) const;
    virtual bool try_write_mapped(std::uint64_t address, unsigned bytes,
                                  std::uint64_t value);
    // Includes a single PE image extent as well as mapped regions. Failure
    // has no effects; callers retain checked bytewise boundary/fault behavior.
    [[nodiscard]] virtual std::optional<std::uint64_t> try_read_contiguous(
        std::uint64_t address, unsigned bytes) const;
    virtual bool try_write_contiguous(std::uint64_t address, unsigned bytes,
                                      std::uint64_t value);
    // Optional content revision for one readable owned mapping. An unchanged
    // revision proves no guest writes since the prior query. Unsupported,
    // external, image and boundary-spanning ranges return no revision.
    [[nodiscard]] virtual std::optional<std::uint64_t> write_revision(
        std::uint64_t address, std::uint64_t size) const;
    // Checked bulk transfers own their backing only for the call. No borrowed
    // host pointer escapes into native I/O or callbacks. Faults preserve the
    // transferred prefix (or suffix for backward move) and byte fault order.
    virtual void read_bytes(std::uint64_t address, std::span<std::uint8_t> bytes) const;
    virtual void write_bytes(std::uint64_t address, std::span<const std::uint8_t> bytes);
    virtual void fill_bytes(std::uint64_t address, std::uint8_t value, std::uint64_t count);
    virtual void copy_bytes(std::uint64_t destination, std::uint64_t source,
                            std::uint64_t count, bool move = false);
    // Returns bytes before the first zero character, or the complete bounded
    // span. Character width is 1 or 2; an optional flag reports termination.
    [[nodiscard]] virtual std::vector<std::uint8_t> read_string(
        std::uint64_t address, unsigned character_bytes, std::size_t maximum_characters,
        bool* terminated = nullptr) const;
    [[nodiscard]] virtual std::size_t string_length(
        std::uint64_t address, unsigned character_bytes, std::size_t maximum_characters,
        bool* terminated = nullptr) const;

private:
    friend class X64BlockCache;
    // Only the engine's private scoped adapter supplies a borrowed capability.
    // Ordinary/custom memory callers retain the virtual checked contract.
    [[nodiscard]] virtual NativeReadLease* native_read_lease();
    [[nodiscard]] virtual NativeWriteLease* native_write_lease();
};

using ImportDispatcher =
    std::function<std::optional<std::uint32_t>(const Import&, CpuState&, GuestMemory&)>;
// A resolved scalar-only host wrapper. No guest pointers, callbacks or bridge
// state may be accessed through this path. The Windows personality supplies it.
using NativeImportCall = void (*)(CpuState&) noexcept;
using NativeImportResolver = NativeImportCall (*)(const Import&) noexcept;
using GuestCallHandler = std::function<void(CpuState&, GuestMemory&)>;
// Empty optional continues at the instruction following SVC; a value exits.
// The handler runs without an execution lease and uses checked guest memory.
using SupervisorCallHandler = std::function<std::optional<std::uint32_t>(
    const SupervisorCall&, CpuState&, GuestMemory&)>;

struct JitStats;

// A deliberately small, exact baseline. Unsupported instructions fail visibly.
// The optimized backend will implement the same guest memory and state contract.
class ReferenceEngine : public GuestMemory {
public:
    explicit ReferenceEngine(PeImage& image, ImportDispatcher dispatcher = {});
    explicit ReferenceEngine(ExecutionStart start);
    ~ReferenceEngine();
    ReferenceEngine(const ReferenceEngine&) = delete;
    ReferenceEngine& operator=(const ReferenceEngine&) = delete;
    ReferenceEngine(ReferenceEngine&&) = delete;
    ReferenceEngine& operator=(ReferenceEngine&&) = delete;
    void add_module(PeImage& module);
    // Configure before starting guest workers. Bind each existing/future import
    // once; unresolved calls retain the complete dispatcher and memory contract.
    void set_native_import_resolver(NativeImportResolver resolver);
    void map_memory(std::uint64_t base, std::size_t size) override;
    void reserve_memory(std::uint64_t base, std::uint64_t size) override;
    void map_external_memory(std::uint64_t base, std::uint8_t* bytes,
                             std::size_t size, bool writable) override;
    void unmap_memory(std::uint64_t base) override;
    [[nodiscard]] bool is_range_available(std::uint64_t base,
                                           std::uint64_t size) const override;
    [[nodiscard]] std::optional<RegionInfo> query_region(
        std::uint64_t address) const override;
    void set_memory_access(std::uint64_t base, bool readable,
                           bool writable, bool executable) override;
    void set_memory_writable(std::uint64_t base, bool writable);
    void set_initial_register(unsigned index, std::uint64_t value);
    void set_initial_vector(unsigned index, std::array<std::uint64_t, 2> value);
    void set_initial_stack_pointer(std::uint64_t value);
    // Configure before starting guest workers. Each worker copies the handler.
    void set_supervisor_call_handler(SupervisorCallHandler handler);
    void set_instruction_limit(std::uint64_t limit);
    void set_jit_enabled(bool enabled);
    void set_jit_profiling(bool enabled);
    [[nodiscard]] std::uint64_t jit_executed_instructions() const noexcept {
        return jit_executed_instructions_;
    }
    [[nodiscard]] JitStats jit_stats(std::size_t hottest = 10) const;
    // Counts only engines with JIT profiling enabled, across their shared
    // address space. Normal execution does not update these counters.
    [[nodiscard]] NativeConcurrencyStats native_concurrency_stats() const noexcept;
    // Engine-local cold-path counters. Entry counts are observations at a
    // refresh, not distinct mappings or a count of dynamic memory accesses.
    [[nodiscard]] NativeTopologyStats native_topology_stats() const noexcept;
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> stack_bounds() const noexcept;
    void initialize_dlls(std::span<PeImage* const> dependency_order);
    void stage_module(PeImage& module);
    void commit_staged_modules();
    void discard_staged_modules();
    std::uint64_t invoke_callback(std::uint64_t address, std::uint64_t argument);
    std::uint64_t invoke_callback(std::uint64_t address,
                                  std::span<const std::uint64_t> arguments);
    // Returns an address suitable for a guest function pointer or vtable slot.
    // Returning from the handler resumes guest execution at LR.
    // The shared registry owns the handler until unregister_guest_call(). An
    // in-flight call owns a copy and can finish after unregistration; callers
    // should capture shared ownership of any state needed by that call.
    [[nodiscard]] std::uint64_t register_guest_call(GuestCallHandler handler);
    void unregister_guest_call(std::uint64_t address);
    // stack_size 0 uses the process default worker stack size (the PE image's
    // SizeOfStackReserve, or 1 MiB for format-neutral starts).
    [[nodiscard]] std::unique_ptr<ReferenceEngine> fork_thread(
        std::uint64_t entry, std::uint64_t argument, std::uint64_t stack_size = 0);
    // Replaces the main stack with one of `size` bytes. Only valid on the main
    // engine before any worker exists and before guest code runs on it.
    void resize_stack(std::uint64_t size);
    void set_default_worker_stack_size(std::uint64_t size);
    [[nodiscard]] std::uint64_t default_worker_stack_size() const noexcept;
    [[nodiscard]] static ReferenceEngine* current() noexcept;
    RunResult run(std::uint64_t maximum_instructions = 0);
    // Executes exactly one instruction on `state` without the run loop, import
    // dispatch or JIT. Memory goes through this engine's virtual GuestMemory
    // functions, so a derived engine can supply direct host memory (the fx
    // engine's oracle and fallback). On an exception `state` is unchanged.
    // Returns true when the instruction requests a guest exit.
    bool step_external(CpuState& state, std::uint32_t instruction);
    [[nodiscard]] const CpuState& state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t read32(std::uint64_t address) const override;
    [[nodiscard]] std::uint64_t read64(std::uint64_t address) const override;
    void write32(std::uint64_t address, std::uint32_t value) override;
    void write64(std::uint64_t address, std::uint64_t value) override;
    void write_scalar_fields(std::span<const GuestScalarWrite> fields) override;
    [[nodiscard]] std::optional<std::uint64_t> try_read_mapped(
        std::uint64_t address, unsigned bytes) const override;
    bool try_write_mapped(std::uint64_t address, unsigned bytes,
                          std::uint64_t value) override;
    [[nodiscard]] std::optional<std::uint64_t> try_read_contiguous(
        std::uint64_t address, unsigned bytes) const override;
    bool try_write_contiguous(std::uint64_t address, unsigned bytes,
                              std::uint64_t value) override;
    [[nodiscard]] std::optional<std::uint64_t> write_revision(
        std::uint64_t address, std::uint64_t size) const override;
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

protected:
    [[nodiscard]] std::uint8_t read8(std::uint64_t address) const override;
    void write8(std::uint64_t address, std::uint8_t value) override;

private:
    friend class NativeReadLease;
    friend class NativeWriteLease;
    friend class NativeMemoryContext;
    class ScopedMemoryAccess;
    class AddressSpaceLock;
    struct MemoryRegion {
        std::uint64_t base;
        std::vector<std::uint8_t> bytes;
        std::uint8_t* external{};
        std::size_t external_size{};
        bool readable;
        bool writable;
        bool executable;
        bool mapped;
        std::uint64_t revision{};
        std::uint64_t backing_identity{};
        [[nodiscard]] std::size_t size() const noexcept {
            return external == nullptr ? bytes.size() : external_size;
        }
        [[nodiscard]] std::uint8_t read(std::size_t offset) const noexcept {
            return external == nullptr ? bytes[offset] : external[offset];
        }
        void write(std::size_t offset, std::uint8_t value) noexcept {
            if (external == nullptr) bytes[offset] = value;
            else external[offset] = value;
        }
    };
    struct Reservation {
        std::uint64_t address{};
        unsigned bytes{};
        bool valid{};
        std::uint64_t granule_epoch{};
        std::array<std::uint64_t, 16> backing_identities{};
    };
    struct BoundImport {
        std::shared_ptr<const Import> imported;
        NativeImportCall native{};
    };
    struct SharedAddressSpace {
        SharedAddressSpace();
        ~SharedAddressSpace();
        // Execution leases pin all backing and metadata. Checked effects and
        // topology changes acquire exclusive ownership before mutex.
        std::unique_ptr<ExecutionGate> execution_gate;
        std::recursive_mutex mutex;
        std::atomic<unsigned> profiled_shared_readers{};
        std::atomic<unsigned> peak_profiled_shared_readers{};
        std::atomic<std::uint64_t> profiled_shared_read_leases{};
        std::atomic<std::uint64_t> profiled_worker_caches{};
        std::atomic<std::uint64_t> inherited_worker_native_regions{};
        std::vector<PeImage*> modules;
        std::map<std::uint64_t, PeImage*> modules_by_base;
        std::unordered_map<const PeImage*, std::uint64_t> module_identities;
        std::unordered_map<std::uint64_t, BoundImport> import_entries;
        NativeImportResolver native_import_resolver{};
        std::map<std::uint64_t, MemoryRegion> regions;
        std::map<std::uint64_t, std::uint64_t> reserved_ranges;
        std::uint64_t generation{};
        // Never match a borrowed checked hint again after generation reuse.
        bool checked_hints_usable{true};
        // All backing/view mutations append while holding exclusive execution
        // ownership. A lagging engine falls back to a full epoch invalidation
        // when the bounded history is incomplete. Era changes prevent replay
        // after generation wrap; era exhaustion permanently disables replay.
        std::unique_ptr<NativeTopologyHistory> topology_history;
        std::uint64_t code_generation{};
        std::uint64_t next_write_revision{1};
        std::uint64_t next_backing_identity{1};
        // Collisions can cause a permitted spurious exclusive-store failure,
        // but a write to the reserved granule can never be missed.
        std::array<std::uint64_t, 1U << 16> exclusive_write_epochs{};
        std::uint64_t next_stack_index{1};
        std::uint64_t main_stack_size{};
        std::uint64_t default_worker_stack_size{};
        // Next worker stack top, allocated downwards; 0 before the first worker.
        std::uint64_t worker_stack_cursor{};
        std::unordered_map<std::uint64_t, GuestCallHandler> guest_calls;
        std::uint64_t next_guest_call_index{};
    };
    ReferenceEngine(ReferenceEngine& parent, std::uint64_t stack_size);
    [[nodiscard]] BoundImport make_import_binding(const Import& imported) const;
    [[nodiscard]] std::uint64_t reg(unsigned index, bool stack_pointer = false) const;
    [[nodiscard]] PeImage* module_at(std::uint64_t address) const;
    void set_reg(unsigned index, std::uint64_t value, bool is_64_bit,
                 bool stack_pointer = false);
    [[nodiscard]] bool execute(std::uint32_t instruction);
    // Reads hold a native read lease or the address-space mutex; writes hold
    // exclusive execution ownership and the address-space mutex.
    [[nodiscard]] std::optional<std::uint64_t> read_mapped(
        std::uint64_t address, unsigned bytes) const;
    bool write_mapped(std::uint64_t address, unsigned bytes, std::uint64_t value);
    [[nodiscard]] std::optional<std::uint64_t> read_contiguous(
        std::uint64_t address, unsigned bytes) const;
    bool write_contiguous(std::uint64_t address, unsigned bytes,
                           std::uint64_t value);
    struct VisiblePeInterval {
        std::uint64_t base;
        std::uint64_t size;
    };
    // A private staged PE can be partially shadowed by another engine's map.
    // Published images return their complete extent without a region lookup.
    [[nodiscard]] std::optional<VisiblePeInterval> visible_pe_interval(
        const PeImage& module, std::uint64_t address) const;
    void record_memory_write(std::uint64_t address, unsigned bytes,
                              bool executable);
    [[nodiscard]] std::uint64_t advance_write_revision();
    [[nodiscard]] std::uint64_t advance_backing_identity();
    [[nodiscard]] std::array<std::uint64_t, 16> reservation_backings(
        std::uint64_t address, unsigned bytes) const;
    [[nodiscard]] std::optional<std::uint64_t> mapped_write_revision(
        std::uint64_t address, std::uint64_t size) const;
    void refresh_native_reads();
    void record_topology_change(std::uint64_t base = 0, std::uint64_t size = 0) noexcept;
    [[nodiscard]] bool refill_native_read(std::uint64_t address, unsigned bytes,
        NativeSpanRefillDiagnostic* diagnostic);
    [[nodiscard]] bool refill_native_write(std::uint64_t address, unsigned bytes,
        NativeSpanRefillDiagnostic* diagnostic);
    void commit_native_writes(NativeWriteJournal& journal) noexcept;
    struct BulkMemoryView {
        std::uint64_t base;
        std::size_t size;
        std::uint8_t* data;
        MemoryRegion* region;
        PeImage* image;
    };
    [[nodiscard]] std::optional<BulkMemoryView> bulk_memory_view(
        std::uint64_t address, bool writable) const;
    void record_bulk_write(const BulkMemoryView& view, std::uint64_t address,
                           std::size_t bytes, std::uint64_t operations);
    [[nodiscard]] std::size_t scan_string(std::uint64_t address,
        unsigned character_bytes, std::size_t maximum_characters,
        std::vector<std::uint8_t>* snapshot, bool* terminated) const;

    PeImage* image_{};
    ImportDispatcher dispatcher_;
    SupervisorCallHandler supervisor_call_handler_;
    CpuState state_{};
    CpuState initial_state_{};
    std::shared_ptr<SharedAddressSpace> address_space_;
    std::vector<PeImage*> staged_modules_;
    std::map<std::uint64_t, PeImage*> staged_modules_by_base_;
    std::unordered_map<const PeImage*, std::uint64_t> staged_module_identities_;
    std::unordered_map<std::uint64_t, BoundImport> staged_import_entries_;
    std::uint64_t stack_top_{};
    std::uint64_t stack_size_{};
    bool worker_stack_{};
    mutable const MemoryRegion* read_region_hint_{};
    MemoryRegion* write_region_hint_{};
    mutable std::uint64_t read_hint_generation_{};
    std::uint64_t write_hint_generation_{};
    // Larger span caches live on the heap so engines and callbacks do not
    // consume another 88 KiB of the Windows thread stack.
    std::unique_ptr<NativeReadTable> native_read_table_{
        std::make_unique<NativeReadTable>()};
    std::unique_ptr<NativeWriteTable> native_write_table_{
        std::make_unique<NativeWriteTable>()};
    // Profiling remembers which cleared slots were previously populated. This
    // distinguishes topology invalidation from cold empties without changing
    // the native tables or their existing invalidation behavior.
    std::bitset<NativeReadTable::entry_count> native_read_invalidated_slots_;
    std::bitset<NativeWriteTable::entry_count> native_write_invalidated_slots_;
    std::uint64_t native_read_generation_{UINT64_MAX};
    std::uint64_t native_topology_era_{};
    bool native_topology_seen_{};
    NativeTopologyStats native_topology_stats_;
    std::uint64_t instruction_limit_{10'000'000};
    std::unique_ptr<X64BlockCache> jit_cache_;
    std::uint64_t jit_seen_generation_{};
    std::uint64_t jit_executed_instructions_{};
    std::uint64_t jit_fallback_instructions_{};
    bool jit_enabled_{};
    bool jit_profiling_{};
    bool force_exclusive_jit_{};
    Reservation reservation_{};
};

} // namespace transmute
