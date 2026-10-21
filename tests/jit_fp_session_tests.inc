// Included after jit_fma_tests.inc inside test_main.cpp's anonymous namespace.

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
class NativeFpSessionTestKernel {
public:
    struct Context {
        std::array<std::uint64_t, 2> prefix{0x123456789abcdef0ULL, UINT64_MAX};
        std::uint64_t completed{};
        std::uint64_t observed_fpsr{};
        std::uint64_t canary{0xfedcba9876543210ULL};
        transmute::jit_detail::NativeFpScratch fp;
        std::array<std::uint64_t, 2> suffix{0x0123456789abcdefULL, UINT64_MAX};
    };
    static_assert(offsetof(Context, fp) == 40);

    explicit NativeFpSessionTestKernel(std::span<const std::uint32_t> opcodes,
        transmute::jit_detail::NativeFpEmissionMode mode =
            transmute::jit_detail::NativeFpEmissionMode::region_session) {
        std::vector<std::uint8_t> code{0x49, 0x89, 0xd3}; // mov r11,rdx
        std::vector<std::size_t> exits;
        for (const auto opcode : opcodes) {
            auto fragment = transmute::jit_detail::emit_guarded_fp(opcode, 40, mode);
            const auto begin = code.size();
            code.insert(code.end(), fragment.bytes.begin(), fragment.bytes.end());
            for (const auto displacement : fragment.bailout_displacements) {
                exits.push_back(begin + displacement);
            }
            code.insert(code.end(), {0x49, 0xff, 0x43, 16}); // inc completed
        }
        const auto exit = code.size();
        // Observe deferred FPSR while the session is still active, without a
        // host call or floating-point operation changing the live native CSR.
        code.insert(code.end(), {0x8b, 0x81});
        const auto fpsr_offset = static_cast<std::uint32_t>(offsetof(transmute::CpuState, fpsr));
        for (unsigned i = 0; i != 4; ++i) code.push_back(static_cast<std::uint8_t>(fpsr_offset >> (i * 8)));
        code.insert(code.end(), {0x49, 0x89, 0x43, 24}); // observed_fpsr
        const auto finish = transmute::jit_detail::emit_fp_session_finish(40);
        code.insert(code.end(), finish.begin(), finish.end());
        code.insert(code.end(), {0x49, 0x8b, 0x43, 16, 0xc3}); // return completed
        for (const auto displacement : exits) {
            put32(code, displacement, static_cast<std::uint32_t>(exit - displacement - 4));
        }
        address_ = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!address_) throw std::runtime_error("could not allocate native FP session test kernel");
        auto* output = static_cast<std::uint8_t*>(address_);
        for (std::size_t i = 0; i != code.size(); ++i) output[i] = code[i];
        DWORD previous{};
        if (!VirtualProtect(address_, code.size(), PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), address_, code.size())) {
            VirtualFree(address_, 0, MEM_RELEASE);
            address_ = nullptr;
            throw std::runtime_error("could not finalize native FP session test kernel");
        }
    }
    ~NativeFpSessionTestKernel() { VirtualFree(address_, 0, MEM_RELEASE); }
    NativeFpSessionTestKernel(const NativeFpSessionTestKernel&) = delete;
    NativeFpSessionTestKernel& operator=(const NativeFpSessionTestKernel&) = delete;

    std::uint64_t execute(transmute::CpuState& state, Context& context) const {
        const auto prefix = context.prefix, suffix = context.suffix;
        const auto canary = context.canary;
        using Function = std::uint64_t (*)(transmute::CpuState*, Context*);
        const auto completed = reinterpret_cast<Function>(address_)(&state, &context);
        expect(context.prefix == prefix && context.suffix == suffix && context.canary == canary,
               "native FP session overwrote adjacent context fields");
        expect(context.fp.session_active == 0,
               "native FP session returned without restoring host state");
        return completed;
    }
private:
    void* address_{};
};
#endif

void test_jit_fp_session_layout() {
    using transmute::jit_detail::NativeFpScratch;
    expect(sizeof(NativeFpScratch) == 32 && offsetof(NativeFpScratch, saved_mxcsr) == 0 &&
               offsetof(NativeFpScratch, work_mxcsr) == 4 && offsetof(NativeFpScratch, result) == 8 &&
               offsetof(NativeFpScratch, committed_mxcsr) == 24 &&
               offsetof(NativeFpScratch, session_active) == 28,
           "native FP session scratch moved a stable field or changed its ABI");
    bool rejected = false;
    try {
        static_cast<void>(transmute::jit_detail::emit_fp_session_finish(UINT32_MAX));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "native FP session accepted an invalid scratch displacement");
}

void test_jit_fp_sessions() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    std::size_t committed = 0;
    const auto run_form = [&](GuardedFpTestForm form, bool fused) {
        const auto opcode = fused ? guarded_fma_test_opcode(form, 0, 1, 2, 3)
                                  : guarded_fp_test_opcode(form, 0, 1, 2);
        const std::array opcodes{opcode, opcode, opcode};
        NativeFpSessionTestKernel kernel(opcodes);
        auto image = transmute::PeImage::parse(executable({
            0xd51b4214U, 0xd51b4415U, 0xd51b4436U,
            opcode, opcode, opcode, 0xd65f03c0U}));
        for (unsigned rounding = 0; rounding != 4; ++rounding) {
            for (const auto controls : {0U, 0x03000000U}) {
                const auto fpcr = (rounding << 22) | controls;
                constexpr auto fpsr = 0xa8000080U;
                transmute::ReferenceEngine oracle(image);
                const auto left = form.wide ? 0x3ff0000000000001ULL : 0x3f800001ULL;
                const auto right = form.wide ? 0x4008000000000000ULL : 0x40400000ULL;
                guarded_fp_initialize(oracle, fpcr, fpsr, 0, 1, 2,
                    guarded_fp_test_vector(form, left, right),
                    guarded_fp_test_vector(form, right, left));
                if (fused) oracle.set_initial_vector(form.scalar ? 3 : 0,
                    guarded_fp_test_vector(form, left, left));
                auto native = oracle.state();
                native.pc = image.entry() + 12;
                native.nzcv = 0xb0000000U;
                native.fpcr = fpcr;
                native.fpsr = fpsr;
                static_cast<void>(oracle.run());
                auto expected = oracle.state();
                expected.pc = native.pc;
                NativeFpSessionTestKernel::Context context;
                const auto host = 0x1fbfU | ((3U - rounding) << 13) |
                    ((rounding & 1U) != 0 ? 0x8000U : 0) | (restore.saved & 0x40U);
                _mm_setcsr(host);
                const auto count = kernel.execute(native, context);
                expect(count == 3 && context.completed == 3 && context.observed_fpsr == fpsr &&
                           integer_simd_same_state(native, expected) && _mm_getcsr() == host,
                       "native FP session changed finite results, rounding or deferred sticky flags");
                ++committed;
            }
        }
    };
    for (const auto form : guarded_fp_test_forms) run_form(form, false);
    if (transmute::jit_detail::native_fma_available()) {
        for (const auto form : guarded_fma_test_forms) run_form(form, true);
    }
    expect(committed >= guarded_fp_test_forms.size() * 8,
           "native FP session matrix missed baseline arrangements");

    // Different operations contribute every IEEE exception bit; an exact last
    // instruction must retain those earlier flags until the common exit.
    const std::array opcodes{0x1e621820U, 0x1e641865U, 0x1e6708c8U,
                            0x1e6a192bU, 0x1e6d198eU, 0x1e7029f1U};
    NativeFpSessionTestKernel kernel(opcodes);
    auto image = transmute::PeImage::parse(executable({
        0xd51b4214U, 0xd51b4415U, 0xd51b4436U,
        opcodes[0], opcodes[1], opcodes[2], opcodes[3], opcodes[4], opcodes[5], 0xd65f03c0U}));
    transmute::ReferenceEngine oracle(image);
    guarded_fp_initialize(oracle, 0, 0xa8000080U, 0, 1, 2, {0, UINT64_MAX}, {0, UINT64_MAX});
    for (const auto reg : {3U, 12U, 15U}) oracle.set_initial_vector(reg, {0x3ff0000000000000ULL, UINT64_MAX});
    oracle.set_initial_vector(4, {0, UINT64_MAX});
    oracle.set_initial_vector(6, {0x7fefffffffffffffULL, UINT64_MAX});
    for (const auto reg : {7U, 16U}) oracle.set_initial_vector(reg, {0x4000000000000000ULL, UINT64_MAX});
    oracle.set_initial_vector(9, {0x0010000000000000ULL, UINT64_MAX});
    for (const auto reg : {10U, 13U}) oracle.set_initial_vector(reg, {0x4008000000000000ULL, UINT64_MAX});
    auto native = oracle.state();
    native.pc = image.entry() + 12;
    native.nzcv = 0xb0000000U;
    native.fpsr = 0xa8000080U;
    static_cast<void>(oracle.run());
    auto expected = oracle.state();
    expected.pc = native.pc;
    const auto host = 0xffbfU | (restore.saved & 0x40U);
    _mm_setcsr(host);
    NativeFpSessionTestKernel::Context context;
    expect(kernel.execute(native, context) == opcodes.size() &&
               context.observed_fpsr == 0xa8000080U && (native.fpsr & 31U) == 31U &&
               guarded_fp_same_allowed_state(native, expected,
                   GuardedFpTestForm{0x1e201800U, true, true, false}, 0) && _mm_getcsr() == host,
           "native FP session lost an earlier exception or exceeded the NaN representation policy");
#endif
}

void test_jit_fp_session_failed_instruction() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    // Prefix FDIV commits IXC. A later FZ underflow (or conservative minimum
    // normal guard after prior IXC) must not commit its Rd, flags or count.
    const std::array opcodes{0x1e221824U, 0x1e2708c0U, 0x1e222828U};
    NativeFpSessionTestKernel kernel(opcodes);
    auto prefix_image = transmute::PeImage::parse(executable({
        0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcodes[0], 0xd65f03c0U}));
    for (const bool flush : {false, true}) {
        const auto fpcr = flush ? 1U << 24 : 0;
        transmute::ReferenceEngine oracle(prefix_image);
        guarded_fp_initialize(oracle, fpcr, 0xa8000080U, 0, 1, 2,
            {0x3f800000U, UINT64_MAX}, {0x40400000U, UINT64_MAX});
        oracle.set_initial_vector(6, {0x00800000U, UINT64_MAX});
        oracle.set_initial_vector(7, {flush ? 0x3f000000U : 0x3f800000U, UINT64_MAX});
        auto native = oracle.state();
        native.pc = prefix_image.entry() + 12;
        native.nzcv = 0xb0000000U;
        native.fpcr = fpcr;
        native.fpsr = 0xa8000080U;
        static_cast<void>(oracle.run());
        auto expected = oracle.state();
        expected.pc = native.pc;
        const auto host = 0xdfbfU | (restore.saved & 0x40U);
        _mm_setcsr(host);
        NativeFpSessionTestKernel::Context context;
        expect(kernel.execute(native, context) == 1 && context.completed == 1 &&
                   context.observed_fpsr == 0xa8000080U &&
                   integer_simd_same_state(native, expected) && (native.fpsr & 0x1eU) == 0x10U &&
                   _mm_getcsr() == host,
               "failed FP session instruction committed its result/status or discarded its prefix");
    }
    transmute::CpuState trapped{};
    trapped.fpcr = 1U << 8;
    trapped.fpsr = 0xa8000080U;
    const auto before = trapped;
    NativeFpSessionTestKernel::Context context;
    const auto host = _mm_getcsr();
    expect(kernel.execute(trapped, context) == 0 && integer_simd_same_state(trapped, before) &&
               _mm_getcsr() == host,
           "first FP session guard changed host or architectural state");
    // Finishing a session that never began is idempotent, even with stale flags
    // in its private scratch. Such flags must not be observed by the guest.
    NativeFpSessionTestKernel empty(std::span<const std::uint32_t>{});
    context.fp.committed_mxcsr = 0x1fbfU;
    expect(empty.execute(trapped, context) == 0 && integer_simd_same_state(trapped, before) &&
               empty.execute(trapped, context) == 0 && _mm_getcsr() == host,
           "inactive FP session finish merged stale flags or changed host state");
#endif
}

void benchmark_jit_fp_sessions() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    GuardedFpHostState restore;
    std::array<std::uint32_t, 64> opcodes{};
    opcodes.fill(0x4e22d420U); // FADD V0.4S,V1.4S,V2.4S
    NativeFpSessionTestKernel isolated(opcodes, transmute::jit_detail::NativeFpEmissionMode::isolated);
    NativeFpSessionTestKernel session(opcodes);
    transmute::CpuState initial{};
    initial.v[1] = {0x3f8000003f800000ULL, 0x3f8000003f800000ULL};
    initial.v[2] = {0x3380000033800000ULL, 0x3380000033800000ULL};
    constexpr std::uint64_t invocations = 100000;
    const auto measure = [&](const auto& kernel) {
        auto state = initial;
        std::uint64_t completed = 0;
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i != invocations; ++i) {
            NativeFpSessionTestKernel::Context context;
            completed += kernel.execute(state, context);
        }
        const auto duration = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        expect(completed == invocations * opcodes.size() && (state.fpsr & 0x10U) != 0,
               "native FP session benchmark missed instructions or sticky status");
        return std::pair{duration, state};
    };
    const auto baseline = measure(isolated), optimized = measure(session);
    expect(integer_simd_same_state(baseline.second, optimized.second),
           "native FP session benchmark changed finite arithmetic");
    std::cout << "native_fp_session instructions=" << invocations * opcodes.size()
              << " isolated_ms=" << baseline.first << " session_ms=" << optimized.first << '\n';
#endif
}
