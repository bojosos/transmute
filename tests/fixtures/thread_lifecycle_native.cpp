#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// The same loader events are checked against Windows and the guest bridge.
// No CRT entry point is involved, so only PE TLS and DllMain contribute events.
extern "C" {
__declspec(dllexport) DWORD thread_lifecycle_events[32]{};
__declspec(dllexport) DWORD thread_lifecycle_count{};
__declspec(dllexport) DWORD thread_lifecycle_thread{};
DWORD _tls_index{};
#pragma section(".tls", read, write)
__declspec(allocate(".tls")) DWORD thread_lifecycle_tls_template{0x12345678};

static void record(DWORD reason, DWORD callback) {
    thread_lifecycle_thread = GetCurrentThreadId();
    thread_lifecycle_events[thread_lifecycle_count++] = reason * 10 + callback;
}

void NTAPI first_tls_callback(void*, DWORD reason, void*) { record(reason, 1); }
void NTAPI second_tls_callback(void*, DWORD reason, void*) { record(reason, 2); }

const PIMAGE_TLS_CALLBACK thread_lifecycle_callbacks[]{
    first_tls_callback, second_tls_callback, nullptr};

extern const IMAGE_TLS_DIRECTORY64 _tls_used{
    (ULONGLONG)&thread_lifecycle_tls_template,
    (ULONGLONG)(&thread_lifecycle_tls_template + 1),
    (ULONGLONG)&_tls_index,
    (ULONGLONG)thread_lifecycle_callbacks, 0, 0};

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*) {
    record(reason, 3);
    return TRUE;
}
}
