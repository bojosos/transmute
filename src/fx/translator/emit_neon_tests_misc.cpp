// describe(), multi-instruction regions (shared constant pool), random
// straight-line sequences and register-usage reporting (owner: neon).
#include "fx/translator/emit_neon_test_util.hpp"

#include <cstdio>

using namespace transmute::fx;
using namespace transmute::fx::neon_test;

namespace {

InstructionUse describe_word(bool memory, std::uint32_t word, bool& ok) {
    InstructionUse use{};
    ok = memory ? describe_vector_memory(word, use) : describe_neon(word, use);
    return use;
}

constexpr std::uint32_t bits_of(std::initializer_list<unsigned> regs) {
    std::uint32_t m = 0;
    for (auto r : regs) m |= 1u << r;
    return m;
}

// Data processing first, then loads/stores (as the translator would try them).
unsigned emit_neon_or_memory(EmitContext& ctx, std::span<const std::uint32_t> words) {
    const unsigned n = emit_neon(ctx, words);
    return n ? n : emit_vector_memory(ctx, words);
}

Buffer& seq_buffer() {
    static Buffer b(64 * 1024);
    return b;
}

} // namespace

FX_TEST(neon_describe) {
    bool ok = false;
    auto use = describe_word(false, 0x4ea28420u, ok);   // add v0.4s, v1.4s, v2.4s
    FX_CHECK(ok);
    FX_CHECK_EQ(use.vec_read, bits_of({1, 2}));
    FX_CHECK_EQ(use.vec_write, bits_of({0}));
    FX_CHECK_EQ(use.gpr_read, 0u);
    FX_CHECK(!use.may_fault && !use.reads_flags && !use.writes_flags);

    use = describe_word(false, 0x6e621c20u, ok);   // bsl v0.16b, v1.16b, v2.16b
    FX_CHECK(ok);
    FX_CHECK_EQ(use.vec_read, bits_of({0, 1, 2}));

    use = describe_word(false, 0x0e0c3c23u, ok);   // umov w3, v1.s[1]
    FX_CHECK(ok);
    FX_CHECK_EQ(use.vec_read, bits_of({1}));
    FX_CHECK_EQ(use.gpr_write, bits_of({3}));
    FX_CHECK_EQ(use.vec_write, 0u);

    use = describe_word(false, 0x4e040ca0u, ok);   // dup v0.4s, w5
    FX_CHECK(ok);
    FX_CHECK_EQ(use.gpr_read, bits_of({5}));
    FX_CHECK_EQ(use.vec_write, bits_of({0}));

    use = describe_word(false, 0x4e0223e0u, ok);   // tbl v0.16b, {v31.16b, v0.16b}, v2.16b
    FX_CHECK(ok);
    FX_CHECK_EQ(use.vec_read, bits_of({31, 0, 2}));

    use = describe_word(true, 0xacc10440u, ok);    // ldp q0, q1, [x2], #32
    FX_CHECK(ok);
    FX_CHECK_EQ(use.gpr_read, bits_of({2}));
    FX_CHECK_EQ(use.gpr_write, bits_of({2}));
    FX_CHECK_EQ(use.vec_write, bits_of({0, 1}));
    FX_CHECK(use.may_fault);

    use = describe_word(true, 0x4c0003feu, ok);    // st4 {v30.16b-v1.16b}, [sp]
    FX_CHECK(ok);
    FX_CHECK_EQ(use.vec_read, bits_of({30, 31, 0, 1}));
    FX_CHECK_EQ(use.gpr_read, bits_of({31}));
    FX_CHECK_EQ(use.gpr_write, 0u);
    FX_CHECK(use.may_fault);

    use = describe_word(true, 0x0dc49060u, ok);    // ld1 {v0.s}[1], [x3], x4
    FX_CHECK(ok);
    FX_CHECK_EQ(use.vec_read, bits_of({0}));
    FX_CHECK_EQ(use.vec_write, bits_of({0}));
    FX_CHECK_EQ(use.gpr_read, bits_of({3, 4}));
    FX_CHECK_EQ(use.gpr_write, bits_of({3}));

    // Floating point belongs to the fp family; loads with V = 0 to the core.
    describe_word(false, 0x4e22d420u, ok);   // fadd v0.4s, v1.4s, v2.4s
    FX_CHECK(!ok);
    describe_word(true, 0xf9400c62u, ok);    // ldr x2, [x3, #24]
    FX_CHECK(!ok);
    describe_word(false, 0x8b020020u, ok);   // add x0, x1, x2
    FX_CHECK(!ok);
}

FX_TEST(neon_sequence_movemask_idiom) {
    // sse2neon _mm_movemask_epi8 (shift/accumulate form).
    const std::uint32_t words[] = {0x6f090401u, 0x6f191421u, 0x6f321421u, 0x6f641421u, 0x0e013c20u, 0x0e113c21u};
    for (std::uint64_t seed = 0; seed < 16; ++seed) {
        const auto ctx = special_context(seed, 0);
        if (!check_sequence(emit_neon, ctx, words, "movemask")) break;
    }
}

FX_TEST(neon_sequence_dot_kernel) {
    // ld1 {v0.4s, v1.4s}, [x0], #32; ld1 {v2.4s, v3.4s}, [x1], #32;
    // mla v4.4s, v0.4s, v2.4s; mla v4.4s, v1.4s, v3.4s; addv s5, v4.4s; umov w2, v5.s[0]
    const std::uint32_t loads[] = {0x4cdfa800u, 0x4cdfa822u};
    const std::uint32_t math[] = {0x4ea29404u, 0x4ea39424u, 0x4eb1b885u, 0x0e043ca2u};
    auto& buffer = seq_buffer();
    std::uint64_t state = 77;
    for (std::size_t i = 0; i < buffer.size() / 8; ++i) reinterpret_cast<std::uint64_t*>(buffer.data())[i] = rnd(state);
    for (std::uint64_t seed = 0; seed < 8; ++seed) {
        auto ctx = special_context(seed, 2);
        ctx.x[0] = buffer.address() + 1024 + seed * 48;
        ctx.x[1] = buffer.address() + 8192 + seed * 16;
        std::vector<std::uint32_t> words(std::begin(loads), std::end(loads));
        for (unsigned r = 0; r < 3; ++r) words.insert(words.end(), std::begin(math), std::end(math));
        if (!check_sequence(emit_neon_or_memory, ctx, words, "dot_kernel", {buffer.data(), buffer.size()})) break;
    }
}

namespace {

// Random valid data-processing words: templates with random fields, kept when
// both the oracle and the family accept them.
std::uint32_t random_word(std::uint64_t& state) {
    static constexpr struct { std::uint32_t mask, value; } groups[] = {
        {0x9f200400u, 0x0e200400u},   // three same
        {0x9f200c00u, 0x0e200000u},   // three different
        {0x9f3e0c00u, 0x0e200800u},   // two-register misc
        {0x9f3e0c00u, 0x0e300800u},   // across lanes
        {0x9fe08400u, 0x0e000400u},   // copy
        {0xbfe08c00u, 0x0e000000u},   // tbl/tbx
        {0xbf208c00u, 0x0e000800u},   // permute
        {0xbfe08400u, 0x2e000000u},   // ext
        {0x9ff80400u, 0x0f000400u},   // modified immediate
        {0x9f800400u, 0x0f000400u},   // shift by immediate
        {0x9f000400u, 0x0f000000u},   // by element
        {0xdf200400u, 0x5e200400u},   // scalar three same
        {0xdf800400u, 0x5f000400u},   // scalar shift by immediate
    };
    for (;;) {
        const auto& g = groups[rnd(state) % std::size(groups)];
        std::uint32_t w = (static_cast<std::uint32_t>(rnd(state)) & ~g.mask) | g.value;
        // Keep Vd/Vn within v0..v7 so the instructions interact.
        w = (w & ~0x3ffu) | static_cast<std::uint32_t>(rnd(state) & 7) |
            (static_cast<std::uint32_t>(rnd(state) & 7) << 5);
        GuestContext probe = testing::random_context(1);
        if (!testing::oracle_step(probe, w)) continue;
        x64::Assembler a;
        testing::MemoryEmitContext emit(a, probe.pc);
        emit.begin_instruction(probe.pc);
        if (emit_neon(emit, std::span<const std::uint32_t>(&w, 1)) == 0) continue;
        return w;
    }
}

} // namespace

FX_TEST(neon_random_sequences) {
    std::uint64_t state = 2024;
    for (unsigned iteration = 0; iteration < 60; ++iteration) {
        std::vector<std::uint32_t> words;
        for (unsigned i = 0; i < 12; ++i) words.push_back(random_word(state));
        const auto ctx = special_context(rnd(state), static_cast<unsigned>(rnd(state) % 4));
        if (!check_sequence(emit_neon, ctx, words, "random_sequence " + std::to_string(iteration))) break;
    }
}

FX_TEST(neon_zz_register_usage) {
    std::printf("[neon] heaviest instructions (host XMM registers incl. operands / temps):\n");
    for (const auto& u : heaviest_instructions()) std::printf("  0x%08x  xmm=%u temps=%u\n", u.word, u.xmm, u.temps);
}
