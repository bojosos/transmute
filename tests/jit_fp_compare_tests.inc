// Included after jit_fp_session_tests.inc inside test_main.cpp's anonymous namespace.

struct NativeFpCompareTestForm { bool wide, signal, zero; };
constexpr std::array<NativeFpCompareTestForm, 8> native_fp_compare_test_forms{
    NativeFpCompareTestForm{false, false, false}, {true, false, false},
    {false, true, false}, {true, true, false}, {false, false, true},
    {true, false, true}, {false, true, true}, {true, true, true}};

std::uint32_t native_fp_compare_test_opcode(NativeFpCompareTestForm form,
                                            unsigned rn, unsigned rm) {
    return 0x1e202000U | (form.wide ? 0x00400000U : 0) |
        (form.signal ? 0x10U : 0) | (form.zero ? 8U : rm << 16) | (rn << 5);
}

void test_jit_fp_compare_predicate() {
    for (const auto form : native_fp_compare_test_forms) {
        const auto opcode = native_fp_compare_test_opcode(form, 31, 30);
        expect(transmute::jit_detail::accepts_guarded_fp(opcode, false),
               "native FP compare rejected a baseline S/D register/zero form");
        expect(!transmute::jit_detail::accepts_guarded_fp(opcode | 0x80000000U, true),
               "native FP compare accepted a reserved top bit");
        if (form.zero) {
            expect(!transmute::jit_detail::accepts_guarded_fp(opcode | 0x00010000U, true),
                   "native FP compare zero accepted a reserved nonzero Rm field");
        }
    }
    expect(transmute::jit_detail::accepts_guarded_fp(0x1e752200U, false),
           "native FP compare missed the real Cycles D-register form");
    for (const auto opcode : {0x1ea22020U, 0x1ee22020U, 0x1e202001U,
                              0x1e202004U, 0x1e202400U, 0x1e222028U}) {
        expect(!transmute::jit_detail::accepts_guarded_fp(opcode, true),
               "native FP compare admitted half/reserved encodings");
        bool rejected = false;
        try {
            static_cast<void>(transmute::jit_detail::emit_guarded_fp(opcode, 40));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        expect(rejected, "native FP compare emitted half/reserved instructions");
    }
}

void test_jit_fp_compare() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    struct Values { std::uint32_t left_s, right_s; std::uint64_t left_d, right_d; };
    constexpr std::array values{
        Values{0, 0, 0, 0},
        Values{0x80000000U, 0, 0x8000000000000000ULL, 0},
        Values{0, 0x80000000U, 0, 0x8000000000000000ULL},
        Values{0x3f800000U, 0x40000000U, 0x3ff0000000000000ULL, 0x4000000000000000ULL},
        Values{0x40000000U, 0x3f800000U, 0x4000000000000000ULL, 0x3ff0000000000000ULL},
        Values{0x3f800000U, 0x3f800000U, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL},
        Values{0xbf800000U, 0x3f800000U, 0xbff0000000000000ULL, 0x3ff0000000000000ULL},
        Values{0xc0000000U, 0xbf800000U, 0xc000000000000000ULL, 0xbff0000000000000ULL},
        Values{0x7f800000U, 0x3f800000U, 0x7ff0000000000000ULL, 0x3ff0000000000000ULL},
        Values{0xff800000U, 0x3f800000U, 0xfff0000000000000ULL, 0x3ff0000000000000ULL},
        Values{0x7f800000U, 0x7f800000U, 0x7ff0000000000000ULL, 0x7ff0000000000000ULL},
        Values{0xff800000U, 0xff800000U, 0xfff0000000000000ULL, 0xfff0000000000000ULL},
        Values{1, 0, 1, 0},
        Values{0x007fffffU, 0, 0x000fffffffffffffULL, 0},
        Values{0x00800000U, 0, 0x0010000000000000ULL, 0},
        Values{0x7fc12345U, 0x3f800000U, 0x7ff8123456789abcULL, 0x3ff0000000000000ULL},
        Values{0x3f800000U, 0xffc12345U, 0x3ff0000000000000ULL, 0xfff8123456789abcULL},
        Values{0x7f812345U, 0x3f800000U, 0x7ff0123456789abcULL, 0x3ff0000000000000ULL},
        Values{0x3f800000U, 0xff812345U, 0x3ff0000000000000ULL, 0xfff0123456789abcULL},
        Values{0x7fc12345U, 0x7f812345U, 0x7ff8123456789abcULL, 0x7ff0123456789abcULL},
        Values{0x7fc12345U, 1, 0x7ff8123456789abcULL, 1},
        Values{1, 0xffc12345U, 1, 0xfff8123456789abcULL},
    };
    std::size_t accepted = 0, rejected = 0;
    for (const auto form : native_fp_compare_test_forms) {
        for (const auto [rn, rm] : std::array<std::pair<unsigned, unsigned>, 5>{
                 std::pair{1U, 2U}, {1U, 1U}, {31U, 30U}, {31U, 31U}, {0U, 0U}}) {
            const auto opcode = native_fp_compare_test_opcode(form, rn, rm);
            GuardedFpTestKernel kernel(opcode);
            auto image = transmute::PeImage::parse(executable({
                0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcode, 0xd65f03c0U}));
            for (const auto control : {1U, 1U << 8, 1U << 15, 1U << 19}) {
                transmute::CpuState state{};
                state.fpcr = control;
                state.fpsr = 0xa8000080U;
                state.nzcv = 0xb0000000U;
                state.v[rn] = {1, UINT64_MAX};
                state.v[rm] = {0x7ff0123456789abcULL, UINT64_MAX};
                const auto before = state;
                const auto host = _mm_getcsr();
                expect(!kernel.execute(state) && integer_simd_same_state(state, before) &&
                           _mm_getcsr() == host,
                       "native FP compare accepted traps/unsupported controls or changed state");
            }
            for (unsigned rounding = 0; rounding != 4; ++rounding) {
                for (const auto controls : {0U, 0x03000000U}) {
                    for (const auto sample : values) {
                        const auto fpcr = (rounding << 22) | controls;
                        constexpr auto fpsr = 0xa800009eU; // IOC initially clear
                        transmute::ReferenceEngine oracle(image);
                        const auto left = form.wide ? sample.left_d : sample.left_s;
                        const auto right = form.wide ? sample.right_d : sample.right_s;
                        const GuardedFpTestForm arrangement{0, true, form.wide, false};
                        guarded_fp_initialize(oracle, fpcr, fpsr, 29, rn, rm,
                            guarded_fp_test_vector(arrangement, left, UINT64_MAX),
                            guarded_fp_test_vector(arrangement, right, UINT64_MAX));
                        auto state = oracle.state();
                        state.pc = image.entry() + 12;
                        state.nzcv = 0xb0000000U;
                        state.fpcr = fpcr;
                        state.fpsr = fpsr;
                        const auto before = state;
                        static_cast<void>(oracle.run());
                        auto expected = oracle.state();
                        expected.pc = state.pc;
                        const auto host = 0x1fbfU | ((3U - rounding) << 13) |
                            ((rounding & 1U) != 0 ? 0x8000U : 0) | (restore.saved & 0x40U);
                        _mm_setcsr(host);
                        const auto success = kernel.execute(state);
                        expect(_mm_getcsr() == host &&
                                   (success ? integer_simd_same_state(state, expected)
                                            : integer_simd_same_state(state, before)),
                               "native FP compare changed NZCV, IOC, vectors or host control state");
                        accepted += success;
                        rejected += !success;
                        if ((fpcr & (1U << 24)) == 0) {
                            expect(success, "native FP compare unnecessarily rejected default NaN/subnormal inputs");
                        }
                    }
                }
            }
        }
    }
    expect(accepted > 1000 && rejected > 0,
           "native FP compare differential matrix missed commit or FZ fallback");
#endif
}

void test_jit_fp_compare_sessions() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    constexpr auto host = 0xdfbfU;
    // Quiet/signal comparison follows an inexact successful operation in the
    // same live MXCSR session. NZCV commits immediately, FPSR at the exit.
    for (const bool signal : {false, true}) {
        const auto compare = native_fp_compare_test_opcode({false, signal, false}, 6, 7);
        const std::array opcodes{0x1e221824U, compare, 0x1e602108U}; // fcmp d8,#0
        NativeFpSessionTestKernel kernel(opcodes);
        auto image = transmute::PeImage::parse(executable({
            0xd51b4214U, 0xd51b4415U, 0xd51b4436U,
            opcodes[0], opcodes[1], opcodes[2], 0xd65f03c0U}));
        for (const auto fpcr : {0U, 1U << 24}) {
            transmute::ReferenceEngine oracle(image);
            guarded_fp_initialize(oracle, fpcr, 0xa8000080U, 0, 1, 2,
                {0x3f800000U, UINT64_MAX}, {0x40400000U, UINT64_MAX});
            oracle.set_initial_vector(6, {0x7fc12345U, UINT64_MAX});
            oracle.set_initial_vector(7, {1, UINT64_MAX});
            oracle.set_initial_vector(8, {0x8000000000000000ULL, UINT64_MAX});
            auto state = oracle.state();
            state.pc = image.entry() + 12;
            state.nzcv = 0xb0000000U;
            state.fpcr = fpcr;
            state.fpsr = 0xa8000080U;
            if (fpcr != 0) {
                // The compare must bail without IOC/IDC/NZCV from that failed
                // operation, retaining exactly the successfully divided prefix.
                GuardedFpTestKernel prefix(opcodes[0]);
                auto expected = state;
                _mm_setcsr(host);
                expect(prefix.execute(expected), "compare-session prefix unexpectedly bailed");
                NativeFpSessionTestKernel::Context context;
                expect(kernel.execute(state, context) == 1 &&
                           integer_simd_same_state(state, expected) && _mm_getcsr() == host,
                       "FZ compare session committed failed-operation flags or NZCV");
            } else {
                static_cast<void>(oracle.run());
                auto expected = oracle.state();
                expected.pc = state.pc;
                NativeFpSessionTestKernel::Context context;
                _mm_setcsr(host);
                expect(kernel.execute(state, context) == 3 &&
                           context.observed_fpsr == 0xa8000080U &&
                           integer_simd_same_state(state, expected) && _mm_getcsr() == host,
                       "native compare session lost preceding arithmetic flags or final NZCV");
            }
        }
    }

    // Compare flags feed a native conditional branch, then an exact budget
    // return and FPSR observer. Every prefix must match the checked engine.
    const std::vector<std::uint32_t> words{
        0xd51b4414U, 0xd51b4435U, 0x1e752200U, // fcmp d16,d21
        0x5400004bU, // b.lt +8
        0x91000400U, // add x0,x0,#1, skipped for less-than or unordered
        0xd53b4436U, 0xd65f03c0U}; // mrs x22,fpsr; ret
    for (const auto [left, right, total] :
         std::array<std::tuple<std::uint64_t, std::uint64_t, unsigned>, 5>{
             std::tuple{0ULL, 0ULL, 7U},
             {0x3ff0000000000000ULL, 0x4000000000000000ULL, 6U},
             {0x4000000000000000ULL, 0x3ff0000000000000ULL, 7U},
             {0x7ff8123456789abcULL, 0ULL, 6U}, {0x7ff0123456789abcULL, 0ULL, 6U}}) {
        for (unsigned budget = 1; budget <= total; ++budget) {
            auto reference_image = transmute::PeImage::parse(executable(words));
            auto native_image = transmute::PeImage::parse(executable(words));
            transmute::ReferenceEngine reference(reference_image), native(native_image);
            for (auto* engine : {&reference, &native}) {
                engine->set_initial_register(20, 0);
                engine->set_initial_register(21, 0xa800009eU);
                engine->set_initial_vector(16, {left, UINT64_MAX});
                engine->set_initial_vector(21, {right, UINT64_MAX});
            }
            native.set_jit_enabled(true);
            if (budget < total) {
                for (auto* engine : {&reference, &native}) {
                    _mm_setcsr(host);
                    expect_failure([&] { static_cast<void>(engine->run(budget)); },
                                   "guest exceeded instruction limit");
                    expect(_mm_getcsr() == host, "native compare budget return leaked host CSR");
                }
                expect(integer_simd_same_state(native.state(), reference.state()),
                       "native compare branch or budget changed an architectural prefix");
            }
            _mm_setcsr(host);
            const auto expected = reference.run(), actual = native.run();
            expect(actual.instructions == expected.instructions &&
                       integer_simd_same_state(native.state(), reference.state()) && _mm_getcsr() == host &&
                       native.jit_stats().native_region_instructions > 0,
                   "native compare branch/cache execution missed exact state or native admission");
        }
    }
#endif
}

void test_jit_fp_nan_flush_priority() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    const auto check = [&](GuardedFpTestForm form, bool fused) {
        const auto opcode = fused ? guarded_fma_test_opcode(form, 0, 1, 2, 3)
                                  : guarded_fp_test_opcode(form, 0, 1, 2);
        GuardedFpTestKernel kernel(opcode);
        auto image = transmute::PeImage::parse(executable({
            0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcode, 0xd65f03c0U}));
        const unsigned operands = fused ? 3 : 2;
        const std::array registers{1U, 2U, form.scalar ? 3U : 0U};
        const auto one = form.wide ? 0x3ff0000000000000ULL : 0x3f800000ULL;
        for (unsigned nan_operand = 0; nan_operand != operands; ++nan_operand) {
            for (unsigned denormal_operand = 0; denormal_operand != operands; ++denormal_operand) {
                if (nan_operand == denormal_operand) continue;
                for (const bool signal : {false, true}) {
                    const auto nan = form.wide
                        ? (signal ? 0x7ff0123456789abcULL : 0x7ff8123456789abcULL)
                        : (signal ? 0x7f812345ULL : 0x7fc12345ULL);
                    for (const auto fpcr : {0U, 1U << 24}) {
                        transmute::ReferenceEngine oracle(image);
                        constexpr auto fpsr = 0xa8000000U; // IDC/IOC initially clear
                        guarded_fp_initialize(oracle, fpcr, fpsr, 0, 1, 2,
                            guarded_fp_test_vector(form, one, one),
                            guarded_fp_test_vector(form, one, one));
                        if (fused) oracle.set_initial_vector(registers[2],
                            guarded_fp_test_vector(form, one, one));
                        oracle.set_initial_vector(registers[nan_operand],
                            guarded_fp_test_vector(form, nan, nan));
                        oracle.set_initial_vector(registers[denormal_operand],
                            guarded_fp_test_vector(form, 1, 1));
                        auto state = oracle.state();
                        state.pc = image.entry() + 12;
                        state.nzcv = 0xb0000000U;
                        state.fpcr = fpcr;
                        state.fpsr = fpsr;
                        const auto before = state;
                        static_cast<void>(oracle.run());
                        auto expected = oracle.state();
                        expected.pc = state.pc;
                        const auto host = 0xffbfU | (restore.saved & 0x40U);
                        _mm_setcsr(host);
                        const auto success = kernel.execute(state);
                        expect(_mm_getcsr() == host &&
                                   (success ? guarded_fp_same_allowed_state(state, expected, form, 0)
                                            : integer_simd_same_state(state, before)),
                               "native FP NaN/denormal priority changed FPSR outside the representation policy");
                    }
                }
            }
        }
    };
    for (const auto form : guarded_fp_test_forms) check(form, false);
    if (transmute::jit_detail::native_fma_available()) {
        for (const auto form : guarded_fma_test_forms) check(form, true);
    }
#endif
}
