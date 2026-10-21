// win32 backend of fx/host/thread.hpp.

#include "fx/host/thread.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

namespace transmute::fx::host {

static_assert(kThreadSuspended == CREATE_SUSPENDED && kThreadStackReservation == STACK_SIZE_PARAM_IS_A_RESERVATION);
static_assert(sizeof(DWORD) == sizeof(std::uint32_t));

ThreadHandle create_thread(ThreadEntry entry, void* argument, std::size_t stack_size, std::uint32_t flags,
                           std::uint32_t* thread_id) noexcept {
    DWORD id = 0;
    // ThreadEntry and LPTHREAD_START_ROUTINE differ only in the spelling of
    // the 32-bit result type: one x64 calling convention.
    HANDLE handle = CreateThread(nullptr, stack_size, reinterpret_cast<LPTHREAD_START_ROUTINE>(entry), argument, flags,
                                 thread_id ? &id : nullptr);
    if (handle && thread_id) *thread_id = id;
    return handle;
}

void close_thread(ThreadHandle thread) noexcept { CloseHandle(thread); }

ThreadHandle current_thread() noexcept { return GetCurrentThread(); }

void set_thread_priority(ThreadHandle thread, ThreadPriority priority) noexcept {
    int value = THREAD_PRIORITY_NORMAL;
    switch (priority) {
    case ThreadPriority::normal: value = THREAD_PRIORITY_NORMAL; break;
    case ThreadPriority::below_normal: value = THREAD_PRIORITY_BELOW_NORMAL; break;
    case ThreadPriority::idle: value = THREAD_PRIORITY_IDLE; break;
    case ThreadPriority::above_normal: value = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case ThreadPriority::highest: value = THREAD_PRIORITY_HIGHEST; break;
    }
    SetThreadPriority(thread, value);
}

void set_thread_affinity(ThreadHandle thread, std::uint64_t mask) noexcept {
    SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(mask));
}

void set_thread_eco_qos(ThreadHandle thread) noexcept {
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    SetThreadInformation(thread, ThreadPowerThrottling, &state, sizeof(state));
}

void exit_thread(std::uint32_t exit_code) noexcept { ExitThread(exit_code); }

void yield_thread() noexcept { SwitchToThread(); }

namespace {
struct PoolWork {
    void (*work)(void*);
    void* context;
};
void CALLBACK pool_callback(PTP_CALLBACK_INSTANCE, void* parameter) {
    const PoolWork item = *static_cast<PoolWork*>(parameter);
    delete static_cast<PoolWork*>(parameter);
    item.work(item.context);
}
} // namespace

bool submit_to_thread_pool(void (*work)(void* context), void* context) noexcept {
    auto* item = new (std::nothrow) PoolWork{work, context};
    if (!item) return false;
    if (TrySubmitThreadpoolCallback(&pool_callback, item, nullptr)) return true;
    delete item;
    return false;
}

std::uint32_t last_error() noexcept { return GetLastError(); }

void set_last_error(std::uint32_t error) noexcept { SetLastError(error); }

std::size_t copy_thread_block(void* out, std::size_t size) noexcept {
    const auto* teb = reinterpret_cast<const std::uint8_t*>(NtCurrentTeb());
    // The TEB of a Windows 10/11 x64 thread is 0x1838 bytes; copy no further
    // than its committed region.
    std::size_t bytes = std::min<std::size_t>(0x1838, size);
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(teb, &info, sizeof(info)) == sizeof(info)) {
        const auto available = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize - teb;
        bytes = std::min<std::size_t>(size, static_cast<std::size_t>(available));
    }
    std::memcpy(out, teb, bytes);
    return bytes;
}

std::uintptr_t activate_context(void* context) noexcept {
    ULONG_PTR cookie = 0;
    return ActivateActCtx(context, &cookie) ? cookie : 0;
}

void deactivate_context(std::uintptr_t cookie) noexcept { DeactivateActCtx(0, cookie); }

namespace {

std::atomic<void (*)()> g_thread_exit_hook{nullptr};

void NTAPI thread_detach_callback(PVOID, DWORD reason, PVOID) {
    if (reason != DLL_THREAD_DETACH) return;
    if (const auto hook = g_thread_exit_hook.load(std::memory_order_acquire)) hook();
}

} // namespace

void set_thread_exit_hook(void (*hook)()) noexcept { g_thread_exit_hook.store(hook, std::memory_order_release); }

} // namespace transmute::fx::host

// ---- thread teardown hook ------------------------------------------------------------
// ntdll calls TLS callbacks on DLL_THREAD_DETACH after the thread's FLS
// callbacks. .CRT$XLB sorts after the TLS directory start (.CRT$XLA) and
// before the CRT's own callbacks (.CRT$XLC/XLD, which run thread_local
// destructors), so every thread_local is still alive while the hook runs.
// set_thread_exit_hook (in this object) pulls it into every image that uses
// the hook.
extern "C" void NTAPI fx_runtime_thread_detach(PVOID module, DWORD reason, PVOID reserved) {
    transmute::fx::host::thread_detach_callback(module, reason, reserved);
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:fx_runtime_tls_callback")
#pragma const_seg(push)
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK fx_runtime_tls_callback = &fx_runtime_thread_detach;
#pragma const_seg(pop)
