#pragma once

// Test-only EmitContext that mimics the core RegionEmitter's constraints
// (owner: atomics/system): 7 scratch GPRs handed out in the order r8, r9,
// r10, r11, rcx, rdx, rax and shared with unpinned operands; pinned guest
// registers live in callee-saved host registers, so read_gpr/write_gpr of the
// same guest register return the same host register; running out of scratch
// throws. The register hooks (temp_gpr_prefer/load_gpr/store_gpr) can be
// enabled or left at their EmitContext defaults.

#include "fx/translator/emit_context.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace transmute::fx::systest {

struct CoreLikeOptions {
    std::array<std::int8_t, 32> pins{};   // guest -> host register id, -1 = not pinned
    bool hooks = true;
    CoreLikeOptions() { pins.fill(-1); }
};

class CoreLikeContext final : public EmitContext {
public:
    CoreLikeContext(x64::Assembler& assembler, std::uint64_t pc, const CoreLikeOptions& options);

    x64::Assembler& as() override { return as_; }
    [[nodiscard]] std::uint64_t pc() const override { return pc_; }
    x64::Gp read_gpr(unsigned index, bool sp = false) override;
    x64::Gp write_gpr(unsigned index, bool sp = false) override;
    x64::Gp copy_gpr(unsigned index, bool sp = false) override;
    x64::Gp temp_gpr() override { return alloc(); }
    x64::Xmm read_vec(unsigned index) override;
    x64::Xmm write_vec(unsigned index) override;
    x64::Xmm temp_vec() override { return alloc_vec(); }
    void clobber_host_flags() override {}
    void set_flags(FlagsProducer producer) override;
    void set_nzcv(x64::Gp value) override;
    x64::Gp get_nzcv() override;
    x64::Cond condition(unsigned arm_condition) override;
    void mark_may_fault() override { ++fault_marks_; }
    x64::Gp call_helper(unsigned id, std::initializer_list<HelperArg> args, HelperEffects effects) override;

    x64::Gp temp_gpr_prefer(x64::Gp preferred) override;
    void load_gpr(x64::Gp dst, unsigned index, bool sp = false) override;
    void store_gpr(unsigned index, x64::Gp src, bool sp = false) override;

    void begin_instruction(std::uint64_t pc);
    void end_instruction();
    // Region entry/exit: pinned registers <-> GuestContext.
    void load_pins();
    void flush_pins();
    [[nodiscard]] unsigned max_scratch_used() const { return max_used_; }

private:
    x64::Gp alloc();
    x64::Xmm alloc_vec();
    [[nodiscard]] bool pinned(unsigned g) const { return options_.pins[g] >= 0; }
    [[nodiscard]] x64::Gp pin(unsigned g) const { return x64::gp(static_cast<unsigned>(options_.pins[g])); }
    void combine_flags(x64::Gp t, x64::Gp u);

    x64::Assembler& as_;
    std::uint64_t pc_;
    CoreLikeOptions options_;
    std::uint32_t free_{};
    unsigned used_{};
    unsigned max_used_{};
    unsigned next_vec_{};
    unsigned fault_marks_{};
    bool flags_pending_{};
    FlagsProducer producer_{};
    struct Pending { unsigned guest; x64::Gp host; };
    std::array<Pending, 12> pending_{};
    unsigned pending_count_{};
    struct PendingVec { unsigned guest; x64::Xmm host; };
    std::array<PendingVec, 8> pending_vec_{};
    unsigned pending_vec_count_{};
};

} // namespace transmute::fx::systest
