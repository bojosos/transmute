// Guest vectored exception/continue handlers and the unhandled-exception
// filter. These lists are process-wide, mirroring ntdll/kernel32 semantics
// for the guest.

#include "fx/seh/vectored.hpp"
#include "fx/host/sync.hpp"

#include <algorithm>
#include <vector>

namespace transmute::fx::seh {
namespace {

struct VectoredHandler {
    std::uint64_t handle;    // cookie returned to the guest
    std::uint64_t routine;   // guest PVECTORED_EXCEPTION_HANDLER
};

host::RwLock g_lock;
std::vector<VectoredHandler> g_exception_handlers;   // front = first called
std::vector<VectoredHandler> g_continue_handlers;
std::uint64_t g_next_handle = 0x5000;
std::uint64_t g_unhandled_filter = 0;
FaultHookEntry g_fault_hook;   // personality (set_guest_fault_hook)

} // namespace

std::uint64_t add_vectored_exception_handler(bool first, std::uint64_t handler) {
    if (!handler) return 0;
    g_lock.lock();
    const std::uint64_t cookie = g_next_handle++;
    const VectoredHandler entry{cookie, handler};
    if (first) g_exception_handlers.insert(g_exception_handlers.begin(), entry);
    else g_exception_handlers.push_back(entry);
    g_lock.unlock();
    return cookie;
}

std::uint64_t add_vectored_continue_handler(bool first, std::uint64_t handler) {
    if (!handler) return 0;
    g_lock.lock();
    const std::uint64_t cookie = g_next_handle++;
    const VectoredHandler entry{cookie, handler};
    if (first) g_continue_handlers.insert(g_continue_handlers.begin(), entry);
    else g_continue_handlers.push_back(entry);
    g_lock.unlock();
    return cookie;
}

bool remove_vectored_exception_handler(std::uint64_t handle) {
    bool removed = false;
    g_lock.lock();
    auto& list = g_exception_handlers;
    const auto it = std::find_if(list.begin(), list.end(),
                                 [&](const VectoredHandler& h) { return h.handle == handle; });
    if (it != list.end()) { list.erase(it); removed = true; }
    else {
        auto& cont = g_continue_handlers;
        const auto ct = std::find_if(cont.begin(), cont.end(),
                                     [&](const VectoredHandler& h) { return h.handle == handle; });
        if (ct != cont.end()) { cont.erase(ct); removed = true; }
    }
    g_lock.unlock();
    return removed;
}

std::vector<std::uint64_t> vectored_exception_routines() {
    std::vector<std::uint64_t> result;
    g_lock.lock_shared();
    for (const auto& h : g_exception_handlers) result.push_back(h.routine);
    g_lock.unlock_shared();
    return result;
}

std::vector<std::uint64_t> vectored_continue_routines() {
    std::vector<std::uint64_t> result;
    g_lock.lock_shared();
    for (const auto& h : g_continue_handlers) result.push_back(h.routine);
    g_lock.unlock_shared();
    return result;
}

std::uint64_t set_unhandled_exception_filter(std::uint64_t filter) {
    g_lock.lock();
    const std::uint64_t previous = g_unhandled_filter;
    g_unhandled_filter = filter;
    g_lock.unlock();
    return previous;
}

std::uint64_t unhandled_exception_filter_routine() {
    g_lock.lock_shared();
    const std::uint64_t filter = g_unhandled_filter;
    g_lock.unlock_shared();
    return filter;
}

FaultHookEntry personality_fault_hook() {
    g_lock.lock_shared();
    const FaultHookEntry entry = g_fault_hook;
    g_lock.unlock_shared();
    return entry;
}

void reset_process_state() {
    g_lock.lock();
    g_exception_handlers.clear();
    g_continue_handlers.clear();
    g_unhandled_filter = 0;
    g_fault_hook = {};
    g_lock.unlock();
}

} // namespace transmute::fx::seh

namespace transmute::fx {

void set_guest_fault_hook(GuestFaultHook hook, void* user) {
    seh::g_lock.lock();
    seh::g_fault_hook = {hook, hook ? user : nullptr};
    seh::g_lock.unlock();
}

} // namespace transmute::fx
