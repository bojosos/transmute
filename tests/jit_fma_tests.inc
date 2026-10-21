// Included after jit_fp_tests.inc inside test_main.cpp's anonymous namespace.

constexpr std::array guarded_fma_test_forms{
    GuardedFpTestForm{0x1f000000U, true, false, false},
    GuardedFpTestForm{0x1f000000U, true, true, false},
    GuardedFpTestForm{0x1f008000U, true, false, false},
    GuardedFpTestForm{0x1f008000U, true, true, false},
    GuardedFpTestForm{0x1f200000U, true, false, false},
    GuardedFpTestForm{0x1f200000U, true, true, false},
    GuardedFpTestForm{0x1f208000U, true, false, false},
    GuardedFpTestForm{0x1f208000U, true, true, false},
    GuardedFpTestForm{0x0e20cc00U, false, false, false},
    GuardedFpTestForm{0x0e20cc00U, false, false, true},
    GuardedFpTestForm{0x0e20cc00U, false, true, true},
    GuardedFpTestForm{0x0ea0cc00U, false, false, false},
    GuardedFpTestForm{0x0ea0cc00U, false, false, true},
    GuardedFpTestForm{0x0ea0cc00U, false, true, true},
};

std::uint32_t guarded_fma_test_opcode(GuardedFpTestForm form,
                                     unsigned rd, unsigned rn, unsigned rm,
                                     unsigned ra) {
    return guarded_fp_test_opcode(form, rd, rn, rm) | (form.scalar ? ra << 10 : 0);
}

void test_jit_guarded_fma_predicate() {
    for (const auto form : guarded_fma_test_forms) {
        const auto word = guarded_fma_test_opcode(form, 31, 30, 29, 28);
        expect(transmute::jit_detail::accepts_guarded_fp(word, true) &&
                   !transmute::jit_detail::accepts_guarded_fp(word, false) &&
                   transmute::jit_detail::accepts_guarded_fp(word) ==
                       transmute::jit_detail::native_fma_available(),
               "fused native FP did not enforce CPU/OS FMA eligibility");
        if (!transmute::jit_detail::native_fma_available()) {
            bool rejected = false;
            try {
                static_cast<void>(transmute::jit_detail::emit_guarded_fp(word, 40));
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            expect(rejected, "FMA fragment emitted unsupported host instructions");
        }
    }
    expect(transmute::jit_detail::accepts_guarded_fp(0x1e222820U, false),
           "FMA feature gate removed baseline SSE2 arithmetic");
    for (const auto word : {0x1f820c20U, 0x1fc20c20U,
                            0x0e62cc20U, 0x0ee2cc20U,
                            0x0fc21020U, 0x4e22cc20U | 0x80000000U}) {
        expect(!transmute::jit_detail::accepts_guarded_fp(word, true),
               "fused native FP accepted half/indexed/reserved forms");
    }
}

void test_jit_guarded_fma() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (!transmute::jit_detail::native_fma_available()) return;
    GuardedFpHostState restore;
    struct Registers { unsigned destination, source, second, accumulator; };
    constexpr std::array scalar_registers{
        Registers{0, 1, 2, 3}, Registers{1, 1, 2, 3}, Registers{2, 1, 2, 3},
        Registers{3, 1, 2, 3}, Registers{0, 1, 1, 3}, Registers{0, 1, 2, 1},
        Registers{0, 1, 2, 2}, Registers{1, 1, 1, 3}, Registers{1, 1, 2, 1},
        Registers{2, 1, 2, 2}, Registers{0, 1, 1, 1}, Registers{1, 1, 1, 1},
        Registers{1, 1, 2, 2}, Registers{2, 1, 2, 1}, Registers{3, 1, 1, 3},
        Registers{31, 31, 30, 29}, Registers{29, 31, 30, 29}};
    constexpr std::array vector_registers{
        Registers{0, 1, 2, 0}, Registers{1, 1, 2, 1}, Registers{2, 1, 2, 2},
        Registers{0, 1, 1, 0}, Registers{1, 1, 1, 1},
        Registers{31, 31, 30, 31}, Registers{30, 31, 30, 30}};
    struct Values { std::uint64_t left, right, addend; };
    constexpr std::array singles{
        Values{0x3fc00000U, 0x40000000U, 0x40400000U},
        Values{0x3f800001U, 0x3f7ffffeU, 0xbf800000U}, // exact -2^-46
        Values{0x7f7fffffU, 0x40000000U, 0xff7fffffU}, // huge product cancels
        Values{0x80000000U, 0x40000000U, 0x80000000U},
        Values{0x00800000U, 0x3f000000U, 0x3f800000U}, // tiny product, normal sum
        Values{0x3f900000U, 0x3fb00000U, 0x32800000U},
    };
    constexpr std::array doubles{
        Values{0x3ff8000000000000ULL, 0x4000000000000000ULL, 0x4008000000000000ULL},
        Values{0x3ff0000000000001ULL, 0x3feffffffffffffeULL, 0xbff0000000000000ULL},
        Values{0x7fefffffffffffffULL, 0x4000000000000000ULL, 0xffefffffffffffffULL},
        Values{0x8000000000000000ULL, 0x4000000000000000ULL, 0x8000000000000000ULL},
        Values{0x0010000000000000ULL, 0x3fe0000000000000ULL, 0x3ff0000000000000ULL},
        Values{0x3ff2000000000000ULL, 0x3ff6000000000000ULL, 0x3c80000000000000ULL},
    };
    std::size_t commits = 0;
    for (const auto form : guarded_fma_test_forms) {
        const auto registers = form.scalar
            ? std::span<const Registers>(scalar_registers)
            : std::span<const Registers>(vector_registers);
        for (const auto reg : registers) {
            const auto opcode = guarded_fma_test_opcode(form, reg.destination,
                reg.source, reg.second, reg.accumulator);
            GuardedFpTestKernel kernel(opcode);
            auto image = transmute::PeImage::parse(executable({
                0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcode, 0xd65f03c0U}));
            for (unsigned rounding = 0; rounding != 4; ++rounding) {
                for (const auto controls : {0U, 0x03000000U}) {
                    unsigned sample = 0;
                    for (const auto values : (form.wide ? doubles : singles)) {
                        const auto fpcr = (rounding << 22) | controls;
                        // Keep existing IOC/IDC/QC and reserved sticky bits,
                        // but leave OFC/UFC/IXC clear to detect new exceptions.
                        constexpr auto fpsr = 0xa8000081U;
                        const auto initialize = [&](transmute::ReferenceEngine& engine) {
                            guarded_fp_initialize(engine, fpcr, fpsr,
                                reg.destination, reg.source, reg.second,
                                guarded_fp_test_vector(form, values.left, values.left),
                                guarded_fp_test_vector(form, values.right, values.right));
                            engine.set_initial_vector(form.scalar ? reg.accumulator : reg.destination,
                                guarded_fp_test_vector(form, values.addend, values.addend));
                        };
                        transmute::ReferenceEngine oracle(image);
                        initialize(oracle);
                        auto before = oracle.state();
                        before.pc = image.entry() + 12;
                        before.nzcv = 0xb0000000U;
                        before.fpcr = fpcr;
                        before.fpsr = fpsr;
                        static_cast<void>(oracle.run());
                        auto expected = oracle.state();
                        expected.pc = before.pc;
                        auto native = before;
                        const auto host = 0x1fbfU | ((3U - rounding) << 13) |
                            ((rounding & 1U) != 0 ? 0x8000U : 0) | (restore.saved & 0x40U);
                        _mm_setcsr(host);
                        const auto success = kernel.execute(native);
                        expect(_mm_getcsr() == host &&
                                   (success ? guarded_fp_same_allowed_state(native, expected,
                                                                          form, reg.destination)
                                            : integer_simd_same_state(native, before)),
                               "fused native FP changed rounding/aliases/FPSR or leaked host state");
                        commits += success;
                        const bool distinct = reg.destination == 0 && reg.source == 1 &&
                            reg.second == 2 && reg.accumulator == (form.scalar ? 3U : 0U);
                        const bool adds_product = form.base == 0x1f000000U ||
                            form.base == 0x0e20cc00U;
                        if (distinct && adds_product && sample == 1 && rounding == 0 && controls == 0) {
                            expect(success && (native.v[0][0] &
                                (form.wide ? 0x7fffffffffffffffULL : 0x7fffffffULL)) != 0,
                                "fused cancellation was replaced by separately rounded multiply/add");
                        }
                        if (distinct && adds_product && sample == 2 && rounding == 0 && controls == 0) {
                            expect(success && (native.fpsr & (1U << 2)) == 0,
                                   "fused finite result reported overflow of its intermediate product");
                        }
                        if (distinct && rounding == 0 && controls == 0) {
                            transmute::ReferenceEngine runtime(image);
                            initialize(runtime);
                            runtime.set_jit_enabled(true);
                            const auto result = runtime.run();
                            expect(result.instructions == 5 &&
                                       guarded_fp_same_allowed_state(runtime.state(), oracle.state(),
                                                                    form, reg.destination) &&
                                       runtime.jit_stats().native_region_instructions ==
                                           (success ? 2U : 0U),
                                   "fused runtime region differed from direct arithmetic/bailout");
                        }
                        ++sample;
                    }
                }
            }
        }
    }
    expect(commits > 1000, "fused native matrix did not exercise committed arithmetic");
#endif
}

void test_jit_guarded_fma_special_values() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (!transmute::jit_detail::native_fma_available()) return;
    GuardedFpHostState restore;
    for (const auto form : guarded_fma_test_forms) {
        const unsigned accumulator = form.scalar ? 3 : 0;
        const auto opcode = guarded_fma_test_opcode(form, 0, 1, 2, accumulator);
        GuardedFpTestKernel kernel(opcode);
        auto image = transmute::PeImage::parse(executable({
            0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcode, 0xd65f03c0U}));
        const auto one = form.wide ? 0x3ff0000000000000ULL : 0x3f800000ULL;
        const auto infinity = form.wide ? 0x7ff0000000000000ULL : 0x7f800000ULL;
        const auto quiet_nan = form.wide ? 0xfff8123456789abcULL : 0xffc12345ULL;
        const auto signaling_nan = form.wide ? 0x7ff0123456789abcULL : 0x7f812345ULL;
        for (const auto fpcr : {0U, 0x02000000U}) {
            for (const auto addend : {infinity, quiet_nan, signaling_nan}) {
                transmute::ReferenceEngine oracle(image);
                guarded_fp_initialize(oracle, fpcr, 0xa8000080U, 0, 1, 2,
                    guarded_fp_test_vector(form, one, one),
                    guarded_fp_test_vector(form, one, one));
                oracle.set_initial_vector(accumulator,
                    guarded_fp_test_vector(form, addend, addend));
                auto state = oracle.state();
                state.pc = image.entry() + 12;
                state.nzcv = 0xb0000000U;
                state.fpcr = fpcr;
                state.fpsr = 0xa8000080U;
                const auto pc = state.pc;
                static_cast<void>(oracle.run());
                auto expected = oracle.state();
                expected.pc = pc;
                const auto host = _mm_getcsr();
                expect(kernel.execute(state) &&
                           guarded_fp_same_allowed_state(state, expected, form, 0) &&
                           _mm_getcsr() == host,
                       "fused NaN/addend path exceeded native representation policy");
            }
        }
    }
#endif
}
