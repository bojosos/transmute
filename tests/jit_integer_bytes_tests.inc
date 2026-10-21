// Included after native_region_same_state in test_main.cpp.

transmute::CpuState integer_bytes_state(std::uint64_t pc, std::uint64_t seed) {
    transmute::CpuState state{};
    state.pc = pc;
    state.sp = 0x00007000000ff000ULL;
    state.nzcv = 0xb0000000U;
    state.fpcr = 0x02400000U;
    state.fpsr = 0x08000091U;
    for (std::size_t index = 0; index < state.x.size(); ++index)
        state.x[index] = seed ^ (0x9e3779b97f4a7c15ULL * (index + 1));
    for (std::size_t index = 0; index < state.v.size(); ++index)
        state.v[index] = {seed + index, ~seed - index};
    return state;
}

std::uint64_t integer_bytes_reverse_oracle(std::uint64_t input,
                                           unsigned bytes, unsigned group) {
    std::uint64_t result = 0;
    for (unsigned output = 0; output < bytes; ++output) {
        const auto source = (output / group) * group + group - 1 - output % group;
        result |= ((input >> (source * 8)) & 255U) << (output * 8);
    }
    return result;
}

std::uint64_t integer_bytes_extract_oracle(std::uint64_t high, std::uint64_t low,
                                           unsigned width, unsigned shift) {
    std::uint64_t result = 0;
    for (unsigned output = 0; output < width; ++output) {
        const auto source = output + shift;
        const auto bit = source < width ? (low >> source) & 1U
                                       : (high >> (source - width)) & 1U;
        result |= bit << output;
    }
    return result;
}

void test_jit_integer_reverse_matrix() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    struct Form { std::uint32_t encoding; unsigned bytes, group; };
    constexpr std::array<Form, 5> forms{{
        {0x5ac00400U, 4, 2}, {0xdac00400U, 8, 2},
        {0x5ac00800U, 4, 4}, {0xdac00800U, 8, 4}, {0xdac00c00U, 8, 8}}};
    constexpr std::array<std::array<unsigned, 2>, 5> registers{{
        {0, 1}, {1, 1}, {31, 1}, {0, 31}, {30, 29}}};
    constexpr std::array<std::uint64_t, 5> inputs{
        0, UINT64_MAX, 0x0123456789abcdefULL,
        0x8000000180000000ULL, 0xfedcba9876543210ULL};
    constexpr std::uint64_t pc = 0x100000;
    transmute::X64BlockCache cache;
    for (const auto& form : forms) for (const auto& regs : registers)
        for (const auto input : inputs) {
            auto state = integer_bytes_state(pc, input);
            if (regs[1] != 31) state.x[regs[1]] = input;
            const auto initial = state;
            auto expected = state;
            if (regs[0] != 31)
                expected.x[regs[0]] = integer_bytes_reverse_oracle(
                    regs[1] == 31 ? 0 : input, form.bytes, form.group);
            expected.pc += 4;
            const std::array words{form.encoding | (regs[1] << 5) | regs[0]};
            const auto decoded = transmute::decode_native_read_region(pc, words);
            expect(decoded && decoded->instructions[0].op == transmute::BlockOp::reverse_bytes &&
                       decoded->instructions[0].field_width == form.group,
                   "legal REV form did not retain its exact byte grouping");
            expect(cache.execute(state, pc, words, 17) &&
                       native_region_same_state(state, expected),
                   "cold REV byte oracle, register alias or W zero extension differs");
            // Cold compilation's C++ duration accounting can set host IX.
            // The warmed native body must preserve a fresh, controlled CSR.
            state = initial;
            const auto saved_csr = _mm_getcsr(), csr = saved_csr & ~0x3fU;
            _mm_setcsr(csr);
            const auto completed = cache.execute_cached(state, pc, 17, 1);
            const bool host_retained = _mm_getcsr() == csr;
            _mm_setcsr(saved_csr);
            expect(completed == 1 && native_region_same_state(state, expected) && host_retained,
                   "REV byte oracle, register alias, W zero extension or retained flags differs");
        }
    expect(integer_bytes_reverse_oracle(0x0123456789abcdefULL, 8, 2) ==
               0x23016745ab89efcdULL &&
           integer_bytes_reverse_oracle(0x0123456789abcdefULL, 8, 4) ==
               0x67452301efcdab89ULL &&
           integer_bytes_reverse_oracle(0x0123456789abcdefULL, 8, 8) ==
               0xefcdab8967452301ULL,
           "REV independent fixed-pattern oracle changed");
#endif
}

void test_jit_integer_extract_matrix() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // Rd, Rn/high, Rm/low: independent, each destination alias, rotate,
    // discarded destination, and both zero-register operand positions.
    constexpr std::array<std::array<unsigned, 3>, 9> registers{{
        {0, 1, 2}, {1, 1, 2}, {2, 1, 2}, {1, 1, 1}, {31, 1, 2},
        {0, 31, 2}, {0, 1, 31}, {0, 31, 31}, {29, 30, 29}}};
    constexpr std::array<std::uint64_t, 4> inputs{
        0, UINT64_MAX, 0x0123456789abcdefULL, 0x8000000180000000ULL};
    constexpr std::uint64_t pc = 0x110000;
    transmute::X64BlockCache cache;
    for (const auto width : {32U, 64U}) for (unsigned shift = 0; shift < width; ++shift)
        for (const auto& regs : registers) for (const auto input : inputs) {
            auto state = integer_bytes_state(pc, input);
            if (regs[1] != 31) state.x[regs[1]] = input;
            if (regs[2] != 31) state.x[regs[2]] = ~input;
            const auto initial = state;
            auto expected = state;
            if (regs[0] != 31)
                expected.x[regs[0]] = integer_bytes_extract_oracle(
                    regs[1] == 31 ? 0 : state.x[regs[1]],
                    regs[2] == 31 ? 0 : state.x[regs[2]], width, shift);
            expected.pc += 4;
            const std::array words{(width == 64 ? 0x93c00000U : 0x13800000U) |
                (regs[2] << 16) | (shift << 10) | (regs[1] << 5) | regs[0]};
            const auto decoded = transmute::decode_native_read_region(pc, words);
            expect(decoded && decoded->instructions[0].op == transmute::BlockOp::extract_registers &&
                       decoded->instructions[0].shift == shift,
                   "legal EXTR form did not retain its complete immediate range");
            expect(cache.execute(state, pc, words, 19) &&
                       native_region_same_state(state, expected),
                   "cold EXTR CPU mismatch width=" + std::to_string(width) +
                   " shift=" + std::to_string(shift) + " Rd=" + std::to_string(regs[0]) +
                   " Rn=" + std::to_string(regs[1]) + " Rm=" + std::to_string(regs[2]) +
                   " input=" + std::to_string(input));
            state = initial;
            const auto saved_csr = _mm_getcsr(), csr = saved_csr & ~0x3fU;
            _mm_setcsr(csr);
            const auto completed = cache.execute_cached(state, pc, 19, 1);
            const auto actual_csr = _mm_getcsr();
            _mm_setcsr(saved_csr);
            expect(completed == 1 && native_region_same_state(state, expected) && actual_csr == csr,
                   "EXTR mismatch width=" + std::to_string(width) +
                   " shift=" + std::to_string(shift) + " Rd=" + std::to_string(regs[0]) +
                   " Rn=" + std::to_string(regs[1]) + " Rm=" + std::to_string(regs[2]) +
                   " input=" + std::to_string(input) + " opcode=" + std::to_string(words[0]) +
                   " actual=" + std::to_string(regs[0] == 31 ? 0 : state.x[regs[0]]) +
                   " expected=" + std::to_string(regs[0] == 31 ? 0 : expected.x[regs[0]]) +
                   " PC=" + std::to_string(state.pc) + " expectedPC=" + std::to_string(expected.pc) +
                   " CSR=" + std::to_string(actual_csr) + " expectedCSR=" + std::to_string(csr));
        }
    expect(integer_bytes_extract_oracle(0x0123456789abcdefULL,
               0xfedcba9876543210ULL, 64, 32) == 0x89abcdeffedcba98ULL &&
           integer_bytes_extract_oracle(0x01234567, 0x89abcdef, 32, 31) == 0x02468acf,
           "EXTR independent concatenated-bit oracle changed");
#endif
}

void test_jit_integer_bytes_reserved_encodings() {
    constexpr std::uint64_t pc = 0x120000;
    constexpr std::array<std::uint32_t, 9> reserved{
        0x5ac00c00U, // No W form of the 64-bit byte group.
        0x5ac01c00U, 0xdac01c00U, // Reserved one-source operations.
        0x13c00000U, 0x93800000U, // EXTR N must match sf.
        0x13808000U, 0x1380fc00U, // W shifts 32/63.
        0x93e00000U, 0x33800000U}; // Fixed EXTR bit21/op21.
    for (const auto opcode : reserved) {
        const std::array only{opcode};
        const std::array prefix{0xd503201fU, opcode, 0xd65f03c0U};
        expect(!transmute::decode_integer_block(pc, only) &&
                   !transmute::decode_native_read_region(pc, only),
               "reserved REV/EXTR encoding was admitted as a native integer operation");
        const auto leaf = transmute::decode_integer_block(pc, prefix);
        const auto region = transmute::decode_native_read_region(pc, prefix);
        expect(leaf && region && leaf->words.size() == 1 && region->words.size() == 1,
               "reserved REV/EXTR encoding consumed or discarded a valid decoded prefix");
    }
}

void test_jit_integer_bytes_chain_budgets_and_fault() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t data = 0x50bf00000ULL;
    std::vector<std::uint32_t> words(68, 0xd503201fU);
    words[0] = 0xdac00c84U; // rev x4,x4
    words[1] = 0x93cb8168U; // extr x8,x11,x11,#32
    words[2] = 0x1400003eU; // b entry+256, outside the first candidate extent
    words[64] = 0xf940000aU; // ldr x10,[x0]
    words[65] = 0xdac00841U; // rev32 x1,x2
    words[66] = 0x13847c65U; // extr w5,w3,w4,#31
    words[67] = 0xd65f03c0U;
    const auto prepare = [&](transmute::ReferenceEngine& engine) {
        engine.map_memory(data, 8);
        engine.write64(data, 0x1234);
        engine.set_initial_register(0, data);
        engine.set_initial_register(2, 0x0123456789abcdefULL);
        engine.set_initial_register(3, 0xfedcba9876543210ULL);
        engine.set_initial_register(4, 0x0123456789abcdefULL);
        engine.set_initial_register(10, 0xfeed);
        engine.set_initial_register(11, 0xfedcba9876543210ULL);
    };
    for (unsigned budget = 1; budget <= 7; ++budget) {
        auto ri = transmute::PeImage::parse(executable(words));
        auto ni = transmute::PeImage::parse(executable(words));
        transmute::ReferenceEngine reference(ri), native(ni);
        prepare(reference); prepare(native);
        native.set_jit_enabled(true);
        static_cast<void>(native.invoke_callback(ni.entry(), data));
        const auto owners = native.jit_stats().compiled_native_regions;
        const auto csr = _mm_getcsr();
        expect(owners == 2, "REV/EXTR trace did not warm two bounded native owners");
        if (budget < 7) {
            for (auto* engine : {&reference, &native})
                expect_failure([&] { static_cast<void>(engine->run(budget)); },
                               "guest exceeded instruction limit");
            expect(native_region_same_state(native.state(), reference.state()) &&
                       _mm_getcsr() == csr,
                   "REV/EXTR cross-owner short budget changed its exact completed prefix");
        }
        const auto expected = reference.run(), actual = native.run();
        // Budget 3 ends exactly on the external branch; its resumed invocation
        // starts at the second owner and therefore needs no native chain edge.
        expect(actual.instructions == expected.instructions &&
                   native_region_same_state(native.state(), reference.state()) &&
                   native.jit_stats().compiled_native_regions == owners &&
                   (budget == 3 || native.jit_stats().native_chain_edges != 0) &&
                   _mm_getcsr() == csr,
               "REV/EXTR native aliases recompiled, repeated a prefix or lost a chained edge");
    }
    auto ri = transmute::PeImage::parse(executable(words));
    auto ni = transmute::PeImage::parse(executable(words));
    transmute::ReferenceEngine reference(ri), native(ni);
    prepare(reference); prepare(native);
    native.set_jit_enabled(true);
    static_cast<void>(native.invoke_callback(ni.entry(), data));
    const auto csr = _mm_getcsr();
    for (auto* engine : {&reference, &native}) {
        engine->unmap_memory(data);
        expect_failure([&] { static_cast<void>(engine->run()); },
                       "guest read outside mapped memory");
    }
    expect(native_region_same_state(native.state(), reference.state()) &&
               native.state().pc == ni.entry() + 256 && native.state().x[10] == 0xfeed &&
               native.state().x[4] == 0xefcdab8967452301ULL && _mm_getcsr() == csr,
           "REV/EXTR chain fault lost its prefix or changed the faulting destination");
    for (auto* engine : {&reference, &native}) {
        engine->map_memory(data, 8);
        engine->write64(data, 0x1234);
    }
    const auto expected = reference.run(), actual = native.run();
    expect(actual.instructions == expected.instructions &&
               native_region_same_state(native.state(), reference.state()) &&
               native.state().x[10] == 0x1234 && _mm_getcsr() == csr,
           "REV/EXTR cross-owner memory fault did not resume at the exact next instruction");
#endif
}
