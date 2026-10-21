// GetCurrentThreadStackLimits: the reported [low, high) range contains a
// local variable's address, on the main thread (default 1 MiB reserve from
// the PE header), on threads with default and explicit sizes, and inside a
// callback invoked by the CRT.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    unsigned long long low, high, local;
} Limits;

static void sample(Limits* out) {
    volatile int local = 0;
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    out->low = low;
    out->high = high;
    out->local = (unsigned long long)(uintptr_t)&local;
}

static void report(const char* what, const Limits* l, int print_size) {
    int contains = l->local >= l->low && l->local < l->high;
    int aligned = (l->low & 0xfff) == 0 && (l->high & 0xfff) == 0;
    // The local is near the top of a fresh stack (within 256 KiB).
    int near_top = l->high - l->local < (256u << 10);
    printf("%s: contains-local=%d page-aligned=%d near-top=%d", what, contains, aligned, near_top);
    if (print_size) printf(" reserve KiB=%llu", (l->high - l->low) >> 10);
    printf("\n");
}

static DWORD WINAPI thread_proc(void* arg) {
    sample((Limits*)arg);
    return 0;
}

static Limits in_callback;
static int __cdecl compare(const void* a, const void* b) {
    sample(&in_callback);
    return *(const int*)a - *(const int*)b;
}

int main(void) {
    Limits main_limits;
    sample(&main_limits);
    report("main", &main_limits, 1);

    Limits t_default, t_reserve, t_commit;
    HANDLE h = CreateThread(NULL, 0, thread_proc, &t_default, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    report("thread default", &t_default, 1);
    h = CreateThread(NULL, 4 << 20, thread_proc, &t_reserve, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    report("thread reserve 4MiB", &t_reserve, 1);
    h = CreateThread(NULL, 2 << 20, thread_proc, &t_commit, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    report("thread commit 2MiB", &t_commit, 0);
    printf("thread commit 2MiB reserve>=2MiB=%d\n", (t_commit.high - t_commit.low) >= (2u << 20));
    printf("thread stacks distinct from main=%d\n", t_default.low != main_limits.low &&
                                                       t_reserve.low != main_limits.low &&
                                                       t_commit.low != main_limits.low);

    int values[4] = {3, 1, 2, 0};
    qsort(values, 4, sizeof values[0], compare);
    int same_stack = in_callback.low == main_limits.low && in_callback.high == main_limits.high;
    printf("callback: contains-local=%d same-as-main=%d\n",
           in_callback.local >= in_callback.low && in_callback.local < in_callback.high, same_stack);

    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    printf("TEB StackBase matches high=%d StackLimit within=%d\n", (ULONG_PTR)tib->StackBase == high,
           (ULONG_PTR)tib->StackLimit >= low && (ULONG_PTR)tib->StackLimit < high);
    fflush(stdout);
    return 0;
}
