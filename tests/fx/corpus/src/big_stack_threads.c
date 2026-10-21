// Threads with 16 MiB stacks created through _beginthreadex and CreateThread,
// both as commit sizes and with STACK_SIZE_PARAM_IS_A_RESERVATION. Each
// recurses through ~12 MiB of 32 KiB frames and checks its stack limits.
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>

#define FRAME_BYTES (32 * 1024)
#define TARGET_BYTES (12 * 1024 * 1024)
#define STACK_BYTES (16 * 1024 * 1024)

typedef struct {
    const char* how;
    unsigned long long sum;
    unsigned long long reserve_mib;
    int contains_local;
    int used_ok;
    int within_limits;
} Report;

__declspec(noinline) static unsigned long long recurse(int depth, int max_depth, uintptr_t* deepest) {
    volatile unsigned char frame[FRAME_BYTES];
    for (int i = 0; i < FRAME_BYTES; i += 4096) frame[i] = (unsigned char)(depth * 3 + i / 4096);
    frame[FRAME_BYTES - 1] = (unsigned char)depth;
    uintptr_t here = (uintptr_t)&frame[0];
    if (here < *deepest) *deepest = here;
    unsigned long long sum = 0;
    if (depth < max_depth) sum = recurse(depth + 1, max_depth, deepest);
    for (int i = 0; i < FRAME_BYTES; i += 4096) sum += frame[i];
    return sum + frame[FRAME_BYTES - 1];
}

static void run(Report* r) {
    int local = 0;
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    uintptr_t addr = (uintptr_t)&local;
    uintptr_t deepest = addr;
    r->reserve_mib = (unsigned long long)((high - low) >> 20);
    r->contains_local = addr >= low && addr < high;
    r->sum = recurse(1, TARGET_BYTES / FRAME_BYTES, &deepest);
    r->used_ok = (addr - deepest) >= (12u << 20) - FRAME_BYTES;
    r->within_limits = deepest >= low;
}

static unsigned __stdcall crt_thread(void* arg) {
    run((Report*)arg);
    return 0;
}

static DWORD WINAPI win32_thread(void* arg) {
    run((Report*)arg);
    return 0;
}

int main(void) {
    Report reports[4] = {
        {"_beginthreadex commit"}, {"_beginthreadex reserve"}, {"CreateThread commit"}, {"CreateThread reserve"}};
    HANDLE h[4];
    h[0] = (HANDLE)_beginthreadex(NULL, STACK_BYTES, crt_thread, &reports[0], 0, NULL);
    h[1] = (HANDLE)_beginthreadex(NULL, STACK_BYTES, crt_thread, &reports[1], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    h[2] = CreateThread(NULL, STACK_BYTES, win32_thread, &reports[2], 0, NULL);
    h[3] = CreateThread(NULL, STACK_BYTES, win32_thread, &reports[3], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    for (int i = 0; i < 4; i++) {
        if (!h[i]) {
            printf("%s: create failed error=%lu\n", reports[i].how, GetLastError());
            continue;
        }
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
        const Report* r = &reports[i];
        printf("%s: reserve>=16MiB=%d contains-local=%d sum=%llu used>=12MiB=%d within-limits=%d\n", r->how,
               r->reserve_mib >= 16, r->contains_local, r->sum, r->used_ok, r->within_limits);
    }
    // Four 16 MiB threads at once.
    Report together[4] = {{"a"}, {"b"}, {"c"}, {"d"}};
    for (int i = 0; i < 4; i++) {
        h[i] = CreateThread(NULL, STACK_BYTES, win32_thread, &together[i], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    }
    WaitForMultipleObjects(4, h, TRUE, INFINITE);
    int all_ok = 1;
    for (int i = 0; i < 4; i++) {
        CloseHandle(h[i]);
        all_ok &= together[i].used_ok && together[i].within_limits && together[i].sum == reports[0].sum;
    }
    printf("concurrent 16 MiB threads ok=%d\n", all_ok);
    fflush(stdout);
    return 0;
}
