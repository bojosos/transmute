#include "alchemy/host/sync.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <vector>

namespace alchemy::host::sync {

Result<Handle> create_event(bool manual_reset, bool initially_set) {
    const auto event = CreateEventW(nullptr, manual_reset, initially_set, nullptr);
    if (!event) return win32::last_error();
    return win32::wrap(event);
}

Status set_event(Handle event) {
    if (!SetEvent(win32::native(event))) return win32::last_error();
    return {};
}

Status reset_event(Handle event) {
    if (!ResetEvent(win32::native(event))) return win32::last_error();
    return {};
}

Result<std::size_t> wait_any(std::span<const Handle> handles, std::optional<std::uint64_t> timeout_ns) {
    if (handles.empty() || handles.size() > MAXIMUM_WAIT_OBJECTS) return Error::invalid_argument;
    std::vector<HANDLE> native;
    native.reserve(handles.size());
    for (const auto handle : handles) native.push_back(win32::native(handle));
    DWORD milliseconds = INFINITE;
    if (timeout_ns) {
        const auto rounded = (*timeout_ns + 999'999) / 1'000'000;
        milliseconds = rounded >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(rounded);
    }
    const auto result = WaitForMultipleObjects(static_cast<DWORD>(native.size()), native.data(), FALSE, milliseconds);
    if (result == WAIT_TIMEOUT) return Error::timed_out;
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + native.size()) return static_cast<std::size_t>(result - WAIT_OBJECT_0);
    // An abandoned mutex still hands ownership to the caller.
    if (result >= WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + native.size()) return static_cast<std::size_t>(result - WAIT_ABANDONED_0);
    return win32::last_error();
}

Result<Handle> create_mutex(std::string_view name) {
    const auto wide = win32::local_name(name);
    const auto mutex = CreateMutexW(nullptr, FALSE, wide.empty() ? nullptr : wide.c_str());
    if (!mutex) return win32::last_error();
    return win32::wrap(mutex);
}

Status unlock_mutex(Handle mutex) {
    if (!ReleaseMutex(win32::native(mutex))) return win32::last_error();
    return {};
}

Result<Handle> create_semaphore(std::string_view name, std::uint32_t initial, std::uint32_t maximum, bool exclusive) {
    const auto wide = win32::local_name(name);
    const auto clamp = [](std::uint32_t value) { return static_cast<LONG>(value > LONG_MAX ? LONG_MAX : value); };
    const auto semaphore = CreateSemaphoreW(nullptr, clamp(initial), clamp(maximum), wide.empty() ? nullptr : wide.c_str());
    if (!semaphore) return win32::last_error();
    if (exclusive && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(semaphore);
        return Error::exists;
    }
    return win32::wrap(semaphore);
}

namespace {
struct Callback { void (*function)(void*); void* context; };
// A watch: the wait handle and its callback record, both kept until unwatch.
struct Registration { HANDLE wait{}; Callback callback; };
void CALLBACK fired(void* parameter, BOOLEAN) {
    const auto* callback = static_cast<const Callback*>(parameter);
    callback->function(callback->context);
}
} // namespace

Result<Handle> watch_once(Handle object, void (*callback)(void* context), void* context) {
    auto* registration = new Registration{nullptr, Callback{callback, context}};
    if (!RegisterWaitForSingleObject(&registration->wait, win32::native(object), fired, &registration->callback, INFINITE, WT_EXECUTEONLYONCE)) {
        const auto error = win32::last_error();
        delete registration;
        return error;
    }
    return Handle{reinterpret_cast<std::uintptr_t>(registration)};
}

void unwatch(Handle watch, bool wait_for_callback) noexcept {
    auto* registration = reinterpret_cast<Registration*>(watch.value);
    if (!registration) return;
    UnregisterWaitEx(registration->wait, wait_for_callback ? INVALID_HANDLE_VALUE : nullptr);
    // Without waiting, a callback may still be running: the record stays (a
    // few bytes per watch cancelled from its own callback).
    if (wait_for_callback) delete registration;
}

Result<Handle> open_semaphore(std::string_view name) {
    const auto semaphore = OpenSemaphoreW(SEMAPHORE_ALL_ACCESS, FALSE, win32::local_name(name).c_str());
    if (!semaphore) return win32::last_error();
    return win32::wrap(semaphore);
}

Status release_semaphore(Handle semaphore, std::uint32_t count) {
    if (!ReleaseSemaphore(win32::native(semaphore), static_cast<LONG>(count > LONG_MAX ? LONG_MAX : count), nullptr))
        return win32::last_error();
    return {};
}

} // namespace alchemy::host::sync
