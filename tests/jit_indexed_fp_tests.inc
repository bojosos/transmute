// Include after jit_fp_session_tests.inc. Reuses the independent direct/session
// kernels and full architectural-state comparison from the FP test includes.

constexpr std::array indexed_fp_test_forms{
    GuardedFpTestForm{0x0f809000U, true, false, true},
    GuardedFpTestForm{0x0f809000U, true, true, true},
    GuardedFpTestForm{0x0f809000U, false, false, false},
    GuardedFpTestForm{0x0f809000U, false, false, true},
    GuardedFpTestForm{0x0f809000U, false, true, true},
    GuardedFpTestForm{0x0f801000U, true, false, true},
    GuardedFpTestForm{0x0f801000U, true, true, true},
    GuardedFpTestForm{0x0f801000U, false, false, false},
    GuardedFpTestForm{0x0f801000U, false, false, true},
    GuardedFpTestForm{0x0f801000U, false, true, true},
    GuardedFpTestForm{0x0f805000U, true, false, true},
    GuardedFpTestForm{0x0f805000U, true, true, true},
    GuardedFpTestForm{0x0f805000U, false, false, false},
    GuardedFpTestForm{0x0f805000U, false, false, true},
    GuardedFpTestForm{0x0f805000U, false, true, true},
};

std::uint32_t indexed_fp_test_opcode(GuardedFpTestForm form,
    unsigned rd, unsigned rn, unsigned rm, unsigned index) {
    return guarded_fp_test_opcode(form, rd, rn, rm) |
        (form.scalar ? 0x10000000U : 0) |
        (form.wide ? index << 11 : ((index & 1U) << 21) | ((index >> 1) << 11));
}

void indexed_fp_test_set_lane(std::array<std::uint64_t, 2>& vector,
                             bool wide, unsigned lane, std::uint64_t bits) {
    const auto word = wide ? lane : lane / 2;
    const auto shift = wide ? 0U : (lane & 1U) * 32;
    const auto mask = wide ? UINT64_MAX : 0xffffffffULL;
    vector[word] = (vector[word] & ~(mask << shift)) | ((bits & mask) << shift);
}

std::array<std::uint64_t, 2> indexed_fp_test_second(GuardedFpTestForm form,
    unsigned index, std::uint64_t selected, std::uint64_t unselected) {
    std::array<std::uint64_t, 2> vector{};
    for (unsigned lane = 0; lane != (form.wide ? 2U : 4U); ++lane) {
        indexed_fp_test_set_lane(vector, form.wide, lane,
                                lane == index ? selected : unselected);
    }
    return vector;
}

void test_jit_indexed_fp_predicate() {
    // Independent Clang ARM64 assembler oracle, fixtures/jit_indexed_fp_encodings.s.
    constexpr std::array<std::uint32_t, 48> assembled{
        0x5f829020U, 0x5fa29020U, 0x5f829820U, 0x5fa29820U,
        0x5fc29020U, 0x5fc29820U, 0x0f829020U, 0x0fa29020U,
        0x0f829820U, 0x0fa29820U, 0x4f829020U, 0x4fa29020U,
        0x4f829820U, 0x4fa29820U, 0x4fc29020U, 0x4fc29820U,
        0x5f821020U, 0x5fa21020U, 0x5f821820U, 0x5fa21820U,
        0x5fc21020U, 0x5fc21820U, 0x0f821020U, 0x0fa21020U,
        0x0f821820U, 0x0fa21820U, 0x4f821020U, 0x4fa21020U,
        0x4f821820U, 0x4fa21820U, 0x4fc21020U, 0x4fc21820U,
        0x5f825020U, 0x5fa25020U, 0x5f825820U, 0x5fa25820U,
        0x5fc25020U, 0x5fc25820U, 0x0f825020U, 0x0fa25020U,
        0x0f825820U, 0x0fa25820U, 0x4f825020U, 0x4fa25020U,
        0x4f825820U, 0x4fa25820U, 0x4fc25020U, 0x4fc25820U,
    };
    std::size_t checked = 0;
    for (const auto form : indexed_fp_test_forms) {
        for (unsigned index = 0; index != (form.wide ? 2U : 4U); ++index) {
            const auto word = indexed_fp_test_opcode(form, 0, 1, 2, index);
            const bool fused = form.base != 0x0f809000U;
            expect(word == assembled[checked++] &&
                       transmute::jit_detail::accepts_guarded_fp(word, true) &&
                       transmute::jit_detail::accepts_guarded_fp(word, false) == !fused,
                   "indexed FP predicate disagreed with assembler or FMA eligibility");
            expect(!transmute::jit_detail::accepts_guarded_fp(word | 0x80000000U, true) &&
                       !transmute::jit_detail::accepts_guarded_fp(word | 0x400U, true) &&
                       !transmute::jit_detail::accepts_guarded_fp(word & ~0x800000U, true),
                   "indexed FP accepted reserved opcode bits or half precision");
            if (form.scalar) {
                expect(!transmute::jit_detail::accepts_guarded_fp(word & ~0x40000000U, true),
                       "indexed scalar FP accepted Q=0");
            }
            if (form.wide) {
                expect(!transmute::jit_detail::accepts_guarded_fp(word | 0x200000U, true) &&
                           !transmute::jit_detail::accepts_guarded_fp(word & ~0x40000000U, true),
                       "indexed D accepted a reserved L bit or Q=0");
            }
        }
    }
    expect(checked == assembled.size(), "indexed FP predicate omitted an arrangement/index");
    for (const auto word : {0x0fc21020U, 0x1f821020U, 0x4fe21020U,
                            0x4f829420U, 0xdf821020U}) {
        bool rejected = false;
        try { static_cast<void>(transmute::jit_detail::emit_guarded_fp(word, 40)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "indexed FP emitter accepted a reserved arrangement");
    }
}

void test_jit_indexed_fp_aliases_rounding() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    struct Registers { unsigned rd, rn, rm; };
    constexpr std::array aliases{
        Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
        Registers{0, 1, 1}, Registers{1, 1, 1}, Registers{31, 30, 29},
        Registers{29, 31, 29}, Registers{31, 31, 31},
    };
    struct Values { std::uint64_t lhs, rhs, addend; };
    constexpr std::array singles{
        Values{0x3f800001U, 0x3f7ffffeU, 0xbf800000U},
        Values{0x7f7fffffU, 0x40000000U, 0xff7fffffU},
        Values{0x80000000U, 0x40000000U, 0x80000000U},
    };
    constexpr std::array doubles{
        Values{0x3ff0000000000001ULL, 0x3feffffffffffffeULL, 0xbff0000000000000ULL},
        Values{0x7fefffffffffffffULL, 0x4000000000000000ULL, 0xffefffffffffffffULL},
        Values{0x8000000000000000ULL, 0x4000000000000000ULL, 0x8000000000000000ULL},
    };
    std::size_t commits = 0;
    for (const auto form : indexed_fp_test_forms) {
        if (form.base != 0x0f809000U && !transmute::jit_detail::native_fma_available()) continue;
        for (unsigned index = 0; index != (form.wide ? 2U : 4U); ++index) {
            for (const auto regs : aliases) {
                const auto word = indexed_fp_test_opcode(form, regs.rd, regs.rn, regs.rm, index);
                GuardedFpTestKernel kernel(word);
                auto image = transmute::PeImage::parse(executable({
                    0xd51b4214U, 0xd51b4415U, 0xd51b4436U, word, 0xd65f03c0U}));
                for (unsigned rounding = 0; rounding != 4; ++rounding) {
                    for (const auto control : {0U, 0x03000000U}) {
                        unsigned sample = 0;
                        for (const auto values : (form.wide ? doubles : singles)) {
                            const auto fpcr = (rounding << 22) | control;
                            constexpr auto fpsr = 0xa8000080U;
                            const auto initialize = [&](transmute::ReferenceEngine& engine) {
                                guarded_fp_initialize(engine, fpcr, fpsr, regs.rd, regs.rn, regs.rm,
                                    guarded_fp_test_vector(form, values.lhs, values.lhs),
                                    indexed_fp_test_second(form, index, values.rhs,
                                        form.wide ? 0x4008000000000000ULL : 0x40400000U));
                                auto addend = values.addend;
                                if (form.base == 0x0f805000U && sample == 1) {
                                    addend ^= form.wide ? 1ULL << 63 : 1ULL << 31;
                                }
                                engine.set_initial_vector(regs.rd,
                                    guarded_fp_test_vector(form, addend, addend));
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
                                ((rounding & 1U) ? 0x8000U : 0) | (restore.saved & 0x40U);
                            _mm_setcsr(host);
                            const bool success = kernel.execute(native);
                            expect(_mm_getcsr() == host &&
                                       (success ? guarded_fp_same_allowed_state(native, expected, form, regs.rd)
                                                : integer_simd_same_state(native, before)),
                                   "indexed FP changed lane selection, aliases, rounding, FPSR or host state");
                            commits += success;
                            const bool distinct = regs.rd == 0 && regs.rn == 1 && regs.rm == 2;
                            if (distinct && form.base == 0x0f801000U && sample == 0 &&
                                rounding == 0 && control == 0) {
                                expect(success && (native.v[0][0] &
                                    (form.wide ? 0x7fffffffffffffffULL : 0x7fffffffULL)) != 0,
                                    "indexed FMLA lost fused single-round cancellation");
                            }
                            if (distinct && form.base != 0x0f809000U && sample == 1 &&
                                rounding == 0 && control == 0) {
                                expect(success && (native.fpsr & 4U) == 0,
                                       "indexed FMA reported overflow of its intermediate product");
                            }
                            if (distinct && sample == 0 && rounding == 0 && control == 0) {
                                transmute::ReferenceEngine runtime(image);
                                initialize(runtime);
                                runtime.set_jit_enabled(true);
                                const auto result = runtime.run();
                                expect(result.instructions == 5 &&
                                           guarded_fp_same_allowed_state(runtime.state(), oracle.state(), form, regs.rd) &&
                                           runtime.jit_stats().native_region_instructions == 2,
                                       "indexed FP runtime did not execute its native arithmetic/RET region");
                            }
                            ++sample;
                        }
                    }
                }
            }
        }
    }
    expect(commits >= 1000, "indexed FP matrix missed committed arithmetic");
#endif
}

void test_jit_indexed_fp_special_values() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    constexpr auto host = 0xbfe5U;
    for (const auto form : indexed_fp_test_forms) {
        if (form.base != 0x0f809000U && !transmute::jit_detail::native_fma_available()) continue;
        const auto one = form.wide ? 0x3ff0000000000000ULL : 0x3f800000U;
        const auto infinity = form.wide ? 0x7ff0000000000000ULL : 0x7f800000U;
        const auto qnan = form.wide ? 0xfff8123456789abcULL : 0xffc12345U;
        const auto snan = form.wide ? 0x7ff0123456789abcULL : 0x7f812345U;
        for (unsigned index = 0; index != (form.wide ? 2U : 4U); ++index) {
            const auto word = indexed_fp_test_opcode(form, 0, 1, 2, index);
            GuardedFpTestKernel kernel(word);
            auto image = transmute::PeImage::parse(executable({
                0xd51b4214U, 0xd51b4415U, 0xd51b4436U, word, 0xd65f03c0U}));
            for (const auto fpcr : {0U, 0x01000000U, 0x03000000U}) {
                for (const auto selected : {one, infinity, qnan, snan, 1ULL}) {
                    constexpr auto fpsr = 0xa8000000U;
                    transmute::ReferenceEngine oracle(image);
                    guarded_fp_initialize(oracle, fpcr, fpsr, 0, 1, 2,
                        guarded_fp_test_vector(form, one, one),
                        indexed_fp_test_second(form, index, selected, 1));
                    oracle.set_initial_vector(0, guarded_fp_test_vector(form, one, one));
                    auto state = oracle.state();
                    state.pc = image.entry() + 12;
                    state.nzcv = 0xb0000000U;
                    state.fpcr = fpcr;
                    state.fpsr = fpsr;
                    const auto before = state;
                    static_cast<void>(oracle.run());
                    auto expected = oracle.state();
                    expected.pc = state.pc;
                    _mm_setcsr(host);
                    const bool success = kernel.execute(state);
                    expect(_mm_getcsr() == host &&
                               (success ? guarded_fp_same_allowed_state(state, expected, form, 0)
                                        : integer_simd_same_state(state, before)),
                           "indexed FP special value changed flags, inactive lanes or host state");
                    const bool selected_flush = (fpcr & 0x01000000U) != 0 && selected == 1;
                    expect(success == !selected_flush,
                           "indexed FP guarded an unused Vm lane or missed the selected FZ input");
                    if (success) expect((state.fpsr & 0x80U) == 0,
                                        "unused indexed source denormal spuriously set IDC");
                }
            }
            // Unselected signaling NaNs must not raise IOC or reject a normal operation.
            transmute::CpuState state{};
            state.fpcr = 1U << 24;
            state.v[0] = guarded_fp_test_vector(form, one, one);
            state.v[1] = guarded_fp_test_vector(form, one, one);
            state.v[2] = indexed_fp_test_second(form, index, one, snan);
            _mm_setcsr(host);
            expect(kernel.execute(state) && state.fpsr == 0 && _mm_getcsr() == host,
                   "unused indexed signaling NaN affected an active FP operation");
        }
    }
#endif
}

void test_jit_indexed_fp_regions_sessions() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    constexpr auto host = 0xbfe5U;
    for (const auto form : indexed_fp_test_forms) {
        if (form.base != 0x0f809000U && !transmute::jit_detail::native_fma_available()) continue;
        const auto one = form.wide ? 0x3ff0000000000000ULL : 0x3f800000U;
        for (unsigned index = 0; index != (form.wide ? 2U : 4U); ++index) {
            const auto word = indexed_fp_test_opcode(form, 0, 1, 2, index);
            const std::array words{word, 0xd65f03c0U};
            transmute::CpuState initial{};
            initial.pc = 0x1000;
            initial.x[30] = 0x3000;
            initial.nzcv = 0xb0000000U;
            initial.fpsr = 0xa8000080U;
            initial.v[0] = guarded_fp_test_vector(form, one, one);
            initial.v[1] = guarded_fp_test_vector(form, one, one);
            initial.v[2] = indexed_fp_test_second(form, index, one, 1);
            _mm_setcsr(host);
            transmute::X64BlockCache cache;
            auto trapped = initial;
            trapped.fpcr = 1U << 8;
            const auto before = trapped;
            expect(cache.execute_native_region(trapped, initial.pc, words, 0, 2, nullptr) == 0 &&
                       integer_simd_same_state(trapped, before) && _mm_getcsr() == host,
                   "indexed FP without a pinned memory capability changed state");
            // Warm an actual engine-owned native region with a rejected FPCR
            // policy, then re-enter that same cached owner with a valid policy.
            // Native regions require its scoped lease even without data memory.
            auto warm_image = transmute::PeImage::parse(executable({
                0xd51b4214U, 0xd51b4415U, 0xd51b4436U, word, 0xd65f03c0U}));
            for (const auto budget : {4U, 5U}) {
                transmute::ReferenceEngine oracle(warm_image), runtime(warm_image);
                for (auto* engine : {&oracle, &runtime}) {
                    guarded_fp_initialize(*engine, 1U << 8, initial.fpsr, 0, 1, 2,
                        initial.v[1], initial.v[2]);
                    engine->set_initial_vector(0, initial.v[0]);
                }
                runtime.set_jit_enabled(true);
                const auto empty = std::span<const std::uint64_t>{};
                expect(runtime.invoke_callback(warm_image.entry(), empty) ==
                           oracle.invoke_callback(warm_image.entry(), empty) &&
                           _mm_getcsr() == host,
                       "indexed FP warm trap-policy fallback changed arithmetic or host state");
                const auto warmed = runtime.jit_stats();
                expect(warmed.compiled_native_regions == 1 &&
                           warmed.native_region_instructions == 1,
                       "indexed FP trap-policy warmup lost its cached owner or RET");
                oracle.set_initial_register(21, 0);
                runtime.set_initial_register(21, 0);
                if (budget == 4) {
                    expect_failure([&] { static_cast<void>(oracle.run(budget)); },
                                   "guest exceeded instruction limit");
                    expect_failure([&] { static_cast<void>(runtime.run(budget)); },
                                   "guest exceeded instruction limit");
                } else {
                    expect(oracle.run(budget).instructions == budget &&
                               runtime.run(budget).instructions == budget,
                           "warm indexed FP changed its exact runtime budget");
                }
                expect(integer_simd_same_state(runtime.state(), oracle.state()) &&
                           runtime.jit_stats().compiled_native_regions == 1 &&
                           runtime.jit_stats().native_region_instructions ==
                               warmed.native_region_instructions + budget - 3 &&
                           _mm_getcsr() == host,
                       "warm indexed FP changed full state, budget, cache reuse, FPSR or host state");
            }
            // A signaling source raises fresh IOC before the selected FZ input
            // rejects this operation. Only the FDIV prefix's IXC may commit.
            auto state = initial;
            state.fpcr = 1U << 24;
            state.fpsr = 0xa8000000U;
            const auto snan = form.wide ? 0x7ff0123456789abcULL : 0x7f812345U;
            state.v[1] = guarded_fp_test_vector(form, snan, snan);
            state.v[2] = indexed_fp_test_second(form, index, 1, one);
            // The successful prefix uses separate S6/S7 registers so every
            // indexed source width/index remains independently controlled.
            const std::array prefix_words{0x1e2718c4U, word}; // FDIV S4,S6,S7
            NativeFpSessionTestKernel prefix_session(prefix_words);
            state.v[6] = {0x3f800000U, UINT64_MAX};
            state.v[7] = {0x40400000U, UINT64_MAX};
            GuardedFpTestKernel prefix(prefix_words[0]);
            auto target = state;
            expect(prefix.execute(target), "indexed FP session prefix failed");
            NativeFpSessionTestKernel::Context context;
            expect(prefix_session.execute(state, context) == 1 && context.completed == 1 &&
                       integer_simd_same_state(state, target) && (state.fpsr & 0x11U) == 0x10U &&
                       _mm_getcsr() == host,
                   "failed indexed FP committed its result/flags or discarded its successful prefix");
            // Runtime must materialize the committed prefix before checked
            // indexed fallback, FPSR observation and every exact budget exit.
            auto image = transmute::PeImage::parse(executable({
                0xd51b4214U, 0xd51b4415U, 0xd51b4436U,
                prefix_words[0], word, 0xd53b4437U, 0xd65f03c0U}));
            const auto initialize = [&](transmute::ReferenceEngine& engine) {
                guarded_fp_initialize(engine, 1U << 24, 0xa8000000U, 0, 1, 2,
                    guarded_fp_test_vector(form, snan, snan),
                    indexed_fp_test_second(form, index, 1, one));
                engine.set_initial_vector(0, guarded_fp_test_vector(form, one, one));
                engine.set_initial_vector(6, {0x3f800000U, UINT64_MAX});
                engine.set_initial_vector(7, {0x40400000U, UINT64_MAX});
            };
            for (const auto budget : {4U, 5U, 6U, 7U}) {
                transmute::ReferenceEngine oracle(image), runtime(image);
                initialize(oracle);
                initialize(runtime);
                runtime.set_jit_enabled(true);
                _mm_setcsr(host);
                if (budget < 7) {
                    expect_failure([&] { static_cast<void>(oracle.run(budget)); },
                                   "guest exceeded instruction limit");
                    expect_failure([&] { static_cast<void>(runtime.run(budget)); },
                                   "guest exceeded instruction limit");
                } else {
                    expect(oracle.run(budget).instructions == 7 &&
                               runtime.run(budget).instructions == 7,
                           "indexed fallback runtime lost a dynamic instruction");
                }
                expect(integer_simd_same_state(runtime.state(), oracle.state()) &&
                           _mm_getcsr() == host &&
                           runtime.jit_stats().native_region_instructions == (budget == 7 ? 2U : 1U),
                       "indexed guard runtime changed exact budget, fallback PC, FPSR observer or host state");
            }
        }
    }
#endif
}
