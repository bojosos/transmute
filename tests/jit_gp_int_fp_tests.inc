// Include after jit_indexed_fp_tests.inc; shares the independent FP kernels.

std::uint32_t gp_int_fp_opcode(bool source_wide, bool result_wide, bool unsign,
                              unsigned scale, unsigned rd = 0, unsigned rn = 1) {
    return (scale == 0 ? 0x1e220000U : 0x1e020000U | ((64U - scale) << 10)) |
        (source_wide ? 0x80000000U : 0) | (result_wide ? 0x400000U : 0) |
        (unsign ? 0x10000U : 0) | rd | (rn << 5);
}

void test_jit_gp_int_fp_predicate() {
    // Independent assembler fixture: fixtures/jit_gp_int_fp_encodings.s.
    constexpr std::array normals{0x1e220020U, 0x1e230020U, 0x1e620020U, 0x1e630020U,
                                 0x9e220020U, 0x9e230020U, 0x9e620020U, 0x9e630020U};
    constexpr std::array scale_one{0x1e02fc20U, 0x1e03fc20U, 0x1e42fc20U, 0x1e43fc20U,
                                   0x9e02fc20U, 0x9e03fc20U, 0x9e42fc20U, 0x9e43fc20U};
    std::size_t form = 0;
    for (const bool source_wide : {false, true}) {
        for (const bool result_wide : {false, true}) {
            for (const bool unsign : {false, true}) {
                expect(gp_int_fp_opcode(source_wide, result_wide, unsign, 0) == normals[form] &&
                           gp_int_fp_opcode(source_wide, result_wide, unsign, 1) == scale_one[form],
                       "GP integer-to-FP encoding disagreed with independent assembler words");
                ++form;
                for (unsigned scale = 0; scale <= (source_wide ? 64U : 32U); ++scale) {
                    for (const auto reg : {0U, 1U, 8U, 12U, 30U, 31U}) {
                        const auto word = gp_int_fp_opcode(source_wide, result_wide, unsign,
                                                           scale, reg, reg);
                        expect(transmute::jit_detail::accepts_guarded_fp(word, false) &&
                                   transmute::jit_detail::accepts_guarded_fp(word, true),
                               "GP conversion unexpectedly required FMA or rejected a valid form");
                        expect(!transmute::jit_detail::accepts_guarded_fp(word | 0x40000000U, true) &&
                                   !transmute::jit_detail::accepts_guarded_fp(word | 0x800000U, true),
                               "GP conversion accepted a reserved scalar bit or unsupported FP type");
                    }
                }
                for (unsigned scale = 33; !source_wide && scale <= 64; ++scale) {
                    expect(!transmute::jit_detail::accepts_guarded_fp(
                               gp_int_fp_opcode(false, result_wide, unsign, scale), true),
                           "W fixed-point conversion accepted scale>32");
                }
            }
        }
    }
    expect(transmute::jit_detail::accepts_guarded_fp(0x1e220191U) &&
               transmute::jit_detail::accepts_guarded_fp(0x1e620114U),
           "observed OCIO/DLL GP conversions remained unsupported");
    for (const auto word : {0x1e220420U, 0x1e228020U, 0x1ea20020U,
                            0x1ee20020U, 0x1e020020U, 0x1e024020U,
                            0x5e220020U, 0x9ee20020U}) {
        bool rejected = false;
        try { static_cast<void>(transmute::jit_detail::emit_guarded_fp(word, 40)); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "GP conversion emitter accepted a reserved encoding");
    }
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
class GpIntFpOracle {
public:
    explicit GpIntFpOracle(std::uint32_t word)
        : image_(transmute::PeImage::parse(executable({0xd65f03c0U}))), engine_(image_),
          source_((word >> 5) & 31U) {
        engine_.map_memory(code_, 4096);
        engine_.set_memory_access(code_, true, true, true);
        sink_ = engine_.register_guest_call([this](transmute::CpuState& state,
                                                  transmute::GuestMemory&) {
            observed_ = state;
            captured_ = true;
            state.x[30] = sentinel_;
        });
        unsigned count = 0;
        for (unsigned reg = 0; count != scratch_.size(); ++reg) {
            if (reg != source_) scratch_[count++] = reg;
        }
        for (unsigned reg = 0; reg != 31; ++reg)
            engine_.set_initial_register(reg, 0x0123456789abcdefULL + reg * 0x1020304ULL);
        for (unsigned reg = 0; reg != 32; ++reg)
            engine_.set_initial_vector(reg, {0xfedcba9876543210ULL + reg, UINT64_MAX - reg});
        engine_.write32(code_, 0xd51b4200U | scratch_[0]); // MSR NZCV
        engine_.write32(code_ + 4, 0xd51b4400U | scratch_[1]);
        engine_.write32(code_ + 8, 0xd51b4420U | scratch_[2]);
        // Callback entry normally sets LR. Restore the independently chosen
        // X30 input before conversion; the capture handler supplies its return.
        engine_.write32(code_ + 12, 0xaa0003feU | (scratch_[4] << 16));
        engine_.write32(code_ + 16, word);
        engine_.write32(code_ + 20, 0xd61f0000U | (scratch_[3] << 5));
        engine_.set_initial_register(scratch_[3], sink_);
    }

    std::pair<transmute::CpuState, transmute::CpuState> states(
        std::uint64_t value, std::uint32_t fpcr, std::uint32_t fpsr) {
        engine_.set_initial_register(scratch_[0], 0xb0000000U);
        engine_.set_initial_register(scratch_[1], fpcr);
        engine_.set_initial_register(scratch_[2], fpsr);
        if (source_ != 31) engine_.set_initial_register(source_, value);
        engine_.set_initial_register(scratch_[4], source_ == 30 ? value : sentinel_);
        auto before = engine_.state();
        before.pc = code_ + 16;
        before.sp = (before.sp - 256) & ~15ULL;
        before.x[30] = source_ == 30 ? value : sentinel_;
        before.nzcv = 0xb0000000U;
        before.fpcr = fpcr;
        before.fpsr = fpsr;
        captured_ = false;
        static_cast<void>(engine_.invoke_callback(code_, std::span<const std::uint64_t>{}));
        expect(captured_, "GP conversion oracle did not capture architectural state");
        auto expected = observed_;
        expected.pc = before.pc;
        return {before, expected};
    }
private:
    static constexpr auto code_ = 0x500000000ULL;
    static constexpr auto sentinel_ = 0xfffffffffffffff0ULL;
    transmute::PeImage image_;
    transmute::ReferenceEngine engine_;
    unsigned source_;
    std::array<unsigned, 5> scratch_{};
    transmute::CpuState observed_{};
    std::uint64_t sink_{};
    bool captured_{};
};
#endif

void test_jit_gp_int_fp_rounding_scaling() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    std::vector<std::uint64_t> inputs{0, 1, 2, 3, UINT64_MAX, UINT64_MAX - 1,
        UINT64_MAX - 2, 0xabcdef0180000001ULL, 0x12345678ffffffffULL};
    // Every power transition and both single/double halfway boundaries,
    // including unsigned64 sticky-half ties and signed minimum magnitudes.
    for (const auto bit : {23U, 24U, 31U, 32U, 52U, 53U, 62U, 63U}) {
        const auto power = 1ULL << bit;
        for (const auto delta : {0ULL, 1ULL, 2ULL, 3ULL}) {
            inputs.push_back(power + delta);
            inputs.push_back(power - delta);
            inputs.push_back(0ULL - power + delta);
            inputs.push_back(0ULL - power - delta);
        }
    }
    for (const auto precision : {24U, 53U}) {
        const auto spacing = 1ULL << (63U - precision + 1);
        for (const auto base : {1ULL << 63, (1ULL << 63) + spacing, UINT64_MAX - spacing * 2}) {
            for (const auto delta : {0ULL, spacing / 2 - 1, spacing / 2,
                                    spacing / 2 + 1, spacing - 1}) {
                inputs.push_back(base + delta);
            }
        }
    }
    std::size_t checked = 0;
    for (const bool source_wide : {false, true}) {
        for (const bool result_wide : {false, true}) {
            for (const bool unsign : {false, true}) {
                for (unsigned scale = 0; scale <= (source_wide ? 64U : 32U); ++scale) {
                    const auto word = gp_int_fp_opcode(source_wide, result_wide, unsign, scale);
                    GuardedFpTestKernel kernel(word);
                    GpIntFpOracle oracle(word);
                    for (unsigned rounding = 0; rounding != 4; ++rounding) {
                        const auto fpcr = (rounding << 22) | (rounding & 1U ? 0x07000000U : 0);
                        const auto host = 0x1fbfU | ((3U - rounding) << 13) |
                            ((rounding & 1U) ? 0x8000U : 0) | (restore.saved & 0x40U);
                        for (const auto input : inputs) {
                            auto [state, expected] = oracle.states(input, fpcr, 0xa8000080U);
                            _mm_setcsr(host);
                            expect(kernel.execute(state) && integer_simd_same_state(state, expected) &&
                                       _mm_getcsr() == host,
                                   "GP conversion changed signed/unsigned rounding, scale, IXC or host state; word=" +
                                       std::to_string(word) + ", input=" + std::to_string(input) +
                                       ", FPCR=" + std::to_string(fpcr));
                            ++checked;
                        }
                    }
                }
            }
        }
    }
    expect(checked > 100000, "GP conversion boundary matrix missed forms/scales");
#endif
}

void test_jit_gp_int_fp_registers_guards() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    constexpr auto host = 0xbfe5U;
    for (const bool source_wide : {false, true}) {
        for (const bool result_wide : {false, true}) {
            for (const bool unsign : {false, true}) {
                for (const auto scale : {0U, 1U, source_wide ? 64U : 32U}) {
                    for (unsigned rn = 0; rn != 32; ++rn) {
                        for (const auto rd : {rn, 31U - rn}) {
                            const auto word = gp_int_fp_opcode(source_wide, result_wide, unsign, scale, rd, rn);
                            GuardedFpTestKernel kernel(word);
                            GpIntFpOracle oracle(word);
                            auto [before, expected] = oracle.states(UINT64_MAX, 0x03000000U, 0xa8000000U);
                            auto state = before;
                            _mm_setcsr(host);
                            expect(kernel.execute(state) && integer_simd_same_state(state, expected) &&
                                       _mm_getcsr() == host,
                                   "GP conversion changed GPR/vector aliases, ZR, SP or upper bits");
                            for (const auto invalid_control : {1U << 8, 1U << 12, 1U << 15, 1U << 19}) {
                                state = before;
                                state.fpcr = invalid_control;
                                const auto trapped = state;
                                expect(!kernel.execute(state) && integer_simd_same_state(state, trapped) &&
                                           _mm_getcsr() == host,
                                       "GP conversion guard changed state before trap/unsupported-policy fallback");
                            }
                        }
                    }
                }
            }
        }
    }
#endif
}

void test_jit_gp_int_fp_regions_sessions() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    constexpr auto host = 0xbfe5U;
    for (const bool source_wide : {false, true}) {
        for (const bool result_wide : {false, true}) {
            for (const bool unsign : {false, true}) {
                for (const auto scale : {0U, 1U, source_wide ? 64U : 32U}) {
                    // Includes both measured PCs' exact register encodings.
                    const auto word = gp_int_fp_opcode(source_wide, result_wide, unsign,
                                                       scale, result_wide ? 20 : 17,
                                                       result_wide ? 8 : 12);
                    auto image = transmute::PeImage::parse(executable({
                        0xd51b4214U, 0xd51b4415U, 0xd51b4436U, word, 0xd65f03c0U}));
                    const auto initialize = [&](transmute::ReferenceEngine& engine) {
                        guarded_fp_initialize(engine, 0x03400000U, 0xa8000080U, 0, 1, 2,
                            {UINT64_MAX, UINT64_MAX}, {UINT64_MAX, UINT64_MAX});
                        engine.set_initial_register(result_wide ? 8 : 12, UINT64_MAX);
                    };
                    for (const auto budget : {3U, 4U, 5U}) {
                        transmute::ReferenceEngine oracle(image), runtime(image);
                        initialize(oracle);
                        initialize(runtime);
                        runtime.set_jit_enabled(true);
                        _mm_setcsr(host);
                        if (budget < 5) {
                            expect_failure([&] { static_cast<void>(oracle.run(budget)); },
                                           "guest exceeded instruction limit");
                            expect_failure([&] { static_cast<void>(runtime.run(budget)); },
                                           "guest exceeded instruction limit");
                        } else {
                            expect(oracle.run(budget).instructions == 5 &&
                                       runtime.run(budget).instructions == 5,
                                   "native GP conversion changed dynamic instruction accounting");
                        }
                        expect(integer_simd_same_state(runtime.state(), oracle.state()) &&
                                   runtime.jit_stats().native_region_instructions == budget - 3 &&
                                   _mm_getcsr() == host,
                               "native GP conversion changed exact budget, FPSR, upper bits or host state");
                    }
                    // A runtime policy guard must not turn the owner into a
                    // permanent negative entry. Callback restores its entry state.
                    transmute::ReferenceEngine oracle(image), runtime(image);
                    initialize(oracle);
                    initialize(runtime);
                    oracle.set_initial_register(21, 1U << 8);
                    runtime.set_initial_register(21, 1U << 8);
                    runtime.set_jit_enabled(true);
                    const auto empty = std::span<const std::uint64_t>{};
                    expect(runtime.invoke_callback(image.entry(), empty) ==
                               oracle.invoke_callback(image.entry(), empty),
                           "GP conversion warm policy fallback changed the oracle");
                    const auto warmed = runtime.jit_stats();
                    expect(warmed.compiled_native_regions == 1 && warmed.native_region_instructions == 1,
                           "GP conversion warm fallback lost its owner or native RET suffix");
                    oracle.set_initial_register(21, 0x03400000U);
                    runtime.set_initial_register(21, 0x03400000U);
                    expect(runtime.run().instructions == 5 && oracle.run().instructions == 5 &&
                               integer_simd_same_state(runtime.state(), oracle.state()) &&
                               runtime.jit_stats().compiled_native_regions == 1 &&
                               runtime.jit_stats().native_region_instructions == warmed.native_region_instructions + 2 &&
                               _mm_getcsr() == host,
                           "GP conversion could not execute natively after a warm runtime-policy bailout");
                }
            }
        }
    }
    // FDIV contributes IXC, conversion contributes IXC, and a later FZ-rejected
    // NaN/subnormal operation raises fresh native IOC which must stay private.
    const auto conversion = gp_int_fp_opcode(true, true, true, 64, 0, 12);
    constexpr auto prefix = 0x1e2718c4U; // FDIV S4,S6,S7
    constexpr auto rejected = 0x1e651862U; // FDIV D2,D3,D5
    const std::array words{prefix, conversion, rejected};
    NativeFpSessionTestKernel session(words);
    auto image = transmute::PeImage::parse(executable({
        0xd51b4214U, 0xd51b4415U, 0xd51b4436U, prefix, conversion, 0xd65f03c0U}));
    transmute::ReferenceEngine oracle(image);
    guarded_fp_initialize(oracle, 1U << 24, 0xa8000000U, 0, 1, 2,
        {UINT64_MAX, UINT64_MAX}, {UINT64_MAX, UINT64_MAX});
    oracle.set_initial_register(12, UINT64_MAX);
    oracle.set_initial_vector(3, {0x7ff0123456789abcULL, UINT64_MAX});
    oracle.set_initial_vector(5, {1, UINT64_MAX});
    oracle.set_initial_vector(6, {0x3f800000U, UINT64_MAX});
    oracle.set_initial_vector(7, {0x40400000U, UINT64_MAX});
    auto state = oracle.state();
    state.pc = image.entry() + 12;
    state.nzcv = 0xb0000000U;
    state.fpcr = 1U << 24;
    state.fpsr = 0xa8000000U;
    static_cast<void>(oracle.run());
    auto expected = oracle.state();
    expected.pc = state.pc;
    NativeFpSessionTestKernel::Context context;
    _mm_setcsr(host);
    expect(session.execute(state, context) == 2 && context.completed == 2 &&
               context.observed_fpsr == 0xa8000000U &&
               integer_simd_same_state(state, expected) && (state.fpsr & 0x11U) == 0x10U &&
               _mm_getcsr() == host,
           "GP conversion session lost committed prefix flags or committed a rejected suffix");
#endif
}
