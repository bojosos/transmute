/* alchemy-fx engine fixture: JIT code rewritten in place and announced with
 * sys_icache_invalidate, as V8 does (RWX MAP_JIT pages, code written through
 * the mapping that executes it, then an instruction-cache flush):
 *  1. rewrite + flush on a MAP_JIT page;
 *  2. three functions on one page, the middle one rewritten;
 *  3. code changed by a path fx cannot see (pwrite into a file that a
 *     MAP_SHARED view executes, when Alchemy allows executable file views):
 *     only sys_icache_invalidate drops the old translation; and pread (a
 *     kernel write) into a translated JIT page;
 *  4. a thread spinning inside translated code while another thread
 *     rewrites that code: it must leave the stale loop and run the new code;
 *  5. mprotect of translated JIT code to RX and RW: the guest's protection
 *     is what mach_vm_region reports, also after fx write-protected the
 *     page for SMC and after code on it was invalidated.
 * No SDK headers. Prints "fx jit flush: ok" and returns 0, else returns the
 * failed step number. */
typedef unsigned long size_t;
typedef void* pthread_t;
extern void* mmap(void*, size_t, int, int, int, long);
extern int munmap(void*, size_t);
extern int mprotect(void*, size_t, int);
extern int open(const char*, int, ...);
extern int close(int);
extern long pwrite(int, const void*, size_t, long);
extern long pread(int, void*, size_t, long);
extern int unlink(const char*);
extern int getpid(void);
extern int puts(const char*);
extern int usleep(unsigned);
extern int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern void sys_icache_invalidate(void*, size_t);
typedef unsigned mach_port_t;
extern mach_port_t mach_task_self_;
extern int mach_vm_region_recurse(mach_port_t, unsigned long*, unsigned long*, unsigned*, int*, unsigned*);

#define PAGE 16384ul
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x1
#define MAP_PRIVATE 0x2
#define MAP_JIT 0x800
#define MAP_ANON 0x1000
#define MAP_FAILED ((void*)-1)

typedef int (*function)(void);
typedef int (*spin_function)(volatile int*);

#define MOV_W0(value) (0x52800000u | ((unsigned)(value) << 5))
#define RET 0xd65f03c0u

static void put_code(volatile unsigned* at, unsigned value) {
    at[0] = MOV_W0(value);
    at[1] = RET;
}

/* The protection mach_vm_region_recurse reports for the page at `address`. */
static int protection_at(void* address) {
    int info[19] = {0};
    unsigned depth = 0, count = 19;
    unsigned long base = (unsigned long)address, size = 0;
    if (mach_vm_region_recurse(mach_task_self_, &base, &size, &depth, info, &count) != 0 ||
        base > (unsigned long)address)
        return -1;
    return info[0];
}

static volatile int spin_flag = 0;
static volatile int spin_entered = 0;
static volatile int spin_result = -1;
static spin_function spin_code;

static void* spinner(void* unused) {
    (void)unused;
    spin_entered = 1;
    spin_result = spin_code(&spin_flag);
    return 0;
}

int main(void) {
    /* 1. A MAP_JIT page rewritten through itself, then flushed. */
    char* jit = mmap(0, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit == MAP_FAILED) return 1;
    function run = (function)jit;
    put_code((volatile unsigned*)jit, 1);
    sys_icache_invalidate(jit, 8);
    if (run() != 1 || run() != 1) return 2;
    for (unsigned i = 2; i < 50; ++i) {
        put_code((volatile unsigned*)jit, i);
        sys_icache_invalidate(jit, 8);
        if (run() != (int)i) return 3;
    }

    /* 2. Three functions on one page; only the middle one changes. */
    function a = (function)(jit + 0x100), b = (function)(jit + 0x200), c = (function)(jit + 0x300);
    put_code((volatile unsigned*)(jit + 0x100), 11);
    put_code((volatile unsigned*)(jit + 0x200), 12);
    put_code((volatile unsigned*)(jit + 0x300), 13);
    sys_icache_invalidate(jit + 0x100, 0x208);
    if (a() != 11 || b() != 12 || c() != 13) return 4;
    for (unsigned i = 0; i < 20; ++i) {
        put_code((volatile unsigned*)(jit + 0x200), 20 + i);
        sys_icache_invalidate(jit + 0x200, 8);
        if (a() != 11 || b() != (int)(20 + i) || c() != 13) return 5;
    }
    put_code((volatile unsigned*)(jit + 0x100), 14);   /* a neighbour of translated code */
    sys_icache_invalidate(jit + 0x100, 8);
    if (a() != 14 || b() != 39 || c() != 13) return 6;
    if (munmap(jit, PAGE) != 0) return 7;

    /* 3. Code rewritten through the file under a MAP_SHARED view: no store
     * through any guest mapping, so no write fault can tell fx. */
    {
        static char name[] = "/tmp/fx_jit_flush_00000000.bin";
        int pid = getpid();
        for (int i = 0; i < 8; ++i) name[sizeof(name) - 6 - i] = (char)('0' + (pid >> (4 * i)) % 10);
        int fd = open(name, 0x2 | 0x200 | 0x400, 0600);   /* O_RDWR | O_CREAT | O_TRUNC */
        if (fd < 0) return 10;
        static unsigned words[PAGE / 4];
        put_code(words, 21);
        if (pwrite(fd, words, PAGE, 0) != (long)PAGE) return 11;
        char* view = mmap(0, PAGE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (view == MAP_FAILED) {
            view = mmap(0, PAGE, PROT_READ, MAP_SHARED, fd, 0);
            if (view == MAP_FAILED) return 12;
            if (mprotect(view, PAGE, PROT_READ | PROT_EXEC) != 0) {
                munmap(view, PAGE);
                view = 0;
            }
        }
        unsigned next[2] = {MOV_W0(22), RET};
        if (!view) {
            puts("unseen write: executable file views unsupported, not tested");
        } else {
            function file_code = (function)view;
            if (file_code() != 21 || file_code() != 21) return 14;
            if (pwrite(fd, next, sizeof(next), 0) != (long)sizeof(next)) return 15;
            if (((volatile unsigned*)view)[0] != MOV_W0(22)) return 16;   /* the view sees the file */
            puts(file_code() == 21 ? "unseen write: stale before the flush" : "unseen write: already current");
            sys_icache_invalidate(view, 8);
            if (file_code() != 22) return 17;
            if (munmap(view, PAGE) != 0) return 18;
        }
        /* The kernel's write path: pread into a translated (fx write-protected) JIT page. */
        if (pwrite(fd, next, sizeof(next), 0) != (long)sizeof(next)) return 25;
        char* jit2 = mmap(0, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (jit2 == MAP_FAILED) return 26;
        put_code((volatile unsigned*)jit2, 21);
        sys_icache_invalidate(jit2, 8);
        function read_code = (function)jit2;
        if (read_code() != 21) return 27;
        if (pread(fd, jit2, 8, 0) != 8) return 28;
        sys_icache_invalidate(jit2, 8);
        if (read_code() != 22) return 29;
        if (munmap(jit2, PAGE) != 0) return 19;
        close(fd);
        unlink(name);
    }

    /* 4. A thread spins in translated code (ldr w1, [x0]; cbz w1, loop) that
     * the main thread then turns into "mov w0, #2; ret". */
    {
        char* code = mmap(0, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (code == MAP_FAILED) return 20;
        volatile unsigned* w = (volatile unsigned*)code;
        w[0] = 0xb9400001u;   /* loop: ldr w1, [x0] */
        w[1] = 0x34ffffe1u;   /*       cbz w1, loop */
        w[2] = MOV_W0(1);
        w[3] = RET;
        sys_icache_invalidate(code, 16);
        spin_code = (spin_function)code;
        pthread_t thread;
        if (pthread_create(&thread, 0, spinner, 0) != 0) return 21;
        for (int i = 0; i < 1000 && !spin_entered; ++i) usleep(1000);
        usleep(50000);   /* well inside the loop's translation */
        if (spin_result != -1) return 22;
        w[0] = MOV_W0(2);   /* mov w0, #2; cbz w1, loop: still spinning (w1 == 0) */
        w[1] = RET;
        sys_icache_invalidate(code, 8);
        for (int i = 0; i < 5000 && spin_result == -1; ++i) usleep(1000);
        if (spin_result != 2) {
            spin_flag = 1;   /* release a stale loop so the process can end */
            return 23;
        }
        pthread_join(thread, 0);
        if (munmap(code, PAGE) != 0) return 24;
    }

    /* 5. Protection changes on translated JIT code. */
    {
        char* code = mmap(0, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (code == MAP_FAILED) return 30;
        function f = (function)code;
        put_code((volatile unsigned*)code, 31);
        sys_icache_invalidate(code, 8);
        if (f() != 31) return 31;   /* translated: fx write-protects the page */
        if (protection_at(code) != (PROT_READ | PROT_WRITE | PROT_EXEC)) return 32;
        if (mprotect(code, PAGE, PROT_READ | PROT_EXEC) != 0) return 33;
        if (protection_at(code) != (PROT_READ | PROT_EXEC)) return 34;
        if (f() != 31) return 35;
        /* Invalidating code on the page must not hand it back its old RW. */
        sys_icache_invalidate(code, 8);
        if (protection_at(code) != (PROT_READ | PROT_EXEC) || f() != 31) return 36;
        if (mprotect(code, PAGE, PROT_READ | PROT_WRITE) != 0) return 37;
        if (protection_at(code) != (PROT_READ | PROT_WRITE)) return 38;
        put_code((volatile unsigned*)code, 32);
        if (mprotect(code, PAGE, PROT_READ | PROT_EXEC) != 0) return 39;
        sys_icache_invalidate(code, 8);
        if (f() != 32 || protection_at(code) != (PROT_READ | PROT_EXEC)) return 40;
        if (munmap(code, PAGE) != 0) return 41;
    }

    puts("fx jit flush: ok");
    return 0;
}
