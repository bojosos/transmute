// Exercises MemoryEmitContext, Snippet, the oracle and the comparator with a
// toy family (ADD/SUBS shifted register, LDR X immediate, CSEL).
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"

#include <cstring>
#include <nmmintrin.h>

using namespace transmute::fx;
using namespace transmute::fx::testing;
using x64::Gp;

namespace {

unsigned toy_family(EmitContext& ctx, std::span<const std::uint32_t> words) {
    const auto w = words[0];
    auto& a = ctx.as();
    const unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    if ((w & 0xffe0fc00u) == 0x8b000000u || (w & 0xffe0fc00u) == 0xeb000000u) {  // ADD / SUBS Xd,Xn,Xm
        const bool subs = (w & 0xffe0fc00u) == 0xeb000000u;
        const auto n = ctx.read_gpr(rn);
        const auto m = ctx.read_gpr(rm);
        const auto t = ctx.temp_gpr();
        a.mov(64, t, n);
        if (subs) { a.sub(64, t, m); ctx.set_flags(FlagsProducer::sub); }
        else { ctx.clobber_host_flags(); a.add(64, t, m); }
        const auto d = ctx.write_gpr(rd);
        a.mov(64, d, t);
        return 1;
    }
    if ((w & 0xffc00000u) == 0xf9400000u) {   // LDR Xt, [Xn|SP, #imm12*8]
        const auto base = ctx.read_gpr(rn, true);
        const auto value = ctx.temp_gpr();
        ctx.mark_may_fault();
        a.mov(64, value, x64::ptr(base, static_cast<std::int32_t>(((w >> 10) & 0xfff) * 8)));
        const auto d = ctx.write_gpr(rd);
        a.mov(64, d, value);
        return 1;
    }
    if ((w & 0xffe00c00u) == 0x9a800000u) {   // CSEL Xd, Xn, Xm, cond
        const auto n = ctx.read_gpr(rn);
        const auto m = ctx.read_gpr(rm);
        const auto t = ctx.temp_gpr();
        a.mov(64, t, m);
        const auto cond = (w >> 12) & 15;
        if (cond >= 14) a.mov(64, t, n);
        else a.cmov(ctx.condition(cond), 64, t, n);
        const auto d = ctx.write_gpr(rd);
        a.mov(64, d, t);
        return 1;
    }
    return 0;
}

} // namespace

FX_TEST(harness_add_subs_matches_oracle) {
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        auto ctx = random_context(seed);
        for (std::uint32_t word : {0x8b020020u /*add x0,x1,x2*/, 0xeb030041u /*subs x1,x2,x3*/,
                                   0xeb1f03e5u /*subs x5,xzr,xzr*/}) {
            auto expected = ctx;
            FX_REQUIRE(oracle_step(expected, word));
            auto actual = ctx;
            FX_REQUIRE(run_family(actual, toy_family, std::span(&word, 1)));
            compare_contexts(expected, actual, "seed " + std::to_string(seed));
        }
    }
}

FX_TEST(harness_csel_all_conditions) {
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        for (std::uint32_t cond = 0; cond < 16; ++cond) {
            auto ctx = random_context(seed);
            const std::uint32_t word = 0x9a820020u | (cond << 12);   // csel x0, x1, x2, cond
            auto expected = ctx;
            FX_REQUIRE(oracle_step(expected, word));
            auto actual = ctx;
            FX_REQUIRE(run_family(actual, toy_family, std::span(&word, 1)));
            compare_contexts(expected, actual, "cond " + std::to_string(cond));
        }
    }
}

// The snippet runner must preserve the Win64 callee-saved XMM registers even
// when emitted code clobbers all sixteen.
FX_TEST(harness_snippet_preserves_callee_saved_xmm) {
    x64::Assembler a;
    a.sub(64, Gp::rsp, 40);
    for (unsigned i = 0; i < 16; ++i) a.pxor(x64::xmm(i), x64::xmm(i));
    a.add(64, Gp::rsp, 40);
    a.ret();
    a.finalize();
    Snippet snippet(a.bytes());
    auto ctx = random_context(9);
    volatile double keep[10];
    double sum_before = 0, sum_after = 0;
    for (int i = 0; i < 10; ++i) { keep[i] = 1.5 * (i + 1); sum_before += keep[i]; }
    snippet.run(ctx);
    for (int i = 0; i < 10; ++i) sum_after += keep[i];
    FX_CHECK_EQ(sum_before, sum_after);
}

FX_TEST(harness_load_uses_host_memory) {
    Buffer buffer(4096);
    for (unsigned i = 0; i < 512; ++i) reinterpret_cast<std::uint64_t*>(buffer.data())[i] = i * 0x1111;
    auto ctx = random_context(7);
    ctx.x[3] = buffer.address();
    const std::uint32_t word = 0xf9400c62u;   // ldr x2, [x3, #24]
    auto expected = ctx;
    FX_REQUIRE(oracle_step(expected, word));
    auto actual = ctx;
    FX_REQUIRE(run_family(actual, toy_family, std::span(&word, 1)));
    FX_CHECK_EQ(actual.x[2], std::uint64_t{3 * 0x1111});
    compare_contexts(expected, actual, "ldr");
}

// FEAT_LRCPC / FEAT_LRCPC2 in the reference interpreter (oracle and fallback).
// Encodings come from LLVM (-march=armv8.4-a); expected values are computed
// from memory directly.
FX_TEST(interpreter_rcpc_loads_and_stores) {
    Buffer buffer(4096);
    auto* b = buffer.data();
    for (unsigned i = 0; i < 512; ++i) b[i] = static_cast<std::uint8_t>(i * 37 + 11);
    b[65] = 0x9c;                                   // negative byte at x6 + 1
    b[68] = 0x78; b[69] = 0x56; b[70] = 0x34; b[71] = 0xf2;  // negative half/word
    const auto base = buffer.address();
    const auto load = [&](std::uint64_t address, unsigned bytes) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(address), bytes);
        return value;
    };
    struct Case {
        const char* name;
        std::uint32_t word;
        unsigned rt;
        std::uint64_t x2, x6, x8, sp;
        std::uint64_t expected;
    };
    const std::uint64_t x6 = base + 64;
    const Case loads[] = {
        {"ldapr x0,[x8]", 0xf8bfc100u, 0, 0, 0, base, 0, load(base, 8)},
        {"ldapr w8,[x8]", 0xb8bfc108u, 8, 0, 0, base, 0, load(base, 4)},
        {"ldaprb w1,[x2]", 0x38bfc041u, 1, base + 3, 0, 0, 0, load(base + 3, 1)},
        {"ldaprh w3,[sp]", 0x78bfc3e3u, 3, 0, 0, 0, base + 16, load(base + 16, 2)},
        {"ldapur x5,[x6,#-8]", 0xd95f80c5u, 5, 0, x6, 0, 0, load(x6 - 8, 8)},
        {"ldapur w13,[x6,#255]", 0x994ff0cdu, 13, 0, base + 1, 0, 0, load(base + 256, 4)},
        {"ldapurb w14,[x6,#-256]", 0x195000ceu, 14, 0, base + 300, 0, 0, load(base + 44, 1)},
        {"ldapurh w12,[x6,#2]", 0x594020ccu, 12, 0, x6, 0, 0, load(x6 + 2, 2)},
        {"ldapursb x7,[x6,#1]", 0x198010c7u, 7, 0, x6, 0, 0, 0xffffffffffffff9cULL},
        {"ldapursb w7,[x6,#1]", 0x19c010c7u, 7, 0, x6, 0, 0, 0xffffff9cULL},
        {"ldapursh x15,[x6,#6]", 0x598060cfu, 15, 0, x6, 0, 0, 0xfffffffffffff234ULL},
        {"ldapursh w16,[x6,#6]", 0x59c060d0u, 16, 0, x6, 0, 0, 0xfffff234ULL},
        {"ldapursw x9,[x6,#4]", 0x998040c9u, 9, 0, x6, 0, 0, 0xfffffffff2345678ULL},
    };
    for (const auto& c : loads) {
        auto ctx = random_context(42);
        ctx.x[2] = c.x2; ctx.x[6] = c.x6; ctx.x[8] = c.x8; ctx.x[31] = c.sp;
        auto expected = ctx;
        expected.x[c.rt] = c.expected;
        expected.pc += 4;
        FX_REQUIRE(oracle_step(ctx, c.word));
        compare_contexts(expected, ctx, c.name);
    }

    struct Store {
        const char* name;
        std::uint32_t word;
        std::uint64_t address;
        unsigned bytes;
        std::uint64_t value;
    };
    auto ctx = random_context(43);
    ctx.x[6] = x6;
    ctx.x[31] = base + 128;
    ctx.x[10] = 0x1122334455667788ULL;
    ctx.x[11] = 0xaabbccddeeff0011ULL;
    ctx.x[17] = 0x0123456789abcdefULL;
    const Store stores[] = {
        {"stlur x17,[x6,#16]", 0xd90100d1u, x6 + 16, 8, ctx.x[17]},
        {"stlur w10,[x6,#12]", 0x9900c0cau, x6 + 12, 4, ctx.x[10] & 0xffffffffu},
        {"stlurh w11,[x6,#-2]", 0x591fe0cbu, x6 - 2, 2, ctx.x[11] & 0xffffu},
        {"stlurb wzr,[sp,#3]", 0x190033ffu, base + 131, 1, 0},
    };
    for (const auto& s : stores) {
        b[s.address - base] = 0xa5;   // make a stored zero byte observable
        auto expected = ctx;
        expected.pc += 4;
        FX_REQUIRE(oracle_step(ctx, s.word));
        compare_contexts(expected, ctx, s.name);
        FX_CHECK_EQ(load(s.address, s.bytes), s.value);
    }

    // Misaligned acquire loads fault exactly like LDAR; unallocated LRCPC2
    // encodings (size 11, opc 1x) stay unsupported.
    auto misaligned = random_context(44);
    misaligned.x[8] = base + 1;
    FX_CHECK(!oracle_step(misaligned, 0xf8bfc100u));
    auto unallocated = random_context(45);
    unallocated.x[6] = x6;
    FX_CHECK(!oracle_step(unallocated, 0xd98010c7u));
}

// FEAT_CRC32 in the reference interpreter: standard check values for
// "123456789" (CRC-32 0xCBF43926, CRC-32C 0xE3069283), wide forms equal to
// byte-wise accumulation, and CRC32C X matching the host SSE4.2 crc32.
FX_TEST(interpreter_crc32_family) {
    const char* text = "123456789";
    const auto checksum = [&](std::uint32_t byte_word) {
        auto ctx = random_context(50);
        ctx.x[1] = 0xffffffffu;
        for (const char* p = text; *p; ++p) {
            ctx.x[2] = 0xffffffffffffff00ULL | static_cast<std::uint8_t>(*p);  // upper bits ignored
            if (!oracle_step(ctx, byte_word)) return std::uint64_t{0};
        }
        return ctx.x[1] ^ 0xffffffffu;
    };
    FX_CHECK_EQ(checksum(0x1ac24021u), std::uint64_t{0xcbf43926u});   // crc32b w1, w1, w2
    FX_CHECK_EQ(checksum(0x1ac25021u), std::uint64_t{0xe3069283u});   // crc32cb w1, w1, w2

    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        auto ctx = random_context(seed);
        // crc32x w8, w8, x13 (the zlib instruction) == 8 x crc32b
        auto wide = ctx;
        FX_REQUIRE(oracle_step(wide, 0x9acd4d08u));
        auto narrow = ctx;
        narrow.x[8] &= 0xffffffffu;
        const auto data = ctx.x[13];
        for (unsigned i = 0; i < 8; ++i) {
            narrow.x[2] = (data >> (8 * i)) & 0xff;
            FX_REQUIRE(oracle_step(narrow, 0x1ac24108u));   // crc32b w8, w8, w2
        }
        FX_CHECK_EQ(wide.x[8], narrow.x[8]);
        FX_CHECK(wide.x[8] <= 0xffffffffu);
        // crc32cx w9, w10, x11 == host crc32 (Castagnoli)
        auto castagnoli = ctx;
        FX_REQUIRE(oracle_step(castagnoli, 0x9acb5d49u));
        const auto host = _mm_crc32_u64(ctx.x[10] & 0xffffffffu, ctx.x[11]);
        FX_CHECK_EQ(castagnoli.x[9], host);
    }
    // crc32x with sf = 0 is unallocated.
    auto ctx = random_context(51);
    FX_CHECK(!oracle_step(ctx, 0x1acd4d08u));
}
