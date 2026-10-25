// Differential fuzzing of random programs: straight-line core instructions,
// forward conditional branches, counted loops, calls (BL/BLR + RET through
// the RSB), indirect jumps, literal loads and interpreted instructions.
// Translated execution is compared with the reference interpreter stepping
// word by word (owner: translator core).
// ABI mode (core_fuzz_programs_abi_flags): TranslateOptions::abi_flags, so
// NZCV is dead at BL/BLR and RET (and, with the opt-in brdeadflags, at a BR
// that does not look intra-function). Its programs never read flags across
// those: every call continuation, the function's first instruction and every
// BR target write NZCV first, and half of the BRs sit in a block of their
// own (adr; b next; next: br), which brdeadflags makes flag-dead. Odd seeds turn call inlining off, so BL and RET
// are region exits (an inlined call keeps NZCV in the region).
// Deeper runs (integration gates): TRANSMUTE_FX_FUZZ_SCALE=<n> runs n times as
// many programs per test, TRANSMUTE_FX_FUZZ_SEED=<s> starts at other seeds.
#include "fx/testing/test.hpp"
#include "fx/translator/core_isel.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/region_plan.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::coretest;

namespace {

struct Patch {
    std::size_t at;
    std::size_t target;   // instruction index
    int kind;             // 0 b.cond, 1 cbz/cbnz, 2 tbz/tbnz, 3 b, 4 bl, 5 adr
};

void apply(std::vector<std::uint32_t>& w, const Patch& p) {
    const auto off = static_cast<std::int32_t>(p.target) - static_cast<std::int32_t>(p.at);
    auto& word = w[p.at];
    switch (p.kind) {
    case 0:
    case 1: word |= (static_cast<std::uint32_t>(off) & 0x7ffff) << 5; break;
    case 2: word |= (static_cast<std::uint32_t>(off) & 0x3fff) << 5; break;
    case 3:
    case 4: word |= static_cast<std::uint32_t>(off) & 0x3ffffff; break;
    case 5: {
        const auto bytes = static_cast<std::uint32_t>(off * 4) & 0x1fffff;
        word |= ((bytes & 3) << 29) | (((bytes >> 2) & 0x7ffff) << 5);
        break;
    }
    }
}

std::uint32_t forward_branch(Rng& rng, int& kind) {
    switch (rng.below(5)) {
    case 0:
    case 1: kind = 0; return a64::bcond(rng.below(16), 0);
    case 2: kind = 1; return a64::cbz(rng.chance(50), rng.chance(50), 0, rng.pick(kSrc));
    case 3: kind = 2; return a64::tbz(rng.chance(50), rng.below(64), 0, rng.pick(kSrc));
    default: kind = 3; return a64::b(0);
    }
}

struct Program {
    std::vector<std::uint32_t> words;
};

Program generate(std::uint64_t seed, bool abi = false) {
    Rng rng(seed);
    InstGen g(rng);
    Program p;
    auto& w = p.words;
    std::vector<Patch> patches;
    std::vector<std::size_t> to_end;      // branches to the end of main
    std::vector<std::size_t> to_func;     // BL / ADR of the function
    const unsigned main_len = 5 + rng.below(36);
    const bool has_func = rng.chance(40);
    bool loop_used = false;
    std::size_t loop_begin = 0, loop_end = 0;   // [mov w12 .. closing branch]

    std::vector<std::size_t> protect;     // ABI mode: the `b next` between an ADR and its BR
    const auto flag_writer = [&] { return a64::add_shift(true, true, true, 0, g.src(), 0, g.src(), 31); };   // cmp
    const auto straight = [&](unsigned n) {
        for (unsigned i = 0; i < n; ++i) w.push_back(g.data_or_memory());
    };
    while (w.size() < main_len) {
        const auto choice = rng.below(100);
        if (choice < 66) {
            w.push_back(g.data_or_memory());
        } else if (choice < 76) {   // forward conditional / unconditional branch
            int kind;
            w.push_back(forward_branch(rng, kind));
            patches.push_back({w.size() - 1, w.size() + rng.below(5), kind});
        } else if (choice < 81 && !loop_used) {
            loop_used = true;
            loop_begin = w.size();
            w.push_back(a64::movw(false, 2, 0, 1 + rng.below(5), 12));   // mov w12, #n
            const auto start = w.size();
            const auto body = 1 + rng.below(8);
            for (unsigned i = 0; i < body; ++i) {
                if (rng.chance(15) && i + 1 < body) {
                    int kind;
                    w.push_back(forward_branch(rng, kind));
                    // stay inside the body
                    const auto remaining = body - i - 1;
                    patches.push_back({w.size() - 1, w.size() + rng.below(remaining), kind});
                } else {
                    w.push_back(g.data_or_memory());
                }
            }
            switch (rng.below(3)) {
            case 0:
                w.push_back(a64::add_imm(true, true, true, 12, 12, 1));   // subs x12, x12, #1
                w.push_back(a64::bcond(1, 0));
                patches.push_back({w.size() - 1, start, 0});
                break;
            case 1:
                w.push_back(a64::add_imm(true, true, false, 12, 12, 1));   // sub x12, x12, #1
                w.push_back(a64::cbz(true, true, 0, 12));
                patches.push_back({w.size() - 1, start, 1});
                break;
            default:
                w.push_back(a64::add_imm(false, true, false, 12, 12, 1));   // sub w12, w12, #1
                w.push_back(a64::add_imm(false, true, true, 31, 12, 0));    // cmp w12, #0
                w.push_back(a64::bcond(12, 0));                             // b.gt
                patches.push_back({w.size() - 1, start, 0});
                break;
            }
            loop_end = w.size() - 1;
        } else if (choice < 86 && has_func) {
            if (rng.chance(60)) {
                w.push_back(a64::bl(0));
                to_func.push_back(w.size() - 1);
            } else {
                w.push_back(a64::adr(false, 0, 9));
                to_func.push_back(w.size() - 1);
                w.push_back(a64::blr(9));
            }
            if (abi) w.push_back(flag_writer());   // NZCV is undefined after the call returns
        } else if (choice < 90) {   // interpreted instructions
            switch (rng.below(3)) {
            case 0: w.push_back(a64::mrs_nzcv(rng.pick(kDest))); break;
            case 1: w.push_back(a64::msr_nzcv(rng.pick(kSrc))); break;
            default: w.push_back(a64::neon_add2d(rng.below(32), rng.below(32), rng.below(32))); break;
            }
        } else if (choice < 93) {   // literal loads from the pool below the program
            static constexpr unsigned opcs[] = {0, 1, 2, 3};
            const auto off = -static_cast<std::int32_t>(1 + rng.below(1000));
            w.push_back(a64::ldr_literal(rng.pick(opcs), off, rng.pick(kDest)));
        } else if (choice < 95 && abi) {   // forward indirect jump to a flags writer
            w.push_back(a64::adr(false, 0, 10));
            const auto adr_at = w.size() - 1;
            if (rng.chance(50)) {   // the BR in a block of its own (flag-dead with brdeadflags)
                w.push_back(a64::b(1));
                protect.push_back(w.size() - 1);
            }
            w.push_back(a64::br(10));
            patches.push_back({adr_at, w.size(), 5});
            w.push_back(flag_writer());
        } else if (choice < 95) {   // forward indirect jump
            w.push_back(a64::adr(false, 0, 10));
            patches.push_back({w.size() - 1, w.size() + 1 + rng.below(4), 5});
            w.push_back(a64::br(10));
        } else {
            straight(1);
        }
    }
    const auto main_end = w.size();
    for (auto& patch : patches) {
        if (patch.target > main_end) patch.target = main_end;
        // Never enter a loop body from outside (it would skip the counter set-up).
        const bool from_inside = patch.at > loop_begin && patch.at <= loop_end;
        if (loop_used && !from_inside && patch.target > loop_begin && patch.target <= loop_end)
            patch.target = loop_end + 1;
    }
    if (has_func) {
        w.push_back(a64::b(0));
        to_end.push_back(w.size() - 1);
        const auto func = w.size();
        if (abi) w.push_back(flag_writer());   // NZCV is undefined on entry
        straight(rng.below(10));
        if (rng.chance(30)) {   // a conditional early return
            w.push_back(a64::cbz(true, false, 2, rng.pick(kSrc)));
            straight(1 + rng.below(3));
        }
        w.push_back(a64::ret());
        for (const auto at : to_func) {
            const bool is_bl = (w[at] & 0xfc000000u) == 0x94000000u;
            patches.push_back({at, func, is_bl ? 4 : 5});
        }
    }
    for (const auto at : to_end) patches.push_back({at, w.size(), 3});
    // Never land on a BR/BLR (or the `b next` before it) whose ADR was skipped.
    const auto is_indirect = [&](std::size_t t) {
        return t < w.size() && ((w[t] & 0xfffffc1fu) == 0xd61f0000u || (w[t] & 0xfffffc1fu) == 0xd63f0000u ||
                                std::find(protect.begin(), protect.end(), t) != protect.end());
    };
    for (auto& patch : patches)
        while (patch.kind != 4 && is_indirect(patch.target)) ++patch.target;
    for (const auto& patch : patches) apply(w, patch);
    return p;
}

std::string hexdump(const std::vector<std::uint32_t>& w) {
    std::string s;
    char buf[16];
    for (auto x : w) {
        std::snprintf(buf, sizeof buf, "%08x ", x);
        s += buf;
    }
    return s;
}

std::uint64_t env_number(const char* name, std::uint64_t fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::strtoull(value, nullptr, 10) : fallback;
}

// Call inlining off while alive (BL/BLR and RET become region exits).
struct NoInline {
    core::PlanOverrides o{};
    NoInline() {
        o = core::default_plan_overrides();
        o.no_inline = true;
        core::set_overrides_for_testing(&o);
    }
    ~NoInline() { core::set_overrides_for_testing(nullptr); }
    NoInline(const NoInline&) = delete;
    NoInline& operator=(const NoInline&) = delete;
};

void fuzz(std::uint64_t first, std::uint64_t count, unsigned variant) {
    count *= std::max<std::uint64_t>(1, env_number("TRANSMUTE_FX_FUZZ_SCALE", 1));
    first += env_number("TRANSMUTE_FX_FUZZ_SEED", 0) * 10'000'000;
    static DiffRunner r;
    const auto saved = r.machine.options;
    int failures = 0;
    for (std::uint64_t seed = first; seed < first + count && failures < 6; ++seed) {
        const auto program = generate(seed, variant == 4);
        auto ctx = r.context(seed ^ 0xabcdef);
        r.machine.options = saved;
        r.machine.link = true;
        switch (variant) {
        case 1: r.machine.link = false; break;
        case 2: r.machine.options.max_instructions = 1 + static_cast<unsigned>(seed % 6); break;
        case 3: r.machine.options.interrupt_checks = false; break;
        case 4: r.machine.options.abi_flags = true; break;
        default: break;
        }
        std::optional<NoInline> no_inline;
        if (variant == 4 && (seed & 1)) no_inline.emplace();
        bool ok = false;
        try {
            ok = r.run(program.words, ctx, "fuzz seed " + std::to_string(seed));
        } catch (const std::exception& e) {   // a translator exception names its seed and program too
            std::fprintf(stderr, "    fuzz seed %llu (variant %u): exception: %s\n", static_cast<unsigned long long>(seed),
                         variant, e.what());
        }
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "    program: %s\n", hexdump(program.words).c_str());
        }
    }
    r.machine.options = saved;
    r.machine.link = true;
    FX_CHECK_EQ(failures, 0);   // mismatches also fail inside run(); exceptions only here
}

} // namespace

FX_TEST(core_fuzz_programs) { fuzz(1, 3000, 0); }
FX_TEST(core_fuzz_programs_unlinked) { fuzz(100000, 800, 1); }
FX_TEST(core_fuzz_programs_small_regions) { fuzz(200000, 800, 2); }
FX_TEST(core_fuzz_programs_no_interrupt_checks) { fuzz(300000, 400, 3); }

// Hand-written arm64 keeps NZCV live across a BR: dav1d's NEON (refmvs.S
// splat_mv_neon; ffmpeg and libvpx do the same) computes a jump-table target
// once at function entry, then loops `subs ...; br xT` and the target reads
// the flags (`b.gt loop`). The BR's block has no ADR/ADRP, so a heuristic that
// treats such a BR as flag-dead breaks it. Runs with the production flags
// model (abi_flags), unlike the fuzz above. With TRANSMUTE_FX_CORE=brdeadflags
// (opt-in, unsound for this code) it diverges, which shows the test sees it.
// The loop is closed by CBNZ, not by the flags, so the diverging run ends too.
FX_TEST(core_flags_live_across_br_to_a_precomputed_target) {
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        adr(false, 24, 3),                               // 0 adr x3, target (6): computed once, outside the loop
        movw(false, 2, 0, 5, 4),                         // 1 movz w4, #5
        add_shift(true, false, false, 0, 2, 0, 1, 1),    // 2 loop: add x1, x1, x2
        add_imm(false, true, true, 4, 4, 1),             // 3 subs w4, w4, #1
        br(3),                                           // 4 br x3 (flags live: the target reads them)
        brk(0x1),                                        // 5 (not reached)
        bcond(13, 2),                                    // 6 target: b.le 8
        add_imm(true, false, false, 5, 5, 1),            // 7 add x5, x5, #1
        cbz(false, true, -6, 4),                         // 8 cbnz w4, loop
    };
    static DiffRunner r;
    const auto saved = r.machine.options;
    r.machine.options.abi_flags = true;   // the production model
    for (const bool dead_knob : {false, true}) {
        core::set_p2_knobs_for_testing(dead_knob ? core::knob_br_flags_dead : 0u);
        r.report = !dead_knob;   // the knob run is expected to diverge
        auto ctx = r.context(7);
        ctx.x[1] = 0; ctx.x[2] = 3; ctx.x[5] = 0;
        const bool same = r.run(prog, ctx, dead_knob ? "brdeadflags" : "flags live across br");
        if (!dead_knob) {
            FX_CHECK(same);
            FX_CHECK_EQ(r.actual.x[5], std::uint64_t{4});   // gt on the first four iterations, le on the last
        } else {
            FX_CHECK(!same);   // the unsound knob is detected
        }
    }
    r.report = true;
    core::set_p2_knobs_for_testing(~0u);
    r.machine.options = saved;
}

FX_TEST(core_fuzz_programs_abi_flags) { fuzz(400000, 1500, 4); }

// ABI mode, targeted: flags written before a call, a register-target BR and
// a return are rewritten before any read (they are dead there), and a BR
// whose block builds its target with ADR (a computed goto, intra-function)
// keeps NZCV live: its target reads the flags the code before it set.
FX_TEST(core_abi_flags_targeted) {
    using namespace a64;
    static DiffRunner r;
    const auto saved = r.machine.options;
    r.machine.options.abi_flags = true;
    const auto cmp12 = add_shift(true, true, true, 0, 2, 0, 1, 31);   // cmp x1, x2
    const auto cmp34 = add_shift(true, true, true, 0, 4, 0, 3, 31);   // cmp x3, x4
    const auto csel = a64::csel(true, false, 6, 0, 0, 5, 7);          // csel x7, x5, x6, eq
    const std::vector<std::vector<std::uint32_t>> progs = {
        // cmp; bl f; cmp; csel; b end; f: cmp; csel; ret
        {cmp12, bl(4), cmp34, csel, b(4), cmp34, csel, ret()},
        // cmp; adr x10, T; b L; L: br x10; T: cmp; csel (flag-dead with brdeadflags)
        {cmp12, adr(false, 12, 10), b(1), br(10), cmp34, csel},
        // cmp; adr x10, T; br x10; T: csel (computed goto: NZCV stays live across it)
        {cmp12, adr(false, 8, 10), br(10), csel},
        // cmp; b.eq L; adr x10, T; br x10; L: nop; T: csel
        {cmp12, bcond(0, 3), adr(false, 12, 10), br(10), nop(), csel},
    };
    for (const bool inlining : {true, false}) {
        std::optional<NoInline> no_inline;
        if (!inlining) no_inline.emplace();
        for (std::size_t k = 0; k < progs.size(); ++k)
            for (unsigned c = 0; c < 6; ++c)
                r.run(progs[k], r.context(4100 + 16 * k + c), "abi targeted " + std::to_string(k) + (inlining ? "" : " noinline"));
    }
    // Not vacuous: flags read across a call that is a region exit (cmp; bl f;
    // mrs x7, nzcv; b end; f: ret) are the dropped ones, so some contexts
    // differ from the oracle.
    NoInline no_inline;
    r.report = false;
    unsigned differ = 0;
    for (unsigned c = 0; c < 16; ++c) differ += r.run({cmp12, bl(3), mrs_nzcv(7), b(2), ret()}, r.context(4200 + c), "abi stale") ? 0 : 1;
    r.report = true;
    FX_CHECK(differ > 0);
    r.machine.options = saved;
}

// Regression (dav1d refmvs.S splat_mv_neon under checkasm): hand-written NEON
// computes a jump-table target once at function entry and loops
// `ldr; subs; add; br x3`, the targets ending in `b.gt 1b`. The BR's block
// has no ADR (that is in the entry block, with the table offset added: an
// unknown target), and its target reads the flags the
// SUBS before the BR set: NZCV must stay live across this BR in ABI mode.
FX_TEST(core_abi_flags_br_loop_reads_flags) {
    using namespace a64;
    static DiffRunner r;
    const auto saved = r.machine.options;
    r.machine.options.abi_flags = true;
    const std::vector<std::uint32_t> prog = {
        movw(false, 2, 0, 5, 12),                  // mov w12, #5
        adr(false, 28, 3),                         // adr x3, T
        bitfield(true, 2, 63, 63, 14, 13),         // lsr x13, x14, #63 (0: x14 is small; a table offset)
        add_shift(true, false, false, 0, 13, 0, 3, 3),  // add x3, x3, x13
        ldst_imm9(3, 1, 8, 1, 16, 1),              // L: ldr x1, [x16], #8
        add_imm(false, true, true, 12, 12, 1),     // subs w12, w12, #1
        add_shift(true, false, false, 0, 2, 0, 1, 1),   // add x1, x1, x2
        br(3),                                     // br x3
        add_shift(true, false, false, 0, 1, 0, 5, 5),   // T: add x5, x5, x1
        bcond(12, -5),                             // b.gt L
    };
    for (const bool inlining : {true, false}) {
        std::optional<NoInline> no_inline;
        if (!inlining) no_inline.emplace();
        for (unsigned c = 0; c < 4; ++c) r.run(prog, r.context(4300 + c), "dav1d br loop");
    }
    r.machine.options = saved;
}
