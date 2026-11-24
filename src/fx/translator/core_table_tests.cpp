// Switch dispatch tests (owner: translator core). A BR through a recognised
// jump table (MSVC ARM64 lowering: adr; ldr{sw,sb,b,h} [xT, wI, uxtw]; [adr];
// add; br) dispatches inside the region (region_plan.cpp match_table,
// emit_branch.cpp e_table_branch). Checked here: differential execution of
// interpreter-like dispatch loops against the reference interpreter (every
// table form and bound form, out-of-range indices, cases outside the region),
// that the dispatch leaves no lookup exits, and that a table changed after
// translation still branches where the guest computes (the run-time target
// check). LLVM's lowering (ldr{b,h,sw} [xT, xI, lsl #k] with the table
// address set up before the loop) dispatches on the target: the same checks.
#include "fx/runtime/runtime.hpp"   // kTableShadowSlots
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/region_plan.hpp"
#include "fx/translator/translator.hpp"

#include <bit>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::coretest;

namespace {

DiffRunner& runner() {
    static DiffRunner r;
    return r;
}

bool tables_enabled() { return (core::codegen_knob_bits() & (1u << 20)) == 0; }
// Cases inside the dispatching region (not TRANSMUTE_FX_CORE=tableexits).
bool cases_in_region() { return tables_enabled() && (core::codegen_knob_bits() & (1u << 21)) == 0; }

enum class Form { ldrsw_base, ldrsb_self, ldrb_self, ldrh_self };
enum class Bound { cmp_hi, cmp_hs, and_mask, uxtb };

struct Switch {
    std::vector<std::uint32_t> words;
    std::size_t br{};          // word index of the BR
    std::size_t table{};       // word index of the table
    std::vector<std::size_t> case_at;   // word index of each case
    std::size_t base{};        // word index the entries are relative to
    unsigned entries{};
};

// mov w12, #iterations
// dispatch: add x14, x14, #step; and w14, w14, #mask (or uxtb) [cmp w14, #n; b.hi/b.hs default]
//           adr x9, table; ldr.. x8, [x9, w14, uxtw #size]; [adr x10, base]; add x8, x10|x9, x8, lsl #2; br x8
// case k:   1..4 random instructions; b tail (or fall into the next case)
// default:  1..2 random instructions
// tail:     subs w12, w12, #1; b.ne dispatch; b end
// table:    entries
Switch make_switch(std::uint64_t seed, Form form, Bound bound, unsigned cases, unsigned iterations) {
    using namespace a64;
    Rng rng(seed);
    InstGen g(rng);
    Switch s;
    auto& w = s.words;
    const unsigned mask = bound == Bound::uxtb ? 255u : cases <= 8 ? 7u : 15u;
    w.push_back(movw(false, 2, 0, iterations, 12));
    const auto dispatch = w.size();
    w.push_back(add_imm(true, false, false, 14, 14, 1 + rng.below(5)));
    if (bound == Bound::uxtb) w.push_back(bitfield(false, 2, 0, 7, 14, 14));
    else w.push_back(logic_imm(false, 0, 0, 0, mask == 7 ? 2 : 3, 14, 14));   // and w14, w14, #mask
    std::size_t to_default = SIZE_MAX;
    const unsigned entries = bound == Bound::and_mask ? mask + 1 : bound == Bound::uxtb ? 256 : cases;
    if (bound == Bound::cmp_hi) {
        w.push_back(add_imm(false, true, true, 31, 14, cases - 1));   // cmp w14, #cases-1
        to_default = w.size();
        w.push_back(bcond(8, 0));                                      // b.hi default
    } else if (bound == Bound::cmp_hs) {
        w.push_back(add_imm(false, true, true, 31, 14, cases));       // cmp w14, #cases
        to_default = w.size();
        w.push_back(bcond(2, 0));                                      // b.hs default
    }
    const auto adr_table = w.size();
    w.push_back(0);   // adr x9, table (patched)
    switch (form) {
    case Form::ldrsw_base: w.push_back(ldst_reg(2, 2, 14, 2, true, 9, 8)); break;    // ldrsw x8, [x9, w14, uxtw #2]
    case Form::ldrsb_self: w.push_back(ldst_reg(0, 2, 14, 2, false, 9, 8)); break;   // ldrsb x8, [x9, w14, uxtw]
    case Form::ldrb_self: w.push_back(ldst_reg(0, 1, 14, 2, false, 9, 8)); break;    // ldrb w8, [x9, w14, uxtw]
    case Form::ldrh_self: w.push_back(ldst_reg(1, 1, 14, 2, true, 9, 8)); break;     // ldrh w8, [x9, w14, uxtw #1]
    }
    std::size_t adr_base = SIZE_MAX;
    if (form == Form::ldrsw_base) {
        adr_base = w.size();
        w.push_back(0);   // adr x10, base (patched)
        w.push_back(add_shift(true, false, false, 0, 8, 2, 10, 8));   // add x8, x10, x8, lsl #2
    } else {
        w.push_back(add_shift(true, false, false, 0, 8, 2, 9, 8));    // add x8, x9, x8, lsl #2
    }
    s.br = w.size();
    w.push_back(br(8));
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < cases; ++k) {
        s.case_at.push_back(w.size());
        const unsigned n = 1 + rng.below(4);
        for (unsigned j = 0; j < n; ++j) w.push_back(g.data_or_memory());
        if (k + 1 == cases || rng.chance(75)) {
            to_tail.push_back(w.size());
            w.push_back(b(0));
        }
    }
    const auto deflt = w.size();
    for (unsigned j = 0, n = 1 + rng.below(2); j < n; ++j) w.push_back(g.data_or_memory());
    const auto tail = w.size();
    w.push_back(add_imm(false, true, true, 12, 12, 1));   // subs w12, w12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    // Table: ldrsw entries are words relative to `base` (the first case);
    // byte/halfword entries are relative to the table itself, so the cases
    // sit before it (negative ldrsb offsets) or anywhere in reach.
    s.table = w.size();
    s.entries = entries;
    s.base = form == Form::ldrsw_base ? s.case_at[0] : s.table;
    std::vector<std::int32_t> rel(entries);
    for (unsigned k = 0; k < entries; ++k) {
        const auto target = k < cases ? s.case_at[k % cases] : (bound == Bound::uxtb ? s.case_at[k % cases] : deflt);
        rel[k] = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(s.base);
    }
    const unsigned bytes = form == Form::ldrsw_base ? 4 : form == Form::ldrh_self ? 2 : 1;
    std::vector<std::uint8_t> data(entries * bytes);
    for (unsigned k = 0; k < entries; ++k) {
        const auto v = static_cast<std::uint32_t>(rel[k]);
        std::memcpy(data.data() + k * bytes, &v, bytes);
    }
    data.resize((data.size() + 3) & ~std::size_t{3});
    for (std::size_t at = 0; at < data.size(); at += 4) {
        std::uint32_t word;
        std::memcpy(&word, data.data() + at, 4);
        w.push_back(word);
    }
    const auto end = w.size();
    // Patch branches and ADRs.
    const auto rel_words = [](std::size_t from, std::size_t to) {
        return static_cast<std::int32_t>(to) - static_cast<std::int32_t>(from);
    };
    if (to_default != SIZE_MAX) w[to_default] |= (static_cast<std::uint32_t>(rel_words(to_default, deflt)) & 0x7ffff) << 5;
    for (const auto at : to_tail) w[at] = b(rel_words(at, tail));
    w[to_end] = b(rel_words(to_end, end));
    w[adr_table] = adr(false, 4 * rel_words(adr_table, s.table), 9);
    if (adr_base != SIZE_MAX) w[adr_base] = adr(false, 4 * rel_words(adr_base, s.base), 10);
    return s;
}

// LLVM's lowering (JumpTable::by_target): the table address in x18 is set up
// once before the loop (ADR, or ADRP+ADD), entries are relative to an ADR
// base before the first case, the index x14 is a 64-bit (LSL) operand and,
// with `clobber`, is rewritten between the load and the BR.
// mov w12, #iterations; adr x18, table | adrp x18, table; add x18, x18, #lo12
// dispatch: add x14, x14, #step; and w14, w14, #15; cmp w14, #n-1; b.hi default
//           adr x10, base; ldrb w8, [x18, x14] | ldrh w8, [x18, x14, lsl #1] | ldrsw x8, [x18, x14, lsl #2]
//           [and w14, w14, #15]; add x9, x10, x8, lsl #2; br x9
// base: case k: 1..4 random instructions; b tail (or fall into the next case)
// default, tail and table as in make_switch.
enum class LlvmForm { ldrb, ldrh, ldrsw };
// The index's bound: cmp + b.hi (as above), a byte load (ldrb w14, [x16, #5]: 256 entries, no
// compare), or a bit field (ubfx w14, w14, #2, #4: 16 entries).
enum class LlvmBound { cmp, byte_load, ubfx };
Switch make_llvm_switch(std::uint64_t seed, std::uint64_t program_base, LlvmForm form, bool adrp, bool clobber,
                        unsigned cases, unsigned iterations, LlvmBound bound = LlvmBound::cmp) {
    using namespace a64;
    Rng rng(seed);
    InstGen g(rng);
    Switch s;
    auto& w = s.words;
    w.push_back(movw(false, 2, 0, iterations, 12));
    const auto table_def = w.size();
    w.push_back(0);                                                    // adr/adrp x18 (patched)
    w.push_back(adrp ? 0u : nop());                                    // add x18, x18, #lo12 (patched)
    const auto dispatch = w.size();
    w.push_back(add_imm(true, false, false, 14, 14, 1 + rng.below(5)));
    std::size_t to_default = SIZE_MAX;
    unsigned entries = cases;
    if (bound == LlvmBound::cmp) {
        w.push_back(logic_imm(false, 0, 0, 0, 3, 14, 14));            // and w14, w14, #15
        w.push_back(add_imm(false, true, true, 31, 14, cases - 1));   // cmp w14, #cases-1
        to_default = w.size();
        w.push_back(bcond(8, 0));                                      // b.hi default
    } else if (bound == LlvmBound::byte_load) {
        w.push_back(ldst_uimm(0, 1, 5, 16, 14));                       // ldrb w14, [x16, #5]
        entries = 256;
    } else {
        w.push_back(bitfield(false, 2, 2, 5, 14, 14));                 // ubfx w14, w14, #2, #4
        entries = 16;
    }
    const auto adr_base = w.size();
    w.push_back(0);                                                    // adr x10, base (patched)
    switch (form) {
    case LlvmForm::ldrb: w.push_back(ldst_reg(0, 1, 14, 3, false, 18, 8)); break;   // ldrb w8, [x18, x14]
    case LlvmForm::ldrh: w.push_back(ldst_reg(1, 1, 14, 3, true, 18, 8)); break;    // ldrh w8, [x18, x14, lsl #1]
    case LlvmForm::ldrsw: w.push_back(ldst_reg(2, 2, 14, 3, true, 18, 8)); break;   // ldrsw x8, [x18, x14, lsl #2]
    }
    if (clobber) w.push_back(logic_imm(false, 0, 0, 0, 3, 14, 14));   // and w14, w14, #15 (x14 rewritten)
    w.push_back(add_shift(true, false, false, 0, 8, 2, 10, 9));        // add x9, x10, x8, lsl #2
    s.br = w.size();
    w.push_back(br(9));
    s.base = w.size();
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < cases; ++k) {
        s.case_at.push_back(w.size());
        const unsigned n = 1 + rng.below(4);
        for (unsigned j = 0; j < n; ++j) w.push_back(g.data_or_memory());
        if (k + 1 == cases || rng.chance(75)) {
            to_tail.push_back(w.size());
            w.push_back(b(0));
        }
    }
    const auto deflt = w.size();
    for (unsigned j = 0, n = 1 + rng.below(2); j < n; ++j) w.push_back(g.data_or_memory());
    const auto tail = w.size();
    w.push_back(add_imm(false, true, true, 12, 12, 1));   // subs w12, w12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    s.table = w.size();
    s.entries = entries;
    const unsigned bytes = form == LlvmForm::ldrb ? 1 : form == LlvmForm::ldrh ? 2 : 4;
    std::vector<std::uint8_t> data(entries * bytes);
    for (unsigned k = 0; k < entries; ++k) {
        const auto v = static_cast<std::uint32_t>(s.case_at[k % cases] - s.base);   // (target - base) / 4
        std::memcpy(data.data() + k * bytes, &v, bytes);
    }
    data.resize((data.size() + 3) & ~std::size_t{3});
    for (std::size_t at = 0; at < data.size(); at += 4) {
        std::uint32_t word;
        std::memcpy(&word, data.data() + at, 4);
        w.push_back(word);
    }
    const auto end = w.size();
    const auto rel_words = [](std::size_t from, std::size_t to) {
        return static_cast<std::int32_t>(to) - static_cast<std::int32_t>(from);
    };
    if (to_default != SIZE_MAX) w[to_default] |= (static_cast<std::uint32_t>(rel_words(to_default, deflt)) & 0x7ffff) << 5;
    for (const auto at : to_tail) w[at] = b(rel_words(at, tail));
    w[to_end] = b(rel_words(to_end, end));
    w[adr_base] = adr(false, 4 * rel_words(adr_base, s.base), 10);
    if (adrp) {
        const auto table_pc = program_base + 4 * s.table, def_pc = program_base + 4 * table_def;
        const auto pages = static_cast<std::int64_t>(table_pc >> 12) - static_cast<std::int64_t>(def_pc >> 12);
        w[table_def] = adr(true, static_cast<std::int32_t>(pages), 18);
        w[table_def + 1] = add_imm(true, false, false, 18, 18, static_cast<unsigned>(table_pc & 0xfff));
    } else {
        w[table_def] = adr(false, 4 * rel_words(table_def, s.table), 18);
    }
    return s;
}

// Computed goto (label table): absolute 8-byte case addresses, the table
// address set up once before the loop (ADRP+ADD), `goto *labels[x14]`.
// bounded: and w14, w14, #15 right before the load (16 entries); else the
// index is masked earlier and moved (add x14, x14, #0), so no bound is in
// sight and the planner sizes the table from its contents (a zero entry
// ends it).
// mov w12, #iterations; adrp x18, labels; add x18, x18, #lo12
// dispatch: add x14, x14, #step; and w14, w14, #mask; [add x14, x14, #0]; ldr x9, [x18, x14, lsl #3]; br x9
// scaled false: the index is a byte offset (lsl x14, x14, #3; ldr x9, [x18, x14]).
Switch make_label_switch(std::uint64_t seed, std::uint64_t program_base, bool bounded, unsigned cases, unsigned iterations,
                         bool scaled = true) {
    using namespace a64;
    Rng rng(seed);
    InstGen g(rng);
    Switch s;
    auto& w = s.words;
    const unsigned entries = bounded ? 16 : cases;   // unbounded: cases is a power of two (the mask)
    w.push_back(movw(false, 2, 0, iterations, 12));
    const auto table_def = w.size();
    w.push_back(0);   // adrp x18 (patched)
    w.push_back(0);   // add x18, x18, #lo12 (patched)
    const auto dispatch = w.size();
    w.push_back(add_imm(true, false, false, 14, 14, 1 + rng.below(5)));
    if (bounded) {
        w.push_back(logic_imm(false, 0, 0, 0, 3, 14, 14));                                   // and w14, w14, #15
    } else {
        w.push_back(logic_imm(false, 0, 0, 0, static_cast<unsigned>(std::countr_zero(cases)) - 1, 14, 14));   // and w14, #cases-1
        w.push_back(add_imm(true, false, false, 14, 14, 0));                                 // add x14, x14, #0
    }
    if (scaled) {
        w.push_back(ldst_reg(3, 1, 14, 3, true, 18, 9));                                     // ldr x9, [x18, x14, lsl #3]
    } else {
        w.push_back(bitfield(true, 2, 61, 60, 14, 14));                                      // lsl x14, x14, #3
        w.push_back(ldst_reg(3, 1, 14, 3, false, 18, 9));                                    // ldr x9, [x18, x14]
    }
    s.br = w.size();
    w.push_back(br(9));
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < cases; ++k) {
        s.case_at.push_back(w.size());
        const unsigned n = 1 + rng.below(4);
        for (unsigned j = 0; j < n; ++j) w.push_back(g.data_or_memory());
        if (k + 1 == cases || rng.chance(75)) {
            to_tail.push_back(w.size());
            w.push_back(b(0));
        }
    }
    const auto tail = w.size();
    w.push_back(add_imm(false, true, true, 12, 12, 1));   // subs w12, w12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    if (w.size() & 1) w.push_back(0);   // 8-byte aligned entries
    s.table = w.size();
    s.entries = entries;
    for (unsigned k = 0; k < entries; ++k) {
        const auto target = program_base + 4 * s.case_at[k % cases];
        w.push_back(static_cast<std::uint32_t>(target));
        w.push_back(static_cast<std::uint32_t>(target >> 32));
    }
    w.push_back(0);   // a zero entry ends an unbounded table
    w.push_back(0);
    const auto end = w.size();
    for (const auto at : to_tail) w[at] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(at));
    w[to_end] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));
    const auto table_pc = program_base + 4 * s.table, def_pc = program_base + 4 * table_def;
    const auto pages = static_cast<std::int64_t>(table_pc >> 12) - static_cast<std::int64_t>(def_pc >> 12);
    w[table_def] = adr(true, static_cast<std::int32_t>(pages), 18);
    w[table_def + 1] = add_imm(true, false, false, 18, 18, static_cast<unsigned>(table_pc & 0xfff));
    return s;
}

// A dispatch table in memory (V8's Ignition: the table base is a long-lived
// register, no ADR/ADRP in sight), so the BR is not a label table but goes
// through the thread's table shadow (GuestContext::table_shadow).
// x18 = the table (the caller sets it); offset: added to the index first
// (indices past the shadow's slots); uxtw: the load extends wI; copy: the
// target goes through another register (V8: `mov x17, xN; br x17`).
// dispatch: add x14, x14, #step; and w14, w14, #15; add x15, x14, #offset
//           ldr x9, [x18, x15, lsl #3] (or [x18, w15, uxtw #3]); br x9
//           (copy: ldr x10, ...; mov x11, x10; mov x9, x11; br x9)
Switch make_heap_switch(std::uint64_t seed, std::uint64_t program_base, unsigned cases, unsigned iterations,
                        unsigned offset, bool uxtw, bool copy = false) {
    using namespace a64;
    Rng rng(seed);
    InstGen g(rng);
    Switch s;
    auto& w = s.words;
    w.push_back(movw(false, 2, 0, iterations, 12));
    const auto dispatch = w.size();
    w.push_back(add_imm(true, false, false, 14, 14, 1 + rng.below(5)));
    w.push_back(logic_imm(false, 0, 0, 0, 3, 14, 14));                       // and w14, w14, #15
    w.push_back(add_imm(true, false, false, 15, 14, offset));                // add x15, x14, #offset
    w.push_back(ldst_reg(3, 1, 15, uxtw ? 2 : 3, true, 18, copy ? 10 : 9));   // ldr x9, [x18, x15, lsl #3]
    if (copy) {
        w.push_back(logic_shift(true, 1, 0, false, 10, 0, 31, 11));          // mov x11, x10
        w.push_back(logic_shift(true, 1, 0, false, 11, 0, 31, 9));           // mov x9, x11
    }
    s.br = w.size();
    w.push_back(br(9));
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < cases; ++k) {
        s.case_at.push_back(w.size());
        const unsigned n = 1 + rng.below(4);
        for (unsigned j = 0; j < n; ++j) w.push_back(g.data_or_memory());
        if (k + 1 == cases || rng.chance(75)) {
            to_tail.push_back(w.size());
            w.push_back(b(0));
        }
    }
    const auto tail = w.size();
    w.push_back(add_imm(false, true, true, 12, 12, 1));   // subs w12, w12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    if (w.size() & 1) w.push_back(0);   // 8-byte aligned entries
    s.table = w.size();
    s.entries = offset + 16;
    for (unsigned k = 0; k < s.entries; ++k) {
        const auto target = program_base + 4 * s.case_at[k % cases];
        w.push_back(static_cast<std::uint32_t>(target));
        w.push_back(static_cast<std::uint32_t>(target >> 32));
    }
    const auto end = w.size();
    for (const auto at : to_tail) w[at] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(at));
    w[to_end] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));
    return s;
}

bool shadows_enabled() { return (core::codegen_layout_bits() & 32u) == 0; }

const char* form_name(Form f) {
    switch (f) {
    case Form::ldrsw_base: return "ldrsw+base";
    case Form::ldrsb_self: return "ldrsb";
    case Form::ldrb_self: return "ldrb";
    default: return "ldrh";
    }
}
const char* bound_name(Bound b) {
    switch (b) {
    case Bound::cmp_hi: return "cmp/b.hi";
    case Bound::cmp_hs: return "cmp/b.hs";
    case Bound::and_mask: return "and";
    default: return "uxtb";
    }
}

} // namespace

// Every form and bound: the dispatch is recognised (the BR's plan has a
// table), the program runs like the reference, and no lookup exits happen.
FX_TEST(table_dispatch_forms) {
    auto& r = runner();
    const Form forms[] = {Form::ldrsw_base, Form::ldrsb_self};   // unsigned entries: table_dispatch_unsigned_entries
    const Bound bounds[] = {Bound::cmp_hi, Bound::cmp_hs, Bound::and_mask, Bound::uxtb};
    unsigned seed = 1;
    for (const auto form : forms) {
        for (const auto bound : bounds) {
            for (unsigned cases : {3u, 7u, 12u}) {
                if (bound == Bound::and_mask && cases > 8) continue;   // and w14, #7: 8 entries
                const auto s = make_switch(seed++, form, bound, cases, 40);
                const std::string label = std::string(form_name(form)) + " " + bound_name(bound) + " " + std::to_string(cases);
                auto ctx = r.context(900 + seed);
                FX_CHECK(r.run(s.words, ctx, label));
                if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);
                if (cases_in_region()) FX_CHECK(r.last.dispatches <= 3u);
            }
        }
    }
}

// Unsigned byte/halfword entries: targets after the base (an ADR before the
// cases; make_switch places cases before the table).
FX_TEST(table_dispatch_unsigned_entries) {
    auto& r = runner();
    using namespace a64;
    for (unsigned variant = 0; variant < 2; ++variant) {
        // 0 mov w12, #20
        // 1 L: add x14, x14, #1 ; 2 and w14, w14, #3
        // 3 adr x9, table ; 4 ldrb/ldrh w8, [x9, w14, uxtw #s]; 5 adr x10, base; 6 add x8, x10, x8, lsl #2; 7 br x8
        // 8 base: case0: add x0, x0, #1; b tail ; case1 ...
        std::vector<std::uint32_t> w = {movw(false, 2, 0, 20, 12), add_imm(true, false, false, 14, 14, 1),
                                        logic_imm(false, 0, 0, 0, 1, 14, 14), 0,
                                        variant == 0 ? ldst_reg(0, 1, 14, 2, false, 9, 8) : ldst_reg(1, 1, 14, 2, true, 9, 8),
                                        0, add_shift(true, false, false, 0, 8, 2, 10, 8), br(8)};
        const std::size_t base = w.size();
        std::vector<std::size_t> cases, tails;
        for (unsigned k = 0; k < 4; ++k) {
            cases.push_back(w.size());
            w.push_back(add_imm(true, false, false, k, k, 1 + k));
            w.push_back(ldst_uimm(3, 1, k, 16, 20 + k));   // ldr x2k, [x16, #8k]
            tails.push_back(w.size());
            w.push_back(b(0));
        }
        const auto tail = w.size();
        w.push_back(add_imm(false, true, true, 12, 12, 1));
        w.push_back(bcond(1, 1 - static_cast<std::int32_t>(w.size())));
        const auto to_end = w.size();
        w.push_back(b(0));
        const auto table = w.size();
        if (variant == 0) {
            w.push_back(static_cast<std::uint32_t>(cases[0] - base) | static_cast<std::uint32_t>(cases[1] - base) << 8 |
                        static_cast<std::uint32_t>(cases[2] - base) << 16 | static_cast<std::uint32_t>(cases[3] - base) << 24);
        } else {
            w.push_back(static_cast<std::uint32_t>(cases[0] - base) | static_cast<std::uint32_t>(cases[1] - base) << 16);
            w.push_back(static_cast<std::uint32_t>(cases[2] - base) | static_cast<std::uint32_t>(cases[3] - base) << 16);
        }
        const auto end = w.size();
        for (const auto at : tails) w[at] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(at));
        w[to_end] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));
        w[3] = adr(false, 4 * (static_cast<std::int32_t>(table) - 3), 9);
        w[5] = adr(false, 4 * (static_cast<std::int32_t>(base) - 5), 10);
        FX_CHECK(r.run(w, r.context(950 + variant), variant == 0 ? "ldrb entries" : "ldrh entries"));
        if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);
    }
}

// Random interpreter-like loops: random case bodies (flags, memory, all
// registers), random index steps, indices beyond the bound (default path).
FX_TEST(table_dispatch_differential) {
    auto& r = runner();
    const Form forms[] = {Form::ldrsw_base, Form::ldrsb_self};
    const Bound bounds[] = {Bound::cmp_hi, Bound::cmp_hs, Bound::and_mask, Bound::uxtb};
    for (unsigned k = 0; k < 400; ++k) {
        Rng rng(77000 + k);
        const auto form = forms[rng.below(2)];
        const auto bound = bounds[rng.below(4)];
        const unsigned cases = bound == Bound::and_mask ? 2 + rng.below(7) : 2 + rng.below(14);
        const auto s = make_switch(77000 + k, form, bound, cases, 1 + rng.below(60));
        FX_CHECK(r.run(s.words, r.context(78000 + k), "table fuzz " + std::to_string(k)));
    }
}

// LLVM tables (target dispatch): every entry size, table address set up by
// ADR or ADRP+ADD before the loop, the index rewritten before the BR or not.
// Recognised (no lookup exits) and run like the reference.
FX_TEST(table_dispatch_llvm_forms) {
    auto& r = runner();
    const LlvmForm forms[] = {LlvmForm::ldrb, LlvmForm::ldrh, LlvmForm::ldrsw};
    unsigned seed = 1;
    for (const auto form : forms)
        for (const bool adrp : {false, true})
            for (const bool clobber : {false, true})
                for (unsigned cases : {2u, 5u, 16u}) {
                    const auto s = make_llvm_switch(seed++, r.program_base(), form, adrp, clobber, cases, 40);
                    const std::string label = std::string("llvm ") + (form == LlvmForm::ldrb ? "ldrb" : form == LlvmForm::ldrh ? "ldrh" : "ldrsw") +
                                              (adrp ? " adrp" : " adr") + (clobber ? " clobber " : " ") + std::to_string(cases);
                    FX_CHECK(r.run(s.words, r.context(1200 + seed), label));
                    if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);
                    if (cases_in_region()) FX_CHECK(r.last.dispatches <= 3u);
                }
}

// Bounds from the index itself: a byte load (256 entries) or a bit field (16), no compare.
FX_TEST(table_dispatch_llvm_index_bounds) {
    auto& r = runner();
    unsigned seed = 300;
    for (const auto bound : {LlvmBound::byte_load, LlvmBound::ubfx})
        for (const auto form : {LlvmForm::ldrb, LlvmForm::ldrsw})
            for (unsigned cases : {3u, 9u}) {
                const auto s = make_llvm_switch(seed++, r.program_base(), form, true, false, cases, 30, bound);
                const std::string label = std::string(bound == LlvmBound::byte_load ? "llvm ldrb-index " : "llvm ubfx-index ") +
                                          (form == LlvmForm::ldrb ? "ldrb " : "ldrsw ") + std::to_string(cases);
                FX_CHECK(r.run(s.words, r.context(1300 + seed), label));
                if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);
            }
}

// Computed goto through a label table: bounded by a mask, or sized from the
// table's contents; no lookup exits, runs like the reference.
FX_TEST(table_dispatch_label_tables) {
    auto& r = runner();
    unsigned seed = 600;
    for (const bool bounded : {true, false})
        for (const bool scaled : {true, false})
        for (unsigned cases : {2u, 4u, 8u}) {
            const auto s = make_label_switch(seed++, r.program_base(), bounded, cases, 40, scaled);
            const std::string label = std::string("label table ") + (bounded ? "bounded " : "sized ") +
                                      (scaled ? "" : "byte-offset ") + std::to_string(cases);
            FX_CHECK(r.run(s.words, r.context(1400 + seed), label));
            if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);
            if (cases_in_region()) FX_CHECK(r.last.dispatches <= 3u);
        }
    for (unsigned k = 0; k < 100; ++k) {
        Rng rng(91000 + k);
        const auto s = make_label_switch(91000 + k, r.program_base(), rng.chance(50), 2u << rng.below(3), 1 + rng.below(60),
                                         rng.chance(50));
        FX_CHECK(r.run(s.words, r.context(92000 + k), "label table fuzz " + std::to_string(k)));
    }
}

// A label table changed after translation (entries rotated, one pointing into
// the middle of a case) still sends the BR where the guest's load points.
FX_TEST(table_dispatch_label_table_changed) {
    auto& r = runner();
    const auto s = make_label_switch(4545, r.program_base(), true, 4, 1);
    const auto base = r.program_base();
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(base));
    std::memcpy(program, s.words.data(), s.words.size() * 4);
    program[s.words.size()] = a64::brk(0xdead);
    const BufferCode code(r.code_base(), base + (s.words.size() + 1) * 4);
    r.machine.reset();
    auto ctx = r.context(4546);
    ctx.pc = base;
    auto res = r.machine.run(ctx, code, kReturnToHostPc);
    FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
    auto* table = reinterpret_cast<std::uint64_t*>(program + s.table);
    std::vector<std::uint64_t> entries(table, table + s.entries);
    for (unsigned k = 0; k < s.entries; ++k) table[k] = entries[(k + 1) % s.entries];
    table[3] = base + 4 * (s.case_at[0] + 1);   // not a case
    for (unsigned trial = 0; trial < 6; ++trial) {
        auto start = r.context(4600 + trial);
        start.x[14] = trial;
        start.x[12] = 1;
        start.pc = base + 4 * 3;   // dispatch (after the table setup)
        start.x[18] = base + 4 * s.table;
        auto expect = start;
        r.reset_memory();
        const auto stop = base + s.words.size() * 4;
        for (unsigned steps = 0; expect.pc != stop && steps < 100000; ++steps) {
            const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expect.pc));
            FX_REQUIRE(testing::oracle_step(expect, word));
        }
        r.reset_memory();
        auto got = start;
        res = r.machine.run(got, code, kReturnToHostPc);
        FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
        got.pc = stop;
        testing::compare_contexts(expect, got, "label table changed " + std::to_string(trial));
    }
}

// A BR through a dispatch table in memory takes the table shadow: the program
// runs like the reference for every index form (lsl #3, uxtw #3, indices past
// the shadow's slots), and on a second run every target is in the shadow
// already (one miss: the first BR makes the shadow the thread's again).
FX_TEST(table_dispatch_heap_table_shadow) {
    auto& r = runner();
    unsigned seed = 700;
    for (const unsigned offset : {0u, 1000u, 2000u})
        for (const bool uxtw : {false, true})
            for (unsigned cases : {2u, 5u, 8u}) {
                const bool copy = cases == 5;
                const auto s = make_heap_switch(seed++, r.program_base(), cases, 40, offset, uxtw, copy);
                auto start = r.context(1700 + seed);
                start.x[18] = r.program_base() + 4 * s.table;
                const std::string label = "heap table " + std::to_string(cases) + (uxtw ? " uxtw" : "") +
                                          (copy ? " copy" : "") + " +" + std::to_string(offset);
                FX_CHECK(r.run(s.words, start, label));
                if (!shadows_enabled() || offset + 16 > kTableShadowSlots) continue;
                FX_CHECK(r.last.shadow_misses >= 1u);
                FX_CHECK_EQ(r.last.lookups, 0u);
                // Again on the same translations and shadows.
                const BufferCode code(r.code_base(), r.program_base() + (s.words.size() + 1) * 4);
                r.reset_memory();
                auto again = start;
                again.pc = r.program_base();
                const auto res = r.machine.run(again, code, kReturnToHostPc);
                FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
                FX_CHECK_EQ(res.shadow_misses, 1u);
                FX_CHECK_EQ(res.lookups, 0u);
                FX_CHECK_EQ(res.translations, 0u);
                again.pc = r.expected.pc;
                testing::compare_contexts(r.expected, again, label + " again");
            }
    for (unsigned k = 0; k < 60; ++k) {
        Rng rng(93000 + k);
        const auto s = make_heap_switch(93000 + k, r.program_base(), 1 + rng.below(8), 1 + rng.below(60),
                                        rng.chance(80) ? 0u : 1020u, rng.chance(50), rng.chance(50));
        auto start = r.context(94000 + k);
        start.x[18] = r.program_base() + 4 * s.table;
        FX_CHECK(r.run(s.words, start, "heap table fuzz " + std::to_string(k)));
    }
}

// The table changed after its shadow was filled (entries rotated, one pointing
// into the middle of a case), and a second table at another base: the BR still
// goes where the guest's load points.
FX_TEST(table_dispatch_heap_table_changed) {
    auto& r = runner();
    const auto s = make_heap_switch(4747, r.program_base(), 4, 1, 0, false);
    const auto base = r.program_base();
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(base));
    // The program, brk, then a copy of its table (another base).
    std::vector<std::uint32_t> words = s.words;
    words.push_back(a64::brk(0xdead));
    if (words.size() & 1) words.push_back(0);
    const auto copy = words.size();
    words.insert(words.end(), s.words.begin() + static_cast<std::ptrdiff_t>(s.table), s.words.end());
    std::memcpy(program, words.data(), words.size() * 4);
    const BufferCode code(r.code_base(), base + words.size() * 4);
    r.machine.reset();
    auto* table = reinterpret_cast<std::uint64_t*>(program + s.table);
    const std::vector<std::uint64_t> entries(table, table + s.entries);
    const auto run_one = [&](unsigned trial, std::uint64_t table_pc, const std::string& label) {
        auto start = r.context(4800 + trial);
        start.x[14] = trial;
        start.x[12] = 1;
        start.pc = base;
        start.x[18] = table_pc;
        auto expect = start;
        r.reset_memory();
        const auto end = base + s.words.size() * 4;
        for (unsigned steps = 0; expect.pc != end && steps < 100000; ++steps) {
            const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expect.pc));
            FX_REQUIRE(testing::oracle_step(expect, word));
        }
        r.reset_memory();
        auto got = start;
        const auto res = r.machine.run(got, code, kReturnToHostPc);
        FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
        got.pc = end;
        testing::compare_contexts(expect, got, label + " " + std::to_string(trial));
    };
    for (unsigned trial = 0; trial < 16; ++trial) run_one(trial, base + 4 * s.table, "heap table");
    for (unsigned k = 0; k < s.entries; ++k) table[k] = entries[(k + 1) % s.entries];
    table[3] = base + 4 * (s.case_at[0] + 1);   // not a case
    for (unsigned trial = 0; trial < 16; ++trial) run_one(trial, base + 4 * s.table, "heap table changed");
    for (unsigned trial = 0; trial < 16; ++trial) run_one(trial, base + 4 * copy, "heap table copy");
}

// Random LLVM-form loops, as table_dispatch_differential.
FX_TEST(table_dispatch_llvm_differential) {
    auto& r = runner();
    for (unsigned k = 0; k < 300; ++k) {
        Rng rng(88000 + k);
        const auto form = static_cast<LlvmForm>(rng.below(3));
        const auto bound = static_cast<LlvmBound>(rng.below(3));
        const auto s = make_llvm_switch(88000 + k, r.program_base(), form, rng.chance(50), rng.chance(50),
                                        2 + rng.below(15), 1 + rng.below(60), bound);
        FX_CHECK(r.run(s.words, r.context(89000 + k), "llvm table fuzz " + std::to_string(k)));
    }
}

// The run-time check of target dispatch: table entries rewritten after
// translation still send the BR where the guest computes, including to pcs
// that are not cases (the fallback).
FX_TEST(table_dispatch_llvm_changed_table) {
    auto& r = runner();
    const auto s = make_llvm_switch(4343, r.program_base(), LlvmForm::ldrb, true, true, 6, 1);
    const auto base = r.program_base();
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(base));
    std::memcpy(program, s.words.data(), s.words.size() * 4);
    program[s.words.size()] = a64::brk(0xdead);
    const BufferCode code(r.code_base(), base + (s.words.size() + 1) * 4);
    r.machine.reset();
    auto ctx = r.context(4344);
    ctx.pc = base;
    auto res = r.machine.run(ctx, code, kReturnToHostPc);
    FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
    // Byte entries rotated, and entry 5 pointing at the second instruction of case 0.
    auto* table = reinterpret_cast<std::uint8_t*>(program + s.table);
    std::uint8_t rotated[6];
    for (unsigned k = 0; k < 6; ++k) rotated[k] = table[(k + 1) % 6];
    std::memcpy(table, rotated, 6);
    table[5] = static_cast<std::uint8_t>(s.case_at[0] + 1 - s.base);
    for (unsigned trial = 0; trial < 6; ++trial) {
        auto start = r.context(4400 + trial);
        start.x[14] = trial + 15;   // the loop adds its step, then masks
        start.x[12] = 1;
        start.pc = base + 4 * 3;    // dispatch (after the table setup)
        start.x[18] = base + 4 * s.table;
        auto expect = start;
        r.reset_memory();
        const auto stop = base + s.words.size() * 4;
        for (unsigned steps = 0; expect.pc != stop && steps < 100000; ++steps) {
            const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expect.pc));
            FX_REQUIRE(testing::oracle_step(expect, word));
        }
        r.reset_memory();
        auto got = start;
        res = r.machine.run(got, code, kReturnToHostPc);
        FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
        got.pc = stop;
        testing::compare_contexts(expect, got, "llvm changed table " + std::to_string(trial));
    }
}

// Cases outside the region (formation limit): table entries become direct
// exits; the program still runs like the reference.
FX_TEST(table_dispatch_cases_outside_region) {
    auto& r = runner();
    const auto saved = r.machine.options.max_instructions;
    for (unsigned limit : {12u, 20u, 32u}) {
        r.machine.options.max_instructions = limit;
        const auto s = make_switch(500 + limit, Form::ldrsw_base, Bound::cmp_hi, 10, 30);
        FX_CHECK(r.run(s.words, r.context(510 + limit), "outside region " + std::to_string(limit)));
    }
    r.machine.options.max_instructions = saved;
}

// The run-time check: a table rewritten after translation (no retranslation)
// still sends the BR where the guest's own computation points.
FX_TEST(table_dispatch_changed_table) {
    auto& r = runner();
    const auto s = make_switch(4242, Form::ldrsw_base, Bound::cmp_hi, 6, 1);
    const auto base = r.program_base();
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(base));
    std::memcpy(program, s.words.data(), s.words.size() * 4);
    program[s.words.size()] = a64::brk(0xdead);
    const BufferCode code(r.code_base(), base + (s.words.size() + 1) * 4);
    const auto region = translate_region(base, code);
    if (cases_in_region()) {
        // One region holds the dispatch and every case: no exit targets a case.
        for (const auto at : s.case_at) {
            bool covered = false;
            for (const auto& [b, e] : region.guest_ranges) covered |= base + 4 * at >= b && base + 4 * at < e;
            FX_CHECK(covered);
            for (const auto& e : region.exits) FX_CHECK(e.target_pc != base + 4 * at);
        }
    }
    // Translate and run once with index 0 (x14 = 0 - step... set directly).
    r.machine.reset();
    auto ctx = r.context(4243);
    ctx.pc = base;
    auto res = r.machine.run(ctx, code, kReturnToHostPc);
    FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
    // Rotate the table entries (the program's code is unchanged; the
    // translation is kept), then run again: compare with the reference.
    std::vector<std::int32_t> entries(s.entries);
    std::memcpy(entries.data(), program + s.table, entries.size() * 4);
    for (std::size_t k = 0; k < entries.size(); ++k) {
        const auto v = entries[(k + 1) % entries.size()];
        std::memcpy(program + s.table + k, &v, 4);
    }
    for (unsigned trial = 0; trial < 6; ++trial) {
        auto start = r.context(4300 + trial);
        start.x[14] = trial;
        start.x[12] = 1;       // one dispatch: start after mov w12
        start.pc = base + 4;
        auto expect = start;
        r.reset_memory();
        const auto stop = base + s.words.size() * 4;
        for (unsigned steps = 0; expect.pc != stop && steps < 100000; ++steps) {
            const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expect.pc));
            FX_REQUIRE(testing::oracle_step(expect, word));
        }
        r.reset_memory();
        auto got = start;
        res = r.machine.run(got, code, kReturnToHostPc);
        FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
        got.pc = stop;
        testing::compare_contexts(expect, got, "changed table " + std::to_string(trial));
    }
}

// Apple ld64 relaxes ADRP+ADD into ADR+NOP before an LLVM 32-bit byte-offset
// switch table (`adr x8, table; nop; adr x9, base; ldrsw x10, [x8, x27, lsl #2];
// add x9, x9, x10; br x9`). The NOP must not break table or bound recognition.
FX_TEST(table_dispatch_adr_nop_llvm) {
    auto& r = runner();
    using namespace a64;
    std::vector<std::uint32_t> w;
    w.push_back(movw(false, 2, 0, 40, 12));                         // 0  movz w12, #40
    const auto dispatch = w.size();
    w.push_back(add_imm(false, false, false, 27, 27, 1));           // 1  add w27, w27, #1
    w.push_back(logic_imm(false, 0, 0, 0, 1, 27, 27));              // 2  and w27, w27, #3
    w.push_back(add_imm(false, true, true, 31, 27, 3));             // 3  cmp w27, #3
    const auto b_hi_at = w.size();
    w.push_back(bcond(8, 0));                                       // 4  b.hi default
    const auto adr_tab_at = w.size();
    w.push_back(adr(false, 0, 8));                                  // 5  adr x8, table
    w.push_back(nop());                                             // 6  nop (ld64 relaxation)
    const auto adr_base_at = w.size();
    w.push_back(adr(false, 0, 9));                                  // 7  adr x9, base (self)
    w.push_back(ldst_reg(2, 2, 27, 3, true, 8, 10));                // 8  ldrsw x10, [x8, x27, lsl #2]
    w.push_back(add_shift(true, false, false, 0, 10, 0, 9, 9));     // 9  add x9, x9, x10
    w.push_back(br(9));                                             // 10 br x9
    std::size_t case_at[4]{};
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < 4; ++k) {
        case_at[k] = w.size();
        w.push_back(add_imm(true, false, false, 0, 0, 10 + k * 7));
        to_tail.push_back(w.size());
        w.push_back(b(0));
    }
    const auto def_at = w.size();
    w.push_back(add_imm(true, false, false, 0, 0, 99));
    const auto tail = w.size();
    w.push_back(add_imm(false, true, true, 12, 12, 1));             // subs w12, w12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    const auto tab_at = w.size();
    for (unsigned k = 0; k < 4; ++k) {
        const auto off = static_cast<std::int32_t>(4 * (static_cast<std::int64_t>(case_at[k]) - static_cast<std::int64_t>(adr_base_at)));
        w.push_back(static_cast<std::uint32_t>(off));
    }
    const auto end = w.size();
    w[b_hi_at] = bcond(8, static_cast<std::int32_t>(def_at) - static_cast<std::int32_t>(b_hi_at));
    w[adr_tab_at] = adr(false, static_cast<std::int32_t>(4 * (tab_at - adr_tab_at)), 8);
    for (const auto at : to_tail) w[at] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(at));
    w[to_end] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));

    const auto base = r.program_base();
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(base));
    std::memcpy(program, w.data(), w.size() * 4);
    program[w.size()] = brk(0xdead);
    const BufferCode code(r.code_base(), base + (w.size() + 1) * 4);
    const auto region = translate_region(base, code);
    if (cases_in_region()) {
        for (const auto at : case_at) {
            bool covered = false;
            for (const auto& [b0, e0] : region.guest_ranges) covered |= base + 4 * at >= b0 && base + 4 * at < e0;
            FX_CHECK(covered);
        }
    }
    auto ctx = r.context(9191);
    FX_CHECK(r.run(w, ctx, "adr+nop llvm table"));
}

// V8 15 JSDispatchTable pattern: stride-16 entries indexed by `handle >> 8`
// with in-place base advance (`add x20, x20, x2, lsl #4`) and overwritten
// index (`ldr x2, [x20]; mov x17, x2; br x17`). Must hit table_shadow.
FX_TEST(table_dispatch_stride16_shadow) {
    auto& r = runner();
    using namespace a64;
    std::vector<std::uint32_t> w;
    w.push_back(movw(false, 2, 0, 40, 12));                         // 0  movz w12, #40
    const auto dispatch = w.size();
    w.push_back(add_imm(true, false, false, 14, 14, 1));            // 1  add x14, x14, #1
    w.push_back(logic_imm(true, 0, 1, 0, 2, 14, 14));               // 2  and x14, x14, #7
    w.push_back(bitfield(true, 2, 56, 55, 14, 4));                  // 3  lsl x4, x14, #8 (dispatch handle)
    w.push_back(logic_shift(true, 1, 0, false, 18, 0, 31, 20));     // 4  mov x20, x18 (table base)
    w.push_back(bitfield(true, 2, 8, 63, 4, 2));                    // 5  lsr x2, x4, #8
    w.push_back(add_shift(true, false, false, 0, 2, 4, 20, 20));    // 6  add x20, x20, x2, lsl #4
    w.push_back(ldst_uimm(3, 1, 0, 20, 2));                         // 7  ldr x2, [x20]
    w.push_back(logic_shift(true, 1, 0, false, 2, 0, 31, 17));      // 8  mov x17, x2
    w.push_back(br(17));                                            // 9  br x17
    std::size_t case_at[4]{};
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < 4; ++k) {
        case_at[k] = w.size();
        w.push_back(add_imm(true, false, false, 0, 0, 3 + k * 5));
        to_tail.push_back(w.size());
        w.push_back(b(0));
    }
    const auto tail = w.size();
    w.push_back(add_imm(false, true, true, 12, 12, 1));
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    if (w.size() & 1) w.push_back(0);
    const auto tab_at = w.size();
    const auto base = r.program_base();
    for (unsigned k = 0; k < 8; ++k) {
        const auto target = base + 4 * case_at[k % 4];
        w.push_back(static_cast<std::uint32_t>(target));
        w.push_back(static_cast<std::uint32_t>(target >> 32));
        w.push_back(0x12345678u + k);   // second 8-byte metadata word of stride-16 entry
        w.push_back(0);
    }
    const auto end = w.size();
    for (const auto at : to_tail) w[at] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(at));
    w[to_end] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));

    auto start = r.context(9292);
    start.x[18] = base + 4 * tab_at;
    FX_CHECK(r.run(w, start, "stride16 table dispatch"));

    // High indices (e.g. V8 JSDispatchTable handles >= kTableShadowSlots) with
    // adjusted base pointer must also execute identically to the oracle.
    const std::uint64_t high_idx = kTableShadowSlots + 37u;
    std::vector<std::uint32_t> w2 = w;
    // Replace `and x14, x14, #7` with `and x14, x14, #7; add x14, x14, #high_idx` via x15 holding high_idx.
    w2[2] = logic_imm(true, 0, 1, 0, 2, 14, 14);
    w2.insert(w2.begin() + 3, add_shift(true, false, false, 0, 15, 0, 14, 14));   // add x14, x14, x15
    // Re-patch branches shifted by +1 instruction (case_at and tail shifted by +1).
    for (unsigned k = 0; k < 4; ++k) w2[to_tail[k] + 1] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(to_tail[k]));
    w2[tail + 2] = bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(tail + 2));
    w2[to_end + 1] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));
    // Re-adjust table entries for the +1 shift (aligned to 8 bytes if needed).
    FX_REQUIRE(w2.size() == w.size() + 1);
    const std::size_t tab2 = tab_at + 1;
    // Ensure 8-byte alignment isn't needed (x64 unaligned ok), update targets by +4.
    for (unsigned k = 0; k < 8; ++k) {
        const auto target = base + 4 * (case_at[k % 4] + 1);
        w2[tab2 + 4 * k + 0] = static_cast<std::uint32_t>(target);
        w2[tab2 + 4 * k + 1] = static_cast<std::uint32_t>(target >> 32);
    }
    auto start2 = r.context(9393);
    start2.x[15] = high_idx;
    start2.x[18] = (base + 4 * tab2) - high_idx * 16u;
    FX_CHECK(r.run(w2, start2, "stride16 high-index dispatch"));
}

// Wave F1: Clang/LLVM switch with LSL #2 (`ldrsw x10, [x8, x11, lsl #2]`) and
// unmodified index whose case span exceeds kMaxTargetSpan (16 KiB) — Cycles
// SVM shader pattern. Must use indexed dispatch (not target-span rejected),
// AND must also recognize image-code switches whose cases exceed window_forward.
FX_TEST(table_dispatch_lsl_wide_span) {
    auto& r = runner();
    using namespace a64;
    std::vector<std::uint32_t> w;
    w.push_back(movw(false, 2, 0, 32, 12));                         // 0  movz w12, #32
    const auto dispatch = w.size();
    w.push_back(add_imm(false, false, false, 11, 11, 1));           // 1  add w11, w11, #1 (zero-extends x11)
    w.push_back(logic_imm(false, 0, 0, 0, 1, 11, 11));              // 2  and w11, w11, #3
    w.push_back(add_imm(false, true, true, 31, 11, 3));             // 3  cmp w11, #3
    const auto b_hi_at = w.size();
    w.push_back(bcond(8, 0));                                       // 4  b.hi default
    const auto adr_tab_at = w.size();
    w.push_back(adr(false, 0, 8));                                  // 5  adr x8, table
    const auto adr_base_at = w.size();
    w.push_back(adr(false, 0, 9));                                  // 6  adr x9, base
    w.push_back(ldst_reg(2, 2, 11, 3, true, 8, 10));                // 7  ldrsw x10, [x8, x11, lsl #2]
    w.push_back(add_shift(true, false, false, 0, 10, 0, 9, 9));     // 8  add x9, x9, x10
    w.push_back(br(9));                                             // 9  br x9
    std::size_t case_at[4]{};
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < 4; ++k) {
        case_at[k] = w.size();
        w.push_back(add_imm(true, false, false, 0, 0, 5 + k * 3));
        to_tail.push_back(w.size());
        w.push_back(b(0));
        // Pad > 4200 words (~16.8 KiB) after case 0 so hi_t - lo_t > kMaxTargetSpan (16 KiB).
        if (k == 0) w.insert(w.end(), 4200, nop());
    }
    const auto def_at = w.size();
    w.push_back(add_imm(true, false, false, 0, 0, 77));
    const auto tail = w.size();
    w.push_back(add_imm(false, true, true, 12, 12, 1));             // subs w12, w12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    const auto tab_at = w.size();
    for (unsigned k = 0; k < 4; ++k) {
        const auto off = static_cast<std::int32_t>(4 * (static_cast<std::int64_t>(case_at[k]) - static_cast<std::int64_t>(adr_base_at)));
        w.push_back(static_cast<std::uint32_t>(off));
    }
    const auto end = w.size();
    w[b_hi_at] = bcond(8, static_cast<std::int32_t>(def_at) - static_cast<std::int32_t>(b_hi_at));
    w[adr_tab_at] = adr(false, static_cast<std::int32_t>(4 * (tab_at - adr_tab_at)), 8);
    for (const auto at : to_tail) w[at] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(at));
    w[to_end] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));

    // 1. Full region (unbounded): indexed dispatch handles >16 KiB span with 0 lookups.
    FX_CHECK(r.run(w, r.context(0xf501), "lsl wide span"));
    if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);

    // 2. Synthetic window_forward = 8192 (Mach-O without .pdata): cases past
    //    8 KiB become linkable direct exits instead of rejecting the table.
    const auto saved_fwd = r.machine.options.window_forward;
    const auto saved_bwd = r.machine.options.window_backward;
    r.machine.options.window_forward = 8192;
    r.machine.options.window_backward = 2048;
    FX_CHECK(r.run(w, r.context(0xf502), "lsl wide span windowed"));
    if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);
    r.machine.options.window_forward = saved_fwd;
    r.machine.options.window_backward = saved_bwd;
}

// Wave M2 / O2: Explicit regression test for interleaved 16-byte table_shadow
// (kTableShadowSlots = 8192) even when no_shadows is enabled by default, plus
// Wave O1 wide-arm switch direct exits preserving live NZCV into a case.
FX_TEST(table_dispatch_interleaved_shadow_explicit) {
    auto& r = runner();
    core::PlanOverrides o = core::default_plan_overrides();
    o.no_shadows = false;
    core::set_overrides_for_testing(&o);
    struct Guard {
        ~Guard() { core::set_overrides_for_testing(nullptr); }
    } guard;
    for (const unsigned offset : {0u, 2000u, static_cast<unsigned>(kTableShadowSlots)}) {
        const auto s = make_heap_switch(8100 + offset, r.program_base(), 5, 32, offset, false, false);
        auto start = r.context(8200 + offset);
        start.x[18] = r.program_base() + 4 * s.table;
        FX_CHECK(r.run(s.words, start, "explicit shadow +" + std::to_string(offset)));
        if (offset + 16 <= kTableShadowSlots) {
            FX_CHECK(r.last.shadow_misses >= 1u);
            FX_CHECK_EQ(r.last.lookups, 0u);
        }
    }

    // Wave O1: Wide-gap C++ switch (> 256 B between cases -> linkable direct exits)
    // where case 0 reads incoming NZCV from the bounds CMP via CSEL (tests raw_ok
    // in e_table_branch + normalize_entry_flags on the target region).
    using namespace a64;
    std::vector<std::uint32_t> w;
    w.push_back(movw(false, 2, 0, 16, 12));                         // 0  mov w12, #16
    const auto dispatch = w.size();
    w.push_back(add_imm(false, false, false, 11, 11, 1));           // 1  add w11, w11, #1
    w.push_back(logic_imm(false, 0, 0, 0, 1, 11, 11));              // 2  and w11, w11, #3
    w.push_back(add_imm(false, true, true, 31, 11, 3));             // 3  cmp w11, #3 (sets NZCV!)
    const auto b_hi_at = w.size();
    w.push_back(bcond(8, 0));                                       // 4  b.hi default
    const auto adr_tab_at = w.size();
    w.push_back(adr(false, 0, 8));                                  // 5  adr x8, table
    const auto adr_base_at = w.size();
    w.push_back(adr(false, 0, 9));                                  // 6  adr x9, base
    w.push_back(ldst_reg(2, 2, 11, 3, true, 8, 10));                // 7  ldrsw x10, [x8, x11, lsl #2]
    w.push_back(add_shift(true, false, false, 0, 10, 0, 9, 9));     // 8  add x9, x9, x10
    w.push_back(br(9));                                             // 9  br x9
    std::size_t case_at[4]{};
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < 4; ++k) {
        case_at[k] = w.size();
        // Read NZCV from CMP w11, #3 right at case entry!
        w.push_back(csel(true, false, 1, 0, 0, 0, 0));              // csel x0, x0, x1, eq (true iff w11==3)
        w.push_back(add_imm(true, false, false, 0, 0, 1 + k * 2));
        to_tail.push_back(w.size());
        w.push_back(b(0));
        // 80 NOPs (320 B > 256 B) between cases so Wave O1 keeps cases as linkable direct exits.
        w.insert(w.end(), 80, nop());
    }
    const auto def_at = w.size();
    w.push_back(add_imm(true, false, false, 0, 0, 77));
    const auto tail = w.size();
    w.push_back(add_imm(false, true, true, 12, 12, 1));
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    const auto tab_at = w.size();
    for (unsigned k = 0; k < 4; ++k) {
        const auto off = static_cast<std::int32_t>(4 * (static_cast<std::int64_t>(case_at[k]) - static_cast<std::int64_t>(adr_base_at)));
        w.push_back(static_cast<std::uint32_t>(off));
    }
    const auto end = w.size();
    w[b_hi_at] = bcond(8, static_cast<std::int32_t>(def_at) - static_cast<std::int32_t>(b_hi_at));
    w[adr_tab_at] = adr(false, static_cast<std::int32_t>(4 * (tab_at - adr_tab_at)), 8);
    for (const auto at : to_tail) w[at] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(at));
    w[to_end] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));
    FX_CHECK(r.run(w, r.context(8300), "wave o1 wide-gap switch raw nzcv"));
    if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);
}

// Wave T2a: Hoisted immutable table pointer guarded by `cmp xT, table_addr; jne generic`.
// Verifies 0 lookups on the fast path AND exact fallback when xT is switched to a
// different table dynamically at runtime.
FX_TEST(table_dispatch_hoisted_guarded) {
    auto& r = runner();
    using namespace a64;
    std::vector<std::uint32_t> w;
    w.push_back(movw(false, 2, 0, 24, 12));                         // 0  movz w12, #24
    const auto adr_tab1_at = w.size();
    w.push_back(adr(false, 0, 8));                                  // 1  adr x8, table1 (hoisted before loop!)
    const auto adr_tab2_at = w.size();
    w.push_back(adr(false, 0, 13));                                 // 2  adr x13, table2
    const auto dispatch = w.size();
    w.push_back(add_imm(false, false, false, 11, 11, 1));           // 3  add w11, w11, #1
    w.push_back(logic_imm(false, 0, 0, 0, 1, 11, 11));              // 4  and w11, w11, #3
    w.push_back(add_imm(false, true, true, 31, 11, 3));             // 5  cmp w11, #3
    const auto b_hi_at = w.size();
    w.push_back(bcond(8, 0));                                       // 6  b.hi default
    const auto adr_base_at = w.size();
    w.push_back(adr(false, 0, 9));                                  // 7  adr x9, base
    w.push_back(ldst_reg(2, 2, 11, 3, true, 8, 10));                // 8  ldrsw x10, [x8, x11, lsl #2]
    w.push_back(add_shift(true, false, false, 0, 10, 0, 9, 9));     // 9  add x9, x9, x10
    w.push_back(br(9));                                             // 10 br x9
    std::size_t case_at[4]{};
    std::vector<std::size_t> to_tail;
    for (unsigned k = 0; k < 4; ++k) {
        case_at[k] = w.size();
        w.push_back(add_imm(true, false, false, 0, 0, 10 + k * 7));
        to_tail.push_back(w.size());
        w.push_back(b(0));
    }
    const auto def_at = w.size();
    w.push_back(add_imm(true, false, false, 0, 0, 99));
    const auto tail = w.size();
    // If x15 != 0 and w12 == 12, swap x8 to table2 to exercise the table_reg guard fallback!
    w.push_back(add_imm(false, true, true, 31, 12, 12));            // cmp w12, #12
    w.push_back(csel(true, false, 8, 0, 0, 13, 14));                // csel x14, x13, x8, eq (rn=13, rm=8)
    w.push_back(add_imm(true, true, true, 31, 15, 0));              // cmp x15, #0
    w.push_back(csel(true, false, 8, 1, 0, 14, 8));                 // csel x8, x14, x8, ne (rn=14, rm=8)
    w.push_back(add_imm(false, true, true, 12, 12, 1));             // subs w12, w12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(dispatch) - static_cast<std::int32_t>(w.size())));
    const auto to_end = w.size();
    w.push_back(b(0));
    const auto tab1_at = w.size();
    for (unsigned k = 0; k < 4; ++k) {
        const auto off = static_cast<std::int32_t>(4 * (static_cast<std::int64_t>(case_at[k]) - static_cast<std::int64_t>(adr_base_at)));
        w.push_back(static_cast<std::uint32_t>(off));
    }
    const auto tab2_at = w.size();
    for (unsigned k = 0; k < 4; ++k) {
        // Reversed mapping in table2 so taking the wrong table immediately mismatches oracle!
        const auto off = static_cast<std::int32_t>(4 * (static_cast<std::int64_t>(case_at[3 - k]) - static_cast<std::int64_t>(adr_base_at)));
        w.push_back(static_cast<std::uint32_t>(off));
    }
    const auto end = w.size();
    w[b_hi_at] = bcond(8, static_cast<std::int32_t>(def_at) - static_cast<std::int32_t>(b_hi_at));
    w[adr_tab1_at] = adr(false, static_cast<std::int32_t>(4 * (tab1_at - adr_tab1_at)), 8);
    w[adr_tab2_at] = adr(false, static_cast<std::int32_t>(4 * (tab2_at - adr_tab2_at)), 13);
    for (const auto at : to_tail) w[at] = b(static_cast<std::int32_t>(tail) - static_cast<std::int32_t>(at));
    w[to_end] = b(static_cast<std::int32_t>(end) - static_cast<std::int32_t>(to_end));

    // 1. Fast path: x15 = 0 keeps x8 == table1 for all 24 iterations -> 0 lookups.
    auto ctx1 = r.context(0x72a1);
    ctx1.x[15] = 0;
    FX_CHECK(r.run(w, ctx1, "hoisted guarded fast path"));
    if (tables_enabled()) FX_CHECK_EQ(r.last.lookups, 0u);

    // 2. Dynamic table swap: x15 = 1 switches x8 to table2 mid-loop -> guard falls back accurately.
    auto ctx2 = r.context(0x72a2);
    ctx2.x[15] = 1;
    FX_CHECK(r.run(w, ctx2, "hoisted guarded fallback swap"));
    if (tables_enabled()) FX_CHECK(r.last.lookups >= 1u);
}
