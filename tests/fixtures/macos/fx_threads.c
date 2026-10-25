/* alchemy-fx engine fixture: many guest threads, stack sizes, process exit
 * with CPU-bound and blocked workers, and fatal signals. No SDK headers.
 * Usage: fx_threads <mode> [count]; the exit status is the result. */
typedef unsigned long size_t;
typedef void* pthread_t;
typedef struct { long sig; char opaque[56]; } pthread_attr_t;
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct { long sig; char opaque[40]; } pthread_cond_t;
extern int pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern int pthread_attr_init(pthread_attr_t*);
extern int pthread_attr_destroy(pthread_attr_t*);
extern int pthread_attr_setstacksize(pthread_attr_t*, size_t);
extern int pthread_mutex_lock(pthread_mutex_t*);
extern int pthread_mutex_unlock(pthread_mutex_t*);
extern int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern void* dispatch_semaphore_create(long);
extern long dispatch_semaphore_wait(void*, unsigned long long);
extern int puts(const char*);
extern int usleep(unsigned);
extern void exit(int) __attribute__((noreturn));
extern void abort(void) __attribute__((noreturn));

static pthread_mutex_t mutex = {0x32aaaba7, {0}};
static pthread_mutex_t held = {0x32aaaba7, {0}};
static pthread_cond_t never = {0x3cb0b1bb, {0}};
static unsigned long total;
static unsigned long atomic_total;
static volatile int started;
static volatile int spin_forever = 1;

static int same(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

static unsigned number(const char* text) {
    unsigned value = 0;
    while (*text >= '0' && *text <= '9') value = value * 10 + (unsigned)(*text++ - '0');
    return value;
}

static void print_count(const char* prefix, unsigned value, const char* suffix) {
    char line[96];
    char digits[12];
    unsigned n = 0, i = 0;
    do digits[n++] = (char)('0' + value % 10); while ((value /= 10) && n < sizeof digits);
    while (*prefix && i < 40) line[i++] = *prefix++;
    while (n) line[i++] = digits[--n];
    while (*suffix && i < 94) line[i++] = *suffix++;
    line[i] = 0;
    puts(line);
}

static unsigned long work(unsigned long seed, unsigned iterations) {
    for (unsigned i = 0; i < iterations; ++i) seed = seed * 6364136223846793005UL + 1442695040888963407UL;
    return seed >> 33;
}

/* Touches `depth` 4 KiB frames of guest stack. */
__attribute__((noinline)) static unsigned long deep(unsigned depth, unsigned long seed) {
    volatile unsigned char frame[4096];
    frame[0] = (unsigned char)seed;
    frame[4095] = (unsigned char)(seed >> 8);
    if (depth == 0) return seed & 0xff;
    return deep(depth - 1, seed * 6364136223846793005UL + 1) + frame[0] + frame[4095];
}

struct Job { unsigned index; unsigned depth; unsigned long result; pthread_t thread; };

static void* child(void* argument) {
    struct Job* job = argument;
    return (void*)(work(job->index + 7, 1000) | 1);
}

static void* stress_worker(void* argument) {
    struct Job* job = argument;
    unsigned long value = work(job->index, 20000) + deep(job->depth, job->index);
    if (job->index % 8 == 3) {   /* threads created from workers */
        pthread_t nested;
        void* nested_result = 0;
        if (pthread_create(&nested, 0, child, job) || pthread_join(nested, &nested_result) || !nested_result)
            return (void*)1;
    }
    pthread_mutex_lock(&mutex);
    total += value;
    pthread_mutex_unlock(&mutex);
    __atomic_fetch_add(&atomic_total, value, __ATOMIC_SEQ_CST);
    job->result = value;
    return 0;
}

static int stress(unsigned count) {
    static struct Job jobs[256];
    /* 0: Darwin default (512 KiB), then 256 KiB, 4 MiB and 16 MiB stacks. */
    static const size_t sizes[4] = {0, 256UL << 10, 4UL << 20, 16UL << 20};
    static const unsigned depths[4] = {64, 32, 768, 3072};
    if (count == 0 || count > 256) count = 64;
    for (unsigned i = 0; i < count; ++i) {
        pthread_attr_t attribute;
        jobs[i].index = i;
        jobs[i].depth = depths[i % 4];
        if (pthread_attr_init(&attribute)) return 10;
        if (sizes[i % 4] && pthread_attr_setstacksize(&attribute, sizes[i % 4])) return 11;
        if (pthread_create(&jobs[i].thread, &attribute, stress_worker, &jobs[i])) return 12;
        pthread_attr_destroy(&attribute);
    }
    unsigned long expected = 0;
    for (unsigned i = 0; i < count; ++i) {
        void* result = (void*)1;
        if (pthread_join(jobs[i].thread, &result) || result) return 13;
        const unsigned long value = work(i, 20000) + deep(jobs[i].depth, i);   /* main stack: 8 MiB */
        if (jobs[i].result != value) return 15;
        expected += value;
    }
    if (total != expected) return 14;
    if (atomic_total != expected) return 16;
    print_count("fx threads: ", count, " workers ok");
    return 0;
}

static void* spinner(void* argument) {
    volatile unsigned long counter = (unsigned long)argument;
    __atomic_fetch_add((int*)&started, 1, __ATOMIC_SEQ_CST);
    while (spin_forever) ++counter;
    return (void*)counter;
}

static void* locker(void* argument) {
    (void)argument;
    __atomic_fetch_add((int*)&started, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&held);   /* main holds it forever */
    return 0;
}

static void* waiter(void* argument) {
    (void)argument;
    pthread_mutex_lock(&mutex);
    __atomic_fetch_add((int*)&started, 1, __ATOMIC_SEQ_CST);
    for (;;) pthread_cond_wait(&never, &mutex);
    return 0;
}

static void* semaphore_waiter(void* argument) {
    __atomic_fetch_add((int*)&started, 1, __ATOMIC_SEQ_CST);
    dispatch_semaphore_wait(argument, ~0ULL);
    return 0;
}

static void* exiter(void* argument) {
    while (started < (int)(unsigned long)argument) usleep(1000);
    usleep(20000);
    exit(5);
}

static void* faulter(void* argument) {
    (void)argument;
    *(volatile int*)16 = 1;
    return 0;
}

static unsigned long atomic_counter;
static void* incrementer(void* argument) {
    for (unsigned long i = 0; i < (unsigned long)argument; ++i) __atomic_fetch_add(&atomic_counter, 1, __ATOMIC_SEQ_CST);
    return 0;
}

/* LSE atomics under contention: count threads x 100000 increments. */
static int atomics(unsigned count) {
    pthread_t threads[64];
    if (count == 0 || count > 64) count = 16;
    for (unsigned i = 0; i < count; ++i) if (pthread_create(&threads[i], 0, incrementer, (void*)100000UL)) return 50;
    for (unsigned i = 0; i < count; ++i) if (pthread_join(threads[i], 0)) return 51;
    if (atomic_counter != 100000UL * count) { print_count("fx threads: atomic total ", (unsigned)atomic_counter, " wrong"); return 52; }
    print_count("fx threads: atomics ", count, " threads ok");
    return 0;
}

/* CPU-bound and blocked workers that never finish by themselves. */
static int start_busy(void) {
    pthread_t thread;
    pthread_mutex_lock(&held);
    for (unsigned long i = 0; i < 8; ++i) if (pthread_create(&thread, 0, spinner, (void*)i)) return 20;
    for (unsigned i = 0; i < 2; ++i) if (pthread_create(&thread, 0, locker, 0)) return 21;
    for (unsigned i = 0; i < 2; ++i) if (pthread_create(&thread, 0, waiter, 0)) return 22;
    if (pthread_create(&thread, 0, semaphore_waiter, dispatch_semaphore_create(0))) return 23;
    while (started < 13) usleep(1000);
    usleep(20000);
    return 0;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "stress";
    if (same(mode, "stress")) return stress(argc > 2 ? number(argv[2]) : 64);
    if (same(mode, "atomics")) return atomics(argc > 2 ? number(argv[2]) : 16);
    if (same(mode, "exit-busy")) {
        const int failed = start_busy();
        if (failed) return failed;
        puts("fx threads: exit with busy workers");
        exit(7);
    }
    if (same(mode, "return-busy")) {
        const int failed = start_busy();
        if (failed) return failed;
        puts("fx threads: return with busy workers");
        return 9;
    }
    if (same(mode, "worker-exit")) {
        pthread_t thread;
        for (unsigned long i = 0; i < 4; ++i) if (pthread_create(&thread, 0, spinner, (void*)i)) return 30;
        if (pthread_create(&thread, 0, exiter, (void*)4UL)) return 31;
        volatile unsigned long counter = 0;
        while (spin_forever) ++counter;   /* main stays CPU-bound in guest code */
        return 32;
    }
    if (same(mode, "fault")) { *(volatile int*)16 = 1; return 40; }
    if (same(mode, "worker-fault")) {
        pthread_t thread;
        void* result;
        if (pthread_create(&thread, 0, faulter, 0)) return 41;
        pthread_join(thread, &result);
        return 42;
    }
    if (same(mode, "wait")) {
        /* A worker blocked in pthread_cond_wait, main in usleep for count / 5
           seconds (ALCHEMY_HANG_DUMP prints both). */
        pthread_t thread;
        if (pthread_create(&thread, 0, waiter, 0)) return 50;
        const unsigned ticks = argc > 2 ? number(argv[2]) : 40;
        for (unsigned i = 0; i < ticks; ++i) usleep(200000);
        puts("fx threads: wait done");
        exit(0);
    }
    if (same(mode, "trap")) __builtin_trap();
    if (same(mode, "abort")) abort();
    return 2;
}
