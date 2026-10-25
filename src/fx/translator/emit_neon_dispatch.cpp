// NEON family entry points: decode dispatch and describe() (owner: neon).
//
// describe() runs the emitter against a recording EmitContext, so register
// usage always matches what emit() actually reads and writes.
#include "fx/translator/emit_neon_internal.hpp"

#include <string>

namespace transmute::fx {

namespace neon {

unsigned dispatch_data(EmitContext& ctx, std::uint32_t w) {
    // Vector groups (bit 31 = 0, bits 28:24 = 01110 / 01111).
    if ((w & 0x9f200400u) == 0x0e200400u) return emit_three_same(ctx, w);
    if ((w & 0x9f200c00u) == 0x0e200000u) return emit_three_different(ctx, w);
    if ((w & 0x9f3e0c00u) == 0x0e200800u) return emit_two_reg_misc(ctx, w);
    if ((w & 0x9f3e0c00u) == 0x0e300800u) return emit_across_lanes(ctx, w);
    if ((w & 0x9fe08400u) == 0x0e000400u) return emit_copy(ctx, w);
    if ((w & 0xbfe08c00u) == 0x0e000000u) return emit_table(ctx, w);
    if ((w & 0xbf208c00u) == 0x0e000800u) return emit_permute(ctx, w);
    if ((w & 0xbfe08400u) == 0x2e000000u) return emit_extract(ctx, w);
    if ((w & 0x9f208400u) == 0x0e008400u) return emit_dot_product(ctx, w);
    if ((w & 0x9ff80400u) == 0x0f000400u) return emit_modified_immediate(ctx, w);
    if ((w & 0x9f800400u) == 0x0f000400u) return emit_shift_imm(ctx, w);
    if ((w & 0x9f000400u) == 0x0f000000u) return emit_indexed(ctx, w);
    // Scalar groups (bits 31:30 = 01).
    if ((w & 0xdf200400u) == 0x5e200400u) return emit_scalar_three_same(ctx, w);
    if ((w & 0xdf200c00u) == 0x5e200000u) return emit_scalar_three_different(ctx, w);
    if ((w & 0xdf208400u) == 0x5e008400u) return emit_scalar_three_same_extra(ctx, w);
    if ((w & 0xdf3e0c00u) == 0x5e200800u) return emit_scalar_two_reg_misc(ctx, w);
    if ((w & 0xdf3e0c00u) == 0x5e300800u) return emit_scalar_pairwise(ctx, w);
    if ((w & 0xdfe08400u) == 0x5e000400u) return emit_scalar_copy(ctx, w);
    if ((w & 0xdf800400u) == 0x5f000400u) return emit_scalar_shift_imm(ctx, w);
    if ((w & 0xdf000400u) == 0x5f000000u) return emit_scalar_indexed(ctx, w);
    return 0;
}

// Low bits of Vd that a data-processing word handled by dispatch_data()
// defines; A64 zeroes the bits above. 128: the whole register is defined (or
// partly kept); 0: no vector register is written. Mirrors dispatch_data's
// group decode. The tests check every note against the reference state.
unsigned result_width(std::uint32_t w) {
    const unsigned vq = bit(w, 30) ? 128 : 64;
    // Vector groups.
    if ((w & 0x9f200400u) == 0x0e200400u) return vq;                       // three same
    if ((w & 0x9f200c00u) == 0x0e200000u) {                                  // three different
        const unsigned opc = bits(w, 15, 12);
        return (opc == 0x4 || opc == 0x6) ? vq : 128;                        // [R]ADDHN/[R]SUBHN narrow; long/wide: 128
    }
    if ((w & 0x9f3e0c00u) == 0x0e200800u) return bits(w, 16, 12) == 0x13 ? 128 : vq;   // two-reg misc (SHLL widens)
    if ((w & 0x9f3e0c00u) == 0x0e300800u) {                                  // across lanes: scalar result
        const unsigned esize = 8u << bits(w, 23, 22);
        return bits(w, 16, 12) == 0x03 ? 2 * esize : esize;                  // [SU]ADDLV: twice as wide
    }
    if ((w & 0x9fe08400u) == 0x0e000400u) {                                  // copy
        if (bit(w, 29)) return 128;                                          // INS (element)
        switch (bits(w, 14, 11)) {
        case 0x0: case 0x1: return vq;                                       // DUP (element / general)
        case 0x3: return 128;                                                // INS (general)
        default: return 0;                                                   // SMOV / UMOV
        }
    }
    if ((w & 0xbfe08c00u) == 0x0e000000u) return vq;                         // TBL / TBX
    if ((w & 0xbf208c00u) == 0x0e000800u) return vq;                         // ZIP / UZP / TRN
    if ((w & 0xbfe08400u) == 0x2e000000u) return vq;                         // EXT
    if ((w & 0x9f208400u) == 0x0e008400u) return vq;                         // dot product / three same extra
    if ((w & 0x9ff80400u) == 0x0f000400u) {                                  // modified immediate
        if (bit(w, 29) && bits(w, 15, 12) == 0xe) {                          // MOVI with a 64-bit byte mask
            const unsigned imm8 = (bits(w, 18, 16) << 5) | bits(w, 9, 5);
            if (imm8 == 0 || (vq == 64 && (imm8 & 0xf0) == 0)) return 32;   // bytes 4.. are zero
        }
        return vq;
    }
    if ((w & 0x9f800400u) == 0x0f000400u) return bits(w, 15, 11) == 0x14 ? 128 : vq;   // shift imm ([SU]SHLL widens)
    if ((w & 0x9f000400u) == 0x0f000000u) {                                  // by element
        switch (bits(w, 15, 12)) {
        case 0x2: case 0x3: case 0x6: case 0x7: case 0xa: case 0xb: return 128;   // long forms
        default: return vq;
        }
    }
    // Scalar groups: results of at most 64 bits; element size where simple.
    const unsigned size = bits(w, 23, 22);
    if ((w & 0xdf200400u) == 0x5e200400u) return size == 3 ? 64 : 32;      // three same
    if ((w & 0xdf200c00u) == 0x5e200000u) return size == 1 ? 32 : 64;      // SQDMLAL etc: twice as wide
    if ((w & 0xdf208400u) == 0x5e008400u) return size == 3 ? 64 : 32;      // three same extra
    if ((w & 0xdf3e0c00u) == 0x5e200800u) return size == 3 ? 64 : 32;      // two-reg misc (narrow: dest size)
    if ((w & 0xdf3e0c00u) == 0x5e300800u) return 64;                        // ADDP Dd
    if ((w & 0xdfe08400u) == 0x5e000400u) return (bits(w, 20, 16) & 7) ? 32 : 64;   // DUP scalar (element size)
    if ((w & 0xdf800400u) == 0x5f000400u) return 64;                        // shift by immediate
    if ((w & 0xdf000400u) == 0x5f000000u) return 64;                        // by element
    return 128;
}

namespace {

// EmitContext that records register usage; the generated code is discarded.
class RecordingContext final : public EmitContext {
public:
    explicit RecordingContext(InstructionUse& use) : use_(use), as_(512) {}
    x64::Assembler& as() override { return as_; }
    [[nodiscard]] std::uint64_t pc() const override { return 0x0000'0001'0000'0000ULL; }
    Gp read_gpr(unsigned index, bool sp) override {
        if (index != 31 || sp) use_.gpr_read |= 1u << index;
        return gpr();
    }
    Gp write_gpr(unsigned index, bool sp) override {
        if (index != 31 || sp) use_.gpr_write |= 1u << index;
        return gpr();
    }
    Gp copy_gpr(unsigned index, bool sp) override { return read_gpr(index, sp); }
    Gp temp_gpr() override { return gpr(); }
    Xmm read_vec(unsigned index) override {
        use_.vec_read |= 1u << index;
        return vec();
    }
    Xmm write_vec(unsigned index) override {
        use_.vec_write |= 1u << index;
        return vec();
    }
    Xmm temp_vec() override { return vec(); }
    void clobber_host_flags() override {}
    void set_flags(FlagsProducer) override { use_.writes_flags = true; }
    void set_nzcv(Gp) override { use_.writes_flags = true; }
    Gp get_nzcv() override {
        use_.reads_flags = true;
        return gpr();
    }
    x64::Cond condition(unsigned) override {
        use_.reads_flags = true;
        return x64::Cond::e;
    }
    void mark_may_fault() override { use_.may_fault = true; }
    Gp call_helper(unsigned, std::initializer_list<HelperArg>, HelperEffects) override {
        return Gp::rax;
    }

private:
    Gp gpr() {
        static constexpr Gp pool[] = {Gp::rax, Gp::rcx, Gp::rdx, Gp::rbx, Gp::rbp, Gp::rsi, Gp::rdi,
                                      Gp::r8, Gp::r9, Gp::r10, Gp::r11, Gp::r12, Gp::r13, Gp::r14};
        return pool[next_gpr_++ % std::size(pool)];
    }
    Xmm vec() { return x64::xmm(next_vec_++ % 16); }

    InstructionUse& use_;
    x64::Assembler as_;
    unsigned next_gpr_{};
    unsigned next_vec_{};
};

template <unsigned (*Dispatch)(EmitContext&, std::uint32_t)>
bool describe_with(std::uint32_t word, InstructionUse& use) {
    InstructionUse recorded{};
    RecordingContext ctx(recorded);
    unsigned consumed = 0;
    try {
        begin_instruction_constants(ctx.as());
        consumed = Dispatch(ctx, word);
        flush_instruction_constants(ctx.as());
    } catch (...) {
        return false;
    }
    if (consumed == 0) return false;
    use = recorded;
    return true;
}

} // namespace

unsigned dispatch_memory(EmitContext& ctx, std::uint32_t w);
bool address_base(std::uint32_t w, unsigned& base, bool& indexed);   // emit_vector_memory.cpp

} // namespace neon

unsigned emit_neon(EmitContext& ctx, std::span<const std::uint32_t> words) {
    if (words.empty()) return 0;
    auto& as = ctx.as();
    neon::begin_instruction_constants(as);
    unsigned consumed = neon::emit_mask_reduce(ctx, words);
    if (consumed == 0) consumed = neon::emit_mull_pair(ctx, words);
    if (consumed == 0) consumed = neon::dispatch_data(ctx, words[0]);
    neon::flush_instruction_constants(as);
    // A64 zeroes the bits above a scalar or 64-bit vector result: tell the
    // context so later scalar operations can skip their own zeroing.
    if (consumed == 1) {
        const unsigned width = neon::result_width(words[0]);
        if (width != 0 && width < 128) ctx.note_vec_zero_above(words[0] & 31, width < 32 ? 32 : width);
    }
    return consumed;
}

bool describe_neon(std::uint32_t word, InstructionUse& use) {
    return neon::describe_with<neon::dispatch_data>(word, use);
}

unsigned emit_vector_memory(EmitContext& ctx, std::span<const std::uint32_t> words) {
    if (words.empty()) return 0;
    auto& as = ctx.as();
    neon::begin_instruction_constants(as);
    const unsigned consumed = neon::dispatch_memory(ctx, words[0]);
    neon::flush_instruction_constants(as);
    return consumed;
}

bool describe_vector_memory(std::uint32_t word, InstructionUse& use) {
    return neon::describe_with<neon::dispatch_memory>(word, use);
}

bool vector_memory_address_base(std::uint32_t word, unsigned& base, bool& indexed) {
    return neon::address_base(word, base, indexed);
}

const char* neon_family_name() {
    static const std::string name = std::string("neon") + neon::neon_knob_suffix();
    return name.c_str();
}

const char* vector_memory_family_name() {
    static const std::string name = std::string("vector_memory") + neon::neon_knob_suffix();
    return name.c_str();
}

} // namespace transmute::fx
