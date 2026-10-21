// Linked with /STACK:16777216 (CPython-style). The main thread recurses with
// 32 KiB frames (each probed through __chkstk) until ~12 MiB of stack is in
// use, and checks GetCurrentThreadStackLimits on the way.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define FRAME_BYTES (32 * 1024)
#define TARGET_BYTES (12 * 1024 * 1024)

static ULONG_PTR stack_low, stack_high;
static uintptr_t deepest;

__declspec(noinline) static unsigned long long recurse(int depth, int max_depth) {
    volatile unsigned char frame[FRAME_BYTES];
    for (int i = 0; i < FRAME_BYTES; i += 4096) frame[i] = (unsigned char)(depth + i / 4096);
    frame[FRAME_BYTES - 1] = (unsigned char)depth;
    uintptr_t here = (uintptr_t)&frame[0];
    if (here < deepest) deepest = here;
    unsigned long long sum = 0;
    if (depth < max_depth) sum = recurse(depth + 1, max_depth);
    for (int i = 0; i < FRAME_BYTES; i += 4096) sum += frame[i];
    return sum + frame[FRAME_BYTES - 1];
}

int main(void) {
    int local = 0;
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    uintptr_t addr = (uintptr_t)&local;
    printf("main reserve MiB=%llu contains-local=%d\n", (unsigned long long)((stack_high - stack_low) >> 20),
           addr >= stack_low && addr < stack_high);
    deepest = addr;
    int depth = TARGET_BYTES / FRAME_BYTES;
    unsigned long long sum = recurse(1, depth);
    unsigned long long used = (unsigned long long)(addr - deepest);
    printf("recursed depth=%d sum=%llu used>=12MiB=%d within-limits=%d\n", depth, sum, used >= (12ull << 20) - FRAME_BYTES,
           deepest >= stack_low);
    // Second pass reuses the now-committed stack.
    deepest = addr;
    sum = recurse(1, depth);
    printf("second pass sum=%llu\n", sum);
    fflush(stdout);
    return 0;
}
