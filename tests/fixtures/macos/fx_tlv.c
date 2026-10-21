/* alchemy-fx engine fixture: Mach-O thread-local variables (TLVs) through
 * __tlv_bootstrap and its guest fast path (TPIDR_EL0 table). No SDK headers.
 * Usage: fx_tlv <fx_tlv_plugin.bundle>; prints "fx tlv: ok" and exits 0, or
 * prints the failing case and exits with its number.
 *  1 initial values on the main thread: __thread_data template, __thread_bss zero
 *  2 a TLV image dlopen'd after threads exist: fresh blocks on old and new threads
 *  3 two TLV images in one process (the executable and the plugin)
 *  4 isolation under contention: 8 threads write and read their own TLVs while yielding
 *  5 a thread that exits and a new thread: the new one starts from the template
 *  6 first access inside a callback a provider runs on a worker (pthread_once) */
typedef void* pthread_t;
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct { long sig; char opaque[40]; } pthread_cond_t;
typedef struct { long sig; char opaque[8]; } pthread_once_t;
extern int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern int pthread_mutex_lock(pthread_mutex_t*);
extern int pthread_mutex_unlock(pthread_mutex_t*);
extern int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern int pthread_cond_broadcast(pthread_cond_t*);
extern int pthread_once(pthread_once_t*, void (*)(void));
extern int sched_yield(void);
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int puts(const char*);

__thread long main_data = 0x1234;
__thread long main_bss[8];

static long (*plugin_get)(void);
static void (*plugin_set)(long);
static long (*plugin_bss)(void);
static long* (*plugin_address)(void);

static pthread_mutex_t mutex = {0x32aaaba7, {0}};
static pthread_cond_t condition = {0x3cb0b1bb, {0}};
static int waiting;
static int released;

static int fresh(void) {
    for (int i = 0; i < 8; ++i) if (main_bss[i] != 0) return 0;
    return main_data == 0x1234;
}

/* Case 2: created before the dlopen, first touches the plugin's TLVs after it. */
static void* early_worker(void* argument) {
    (void)argument;
    if (!fresh()) return (void*)21;
    main_data = 1;
    if (pthread_mutex_lock(&mutex)) return (void*)22;
    ++waiting;
    if (pthread_cond_broadcast(&condition)) return (void*)23;
    while (!released) if (pthread_cond_wait(&condition, &mutex)) return (void*)24;
    if (pthread_mutex_unlock(&mutex)) return (void*)25;
    if (plugin_get() != 77 || plugin_bss() != 0) return (void*)26;
    plugin_set(-5);
    if (plugin_get() != -5 || main_data != 1) return (void*)27;
    return (void*)0;
}

/* Case 4: each thread owns its values; a yield between write and read lets
 * other threads run (and their TLV accesses) in between. */
static void* contender(void* argument) {
    const long id = (long)argument;
    long* own = &main_data;
    long* own_plugin = plugin_address();
    for (long i = 0; i < 20000; ++i) {
        main_data = id * 1000000 + i;
        main_bss[i & 7] = id;
        plugin_set(id * 7 + i);
        if ((i & 255) == 0 && sched_yield()) return (void*)41;
        if (main_data != id * 1000000 + i || main_bss[i & 7] != id || plugin_get() != id * 7 + i) return (void*)42;
        if (&main_data != own || plugin_address() != own_plugin) return (void*)43;
    }
    return (void*)own;
}

static void* exiting_worker(void* argument) {
    (void)argument;
    if (!fresh()) return (void*)51;
    main_data = 999;
    main_bss[3] = 999;
    plugin_set(999);
    return (void*)0;
}
static void* next_worker(void* argument) {
    (void)argument;
    if (!fresh() || plugin_get() != 77 || plugin_bss() != 0) return (void*)52;
    return (void*)0;
}

static pthread_once_t once = {0x30b1bcba, {0}};
static int once_result = -1;
static void once_body(void) { once_result = fresh() && plugin_get() == 77 ? 0 : 61; }
static void* once_worker(void* argument) {
    (void)argument;
    if (pthread_once(&once, once_body)) return (void*)62;
    return (void*)(long)once_result;
}

static int run(void* (*body)(void*), void* argument, void** result) {
    pthread_t thread;
    if (pthread_create(&thread, 0, body, argument)) return 1;
    return pthread_join(thread, result) ? 1 : 0;
}

int main(int argc, char** argv) {
    void* result = 0;
    if (argc != 2) return 100;
    /* 1 */
    if (!fresh()) { puts("fx tlv: 1 main thread template"); return 1; }
    main_data = 5;
    /* 2: a thread that exists before the plugin is loaded. */
    pthread_t early;
    if (pthread_create(&early, 0, early_worker, 0)) return 101;
    if (pthread_mutex_lock(&mutex)) return 102;
    while (waiting != 1) if (pthread_cond_wait(&condition, &mutex)) return 103;
    void* plugin = dlopen(argv[1], 2);
    if (!plugin) { puts("fx tlv: 2 dlopen"); return 2; }
    plugin_get = (long (*)(void))dlsym(plugin, "fx_tlv_plugin_get");
    plugin_set = (void (*)(long))dlsym(plugin, "fx_tlv_plugin_set");
    plugin_bss = (long (*)(void))dlsym(plugin, "fx_tlv_plugin_bss");
    plugin_address = (long* (*)(void))dlsym(plugin, "fx_tlv_plugin_address");
    if (!plugin_get || !plugin_set || !plugin_bss || !plugin_address) { puts("fx tlv: 2 dlsym"); return 2; }
    released = 1;
    if (pthread_cond_broadcast(&condition) || pthread_mutex_unlock(&mutex)) return 104;
    if (pthread_join(early, &result) || result) { puts("fx tlv: 2 early thread"); return result ? (int)(long)result : 2; }
    /* 3: the main thread's own plugin block, independent of the executable's. */
    if (plugin_get() != 77 || plugin_bss() != 0 || main_data != 5) { puts("fx tlv: 3 two images"); return 3; }
    plugin_set(12);
    /* 4 */
    pthread_t contenders[8];
    for (long id = 0; id < 8; ++id) if (pthread_create(&contenders[id], 0, contender, (void*)(id + 1))) return 105;
    void* addresses[8];
    int failed = 0;
    for (int id = 0; id < 8; ++id) {
        if (pthread_join(contenders[id], &addresses[id])) return 106;
        if ((unsigned long)addresses[id] < 0x1000) failed = (int)(long)addresses[id];
    }
    for (int a = 0; a < 8 && !failed; ++a) for (int b = a + 1; b < 8; ++b) if (addresses[a] == addresses[b]) failed = 44;
    if (failed || main_data != 5 || plugin_get() != 12) { puts("fx tlv: 4 contention"); return failed ? failed : 4; }
    /* 5 */
    if (run(exiting_worker, 0, &result) || result) { puts("fx tlv: 5 exiting thread"); return result ? (int)(long)result : 5; }
    for (int round = 0; round < 4; ++round)
        if (run(next_worker, 0, &result) || result) { puts("fx tlv: 5 next thread"); return result ? (int)(long)result : 5; }
    /* 6 */
    if (run(once_worker, 0, &result) || result) { puts("fx tlv: 6 pthread_once"); return result ? (int)(long)result : 6; }
    if (main_data != 5 || plugin_get() != 12) { puts("fx tlv: main values changed"); return 7; }
    puts("fx tlv: ok");
    return 0;
}
