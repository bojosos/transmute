// linux backend of fx/host/guard.hpp: the guarded memory helpers.
#include "fx/host/guard.hpp"

#include <emmintrin.h>

#include <bit>
#include <cstring>

namespace transmute::fx::host {

namespace {

using linux_guard::Filter;

// Bytes before the first zero, up to `maximum`: SSE2 over aligned 16-byte
// blocks. A block never crosses a page, so every block read holds a byte the
// byte-wise scan would read too (the first one starts at `p`'s block).
std::size_t length8(const std::uint8_t* p, std::size_t maximum) noexcept {
    const auto start = reinterpret_cast<std::uintptr_t>(p);
    const auto* block = reinterpret_cast<const __m128i*>(start & ~std::uintptr_t{15});
    const __m128i zero = _mm_setzero_si128();
    std::size_t count = 0;   // bytes of p before this block's first byte of p
    unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128(block), zero))) >> (start & 15);
    std::size_t in_block = 16 - (start & 15);
    for (;;) {
        if (mask != 0) {
            const std::size_t n = count + static_cast<std::size_t>(std::countr_zero(mask));
            return n < maximum ? n : maximum;
        }
        count += in_block;
        if (count >= maximum) return maximum;
        ++block;
        in_block = 16;
        mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128(block), zero)));
    }
}

long long compare_bytes(const std::uint8_t* x, const std::uint8_t* y, std::size_t size) noexcept {
    std::size_t i = 0;
    for (; i + 16 <= size; i += 16) {   // SSE2: the first unequal byte of a 16-byte chunk
        const __m128i u = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
        const auto equal = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(u, v)));
        if (equal != 0xffffu) {
            const auto k = static_cast<std::size_t>(std::countr_zero(~equal));
            return static_cast<long long>(x[i + k]) - y[i + k];
        }
    }
    for (; i + 8 <= size; i += 8) {
        std::uint64_t u, v;
        std::memcpy(&u, x + i, 8);
        std::memcpy(&v, y + i, 8);
        if (u != v) {
            const auto k = static_cast<std::size_t>(std::countr_zero(u ^ v)) / 8;   // little-endian: the first byte
            return static_cast<long long>(x[i + k]) - y[i + k];
        }
    }
    for (; i < size; ++i)
        if (x[i] != y[i]) return static_cast<long long>(x[i]) - y[i];
    return 0;
}

} // namespace

bool try_read32(std::uint64_t address, std::uint32_t& value) noexcept {
    std::uint32_t read = 0;
    auto f = [&] { std::memcpy(&read, reinterpret_cast<const void*>(address), 4); };
    if (!linux_guard::run(f, Filter::any, nullptr)) return false;
    value = read;
    return true;
}

bool try_read64(std::uint64_t address, std::uint64_t& value) noexcept {
    std::uint64_t read = 0;
    auto f = [&] { std::memcpy(&read, reinterpret_cast<const void*>(address), 8); };
    if (!linux_guard::run(f, Filter::any, nullptr)) return false;
    value = read;
    return true;
}

bool try_copy(void* destination, const void* source, std::size_t size) noexcept {
    auto f = [&] { std::memcpy(destination, source, size); };
    return linux_guard::run(f, Filter::any, nullptr);
}

bool guarded_move(void* destination, const void* source, std::size_t size) noexcept {
    auto f = [&] { std::memmove(destination, source, size); };
    return linux_guard::run(f, Filter::memory, nullptr);
}

bool guarded_fill(void* destination, int value, std::size_t size) noexcept {
    auto f = [&] { std::memset(destination, value, size); };
    return linux_guard::run(f, Filter::memory, nullptr);
}

bool guarded_compare(const void* a, const void* b, std::size_t size, long long& result) noexcept {
    long long difference = 0;
    auto f = [&] {
        difference = compare_bytes(static_cast<const std::uint8_t*>(a), static_cast<const std::uint8_t*>(b), size);
    };
    if (!linux_guard::run(f, Filter::memory, nullptr)) return false;
    result = difference;
    return true;
}

long long guarded_length(const std::uint8_t* address, unsigned width, std::size_t maximum) noexcept {
    std::size_t count = 0;
    auto f = [&] {
        if (width == 1) {
            count = length8(address, maximum);
        } else {
            const auto* wide = reinterpret_cast<const volatile std::uint16_t*>(address);
            while (count < maximum && wide[count] != 0) ++count;
        }
    };
    if (!linux_guard::run(f, Filter::memory, nullptr)) return -1;
    return static_cast<long long>(count);
}

} // namespace transmute::fx::host
