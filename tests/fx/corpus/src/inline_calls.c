// Calls of small leaf functions, which the fx translator inlines into the
// caller's region: hardware faults inside such callees (the exception address
// is the callee instruction; SEH unwinds the leaf through x30 and runs the
// callers' __finally blocks), and code changes of a generated leaf callee on
// another page than its caller (the caller's translation must be
// invalidated too). ARM64 and x64 builds emit equivalent code for their ISA.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- faults inside inlined leaves ---------------------------------------------

__declspec(noinline) static int load_at(volatile int* p) { return *p; }

__declspec(noinline) static int sum_n(volatile const int* p, int n) {
    int s = 0;
#pragma clang loop vectorize(disable) unroll(disable)
    for (int i = 0; i < n; i++) s += p[i] * (i + 1);
    return s;
}

// mode 0: the loop leaf reads p[0..i] first; mode 1: the load leaf reads p[i] first.
__declspec(noinline) static int caller(volatile int* p, int k, int mode) {
    int r = 0;
    for (int i = 0; i < k; i++) {
        if (mode) {
            r += load_at(p + i);
            r += sum_n(p, i + 1);
        } else {
            r += sum_n(p, i + 1);
            r += load_at(p + i);
        }
    }
    return r;
}

static volatile int finally_runs;

__declspec(noinline) static int guarded(volatile int* p, int k, int mode) {
    int r = -1;
    __try {
        r = caller(p, k, mode);
    } __finally {
        finally_runs++;
    }
    return r;
}

static void* fault_address;
static DWORD fault_code;

static int filter(EXCEPTION_POINTERS* ep) {
    fault_code = ep->ExceptionRecord->ExceptionCode;
    fault_address = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

static int in_function(void* address, void* fn, size_t size) {
    return (uintptr_t)address >= (uintptr_t)fn && (uintptr_t)address < (uintptr_t)fn + size;
}

static void test_faults(void) {
    static int data[16];
    for (int i = 0; i < 16; i++) data[i] = i * 3 + 1;
    long long total = 0;
    for (int round = 0; round < 3000; round++) total += guarded(data, 1 + round % 8, round & 1);   // hot: inlined
    printf("calls total=%lld finally=%d\n", total, finally_runs);

    // A page whose second half is inaccessible: the leaf faults part-way.
    uint8_t* page = (uint8_t*)VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old = 0;
    VirtualProtect(page + 4096, 4096, PAGE_NOACCESS, &old);
    volatile int* edge = (volatile int*)(page + 4096 - 3 * sizeof(int));
    for (int i = 0; i < 3; i++) edge[i] = 100 + i;
    for (int j = 0; j < 10; j++) {
        const int k = 1 + j % 5, mode = j / 5;
        finally_runs = 0;
        fault_address = NULL;
        fault_code = 0;
        int r = 0;
        __try {
            r = guarded(edge, k, mode);
        } __except (filter(GetExceptionInformation())) {
            r = -1;
        }
        printf("k=%d mode=%d r=%d code=%08lx in_load=%d in_sum=%d finally=%d\n", k, mode, r, fault_code,
               in_function(fault_address, (void*)load_at, 16),
               in_function(fault_address, (void*)sum_n, 128) && !in_function(fault_address, (void*)load_at, 16),
               finally_runs);
    }
    // NULL: the very first callee instruction faults.
    finally_runs = 0;
    __try {
        guarded(NULL, 2, 1);
    } __except (filter(GetExceptionInformation())) {
        printf("null code=%08lx in_load=%d finally=%d\n", fault_code, in_function(fault_address, (void*)load_at, 16),
               finally_runs);
    }
    // Execution continues normally after the unwinds.
    total = 0;
    for (int round = 0; round < 1000; round++) total += guarded(data, 1 + round % 5, round & 1);
    printf("after faults total=%lld\n", total);
    VirtualFree(page, 0, MEM_RELEASE);
}

// ---- code changes of an inlined generated callee ------------------------------

typedef int (*Fn1)(int);

#if defined(_M_ARM64)
// leaf(x) = x + value
static void emit_leaf(uint8_t* p, int value) {
    uint32_t* w = (uint32_t*)p;
    w[0] = 0x11000000u | ((uint32_t)(value & 0xfff) << 10);   // add w0, w0, #value
    w[1] = 0xD65F03C0u;                                       // ret
}
// caller(x) = leaf(leaf(x)) with a frame
static void emit_caller(uint8_t* p, uint8_t* leaf) {
    uint32_t* w = (uint32_t*)p;
    const int32_t off2 = (int32_t)((leaf - (p + 8)) / 4), off3 = (int32_t)((leaf - (p + 12)) / 4);
    w[0] = 0xA9BF7BFDu;                                       // stp x29, x30, [sp, #-16]!
    w[1] = 0x910003FDu;                                       // mov x29, sp
    w[2] = 0x94000000u | ((uint32_t)off2 & 0x3ffffffu);       // bl leaf
    w[3] = 0x94000000u | ((uint32_t)off3 & 0x3ffffffu);       // bl leaf
    w[4] = 0xA8C17BFDu;                                       // ldp x29, x30, [sp], #16
    w[5] = 0xD65F03C0u;                                       // ret
}
#elif defined(_M_X64)
static void emit_leaf(uint8_t* p, int value) {
    p[0] = 0x8D; // lea eax, [rcx + imm32]
    p[1] = 0x81;
    memcpy(p + 2, &value, 4);
    p[6] = 0xC3; // ret
}
static void emit_caller(uint8_t* p, uint8_t* leaf) {
    static const uint8_t code[] = {
        0x48, 0x83, 0xEC, 0x28,       // sub rsp, 40
        0xE8, 0, 0, 0, 0,             // call leaf
        0x89, 0xC1,                   // mov ecx, eax
        0xE8, 0, 0, 0, 0,             // call leaf
        0x48, 0x83, 0xC4, 0x28,       // add rsp, 40
        0xC3,                         // ret
    };
    memcpy(p, code, sizeof code);
    int32_t r1 = (int32_t)(leaf - (p + 9)), r2 = (int32_t)(leaf - (p + 16));
    memcpy(p + 5, &r1, 4);
    memcpy(p + 12, &r2, 4);
}
#else
#error unsupported architecture
#endif

static void flush(void* p, size_t n) {
    FlushInstructionCache(GetCurrentProcess(), p, n);
}

static long long run_hot(Fn1 fn, int rounds) {
    long long total = 0;
    for (int i = 0; i < rounds; i++) total += fn(i & 7);
    return total;
}

static void test_smc(void) {
    // Caller on the first page, leaf on the second (RWX): a store to the
    // leaf's page must invalidate the caller's region too.
    uint8_t* code = (uint8_t*)VirtualAlloc(NULL, 3 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uint8_t* leaf = code + 4096 + 128;
    emit_leaf(leaf, 5);
    emit_caller(code, leaf);
    flush(code, 3 * 4096);
    Fn1 fn = (Fn1)code;
    long long a = run_hot(fn, 20000);
    int a3 = fn(3);
    emit_leaf(leaf, 40);
    flush(leaf, 16);
    long long b = run_hot(fn, 20000);
    printf("rwx leaf patch: %d %lld -> %d %lld\n", a3, a, fn(3), b);

    // Repeated patches of the leaf only.
    long long total = 0;
    for (int i = 0; i < 300; i++) {
        emit_leaf(leaf, i);
        flush(leaf, 16);
        total += fn(i);
    }
    printf("leaf repatch total=%lld\n", total);

    // W^X: leaf page RW -> patch -> RX.
    DWORD old = 0;
    VirtualProtect(code, 3 * 4096, PAGE_EXECUTE_READ, &old);
    long long c = run_hot(fn, 5000);
    VirtualProtect(code + 4096, 4096, PAGE_READWRITE, &old);
    emit_leaf(leaf, 77);
    VirtualProtect(code + 4096, 4096, PAGE_EXECUTE_READ, &old);
    flush(leaf, 16);
    long long d = run_hot(fn, 5000);
    printf("wx leaf patch: %lld -> %lld (fn(1)=%d)\n", c, d, fn(1));
    VirtualFree(code, 0, MEM_RELEASE);
}

int main(void) {
    test_faults();
    test_smc();
    fflush(stdout);
    return 0;
}
