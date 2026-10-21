// Uses fxcorpus_dll.dll through its import library: code and data imports,
// many-argument and floating-point calls, struct returns, varargs, callbacks
// from the DLL into the EXE, TLS in the DLL and DllMain thread notifications.
#include <windows.h>
#include <stdio.h>

#include "fxcorpus_dll.h"

static int square_plus_context(int value, void* context) {
    return value * value + *(int*)context;
}

static DWORD WINAPI thread_body(void* arg) {
    (void)arg;
    int before = fxdll_tls_get();
    fxdll_tls_set(before + 1000);
    return (DWORD)fxdll_tls_get();
}

int main(void) {
    printf("name=%s initializer=%x process_attach=%ld\n", fxdll_name(), fxdll_initializer_ran(), fxdll_process_attach);
    int before = fxdll_counter;
    int sum = fxdll_add(40, 2);
    printf("add=%d counter %d -> %d\n", sum, before, fxdll_counter);
    fxdll_counter = 5;
    fxdll_add(1, 1);
    printf("counter after write=%d\n", fxdll_counter);
    printf("mul64=%lld\n", (long long)fxdll_mul64(123456789LL, -98765LL));
    printf("mix=%.6f\n", fxdll_mix(1.25, 0.5f, 3, 10LL, -2.0f));
    printf("sum_many=%d\n", fxdll_sum_many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11));
    printf("sum_fp_many=%.3f\n", fxdll_sum_fp_many(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5));
    int context = 3;
    printf("apply=%d\n", fxdll_apply(square_plus_context, &context, 10));
    FxPoint p = fxdll_make_point(4, 5);
    printf("point=%d,%d\n", p.x, p.y);
    FxBig b = fxdll_make_big(7);
    printf("big=%lld,%lld,%lld weight=%.3f\n", b.values[0], b.values[3], b.values[5], b.weight);
    printf("vsum=%d %d\n", fxdll_vsum(3, 10, 20, 30), fxdll_vsum(9, 1, 2, 3, 4, 5, 6, 7, 8, 9));

    fxdll_tls_set(5);
    long attach_before = fxdll_thread_attach, detach_before = fxdll_thread_detach;
    HANDLE t = CreateThread(NULL, 0, thread_body, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    printf("thread tls=%lu main tls=%d attach delta=%ld detach delta=%ld\n", code, fxdll_tls_get(),
           fxdll_thread_attach - attach_before, fxdll_thread_detach - detach_before);

    HMODULE dll = GetModuleHandleA("fxcorpus_dll.dll");
    typedef int (*ByOrdinal)(int);
    ByOrdinal by_ordinal = (ByOrdinal)GetProcAddress(dll, MAKEINTRESOURCEA(7));
    ByOrdinal by_name = (ByOrdinal)GetProcAddress(dll, "fxdll_by_ordinal");
    printf("ordinal export=%d by-name-null=%d\n", by_ordinal ? by_ordinal(3) : -1, by_name == NULL);
    fflush(stdout);
    return 0;
}
