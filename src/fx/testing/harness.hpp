#pragma once

// Differential test harness for instruction emitters (owner: integration).
//
//   GuestContext ctx = random_context(seed);
//   GuestContext expected = ctx;
//   oracle_step(expected, word);                 // reference interpreter
//   run_family(ctx, my_family_emit, {word});     // translated x64
//   compare_contexts(expected, ctx, "label");    // reports FX failures
//
// MemoryEmitContext keeps every guest register in GuestContext (no pinning)
// and materialises flags eagerly. It is the simplest correct EmitContext and
// the reference behaviour for the core translator's optimized context.

#include "fx/translator/emit_context.hpp"
#include "transmute/fx/context.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace transmute::fx::testing {

class MemoryEmitContext final : public EmitContext {
public:
    MemoryEmitContext(x64::Assembler& assembler, std::uint64_t pc);
    x64::Assembler& as() override { return as_; }
    [[nodiscard]] std::uint64_t pc() const override { return pc_; }
    x64::Gp read_gpr(unsigned index, bool sp = false) override;
    x64::Gp write_gpr(unsigned index, bool sp = false) override;
    x64::Gp copy_gpr(unsigned index, bool sp = false) override;
    x64::Gp temp_gpr() override;
    x64::Xmm read_vec(unsigned index) override;
    x64::Xmm write_vec(unsigned index) override;
    x64::Xmm temp_vec() override;
    void clobber_host_flags() override {}
    void set_flags(FlagsProducer producer) override;
    void set_nzcv(x64::Gp value) override;
    x64::Gp get_nzcv() override;
    x64::Cond condition(unsigned arm_condition) override;
    void mark_may_fault() override { ++fault_marks_; }
    x64::Gp call_helper(unsigned id, std::initializer_list<HelperArg> args,
                        HelperEffects effects) override;

    // Stores pending register writes. Call after the emitter returns.
    void end_instruction();
    // Starts the next instruction at `pc` (temps are released).
    void begin_instruction(std::uint64_t pc);
    [[nodiscard]] unsigned fault_marks() const { return fault_marks_; }

private:
    x64::Gp allocate_gpr();
    x64::Xmm allocate_vec();
    struct PendingGpr { unsigned guest; x64::Gp host; };
    struct PendingVec { unsigned guest; x64::Xmm host; };
    x64::Assembler& as_;
    std::uint64_t pc_;
    unsigned next_gpr_{};
    unsigned next_vec_{};
    unsigned fault_marks_{};
    std::vector<PendingGpr> pending_gpr_;
    std::vector<PendingVec> pending_vec_;
};

// Executable snippet: copies assembled bytes into RWX memory.
class Snippet {
public:
    explicit Snippet(const std::vector<std::uint8_t>& code);
    ~Snippet();
    Snippet(const Snippet&) = delete;
    Snippet& operator=(const Snippet&) = delete;
    // Calls the code with r15 = &ctx + kContextBias, host callee-saved
    // registers preserved, rsp 16-byte aligned with 32 bytes of shadow space.
    // `mxcsr` is loaded before the call and the host value restored after.
    void run(GuestContext& ctx, std::uint32_t mxcsr = 0x1f80) const;
    [[nodiscard]] const std::uint8_t* code() const { return code_; }

private:
    std::uint8_t* code_{};
    std::size_t size_{};
};

// Emits `family` for words[0..] (as many as it consumes, repeatedly, until
// all `words` are consumed) through MemoryEmitContext and runs it on ctx.
// ctx.pc is advanced by 4 per consumed word. Returns false (and reports a
// failure) if the family declined an instruction.
bool run_family(GuestContext& ctx, FamilyEmitFn family, std::span<const std::uint32_t> words,
                std::uint32_t mxcsr = 0x1f80);

// Reference interpreter step (direct memory). Returns false if the reference
// interpreter does not implement the instruction.
bool oracle_step(GuestContext& ctx, std::uint32_t word);

// Deterministic pseudo-random architectural state. Registers listed in
// `pointer_registers` are set to `pointer_value` (e.g. a test buffer).
GuestContext random_context(std::uint64_t seed);
std::uint64_t random_u64(std::uint64_t& state);

struct CompareOptions {
    bool compare_fpsr = false;
    bool compare_vectors = true;
    bool compare_flags = true;
};
// Reports FX failures for every differing field. Returns true if equal.
bool compare_contexts(const GuestContext& expected, const GuestContext& actual,
                      const std::string& label, const CompareOptions& options = {});

// Aligned test memory buffer (page-aligned, zero-filled, PAGE_READWRITE).
class Buffer {
public:
    explicit Buffer(std::size_t size);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    [[nodiscard]] std::uint8_t* data() const { return data_; }
    [[nodiscard]] std::uint64_t address() const { return reinterpret_cast<std::uint64_t>(data_); }
    [[nodiscard]] std::size_t size() const { return size_; }

private:
    std::uint8_t* data_{};
    std::size_t size_{};
};

} // namespace transmute::fx::testing
