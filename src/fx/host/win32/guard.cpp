// win32 backend of fx/host/guard.hpp: structured exception handling.

#include "fx/host/guard.hpp"

#include <windows.h>

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

long long guarded_length(const std::uint8_t* address, unsigned width, std::size_t maximum) noexcept {
    __try {
        std::size_t count = 0;
        if (width == 1) {
            while (count < maximum && address[count] != 0) ++count;
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
