// Differential tests: SIMD&FP loads and stores against host memory, every
// addressing and writeback form, SP base, structures and lanes (owner: neon).
#include "fx/translator/emit_neon_test_util.hpp"

#include <cstdio>
#include <cstring>

using namespace transmute::fx;
using namespace transmute::fx::neon_test;

namespace {

constexpr std::size_t kBufferSize = 256 * 1024;
constexpr std::size_t kBaseOffset = 96 * 1024;

Buffer& buffer() {
    static Buffer b(kBufferSize);
    return b;
}

void fill_buffer(std::uint64_t seed) {
    std::uint64_t state = seed;
    auto* p = reinterpret_cast<std::uint64_t*>(buffer().data());
    for (std::size_t i = 0; i < kBufferSize / 8; ++i) p[i] = rnd(state);
}

MemoryRange whole() { return {buffer().data(), kBufferSize}; }

void report_declined(const char* label, std::uint32_t word, Outcome outcome) {
    char text[112];
    std::snprintf(text, sizeof(text), "%s 0x%08x: valid encoding declined (oracle %s)", label, word,
                  outcome == Outcome::declined ? "implements it" : "declines too");
    testing::report_failure(__FILE__, __LINE__, text);
}

// Context whose Xn (or SP for 31) points into the buffer.
GuestContext memory_context(std::uint64_t seed, unsigned rn, std::int64_t bias = 0) {
    auto ctx = special_context(seed, static_cast<unsigned>(seed % 4));
    ctx.x[rn] = buffer().address() + kBaseOffset + static_cast<std::uint64_t>(bias);
    return ctx;
}

bool run_one(std::uint32_t word, const GuestContext& ctx, const char* label, bool expect_valid = true) {
    const auto outcome = check(emit_vector_memory, ctx, word, label, whole());
    if (outcome == Outcome::declined || outcome == Outcome::both_declined) {
        if (expect_valid) report_declined(label, word, outcome);
        return false;
    }
    return outcome == Outcome::ok;
}

// (size, opc) pairs of the single-register forms: B H S D Q, load and store.
struct Access { unsigned size, opc, bytes; };
constexpr Access kAccesses[] = {
    {0, 0, 1}, {0, 1, 1}, {1, 0, 2}, {1, 1, 2}, {2, 0, 4}, {2, 1, 4}, {3, 0, 8}, {3, 1, 8}, {0, 2, 16}, {0, 3, 16},
};

} // namespace

FX_TEST(vector_ldr_str_unsigned_offset) {
    fill_buffer(1);
    std::uint64_t state = 42;
    for (const auto acc : kAccesses)
        for (unsigned s = 0; s < 24; ++s) {
            const unsigned rt = static_cast<unsigned>(rnd(state) % 32);
            const unsigned rn = s % 4 == 0 ? 31 : static_cast<unsigned>(rnd(state) % 31);
            const unsigned imm12 = s % 5 == 0 ? 4095 : static_cast<unsigned>(rnd(state) % 512);
            const std::uint32_t word = (acc.size << 30) | 0x3d000000u | (acc.opc << 22) | (imm12 << 10) | (rn << 5) | rt;
            if (!run_one(word, memory_context(rnd(state), rn), "ldr_str_uimm")) break;
        }
    print_stats("after ldr_str_uimm");
}

FX_TEST(vector_ldr_str_unscaled_pre_post) {
    fill_buffer(2);
    std::uint64_t state = 43;
    for (const auto acc : kAccesses)
        for (const unsigned mode : {0u, 1u, 3u})
            for (unsigned s = 0; s < 16; ++s) {
                const unsigned rt = static_cast<unsigned>(rnd(state) % 32);
                const unsigned rn = s % 4 == 0 ? 31 : static_cast<unsigned>(rnd(state) % 31);
                const unsigned imm9 = static_cast<unsigned>(rnd(state) % 512);
                const std::uint32_t word = (acc.size << 30) | 0x3c000000u | (acc.opc << 22) | (imm9 << 12) |
                                           (mode << 10) | (rn << 5) | rt;
                if (!run_one(word, memory_context(rnd(state), rn), "ldr_str_imm9")) break;
            }
    print_stats("after ldr_str_imm9");
}

FX_TEST(vector_ldr_str_register_offset) {
    fill_buffer(3);
    std::uint64_t state = 44;
    for (const auto acc : kAccesses)
        for (const unsigned option : {2u, 3u, 6u, 7u})
            for (unsigned sbit = 0; sbit < 2; ++sbit)
                for (unsigned s = 0; s < 8; ++s) {
                    const unsigned rt = static_cast<unsigned>(rnd(state) % 32);
                    const unsigned rn = s % 4 == 0 ? 31 : static_cast<unsigned>(rnd(state) % 31);
                    unsigned rm = static_cast<unsigned>(rnd(state) % 31);
                    if (rm == rn) rm = (rm + 1) % 31;
                    const std::uint32_t word = (acc.size << 30) | 0x3c200800u | (acc.opc << 22) | (rm << 16) |
                                               (option << 13) | (sbit << 12) | (rn << 5) | rt;
                    auto ctx = memory_context(rnd(state), rn);
                    const auto index = static_cast<std::int64_t>(rnd(state) % 2001) - 1000;
                    const auto junk_high = rnd(state) << 32;
                    switch (option) {
                    case 2: ctx.x[rm] = junk_high | static_cast<std::uint32_t>(index < 0 ? -index : index); break;
                    case 6: ctx.x[rm] = junk_high | static_cast<std::uint32_t>(index); break;
                    default: ctx.x[rm] = static_cast<std::uint64_t>(index); break;
                    }
                    if (!run_one(word, ctx, "ldr_str_reg")) break;
                }
    print_stats("after ldr_str_reg");
}

FX_TEST(vector_ldr_literal) {
    fill_buffer(4);
    std::uint64_t state = 45;
    for (unsigned opc = 0; opc < 3; ++opc)
        for (unsigned s = 0; s < 16; ++s) {
            const unsigned rt = static_cast<unsigned>(rnd(state) % 32);
            const auto imm19 = static_cast<std::uint32_t>(static_cast<std::int32_t>(rnd(state) % 4001) - 2000) & 0x7ffff;
            const std::uint32_t word = (opc << 30) | 0x1c000000u | (imm19 << 5) | rt;
            auto ctx = memory_context(rnd(state), 0);
            ctx.pc = buffer().address() + kBaseOffset;
            if (!run_one(word, ctx, "ldr_literal")) break;
        }
    print_stats("after ldr_literal");
}

FX_TEST(vector_ldp_stp) {
    fill_buffer(5);
    std::uint64_t state = 46;
    for (unsigned opc = 0; opc < 3; ++opc)
        for (unsigned mode = 0; mode < 4; ++mode)
            for (unsigned l = 0; l < 2; ++l)
                for (unsigned s = 0; s < 16; ++s) {
                    const unsigned rt = static_cast<unsigned>(rnd(state) % 32);
                    unsigned rt2 = static_cast<unsigned>(rnd(state) % 32);
                    if (l && rt2 == rt) rt2 = (rt + 1) % 32;   // LDP Rt == Rt2 is UNPREDICTABLE
                    const unsigned rn = s % 4 == 0 ? 31 : static_cast<unsigned>(rnd(state) % 31);
                    const unsigned imm7 = static_cast<unsigned>(rnd(state) % 128);
                    const std::uint32_t word = (opc << 30) | 0x2c000000u | (mode << 23) | (l << 22) | (imm7 << 15) |
                                               (rt2 << 10) | (rn << 5) | rt;
                    if (!run_one(word, memory_context(rnd(state), rn), "ldp_stp")) break;
                }
    print_stats("after ldp_stp");
}

FX_TEST(vector_ld_st_multiple) {
    fill_buffer(6);
    std::uint64_t state = 47;
    for (const unsigned opcode : {0x0u, 0x2u, 0x4u, 0x6u, 0x7u, 0x8u, 0xau})
        for (unsigned l = 0; l < 2; ++l)
            for (unsigned size = 0; size < 4; ++size)
                for (unsigned q = 0; q < 2; ++q)
                    for (unsigned post = 0; post < 3; ++post) {   // 0 none, 1 imm, 2 register
                        const bool structured = opcode == 0x0 || opcode == 0x4 || opcode == 0x8;
                        const bool valid = !(structured && size == 3 && !q);
                        for (unsigned s = 0; s < 6; ++s) {
                            const unsigned rt = s == 0 ? 30 : static_cast<unsigned>(rnd(state) % 32);
                            const unsigned rn = s % 3 == 0 ? 31 : static_cast<unsigned>(rnd(state) % 31);
                            unsigned rm = post == 1 ? 31 : static_cast<unsigned>(rnd(state) % 31);
                            if (post == 2 && rm == rn) rm = (rm + 1) % 31;
                            std::uint32_t word = (q << 30) | 0x0c000000u | (l << 22) | (opcode << 12) | (size << 10) |
                                                 (rn << 5) | rt;
                            if (post) word |= 0x00800000u | (rm << 16);
                            auto ctx = memory_context(rnd(state), rn);
                            if (post == 2) ctx.x[rm] = static_cast<std::uint64_t>(static_cast<std::int64_t>(rnd(state) % 257) - 128);
                            if (!run_one(word, ctx, "ld_st_multiple", valid)) break;
                        }
                    }
    print_stats("after ld_st_multiple");
}

FX_TEST(vector_ld_st_single_and_replicate) {
    fill_buffer(7);
    std::uint64_t state = 48;
    for (unsigned l = 0; l < 2; ++l)
        for (unsigned r = 0; r < 2; ++r)
            for (unsigned opcode = 0; opcode < 8; ++opcode)
                for (unsigned sbit = 0; sbit < 2; ++sbit)
                    for (unsigned size = 0; size < 4; ++size)
                        for (unsigned q = 0; q < 2; ++q)
                            for (unsigned post = 0; post < 3; ++post) {
                                bool valid;
                                if (opcode >= 6) valid = l && !sbit;
                                else if ((opcode >> 1) == 0) valid = true;
                                else if ((opcode >> 1) == 1) valid = (size & 1) == 0;
                                else valid = size == 0 || (size == 1 && !sbit);
                                for (unsigned s = 0; s < 3; ++s) {
                                    const unsigned rt = s == 0 ? 31 : static_cast<unsigned>(rnd(state) % 32);
                                    const unsigned rn = s == 1 ? 31 : static_cast<unsigned>(rnd(state) % 31);
                                    unsigned rm = post == 1 ? 31 : static_cast<unsigned>(rnd(state) % 31);
                                    if (post == 2 && rm == rn) rm = (rm + 1) % 31;
                                    std::uint32_t word = (q << 30) | 0x0d000000u | (l << 22) | (r << 21) |
                                                         (opcode << 13) | (sbit << 12) | (size << 10) | (rn << 5) | rt;
                                    if (post) word |= 0x00800000u | (rm << 16);
                                    auto ctx = memory_context(rnd(state), rn);
                                    if (post == 2)
                                        ctx.x[rm] = static_cast<std::uint64_t>(static_cast<std::int64_t>(rnd(state) % 257) - 128);
                                    if (!run_one(word, ctx, "ld_st_single", valid)) break;
                                }
                            }
    print_stats("after ld_st_single");
}

// LD2/LD3/LD4 against explicitly interleaved data, then ST2/ST3/ST4 back.
FX_TEST(vector_structure_round_trip) {
    for (unsigned count = 2; count <= 4; ++count)
        for (unsigned size = 0; size < 4; ++size)
            for (unsigned q = 0; q < 2; ++q) {
                if (size == 3 && !q) continue;
                const unsigned esize = 1u << size, lanes = (q ? 16u : 8u) / esize;
                auto* mem = buffer().data() + kBaseOffset;
                // element e of structure i holds (e << 6) | i (truncated to the element).
                for (unsigned i = 0; i < lanes; ++i)
                    for (unsigned e = 0; e < count; ++e) {
                        const std::uint64_t value = 0x0101010101010101ULL * ((e << 6) | i);
                        std::memcpy(mem + (i * count + e) * esize, &value, esize);
                    }
                const unsigned opcode = count == 2 ? 0x8 : count == 3 ? 0x4 : 0x0;
                const std::uint32_t ld = (q << 30) | 0x0c400000u | (opcode << 12) | (size << 10) | (1u << 5) | 3u;
                auto ctx = memory_context(9, 1);
                GuestContext loaded = ctx;
                FX_REQUIRE(run(loaded, emit_vector_memory, std::span<const std::uint32_t>(&ld, 1)));
                for (unsigned e = 0; e < count; ++e)
                    for (unsigned i = 0; i < lanes; ++i) {
                        std::uint64_t got = 0;
                        std::memcpy(&got, reinterpret_cast<const std::uint8_t*>(loaded.v[3 + e]) + i * esize, esize);
                        const std::uint64_t mask = esize == 8 ? ~0ULL : (1ULL << (8 * esize)) - 1;
                        FX_CHECK_EQ(got, (0x0101010101010101ULL * ((e << 6) | i)) & mask);
                    }
                // Store back to a different place and compare bytes.
                const std::uint32_t st = (q << 30) | 0x0c000000u | (opcode << 12) | (size << 10) | (2u << 5) | 3u;
                loaded.x[2] = buffer().address() + kBaseOffset + 4096;
                loaded.pc = ctx.pc;
                FX_REQUIRE(run(loaded, emit_vector_memory, std::span<const std::uint32_t>(&st, 1)));
                FX_CHECK(std::memcmp(mem, mem + 4096, count * lanes * esize) == 0);
            }
}
