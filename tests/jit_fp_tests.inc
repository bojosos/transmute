// Included after jit_simd_tests.inc inside test_main.cpp's anonymous namespace.
// test_main.cpp includes ../src/jit_fp.hpp and xmmintrin.h outside it.

struct GuardedFpTestForm {
    std::uint32_t base;
    bool scalar, wide, quad;
};

constexpr std::array guarded_fp_test_forms{
    GuardedFpTestForm{0x1e202800U, true, false, false},
    GuardedFpTestForm{0x1e202800U, true, true, false},
    GuardedFpTestForm{0x1e203800U, true, false, false},
    GuardedFpTestForm{0x1e203800U, true, true, false},
    GuardedFpTestForm{0x1e200800U, true, false, false},
    GuardedFpTestForm{0x1e200800U, true, true, false},
    GuardedFpTestForm{0x1e201800U, true, false, false},
    GuardedFpTestForm{0x1e201800U, true, true, false},
    GuardedFpTestForm{0x0e20d400U, false, false, false},
    GuardedFpTestForm{0x0e20d400U, false, false, true},
    GuardedFpTestForm{0x0e20d400U, false, true, true},
    GuardedFpTestForm{0x0ea0d400U, false, false, false},
    GuardedFpTestForm{0x0ea0d400U, false, false, true},
    GuardedFpTestForm{0x0ea0d400U, false, true, true},
    GuardedFpTestForm{0x2e20dc00U, false, false, false},
    GuardedFpTestForm{0x2e20dc00U, false, false, true},
    GuardedFpTestForm{0x2e20dc00U, false, true, true},
};

std::uint32_t guarded_fp_test_opcode(GuardedFpTestForm form,
                                     unsigned rd, unsigned rn, unsigned rm) {
    return form.base | (form.wide ? 0x00400000U : 0) |
        (form.quad ? 0x40000000U : 0) | rd | (rn << 5) | (rm << 16);
}

std::array<std::uint64_t, 2> guarded_fp_test_vector(
    GuardedFpTestForm form, std::uint64_t low, std::uint64_t high) {
    if (form.scalar) {
        // Non-finite garbage in inactive bits must not reject a scalar op.
        return {form.wide ? low : 0x7fc0012300000000ULL | low, UINT64_MAX};
    }
    if (form.wide) return {low, high};
    return {low | (high << 32), form.quad ? high | (low << 32) : UINT64_MAX};
}

bool guarded_fp_same_allowed_state(const transmute::CpuState& actual,
                                    const transmute::CpuState& expected,
                                    GuardedFpTestForm form, unsigned rd) {
    auto normalized = actual;
    const auto bits = form.wide ? 64U : 32U;
    const auto fraction = form.wide ? 52U : 23U;
    const auto exponent = form.wide ? 0x7ffULL : 0xffULL;
    const auto fraction_mask = (1ULL << fraction) - 1;
    const auto mask = form.wide ? UINT64_MAX : 0xffffffffULL;
    const auto lanes = form.scalar ? 1U : (form.quad ? 128U : 64U) / bits;
    for (unsigned lane = 0; lane != lanes; ++lane) {
        const auto word = lane * bits / 64;
        const auto shift = lane * bits % 64;
        const auto a = (actual.v[rd][word] >> shift) & mask;
        const auto e = (expected.v[rd][word] >> shift) & mask;
        const bool actual_nan = ((a >> fraction) & exponent) == exponent &&
            (a & fraction_mask) != 0;
        const bool expected_nan = ((e >> fraction) & exponent) == exponent &&
            (e & fraction_mask) != 0;
        if (actual_nan && expected_nan) {
            normalized.v[rd][word] = (normalized.v[rd][word] & ~(mask << shift)) |
                (e << shift);
        }
    }
    return integer_simd_same_state(normalized, expected);
}

void test_jit_guarded_fp_predicate() {
    for (const auto form : guarded_fp_test_forms) {
        const auto word = guarded_fp_test_opcode(form, 31, 30, 29);
        expect(transmute::jit_detail::accepts_guarded_fp(word),
               "guarded native FP rejected a implemented S/D arrangement");
        expect(!transmute::jit_detail::accepts_guarded_fp(word | 0x80000000U),
               "guarded native FP accepted a reserved top bit");
    }
    for (const auto word : {0x1ea22820U, 0x1ee22820U, 0x0e62d420U,
                            0x0ee2d420U, 0x2e62dc20U, 0x4e22fc20U,
                            0x1fa20c20U, 0x1e224820U, 0xffffffffU}) {
        expect(!transmute::jit_detail::accepts_guarded_fp(word),
               "guarded native FP claimed half precision or an unimplemented form");
        bool rejected = false;
        try {
            static_cast<void>(transmute::jit_detail::emit_guarded_fp(word, 40));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        expect(rejected, "guarded native FP emitter accepted an invalid encoding");
    }
    bool rejected = false;
    try {
        static_cast<void>(transmute::jit_detail::emit_guarded_fp(0x1e222820U, UINT32_MAX));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "guarded native FP accepted a scratch displacement outside disp32");
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
struct GuardedFpHostState {
    unsigned saved{_mm_getcsr()};
    ~GuardedFpHostState() { _mm_setcsr(saved); }
};

class GuardedFpTestKernel {
public:
    struct Context {
        std::array<std::uint64_t, 5> prefix;
        transmute::jit_detail::NativeFpScratch fp;
    };
    static_assert(offsetof(Context, fp) == 40);

    explicit GuardedFpTestKernel(std::uint32_t opcode) {
        auto fragment = transmute::jit_detail::emit_guarded_fp(opcode, 40);
        std::vector<std::uint8_t> code{0x49, 0x89, 0xd3}; // mov r11,rdx
        code.insert(code.end(), fragment.bytes.begin(), fragment.bytes.end());
        code.insert(code.end(), {0xb8, 1, 0, 0, 0, 0xc3}); // success true
        const auto failure = code.size();
        code.insert(code.end(), {0x31, 0xc0, 0xc3}); // guard false
        for (const auto offset : fragment.bailout_displacements) {
            const auto displacement = offset + 3;
            put32(code, displacement,
                  static_cast<std::uint32_t>(failure - displacement - 4));
        }
        address_ = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!address_) throw std::runtime_error("could not allocate guarded FP test kernel");
        auto* output = static_cast<std::uint8_t*>(address_);
        for (std::size_t i = 0; i != code.size(); ++i) output[i] = code[i];
        DWORD previous{};
        if (!VirtualProtect(address_, code.size(), PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), address_, code.size())) {
            VirtualFree(address_, 0, MEM_RELEASE);
            address_ = nullptr;
            throw std::runtime_error("could not finalize guarded FP test kernel");
        }
    }
    ~GuardedFpTestKernel() { VirtualFree(address_, 0, MEM_RELEASE); }
    GuardedFpTestKernel(const GuardedFpTestKernel&) = delete;
    GuardedFpTestKernel& operator=(const GuardedFpTestKernel&) = delete;

    bool execute(transmute::CpuState& state) const {
        Context context{{0x1111111111111111ULL, 0x2222222222222222ULL,
                         0x3333333333333333ULL, 0x4444444444444444ULL,
                         0x5555555555555555ULL}, {}};
        const auto prefix = context.prefix;
        using Function = bool (*)(transmute::CpuState*, Context*);
        const auto accepted = reinterpret_cast<Function>(address_)(&state, &context);
        expect(context.prefix == prefix,
               "guarded FP fragment overwrote another region context field");
        return accepted;
    }
private:
    void* address_{};
};

void guarded_fp_initialize(transmute::ReferenceEngine& engine,
                            std::uint32_t fpcr, std::uint32_t fpsr,
                            unsigned rd, unsigned rn, unsigned rm,
                            std::array<std::uint64_t, 2> left,
                            std::array<std::uint64_t, 2> right) {
    for (unsigned reg = 0; reg != 30; ++reg) {
        engine.set_initial_register(reg, 0x1234567800000000ULL + reg * 0x1020304ULL);
    }
    engine.set_initial_register(20, 0xb0000000U);
    engine.set_initial_register(21, fpcr);
    engine.set_initial_register(22, fpsr);
    for (unsigned reg = 0; reg != 32; ++reg) {
        engine.set_initial_vector(reg,
            {0xa5a5000000000000ULL + reg, 0x5a5affffffffffffULL - reg});
    }
    engine.set_initial_vector(rd, {0xdeadbeefcafef00dULL, UINT64_MAX});
    engine.set_initial_vector(rn, left);
    engine.set_initial_vector(rm, right);
}
#endif

void test_jit_guarded_fp() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    struct Registers { unsigned destination, source, second; };
    constexpr std::array register_forms{
        Registers{0, 1, 2}, Registers{1, 1, 2}, Registers{2, 1, 2},
        Registers{0, 1, 1}, Registers{1, 1, 1},
        Registers{31, 31, 30}, Registers{30, 31, 30}};
    struct Values { std::uint64_t left, right, other_left, other_right; };
    constexpr std::array singles{
        Values{0x3fc00000U, 0x40000000U, 0x3f800000U, 0x40400000U},
        Values{0, 0x40000000U, 0x80000000U, 0x3f800000U},
        Values{0x80000000U, 0x40000000U, 0x3f800000U, 0x80000000U},
        Values{0x3f800000U, 0x33800000U, 0xbf800000U, 0x33800000U},
        Values{0x3f800001U, 0x3f800001U, 0x40000000U, 0x40000000U},
        Values{0x00800000U, 0x3f7fffffU, 0x3f000000U, 0x00800000U},
    };
    constexpr std::array doubles{
        Values{0x3ff8000000000000ULL, 0x4000000000000000ULL,
               0x3ff0000000000000ULL, 0x4008000000000000ULL},
        Values{0, 0x4000000000000000ULL, 0x8000000000000000ULL, 0x3ff0000000000000ULL},
        Values{0x8000000000000000ULL, 0x4000000000000000ULL,
               0x3ff0000000000000ULL, 0x8000000000000000ULL},
        Values{0x3ff0000000000000ULL, 0x3ca0000000000000ULL,
               0xbff0000000000000ULL, 0x3ca0000000000000ULL},
        Values{0x3ff0000000000001ULL, 0x3ff0000000000001ULL,
               0x4000000000000000ULL, 0x4000000000000000ULL},
        Values{0x0010000000000000ULL, 0x3fefffffffffffffULL,
               0x3fe0000000000000ULL, 0x0010000000000000ULL},
    };
    std::size_t accepted = 0, rejected = 0;
    for (const auto form : guarded_fp_test_forms) {
        for (const auto registers : register_forms) {
            const auto opcode = guarded_fp_test_opcode(form, registers.destination,
                registers.source, registers.second);
            GuardedFpTestKernel kernel(opcode);
            auto image = transmute::PeImage::parse(executable({
                0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcode, 0xd65f03c0U}));
            for (unsigned rounding = 0; rounding != 4; ++rounding) {
                for (const auto controls : {0U, 0x03000000U}) {
                    for (const auto values : (form.wide ? doubles : singles)) {
                        transmute::ReferenceEngine oracle(image);
                        const auto fpcr = (rounding << 22) | controls;
                        constexpr auto fpsr = 0xa800008fU; // IXC initially clear
                        guarded_fp_initialize(oracle, fpcr, fpsr,
                            registers.destination, registers.source, registers.second,
                            guarded_fp_test_vector(form, values.left, values.other_left),
                            guarded_fp_test_vector(form, values.right, values.other_right));
                        auto before = oracle.state();
                        before.pc = image.entry() + 12;
                        before.nzcv = 0xb0000000U;
                        before.fpcr = fpcr;
                        before.fpsr = fpsr;
                        static_cast<void>(oracle.run());
                        auto expected = oracle.state();
                        expected.pc = before.pc; // fragment leaves PC/accounting to region
                        auto native = before;
                        const auto host = 0x1fbfU | ((3U - rounding) << 13) |
                            ((rounding & 1U) != 0 ? 0x8000U : 0) |
                            (restore.saved & 0x40U);
                        _mm_setcsr(host);
                        const auto success = kernel.execute(native);
                        expect(_mm_getcsr() == host,
                               "guarded FP fragment leaked host rounding, flush or exception state");
                        if (success) {
                            ++accepted;
                            expect(integer_simd_same_state(native, expected),
                                   "guarded native FP differs from exact bits/FPSR oracle");
                        } else {
                            ++rejected;
                            expect(integer_simd_same_state(native, before),
                                   "guarded native FP changed architectural state before bailout");
                        }
                    }
                }
            }
        }
    }
    expect(accepted > 1000 && rejected > 0,
           "guarded FP differential matrix did not exercise both commit and bailout paths");
#endif
}

void test_jit_guarded_fp_edges() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    for (const auto form : guarded_fp_test_forms) {
        const auto opcode = guarded_fp_test_opcode(form, 0, 1, 2);
        GuardedFpTestKernel kernel(opcode);
        const auto one = form.wide ? 0x3ff0000000000000ULL : 0x3f800000ULL;
        const auto two = form.wide ? 0x4000000000000000ULL : 0x40000000ULL;
        const auto infinity = form.wide ? 0x7ff0000000000000ULL : 0x7f800000ULL;
        const auto quiet_nan = form.wide ? 0xfff8123456789abcULL : 0xffc12345ULL;
        const auto signaling_nan = form.wide ? 0x7ff0123456789abcULL : 0x7f812345ULL;
        const auto minimum = form.wide ? 0x0010000000000000ULL : 0x00800000ULL;
        auto image = transmute::PeImage::parse(executable({
            0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcode, 0xd65f03c0U}));
        for (const auto fpcr : {0U, 0x03000000U, 0x00400000U, 0x00800000U, 0x00c00000U}) {
            for (const auto bad : {1ULL, minimum - 1, infinity, quiet_nan, signaling_nan}) {
                for (const bool bad_right : {false, true}) {
                    transmute::ReferenceEngine oracle(image);
                    guarded_fp_initialize(oracle, fpcr, 0xa8000080U, 0, 1, 2,
                        guarded_fp_test_vector(form, bad_right ? one : bad, one),
                        guarded_fp_test_vector(form, bad_right ? bad : one, one));
                    auto state = oracle.state();
                    state.pc = image.entry() + 12;
                    state.nzcv = 0xb0000000U;
                    state.fpcr = fpcr;
                    state.fpsr = 0xa8000080U;
                    const auto before = state;
                    static_cast<void>(oracle.run());
                    auto expected = oracle.state();
                    expected.pc = before.pc;
                    _mm_setcsr(0xbfbfU | (restore.saved & 0x40U));
                    const auto accepted = kernel.execute(state);
                    expect(_mm_getcsr() == (0xbfbfU | (restore.saved & 0x40U)) &&
                               (accepted ? guarded_fp_same_allowed_state(state, expected, form, 0)
                                         : integer_simd_same_state(state, before)),
                           "direct FP special-value result exceeded NaN policy or leaked state");
                    if (bad == infinity || bad == quiet_nan || bad == signaling_nan) {
                        expect(accepted, "direct FP retained a NaN/infinity operand bailout");
                    }
                }
            }
        }
        for (const auto control : {1U, 2U, 1U << 8, 1U << 9, 1U << 10,
                                   1U << 11, 1U << 12, 1U << 15, 1U << 19}) {
            transmute::CpuState state{};
            state.fpcr = control;
            state.fpsr = 0xa800009fU;
            state.v[1] = guarded_fp_test_vector(form, one, one);
            state.v[2] = guarded_fp_test_vector(form, two, two);
            const auto before = state;
            const auto host = _mm_getcsr();
            expect(!kernel.execute(state) && integer_simd_same_state(state, before) &&
                       _mm_getcsr() == host,
                   "guarded FP accepted trap/unsupported controls or changed host state");
        }
        // FZ output and pre-round-tininess failures retain the precise oracle.
        if (form.base == 0x1e200800U || form.base == 0x2e20dc00U) {
            const auto half = form.wide ? 0x3fe0000000000000ULL : 0x3f000000ULL;
            const auto below_one = form.wide ? 0x3fefffffffffffffULL : 0x3f7fffffULL;
            for (const bool flush : {false, true}) {
                transmute::CpuState state{};
                state.fpcr = flush ? 1U << 24 : 0;
                state.fpsr = 0xa8000001U;
                state.v[0] = {0x0123456789abcdefULL, UINT64_MAX};
                state.v[1] = guarded_fp_test_vector(form, minimum, minimum);
                state.v[2] = guarded_fp_test_vector(form, flush ? half : below_one,
                                                               flush ? half : below_one);
                const auto before = state;
                const auto host = _mm_getcsr();
                expect(!kernel.execute(state) && integer_simd_same_state(state, before) &&
                           _mm_getcsr() == host,
                       "direct FP committed FZ/tininess result before precise bailout");
            }
        }
        if (!form.scalar) {
            transmute::ReferenceEngine oracle(image);
            guarded_fp_initialize(oracle, 0, 0xa8000080U, 0, 1, 2,
                guarded_fp_test_vector(form, one, quiet_nan),
                guarded_fp_test_vector(form, two, one));
            auto state = oracle.state();
            state.pc = image.entry() + 12;
            state.nzcv = 0xb0000000U;
            state.fpsr = 0xa8000080U;
            const auto before = state;
            static_cast<void>(oracle.run());
            auto expected = oracle.state();
            expected.pc = before.pc;
            const auto host = _mm_getcsr();
            expect(kernel.execute(state) && guarded_fp_same_allowed_state(state, expected, form, 0) &&
                       _mm_getcsr() == host,
                   "packed FP rejected a NaN lane or exceeded native representation policy");
        }
    }
#endif
}
