/* alchemy-fx engine fixture: an instruction fetch from memory that holds no
 * code raises the guest's SIGSEGV (unmapped, KERN_INVALID_ADDRESS) or SIGBUS
 * (mapped without execute, KERN_PROTECTION_FAILURE), as Darwin's
 * EXC_BAD_ACCESS does: si_addr, the mcontext's far and pc are the target and
 * lr is intact; the handler runs with the signal blocked and leaves with
 * siglongjmp, or returns through sigreturn to a pc it changed. Each step
 * reaches the target through another path of fx's control flow:
 *  1. BLR, BR and RET to 0 (null function pointers);
 *  2. a BLR site whose inline cache holds two targets, then 0;
 *  3. a RET whose return-stack prediction is wrong, to an unmapped page;
 *  4. a BLR site whose inline cache holds JIT code, called after that code
 *     was unmapped; a mapped page without execute, and one with no access;
 *  5. a handler that returns: pc moved to lr and x0 set, every other
 *     register (x16, x17, x19..x28) and the mask as at the fault;
 *  6. a handler on the alternate stack (SA_ONSTACK);
 *  7. many faults in a row.
 * With "unhandled" as argv[1]: a BLR to 0 without a handler, which ends the
 * process with SIGSEGV (status 139). No SDK headers. Prints
 * "fx exec fault: ok" and returns 0, else returns the failed step number. */
typedef unsigned long size_t;
extern void* mmap(void*, size_t, int, int, int, long);
extern int munmap(void*, size_t);
extern int mprotect(void*, size_t, int);
extern int puts(const char*);
extern void sys_icache_invalidate(void*, size_t);

struct sig_action { void* handler; unsigned mask; int flags; };
struct sig_info { int signo, error, code, pid; unsigned uid; int status; void* addr; long value, band, pad[7]; };
struct sig_stack { void* base; size_t size; int flags; };
extern int sigaction(int, const struct sig_action*, struct sig_action*);
extern int sigprocmask(int, const unsigned*, unsigned*);
extern int sigaltstack(const struct sig_stack*, struct sig_stack*);
typedef int sig_jmp_buf[49];
extern int sigsetjmp(sig_jmp_buf, int) __attribute__((returns_twice));
extern void siglongjmp(sig_jmp_buf, int) __attribute__((noreturn));

#define PAGE 16384ul
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 0x2
#define MAP_ANON 0x1000
#define MAP_FAILED ((void*)-1)
#define SIGBUS 10
#define SIGSEGV 11
#define SA_ONSTACK 0x1
#define SA_SIGINFO 0x40
#define BIT(signal) (1u << ((signal) - 1))

/* do_blr(target): calls it, returns its x0. do_br(target): branches to it
 * (lr: the caller's). do_ret(target): returns to it (lr = target).
 * do_rsb(target): calls rsb_callee, which returns to `target` instead.
 * do_blr_regs(target): x16, x17, x19..x28 set to their numbers around a call
 * to `target`; returns its x0 plus 256 per register that changed. */
__asm__(".text\n"
        ".p2align 2\n"
        ".globl _do_blr, _blr_after, _do_br, _do_ret, _do_rsb, _do_blr_regs, _regs_after\n"
        "_do_blr:\n"
        "  stp x29, x30, [sp, #-16]!\n"
        "  mov x29, sp\n"
        "  blr x0\n"
        "_blr_after:\n"
        "  ldp x29, x30, [sp], #16\n"
        "  ret\n"
        "_do_br:\n"
        "  br x0\n"
        "_do_ret:\n"
        "  mov x30, x0\n"
        "  ret\n"
        "_do_rsb:\n"
        "  stp x29, x30, [sp, #-16]!\n"
        "  mov x29, sp\n"
        "  bl _rsb_callee\n"
        "  ldp x29, x30, [sp], #16\n"
        "  ret\n"
        "_rsb_callee:\n"
        "  mov x30, x0\n"
        "  ret\n"
        "_do_blr_regs:\n"
        "  stp x29, x30, [sp, #-96]!\n"
        "  mov x29, sp\n"
        "  stp x19, x20, [sp, #16]\n"
        "  stp x21, x22, [sp, #32]\n"
        "  stp x23, x24, [sp, #48]\n"
        "  stp x25, x26, [sp, #64]\n"
        "  stp x27, x28, [sp, #80]\n"
        "  mov x16, #16\n  mov x17, #17\n  mov x19, #19\n  mov x20, #20\n  mov x21, #21\n"
        "  mov x22, #22\n  mov x23, #23\n  mov x24, #24\n  mov x25, #25\n  mov x26, #26\n"
        "  mov x27, #27\n  mov x28, #28\n"
        "  blr x0\n"
        "_regs_after:\n"
        "  mov x9, #0\n"
        "  cmp x16, #16\n  cinc x9, x9, ne\n  cmp x17, #17\n  cinc x9, x9, ne\n"
        "  cmp x19, #19\n  cinc x9, x9, ne\n  cmp x20, #20\n  cinc x9, x9, ne\n"
        "  cmp x21, #21\n  cinc x9, x9, ne\n  cmp x22, #22\n  cinc x9, x9, ne\n"
        "  cmp x23, #23\n  cinc x9, x9, ne\n  cmp x24, #24\n  cinc x9, x9, ne\n"
        "  cmp x25, #25\n  cinc x9, x9, ne\n  cmp x26, #26\n  cinc x9, x9, ne\n"
        "  cmp x27, #27\n  cinc x9, x9, ne\n  cmp x28, #28\n  cinc x9, x9, ne\n"
        "  add x0, x0, x9, lsl #8\n"
        "  ldp x19, x20, [sp, #16]\n"
        "  ldp x21, x22, [sp, #32]\n"
        "  ldp x23, x24, [sp, #48]\n"
        "  ldp x25, x26, [sp, #64]\n"
        "  ldp x27, x28, [sp, #80]\n"
        "  ldp x29, x30, [sp], #96\n"
        "  ret\n");
extern long do_blr(void*);
extern long do_br(void*);
extern long do_ret(void*);
extern long do_rsb(void*);
extern long do_blr_regs(void*);
extern char blr_after[], regs_after[];

typedef long (*function)(long);

static sig_jmp_buf jump;
static volatile int handler_returns;   /* 0: siglongjmp; 1: return with pc = lr, x0 = 42 */
static volatile int got_signal, got_code, faults;
static void* volatile got_addr;
static void* volatile got_far;
static void* volatile got_pc;
static void* volatile got_lr;
static volatile unsigned long got_sp;
static volatile unsigned got_mask;
static volatile long sink;

static void handler(int signal, struct sig_info* info, void* context) {
    char* machine = *(char**)((char*)context + 48);
    unsigned mask = 0;
    got_signal = signal;
    got_code = info->code;
    got_addr = info->addr;
    got_far = *(void**)machine;
    got_pc = *(void**)(machine + 272);
    got_lr = *(void**)(machine + 256);
    got_sp = (unsigned long)&mask;
    sigprocmask(1, 0, &mask);
    got_mask = mask;
    ++faults;
    if (handler_returns) {
        *(void**)(machine + 272) = got_lr;
        *(long*)(machine + 16) = 42;
        return;
    }
    siglongjmp(jump, 1);
}

static int install(int flags) {
    struct sig_action action = {(void*)handler, 0, SA_SIGINFO | flags};
    return sigaction(SIGSEGV, &action, 0) == 0 && sigaction(SIGBUS, &action, 0) == 0;
}

static unsigned current_mask(void) {
    unsigned mask = 0;
    sigprocmask(1, 0, &mask);
    return mask;
}

/* Calls `call(target)` and expects `signal` with si_addr and pc `target` (and
 * lr `lr` unless it is 0), the signal blocked in the handler and unblocked
 * again after siglongjmp. */
static int expect_fault(long (*call)(void*), void* target, int signal, void* lr) {
    got_signal = 0;
    if (sigsetjmp(jump, 1) == 0) {
        sink = call(target);
        return 0;
    }
    return got_signal == signal && got_code == 1 && got_addr == target && got_far == target && got_pc == target &&
           (!lr || got_lr == lr) && (got_mask & BIT(signal)) && !(current_mask() & BIT(signal));
}

/* One BLR each (the result is used after the call: not a tail call). */
#define CALL_SITE(name)                                              \
    __attribute__((noinline)) static long name(void* f) {            \
        const long result = ((function)f)(5);                        \
        sink += result;                                              \
        return result;                                               \
    }
CALL_SITE(call_two)
CALL_SITE(call_jit)

__attribute__((noinline)) static long plus_one(long x) { return x + 1; }
__attribute__((noinline)) static long twice(long x) { return x * 2; }

static void* map(int protection) {
    void* page = mmap(0, PAGE, protection, MAP_PRIVATE | MAP_ANON, -1, 0);
    return page == MAP_FAILED ? 0 : page;
}

static char alternate[65536] __attribute__((aligned(16)));

int main(int argc, char** argv) {
    if (argc > 1 && argv[1][0] == 'u') {
        puts("fx exec fault: unhandled BLR to 0");
        return (int)do_blr(0);
    }
    if (!install(0)) return 100;

    /* 1. Null function pointers. */
    if (!expect_fault(do_blr, 0, SIGSEGV, blr_after)) return 1;
    if (!expect_fault(do_br, 0, SIGSEGV, 0) || !got_lr) return 1;
    if (!expect_fault(do_ret, 0, SIGSEGV, 0)) return 1;

    /* 2. A BLR site with both inline cache entries filled, then 0. */
    for (int i = 0; i < 200; ++i)
        if (call_two(i & 1 ? (void*)plus_one : (void*)twice) != (i & 1 ? 6 : 10)) return 2;
    if (!expect_fault(call_two, 0, SIGSEGV, 0)) return 2;
    if (call_two((void*)plus_one) != 6 || call_two((void*)twice) != 10) return 2;

    /* 3. A mispredicted return to an unmapped page. */
    char* gone = map(PROT_READ | PROT_WRITE);
    if (!gone || munmap(gone, PAGE) != 0) return 3;
    if (!expect_fault(do_rsb, gone, SIGSEGV, gone)) return 3;
    if (!expect_fault(do_ret, gone + 64, SIGSEGV, gone + 64)) return 3;

    /* 4. JIT code called through an inline cache, then unmapped; a page
     *    without execute and one without access. */
    unsigned* code = map(PROT_READ | PROT_WRITE);
    if (!code) return 4;
    code[0] = 0x91001c00u;   /* add x0, x0, #7 */
    code[1] = 0xd65f03c0u;   /* ret */
    if (mprotect(code, PAGE, PROT_READ | PROT_EXEC) != 0) return 4;
    sys_icache_invalidate(code, 8);
    for (int i = 0; i < 200; ++i)
        if (call_jit(code) != 12) return 4;
    if (munmap(code, PAGE) != 0) return 4;
    if (!expect_fault(call_jit, code, SIGSEGV, 0)) return 4;
    char* data = map(PROT_READ | PROT_WRITE);
    char* none = map(PROT_NONE);
    if (!data || !none) return 4;
    data[0] = 1;
    if (!expect_fault(do_blr, data, SIGBUS, blr_after)) return 4;
    if (!expect_fault(do_blr, none + 32, SIGBUS, blr_after)) return 4;

    /* 5. The handler returns: sigreturn resumes at the pc it set. */
    handler_returns = 1;
    got_signal = 0;
    const long result = do_blr_regs(0);
    handler_returns = 0;
    if (result != 42 || got_signal != SIGSEGV || got_pc != 0 || got_lr != regs_after ||
        !(got_mask & BIT(SIGSEGV)) || (current_mask() & BIT(SIGSEGV)))
        return 5;

    /* 6. On the alternate stack. */
    const struct sig_stack stack = {alternate, sizeof alternate, 0};
    const struct sig_stack disable = {0, 0, 4};
    if (sigaltstack(&stack, 0) != 0 || !install(SA_ONSTACK)) return 6;
    if (!expect_fault(do_blr, 0, SIGSEGV, blr_after)) return 6;
    if (got_sp < (unsigned long)alternate || got_sp >= (unsigned long)alternate + sizeof alternate) return 6;
    if (sigaltstack(&disable, 0) != 0 || !install(0)) return 6;
    if (!expect_fault(do_blr, 0, SIGSEGV, blr_after)) return 6;
    if (got_sp >= (unsigned long)alternate && got_sp < (unsigned long)alternate + sizeof alternate) return 6;

    /* 7. Many in a row, through each path (a fresh hole: mmap reuses freed pages). */
    char* hole = map(PROT_READ | PROT_WRITE);
    if (!hole || munmap(hole, PAGE) != 0) return 7;
    const int before = faults;
    for (int i = 0; i < 300; ++i) {
        void* target = i % 3 == 0 ? 0 : i % 3 == 1 ? (void*)hole : (void*)data;
        const int signal = i % 3 == 2 ? SIGBUS : SIGSEGV;
        if (!expect_fault(i & 1 ? do_blr : call_two, target, signal, 0)) return 7;
    }
    if (faults - before != 300) return 7;

    puts("fx exec fault: ok");
    return 0;
}
