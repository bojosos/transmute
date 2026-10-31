/* alchemy-fx engine fixture: BLR sites whose targets change, as fx's BLR
 * inline caches see them (RegionExit::ic_offset: a site keeps its first
 * target, and its link to that target is undone when the target's code goes
 * away, like any link). Each step has its own call site:
 *  1. a vtable whose slots are rewritten between calls, then alternated;
 *  2. JIT code (MAP_JIT) called, rewritten in place and flushed, called again;
 *  3. that page unmapped and mapped again (MAP_FIXED) with other code;
 *  4. code run through a read/execute vm_remap alias, rewritten through the
 *     writable mapping (no flush: the alias's translation must not survive);
 *  5. a site filled while its target was another region's tier-0 entry, then
 *     called often enough for the target to tier up.
 * No SDK headers. Prints "fx inline cache: ok" and returns 0, else returns
 * the failed step number. */
typedef unsigned long size_t;
typedef unsigned mach_port_t;
extern void* mmap(void*, size_t, int, int, int, long);
extern int munmap(void*, size_t);
extern int puts(const char*);
extern void sys_icache_invalidate(void*, size_t);
extern mach_port_t mach_task_self_;
extern int vm_allocate(mach_port_t, unsigned long*, unsigned long, int);
extern int vm_deallocate(mach_port_t, unsigned long, unsigned long);
extern int vm_protect(mach_port_t, unsigned long, unsigned long, int, int);
extern int vm_remap(mach_port_t, unsigned long*, unsigned long, unsigned long, int, mach_port_t, unsigned long, int,
                    int*, int*, unsigned);

#define PAGE 16384ul
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 0x2
#define MAP_FIXED 0x10
#define MAP_JIT 0x800
#define MAP_ANON 0x1000
#define MAP_FAILED ((void*)-1)
#define ANYWHERE 1

typedef int (*function)(int);

static volatile int sink;

/* One BLR each (the result is used after the call: not a tail call). */
#define CALL_SITE(name)                                         \
    __attribute__((noinline)) static int name(function f, int x) { \
        const int result = f(x);                                \
        sink += result;                                         \
        return result;                                          \
    }
CALL_SITE(call_vtable)
CALL_SITE(call_jit)
CALL_SITE(call_remapped)
CALL_SITE(call_alias)
CALL_SITE(call_hot)

__attribute__((noinline)) static int plus_one(int x) { return x + 1; }
__attribute__((noinline)) static int twice(int x) { return x * 2; }
__attribute__((noinline)) static int minus_five(int x) { return x - 5; }
/* A loop: its region counts iterations and tiers up (step 5). */
__attribute__((noinline)) static int sum_to(int x) {
    int total = 0;
    for (volatile int i = 0; i < x; ++i) total += i;
    return total;
}

static function volatile vtable[3];

static void put_code(volatile unsigned* at, unsigned value) {
    at[0] = 0x52800000u | (value << 5);   /* mov w0, #value */
    at[1] = 0xd65f03c0u;                  /* ret */
}

static int expected(function f, int x) {
    if (f == plus_one) return x + 1;
    if (f == twice) return x * 2;
    return x - 5;
}

static int vtables(void) {
    vtable[0] = plus_one;
    vtable[1] = twice;
    vtable[2] = minus_five;
    unsigned index = 0;
    for (int i = 0; i < 600; ++i) {
        if (i == 150) vtable[0] = twice;
        if (i == 300) vtable[0] = minus_five;
        if (i == 350) vtable[0] = plus_one;
        if (i >= 400) index = (unsigned)i % 3;
        const function f = vtable[index];
        if (call_vtable(f, i) != expected(f, i)) return 0;
    }
    return 1;
}

int main(void) {
    if (!vtables()) return 1;

    /* 2. JIT code rewritten in place. */
    char* jit = mmap(0, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit == MAP_FAILED) return 2;
    for (unsigned value = 11; value <= 15; ++value) {
        put_code((volatile unsigned*)jit, value);
        sys_icache_invalidate(jit, 8);
        for (int i = 0; i < 50; ++i)
            if (call_jit((function)jit, i) != (int)value) return 2;
    }

    /* 3. The page unmapped and mapped again at the same address. */
    put_code((volatile unsigned*)jit, 21);
    sys_icache_invalidate(jit, 8);
    for (int i = 0; i < 50; ++i)
        if (call_remapped((function)jit, i) != 21) return 3;
    if (munmap(jit, PAGE) != 0) return 3;
    char* again = mmap(jit, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT | MAP_FIXED, -1, 0);
    if (again != jit) return 3;
    put_code((volatile unsigned*)jit, 22);
    sys_icache_invalidate(jit, 8);
    for (int i = 0; i < 50; ++i)
        if (call_remapped((function)jit, i) != 22) return 3;
    munmap(jit, PAGE);

    /* 4. A read/execute alias rewritten through the writable mapping. */
    {
        const mach_port_t task = mach_task_self_;
        unsigned long a = 0, c = 0;
        int current = 0, maximum = 0;
        if (vm_allocate(task, &a, PAGE, ANYWHERE) != 0) return 4;
        volatile unsigned* writable = (volatile unsigned*)a;
        put_code(writable + 64, 31);
        if (vm_remap(task, &c, PAGE, 0, ANYWHERE, task, a, 0, &current, &maximum, 0) != 0) return 4;
        if (vm_protect(task, c, PAGE, 0, PROT_READ | PROT_EXEC) != 0) return 4;
        const function aliased = (function)(c + 64 * 4);
        for (unsigned value = 31; value <= 34; ++value) {
            put_code(writable + 64, value);
            for (int i = 0; i < 50; ++i)
                if (call_alias(aliased, i) != (int)value) return 4;
        }
        if (vm_deallocate(task, a, PAGE) != 0 || vm_deallocate(task, c, PAGE) != 0) return 4;
    }

    /* 5. The target tiers up under a filled site. */
    for (int i = 0; i < 20000; ++i)
        if (call_hot(sum_to, i & 63) != (i & 63) * ((i & 63) - 1) / 2) return 5;

    puts("fx inline cache: ok");
    return 0;
}
