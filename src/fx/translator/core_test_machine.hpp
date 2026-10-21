#pragma once

// Test-only dispatcher for translated regions (owner: translator core).
//
// Implements the exit protocol of docs/FX_ENGINE.md with an entry stub that
// saves host callee-saved state and ctx->host_rsp, and stubs that record the
// exit and return to C++. The run loop translates on miss, links direct
// exits (patch rel32), interprets interpret exits with fx::interpret_one and
// follows indirect branches/RSB misses through a C++ lookup.

#include "fx/translator/translator.hpp"
#include "transmute/fx/context.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace transmute::fx::coretest {

// Guest code in host memory [begin, end).
class BufferCode final : public CodeSource {
public:
    BufferCode(std::uint64_t begin, std::uint64_t end) : begin_(begin), end_(end) {}
    bool read(std::uint64_t address, std::uint32_t& word) const override;
    bool function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const override;
    bool has_entry(std::uint64_t pc) const override;
    std::uint64_t fn_begin{}, fn_end{};   // optional bounds reported for every address
    std::vector<std::uint64_t> covered;   // pcs reported by has_entry()

private:
    std::uint64_t begin_, end_;
};

struct RunResult {
    ExitReason reason{ExitReason::none};
    std::uint64_t arg{};
    bool ok{};
    std::string error;
    unsigned dispatches{};
    unsigned translations{};
    unsigned links{};
    unsigned lookups{};
    unsigned interprets{};
    unsigned interrupts{};
    unsigned tier_ups{};
};

class TestMachine {
public:
    explicit TestMachine(TranslateOptions options = {}, std::size_t arena_bytes = 64u << 20);
    ~TestMachine();
    TestMachine(const TestMachine&) = delete;
    TestMachine& operator=(const TestMachine&) = delete;

    // Installs stubs and helpers into ctx.
    void prepare(GuestContext& ctx) const;
    // Host entry for guest `pc`, translating a region on a miss. nullptr if
    // `pc` cannot be read.
    const void* entry_for(std::uint64_t pc, const CodeSource& code);
    // Runs translated code from ctx.pc until ctx.pc == stop_pc or a BRK/SVC
    // exit. `max_dispatches` bounds C++ dispatch round trips.
    RunResult run(GuestContext& ctx, const CodeSource& code, std::uint64_t stop_pc,
                  unsigned max_dispatches = 100000);
    // Enters translated code once.
    void enter(GuestContext& ctx, const void* host) const;
    // Forgets all translations (code arena is reused).
    void reset();

    bool link = true;
    bool inline_lookup = true;   // ctx->arena_base set: translated code inlines indirect lookups
    TranslateOptions options;
    std::vector<TranslatedRegion> regions;
    void set_helper(unsigned id, const void* fn) { helpers_[id] = fn; }

private:
    std::uint8_t* allocate(std::size_t bytes);
    const void* install(TranslatedRegion region, bool replace);
    std::uint8_t* arena_{};
    std::size_t size_{}, used_{}, stubs_end_{};
    const void* enter_{};
    const void* stub_link_{};
    const void* stub_indirect_{};
    const void* stub_exit_{};
    const void* helpers_[kHelperSlots]{};
    std::unordered_map<std::uint64_t, const void*> entries_;
    static constexpr std::uint64_t kLookupSlots = 4096;
    std::vector<std::uint64_t> lookup_;
};

// Fresh context for tests: random_context(seed) with the stack pointer,
// pc and RSB reset.
GuestContext test_context(std::uint64_t seed);

// Differential runner: places a program in a code buffer (followed by
// BRK #0xdead), runs it translated and with the oracle (stepping word by
// word) from the same context and data memory, and compares the results.
class DiffRunner {
public:
    static constexpr std::size_t kCodeBytes = 1u << 20;
    static constexpr std::size_t kDataBytes = 256u << 10;
    static constexpr std::uint64_t kProgramOffset = 4096;   // literal pool below

    DiffRunner();
    ~DiffRunner();
    [[nodiscard]] std::uint64_t code_base() const { return code_; }
    [[nodiscard]] std::uint64_t program_base() const { return code_ + kProgramOffset; }
    [[nodiscard]] std::uint64_t data_base() const { return data_; }
    // Random context with x16/x17/SP pointing into data memory, x14 small,
    // x12 small and x30 = the stop sentinel.
    [[nodiscard]] GuestContext context(std::uint64_t seed) const;
    // Writes `words` at program_base() + BRK. Returns false on mismatch
    // (failures are reported through the test framework).
    bool run(const std::vector<std::uint32_t>& words, const GuestContext& initial, const std::string& label,
             unsigned max_steps = 200000);
    // Translated execution only (for instructions the oracle lacks). Result in `actual`.
    bool run_translated(const std::vector<std::uint32_t>& words, const GuestContext& initial);

    TestMachine machine;
    RunResult last;
    GuestContext expected{}, actual{};
    unsigned oracle_steps{};
    bool compare_memory = true;
    bool report = true;
    // Fails the run if a translated region embeds a guest code address
    // (an 8-byte window in [code_base, code_base + kCodeBytes)) outside its
    // fixup slots, i.e. the code would not be relocatable.
    bool check_relocatable = true;
    unsigned unrecorded_addresses = 0;
    // Restores the data memory pattern (as before each run).
    void reset_memory();

private:
    std::uint64_t code_{}, data_{};
    std::vector<std::uint8_t> pattern_;
    std::vector<std::uint8_t> expected_memory_;
};

} // namespace transmute::fx::coretest
