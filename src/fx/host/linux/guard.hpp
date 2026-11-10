#pragma once

// linux backend of fx/host/guard.hpp: a per-thread stack of recovery points.
// A guarded call pushes a Frame and saves its registers with sigsetjmp (no
// signal mask: the fault handler runs with SA_NODEFER, so nothing stays
// blocked). When a fault reaches the handler and fx's own handler passes it
// on, the handler pops the innermost frame whose filter accepts the fault
// and siglongjmps there (fault.cpp, deliver_to_guard).

#include <setjmp.h>

#include <atomic>

namespace transmute::fx::host {

namespace linux_guard {

enum class Filter : unsigned char {
    any,      // every fault
    memory,   // access violations, in-page errors, guard pages
    access,   // access violations and in-page errors
};

struct Frame {
    sigjmp_buf env;
    Filter filter;
    FaultRecord* record;   // filled before the jump (may be null)
    Frame* previous;
};

// The calling thread's innermost frame.
inline thread_local Frame* t_top = nullptr;

// Installs the fault signal handler (fault.cpp) the first time a guarded call
// runs before the runtime's install_fault_handler: __try needs no setup.
void install_signal_handler() noexcept;
inline std::atomic<bool> g_handler_installed{false};

template <typename F>
inline bool run(F& f, Filter filter, FaultRecord* record) noexcept {
    if (!g_handler_installed.load(std::memory_order_relaxed)) install_signal_handler();
    Frame frame;
    frame.filter = filter;
    frame.record = record;
    frame.previous = t_top;
    if (sigsetjmp(frame.env, 0) != 0) return false;   // the handler restored t_top
    t_top = &frame;
    // The fault handler reads t_top: keep the stores on either side of f()
    // (without the fences the compiler may drop the first as dead, since the
    // second overwrites it and f's loads cannot alias it).
    std::atomic_signal_fence(std::memory_order_seq_cst);
    f();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    t_top = frame.previous;
    return true;
}

} // namespace linux_guard

template <typename F>
bool call_guarded(F&& f, FaultRecord& fault) noexcept {
    return linux_guard::run(f, linux_guard::Filter::any, &fault);
}

template <typename F>
bool call_guarded_memory(F&& f) noexcept {
    return linux_guard::run(f, linux_guard::Filter::memory, nullptr);
}

template <typename F>
bool call_guarded_access(F&& f) noexcept {
    return linux_guard::run(f, linux_guard::Filter::access, nullptr);
}

} // namespace transmute::fx::host
