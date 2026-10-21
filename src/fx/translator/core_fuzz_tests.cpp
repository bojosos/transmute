// Differential fuzzing of random programs: straight-line core instructions,
// forward conditional branches, counted loops, calls (BL/BLR + RET through
// the RSB), indirect jumps, literal loads and interpreted instructions.
// Translated execution is compared with the reference interpreter stepping
// word by word (owner: translator core).
#include "fx/testing/test.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"

#include <cstdio>
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

Program generate(std::uint64_t seed) {
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
    // Never land on a BR/BLR whose ADR was skipped.
    const auto is_indirect = [&](std::size_t t) {
        return t < w.size() && ((w[t] & 0xfffffc1fu) == 0xd61f0000u || (w[t] & 0xfffffc1fu) == 0xd63f0000u);
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

void fuzz(std::uint64_t first, std::uint64_t count, unsigned variant) {
    static DiffRunner r;
    const auto saved = r.machine.options;
    int failures = 0;
    for (std::uint64_t seed = first; seed < first + count && failures < 6; ++seed) {
        const auto program = generate(seed);
        auto ctx = r.context(seed ^ 0xabcdef);
        r.machine.options = saved;
        r.machine.link = true;
        switch (variant) {
        case 1: r.machine.link = false; break;
        case 2: r.machine.options.max_instructions = 1 + static_cast<unsigned>(seed % 6); break;
        case 3: r.machine.options.interrupt_checks = false; break;
        default: break;
        }
        if (!r.run(program.words, ctx, "fuzz seed " + std::to_string(seed))) {
            ++failures;
            std::fprintf(stderr, "    program: %s\n", hexdump(program.words).c_str());
        }
    }
    r.machine.options = saved;
    r.machine.link = true;
}

} // namespace

FX_TEST(core_fuzz_programs) { fuzz(1, 3000, 0); }
FX_TEST(core_fuzz_programs_unlinked) { fuzz(100000, 800, 1); }
FX_TEST(core_fuzz_programs_small_regions) { fuzz(200000, 800, 2); }
FX_TEST(core_fuzz_programs_no_interrupt_checks) { fuzz(300000, 400, 3); }
