// Runtime code generation: write machine code into VirtualAlloc'd memory,
// FlushInstructionCache, run it, then patch it and run again. Exercises
// translation invalidation (same address, same page, re-allocated page).
// ARM64 and x64 builds emit equivalent code for their own ISA.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*Fn0)(void);
typedef int (*Fn1)(int);
typedef int (*FnCb)(int, int (*)(int));

static int callback_target(int v) {
    return v * 10 + 3;
}

#if defined(_M_ARM64)
static size_t emit_return_const(uint8_t* p, int value) {
    uint32_t* w = (uint32_t*)p;
    w[0] = 0x52800000u | ((uint32_t)(value & 0xffff) << 5); // movz w0, #value
    w[1] = 0xD65F03C0u;                                     // ret
    return 8;
}
static size_t emit_add_const(uint8_t* p, int value) {
    uint32_t* w = (uint32_t*)p;
    w[0] = 0x11000000u | ((uint32_t)(value & 0xfff) << 10); // add w0, w0, #value
    w[1] = 0xD65F03C0u;                                     // ret
    return 8;
}
// int sum(int n) { int s = 0; for (int i = n; i != 0; i--) s += i * step; }
static size_t emit_loop(uint8_t* p, int step) {
    uint32_t* w = (uint32_t*)p;
    w[0] = 0x52800001u;                                    // movz w1, #0        (s)
    w[1] = 0x52800002u | ((uint32_t)(step & 0xffff) << 5); // movz w2, #step
    w[2] = 0x34000080u;                                    // cbz w0, +4 insns (to ret)
    w[3] = 0x1B020401u;                                    // madd w1, w0, w2, w1
    w[4] = 0x51000400u;                                    // sub w0, w0, #1
    w[5] = 0x17FFFFFDu;                                    // b -3 (to cbz)
    w[6] = 0x2A0103E0u;                                    // mov w0, w1
    w[7] = 0xD65F03C0u;                                    // ret
    return 32;
}
// int call_cb(int v, int (*cb)(int)) { return cb(v); } as a tail call
static size_t emit_tail_call(uint8_t* p) {
    uint32_t* w = (uint32_t*)p;
    w[0] = 0xAA0103F0u; // mov x16, x1
    w[1] = 0xD61F0200u; // br x16
    return 8;
}
#elif defined(_M_X64)
static size_t emit_return_const(uint8_t* p, int value) {
    p[0] = 0xB8; // mov eax, imm32
    memcpy(p + 1, &value, 4);
    p[5] = 0xC3; // ret
    return 6;
}
static size_t emit_add_const(uint8_t* p, int value) {
    p[0] = 0x8D; // lea eax, [rcx + imm32]
    p[1] = 0x81;
    memcpy(p + 2, &value, 4);
    p[6] = 0xC3;
    return 7;
}
static size_t emit_loop(uint8_t* p, int step) {
    static const uint8_t code[] = {
        0x31, 0xC0,                   // xor eax, eax
        0x85, 0xC9,                   // test ecx, ecx
        0x74, 0x0E,                   // jz done (+14)
        0x89, 0xCA,                   // loop: mov edx, ecx
        0x69, 0xD2, 0, 0, 0, 0,       // imul edx, edx, imm32 (step)
        0x01, 0xD0,                   // add eax, edx
        0xFF, 0xC9,                   // dec ecx
        0x75, 0xF2,                   // jnz loop (-14)
        0xC3,                         // done: ret
    };
    memcpy(p, code, sizeof code);
    memcpy(p + 10, &step, 4);
    return sizeof code;
}
static size_t emit_tail_call(uint8_t* p) {
    static const uint8_t code[] = {
        0x48, 0x89, 0xD0, // mov rax, rdx
        0xFF, 0xE0,       // jmp rax
    };
    memcpy(p, code, sizeof code);
    return sizeof code;
}
#else
#error unsupported architecture
#endif

static void flush(void* p, size_t n) {
    FlushInstructionCache(GetCurrentProcess(), p, n);
}

int main(void) {
    // W^X: RW -> write -> RX -> run -> RW -> patch -> RX -> run.
    uint8_t* code = (uint8_t*)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old = 0;
    size_t n = emit_return_const(code, 11);
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old);
    flush(code, n);
    int first = ((Fn0)code)();
    VirtualProtect(code, 4096, PAGE_READWRITE, &old);
    n = emit_return_const(code, 22);
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old);
    flush(code, n);
    int second = ((Fn0)code)();
    printf("wx patch: %d -> %d\n", first, second);

    // RWX page: two functions on one page; patch one, the other is unchanged.
    uint8_t* rwx = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    emit_add_const(rwx, 5);
    emit_return_const(rwx + 64, 300);
    flush(rwx, 128);
    int a1 = ((Fn1)rwx)(10), b1 = ((Fn0)(rwx + 64))();
    emit_add_const(rwx, 50);
    flush(rwx, 64);
    int a2 = ((Fn1)rwx)(10), b2 = ((Fn0)(rwx + 64))();
    printf("rwx same page: a %d -> %d, b %d -> %d\n", a1, a2, b1, b2);

    // Repeated patch/call cycles at the same address.
    long long total = 0;
    for (int i = 0; i < 2000; i++) {
        emit_return_const(rwx + 256, i);
        flush(rwx + 256, 16);
        total += ((Fn0)(rwx + 256))();
    }
    printf("repatch total=%lld\n", total);

    // Generated loop, then patch its step constant.
    emit_loop(rwx + 512, 1);
    flush(rwx + 512, 64);
    int s1 = ((Fn1)(rwx + 512))(1000);
    int hot = 0;
    for (int i = 0; i < 100; i++) hot += ((Fn1)(rwx + 512))(10);
    emit_loop(rwx + 512, 3);
    flush(rwx + 512, 64);
    int s2 = ((Fn1)(rwx + 512))(1000);
    printf("loop step1=%d hot=%d step3=%d\n", s1, hot, s2);

    // Generated code calling back into compiled code.
    emit_tail_call(rwx + 1024);
    flush(rwx + 1024, 16);
    printf("tail call=%d\n", ((FnCb)(rwx + 1024))(7, callback_target));

    // Release and re-allocate at the same address with different code.
    void* fixed = code + 16384;
    VirtualProtect(code, 65536, PAGE_EXECUTE_READWRITE, &old);
    emit_return_const((uint8_t*)fixed, 1234);
    flush(fixed, 16);
    int before = ((Fn0)fixed)();
    VirtualFree(code, 0, MEM_RELEASE);
    uint8_t* again = (uint8_t*)VirtualAlloc(code, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!again) again = (uint8_t*)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    emit_return_const(again + 16384, 4321);
    flush(again + 16384, 16);
    int after = ((Fn0)(again + 16384))();
    printf("realloc before=%d after=%d\n", before, after);

    // Decommit/recommit: the page comes back zeroed, then gets new code.
    VirtualFree(again, 4096, MEM_DECOMMIT);
    VirtualAlloc(again, 4096, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    int zeroed = again[0] == 0 && again[1] == 0;
    emit_add_const(again, 77);
    flush(again, 16);
    printf("recommit zeroed=%d result=%d\n", zeroed, ((Fn1)again)(1));

    VirtualFree(again, 0, MEM_RELEASE);
    VirtualFree(rwx, 0, MEM_RELEASE);
    fflush(stdout);
    return 0;
}
