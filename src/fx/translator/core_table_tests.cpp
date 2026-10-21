// Switch dispatch tests (owner: translator core). A BR through a recognised
// jump table (MSVC ARM64 lowering: adr; ldr{sw,sb,b,h} [xT, wI, uxtw]; [adr];
// add; br) dispatches inside the region (region_plan.cpp match_table,
// emit_branch.cpp e_table_branch). Checked here: differential execution of
// interpreter-like dispatch loops against the reference interpreter (every
// table form and bound form, out-of-range indices, cases outside the region),
// that the dispatch leaves no lookup exits, and that a table changed after
// translation still branches where the guest computes (the run-time target
// check).
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/region_plan.hpp"
#include "fx/translator/translator.hpp"

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
