#pragma once

// The few kernel32 functions the win32 backend calls from inline code,
// declared exactly as the Windows SDK declares them (synchapi.h,
// profileapi.h, sysinfoapi.h, processthreadsapi.h), so fx headers can inline
// them without including windows.h. A translation unit may include both:
// identical redeclarations are allowed, and fx_host_tests compiles one that
// does. Only src/fx/host/win32/ includes this file.

struct _RTL_SRWLOCK;              // SRWLOCK: one pointer, zero = unlocked
struct _RTL_CONDITION_VARIABLE;   // CONDITION_VARIABLE: one pointer, zero = initialized
union _LARGE_INTEGER;             // LARGE_INTEGER: 8 bytes

extern "C" {
__declspec(dllimport) void __stdcall AcquireSRWLockShared(_RTL_SRWLOCK* lock);
__declspec(dllimport) void __stdcall AcquireSRWLockExclusive(_RTL_SRWLOCK* lock);
__declspec(dllimport) void __stdcall ReleaseSRWLockShared(_RTL_SRWLOCK* lock);
__declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(_RTL_SRWLOCK* lock);
__declspec(dllimport) unsigned char __stdcall TryAcquireSRWLockShared(_RTL_SRWLOCK* lock);
__declspec(dllimport) unsigned char __stdcall TryAcquireSRWLockExclusive(_RTL_SRWLOCK* lock);
__declspec(dllimport) int __stdcall SleepConditionVariableSRW(_RTL_CONDITION_VARIABLE* condition, _RTL_SRWLOCK* lock,
                                                              unsigned long milliseconds, unsigned long flags);
__declspec(dllimport) void __stdcall WakeConditionVariable(_RTL_CONDITION_VARIABLE* condition);
__declspec(dllimport) void __stdcall WakeAllConditionVariable(_RTL_CONDITION_VARIABLE* condition);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(_LARGE_INTEGER* count);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(_LARGE_INTEGER* frequency);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
}
