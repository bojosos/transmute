// Test utilities for the fp family (owner: fp).
#include "fx/translator/emit_fp_test_util.hpp"
#include "fx/translator/emit_neon.hpp"   // region constant pool for instruction counts

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace transmute::fx::fptest {

using x64::Gp;
using x64::Xmm;
using x64::ptr;

std::uint64_t f2b(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
std::uint64_t d2b(double d) {
    std::uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}

namespace {

std::uint64_t lane_mask(unsigned es) { return es == 64 ? ~0ULL : ((1ULL << es) - 1); }
const char* variant_name(const AliasOptions& v) {
    if (v.zero32 || v.zero64) return v.mem_sources ? " [alias mem zero]" : " [alias zero]";
    return v.mem_sources ? " [alias mem]" : " [alias]";
}
unsigned exp_bits(unsigned es) { return es == 64 ? 11 : es == 32 ? 8 : 5; }

std::vector<std::uint64_t> make_specials(unsigned es) {
    std::vector<std::uint64_t> v;
    if (es == 32) {
        const std::uint32_t s[] = {
            0x00000000, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fc12345,
            0x7f800001, 0xff800001, 0x7fa00000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff,
            0x00400000, 0x00200000, 0x001fffff, 0x80200000, 0x00800000, 0x80800000, 0x00800001,
            0x7f7fffff, 0xff7fffff, 0x7f000000, 0x7e800000, 0x3f800000, 0xbf800000, 0x3f000000,
            0xbf000000, 0x3fc00000, 0xbfc00000, 0x40200000, 0xc0200000, 0x3effffff, 0xbeffffff,
            0x3f7fffff, 0x40400000, 0x4b000000, 0x4b000001, 0x4affffff, 0xcaffffff, 0x4b800000,
            0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001, 0x4f7fffff, 0x4f800000, 0x5effffff,
            0x5f000000, 0xdf000000, 0xdf000001, 0x5f7fffff, 0x5f800000, 0x477fe000, 0x47800000,
            0x477ff000, 0x33800000, 0x38800000, 0xc7000000, 0x46fffe00, 0x47000000, 0xbf400000,
            0x3e800000, 0x40000000, 0xc0400000, 0x41200000, 0x3dcccccd, 0x40490fdb, 0xc61c4000,
        };
        for (auto x : s) v.push_back(x);
    } else if (es == 64) {
        const std::uint64_t s[] = {
            0x0000000000000000, 0x8000000000000000, 0x7ff0000000000000, 0xfff0000000000000,
            0x7ff8000000000000, 0xfff8000000000000, 0x7ff8000000012345, 0x7ff0000000000001,
            0xfff0000000000001, 0x7ff4000000000000, 0x0000000000000001, 0x8000000000000001,
            0x000fffffffffffff, 0x800fffffffffffff, 0x0008000000000000, 0x0004000000000000,
            0x0003ffffffffffff, 0x0010000000000000, 0x8010000000000000, 0x0010000000000001,
            0x7fefffffffffffff, 0xffefffffffffffff, 0x7fe0000000000000, 0x7fd0000000000000,
            0x3ff0000000000000, 0xbff0000000000000, 0x3fe0000000000000, 0xbfe0000000000000,
            0x3ff8000000000000, 0xbff8000000000000, 0x4004000000000000, 0xc004000000000000,
            0x3fdfffffffffffff, 0xbfdfffffffffffff, 0x3fefffffffffffff, 0x4008000000000000,
            0x4330000000000000, 0x4330000000000001, 0x432fffffffffffff, 0x4340000000000000,
            0x41dfffffffc00000, 0x41dfffffffe00000, 0x41e0000000000000, 0xc1e0000000000000,
            0xc1e0000000100000, 0xc1e0000000200000, 0x41efffffffe00000, 0x41effffffff00000,
            0x41f0000000000000, 0x43dfffffffffffff, 0x43e0000000000000, 0xc3e0000000000000,
            0xc3e0000000000001, 0x43efffffffffffff, 0x43f0000000000000, 0x36a0000000000000,
            0x3810000000000000, 0x47efffffe0000000, 0x47effffff0000000, 0x47f0000000000000,
            0x380fffffe0000000, 0x3e70000000000000, 0x40effc0000000000, 0x40effe0000000000,
            0x3fb999999999999a, 0x400921fb54442d18, 0xbfe8000000000000, 0x4000000000000000,
        };
        for (auto x : s) v.push_back(x);
    } else {
        const std::uint16_t s[] = {
            0x0000, 0x8000, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7e55, 0x7c01, 0xfc01, 0x7d00,
            0x0001, 0x8001, 0x03ff, 0x83ff, 0x0200, 0x0100, 0x00ff, 0x0400, 0x8400, 0x0401,
            0x7bff, 0xfbff, 0x7800, 0x3c00, 0xbc00, 0x3800, 0xb800, 0x3e00, 0xbe00, 0x4100,
            0xc100, 0x37ff, 0xb7ff, 0x3bff, 0x4200, 0x6400, 0x6401, 0x63ff, 0x6800, 0x77ff,
            0x7800, 0xf800, 0x4000, 0x4900, 0x2e66, 0x4248, 0xba00, 0x3400,
        };
        for (auto x : s) v.push_back(x);
    }
    return v;
}

} // namespace

const std::vector<std::uint64_t>& specials(unsigned es) {
    static const std::vector<std::uint64_t> s16 = make_specials(16), s32 = make_specials(32),
                                           s64 = make_specials(64);
    return es == 64 ? s64 : es == 32 ? s32 : s16;
}

bool is_nan(std::uint64_t v, unsigned es) {
    const unsigned e = exp_bits(es), f = es - e - 1;
    const std::uint64_t emask = ((1ULL << e) - 1) << f, fmask = (1ULL << f) - 1;
    return (v & emask) == emask && (v & fmask) != 0;
}

bool is_snan(std::uint64_t v, unsigned es) {
    const unsigned e = exp_bits(es), f = es - e - 1;
    return is_nan(v, es) && ((v >> (f - 1)) & 1) == 0;
}

std::uint64_t random_fp(Rng& rng, unsigned es, bool allow_snan) {
    const unsigned e = exp_bits(es), f = es - e - 1;
    const std::uint64_t bias = (1ULL << (e - 1)) - 1;
    std::uint64_t v = 0;
    switch (rng.below(10)) {
    case 0: case 1: case 2: {
        const auto& s = specials(es);
        v = s[rng.below(static_cast<unsigned>(s.size()))];
        break;
    }
    case 3: case 4: case 5: {   // moderate magnitudes
        const int span = es == 16 ? 12 : 40;
        const std::int64_t ex = static_cast<std::int64_t>(bias) - span / 2 + rng.below(static_cast<unsigned>(span));
        v = (static_cast<std::uint64_t>(rng.coin()) << (es - 1)) |
            (static_cast<std::uint64_t>(ex) << f) | (rng.next() & ((1ULL << f) - 1));
        if (rng.below(3) == 0) v &= ~((1ULL << (f / 2)) - 1);   // short mantissas (ties)
        break;
    }
    case 6: {   // anywhere
        v = rng.next() & lane_mask(es);
        break;
    }
    case 7: {   // integers and half-integers
        const double base = static_cast<double>(static_cast<std::int64_t>(rng.next() % 2001) - 1000);
        const double val = rng.coin() ? base : base + 0.5;
        if (es == 64) v = d2b(val);
        else if (es == 32) v = f2b(static_cast<float>(val));
        else {
            // small half-integers in half precision: build from single
            const float fv = static_cast<float>(val / 8.0);
            const auto b = static_cast<std::uint32_t>(f2b(fv));
            const std::uint32_t sign = (b >> 16) & 0x8000;
            const int ex = static_cast<int>((b >> 23) & 0xff) - 127 + 15;
            if (fv == 0.0f || ex <= 0) v = sign;
            else v = sign | (static_cast<std::uint32_t>(ex) << 10) | ((b >> 13) & 0x3ff);
        }
        break;
    }
    case 8: {   // near powers of two (rounding boundaries)
        const std::int64_t ex = static_cast<std::int64_t>(bias) + static_cast<std::int64_t>(rng.below(es == 16 ? 30 : 140)) -
                                (es == 16 ? 15 : 70);
        const std::int64_t clamped = ex < 1 ? 1 : ex > static_cast<std::int64_t>(2 * bias) ? static_cast<std::int64_t>(2 * bias) : ex;
        v = (static_cast<std::uint64_t>(clamped) << f);
        const unsigned k = rng.below(4);
        if (k == 1) v += 1;
        else if (k == 2) v -= 1;
        else if (k == 3) v |= 1ULL << (f - 1);
        if (rng.coin()) v |= 1ULL << (es - 1);
        break;
    }
    default: {   // denormals
        v = (rng.next() & ((1ULL << f) - 1)) | (static_cast<std::uint64_t>(rng.coin()) << (es - 1));
        break;
    }
    }
    v &= lane_mask(es);
    if (!allow_snan && is_snan(v, es)) v |= 1ULL << (f - 1);
    return v;
}

void fill_vectors(GuestContext& ctx, Rng& rng, unsigned es, bool allow_snan) {
    for (auto& reg : ctx.v) {
        for (auto& half : reg) {
            std::uint64_t value = 0;
            for (unsigned lane = 0; lane < 64 / es; ++lane) value |= random_fp(rng, es, allow_snan) << (lane * es);
            half = value;
        }
    }
}

const std::vector<std::uint32_t>& fpcr_modes() {
    static const std::vector<std::uint32_t> modes = {
        0u, 1u << 22, 2u << 22, 3u << 22, 1u << 24, (1u << 24) | (1u << 22), (1u << 24) | (2u << 22),
        (1u << 24) | (3u << 22),
    };
    return modes;
}

// ---- comparison ------------------------------------------------------------------------

namespace {

bool lane_ok(std::uint64_t ev, std::uint64_t av, unsigned es, const CheckOptions& opt, bool fz) {
    if (ev == av) return true;
    if (opt.nan_es == 0) return false;
    if (is_nan(ev, es) && is_nan(av, es)) return true;
    if (fz && opt.fz_tininess && es != 16) {
        // A64 detects tininess before rounding, x64 after: with FZ a result
        // just below the minimum normal that rounds up to it is flushed by
        // A64 only.
        const std::uint64_t sign = 1ULL << (es - 1);
        const std::uint64_t min_normal = 1ULL << (es - exp_bits(es) - 1);
        if ((ev & ~sign) == 0 && (av & ~sign) == min_normal && (ev & sign) == (av & sign)) return true;
    }
    return false;
}

bool half_ok(std::uint64_t e, std::uint64_t a, const CheckOptions& opt, bool fz) {
    if (e == a) return true;
    if (opt.nan_es == 0) return false;
    const unsigned es = opt.nan_es;
    for (unsigned lane = 0; lane < 64 / es; ++lane) {
        const auto ev = (e >> (lane * es)) & lane_mask(es);
        const auto av = (a >> (lane * es)) & lane_mask(es);
        if (!lane_ok(ev, av, es, opt, fz)) return false;
    }
    return true;
}

} // namespace

bool compare_fp(const GuestContext& expected, const GuestContext& actual, const CheckOptions& opt,
                std::uint32_t fpcr, const std::string& label) {
    bool equal = true;
    const auto fail = [&](const std::string& what, std::uint64_t e, std::uint64_t a) {
        std::ostringstream message;
        message << label << ": " << what << " expected 0x" << std::hex << e << " got 0x" << a;
        testing::report_failure(__FILE__, __LINE__, message.str());
        equal = false;
    };
    for (unsigned i = 0; i < 32; ++i)
        if (expected.x[i] != actual.x[i]) fail(i == 31 ? "sp" : "x" + std::to_string(i), expected.x[i], actual.x[i]);
    if (expected.pc != actual.pc) fail("pc", expected.pc, actual.pc);
    if ((expected.nzcv & 0xf0000000u) != (actual.nzcv & 0xf0000000u))
        fail("nzcv", expected.nzcv & 0xf0000000u, actual.nzcv & 0xf0000000u);
    const bool fz = (fpcr >> 24) & 1;
    for (unsigned i = 0; i < 32; ++i) {
        for (unsigned h = 0; h < 2; ++h) {
            if (!half_ok(expected.v[i][h], actual.v[i][h], opt, fz))
                fail("v" + std::to_string(i) + (h ? ".hi" : ".lo"), expected.v[i][h], actual.v[i][h]);
        }
    }
    return equal;
}

bool check_expected(std::uint32_t word, const GuestContext& ctx, const GuestContext& expected,
                    const CheckOptions& opt, const std::string& label) {
    const std::uint32_t mxcsr = fp_mxcsr_for_fpcr(ctx.fpcr);
    GuestContext actual = ctx;
    if (!testing::run_family(actual, fp_emit, std::span(&word, 1), mxcsr)) return false;
    bool ok = compare_fp(expected, actual, opt, ctx.fpcr, label);
    if (opt.both_contexts) {
        GuestContext alias = ctx;
        if (!run_aliasing(alias, std::span(&word, 1), mxcsr)) return false;
        ok = compare_fp(expected, alias, opt, ctx.fpcr, label + " [alias]") && ok;
        // Memory sources only (the expected state needs the unsanitized input).
        GuestContext mem = ctx;
        if (!run_aliasing(mem, std::span(&word, 1), mxcsr, alias_variants()[0])) return false;
        ok = compare_fp(expected, mem, opt, ctx.fpcr, label + variant_name(alias_variants()[0])) && ok;
    }
    return ok;
}

bool check_word(std::uint32_t word, const GuestContext& ctx0, const CheckOptions& opt, const std::string& label,
                bool* oracle_missing) {
    GuestContext ctx = ctx0;
    ctx.fpsr = 0;
    std::vector<std::uint32_t> words{word};
    unsigned rt = 32;
    if (opt.compare_fpsr) {
        InstructionUse use;
        fp_describe(word, use);
        for (unsigned r : {28u, 27u, 26u, 25u, 24u, 23u}) {
            if (!((use.gpr_read | use.gpr_write) & (1u << r))) { rt = r; break; }
        }
        words.push_back(0xd53b4420u | rt);
    }
    GuestContext expected = ctx;
    for (const auto w : words) {
        if (opt.reference && w == word) {
            if (!opt.reference(w, expected)) {
                if (oracle_missing) *oracle_missing = true;
                return false;
            }
            expected.pc += 4;
            continue;
        }
        if (testing::oracle_step(expected, w)) continue;
        if (opt.fallback && w == word && opt.fallback(w, expected)) {
            expected.pc += 4;
            continue;
        }
        if (oracle_missing) *oracle_missing = true;
        return false;
    }
    const std::uint32_t mxcsr = fp_mxcsr_for_fpcr(ctx.fpcr);
    const auto fix_fpsr = [&](GuestContext& c) {
        if (rt < 32) c.x[rt] &= opt.fpsr_mask;
    };
    fix_fpsr(expected);
    if (opt.fix_expected) opt.fix_expected(word, expected);
    GuestContext actual = ctx;
    if (!testing::run_family(actual, fp_emit, words, mxcsr)) return false;
    fix_fpsr(actual);
    bool ok = compare_fp(expected, actual, opt, ctx.fpcr, label);
    if (opt.both_contexts) {
        GuestContext alias = ctx;
        std::vector<ZeroNote> notes;
        if (!run_aliasing(alias, words, mxcsr, {}, &notes)) return false;
        fix_fpsr(alias);
        ok = compare_fp(expected, alias, opt, ctx.fpcr, label + " [alias]") && ok;
        ok = check_notes(notes, expected, label) && ok;
        for (const auto& v : alias_variants()) {
            GuestContext vin = ctx;
            apply_zero_claims(vin, v);
            GuestContext vexp = expected;
            if (v.zero32 || v.zero64) {
                vexp = vin;
                bool have = true;
                for (const auto w : words) {
                    if (opt.reference && w == word) {
                        if (!opt.reference(w, vexp)) { have = false; break; }
                        vexp.pc += 4;
                    } else if (testing::oracle_step(vexp, w)) {
                    } else if (opt.fallback && w == word && opt.fallback(w, vexp)) {
                        vexp.pc += 4;
                    } else {
                        have = false;
                        break;
                    }
                }
                if (!have) continue;
                fix_fpsr(vexp);
                if (opt.fix_expected) opt.fix_expected(word, vexp);
            }
            GuestContext run = vin;
            if (!run_aliasing(run, words, mxcsr, v)) return false;
            fix_fpsr(run);
            ok = compare_fp(vexp, run, opt, ctx.fpcr, label + variant_name(v)) && ok;
        }
    }
    return ok;
}

// ---- aliasing context --------------------------------------------------------------------

namespace {

constexpr Gp kGprPool[] = {Gp::rax, Gp::rcx, Gp::rdx, Gp::rbx, Gp::rbp, Gp::rsi, Gp::rdi,
                           Gp::r8, Gp::r9, Gp::r10, Gp::r11, Gp::r12, Gp::r13, Gp::r14};
constexpr Gp kCtx = Gp::r15;

constexpr x64::Cond kSubConditions[14] = {
    x64::Cond::e, x64::Cond::ne, x64::Cond::ae, x64::Cond::b, x64::Cond::s, x64::Cond::ns,
    x64::Cond::o, x64::Cond::no, x64::Cond::a, x64::Cond::be, x64::Cond::ge, x64::Cond::l,
    x64::Cond::g, x64::Cond::le,
};

x64::Mem nzcv_field() { return ptr(kCtx, ctx_disp(offsetof(GuestContext, nzcv))); }

class AliasEmitContext final : public EmitContext {
public:
    AliasEmitContext(x64::Assembler& a, std::uint64_t pc, const AliasOptions& options = {})
        : as_(a), pc_(pc), opt_(options), zero32_(options.zero32), zero64_(options.zero64 | options.zero32) {}
    x64::Assembler& as() override { return as_; }
    [[nodiscard]] std::uint64_t pc() const override { return pc_; }

    // Memory sources: registers not yet in a host register this instruction
    // are handed out as their GuestContext slot (the slot keeps the value
    // from before the instruction until end()).
    VecSrc vsrc(unsigned index) override {
        if (opt_.mem_sources && vmap_.find(index) == vmap_.end())
            return {Xmm{}, ptr(kCtx, v_disp(index)), true};
        return {read_vec(index), {}, false};
    }
    GprSrc gsrc(unsigned index, bool sp) override {
        if (opt_.mem_sources && (index != 31 || sp) && gmap_.find(index) == gmap_.end())
            return {Gp::none, ptr(kCtx, x_disp(index)), true};
        return {read_gpr(index, sp), {}, false};
    }
    // Memory-source mode: every slot holds the value from before the
    // instruction (EmitContext::vec_slot).
    bool vec_slot(unsigned index, x64::Mem& slot) override {
        if (!opt_.mem_sources || index >= 32) return false;
        slot = ptr(kCtx, v_disp(index));
        return true;
    }
    // Upper-zero tracking across the words of one run, like the region
    // emitter within a block.
    [[nodiscard]] std::uint32_t vec_zero_above(unsigned width) const override {
        return width == 32 ? zero32_ : width == 64 ? zero64_ : width >= 128 ? ~0u : 0u;
    }
    void note_vec_zero_above(unsigned index, unsigned width) override {
        if (index >= 32 || vdirty_.find(index) == vdirty_.end())
            throw std::logic_error("note_vec_zero_above for a register the instruction did not write");
        notes.push_back({index, width});
        if (!opt_.track_zero) return;
        if (width <= 32) zero32_ |= 1u << index;
        if (width <= 64) zero64_ |= 1u << index;
    }
    std::vector<ZeroNote> notes;   // every note made (checked against the expected state)

    Gp read_gpr(unsigned index, bool sp) override {
        if (index == 31 && !sp) {
            const Gp r = alloc_gpr();
            as_.mov_imm(r, 0);
            return r;
        }
        if (auto it = gmap_.find(index); it != gmap_.end()) return it->second;
        const Gp r = alloc_gpr();
        as_.mov(64, r, ptr(kCtx, x_disp(index)));
        gmap_[index] = r;
        return r;
    }
    Gp write_gpr(unsigned index, bool sp) override {
        if (index == 31 && !sp) return alloc_gpr();
        Gp r;
        if (auto it = gmap_.find(index); it != gmap_.end()) r = it->second;
        else { r = alloc_gpr(); gmap_[index] = r; }
        gdirty_[index] = r;
        return r;
    }
    Gp copy_gpr(unsigned index, bool sp) override {
        const Gp src = read_gpr(index, sp);
        const Gp r = alloc_gpr();
        as_.mov(64, r, src);
        return r;
    }
    Gp temp_gpr() override { return alloc_gpr(); }
    Xmm read_vec(unsigned index) override {
        if (auto it = vmap_.find(index); it != vmap_.end()) return it->second;
        const Xmm r = alloc_vec();
        as_.movdqu(r, ptr(kCtx, v_disp(index)));
        vmap_[index] = r;
        return r;
    }
    Xmm write_vec(unsigned index) override {
        zero32_ &= ~(1u << index);
        zero64_ &= ~(1u << index);
        Xmm r;
        if (auto it = vmap_.find(index); it != vmap_.end()) r = it->second;
        else { r = alloc_vec(); vmap_[index] = r; }
        vdirty_[index] = r;
        return r;
    }
    Xmm temp_vec() override { return alloc_vec(); }
    void clobber_host_flags() override {}
    void set_flags(FlagsProducer producer) override {
        const Gp flags = alloc_gpr(), result = alloc_gpr(), bit = alloc_gpr();
        as_.pushfq();
        as_.pop(flags);
        as_.mov(32, result, flags);
        as_.and_(32, result, 0x80);
        as_.shl(32, result, 24);
        as_.mov(32, bit, flags);
        as_.and_(32, bit, 0x40);
        as_.shl(32, bit, 24);
        as_.or_(32, result, bit);
        if (producer != FlagsProducer::logic) {
            as_.mov(32, bit, flags);
            as_.and_(32, bit, 1);
            if (producer == FlagsProducer::sub) as_.xor_(32, bit, 1);
            as_.shl(32, bit, 29);
            as_.or_(32, result, bit);
            as_.mov(32, bit, flags);
            as_.and_(32, bit, 0x800);
            as_.shl(32, bit, 17);
            as_.or_(32, result, bit);
        }
        as_.mov(64, nzcv_field(), result);
    }
    void set_nzcv(Gp value) override { as_.mov(64, nzcv_field(), value); }
    Gp get_nzcv() override {
        const Gp r = alloc_gpr();
        as_.mov(32, r, nzcv_field());
        return r;
    }
    x64::Cond condition(unsigned arm_condition) override {
        if (arm_condition >= 14) throw std::logic_error("condition(): AL/NV");
        const Gp nzcv = alloc_gpr(), image = alloc_gpr(), bit = alloc_gpr();
        as_.mov(32, nzcv, nzcv_field());
        as_.mov_imm(image, 2);
        as_.mov(32, bit, nzcv);
        as_.shr(32, bit, 24);
        as_.and_(32, bit, 0xc0);
        as_.or_(32, image, bit);
        as_.mov(32, bit, nzcv);
        as_.shr(32, bit, 29);
        as_.and_(32, bit, 1);
        as_.xor_(32, bit, 1);
        as_.or_(32, image, bit);
        as_.mov(32, bit, nzcv);
        as_.shr(32, bit, 17);
        as_.and_(32, bit, 0x800);
        as_.or_(32, image, bit);
        as_.push(image);
        as_.popfq();
        return kSubConditions[arm_condition];
    }
    void mark_may_fault() override {}
    Gp call_helper(unsigned, std::initializer_list<HelperArg>, HelperEffects) override {
        throw std::logic_error("AliasEmitContext: helpers not supported");
    }

    void begin(std::uint64_t pc) {
        pc_ = pc;
        ngpr_ = nvec_ = 0;
        gmap_.clear(); vmap_.clear(); gdirty_.clear(); vdirty_.clear();
    }
    // Every register obtained is stored back, read-only ones included: an
    // emitter that writes into a source register (which may be a pinned or
    // cached guest register in translated regions) then fails the tests.
    void end() {
        for (const auto& [g, r] : gmap_) as_.mov(64, ptr(kCtx, x_disp(g)), r);
        for (const auto& [v, r] : vmap_) as_.movdqu(ptr(kCtx, v_disp(v)), r);
    }

private:
    Gp alloc_gpr() {
        if (ngpr_ >= std::size(kGprPool)) throw std::logic_error("AliasEmitContext: out of GPRs");
        return kGprPool[ngpr_++];
    }
    Xmm alloc_vec() {
        if (nvec_ >= 16) throw std::logic_error("AliasEmitContext: out of XMMs");
        // Hand out registers from the top so they differ from MemoryEmitContext's.
        return x64::xmm(15 - nvec_++);
    }
    x64::Assembler& as_;
    std::uint64_t pc_;
    AliasOptions opt_;
    std::uint32_t zero32_ = 0, zero64_ = 0;
    unsigned ngpr_ = 0, nvec_ = 0;
    std::map<unsigned, Gp> gmap_, gdirty_;
    std::map<unsigned, Xmm> vmap_, vdirty_;
};

// Emits nothing for operands: models fully pinned guest registers. Records
// the guest registers and flags the emitter touches.
class CountingEmitContext final : public EmitContext {
public:
    explicit CountingEmitContext(x64::Assembler& a, bool zero_upper = false) : as_(a), zero_upper_(zero_upper) {}
    // Optionally every register is known zero above its scalar (the steady
    // state of scalar code in a region).
    [[nodiscard]] std::uint32_t vec_zero_above(unsigned) const override { return zero_upper_ ? ~0u : 0u; }
    x64::Assembler& as() override { return as_; }
    [[nodiscard]] std::uint64_t pc() const override { return 0x140001000ULL; }
    Gp read_gpr(unsigned index, bool sp) override {
        if (index != 31 || sp) used.gpr_read |= 1u << index;
        return pin_gpr(index);
    }
    Gp write_gpr(unsigned index, bool sp) override {
        if (index != 31 || sp) used.gpr_write |= 1u << index;
        return pin_gpr(index);
    }
    Gp copy_gpr(unsigned index, bool sp) override {
        if (index != 31 || sp) used.gpr_read |= 1u << index;
        return next_gpr();
    }
    Gp temp_gpr() override { return next_gpr(); }
    Xmm read_vec(unsigned index) override { used.vec_read |= 1u << index; return pin_vec(index); }
    Xmm write_vec(unsigned index) override { used.vec_write |= 1u << index; return pin_vec(index); }
    Xmm temp_vec() override { return x64::xmm(ntemp_++); }
    void clobber_host_flags() override {}
    void set_flags(FlagsProducer) override { used.writes_flags = true; }
    void set_nzcv(Gp) override { used.writes_flags = true; }
    Gp get_nzcv() override { used.reads_flags = true; return next_gpr(); }
    x64::Cond condition(unsigned c) override { used.reads_flags = true; return kSubConditions[c % 14]; }
    InstructionUse used{};
    void mark_may_fault() override {}
    Gp call_helper(unsigned, std::initializer_list<HelperArg>, HelperEffects) override {
        throw std::logic_error("CountingEmitContext: helper");
    }

private:
    Gp pin_gpr(unsigned index) {
        if (auto it = gmap_.find(index); it != gmap_.end()) return it->second;
        const Gp r = next_gpr();
        gmap_[index] = r;
        return r;
    }
    Gp next_gpr() { return kGprPool[std::size(kGprPool) - 1 - (ngpr_++ % std::size(kGprPool))]; }
    Xmm pin_vec(unsigned index) {
        if (auto it = vmap_.find(index); it != vmap_.end()) return it->second;
        const Xmm r = x64::xmm(15 - nvec_++);
        vmap_[index] = r;
        return r;
    }
    x64::Assembler& as_;
    bool zero_upper_ = false;
    unsigned ngpr_ = 0, nvec_ = 0, ntemp_ = 0;
    std::map<unsigned, Gp> gmap_;
    std::map<unsigned, Xmm> vmap_;
};

} // namespace

namespace {

std::vector<std::uint8_t> assemble(std::uint32_t word, bool aliasing, const AliasOptions& options = {},
                                   std::vector<ZeroNote>* notes = nullptr) {
    x64::Assembler a;
    a.sub(64, Gp::rsp, 40);
    unsigned consumed;
    if (aliasing) {
        AliasEmitContext emit(a, 0, options);
        emit.begin(0);
        consumed = fp_emit(emit, std::span(&word, 1));
        emit.end();
        if (notes) *notes = emit.notes;
    } else {
        testing::MemoryEmitContext emit(a, 0);
        emit.begin_instruction(0);
        consumed = fp_emit(emit, std::span(&word, 1));
        emit.end_instruction();
    }
    if (consumed == 0) return {};
    a.add(64, Gp::rsp, 40);
    a.ret();
    a.finalize();
    if (const char* dump = std::getenv("FP_DUMP"); dump && std::strtoul(dump, nullptr, 16) == word) {
        std::printf("FP_DUMP_%s %08x ", aliasing ? "ALIAS" : "MEMORY", word);
        for (const auto b : a.bytes()) std::printf("%02x", b);
        std::printf("\n");
    }
    return a.bytes();
}

} // namespace

void apply_zero_claims(GuestContext& ctx, const AliasOptions& opt) {
    for (unsigned i = 0; i < 32; ++i) {
        if ((opt.zero32 >> i) & 1u) {
            ctx.v[i][0] &= 0xffffffffULL;
            ctx.v[i][1] = 0;
        } else if ((opt.zero64 >> i) & 1u) {
            ctx.v[i][1] = 0;
        }
    }
}

const std::vector<AliasOptions>& alias_variants() {
    static const std::vector<AliasOptions> variants = {
        AliasOptions{true, false, 0, 0},
        AliasOptions{false, true, 0x55555555u, 0xffffffffu},
        AliasOptions{true, true, 0xaaaaaaaau, 0xffffffffu},
    };
    return variants;
}

unsigned check_batch(std::uint32_t word, const std::vector<GuestContext>& contexts, const CheckOptions& opt,
                     const std::string& name, unsigned max_failures) {
    std::vector<ZeroNote> notes;
    const auto mem_code = assemble(word, false), alias_code = assemble(word, true, {}, &notes);
    if (mem_code.empty() || alias_code.empty()) {
        char text[96];
        std::snprintf(text, sizeof(text), "%s: fp declined 0x%08x", name.c_str(), word);
        testing::report_failure(__FILE__, __LINE__, text);
        return 0;
    }
    testing::Snippet mem(mem_code), alias(alias_code);
    std::vector<std::unique_ptr<testing::Snippet>> variants;
    for (const auto& v : alias_variants()) variants.push_back(std::make_unique<testing::Snippet>(assemble(word, true, v)));
    const auto expect = [&](GuestContext& expected) {
        if (opt.reference) {
            if (!opt.reference(word, expected)) return false;
            expected.pc += 4;
        } else if (testing::oracle_step(expected, word)) {
        } else if (opt.fallback && opt.fallback(word, expected)) {
            expected.pc += 4;
        } else {
            return false;
        }
        if (opt.fix_expected) opt.fix_expected(word, expected);
        return true;
    };
    unsigned checked = 0, failures = 0;
    for (const auto& c0 : contexts) {
        GuestContext ctx = c0;
        ctx.fpsr = 0;
        GuestContext expected = ctx;
        if (!expect(expected)) continue;
        ++checked;
        const std::uint32_t mxcsr = fp_mxcsr_for_fpcr(ctx.fpcr);
        char label[160];
        std::snprintf(label, sizeof(label), "%s 0x%08x fpcr=%08x n.lo=%016llx m.lo=%016llx", name.c_str(), word,
                      ctx.fpcr, static_cast<unsigned long long>(ctx.v[(word >> 5) & 31][0]),
                      static_cast<unsigned long long>(ctx.v[(word >> 16) & 31][0]));
        GuestContext a1 = ctx, a2 = ctx;
        mem.run(a1, mxcsr);
        a1.pc += 4;
        alias.run(a2, mxcsr);
        a2.pc += 4;
        bool ok = compare_fp(expected, a1, opt, ctx.fpcr, label);
        ok = compare_fp(expected, a2, opt, ctx.fpcr, std::string(label) + " [alias]") && ok;
        ok = check_notes(notes, expected, label) && ok;
        for (std::size_t k = 0; k < variants.size(); ++k) {
            const auto& v = alias_variants()[k];
            GuestContext vin = ctx;
            apply_zero_claims(vin, v);
            GuestContext vexp = vin;
            if (v.zero32 || v.zero64) {
                if (!expect(vexp)) continue;
            } else {
                vexp = expected;
            }
            GuestContext a3 = vin;
            variants[k]->run(a3, mxcsr);
            a3.pc += 4;
            ok = compare_fp(vexp, a3, opt, ctx.fpcr, std::string(label) + variant_name(v)) && ok;
        }
        if (!ok && ++failures >= max_failures) break;
    }
    return checked;
}

bool run_aliasing(GuestContext& ctx, std::span<const std::uint32_t> words, std::uint32_t mxcsr,
                  const AliasOptions& options, std::vector<ZeroNote>* notes) {
    x64::Assembler a;
    a.sub(64, Gp::rsp, 40);
    AliasEmitContext emit(a, ctx.pc, options);
    std::size_t index = 0;
    while (index < words.size()) {
        emit.begin(ctx.pc + index * 4);
        const auto consumed = fp_emit(emit, words.subspan(index));
        if (consumed == 0) {
            std::ostringstream message;
            message << "fp declined word 0x" << std::hex << words[index] << " (aliasing context)";
            testing::report_failure(__FILE__, __LINE__, message.str());
            return false;
        }
        emit.end();
        index += consumed;
    }
    a.add(64, Gp::rsp, 40);
    a.ret();
    a.finalize();
    if (const char* dump = std::getenv("FP_DUMP"); dump && std::strtoul(dump, nullptr, 16) == words[0]) {
        std::printf("FP_DUMP_RUN %08x ", words[0]);
        for (const auto b : a.bytes()) std::printf("%02x", b);
        std::printf("\n");
    }
    testing::Snippet snippet(a.bytes());
    snippet.run(ctx, mxcsr);
    ctx.pc += words.size() * 4;
    if (notes) *notes = emit.notes;
    return true;
}

bool check_notes(const std::vector<ZeroNote>& notes, const GuestContext& expected, const std::string& label) {
    bool ok = true;
    for (const auto& n : notes) {
        const std::uint64_t lo = n.width <= 32 ? expected.v[n.index][0] >> 32 : 0;
        const std::uint64_t hi = n.width < 128 ? expected.v[n.index][1] : 0;
        if (hi != 0 || lo != 0) {
            std::ostringstream msg;
            msg << label << ": noted v" << n.index << " zero above bit " << n.width << " but expected v"
                << n.index << " = 0x" << std::hex << expected.v[n.index][1] << ":" << expected.v[n.index][0];
            testing::report_failure(__FILE__, __LINE__, msg.str());
            ok = false;
        }
    }
    return ok;
}

// ---- instruction counting ------------------------------------------------------------------

namespace {

unsigned modrm_length(const std::uint8_t* p) {
    const unsigned mod = p[0] >> 6, rm = p[0] & 7;
    unsigned len = 1;
    if (mod == 3) return len;
    if (rm == 4) {
        const unsigned base = p[1] & 7;
        ++len;
        if (mod == 0 && base == 5) len += 4;
    } else if (mod == 0 && rm == 5) {
        len += 4;
    }
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

unsigned instruction_length(const std::uint8_t* p) {
    unsigned i = 0;
    bool opsize = false;
    while (p[i] == 0x66 || p[i] == 0xf2 || p[i] == 0xf3 || p[i] == 0xf0) {
        if (p[i] == 0x66) opsize = true;
        ++i;
    }
    if (p[i] == 0xc4 || p[i] == 0xc5) {
        unsigned map;
        if (p[i] == 0xc5) { map = 1; i += 2; }
        else { map = p[i + 1] & 0x1f; i += 3; }
        const std::uint8_t opc = p[i++];
        i += modrm_length(p + i);
        const bool imm8 = map == 3 || (map == 1 && (opc == 0xc2 || opc == 0xc4 || opc == 0xc5 ||
                                                    opc == 0xc6 || (opc >= 0x70 && opc <= 0x73)));
        if (imm8) ++i;
        return i;
    }
    bool rexw = false;
    if ((p[i] & 0xf0) == 0x40) { rexw = (p[i] & 8) != 0; ++i; }
    const std::uint8_t opc = p[i++];
    if (opc == 0x0f) {
        const std::uint8_t o2 = p[i++];
        if (o2 == 0x38) { ++i; return i + modrm_length(p + i); }
        if (o2 == 0x3a) { ++i; return i + modrm_length(p + i) + 1; }
        if (o2 >= 0x80 && o2 <= 0x8f) return i + 4;
        if (o2 >= 0xc8 && o2 <= 0xcf) return i;
        if (o2 == 0x0b || o2 == 0x31 || o2 == 0xa2) return i;
        i += modrm_length(p + i);
        if ((o2 >= 0x70 && o2 <= 0x73) || o2 == 0xc2 || o2 == 0xc4 || o2 == 0xc5 || o2 == 0xc6 ||
            o2 == 0xba || o2 == 0xa4 || o2 == 0xac)
            ++i;
        return i;
    }
    if ((opc >= 0x50 && opc <= 0x5f) || opc == 0x90 || opc == 0x99 || opc == 0x9c || opc == 0x9d ||
        opc == 0x9e || opc == 0x9f || opc == 0xc3 || opc == 0xcc || opc == 0xf5 || opc == 0xf8 || opc == 0xf9)
        return i;
    if (opc >= 0xb8 && opc <= 0xbf) return i + (rexw ? 8 : 4);
    if (opc == 0xeb || (opc >= 0x70 && opc <= 0x7f)) return i + 1;
    if (opc == 0xe8 || opc == 0xe9) return i + 4;
    const unsigned m = modrm_length(p + i);
    const unsigned reg = (p[i] >> 3) & 7;
    switch (opc) {
    case 0x80: case 0x83: case 0xc0: case 0xc1: case 0x6b: case 0xc6: return i + m + 1;
    case 0x81: case 0x69: case 0xc7: return i + m + (opsize ? 2 : 4);
    case 0xf6: return i + m + (reg == 0 ? 1 : 0);
    case 0xf7: return i + m + (reg == 0 ? (opsize ? 2 : 4) : 0);
    default: return i + m;
    }
}

} // namespace

unsigned count_x64_instructions(const std::vector<std::uint8_t>& code) {
    unsigned count = 0;
    std::size_t at = 0;
    while (at < code.size()) {
        at += instruction_length(code.data() + at);
        ++count;
    }
    if (at != code.size()) throw std::logic_error("count_x64_instructions: decode overrun");
    return count;
}

unsigned common_path_instructions(const std::vector<std::uint8_t>& code) {
    unsigned count = 0;
    std::size_t at = 0;
    while (at < code.size() && count < 1000) {
        const std::uint8_t* p = code.data() + at;
        const unsigned len = instruction_length(p);
        ++count;
        std::int64_t target = -1;
        if ((p[0] >= 0x70 && p[0] <= 0x7f) || p[0] == 0xeb) target = static_cast<std::int64_t>(at + 2) + static_cast<std::int8_t>(p[1]);
        else if (p[0] == 0xe9) { std::int32_t d; std::memcpy(&d, p + 1, 4); target = static_cast<std::int64_t>(at + 5) + d; }
        else if (p[0] == 0x0f && p[1] >= 0x80 && p[1] <= 0x8f) { std::int32_t d; std::memcpy(&d, p + 2, 4); target = static_cast<std::int64_t>(at + 6) + d; }
        if (target > static_cast<std::int64_t>(at)) at = static_cast<std::size_t>(target);   // fix-up code is skipped
        else at += len;
    }
    return count;
}

bool emitted_uses(std::uint32_t word, InstructionUse& use) {
    x64::Assembler a;
    CountingEmitContext emit(a);
    if (fp_emit(emit, std::span(&word, 1)) == 0) return false;
    use = emit.used;
    return true;
}

unsigned emitted_instructions(std::uint32_t word, bool common_path, bool zero_upper) {
    // Region-style constant pool (as in translated regions): constants after
    // the code, not counted.
    x64::Assembler a;
    CountingEmitContext emit(a, zero_upper);
    neon_begin_region(a);
    if (fp_emit(emit, std::span(&word, 1)) == 0) return 0;
    const auto code_end = a.size();
    neon_end_region(a);
    a.finalize();
    std::vector<std::uint8_t> code(a.bytes().begin(), a.bytes().begin() + static_cast<std::ptrdiff_t>(code_end));
    // Diagnostic: FP_DUMP=<hex word> prints the code bytes (pinned-style
    // operands) for disassembly.
    if (const char* dump = std::getenv("FP_DUMP"); dump && std::strtoul(dump, nullptr, 16) == word && !zero_upper &&
                                                    common_path) {
        std::printf("FP_DUMP %08x ", word);
        for (const auto b : code) std::printf("%02x", b);
        std::printf("\n");
    }
    return common_path ? common_path_instructions(code) : count_x64_instructions(code);
}

} // namespace transmute::fx::fptest
