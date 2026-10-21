// fxcorpus_dll.dll: a guest DLL with code and data exports, an ordinal-only
// export, a C initializer, TLS and DllMain bookkeeping.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#define FXCORPUS_DLL_BUILD
#include "fxcorpus_dll.h"

int fxdll_counter = 1000;
long fxdll_process_attach;
long fxdll_thread_attach;
long fxdll_thread_detach;

static __declspec(thread) int tls_value = 17;
static int initializer_ran;

static void __cdecl dll_initializer(void) {
    initializer_ran = 0x5150;
}
#pragma section(".CRT$XCU", long, read)
__declspec(allocate(".CRT$XCU")) __attribute__((used)) static void(__cdecl* const dll_initializer_entry)(void) =
    dll_initializer;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH: InterlockedIncrement(&fxdll_process_attach); break;
    case DLL_THREAD_ATTACH: InterlockedIncrement(&fxdll_thread_attach); break;
    case DLL_THREAD_DETACH: InterlockedIncrement(&fxdll_thread_detach); break;
    default: break;
    }
    return TRUE;
}

int fxdll_add(int a, int b) {
    fxdll_counter++;
    return a + b;
}

int64_t fxdll_mul64(int64_t a, int64_t b) {
    return a * b;
}

double fxdll_mix(double d, float f, int i, long long l, float g) {
    return d * 2.0 + (double)f - (double)i + (double)l / 4.0 + (double)g * 0.5;
}

int fxdll_sum_many(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11) {
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10 + 11 * a11;
}

double fxdll_sum_fp_many(double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8,
                         double d9, double d10) {
    return d1 + d2 * 2 + d3 * 3 + d4 * 4 + d5 * 5 + d6 * 6 + d7 * 7 + d8 * 8 + d9 * 9 + d10 * 10;
}

int fxdll_apply(FxCallback callback, void* context, int count) {
    int total = 0;
    for (int i = 0; i < count; i++) total += callback(i, context);
    return total;
}

const char* fxdll_name(void) {
    return "fxcorpus_dll";
}

FxPoint fxdll_make_point(int x, int y) {
    FxPoint p = {x * 2, y * 3};
    return p;
}

FxBig fxdll_make_big(int seed) {
    FxBig b;
    for (int i = 0; i < 6; i++) b.values[i] = (long long)seed * (i + 1) * 1000003LL;
    b.weight = seed / 8.0;
    return b;
}

int fxdll_vsum(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++) total += va_arg(ap, int) * (i + 1);
    va_end(ap);
    return total;
}

int fxdll_tls_get(void) {
    return tls_value;
}

void fxdll_tls_set(int value) {
    tls_value = value;
}

int fxdll_initializer_ran(void) {
    return initializer_ran;
}

int fxdll_by_ordinal(int value) {
    return value * 100 + 7;
}
