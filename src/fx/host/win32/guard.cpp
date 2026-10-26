// win32 backend of fx/host/guard.hpp: structured exception handling.

#include "fx/host/guard.hpp"

#include <windows.h>

#include <emmintrin.h>

#include <bit>
#include <cstring>

namespace transmute::fx::host {

namespace win32_guard {

int capture(void* exception_pointers, FaultRecord* out) noexcept {
    std::memcpy(out, static_cast<EXCEPTION_POINTERS*>(exception_pointers)->ExceptionRecord, sizeof(FaultRecord));
    return EXCEPTION_EXECUTE_HANDLER;
}

int memory_filter(unsigned long code) noexcept {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_GUARD_PAGE
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

int access_filter(unsigned long code) noexcept {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                                                  : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace win32_guard

bool try_read32(std::uint64_t address, std::uint32_t& value) noexcept {
    __try {
        value = *reinterpret_cast<const volatile std::uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool try_read64(std::uint64_t address, std::uint64_t& value) noexcept {
    __try {
        value = *reinterpret_cast<const volatile std::uint64_t __unaligned*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool try_copy(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_move(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memmove(destination, source, size);
        return true;
    } __except (win32_guard::memory_filter(GetExceptionCode())) {
        return false;
    }
}

bool guarded_fill(void* destination, int value, std::size_t size) noexcept {
    __try {
        std::memset(destination, value, size);
        return true;
    } __except (win32_guard::memory_filter(GetExceptionCode())) {
        return false;
    }
}

namespace {

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

bool guarded_compare(const void* a, const void* b, std::size_t size, long long& result) noexcept {
    __try {
        result = compare_bytes(static_cast<const std::uint8_t*>(a), static_cast<const std::uint8_t*>(b), size);
        return true;
    } __except (win32_guard::memory_filter(GetExceptionCode())) {
        return false;
    }
}

long long guarded_length(const std::uint8_t* address, unsigned width, std::size_t maximum) noexcept {
    __try {
        std::size_t count = 0;
        if (width == 1) {
            count = length8(address, maximum);
        } else {
            const auto* wide = reinterpret_cast<const std::uint16_t*>(address);
            while (count < maximum && wide[count] != 0) ++count;
        }
        return static_cast<long long>(count);
    } __except (win32_guard::memory_filter(GetExceptionCode())) {
        return -1;
    }
}

} // namespace transmute::fx::host
