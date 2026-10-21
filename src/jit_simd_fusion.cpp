#include "jit_simd_fusion.hpp"

#include "jit_simd.hpp"
#include "transmute/a64.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#include <cpuid.h>
#endif

namespace transmute::jit_detail {
namespace {

struct Pair {
    unsigned destination, source, second;
    std::uint8_t opcode;
    bool invert_second;
};

std::optional<Pair> decode(std::uint32_t first, std::uint32_t second) noexcept {
    constexpr std::uint32_t registers = 0x001f03ffU;
    if (!accepts_integer_simd(first) || !accepts_integer_simd(second) ||
        (first & 0x40000000U) == 0 ||
        (first & ~registers) != (second & ~registers)) return std::nullopt;
    const auto rd = first & 31U;
    const auto rn = (first >> 5) & 31U;
    const auto rm = (first >> 16) & 31U;
    if (rd == 31 || rn == 31 || rm == 31 || (second & 31U) != rd + 1 ||
        ((second >> 5) & 31U) != rn + 1 ||
        ((second >> 16) & 31U) != rm + 1 ||
        rd == rn + 1 || rd == rm + 1) return std::nullopt;
    std::uint8_t opcode = 0;
    bool invert_second = false;
    switch (first & 0xbfe0fc00U) {
    case 0x0e201c00U: opcode = 0xdb; break; // VPAND
    case 0x0e601c00U: opcode = 0xdf; invert_second = true; break; // VPANDN
    case 0x0ea01c00U: opcode = 0xeb; break; // VPOR
    case 0x2e201c00U: opcode = 0xef; break; // VPXOR
    default:
        switch (first & 0xbf20fc00U) {
        case 0x0e208400U: {
            constexpr std::array<std::uint8_t, 4> add{0xfc, 0xfd, 0xfe, 0xd4};
            opcode = add[(first >> 22) & 3U];
            break;
        }
        case 0x2e208400U: {
            constexpr std::array<std::uint8_t, 4> subtract{0xf8, 0xf9, 0xfa, 0xfb};
            opcode = subtract[(first >> 22) & 3U];
            break;
        }
        default: return std::nullopt;
        }
    }
    return Pair{rd, rn, rm, opcode, invert_second};
}

bool detect_avx2() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int features[4]{};
    __cpuid(features, 0);
    if (features[0] < 7) return false;
    __cpuidex(features, 1, 0);
    constexpr int required = (1 << 27) | (1 << 28);
    if ((features[2] & required) != required || (_xgetbv(0) & 6U) != 6U)
        return false;
    __cpuidex(features, 7, 0);
    return (features[1] & (1 << 5)) != 0;
#elif (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7 ||
        !__get_cpuid(1, &eax, &ebx, &ecx, &edx) ||
        (ecx & ((1U << 27) | (1U << 28))) != ((1U << 27) | (1U << 28)))
        return false;
    unsigned low = 0, high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    if ((low & 6U) != 6U) return false;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    return (ebx & (1U << 5)) != 0;
#else
    return false;
#endif
}

} // namespace

bool native_avx2_available() noexcept {
    static const bool available = detect_avx2();
    return available;
}

bool accepts_fused_integer_simd(std::uint32_t first, std::uint32_t second,
                                bool avx2_available) noexcept {
    return avx2_available && decode(first, second).has_value();
}

bool accepts_fused_integer_simd(std::uint32_t first, std::uint32_t second) noexcept {
    return accepts_fused_integer_simd(first, second, native_avx2_available());
}

std::vector<std::uint8_t> emit_fused_integer_simd(
    std::uint32_t first, std::uint32_t second) {
    const auto pair = decode(first, second);
    if (!pair || !native_avx2_available())
        throw std::invalid_argument("unsupported native AVX2 SIMD pair");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(48);
    const auto word = [&bytes](std::uint32_t value) {
        for (unsigned index = 0; index != 4; ++index)
            bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
    };
    const auto transfer = [&](unsigned ymm, unsigned guest, bool store) {
        bytes.insert(bytes.end(), {0xc4, 0xe1, 0x7e,
            static_cast<std::uint8_t>(store ? 0x7f : 0x6f),
            static_cast<std::uint8_t>(0x81U | (ymm << 3))});
        word(static_cast<std::uint32_t>(offsetof(CpuState, v) + guest * 16));
    };
    transfer(1, pair->source, false);
    transfer(2, pair->second, false);
    bytes.insert(bytes.end(), {0xc4, 0xe1,
        static_cast<std::uint8_t>(pair->invert_second ? 0x6d : 0x75), pair->opcode,
        static_cast<std::uint8_t>(pair->invert_second ? 0xc1 : 0xc2)});
    transfer(0, pair->destination, true);
    bytes.insert(bytes.end(), {0xc5, 0xf8, 0x77}); // vzeroupper
    return bytes;
}

} // namespace transmute::jit_detail
