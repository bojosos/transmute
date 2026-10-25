/* alchemy-fx engine fixture: an interpreter whose handlers dispatch through a
 * writable table, as V8's Ignition does (`ldrb w8, [x1], #1;
 * ldr x9, [x2, x8, lsl #3]; mov x17, x9; br x17` at the end of every
 * handler: V8 tail-calls through x17), so fx sends
 * the BRs through its table shadows (GuestContext::table_shadow):
 *  1. many runs over one table: the results stay right once the shadow holds
 *     every handler;
 *  2. a handler rewritten in place and flushed (the shadow's host entry for it
 *     dies with its translation): the new code runs;
 *  3. a table entry pointed at another handler: the BR follows the table;
 *  4. two tables alternating (each thread keeps one shadow at a time);
 *  5. threads running both tables at once.
 * Handlers live on a MAP_JIT page. No SDK headers. Prints "fx table shadow: ok"
 * and returns 0, else returns the failed step number. */
typedef unsigned long size_t;
typedef void* pthread_t;
extern void* mmap(void*, size_t, int, int, int, long);
extern int puts(const char*);
extern int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern void sys_icache_invalidate(void*, size_t);

#define PAGE 16384ul
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 0x2
#define MAP_JIT 0x800
#define MAP_ANON 0x1000
#define MAP_FAILED ((void*)-1)

#define OPS 15                 /* handlers 0..14 add to x0; 15 returns it */
#define HALT 15
#define SLOT 64                /* bytes per handler */
#define PROGRAM 4000

#define ADD_X0(imm) (0x91000000u | ((unsigned)(imm) << 10))   /* add x0, x0, #imm */
#define LDRB_NEXT 0x38401428u  /* ldrb w8, [x1], #1 */
#define LDR_TARGET 0xf8687849u /* ldr x9, [x2, x8, lsl #3] */
#define MOV_X17 0xaa0903f1u    /* mov x17, x9 */
#define BR_X17 0xd61f0220u     /* br x17 */
#define RET 0xd65f03c0u

typedef unsigned long (*run_function)(unsigned long acc, const unsigned char* program, const unsigned long* table);

static unsigned char* jit;
static unsigned long table_a[16], table_b[16];
static unsigned char program[PROGRAM + 1];
static unsigned adds[OPS];

static unsigned long handler(unsigned k) { return (unsigned long)(jit + SLOT * (1 + k)); }

static void put_handler(unsigned k, unsigned add) {
    volatile unsigned* at = (volatile unsigned*)handler(k);
    at[0] = ADD_X0(add);
    at[1] = LDRB_NEXT;
    at[2] = LDR_TARGET;
    at[3] = MOV_X17;
    at[4] = BR_X17;
    adds[k] = add;
    sys_icache_invalidate((void*)at, 20);
}

static unsigned long expected(const unsigned long* table) {
    unsigned long acc = 0;
    for (const unsigned char* p = program; *p != HALT; ++p) {
        unsigned k = 0;
        while (k < OPS && table[*p] != handler(k)) ++k;
        acc += adds[k];
    }
    return acc;
}

static unsigned long run(const unsigned long* table) { return ((run_function)jit)(0, program, table); }

static int check(const unsigned long* table, unsigned times) {
    const unsigned long want = expected(table);
    for (unsigned i = 0; i < times; ++i)
        if (run(table) != want) return 0;
    return 1;
}

static volatile int thread_failed;

static void* worker(void* arg) {
    const unsigned long want_a = expected(table_a), want_b = expected(table_b);
    for (unsigned i = 0; i < 40; ++i) {
        const int b = ((unsigned long)arg + i) % 3 == 0;
        if (run(b ? table_b : table_a) != (b ? want_b : want_a)) thread_failed = 1;
    }
    return 0;
}

int main(void) {
    jit = mmap(0, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit == MAP_FAILED) return 1;
    volatile unsigned* entry = (volatile unsigned*)jit;   /* the first dispatch */
    entry[0] = LDRB_NEXT;
    entry[1] = LDR_TARGET;
    entry[2] = MOV_X17;
    entry[3] = BR_X17;
    sys_icache_invalidate(jit, 16);
    volatile unsigned* halt = (volatile unsigned*)handler(HALT);
    halt[0] = RET;
    sys_icache_invalidate((void*)halt, 4);
    for (unsigned k = 0; k < OPS; ++k) put_handler(k, k + 1);
    for (unsigned k = 0; k < 16; ++k) {
        table_a[k] = handler(k);
        table_b[k] = k == HALT ? handler(HALT) : handler(OPS - 1 - k);
    }
    unsigned seed = 12345;
    for (unsigned i = 0; i < PROGRAM; ++i) {
        seed = seed * 1103515245u + 12345u;
        program[i] = (unsigned char)((seed >> 16) % OPS);
    }
    program[PROGRAM] = HALT;

    /* 1. One table, many runs. */
    if (!check(table_a, 50)) return 2;

    /* 2. Handlers rewritten in place. */
    for (unsigned i = 0; i < 6; ++i) {
        put_handler(3, 100 + i);
        put_handler(9, 200 + i);
        if (!check(table_a, 5)) return 3;
    }

    /* 3. A table entry pointed at another handler. */
    table_a[5] = handler(6);
    if (!check(table_a, 5)) return 4;
    table_a[5] = handler(5);
    if (!check(table_a, 5)) return 4;

    /* 4. Two tables alternating. */
    for (unsigned i = 0; i < 20; ++i)
        if (!check(table_a, 2) || !check(table_b, 3)) return 5;

    /* 5. Threads on both tables. */
    pthread_t threads[4];
    for (unsigned long t = 0; t < 4; ++t)
        if (pthread_create(&threads[t], 0, worker, (void*)t) != 0) return 6;
    for (unsigned t = 0; t < 4; ++t) pthread_join(threads[t], 0);
    if (thread_failed) return 6;

    puts("fx table shadow: ok");
    return 0;
}
