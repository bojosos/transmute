// Included after jit_fp_compare_tests.inc inside test_main.cpp's anonymous namespace.
// test_main.cpp includes ../src/jit_atomic.hpp outside the namespace.

std::uint32_t native_lse_test_opcode(transmute::jit_detail::NativeLseOperation op,
                                    unsigned width, unsigned ordering,
                                    unsigned rs, unsigned rt, unsigned rn) {
    using transmute::jit_detail::NativeLseOperation;
    const unsigned size = width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3;
    auto word = op == NativeLseOperation::compare_swap ? 0x08a07c00U
        : op == NativeLseOperation::swap ? 0x38208000U
        : 0x38200000U | (static_cast<unsigned>(op) << 12);
    if ((ordering & 1U) != 0) word |= op == NativeLseOperation::compare_swap ? 0x00400000U : 0x00800000U;
    if ((ordering & 2U) != 0) word |= op == NativeLseOperation::compare_swap ? 0x00008000U : 0x00400000U;
    return word | (size << 30) | (rs << 16) | (rn << 5) | rt;
}

void test_native_lse_predicate() {
    using transmute::jit_detail::NativeLseOperation;
    for (unsigned operation = 0; operation != 10; ++operation) {
        for (const auto width : {1U, 2U, 4U, 8U}) {
            for (unsigned ordering = 0; ordering != 4; ++ordering) {
                const auto op = static_cast<NativeLseOperation>(operation);
                const auto word = native_lse_test_opcode(op, width, ordering, 31, 30, 31);
                const auto decoded = transmute::jit_detail::decode_native_lse(word);
                expect(decoded && decoded->operation == op && decoded->bytes == width &&
                           decoded->source == 31 && decoded->target == 30 && decoded->base == 31 &&
                           decoded->acquire == ((ordering & 1U) != 0) &&
                           decoded->release == ((ordering & 2U) != 0) &&
                           transmute::jit_detail::accepts_native_lse(word),
                       "native LSE decoder changed width, ordering, SP or ZR semantics");
            }
        }
    }
    // Pair-CAS, exclusives, ordered plain accesses and reserved SWP/CAS bits
    // must remain outside this single-element capability.
    for (const auto word : {0x08207c82U, 0x4860fc82U, 0xc85f7c41U, 0xc8007c41U,
                            0xc8dffc41U, 0xc89ffc41U, 0x38209041U, 0x38208441U,
                            0x08a07841U, 0x1e222020U, 0xffffffffU}) {
        expect(!transmute::jit_detail::accepts_native_lse(word),
               "native LSE admitted an unsupported or reserved form");
        bool rejected = false;
        try {
            static_cast<void>(transmute::jit_detail::emit_native_lse(word));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        expect(rejected, "native LSE emitter accepted an invalid instruction");
    }
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
class NativeLseTestKernel {
public:
    struct Context {
        std::uint64_t state{}, memory{};
        std::array<std::uint64_t, 2> canaries{UINT64_MAX, 0x123456789abcdef0ULL};
    };
    explicit NativeLseTestKernel(std::uint32_t opcode) {
        const auto fragment = transmute::jit_detail::emit_native_lse(opcode);
        std::vector<std::uint8_t> code{
            0x4d, 0x89, 0xc3, // mov r11,r8 (private context)
            0x49, 0x89, 0x0b, // save caller CpuState pointer
            0x49, 0x89, 0x53, 8}; // save validated span pointer
        code.insert(code.end(), fragment.begin(), fragment.end());
        code.insert(code.end(), {
            0x44, 0x89, 0xc8, // mov eax,r9d, private wrote flag
            0x49, 0x3b, 0x0b, 0x75, 7, // cmp rcx,[r11]; jne failure
            0x49, 0x3b, 0x53, 8, 0x75, 1, // cmp rdx,[r11+8]; jne failure
            0xc3, // success returns wrote
            0xb8, 2, 0, 0, 0, 0xc3}); // failure returns invalid wrote=2
        address_ = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!address_) throw std::runtime_error("could not allocate native LSE test kernel");
        auto* output = static_cast<std::uint8_t*>(address_);
        for (std::size_t i = 0; i != code.size(); ++i) output[i] = code[i];
        DWORD previous{};
        if (!VirtualProtect(address_, code.size(), PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), address_, code.size())) {
            VirtualFree(address_, 0, MEM_RELEASE);
            address_ = nullptr;
            throw std::runtime_error("could not finalize native LSE test kernel");
        }
    }
    ~NativeLseTestKernel() { VirtualFree(address_, 0, MEM_RELEASE); }
    NativeLseTestKernel(const NativeLseTestKernel&) = delete;
    NativeLseTestKernel& operator=(const NativeLseTestKernel&) = delete;

    bool execute(transmute::CpuState& state, std::uint8_t* memory) const {
        Context context;
        const auto canaries = context.canaries;
        using Function = unsigned (*)(transmute::CpuState*, std::uint8_t*, Context*);
        const auto wrote = reinterpret_cast<Function>(address_)(&state, memory, &context);
        expect(wrote <= 1 && context.state == reinterpret_cast<std::uintptr_t>(&state) &&
                   context.memory == reinterpret_cast<std::uintptr_t>(memory) && context.canaries == canaries,
               "native LSE fragment lost RCX/RDX/R11 or adjacent private context state");
        return wrote != 0;
    }
private:
    void* address_{};
};
#endif

void test_native_lse_direct() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    GuardedFpHostState restore;
    struct Registers { unsigned source, target, base; };
    constexpr std::array<Registers, 10> registers{
        Registers{0, 1, 2}, {0, 0, 2}, {0, 2, 2}, {2, 1, 2}, {2, 2, 2},
        {31, 1, 2}, {0, 31, 2}, {31, 31, 31}, {30, 30, 31}, {30, 1, 30}};
    std::size_t cases = 0, successful_cas = 0, failed_cas = 0;
    for (unsigned operation = 0; operation != 10; ++operation) {
        const auto op = static_cast<NativeLseOperation>(operation);
        for (const auto width : {1U, 2U, 4U, 8U}) {
            const auto mask = width == 8 ? UINT64_MAX : (1ULL << (width * 8)) - 1;
            const auto sign = 1ULL << (width * 8 - 1);
            const std::array<std::uint64_t, 6> patterns{
                0, 1, UINT64_MAX, sign, sign - 1, 0xa55af00fff0000ffULL};
            const std::array<std::uint64_t, 6> old_values{mask, mask, sign, sign - 1, 0, 1};
            for (unsigned ordering = 0; ordering != 4; ++ordering) {
                for (const auto reg : registers) {
                    const auto opcode = native_lse_test_opcode(op, width, ordering,
                        reg.source, reg.target, reg.base);
                    NativeLseTestKernel kernel(opcode);
                    auto image = transmute::PeImage::parse(executable({
                        0xd51b4214U, 0xd51b4415U, 0xd51b4436U, opcode, 0xd65f0300U})); // RET X24
                    for (unsigned sample = 0; sample != patterns.size(); ++sample) {
                        transmute::ReferenceEngine oracle(image);
                        const auto return_address = oracle.state().x[30];
                        constexpr auto mapped = 0x501000000ULL;
                        oracle.map_memory(mapped, 16);
                        const auto address = reg.base == 31 ? oracle.state().sp : mapped + 8;
                        if (reg.base == 31) oracle.map_memory(address, 16);
                        const auto guard_begin = address - 8;
                        for (unsigned index = 0; index != 30; ++index)
                            oracle.set_initial_register(index, 0x1234567800000000ULL + index * 0x1020304ULL);
                        for (unsigned index = 0; index != 32; ++index)
                            oracle.set_initial_vector(index, {0x0123456789abcdefULL + index, UINT64_MAX - index});
                        if (reg.source != 31) oracle.set_initial_register(reg.source, patterns[sample]);
                        if (reg.target != 31) oracle.set_initial_register(reg.target, patterns[(sample + 1) % patterns.size()]);
                        if (reg.base != 31) oracle.set_initial_register(reg.base, address);
                        oracle.set_initial_register(20, 0xb0000000U);
                        oracle.set_initial_register(21, 0x03c00000U);
                        oracle.set_initial_register(22, 0xa800009fU);
                        oracle.set_initial_register(24, return_address);
                        const auto expected_cas = reg.source == 31 ? 0 : oracle.state().x[reg.source] & mask;
                        const bool cas_match = (sample & 1U) == 0;
                        const auto old = op == NativeLseOperation::compare_swap
                            ? (cas_match ? expected_cas : (expected_cas + 1) & mask)
                            : old_values[sample];
                        alignas(16) std::array<std::uint8_t, 16> memory{};
                        for (unsigned index = 0; index != memory.size(); ++index) {
                            memory[index] = static_cast<std::uint8_t>(0xa5U + index * 7U);
                        }
                        for (unsigned index = 0; index != width; ++index)
                            memory[8 + index] = static_cast<std::uint8_t>(old >> (index * 8));
                        for (unsigned index = 0; index != memory.size(); ++index)
                            static_cast<transmute::GuestMemory&>(oracle).write8(guard_begin + index, memory[index]);
                        auto state = oracle.state();
                        state.pc = image.entry() + 12;
                        state.nzcv = 0xb0000000U;
                        state.fpcr = 0x03c00000U;
                        state.fpsr = 0xa800009fU;
                        static_cast<void>(oracle.run());
                        auto expected = oracle.state();
                        expected.pc = state.pc;
                        const auto host = 0xffbfU | (restore.saved & 0x40U);
                        _mm_setcsr(host);
                        const auto wrote = kernel.execute(state, memory.data() + 8);
                        expect(integer_simd_same_state(state, expected) && _mm_getcsr() == host &&
                                   wrote == (op != NativeLseOperation::compare_swap || cas_match),
                               "native LSE changed width/aliases/ordering/result or conditional write accounting");
                        for (unsigned index = 0; index != memory.size(); ++index)
                            expect(memory[index] == static_cast<transmute::GuestMemory&>(oracle).read8(guard_begin + index),
                                   "native LSE changed adjacent memory or atomic replacement bits");
                        if (op == NativeLseOperation::compare_swap) {
                            successful_cas += wrote;
                            failed_cas += !wrote;
                        }
                        ++cases;
                    }
                }
            }
        }
    }
    expect(cases == 9600 && successful_cas == 480 && failed_cas == 480,
           "native LSE matrix missed a width, ordering, alias or CAS outcome");
#endif
}

void test_native_lse_contention() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    using transmute::jit_detail::NativeLseOperation;
    // Private host storage has a fixed lifetime for this hardware stress test.
    // Guest integration still requires the exclusive execution capability.
    for (const auto operation : {NativeLseOperation::add, NativeLseOperation::eor}) {
        NativeLseTestKernel kernel(native_lse_test_opcode(operation, 8, 3, 0, 1, 2));
        alignas(16) std::array<std::uint8_t, 16> storage{};
        constexpr unsigned workers = 4, iterations = 2000;
        std::array<std::uint64_t, workers> sums{};
        std::array<std::exception_ptr, workers> failures{};
        std::vector<std::thread> threads;
        std::atomic<unsigned> ready{};
        std::atomic<bool> start{};
        for (unsigned worker = 0; worker != workers; ++worker) {
            threads.emplace_back([&, worker] {
                try {
                    transmute::CpuState state{};
                    state.x[0] = 1;
                    ready.fetch_add(1, std::memory_order_release);
                    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                    for (unsigned i = 0; i != iterations; ++i) {
                        expect(kernel.execute(state, storage.data()), "native locked RMW lost its write flag");
                        sums[worker] += state.x[1];
                    }
                } catch (...) { failures[worker] = std::current_exception(); }
            });
        }
        while (ready.load(std::memory_order_acquire) != workers) std::this_thread::yield();
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();
        for (const auto failure : failures) if (failure) std::rethrow_exception(failure);
        std::uint64_t final = 0, sum = 0;
        for (unsigned i = 0; i != 8; ++i) final |= static_cast<std::uint64_t>(storage[i]) << (i * 8);
        for (const auto value : sums) sum += value;
        constexpr std::uint64_t total = workers * iterations;
        const auto expected_final = operation == NativeLseOperation::add ? total : 0;
        const auto expected_sum = operation == NativeLseOperation::add ? total * (total - 1) / 2 : total / 2;
        expect(final == expected_final && sum == expected_sum,
               "native locked LSE add/CAS-loop XOR lost an update or returned a torn old value");
    }
#endif
}
