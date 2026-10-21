#pragma once

// win32 backend of fx/host/guard.hpp: the guarded-call templates. MSVC keeps
// a function with __try out of line, so call_guarded<F> is one frame with
// f() inlined into it, as a hand-written __try function would be.

#include <excpt.h>

namespace transmute::fx::host {

namespace win32_guard {
// Copies the host EXCEPTION_RECORD; EXCEPTION_EXECUTE_HANDLER.
int capture(void* exception_pointers, FaultRecord* out) noexcept;
// EXCEPTION_EXECUTE_HANDLER for memory faults, else EXCEPTION_CONTINUE_SEARCH.
int memory_filter(unsigned long code) noexcept;
// The same for access violations and in-page errors only.
int access_filter(unsigned long code) noexcept;
} // namespace win32_guard

template <typename F>
bool call_guarded(F&& f, FaultRecord& fault) noexcept {
    __try {
        f();
        return true;
    } __except (win32_guard::capture(_exception_info(), &fault)) {
        return false;
    }
}

template <typename F>
bool call_guarded_memory(F&& f) noexcept {
    __try {
        f();
        return true;
    } __except (win32_guard::memory_filter(_exception_code())) {
        return false;
    }
}

template <typename F>
bool call_guarded_access(F&& f) noexcept {
    __try {
        f();
        return true;
    } __except (win32_guard::access_filter(_exception_code())) {
        return false;
    }
}

} // namespace transmute::fx::host
