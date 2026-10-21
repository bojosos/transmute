// Included inside test_main.cpp's anonymous namespace after expect/executable.
// test_main.cpp includes ../src/jit_simd.hpp outside that namespace.

struct IntegerSimdTestForm {
    std::uint32_t base;
    bool unary;
    bool arranged;
};

constexpr std::array integer_simd_test_forms{
    IntegerSimdTestForm{0x0e201c00U, false, false}, // AND
    IntegerSimdTestForm{0x0e601c00U, false, false}, // BIC
    IntegerSimdTestForm{0x0ea01c00U, false, false}, // ORR
    IntegerSimdTestForm{0x0ee01c00U, false, false}, // ORN
    IntegerSimdTestForm{0x2e201c00U, false, false}, // EOR
    IntegerSimdTestForm{0x2e601c00U, false, false}, // BSL
    IntegerSimdTestForm{0x2ea01c00U, false, false}, // BIT
    IntegerSimdTestForm{0x2ee01c00U, false, false}, // BIF
    IntegerSimdTestForm{0x2e205800U, true, false},  // NOT
    IntegerSimdTestForm{0x0e208400U, false, true},  // ADD
    IntegerSimdTestForm{0x2e208400U, false, true},  // SUB
    IntegerSimdTestForm{0x2e20b800U, true, true},   // NEG
    IntegerSimdTestForm{0x2e208c00U, false, true},  // CMEQ register
    IntegerSimdTestForm{0x0e209800U, true, true},   // CMEQ zero
    IntegerSimdTestForm{0x0e208c00U, false, true},  // CMTST
};

std::array<std::uint64_t, 2> integer_simd_spec_result(
    IntegerSimdTestForm form, unsigned size, bool quad,
    const transmute::CpuState& input, unsigned rd, unsigned rn, unsigned rm) {
    std::array<std::uint64_t, 2> output{};
    const auto& left = input.v[rn];
    const auto& right = input.v[rm];
    const auto& prior = input.v[rd];
    if (!form.arranged) {
        for (unsigned half = 0; half != (quad ? 2U : 1U); ++half) {
            const auto a = left[half], b = right[half], d = prior[half];
            switch (form.base) {
            case 0x0e201c00U: output[half] = a & b; break;
            case 0x0e601c00U: output[half] = a & ~b; break;
            case 0x0ea01c00U: output[half] = a | b; break;
            case 0x0ee01c00U: output[half] = a | ~b; break;
            case 0x2e201c00U: output[half] = a ^ b; break;
            case 0x2e601c00U: output[half] = (d & a) | (~d & b); break;
            case 0x2ea01c00U: output[half] = (d & ~b) | (a & b); break;
            case 0x2ee01c00U: output[half] = (d & b) | (a & ~b); break;
            case 0x2e205800U: output[half] = ~a; break;
            default: throw std::logic_error("unknown integer SIMD test operation");
            }
        }
        return output;
    }
    const auto bits = 8U << size;
    const auto mask = bits == 64 ? UINT64_MAX : (1ULL << bits) - 1;
    for (unsigned bit = 0; bit != (quad ? 128U : 64U); bit += bits) {
        const auto shift = bit % 64;
        const auto a = (left[bit / 64] >> shift) & mask;
        const auto b = (right[bit / 64] >> shift) & mask;
        std::uint64_t value;
        switch (form.base) {
        case 0x0e208400U: value = a + b; break;
        case 0x2e208400U: value = a - b; break;
        case 0x2e20b800U: value = 0 - a; break;
        case 0x2e208c00U: value = a == b ? mask : 0; break;
        case 0x0e209800U: value = a == 0 ? mask : 0; break;
        case 0x0e208c00U: value = (a & b) != 0 ? mask : 0; break;
        default: throw std::logic_error("unknown arranged SIMD test operation");
        }
        output[bit / 64] |= (value & mask) << shift;
    }
    return output;
}

bool integer_simd_same_state(const transmute::CpuState& left,
                             const transmute::CpuState& right) {
    return left.x == right.x && left.v == right.v && left.sp == right.sp &&
        left.pc == right.pc && left.nzcv == right.nzcv &&
        left.fpcr == right.fpcr && left.fpsr == right.fpsr;
}

void test_jit_integer_simd_predicate() {
    const auto expect_emitter_rejection = [](std::uint32_t opcode) {
        bool rejected = false;
        try {
            static_cast<void>(transmute::jit_detail::emit_integer_simd(opcode));
        } catch (const std::invalid_argument& error) {
            rejected = std::string_view(error.what()) ==
                "unsupported native integer SIMD encoding";
        }
        expect(rejected, "native SIMD emitter failed its invalid-encoding contract");
    };
    for (const auto form : integer_simd_test_forms) {
        for (const bool quad : {false, true}) {
            for (unsigned size = 0; size != (form.arranged ? 4U : 1U); ++size) {
                const auto opcode = form.base | (quad ? 0x40000000U : 0) |
                    (size << 22) | 31U | (30U << 5) |
                    (form.unary ? 0 : 29U << 16);
                const bool valid = !form.arranged || quad || size != 3;
                expect(transmute::jit_detail::accepts_integer_simd(opcode) == valid,
                       "native SIMD predicate accepted a reserved lane arrangement");
                if (!valid) {
                    expect_emitter_rejection(opcode);
                }
                expect(!transmute::jit_detail::accepts_integer_simd(opcode | 0x80000000U),
                       "native SIMD predicate ignored a reserved top encoding bit");
            }
        }
    }
    // Separate scalar, FP, saturating, table and SVE operations stay outside
    // this explicitly implemented vector integer subset.
    for (const auto opcode : {0x5ee28420U, 0x7ee28420U, 0x4e22d420U,
                             0x4e220c20U, 0x4e020020U, 0x04a20020U,
                             0x2e605820U, 0x2e215820U, 0x0e219820U,
                             0xffffffffU}) {
        expect(!transmute::jit_detail::accepts_integer_simd(opcode),
               "native SIMD predicate claimed an unimplemented or reserved encoding");
        expect_emitter_rejection(opcode);
    }
}

void test_jit_integer_simd() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    struct Registers { unsigned destination, source, second; };
    constexpr std::array register_forms{
        Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
        Registers{0, 1, 1}, Registers{1, 1, 1},
        Registers{31, 31, 30}, Registers{30, 31, 30}};
    using Vector = std::array<std::uint64_t, 2>;
    struct Inputs { Vector left, right, prior; };
    constexpr std::array inputs{
        Inputs{{0, 0}, {0, 0}, {UINT64_MAX, UINT64_MAX}},
        Inputs{{UINT64_MAX, UINT64_MAX}, {1, 1}, {0, UINT64_MAX}},
        Inputs{{0x8080808080808080ULL, 0x8000800080008000ULL},
               {0x7f7f7f7f7f7f7f7fULL, 0x7fff7fff7fff7fffULL},
               {0x55aa55aa55aa55aaULL, 0xaa55aa55aa55aa55ULL}},
        Inputs{{0x8000000080000000ULL, 0x8000000000000000ULL},
               {0x7fffffff7fffffffULL, 0x7fffffffffffffffULL},
               {0x0123456789abcdefULL, 0xfedcba9876543210ULL}},
        Inputs{{0x11111111aaaaaaaaULL, 0xbbbbbbbb22222222ULL},
               {0x11111111aaaaaaabULL, 0xbbbbbbbb22222222ULL},
               {0x00000000ffffffffULL, 0xffffffff00000000ULL}},
        Inputs{{0x11111111aaaaaaaaULL, 0xbbbbbbbb22222222ULL},
               {0x11111111aaaaaaaaULL, 0xbbbbbbba22222222ULL},
               {0xff00ff00ff00ff00ULL, 0x00ff00ff00ff00ffULL}},
    };
    for (const auto form : integer_simd_test_forms) {
        for (const bool quad : {false, true}) {
            for (unsigned size = 0; size != (form.arranged ? 4U : 1U); ++size) {
                if (form.arranged && size == 3 && !quad) continue;
                for (const auto registers : register_forms) {
                    const auto opcode = form.base | (quad ? 0x40000000U : 0) |
                        (size << 22) | registers.destination |
                        (registers.source << 5) |
                        (form.unary ? 0 : registers.second << 16);
                    const std::array words{opcode, 0xd65f03c0U};
                    auto image = transmute::PeImage::parse(executable({
                        0xd51b4214U, // msr nzcv,x20
                        0xd51b4415U, // msr fpcr,x21
                        0xd51b4436U, // msr fpsr,x22
                        words[0], words[1]}));
                    transmute::X64BlockCache cache;
                    for (unsigned sample = 0; sample != inputs.size(); ++sample) {
                        transmute::ReferenceEngine oracle(image);
                        transmute::ReferenceEngine optimized(image);
                        for (auto* engine : {&oracle, &optimized}) {
                            for (unsigned reg = 0; reg != 30; ++reg) {
                                engine->set_initial_register(reg,
                                    0x1234567800000000ULL + reg * 0x1020304ULL);
                            }
                            engine->set_initial_register(20, (sample & 15U) << 28);
                            engine->set_initial_register(21, 0x07c0ff00U);
                            engine->set_initial_register(22, 0xa800009fU);
                            for (unsigned reg = 0; reg != 32; ++reg) {
                                engine->set_initial_vector(reg,
                                    {0xa5a5000000000000ULL + reg,
                                     0x5a5affffffffffffULL - reg});
                            }
                            engine->set_initial_vector(registers.destination, inputs[sample].prior);
                            engine->set_initial_vector(registers.source, inputs[sample].left);
                            if (!form.unary) {
                                engine->set_initial_vector(registers.second, inputs[sample].right);
                            }
                        }
                        auto before = oracle.state();
                        before.pc = image.entry() + 12;
                        before.nzcv = static_cast<std::uint32_t>(before.x[20]);
                        before.fpcr = static_cast<std::uint32_t>(before.x[21]);
                        before.fpsr = static_cast<std::uint32_t>(before.x[22]);
                        auto expected = before;
                        expected.v[registers.destination] = integer_simd_spec_result(
                            form, size, quad, before, registers.destination,
                            registers.source, registers.second);
                        expected.pc = before.x[30];
                        static_cast<void>(oracle.run());
                        expect(integer_simd_same_state(oracle.state(), expected),
                               "SIMD instruction oracle disagrees with independent lane semantics");
                        auto direct = before;
                        expect(cache.execute(direct, before.pc, words) &&
                                   integer_simd_same_state(direct, expected),
                               "direct native SIMD changed lanes, aliases, upper bits or flags");
                        auto cached = before;
                        expect(cache.execute_cached(cached, before.pc, 0, 1) == 0 &&
                                   integer_simd_same_state(cached, before),
                               "cached native SIMD crossed the whole-block instruction budget");
                        expect(cache.execute_cached(cached, before.pc, 0, 2) == 2 &&
                                   integer_simd_same_state(cached, expected),
                               "cached native SIMD differs from direct execution");
                        optimized.set_jit_enabled(true);
                        const auto executed = optimized.run();
                        expect(executed.instructions == 5 &&
                                   optimized.jit_executed_instructions() == 2 &&
                                   integer_simd_same_state(optimized.state(), expected),
                               "runtime SIMD dispatch fell back or changed architectural state");
                    }
                    expect(cache.cache_size() == 1,
                           "SIMD cache duplicated code for different vector contents");
                }
            }
        }
    }
#endif
}

void benchmark_jit_integer_simd() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::uint64_t iterations = 2'000'000;
    auto image = transmute::PeImage::parse(executable({
        0x4ea28428U, // add v8.4s,v1.4s,v2.4s
        0x6ee28429U, // sub v9.2d,v1.2d,v2.2d
        0x6e621c28U, // bsl v8.16b,v1.16b,v2.16b
        0x6ee28c2aU, // cmeq v10.2d,v1.2d,v2.2d
        0x6e221d2bU, // eor v11.16b,v9.16b,v2.16b
        0xf1000400U, // subs x0,x0,#1
        0x54ffff41U, // b.ne to the first vector instruction
        0xd65f03c0U,
    }));
    std::optional<transmute::CpuState> expected;
    for (const bool jit : {false, true}) {
        transmute::ReferenceEngine engine(image);
        engine.set_initial_register(0, iterations);
        engine.set_initial_vector(1, {0x123456789abcdef0ULL, 0xfedcba9876543210ULL});
        engine.set_initial_vector(2, {0x800000017fffffffULL, 0x0102030405060708ULL});
        engine.set_instruction_limit(iterations * 7 + 1);
        engine.set_jit_enabled(jit);
        const auto start = std::chrono::steady_clock::now();
        const auto result = engine.run();
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        expect(result.instructions == iterations * 7 + 1,
               "SIMD benchmark changed loop instruction accounting");
        if (!jit) expected = engine.state();
        else {
            expect(integer_simd_same_state(engine.state(), *expected) &&
                       engine.jit_executed_instructions() == result.instructions,
                   "SIMD benchmark fell back or disagreed with the instruction oracle");
        }
        std::cout << "simd_integer_" << (jit ? "jit" : "reference") << "_seconds="
                  << elapsed << " guest_instructions=" << result.instructions << '\n';
    }
#endif
}
