// Single-instruction differential tests for exclusives, ordered accesses and
// LSE atomics (owner: atomics/system).
#include "fx/translator/emit_system_test_util.hpp"

#include "fx/testing/test.hpp"

#include <cstring>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::systest;
using testing::Buffer;

namespace {

struct Regs { unsigned rt, rn, rs, rv; };

// Register combinations for (Rt, Rn, Rs, Rv) incl. SP base, XZR value/status.
constexpr Regs kCombos[] = {
    {0, 1, 2, 3}, {5, 31, 6, 7}, {30, 2, 3, 30}, {8, 9, 10, 31}, {12, 13, 14, 12}, {3, 4, 31, 5},
};

std::uint64_t value_of(std::uint64_t seed, unsigned bytes) {
    std::uint64_t state = seed + 99;
    const auto v = testing::random_u64(state);
    return bytes == 8 ? v : v & ((1ULL << (bytes * 8)) - 1);
}

GuestContext context_for(std::uint64_t seed, unsigned rn, std::uint64_t address) {
    auto ctx = make_context(seed);
    ctx.x[rn] = address;   // rn == 31 sets SP
    return ctx;
}

} // namespace

FX_TEST(atomic_ldxr_stxr_all_sizes_and_orders) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 1;
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (unsigned order = 0; order < 4; ++order) {
            for (const auto& r : kCombos) {
                if (r.rt == r.rn || r.rs == r.rn || r.rs == r.rt || r.rs == r.rv) continue;
                fill_random(buffer, seed);
                const auto address = buffer.address() + 64 + bytes * (seed % 8);
                auto ctx = context_for(seed++, r.rn, address);
                const std::vector<std::uint32_t> words = {
                    a64::ldxr(bytes, r.rt, r.rn, order & 1), a64::stxr(bytes, r.rs, r.rv, r.rn, order & 2)};
                GuestContext actual;
                differential(words, ctx, buffer, "ldxr/stxr bytes=" + std::to_string(bytes) + " order=" +
                             std::to_string(order) + " rt=" + std::to_string(r.rt), 4096, &actual);
                FX_CHECK_EQ(actual.excl_size, 0u);
                // load alone: reservation recorded
                auto only = ctx;
                const std::uint32_t load = a64::ldxr(bytes, r.rt, r.rn, order & 1);
                differential({load}, ctx, buffer, "ldxr alone", 4096, &only);
                FX_CHECK_EQ(only.excl_size, bytes);
                FX_CHECK_EQ(only.excl_address, address);
                std::uint64_t expected_value = 0;
                std::memcpy(&expected_value, reinterpret_cast<void*>(address), bytes);
                FX_CHECK_EQ(only.excl_value[0], expected_value);
            }
        }
    }
}

FX_TEST(atomic_stxr_fails_without_matching_reservation) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 77;
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        fill_random(buffer, seed);
        auto ctx = context_for(seed++, 1, buffer.address() + 128);
        ctx.x[4] = buffer.address() + 128 + 8;
        // no reservation at all
        differential({a64::stxr(bytes, 2, 3, 1)}, ctx, buffer, "stxr no reservation");
        // after CLREX
        differential({a64::ldxr(bytes, 0, 1), a64::kClrex, a64::stxr(bytes, 2, 3, 1, true)}, ctx, buffer,
                     "stxr after clrex");
        // different address
        differential({a64::ldxr(bytes, 0, 1), a64::stxr(bytes, 2, 3, 4)}, ctx, buffer, "stxr other address");
        // different size
        const unsigned other = bytes == 8 ? 4 : bytes * 2;
        differential({a64::ldxr(bytes, 0, 1), a64::stxr(other, 2, 3, 1)}, ctx, buffer, "stxr other size");
        // reservation consumed by the first store
        differential({a64::ldxr(bytes, 0, 1), a64::stxr(bytes, 2, 3, 1), a64::stxr(bytes, 5, 6, 1)}, ctx, buffer,
                     "second stxr");
    }
}

FX_TEST(atomic_stxr_detects_intervening_write_and_accepts_aba) {
    Buffer buffer(4096);
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        std::memset(buffer.data(), 0x11, 64);
        auto ctx = context_for(5, 1, buffer.address());
        ctx.x[3] = 0x5566778899aabbccULL;
        Program load({a64::ldxr(bytes, 0, 1, true)});
        Program store({a64::stxr(bytes, 2, 3, 1, true)});
        load.run(ctx);
        buffer.data()[0] ^= 0xff;                 // another agent writes
        store.run(ctx);
        FX_CHECK_EQ(ctx.x[2], std::uint64_t{1});
        FX_CHECK_EQ(buffer.data()[0], std::uint8_t{0xee});
        FX_CHECK_EQ(ctx.excl_size, 0u);
        load.run(ctx);
        buffer.data()[0] ^= 0xff;                 // A -> B -> A: accepted like QEMU/Dynarmic
        buffer.data()[0] ^= 0xff;
        store.run(ctx);
        FX_CHECK_EQ(ctx.x[2], std::uint64_t{0});
        std::uint64_t stored = 0;
        std::memcpy(&stored, buffer.data(), bytes);
        FX_CHECK_EQ(stored, bytes == 8 ? ctx.x[3] : ctx.x[3] & ((1ULL << (bytes * 8)) - 1));
    }
}

FX_TEST(atomic_ldxp_stxp_pairs) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 300;
    for (bool wide : {false, true}) {
        for (unsigned order = 0; order < 4; ++order) {
            for (unsigned variant = 0; variant < 3; ++variant) {
                fill_random(buffer, seed);
                const unsigned rn = variant == 1 ? 31 : 4;
                auto ctx = context_for(seed++, rn, buffer.address() + 256);
                const unsigned rt = variant == 2 ? 30 : 0, rt2 = variant == 2 ? 29 : 1;
                const unsigned v1 = variant == 2 ? 31 : 6, v2 = 7;
                differential({a64::ldxp(wide, rt, rt2, rn, order & 1), a64::stxp(wide, 2, v1, v2, rn, order & 2)},
                             ctx, buffer, "ldxp/stxp wide=" + std::to_string(wide) + " v=" + std::to_string(variant));
                differential({a64::ldxp(wide, rt, rt2, rn), a64::kClrex, a64::stxp(wide, 2, v1, v2, rn)}, ctx,
                             buffer, "stxp after clrex");
                differential({a64::ldxp(!wide, rt, rt2, rn), a64::stxp(wide, 2, v1, v2, rn)}, ctx, buffer,
                             "stxp size mismatch");
                // reservation detects a change in either half
                if (variant == 0) {
                    auto c = ctx;
                    Program load({a64::ldxp(wide, rt, rt2, rn, true)});
                    Program store({a64::stxp(wide, 2, v1, v2, rn, true)});
                    load.run(c);
                    buffer.data()[256 + (wide ? 12 : 6)] ^= 1;
                    store.run(c);
                    FX_CHECK_EQ(c.x[2], std::uint64_t{1});
                }
            }
        }
    }
}

FX_TEST(atomic_ordered_loads_and_stores) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 900;
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (const auto& r : kCombos) {
            if (r.rt == r.rn) continue;
            fill_random(buffer, seed);
            const auto address = buffer.address() + 512 + bytes * 3;
            auto ctx = context_for(seed++, r.rn, address);
            differential({a64::ldar(bytes, r.rt, r.rn)}, ctx, buffer, "ldar");
            differential({a64::stlr(bytes, r.rv, r.rn)}, ctx, buffer, "stlr");
            // LDLAR/STLLR/LDAPR behave like LDAR/STLR on one thread.
            differential({a64::ldar(bytes, r.rt, r.rn)}, {a64::ldlar(bytes, r.rt, r.rn)}, ctx, buffer, "ldlar");
            differential({a64::ldar(bytes, r.rt, r.rn)}, {a64::ldapr(bytes, r.rt, r.rn)}, ctx, buffer, "ldapr");
            differential({a64::stlr(bytes, r.rv, r.rn)}, {a64::stllr(bytes, r.rv, r.rn)}, ctx, buffer, "stllr");
            // The oracle implements LDAPR and the LRCPC2 forms directly too.
            differential({a64::ldapr(bytes, r.rt, r.rn)}, ctx, buffer, "ldapr oracle");
            for (unsigned opc = 0; opc < 4; ++opc) {
                if ((bytes == 8 && opc >= 2) || (bytes == 4 && opc == 3)) continue;
                for (int imm : {-256, -8, 0, 8, 248}) {   // oracle models no FEAT_LSE2: aligned only
                    auto c = ctx;
                    c.x[r.rn] = (address + 256) & ~std::uint64_t{7};
                    differential({a64::rcpc2(bytes, opc, imm, opc == 0 ? r.rv : r.rt, r.rn)}, c, buffer,
                                 "rcpc2 oracle opc=" + std::to_string(opc));
                }
            }
            // LDAPUR/LDAPURS/STLUR == LDUR/LDURS/STUR.
            for (unsigned opc = 0; opc < 4; ++opc) {
                if ((bytes == 8 && opc >= 2) || (bytes == 4 && opc == 3)) continue;
                for (int imm : {-256, -3, 0, 7, 255}) {
                    auto c = ctx;
                    c.x[r.rn] = address + 256;
                    const std::uint32_t unscaled = (a64::sz(bytes) << 30) | 0x38000000u | (opc << 22) |
                                                   ((static_cast<unsigned>(imm) & 0x1ffu) << 12) | (r.rn << 5) |
                                                   (opc == 0 ? r.rv : r.rt);
                    differential({unscaled}, {a64::rcpc2(bytes, opc, imm, opc == 0 ? r.rv : r.rt, r.rn)}, c, buffer,
                                 "rcpc2 opc=" + std::to_string(opc) + " imm=" + std::to_string(imm));
                }
            }
        }
    }
}

FX_TEST(atomic_cas_all_sizes_orders_and_registers) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 1200;
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (unsigned order = 0; order < 4; ++order) {
            for (const auto& r : kCombos) {
                if (r.rs == r.rn && r.rs != 31) continue;
                for (bool match : {true, false}) {
                    fill_random(buffer, seed);
                    const auto address = buffer.address() + 1024 + bytes * (seed % 4);
                    auto ctx = context_for(seed++, r.rn, address);
                    std::uint64_t memory = 0;
                    std::memcpy(&memory, reinterpret_cast<void*>(address), bytes);
                    if (r.rs != 31) {
                        // matching compare value with junk above the access size
                        ctx.x[r.rs] = match ? (memory | (bytes < 8 ? (ctx.x[r.rs] << (bytes * 8)) : 0))
                                            : memory ^ 1;
                    } else if (match) {
                        std::memset(reinterpret_cast<void*>(address), 0, bytes);
                    }
                    differential({a64::cas(bytes, order & 1, order & 2, r.rs, r.rv, r.rn)}, ctx, buffer,
                                 "cas bytes=" + std::to_string(bytes) + " match=" + std::to_string(match) +
                                 " rs=" + std::to_string(r.rs) + " rv=" + std::to_string(r.rv));
                }
            }
        }
    }
}

FX_TEST(atomic_casp_pairs) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 1500;
    struct P { unsigned rs, rt, rn; };
    for (bool wide : {false, true}) {
        for (unsigned order = 0; order < 4; ++order) {
            for (const P& p : {P{0, 2, 4}, P{30, 6, 31}, P{8, 30, 10}, P{12, 12, 1}}) {
                for (bool match : {true, false}) {
                    fill_random(buffer, seed);
                    const auto address = buffer.address() + 2048;
                    auto ctx = context_for(seed++, p.rn, address);
                    const unsigned bytes = wide ? 8 : 4;
                    std::uint64_t lo = 0, hi = 0;
                    std::memcpy(&lo, reinterpret_cast<void*>(address), bytes);
                    std::memcpy(&hi, reinterpret_cast<void*>(address + bytes), bytes);
                    if (p.rs != 30 || !wide) {
                        ctx.x[p.rs] = match ? lo : lo + 1;
                        if (p.rs + 1 < 31) ctx.x[p.rs + 1] = hi;
                        if (p.rs + 1 == 31 && match) std::memset(reinterpret_cast<void*>(address + bytes), 0, bytes);
                    } else {
                        ctx.x[30] = match ? lo : lo + 1;
                        if (match) std::memset(reinterpret_cast<void*>(address + bytes), 0, bytes);
                    }
                    differential({a64::casp(wide, order & 1, order & 2, p.rs, p.rt, p.rn)}, ctx, buffer,
                                 "casp wide=" + std::to_string(wide) + " rs=" + std::to_string(p.rs) +
                                 " match=" + std::to_string(match));
                }
            }
        }
    }
}

FX_TEST(atomic_lse_rmw_all_ops_sizes_orders) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 2000;
    for (unsigned opc = 0; opc <= 8; ++opc) {
        for (unsigned bytes : {1u, 2u, 4u, 8u}) {
            for (unsigned order = 0; order < 4; ++order) {
                for (const auto& r : kCombos) {
                    if (r.rs == r.rn && r.rs != 31) continue;
                    for (unsigned trial = 0; trial < 3; ++trial) {
                        fill_random(buffer, seed);
                        const auto address = buffer.address() + 3000 + bytes * (seed % 5);
                        auto ctx = context_for(seed++, r.rn, address & ~std::uint64_t{bytes - 1});
                        if (trial == 1 && r.rs != 31) ctx.x[r.rs] = ~ctx.x[r.rs];
                        if (trial == 2) std::memset(reinterpret_cast<void*>(ctx.x[r.rn == 31 ? 31 : r.rn]), 0x80, 8);
                        const unsigned rt = (trial == 2 && opc < 8) ? 31 : r.rv;   // ST<op> aliases
                        differential({a64::lse(opc, bytes, order & 1, order & 2, r.rs, rt, r.rn)}, ctx, buffer,
                                     "lse opc=" + std::to_string(opc) + " bytes=" + std::to_string(bytes) +
                                     " rs=" + std::to_string(r.rs) + " rt=" + std::to_string(rt));
                    }
                }
            }
        }
    }
}

FX_TEST(atomic_misaligned_lse_performs_the_operation) {
    Buffer buffer(8192);
    // Split across a cache line (and page for 8 bytes at 4093): x64 executes
    // split locks; there is no alignment check.
    for (std::uint64_t offset : {61ULL, 63ULL, 4093ULL}) {
        std::memset(buffer.data(), 0, 8192);
        auto ctx = context_for(3, 1, buffer.address() + offset);
        ctx.x[2] = 0x0102030405060708ULL;
        Program({a64::lse(0, 8, true, true, 2, 3, 1)}).run(ctx);   // ldaddal
        Program({a64::lse(0, 8, true, true, 2, 3, 1)}).run(ctx);
        std::uint64_t v = 0;
        std::memcpy(&v, buffer.data() + offset, 8);
        FX_CHECK_EQ(v, 2 * 0x0102030405060708ULL);
        FX_CHECK_EQ(ctx.x[3], 0x0102030405060708ULL);
        ctx.x[4] = v;
        ctx.x[5] = 77;
        Program({a64::cas(8, true, true, 4, 5, 1)}).run(ctx);     // casal hit
        std::memcpy(&v, buffer.data() + offset, 8);
        FX_CHECK_EQ(v, std::uint64_t{77});
        Program({a64::lse(8, 4, false, false, 2, 6, 1)}).run(ctx); // swp w
        std::uint32_t w = 0;
        std::memcpy(&w, buffer.data() + offset, 4);
        FX_CHECK_EQ(w, 0x05060708u);
        FX_CHECK_EQ(ctx.x[6], std::uint64_t{77});
    }
}

FX_TEST(atomic_describe_reports_registers) {
    InstructionUse use;
    FX_REQUIRE(describe_system(a64::ldxr(4, 3, 31), use));
    FX_CHECK_EQ(use.gpr_read, 1u << 31);
    FX_CHECK_EQ(use.gpr_write, 1u << 3);
    FX_CHECK(use.may_fault);
    FX_REQUIRE(describe_system(a64::stxr(8, 2, 5, 6, true), use));
    FX_CHECK_EQ(use.gpr_read, (1u << 6) | (1u << 5));
    FX_CHECK_EQ(use.gpr_write, 1u << 2);
    FX_REQUIRE(describe_system(a64::lse(0, 8, false, false, 1, 31, 2), use));
    FX_CHECK_EQ(use.gpr_write, 0u);
    FX_CHECK_EQ(use.gpr_read, (1u << 2) | (1u << 1));
    FX_REQUIRE(describe_system(a64::casp(true, true, true, 4, 6, 8), use));
    FX_CHECK_EQ(use.gpr_write, (1u << 4) | (1u << 5));
}

// STLR = plain store + release_pending; LDAR/LDAXR fence iff it is set and
// clear it; full barriers clear it; RCpc loads leave it (NOTES_SYSTEM.md).
FX_TEST(atomic_release_pending_protocol) {
    Buffer buffer(4096);
    struct Case { const char* name; std::vector<std::uint32_t> words; std::uint8_t before, after; };
    const std::uint32_t str_x1 = a64::str_x(1, 0, 16), ldr_x2 = a64::ldr_x(2, 0, 24);
    const Case cases[] = {
        {"stlr", {a64::stlr(8, 1, 0)}, 0, 1},
        {"stlrb", {a64::stlr(1, 1, 0)}, 0, 1},
        {"stllr", {a64::stllr(4, 1, 0)}, 0, 1},
        {"stlur", {a64::rcpc2(8, 0, 8, 1, 0)}, 0, 1},
        {"stlr; ldar (xchg)", {a64::stlr(8, 1, 0), a64::ldar(8, 2, 0)}, 0, 0},
        {"stlr; b; ldar (flag)", {a64::stlr(8, 1, 0), a64::b(1), a64::ldar(8, 2, 0)}, 0, 0},
        {"stlr; b; ldapr", {a64::stlr(8, 1, 0), a64::b(1), a64::ldapr(8, 2, 0)}, 0, 1},
        {"stlr; ldlar", {a64::stlr(8, 1, 0), a64::ldlar(4, 2, 0)}, 0, 0},
        {"stlr; ldaxr; clrex", {a64::stlr(8, 1, 0), a64::ldxr(8, 2, 0, true), a64::kClrex}, 0, 0},
        {"stlr; ldaxp; clrex", {a64::stlr(8, 1, 0), a64::ldxp(true, 2, 3, 0, true), a64::kClrex}, 0, 0},
        {"stlr; ldxr (no acquire); clrex", {a64::stlr(8, 1, 0), a64::ldxr(8, 2, 0), a64::kClrex}, 0, 1},
        {"stlr; ldapr (RCpc)", {a64::stlr(8, 1, 0), a64::ldapr(8, 2, 0)}, 0, 1},
        {"stlr; ldapur (RCpc)", {a64::stlr(8, 1, 0), a64::rcpc2(8, 1, 8, 2, 0)}, 0, 1},
        {"stlr; ldr", {a64::stlr(8, 1, 0), ldr_x2}, 0, 1},
        {"stlr; dmb ishld", {a64::stlr(8, 1, 0), a64::kDmbIshld}, 0, 1},
        {"stlr; dmb ishst", {a64::stlr(8, 1, 0), a64::kDmbIshst}, 0, 1},
        {"stlr; dmb ish", {a64::stlr(8, 1, 0), a64::kDmbIsh}, 0, 0},
        {"stlr; str; add; dmb sy", {a64::stlr(8, 1, 0), str_x1, a64::add_imm(true, 5, 5, 1), a64::kDmbSy}, 0, 0},
        {"stlr; dsb ish", {a64::stlr(8, 1, 0), a64::kDsbIsh}, 0, 0},
        {"stlr; ldaddal (locked)", {a64::stlr(8, 1, 0), a64::lse(0, 8, true, true, 1, 2, 0)}, 0, 1},
        {"pending; ldar", {a64::ldar(8, 2, 0)}, 1, 0},
        {"pending; ldapr", {a64::ldapr(8, 2, 0)}, 1, 1},
        {"pending; dmb ish", {a64::kDmbIsh}, 1, 0},
        {"pending; dmb ish; str; dmb ish", {a64::kDmbIsh, str_x1, a64::kDmbIsh}, 1, 0},
        {"clear; ldar", {a64::ldar(8, 2, 0)}, 0, 0},
    };
    for (bool legacy : {false, true}) {
        LegacyOrderingScope scope(legacy);
        for (const auto& c : cases) {
            auto ctx = make_context(7);
            ctx.x[0] = buffer.address() + 64;
            ctx.x[1] = 0x1122334455667788ULL;
            ctx.release_pending = c.before;
            GuestContext actual;
            const int failures = testing::current_failures();
            // The oracle lacks LDLAR/STLLR: it runs LDAR/STLR (o0 = 1) instead.
            std::vector<std::uint32_t> reference = c.words;
            for (auto& w : reference) {
                if ((w & 0x3fa08000u) == 0x08800000u) w |= 0x8000u;
            }
            differential(reference, c.words, ctx, buffer, std::string(c.name) + (legacy ? " [legacy]" : ""), 4096,
                         &actual);
            // Legacy code never touches the flag (STLR is xchg).
            FX_CHECK_EQ(static_cast<unsigned>(actual.release_pending), static_cast<unsigned>(legacy ? c.before : c.after));
            if (testing::current_failures() != failures) std::printf("    case %s legacy=%d\n", c.name, legacy);
        }
    }
}

// Should-be-one fields (Rs of loads, Rt2 of single-register forms, Rs of
// LDAPR) with other values are CONSTRAINED UNPREDICTABLE; they execute as
// the canonical encoding instead of falling back to the interpreter.
FX_TEST(atomic_should_be_one_fields_execute_as_canonical) {
    Buffer buffer(1 << 16);
    std::uint64_t seed = 1300;
    const auto with = [](std::uint32_t w, unsigned rs, unsigned rt2) {
        return (w & ~((31u << 16) | (31u << 10))) | (rs << 16) | (rt2 << 10);
    };
    for (unsigned bytes : {1u, 2u, 4u, 8u}) {
        for (const std::pair<unsigned, unsigned> f : {std::pair{1u, 31u}, std::pair{31u, 4u}, std::pair{5u, 0u}}) {
            const auto [rs, rt2] = f;
            fill_random(buffer, seed);
            auto ctx = context_for(seed++, 3, buffer.address() + 256);
            ctx.x[7] = buffer.address() + 512;
            struct Pair { const char* name; std::vector<std::uint32_t> canonical, odd; };
            std::vector<Pair> pairs = {
                {"ldxr", {a64::ldxr(bytes, 2, 3), a64::kClrex}, {with(a64::ldxr(bytes, 2, 3), rs, rt2), a64::kClrex}},
                {"ldaxr", {a64::ldxr(bytes, 2, 3, true), a64::kClrex}, {with(a64::ldxr(bytes, 2, 3, true), rs, rt2), a64::kClrex}},
                {"ldar", {a64::ldar(bytes, 2, 3)}, {with(a64::ldar(bytes, 2, 3), rs, rt2)}},
                {"ldlar", {a64::ldar(bytes, 2, 3)}, {with(a64::ldlar(bytes, 2, 3), rs, rt2)}},
                {"stlr", {a64::stlr(bytes, 6, 3)}, {with(a64::stlr(bytes, 6, 3), rs, rt2)}},
                {"stllr", {a64::stlr(bytes, 6, 3)}, {with(a64::stllr(bytes, 6, 3), rs, rt2)}},
                {"ldapr", {a64::ldar(bytes, 2, 3)}, {(a64::ldapr(bytes, 2, 3) & ~(31u << 16)) | (rs << 16)}},
                // STXR/STLXR: only Rt2 is should-be-one (Rs is the status).
                {"ldxr; stxr", {a64::ldxr(bytes, 2, 3), a64::stxr(bytes, 8, 6, 3)},
                 {a64::ldxr(bytes, 2, 3), with(a64::stxr(bytes, 8, 6, 3), 8, rt2)}},
                {"ldaxr; stlxr", {a64::ldxr(bytes, 2, 3, true), a64::stxr(bytes, 8, 6, 3, true)},
                 {a64::ldxr(bytes, 2, 3, true), with(a64::stxr(bytes, 8, 6, 3, true), 8, rt2)}},
            };
            if (bytes >= 4) {   // LDXP/LDAXP: Rs is should-be-one (Rt2 is the second register)
                const bool wide = bytes == 8;
                pairs.push_back({"ldxp", {a64::ldxp(wide, 2, 4, 3), a64::kClrex},
                                 {(a64::ldxp(wide, 2, 4, 3) & ~(31u << 16)) | (rs << 16), a64::kClrex}});
                pairs.push_back({"ldaxp; stlxp", {a64::ldxp(wide, 2, 4, 3, true), a64::stxp(wide, 8, 6, 9, 3, true)},
                                 {(a64::ldxp(wide, 2, 4, 3, true) & ~(31u << 16)) | (rs << 16),
                                  a64::stxp(wide, 8, 6, 9, 3, true)}});
            }
            for (const auto& p : pairs) {
                differential(p.canonical, p.odd, ctx, buffer, std::string(p.name) + " rs=" + std::to_string(rs) +
                             " rt2=" + std::to_string(rt2) + " bytes=" + std::to_string(bytes));
                InstructionUse use;
                FX_CHECK(describe_system(p.odd[0], use));
                x64::Assembler a;
                testing::MemoryEmitContext e(a, 0x1000);
                FX_CHECK(emit_system(e, std::span(p.odd.data(), 1)) != 0u);
                e.end_instruction();
            }
        }
    }
    // Unallocated encodings stay undefined: odd CASP registers, LSE o3=1
    // opcodes other than SWP/LDAPR.
    for (const std::uint32_t word : {a64::casp(true, false, false, 1, 2, 3),
                                     a64::lse(0, 8, false, false, 1, 2, 3) | (1u << 15) | (1u << 12)}) {
        x64::Assembler a;
        testing::MemoryEmitContext e(a, 0x1000);
        FX_CHECK_EQ(emit_system(e, std::span(&word, 1)), 0u);
    }
}
