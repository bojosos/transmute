// setjmp/longjmp: same-frame and cross-frame jumps, value 0 -> 1, preserved
// volatile locals, multiple buffers, and __finally blocks run by longjmp's
// unwind (Windows longjmp unwinds on x64 and ARM64).
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf main_buf;
static jmp_buf loop_buf;
static jmp_buf finally_buf;
static char trace[256];

static void mark(const char* s) {
    strncat(trace, s, sizeof trace - strlen(trace) - 1);
}

__declspec(noinline) static void jump_from_depth(int depth, int value) {
    volatile char pad[256];
    pad[0] = (char)depth;
    if (depth == 0) longjmp(main_buf, value);
    jump_from_depth(depth - 1, value + pad[0] * 0);
}

__declspec(noinline) static void finally_frame(int depth) {
    __try {
        if (depth == 0) {
            mark("jump;");
            longjmp(finally_buf, 5);
        }
        finally_frame(depth - 1);
    } __finally {
        char buf[16];
        snprintf(buf, sizeof buf, "fin%d;", depth);
        mark(buf);
    }
}

__declspec(noinline) static double fp_work(double x) {
    return x * 1.5 + 0.25;
}

int main(void) {
    volatile int counter = 0;
    volatile double fp = 1.0;
    int r = setjmp(main_buf);
    if (r == 0) {
        counter = 10;
        fp = fp_work(fp);
        jump_from_depth(5, 42);
        printf("not reached\n");
    }
    printf("first return r=%d counter=%d fp=%.4f\n", r, counter, fp);

    r = setjmp(main_buf);
    if (r == 0) longjmp(main_buf, 0);
    printf("longjmp(0) delivered r=%d\n", r);

    volatile int iterations = 0;
    volatile long long sum = 0;
    r = setjmp(loop_buf);
    sum += r;
    if (iterations < 100) {
        iterations++;
        longjmp(loop_buf, iterations);
    }
    printf("loop iterations=%d sum=%lld\n", iterations, sum);

    trace[0] = 0;
    r = setjmp(finally_buf);
    if (r == 0) {
        finally_frame(3);
    }
    mark("landed;");
    printf("finally r=%d trace=%s\n", r, trace);

    // Callee-saved registers (locals kept in registers across the call) must
    // be restored by longjmp; volatile ones hold their latest values.
    volatile int a = 1, b = 2, c = 3;
    double d1 = 0.5, d2 = 0.75;
    r = setjmp(main_buf);
    if (r == 0) {
        a = 11;
        b = 22;
        c = 33;
        jump_from_depth(2, 9);
    }
    printf("after jump a=%d b=%d c=%d d=%.2f r=%d\n", a, b, c, d1 + d2, r);
    fflush(stdout);
    return 0;
}
