// Included after the SIMD/region helpers inside test_main.cpp's namespace.

void test_jit_simd_fusion_predicate() {
    constexpr std::uint32_t first = 0x4ea68480U; // add v0.4s,v4.4s,v6.4s
    constexpr std::uint32_t second = 0x4ea784a1U; // add v1.4s,v5.4s,v7.4s
    expect(transmute::jit_detail::accepts_fused_integer_simd(first, second, true) &&
               !transmute::jit_detail::accepts_fused_integer_simd(first, second, false),
           "SIMD fusion did not respect explicit host feature eligibility");
    for (const auto other : {second ^ 0x40000000U, second ^ 0x00400000U,
                             second ^ 1U, second ^ (1U << 5),
                             second ^ (1U << 16)}) {
        expect(!transmute::jit_detail::accepts_fused_integer_simd(first, other, true),
               "SIMD fusion accepted nonadjacent or different operations");
    }
    constexpr auto dependent_first = 0x4ea68422U; // add v2.4s,v1.4s,v6.4s
    constexpr auto dependent_second = 0x4ea78443U; // add v3.4s,v2.4s,v7.4s
    expect(!transmute::jit_detail::accepts_fused_integer_simd(
               dependent_first, dependent_second, true),
           "SIMD fusion ignored a second-instruction dependency on the first result");
}

void test_jit_simd_fusion_fragments() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (!transmute::jit_detail::native_avx2_available()) return;
    struct CodeOwner {
        void* address;
        ~CodeOwner() { if (address) VirtualFree(address, 0, MEM_RELEASE); }
    };
    struct Registers { unsigned destination, source, second; };
    constexpr std::array registers{
        Registers{0, 4, 6}, Registers{4, 4, 6},
        Registers{6, 4, 6}, Registers{29, 0, 29}};
    constexpr std::array<std::uint32_t, 6> forms{
        0x4e201c00U, 0x4e601c00U, 0x4ea01c00U, 0x6e201c00U,
        0x4e208400U, 0x6e208400U};
    GuardedFpHostState restore;
    for (unsigned family = 0; family != forms.size(); ++family) {
        for (unsigned size = 0; size != (family >= 4 ? 4U : 1U); ++size) {
            for (const auto reg : registers) {
                const auto base = forms[family] | (size << 22);
                const auto first = base | reg.destination |
                    (reg.source << 5) | (reg.second << 16);
                const auto second = base | (reg.destination + 1) |
                    ((reg.source + 1) << 5) | ((reg.second + 1) << 16);
                expect(transmute::jit_detail::accepts_fused_integer_simd(first, second),
                       "SIMD fusion rejected an independent in-place arrangement");
                auto code = transmute::jit_detail::emit_fused_integer_simd(first, second);
                code.push_back(0xc3);
                CodeOwner owner{VirtualAlloc(nullptr, code.size(),
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)};
                expect(owner.address != nullptr, "could not allocate SIMD fusion test code");
                std::copy(code.begin(), code.end(), static_cast<std::uint8_t*>(owner.address));
                DWORD prior = 0;
                expect(VirtualProtect(owner.address, code.size(), PAGE_EXECUTE_READ, &prior) &&
                           FlushInstructionCache(GetCurrentProcess(), owner.address, code.size()),
                       "could not publish SIMD fusion test code");
                auto image = transmute::PeImage::parse(executable({first, second, 0xd65f03c0U}));
                for (unsigned sample = 0; sample != 4; ++sample) {
                    transmute::ReferenceEngine oracle(image);
                    for (unsigned index = 0; index != 30; ++index)
                        oracle.set_initial_register(index, 0x1234567800000000ULL + index);
                    for (unsigned index = 0; index != 32; ++index) {
                        const auto low = sample == 0 ? UINT64_MAX :
                            sample == 1 ? 0ULL :
                            0x800000017fffffffULL ^ (index * 0x1111111111111111ULL);
                        oracle.set_initial_vector(index,
                            {low, ~low ^ (sample * 0x0102030405060708ULL)});
                    }
                    auto native = oracle.state();
                    const auto pc = native.pc;
                    const auto result = oracle.run();
                    expect(result.instructions == 3, "SIMD fusion oracle skipped an instruction");
                    auto expected = oracle.state();
                    expected.pc = pc;
                    const auto host = restore.saved | 0x3fU;
                    _mm_setcsr(host);
                    reinterpret_cast<void (*)(transmute::CpuState*)>(owner.address)(&native);
                    expect(integer_simd_same_state(native, expected) && _mm_getcsr() == host,
                           "AVX2 fusion changed sequential lanes, in-place aliases or host state");
                }
            }
        }
    }
#endif
}
