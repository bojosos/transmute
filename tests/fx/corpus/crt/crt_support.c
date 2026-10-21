// fx corpus ARM64 CRT: the static parts of msvcrt.lib/vcruntime.lib that are
// not exported from vcruntime140.dll or ucrtbase.dll.
//   - .CRT$XI*/.CRT$XC* initializer section markers
//   - _fltused, security cookie
//   - thread-safe local statics (_Init_thread_*)
//   - TLS directory (_tls_used), dynamic thread_local init and destructors
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "crt_internal.h"

int _fltused = 0x9875;

// ---- initializer sections -------------------------------------------------
#pragma section(".CRT$XIA", long, read)
#pragma section(".CRT$XIZ", long, read)
#pragma section(".CRT$XCA", long, read)
#pragma section(".CRT$XCZ", long, read)
#pragma section(".CRT$XDA", long, read)
#pragma section(".CRT$XDZ", long, read)
#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLC", long, read)
#pragma section(".CRT$XLD", long, read)
#pragma section(".CRT$XLZ", long, read)
#pragma section(".rdata$T", long, read)

__declspec(allocate(".CRT$XIA")) _PIFV __xi_a[] = {0};
__declspec(allocate(".CRT$XIZ")) _PIFV __xi_z[] = {0};
__declspec(allocate(".CRT$XCA")) _PVFV __xc_a[] = {0};
__declspec(allocate(".CRT$XCZ")) _PVFV __xc_z[] = {0};
// Dynamic initializers of thread_local variables.
__declspec(allocate(".CRT$XDA")) _PVFV __xd_a = 0;
__declspec(allocate(".CRT$XDZ")) _PVFV __xd_z = 0;

// ---- per-module stdio options ------------------------------------------------
// MSVC's startup (__scrt_initialize_printf_standard_rounding_mode) selects
// IEEE-correct rounding for printf of floating-point values; without it the
// UCRT uses its legacy round-half-away-from-zero behavior. The options word
// lives in an inline function's static, i.e. once per module.
void __fx_initialize_stdio_options(void) {
    *__local_stdio_printf_options() |= _CRT_INTERNAL_PRINTF_STANDARD_ROUNDING;
}

// ---- security cookie (corpus builds use /GS-, but keep /GS objects linkable)
uintptr_t __security_cookie = 0x00002B992DDFA232ull;
uintptr_t __security_cookie_complement = ~0x00002B992DDFA232ull;

void __cdecl __security_init_cookie(void) {
    if (__security_cookie != 0x00002B992DDFA232ull) return;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uintptr_t cookie = (uintptr_t)counter.QuadPart;
    cookie ^= (uintptr_t)GetCurrentThreadId() << 32;
    cookie ^= (uintptr_t)GetCurrentProcessId();
    cookie ^= (uintptr_t)&cookie;
    cookie &= 0x0000FFFFFFFFFFFFull;
    if (cookie == 0x00002B992DDFA232ull || cookie == 0) cookie = 0x00002B992DDFA233ull;
    __security_cookie = cookie;
    __security_cookie_complement = ~cookie;
}

void __cdecl __security_check_cookie(uintptr_t cookie) {
    if (cookie != __security_cookie) __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

// ---- thread-safe local statics ---------------------------------------------
// Same protocol as vcruntime's thread_safe_statics.cpp: a guard is 0
// (uninitialized), -1 (being initialized) or the global epoch at completion.
#define FX_EPOCH_START INT_MIN
int _Init_global_epoch = FX_EPOCH_START;
__declspec(thread) int _Init_thread_epoch = FX_EPOCH_START;
static SRWLOCK fx_init_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE fx_init_cv = CONDITION_VARIABLE_INIT;

void __cdecl _Init_thread_header(int* once) {
    AcquireSRWLockExclusive(&fx_init_lock);
    if (*once == 0) {
        *once = -1;
    } else {
        while (*once == -1) {
            SleepConditionVariableSRW(&fx_init_cv, &fx_init_lock, INFINITE, 0);
            if (*once == 0) {
                *once = -1;
                ReleaseSRWLockExclusive(&fx_init_lock);
                return;
            }
        }
        _Init_thread_epoch = _Init_global_epoch;
    }
    ReleaseSRWLockExclusive(&fx_init_lock);
}

void __cdecl _Init_thread_abort(int* once) {
    AcquireSRWLockExclusive(&fx_init_lock);
    *once = 0;
    ReleaseSRWLockExclusive(&fx_init_lock);
    WakeAllConditionVariable(&fx_init_cv);
}

void __cdecl _Init_thread_footer(int* once) {
    AcquireSRWLockExclusive(&fx_init_lock);
    ++_Init_global_epoch;
    *once = _Init_global_epoch;
    _Init_thread_epoch = _Init_global_epoch;
    ReleaseSRWLockExclusive(&fx_init_lock);
    WakeAllConditionVariable(&fx_init_cv);
}

// ---- TLS directory -----------------------------------------------------------
ULONG _tls_index = 0;

#pragma data_seg(".tls")
char _tls_start = 0;
#pragma data_seg(".tls$ZZZ")
char _tls_end = 0;
#pragma data_seg()

__declspec(allocate(".CRT$XLA")) PIMAGE_TLS_CALLBACK __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) PIMAGE_TLS_CALLBACK __xl_z = 0;

const __declspec(allocate(".rdata$T")) IMAGE_TLS_DIRECTORY64 _tls_used = {
    (ULONGLONG)&_tls_start, // StartAddressOfRawData
    (ULONGLONG)&_tls_end,   // EndAddressOfRawData
    (ULONGLONG)&_tls_index, // AddressOfIndex
    (ULONGLONG)(&__xl_a + 1), // AddressOfCallBacks
    0,                      // SizeOfZeroFill
    {0}                     // Characteristics
};

// ---- dynamic thread_local initialization (.CRT$XD*, __tls_guard) -----------
__declspec(thread) char __tls_guard = 0;

void NTAPI __dyn_tls_init(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    if (reason != DLL_THREAD_ATTACH || __tls_guard) return;
    __tls_guard = 1;
    for (_PVFV* it = &__xd_a + 1; it != &__xd_z; ++it) {
        if (*it) (*it)();
    }
}

void __cdecl __dyn_tls_on_demand_init(void) {
    __dyn_tls_init(0, DLL_THREAD_ATTACH, 0);
}

void __fx_dyn_tls_init_current_thread(void) {
    __dyn_tls_init(0, DLL_THREAD_ATTACH, 0);
}

// ---- thread_local destructors (__tlregdtor) --------------------------------
#define FX_TLS_DTOR_SLOTS 32
typedef struct FxTlsDtorNode {
    struct FxTlsDtorNode* next;
    int count;
    _PVFV funcs[FX_TLS_DTOR_SLOTS];
} FxTlsDtorNode;

static __declspec(thread) FxTlsDtorNode fx_tls_dtor_head;
static __declspec(thread) FxTlsDtorNode* fx_tls_dtor_list;

int __cdecl __tlregdtor(_PVFV func) {
    if (!fx_tls_dtor_list) {
        fx_tls_dtor_head.next = 0;
        fx_tls_dtor_head.count = 0;
        fx_tls_dtor_list = &fx_tls_dtor_head;
    }
    if (fx_tls_dtor_list->count == FX_TLS_DTOR_SLOTS) {
        FxTlsDtorNode* node = (FxTlsDtorNode*)HeapAlloc(GetProcessHeap(), 0, sizeof(FxTlsDtorNode));
        if (!node) return -1;
        node->next = fx_tls_dtor_list;
        node->count = 0;
        fx_tls_dtor_list = node;
    }
    fx_tls_dtor_list->funcs[fx_tls_dtor_list->count++] = func;
    return 0;
}

void NTAPI __dyn_tls_dtor(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    if (reason != DLL_THREAD_DETACH && reason != DLL_PROCESS_DETACH) return;
    FxTlsDtorNode* node = fx_tls_dtor_list;
    while (node) {
        for (int i = node->count - 1; i >= 0; --i) {
            if (node->funcs[i]) node->funcs[i]();
        }
        FxTlsDtorNode* next = node->next;
        if (node != &fx_tls_dtor_head) HeapFree(GetProcessHeap(), 0, node);
        node = next;
    }
    fx_tls_dtor_list = 0;
}

__declspec(allocate(".CRT$XLC")) PIMAGE_TLS_CALLBACK __xl_c = __dyn_tls_init;
__declspec(allocate(".CRT$XLD")) PIMAGE_TLS_CALLBACK __xl_d = __dyn_tls_dtor;
