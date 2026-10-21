// Included after the existing JIT SIMD/FP helpers inside the test namespace.

void bit_move_differential_case(transmute::X64BlockCache& cache,
                                std::uint32_t opcode, unsigned flags = 0,
                                unsigned sample = 0) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    auto image = transmute::PeImage::parse(executable({
        0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcode, 0xd65f03c0U}));
    transmute::ReferenceEngine oracle(image);
    for (unsigned reg = 0; reg < 30; ++reg)
        oracle.set_initial_register(reg, 0x123456789abcdef0ULL ^ (reg * 0x1030405ULL));
    oracle.set_initial_register(20, static_cast<std::uint64_t>(flags) << 28);
    oracle.set_initial_register(21, 0x07c0ff00U);
    oracle.set_initial_register(22, 0xa800009fU);
    for (unsigned reg = 0; reg < 32; ++reg) {
        const auto low = sample == 0 ? 0x0123456789abcdefULL ^ (reg * 0x112233ULL)
            : sample == 1 ? 0x7ff000007f800001ULL : UINT64_MAX;
        oracle.set_initial_vector(reg, {low, 0xfedcba9876543210ULL ^ (reg * 0x445566ULL)});
    }
    auto input = oracle.state();
    input.pc = image.entry() + 12;
    input.nzcv = flags << 28;
    input.fpcr = static_cast<std::uint32_t>(input.x[21]);
    input.fpsr = static_cast<std::uint32_t>(input.x[22]);
    static_cast<void>(oracle.run());
    const auto expected = oracle.state();
    GuardedFpHostState restore;
    const auto host = (_mm_getcsr() & ~0x6000U) | 0x6000U | 0x21U;
    _mm_setcsr(host);
    const std::array words{opcode, 0xd65f03c0U};
    auto actual = input;
    expect(cache.execute(actual, input.pc, words) && integer_simd_same_state(actual, expected),
           "native bit movement disagrees with interpreter bits, aliases or flags");
    expect(_mm_getcsr() == host, "bit movement changed host FP control or status");
    auto short_budget = input;
    expect(cache.execute_cached(short_budget, input.pc, 0, 1) == 0 &&
               integer_simd_same_state(short_budget, input),
           "legacy bit movement crossed its complete-block budget");
    auto warmed = input;
    expect(cache.execute_cached(warmed, input.pc, 0, 2) == 2 &&
               integer_simd_same_state(warmed, expected) && _mm_getcsr() == host,
           "cached bit movement changed semantics or FP state");
#else
    static_cast<void>(cache); static_cast<void>(opcode);
    static_cast<void>(flags); static_cast<void>(sample);
#endif
}

void test_jit_bit_move_predicate() {
    for (const auto word : {0x4e010c20U, 0x2f00e410U, 0x1e200e00U,
                            0x9e6703e0U, 0x1e26003fU, 0x5e1f0420U}) {
        expect(transmute::jit_detail::accepts_bit_moves(word),
               "native bit movement rejected a sampled or valid encoding");
    }
    for (const auto word : {0x0e080c20U, 0x0e080420U, 0x4e000c20U,
                            0x4e000420U, 0x4e100420U, 0x4e220c20U,
                            0x1ea04020U, 0x1ee00c20U, 0x2f00f400U,
                            0xffffffffU}) {
        expect(!transmute::jit_detail::accepts_bit_moves(word),
               "native bit movement admitted a reserved or unsupported encoding");
        bool rejected = false;
        try { static_cast<void>(transmute::jit_detail::emit_bit_moves(word)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "native bit movement emitter accepted invalid input");
    }
}

void test_jit_bit_move_constants_and_duplicates() {
    transmute::X64BlockCache cache;
    for (const bool quad : {false, true}) {
        const auto q = quad ? 0x40000000U : 0U;
        for (unsigned bytes = 1; bytes <= 8; bytes *= 2) {
            if (bytes == 8 && !quad) continue;
            for (const unsigned rn : {0U, 1U, 31U})
                bit_move_differential_case(cache, 0x0e000c00U | q | (bytes << 16) | (rn << 5) | 1);
            for (unsigned index = 0; index < 16 / bytes; ++index) {
                const auto imm5 = bytes | (index * bytes * 2);
                for (const unsigned rd : {0U, 1U, 31U})
                    bit_move_differential_case(cache, 0x0e000400U | q | (imm5 << 16) | (1U << 5) | rd);
            }
        }
        for (unsigned cmode = 0; cmode < 16; ++cmode) {
            for (const bool op : {false, true}) {
                if (cmode == 15 && op && !quad) continue;
                for (const unsigned imm : {0U, 1U, 0x55U, 0x80U, 0xa5U, 0xffU}) {
                    const auto word = 0x0f000400U | q | (op ? 0x20000000U : 0) |
                        (cmode << 12) | ((imm >> 5) << 16) | ((imm & 31U) << 5) | 1;
                    bit_move_differential_case(cache, word, imm & 15U);
                }
            }
        }
    }
    for (unsigned bytes = 1; bytes <= 8; bytes *= 2) {
        for (unsigned index = 0; index < 16 / bytes; ++index) {
            const auto imm5 = bytes | (index * bytes * 2);
            bit_move_differential_case(cache, 0x5e000400U | (imm5 << 16) | (31U << 5) | 31);
        }
    }
    for (const bool wide : {false, true}) {
        for (unsigned imm = 0; imm < 256; ++imm)
            bit_move_differential_case(cache, 0x1e201000U | (wide ? 0x00400000U : 0) | (imm << 13) | 31);
    }
}

void test_jit_bit_move_selection_and_transfers() {
    transmute::X64BlockCache cache;
    for (const bool wide : {false, true}) {
        const auto type = wide ? 0x00400000U : 0U;
        for (unsigned condition = 0; condition < 16; ++condition) {
            for (unsigned flags = 0; flags < 16; ++flags) {
                for (const unsigned rd : {0U, 1U, 2U, 31U}) {
                    const auto word = 0x1e200c00U | type | (2U << 16) |
                        (condition << 12) | (1U << 5) | rd;
                    bit_move_differential_case(cache, word, flags, flags % 3);
                }
            }
        }
        for (const auto base : {0x1e204000U, 0x1e20c000U, 0x1e214000U})
            for (const unsigned reg : {0U, 1U, 31U})
                for (unsigned sample = 0; sample < 3; ++sample)
                    bit_move_differential_case(cache, base | type | (reg << 5) | reg, sample * 5, sample);
    }
    for (const auto base : {0x1e260000U, 0x9e660000U, 0x1e270000U, 0x9e670000U})
        for (const unsigned rn : {0U, 1U, 31U})
            for (const unsigned rd : {0U, 1U, 31U})
                bit_move_differential_case(cache, base | (rn << 5) | rd, 15, 1);
}

void test_jit_bit_move_runtime_budgets() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    const std::initializer_list<std::uint32_t> words{
        0x4e010c20U, // DUP V0.16B,W1
        0x2f00e410U, // MOVI D16,#0
        0x1e200e00U, // FCSEL S0,S16,S0,EQ
        0x9e660002U, // FMOV X2,D0
        0xd65f03c0U};
    auto image = transmute::PeImage::parse(executable(words));
    transmute::ReferenceEngine reference(image), optimized(image);
    reference.set_initial_register(1, 0x123456789abcdef0ULL);
    optimized.set_initial_register(1, 0x123456789abcdef0ULL);
    optimized.set_jit_enabled(true);
    static_cast<void>(reference.run());
    static_cast<void>(optimized.run());
    expect(integer_simd_same_state(reference.state(), optimized.state()) &&
               optimized.jit_executed_instructions() >= 4,
           "runtime failed to execute sampled bit-movement sequence natively");
    for (std::uint64_t limit = 1; limit <= 5; ++limit) {
        transmute::ReferenceEngine oracle(image), candidate(image);
        oracle.set_initial_register(1, 0xfedcba9876543210ULL);
        candidate.set_initial_register(1, 0xfedcba9876543210ULL);
        oracle.set_instruction_limit(limit); candidate.set_instruction_limit(limit);
        candidate.set_jit_enabled(true);
        bool first_failed = false, second_failed = false;
        try { static_cast<void>(oracle.run()); } catch (const std::runtime_error&) { first_failed = true; }
        try { static_cast<void>(candidate.run()); } catch (const std::runtime_error&) { second_failed = true; }
        expect(first_failed == second_failed && integer_simd_same_state(oracle.state(), candidate.state()),
               "bit-movement region committed beyond its precise instruction budget");
    }
#endif
}
