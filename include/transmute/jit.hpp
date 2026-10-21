#pragma once

#include "transmute/a64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace transmute {

enum class BlockOp : std::uint8_t {
    nop,
    move_zero,
    move_inverse,
    move_keep,
    float_immediate,
    float_register,
    float_to_integer_bits,
    integer_to_float_bits,
    float_negate_bits,
    float_absolute_bits,
    address,
    add_immediate,
    subtract_immediate,
    add_register,
    subtract_register,
    shift_register,
    divide_signed,
    divide_unsigned,
    count_leading_zero,
    reverse_bytes,
    extract_registers,
    add_extended,
    subtract_extended,
    logical_register,
    logical_immediate,
    unsigned_bitfield,
    signed_bitfield,
    merge_bitfield,
    conditional_select,
    conditional_compare_add,
    conditional_compare_subtract,
    multiply_add,
    multiply_subtract,
    widening_multiply_add,
    widening_multiply_subtract,
    multiply_high_signed,
    multiply_high_unsigned,
    simd_integer,
    guarded_fp,
    atomic_lse,
    load_byte,
    load_signed_byte,
    store_byte,
    load_halfword,
    load_signed_halfword,
    store_halfword,
    load_word,
    load_signed_word,
    store_word,
    load_doubleword,
    store_doubleword,
    load_pair,
    store_pair,
    load_vector,
    store_vector,
    load_vector_pair,
    store_vector_pair,
    compare_branch_zero,
    compare_branch_nonzero,
    test_branch_zero,
    test_branch_nonzero,
    branch,
    branch_link,
    branch_register,
    branch_link_register,
    conditional_branch,
    ret,
};

struct DecodedInstruction {
    BlockOp op;
    std::uint8_t destination{};
    std::uint8_t source{};
    std::uint8_t second{};
    std::uint8_t accumulator{};
    std::uint8_t shift{};
    std::uint8_t shift_type{};
    std::uint8_t extend_option{};
    std::uint8_t condition{};
    std::uint8_t logical_operation{};
    std::uint8_t field_width{};
    std::uint8_t memory_bytes{};
    bool wide{};
    bool set_flags{};
    bool invert_second{};
    bool increment_second{};
    bool bitfield_left{};
    bool compare_immediate{};
    bool unsigned_operands{};
    bool memory_writeback{};
    bool memory_preindex{};
    bool memory_register_offset{};
    bool memory_scaled{};
    bool memory_literal{};
    std::uint64_t immediate{};
};

struct DecodedBlock {
    std::uint64_t guest_pc{};
    std::vector<std::uint32_t> words;
    std::vector<DecodedInstruction> instructions;
};

struct JitBlockStat {
    std::uint64_t guest_pc{};
    std::size_t instructions{};
    std::uint64_t executions{};
};

struct JitFallbackStat {
    std::uint64_t guest_pc{};
    std::uint32_t opcode{};
    std::uint64_t samples{};
};

// Cold-path refill diagnostics are collected only with JIT profiling enabled.
// Causes describe the existing slot; rejections describe why refill failed.
struct JitSpanRefillStats {
    std::uint64_t successful{};
    std::uint64_t rejected{};
    std::array<std::uint64_t,
        static_cast<std::size_t>(NativeSpanMissKind::count)> causes{};
    std::array<std::uint64_t,
        static_cast<std::size_t>(NativeSpanRefillRejection::count)> rejections{};
};

struct JitStats {
    std::size_t live_blocks{};
    std::uint64_t compiled_blocks{};
    std::uint64_t native_blocks{};
    std::uint64_t fast_hits{};
    std::uint64_t native_guest_instructions{};
    std::uint64_t compiled_native_regions{};
    std::uint64_t inherited_native_regions{};
    std::uint64_t inherited_code_bytes{};
    std::uint64_t inherited_chunk_bytes{};
    std::uint64_t native_region_instructions{};
    std::uint64_t native_region_alias_hits{};
    std::uint64_t native_chain_edges{};
    std::uint64_t compiled_register_spans{};
    std::uint64_t native_read_misses{};
    std::uint64_t native_store_misses{};
    JitSpanRefillStats native_read_refills;
    JitSpanRefillStats native_store_refills;
    std::uint64_t native_journal_exits{};
    std::uint64_t native_serialized_access_exits{};
    std::uint64_t front_cache_hits{};
    std::uint64_t map_cache_hits{};
    std::uint64_t arena_reclamations{};
    std::uint64_t fallback_instructions{};
    std::uint64_t negative_cache_hits{};
    std::size_t negative_cache_entries{};
    std::uint64_t fallback_profile_observations{};
    std::uint64_t emitted_code_bytes{};
    std::uint64_t allocated_code_bytes{};
    std::uint64_t live_code_bytes{};
    double compilation_seconds{};
    double decoding_seconds{};
    std::vector<JitBlockStat> hottest_blocks;
    std::vector<JitFallbackStat> hottest_fallbacks;
};

// Stops before the first unsupported instruction. Returns no block if the
// first instruction is unsupported, leaving it for ReferenceEngine.
[[nodiscard]] std::optional<DecodedBlock> decode_integer_block(
    std::uint64_t guest_pc, std::span<const std::uint32_t> words);

// Ordered native regions keep supported control flow inside the region.
// Memory misses and guarded FP side exits occur before the current effect.
// The ordinary decoder retains its checked first-memory-operation contract.
[[nodiscard]] std::optional<DecodedBlock> decode_native_read_region(
    std::uint64_t guest_pc, std::span<const std::uint32_t> words);

// Windows x64 backend. Cache identity includes the guest address, exact decoded
// instruction words, and a caller-managed code generation. Invalidate a range
// after executable-memory writes or module unloads; changed words also miss.
// Each cache belongs to one execution thread: compilation briefly changes its
// inactive code chunk from RX to RW, then back to RX before execution.
class X64BlockCache {
public:
    X64BlockCache();
    ~X64BlockCache();
    X64BlockCache(const X64BlockCache&) = delete;
    X64BlockCache& operator=(const X64BlockCache&) = delete;

    // Best-effort snapshot into an empty receiver. The caller must quiesce
    // both caches and pin the shared published PE registry under exclusive
    // address-space ownership. Only matching-generation native owners whose
    // exact words occupy one executable, nonwritable PE section are inherited.
    // Shared chunks are permanently sealed; dispatch state remains private.
    // Negative entries, mapped code and private staged images are not copied.
    [[nodiscard]] std::size_t inherit_immutable(
        X64BlockCache& parent, std::uint64_t code_generation,
        std::span<PeImage* const> published_modules);

    // False leaves state untouched, allowing ReferenceEngine fallback.
    [[nodiscard]] bool execute(CpuState& state, std::uint64_t guest_pc,
                               std::span<const std::uint32_t> words,
                               std::uint64_t code_generation = 0,
                               GuestMemory* memory = nullptr);
    // Requires the engine's lock-owned native read lease. Returns actual
    // completed steps, including loop iterations, and leaves PC at the next
    // instruction or a checked-memory side exit. Zero leaves state unchanged.
    // requires_exclusive reports a shared alignment/owner exit after committing
    // the prefix; the caller must retry the current PC under exclusive access.
    [[nodiscard]] std::size_t execute_native_region(
        CpuState& state, std::uint64_t guest_pc,
        std::span<const std::uint32_t> words, std::uint64_t code_generation,
        std::size_t instruction_budget, GuestMemory* memory,
        bool* requires_exclusive = nullptr);
    // Fast runtime hit. Callers must advance code_generation or invalidate on
    // every executable write, unload, and protection change. A true
    // requires_exclusive result ends the shared chain before its next effect.
    [[nodiscard]] std::size_t execute_cached(CpuState& state,
                                             std::uint64_t guest_pc,
                                             std::uint64_t code_generation,
                                             std::size_t instruction_budget,
                                             bool* known_fallback = nullptr,
                                             GuestMemory* memory = nullptr,
                                             bool* requires_exclusive = nullptr);
    // Read-only dispatch classification for the engine's concurrency gate.
    // A shared lease may execute only an already compiled, store-free region.
    [[nodiscard]] bool has_read_only_native_region(
        std::uint64_t guest_pc, std::uint64_t code_generation) const noexcept;
    // Also accepts ordinary compiled register-only blocks. Legacy memory
    // helpers and native store owners remain excluded from shared execution.
    [[nodiscard]] bool has_read_only_cached_block(
        std::uint64_t guest_pc, std::uint64_t code_generation) const noexcept;
    // Shared readers/writers require naturally aligned single integer memory
    // accesses, including hardware-locked single-element LSE atomics.
    // Pair/vector memory and checked exclusive monitors remain serialized.
    [[nodiscard]] bool has_parallel_cached_block(
        std::uint64_t guest_pc, std::uint64_t code_generation) const noexcept;
    void remember_fallback(std::uint64_t guest_pc,
                           std::uint64_t code_generation);
    void record_fallback(std::uint64_t guest_pc, std::uint32_t opcode);
    void invalidate_range(std::uint64_t guest_pc, std::uint64_t bytes);
    void invalidate_all();
    [[nodiscard]] std::size_t cache_size() const;
    [[nodiscard]] const void* last_code_address() const noexcept;
    void set_profiling(bool enabled);
    [[nodiscard]] JitStats stats(std::size_t hottest = 10) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace transmute
