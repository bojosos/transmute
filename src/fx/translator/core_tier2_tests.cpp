// Tier 2 (the optimizing tier, region_alloc.cpp): differential tests of
// random loop nests in optimized and plain translations against the reference
// interpreter, the guest state at faults inside tier-2 code, tier-up from hot
// loops, and the dropped interrupt checks (owner: translator core). Built
// with the real families (vector instructions are translated).
#include "fx/testing/test.hpp"
#include "fx/testing/harness.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/region_plan.hpp"
#include "fx/translator/translator.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::coretest;

namespace {

void register_families() {
    static bool done = false;
    if (done) return;
    done = true;
    register_fp_emitters();
    register_neon_emitters();
    register_system_emitters();
}

// Integer vector operations only: FP results would differ from the
// interpreter in NaN encodings (docs/FX_ENGINE.md), random inputs make NaNs.
constexpr std::uint32_t mov16b(unsigned d, unsigned n) { return 0x4ea01c00u | (n << 16) | (n << 5) | d; }
constexpr std::uint32_t add4s(unsigned d, unsigned n, unsigned m) { return 0x4ea08400u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t sub4s(unsigned d, unsigned n, unsigned m) { return 0x6ea08400u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t eor16b(unsigned d, unsigned n, unsigned m) { return 0x6e201c00u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t add_d(unsigned d, unsigned n, unsigned m) { return 0x5ee08400u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t dup4s(unsigned d, unsigned rn) { return 0x4e040c00u | (rn << 5) | d; }
constexpr std::uint32_t umov_d(unsigned rd, unsigned vn) { return 0x4e083c00u | (vn << 5) | rd; }
constexpr std::uint32_t ldr_q(unsigned rt, unsigned rn, unsigned offset) { return 0x3dc00000u | ((offset / 16) << 10) | (rn << 5) | rt; }
constexpr std::uint32_t str_q(unsigned rt, unsigned rn, unsigned offset) { return 0x3d800000u | ((offset / 16) << 10) | (rn << 5) | rt; }
// tbl vd.16b, {vn.16b..vn+len.16b}, vm.16b
constexpr std::uint32_t tbl(unsigned d, unsigned n, unsigned len, unsigned m) {
    return 0x4e000000u | (m << 16) | (len << 13) | (n << 5) | d;
}
// mov xd, xn (orr xd, xzr, xn)
constexpr std::uint32_t mov_x(unsigned d, unsigned n) { return a64::logic_shift(true, 1, 0, false, n, 0, 31, d); }
// ldr xt, [x9], #8 (post-index: the faulting access of the fault tests)
constexpr std::uint32_t ldr_post9(unsigned rt) { return a64::ldst_imm9(3, 1, 8, 1, 9, rt); }

// Copies, rotations (the noise_fbm pattern), loop-carried values and memory
// over a few registers, so that pins, the cache, aliases and lending all
// meet; x9, x12 and x18 are the generator's (fault pointer, loop counters).
class Tier2Gen {
public:
    explicit Tier2Gen(Rng& rng) : r_(rng), g_(rng) {}
    unsigned xreg() {   // data registers (never x9, x12, x14 (an index), x16, x17, x18, x30, SP)
        static constexpr unsigned regs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13, 15, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
        static constexpr unsigned low[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13, 15};
        static constexpr unsigned high[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};
        return phase_ == 0 ? r_.pick(low) : phase_ == 1 ? r_.pick(high) : r_.pick(regs);
    }
    // -1: any register; 0 / 1: only low / high data registers and no other
    // core instructions (the other half, SP and x30 are then unused there:
    // a loop of one phase lends the pins of the other).
    int phase_ = -1;
    bool tbl_ok_ = false;   // v30/v31 hold TBL indices
    // v30 and v31 are TBL index registers, written only before the loops
    // (tier 2 then precomputes TBL's adjusted indices: EmitContext::vec_derived).
    unsigned vreg() {
        const unsigned v = wide_ ? r_.below(32) : base_ + r_.below(6);
        return v >= 30 ? v - 4 : v;
    }
    void body(std::vector<std::uint32_t>& w, unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            const auto c = r_.below(100);
            if (c < 14) {
                w.push_back(mov_x(xreg(), xreg()));
            } else if (c < 22) {
                w.push_back(mov16b(vreg(), vreg()));
            } else if (c < 26) {   // rotation and back (the copy back leaves one copy)
                const unsigned a = vreg(), b = (a + 1) & 31u, t = vreg();
                w.push_back(mov16b(t, b));
                w.push_back(mov16b(b, a));
                w.push_back(add4s(a, vreg(), vreg()));
                w.push_back(mov16b(a, b));
                w.push_back(mov16b(b, t));
            } else if (c < 34) {
                w.push_back(add4s(vreg(), vreg(), vreg()));
            } else if (c < 38) {
                w.push_back(eor16b(vreg(), vreg(), vreg()));
            } else if (c < 41) {
                w.push_back(sub4s(vreg(), vreg(), vreg()));
            } else if (c < 44) {
                w.push_back(add_d(vreg(), vreg(), vreg()));
            } else if (c < 46 && tbl_ok_) {
                const unsigned len = r_.below(4);
                w.push_back(tbl(vreg(), r_.below(29 - len), len, 30 + r_.below(2)));
            } else if (c < 46) {
                w.push_back(dup4s(vreg(), xreg()));
            } else if (c < 48) {
                w.push_back(umov_d(xreg(), vreg()));
            } else if (c < 52) {
                w.push_back(ldr_q(vreg(), 16, 16 * r_.below(16)));
            } else if (c < 55) {
                w.push_back(str_q(vreg(), 17, 16 * r_.below(16)));
            } else if (c < 60) {   // a loop-carried accumulator
                const auto a = xreg();
                w.push_back(a64::add_shift(true, false, false, 0, xreg(), 0, a, a));
            } else if (phase_ >= 0) {
                w.push_back(a64::add_shift(true, r_.chance(50), false, 0, xreg(), r_.below(4), xreg(), xreg()));
            } else {
                auto word = g_.data_or_memory();
                core::Inst d;   // keep the generator's registers (x9, x12, x18) intact
                core::decode(word, 0x1000, d);
                if (core::usage(d).write & ((1u << 9) | (1u << 12) | (1u << 18))) word = a64::nop();
                w.push_back(word);
            }
        }
    }
    void reset_regs() {
        wide_ = r_.chance(40);
        base_ = r_.below(26);
    }

private:
    Rng& r_;
    InstGen g_;
    bool wide_{};
    unsigned base_{};
};

struct Program {
    std::vector<std::uint32_t> words;
    std::size_t fault_word = ~std::size_t{0};   // the ldr [x9], #8 (fault programs)
};

// Two nested loops (counters x12, x18), sometimes a header falling into
// another header (Embree's traversal shape: one interrupt check covers both),
// forward branches inside the bodies and an early exit out of the inner loop.
Program generate(std::uint64_t seed, bool with_fault, unsigned outer_count = 0) {
    Rng rng(seed);
    Tier2Gen gen(rng);
    gen.reset_regs();
    Program p;
    auto& w = p.words;
    gen.body(w, rng.below(4));
    if (rng.chance(50)) {   // TBL index registers, defined once before the loops
        w.push_back(ldr_q(30, 16, 16 * rng.below(16)));
        w.push_back(ldr_q(31, 17, 16 * rng.below(16)));
        gen.tbl_ok_ = true;
    }
    w.push_back(a64::movw(false, 2, 0, outer_count ? outer_count : 1 + rng.below(3), 12));   // mov w12, #n
    const auto outer = w.size();
    gen.body(w, rng.below(3));
    w.push_back(a64::movw(false, 2, 0, 1 + rng.below(4), 18));        // mov w18, #m
    const bool shared_header = rng.chance(50);                         // inner header right after the outer one
    const bool phased = rng.chance(50);                                // inner and outer code use disjoint registers
    if (phased) gen.phase_ = 1;
    if (!shared_header) gen.body(w, 1 + rng.below(3));
    const auto inner = w.size();
    if (phased) gen.phase_ = 0;
    gen.body(w, 1 + rng.below(6) + (phased ? 8u : 0u));   // phased: enough registers for pressure
    if (with_fault) {
        p.fault_word = w.size();
        w.push_back(ldr_post9(gen.xreg()));
    }
    std::size_t early = ~std::size_t{0};
    if (rng.chance(30)) {   // early exit out of the inner loop (patched below)
        early = w.size();
        w.push_back(a64::cbz(true, false, 0, gen.xreg()));
    }
    gen.body(w, rng.below(5) + (phased ? 4u : 0u));
    if (phased) gen.phase_ = 1;
    w.push_back(a64::add_imm(true, true, true, 18, 18, 1));           // subs x18, x18, #1
    w.push_back(a64::bcond(1, static_cast<std::int32_t>(inner) - static_cast<std::int32_t>(w.size())));
    const auto after_inner = w.size();
    if (phased) {   // a sibling loop over the other registers: each lends the other's pins
        w.push_back(a64::movw(false, 2, 0, 1 + rng.below(4), 18));    // mov w18, #m
        const auto second = w.size();
        gen.body(w, 8 + rng.below(6));
        w.push_back(a64::add_imm(true, true, true, 18, 18, 1));       // subs x18, x18, #1
        w.push_back(a64::bcond(1, static_cast<std::int32_t>(second) - static_cast<std::int32_t>(w.size())));
    }
    gen.body(w, rng.below(4));
    w.push_back(a64::add_imm(true, true, true, 12, 12, 1));           // subs x12, x12, #1
    w.push_back(a64::bcond(1, static_cast<std::int32_t>(outer) - static_cast<std::int32_t>(w.size())));
    gen.phase_ = -1;
    gen.tbl_ok_ = false;
    gen.body(w, rng.below(3));
    if (early != ~std::size_t{0})
        w[early] |= (static_cast<std::uint32_t>(after_inner - early) & 0x7ffffu) << 5;
    // Forward conditional branches of the bodies (none generated) stay as is.
    return p;
}

std::string hex(const std::vector<std::uint32_t>& words) {
    std::string s;
    char buf[16];
    for (const auto x : words) {
        std::snprintf(buf, sizeof buf, " %08x", x);
        s += buf;
    }
    return s;
}

struct Config {
    const char* name;
    int optimize;
    bool lend;
    bool no_alloc;
    bool no_galias;
};
constexpr Config kConfigs[] = {
    {"tier2", 1, false, false, false},
    {"tier2+lend", 1, true, false, false},
    {"tier2 one pass, no gpr aliases", 1, false, true, true},
    {"plain", 0, false, false, false},
};

void apply(const Config& c, core::PlanOverrides& o) {
    o = core::default_plan_overrides();
    o.t2_lend = c.lend;
    o.t2_no_alloc = c.no_alloc;
    o.t2_no_galias = c.no_galias;
    core::set_overrides_for_testing(&o);
}

// ---- state at a fault --------------------------------------------------------------------
struct FaultCapture {
    const TranslatedRegion* region{};
    std::uintptr_t host{};
    const void* resume{};
    bool hit{};
    std::uint64_t guest_pc{};
    FlagsLocation flags{};
    std::uint64_t x[32]{}, v[32][2]{};
    std::uint32_t in_host{}, vin_host{};
} g_capture;

LONG CALLBACK capture_fault(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_capture.region) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    if (rip < g_capture.host || rip >= g_capture.host + g_capture.region->code.size()) return EXCEPTION_CONTINUE_SEARCH;
    const auto off = static_cast<std::uint32_t>(rip - g_capture.host);
    const DWORD64* regs = &info->ContextRecord->Rax;
    const M128A* xmm = &info->ContextRecord->Xmm0;
    for (const auto& f : g_capture.region->faults) {
        if (off < f.host_begin || off >= f.host_end) continue;
        g_capture.hit = true;
        g_capture.guest_pc = f.guest_pc;
        g_capture.flags = f.flags;
        for_each_fault_register(g_capture.region->pins, f, g_capture.region->fault_maps, g_capture.region->fault_map_regs,
                                [&](unsigned guest, unsigned host, bool vector) {
                                    if (vector) {
                                        g_capture.v[guest & 31][0] = xmm[host & 15].Low;
                                        g_capture.v[guest & 31][1] = static_cast<std::uint64_t>(xmm[host & 15].High);
                                        g_capture.vin_host |= 1u << (guest & 31);
                                    } else {
                                        g_capture.x[guest & 31] = regs[host & 15];
                                        g_capture.in_host |= 1u << (guest & 31);
                                    }
                                });
    }
    info->ContextRecord->Rip = reinterpret_cast<DWORD64>(g_capture.resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

// Random loop nests: every configuration against the interpreter (full guest
// state and memory), no interpreted instructions.
FX_TEST(core_tier2_loop_nests_differential) {
    register_families();
    static DiffRunner r;
    core::PlanOverrides o;
    unsigned failures = 0;
    for (std::uint64_t seed = 1; seed <= 1500 && failures < 6; ++seed) {
        const auto p = generate(seed, false);
        for (const auto& c : kConfigs) {
            apply(c, o);
            r.machine.options.optimize = static_cast<std::int8_t>(c.optimize);
            auto ctx = r.context(seed * 7 + 1);
            for (unsigned g = 0; g < 32; ++g)
                if (g != 16 && g != 17 && g != 31 && g != 30 && g != 9 && g != 12 && g != 18) ctx.x[g] &= 0xffff;
            if (!r.run(p.words, ctx, std::string("tier2 loop nest seed ") + std::to_string(seed) + " (" + c.name + ")" + hex(p.words))) {
                ++failures;
                break;
            }
            if (r.last.interprets != 0) {
                testing::report_failure(__FILE__, __LINE__, "interpreted instructions: seed " + std::to_string(seed));
                ++failures;
                break;
            }
        }
    }
    core::set_overrides_for_testing(nullptr);
    r.machine.options.optimize = -1;
}

// CPython's Unicode dictionary iterator scans empty entries before returning
// a key. The pointer and index remain live across both exits of the scan loop.
FX_TEST(core_tier2_dictionary_scan_differential) {
    register_families();
    DiffRunner r;
    r.machine.options.optimize = 1;
    using namespace a64;
    std::vector<std::uint32_t> prog = {
        movw(true, 2, 0, 0, 9),
        ldst_uimm(0, 0, 9, 11, 9),                      // keys->log2_index_bytes = 0
        movw(true, 2, 0, 1, 9),
        ldst_uimm(0, 0, 10, 11, 9),                     // Unicode entries
        movw(true, 2, 0, 12, 9),
        ldst_uimm(3, 0, 3, 11, 9),                      // keys->nentries = 12
    };
    for (unsigned i = 0; i < 12; ++i) {
        prog.push_back(ldst_imm9(3, 0, 33 + 16 * i, 0, 11, 16));
        prog.push_back(movw(true, 2, 0, i == 10 ? 1 : 0, 9));
        prog.push_back(ldst_imm9(3, 0, 41 + 16 * i, 0, 11, 9));
    }
    const std::vector<std::uint32_t> scan = {
        0xf9400d6a,  // ldr x10, [x11, #24]
        0x39402569,  // ldrb w9, [x11, #9]
        0x3940296c,  // ldrb w12, [x11, #10]
        0xeb0a011f,  // cmp x8, x10
        0x340001ac,  // cbz w12, empty
        0xd280002c,  // mov x12, #1
        0x9ac9218c,  // lsl x12, x12, x9
        0x91000909,  // add x9, x8, #2
        0x8b091189,  // add x9, x12, x9, lsl #4
        0x8b0b0129,  // add x9, x9, x11
        0x540000ea,  // b.ge empty
        0xf940052b,  // loop: ldr x11, [x9, #8]
        0xb50000eb,  // cbnz x11, found
        0x91000508,  // add x8, x8, #1
        0xeb0a011f,  // cmp x8, x10
        0x91004129,  // add x9, x9, #16
        0x54ffff6b,  // b.lt loop
        0xd2800000,  // empty: mov x0, #0
        0x14000002,  // b end
        0xf9400120,  // found: ldr x0, [x9]
    };
    prog.insert(prog.end(), scan.begin(), scan.end());
    for (unsigned start : {0u, 9u, 10u, 11u, 12u}) {
        auto ctx = r.context(42);
        ctx.x[11] = r.data_base() + 8192;
        ctx.x[8] = start;
        FX_REQUIRE(r.run(prog, ctx, "dictionary scan from " + std::to_string(start)));
    }
}

FX_TEST(core_tier2_branch_operand_remains_dirty_until_tested) {
    DiffRunner r;
    r.machine.options.optimize = 1;
    core::PlanOverrides o = core::default_plan_overrides();
    o.no_pins = true;
    o.no_extra_pins = true;
    core::set_overrides_for_testing(&o);
    using namespace a64;
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (unsigned g = 0; g < 30; ++g) {
            if (g == 16 || g == 17) continue;
            for (unsigned value : {0u, 1u}) {
                std::vector<std::uint32_t> prog;
                // Fill the six earlier scratch hosts so the tested value lands
                // in RAX, which materializing live NZCV evicts before the test.
                for (unsigned other = 0, filled = 0; filled < 6; ++other) {
                    if (other == g) continue;
                    prog.push_back(movw(true, 2, 0, 100 + other, other));
                    ++filled;
                }
                const std::vector<std::uint32_t> tail = {
                    movw(true, 2, 0, value, g),
                    add_shift(true, true, true, 0, 31, 0, 31, 31),  // cmp xzr, xzr
                    kind ? tbz(false, 0, 4, g) : cbz(true, false, 4, g),
                    movw(true, 2, 0, 42, g),
                    movw(true, 2, 0, 1, 16),
                    b(3),
                    movw(true, 2, 0, 42, g),
                    movw(true, 2, 0, 2, 16),
                    bcond(0, 2),
                    add_imm(true, false, false, 16, 16, 10),
                };
                prog.insert(prog.end(), tail.begin(), tail.end());
                auto ctx = r.context(5);
                ctx.x[g] = !value;
                FX_CHECK(r.run(prog, ctx, "dead branch operand " + std::to_string(g)));
            }
        }
    }
    core::set_overrides_for_testing(nullptr);
}

// A load faults in the k-th iteration of the inner loop of tier-2 code: the
// state rebuilt from pins, fault maps (dirty cache entries, copy aliases,
// lent pins) and the context equals the interpreter's before that load.
FX_TEST(core_tier2_fault_state_differential) {
    register_families();
    static DiffRunner r;
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FX_REQUIRE(pages);
    for (unsigned k = 0; k < 4096; ++k) pages[k] = static_cast<std::uint8_t>(k * 29 + 3);
    DWORD old = 0;
    VirtualProtect(pages + 4096, 4096, PAGE_NOACCESS, &old);
    core::PlanOverrides o;
    unsigned failures = 0, faults = 0, lent_sites = 0;
    for (std::uint64_t seed = 1; seed <= 600 && failures < 6; ++seed) {
        const auto p = generate(seed + 50000, true);
        const auto fault_pc = r.program_base() + 4 * p.fault_word;
        // The interpreter's state when the load is about to touch the page.
        Rng rng(seed);
        const unsigned before = 1 + rng.below(6);   // successful loads first
        auto start = r.context(seed * 11 + 5);
        start.x[9] = reinterpret_cast<std::uint64_t>(pages) + 4096 - 8ull * before;
        auto expect = start;
        expect.pc = r.program_base();
        auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
        std::memcpy(program, p.words.data(), p.words.size() * 4);
        program[p.words.size()] = 0xd4200000u | (0xdeadu << 5);
        r.reset_memory();
        bool reached = false;
        for (unsigned steps = 0; steps < 200000; ++steps) {
            if (expect.pc == fault_pc && expect.x[9] >= reinterpret_cast<std::uint64_t>(pages) + 4096) {
                reached = true;
                break;
            }
            if (expect.pc == r.program_base() + 4 * p.words.size()) break;
            FX_REQUIRE(testing::oracle_step(expect, *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expect.pc))));
        }
        if (!reached) continue;   // an early exit left the loop first
        for (const auto& c : kConfigs) {
            if (c.optimize == 0) continue;
            apply(c, o);
            r.machine.options.optimize = static_cast<std::int8_t>(c.optimize);
            const BufferCode code(r.code_base(), r.program_base() + (p.words.size() + 1) * 4);
            r.reset_memory();
            auto ctx = start;
            r.machine.reset();
            r.machine.prepare(ctx);
            const auto* host = r.machine.entry_for(r.program_base(), code);
            FX_REQUIRE(host != nullptr);
            FX_REQUIRE(r.machine.regions.size() == 1);
            g_capture = {};
            g_capture.region = &r.machine.regions.back();
            g_capture.host = reinterpret_cast<std::uintptr_t>(host) - r.machine.regions.back().entries[0].offset;
            g_capture.resume = ctx.stub_dispatch_exit;
            auto* handler = AddVectoredExceptionHandler(1, capture_fault);
            ctx.pc = r.program_base();
            r.machine.enter(ctx, host);
            RemoveVectoredExceptionHandler(handler);
            const std::string label = std::string("tier2 fault seed ") + std::to_string(seed) + " (" + c.name + ")" + hex(p.words);
            if (!g_capture.hit) {
                testing::report_failure(__FILE__, __LINE__, label + ": no fault");
                ++failures;
                break;
            }
            ++faults;
            if (c.lend)   // lent pins appear in fault maps as "in the context"
                for (const auto& reg : r.machine.regions.back().fault_map_regs) lent_sites += reg.host == kFaultMapContext;
            auto state = ctx;
            for (unsigned g = 0; g < 32; ++g) {
                if ((g_capture.in_host >> g) & 1u) state.x[g] = g_capture.x[g];
                if ((g_capture.vin_host >> g) & 1u) {
                    state.v[g][0] = g_capture.v[g][0];
                    state.v[g][1] = g_capture.v[g][1];
                }
            }
            state.pc = g_capture.guest_pc;
            // NZCV: not compared. Dead flags (overwritten before any read, e.g.
            // by the loop's next SUBS) are not materialized for faults, in plain
            // translations as well; host EFLAGS would need the runtime's conversion.
            state.nzcv = expect.nzcv;
            if (state.pc != fault_pc) {
                testing::report_failure(__FILE__, __LINE__, label + ": wrong fault pc");
                ++failures;
                break;
            }
            testing::compare_contexts(expect, state, label + " (state at the fault)");
        }
    }
    VirtualFree(pages, 0, MEM_RELEASE);
    core::set_overrides_for_testing(nullptr);
    r.machine.options.optimize = -1;
    std::printf("    faults checked: %u; lent pins in fault maps: %u\n", faults, lent_sites);
    FX_CHECK(faults > 200);
    FX_CHECK(lent_sites > 0);
}

// Tier-up from a loop: a counted first-tier region exits with tier_up_loop
// after 16 x threshold iterations, its start is retranslated optimized and the
// loop resumes at its header (a region of its own from there); results match.
FX_TEST(core_tier2_loop_tier_up) {
    register_families();
    DiffRunner r;
    r.machine.options.lazy = true;
    r.machine.options.optimize = 1;
    r.machine.options.tier_threshold = 1;   // 16 iterations
    unsigned tier_ups = 0;
    for (std::uint64_t seed = 1; seed <= 60; ++seed) {
        const auto p = generate(seed + 90000, false, 40);   // a long outer loop
        auto ctx = r.context(seed * 3);
        for (unsigned g = 0; g < 32; ++g)
            if (g != 16 && g != 17 && g != 31 && g != 30 && g != 9 && g != 12 && g != 18) ctx.x[g] &= 0xffff;
        FX_REQUIRE(r.run(p.words, ctx, "tier-up from a loop seed " + std::to_string(seed) + hex(p.words)));
        tier_ups += r.last.tier_ups;
    }
    FX_CHECK(tier_ups >= 60u);
    // A region without loops is final (no counter, tier 1).
    const std::vector<std::uint32_t> straight = {a64::add_imm(true, false, false, 0, 0, 1), a64::ret()};
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, straight.data(), straight.size() * 4);
    const BufferCode code(r.code_base(), r.program_base() + straight.size() * 4);
    const auto t = translate_region(r.program_base(), code, r.machine.options);
    FX_CHECK_EQ(t.tier, 1u);
    r.machine.options = TranslateOptions{};
}

// Embree's traversal shape: a loop header that falls into another loop's
// header keeps no interrupt check of its own in tier 2 (every cycle through
// it passes the other one), and an interrupt requested before any iteration
// is still taken.
FX_TEST(core_tier2_interrupt_checks) {
    register_families();
    DiffRunner r;
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        movw(false, 2, 0, 3, 12),                        // mov w12, #3
        movw(false, 2, 0, 2, 18),                        // L1: mov w18, #2
        add_imm(true, false, false, 0, 0, 1),            // L2: add x0, x0, #1
        add_imm(true, true, true, 18, 18, 1),            // subs x18, x18, #1
        bcond(1, -2),                                    // b.ne L2
        add_imm(true, true, true, 12, 12, 1),            // subs x12, x12, #1
        bcond(1, -5),                                    // b.ne L1
    };
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    const BufferCode code(r.code_base(), r.program_base() + prog.size() * 4);
    const auto count_checks = [](const TranslatedRegion& t) {   // cmp dword [r15 + disp32(interrupt_pending)], 0
        const auto disp = static_cast<std::int32_t>(offsetof(GuestContext, interrupt_pending)) - static_cast<std::int32_t>(kContextBias);
        unsigned n = 0;
        for (std::size_t k = 0; k + 8 <= t.code.size(); ++k) {
            std::int32_t d;
            std::memcpy(&d, t.code.data() + k + 3, 4);
            n += t.code[k] == 0x41 && t.code[k + 1] == 0x83 && t.code[k + 2] == 0xbf && d == disp;
        }
        return n;
    };
    TranslateOptions plain, opt;
    plain.optimize = 0;
    opt.optimize = 1;
    const auto a = translate_region(r.program_base(), code, plain);
    const auto b = translate_region(r.program_base(), code, opt);
    FX_CHECK_EQ(count_checks(a), 3u);   // region start, L1, L2
    FX_CHECK_EQ(count_checks(b), 2u);   // region start, L2
    r.machine.options.optimize = 1;
    auto ctx = r.context(5);
    ctx.interrupt_pending = 1;
    FX_REQUIRE(r.run(prog, ctx, "interrupt in a tier-2 loop nest"));
    FX_CHECK(r.last.interrupts >= 1u);
    r.machine.options.optimize = -1;
}
