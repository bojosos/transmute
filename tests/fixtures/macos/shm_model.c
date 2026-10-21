/* Alchemy cross-process shared memory fixture (docs/ALCHEMY_PROCESS.md,
 * "Shared memory"). Project-authored, no SDK headers. One program plays both
 * roles:
 *   shm_model <test>                  a parent test; prints "ok <test>" or "FAIL <test>: ..."
 *   shm_model child <mode> <args...>  the posix_spawn'd second process
 * The exit status of a test is its failure count. The parent and the child
 * are separate Alchemy processes that map the same pages. */
typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned short mode_t;
typedef void* posix_spawn_file_actions_t;
typedef void* posix_spawnattr_t;
typedef int sem_t;
struct timespec { long tv_sec; long tv_nsec; };
/* Darwin arm64 struct stat: st_size at offset 96. */
struct stat_buffer { unsigned char bytes[144]; };
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct { long sig; char opaque[8]; } pthread_mutexattr_t;
typedef struct { long sig; char opaque[40]; } pthread_cond_t;
typedef struct { long sig; char opaque[8]; } pthread_condattr_t;

extern int* __error(void);
extern char** environ;
extern ssize_t write(int, const void*, size_t);
extern ssize_t read(int, void*, size_t);
extern off_t lseek(int, off_t, int);
extern int close(int);
extern int open(const char*, int, ...);
extern int unlink(const char*);
extern int mkdir(const char*, mode_t);
extern int fchmod(int, mode_t);
extern int fstat(int, struct stat_buffer*);
extern int ftruncate(int, off_t);
extern void* mmap(void*, size_t, int, int, int, off_t);
extern int munmap(void*, size_t);
extern int mprotect(void*, size_t, int);
extern int msync(void*, size_t, int);
extern int shm_open(const char*, int, ...);
extern int shm_unlink(const char*);
extern sem_t* sem_open(const char*, int, ...);
extern int sem_close(sem_t*);
extern int sem_unlink(const char*);
extern int sem_post(sem_t*);
extern int sem_wait(sem_t*);
extern int __ulock_wait(unsigned, void*, unsigned long, unsigned);
extern int __ulock_wake(unsigned, void*, unsigned long);
extern int pthread_mutexattr_init(pthread_mutexattr_t*);
extern int pthread_mutexattr_setpshared(pthread_mutexattr_t*, int);
extern int pthread_mutexattr_destroy(pthread_mutexattr_t*);
extern int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
extern int pthread_mutex_lock(pthread_mutex_t*);
extern int pthread_mutex_trylock(pthread_mutex_t*);
extern int pthread_mutex_unlock(pthread_mutex_t*);
extern int pthread_mutex_destroy(pthread_mutex_t*);
extern int pthread_condattr_init(pthread_condattr_t*);
extern int pthread_condattr_setpshared(pthread_condattr_t*, int);
extern int pthread_condattr_destroy(pthread_condattr_t*);
extern int pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
extern int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern int pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
extern int pthread_cond_signal(pthread_cond_t*);
extern int pthread_cond_broadcast(pthread_cond_t*);
extern int pthread_cond_destroy(pthread_cond_t*);
extern int gettimeofday(struct timespec*, void*);
extern pid_t waitpid(pid_t, int*, int);
extern pid_t getpid(void);
extern char* getenv(const char*);
extern int usleep(unsigned);
extern int posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[], char* const[]);

#define errno (*__error())
enum { O_RDONLY = 0, O_RDWR = 2, O_CREAT = 0x200, O_EXCL = 0x800 };
enum { PROT_NONE = 0, PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4 };
enum { MAP_SHARED = 1, MAP_PRIVATE = 2, MAP_FIXED = 0x10, MAP_ANON = 0x1000 };
enum { MS_ASYNC = 1, MS_SYNC = 0x10 };
enum { ENOENT = 2, EACCES = 13, EEXIST = 17, EINVAL = 22, EBUSY = 16, ETIMEDOUT = 60 };
enum { UL_COMPARE_AND_WAIT = 1, ULF_WAKE_ALL = 0x100, ULF_NO_ERRNO = 0x01000000 };
enum { PTHREAD_PROCESS_SHARED = 1 };
#define MAP_FAILED ((void*)-1)
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)

static const char* self;   /* argv[0]: this program's guest path */
static int failures;

static size_t length(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
static int same(const char* a, const char* b) { while (*a && *a == *b) ++a, ++b; return *a == *b; }
static void out(int fd, const char* s) { write(fd, s, length(s)); }
static char* number(char* buffer, long value) {
    char digits[24]; int n = 0; int negative = value < 0; unsigned long v = negative ? -(unsigned long)value : (unsigned long)value;
    do digits[n++] = (char)('0' + v % 10); while ((v /= 10));
    int i = 0; if (negative) buffer[i++] = '-';
    while (n) buffer[i++] = digits[--n];
    buffer[i] = 0; return buffer;
}
static long parse(const char* s) { long v = 0; int negative = *s == '-'; if (negative) ++s; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return negative ? -v : v; }
static void append(char* to, const char* from) { while (*to) ++to; while ((*to++ = *from++)) {} }
static void check(int condition, const char* test, const char* what, long value) {
    if (condition) return;
    char text[32];
    ++failures;
    out(1, "FAIL "); out(1, test); out(1, ": "); out(1, what); out(1, " ("); out(1, number(text, value)); out(1, ")\n");
}
static void done(const char* test, int before) { if (failures == before) { out(1, "ok "); out(1, test); out(1, "\n"); } }
static long file_size(int fd) { struct stat_buffer b; if (fstat(fd, &b)) return -1; return *(long*)(b.bytes + 96); }

/* A spin lock and counter in the shared page, as Boost.Interprocess builds
 * its process-shared mutexes on macOS (atomics plus yields). */
struct Shared {
    unsigned magic;
    unsigned lock;
    unsigned counter;
    unsigned turn;          /* ping-pong */
    unsigned word;          /* ulock */
    unsigned stage;
    unsigned pad[2];
    pthread_mutex_t mutex;  /* process-shared */
    pthread_cond_t cond;
    unsigned guarded;       /* changed under `mutex` */
    unsigned ready;
};
enum { MAGIC = 0x53484d31, ROUNDS = 20000, PINGS = 200 };

static void spin_lock(unsigned* lock) {
    unsigned spins = 0;
    for (;;) {
        unsigned expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (++spins % 64 == 0) usleep(0);
    }
}
static void spin_unlock(unsigned* lock) { __atomic_store_n(lock, 0, __ATOMIC_RELEASE); }
static void count(struct Shared* s) {
    for (int i = 0; i < ROUNDS; ++i) {
        spin_lock(&s->lock);
        unsigned v = s->counter;   /* a non-atomic read-modify-write under the lock */
        s->counter = v + 1;
        spin_unlock(&s->lock);
    }
}
static int wait_value(volatile unsigned* where, unsigned value, int milliseconds) {
    for (int waited = 0; *where != value; ++waited) { if (waited > milliseconds * 10) return 0; usleep(100); }
    return 1;
}

static pid_t spawn(const char* mode, const char* a, const char* b) {
    pid_t pid = 0;
    char* argv[] = {(char*)"shm-child", (char*)"child", (char*)mode, (char*)a, (char*)b, 0};
    if (posix_spawn(&pid, self, 0, 0, argv, environ) != 0) return -1;
    return pid;
}
static int reap(pid_t pid) { int status = -1; if (waitpid(pid, &status, 0) != pid) return -1; return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + status; }

/* ---- child modes ------------------------------------------------------------------------------ */
static int child(int argc, char** argv) {
    const char* mode = argv[2];
    const char* t = mode;
    if (argc < 4) return 100;
    int fd = same(mode, "filemap") ? open(argv[3], O_RDWR) : shm_open(argv[3], O_RDWR);
    check(fd >= 0, t, "child open", errno);
    if (fd < 0) return failures;
    long size = file_size(fd);
    check(size == 65536, t, "child fstat size", size);
    struct Shared* s = (struct Shared*)mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(s != MAP_FAILED, t, "child mmap", errno);
    if (s == MAP_FAILED) return failures;
    close(fd);   /* the mapping outlives the descriptor */
    check(s->magic == MAGIC, t, "child sees the parent's magic", (long)s->magic);
    if (same(mode, "filemap")) {
        sem_t* go = sem_open(argv[4], 0);
        check(go != (sem_t*)-1 && go != 0, t, "child sem_open", errno);
        __atomic_store_n(&s->stage, 1, __ATOMIC_RELEASE);
        if (go && go != (sem_t*)-1) { sem_wait(go); sem_close(go); }
        count(s);
        ((char*)s)[40000] = 'C';   /* parent reads it back through read(2) */
        __atomic_store_n(&s->stage, 2, __ATOMIC_RELEASE);
    } else if (same(mode, "shm")) {
        for (unsigned i = 0; i < PINGS; ++i) {
            check(wait_value(&s->turn, 2 * i + 1, 5000), t, "child waits for ping", (long)i);
            __atomic_store_n(&s->turn, 2 * i + 2, __ATOMIC_RELEASE);
        }
    } else if (same(mode, "ulock")) {
        /* Waits until the parent stores 1 and wakes it. */
        __atomic_store_n(&s->stage, 1, __ATOMIC_RELEASE);
        int woken = 0;
        while (__atomic_load_n(&s->word, __ATOMIC_ACQUIRE) == 0) {
            int r = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &s->word, 0, 10 * 1000 * 1000);
            check(r >= 0 || r == -ETIMEDOUT, t, "child __ulock_wait", r);
            if (r < 0) break;
            ++woken;
        }
        check(s->word == 1, t, "child sees the stored word", (long)s->word);
        /* Second round: the parent waits, the child wakes it. */
        __atomic_store_n(&s->word, 2, __ATOMIC_RELEASE);
        usleep(50000);
        __ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL | ULF_NO_ERRNO, &s->word, 0);
    } else if (same(mode, "pmutex")) {
        __atomic_store_n(&s->stage, 1, __ATOMIC_RELEASE);
        for (int i = 0; i < ROUNDS / 4; ++i) {
            check(pthread_mutex_lock(&s->mutex) == 0, t, "child lock", i);
            unsigned v = s->guarded; s->guarded = v + 1;
            pthread_mutex_unlock(&s->mutex);
        }
        /* Hand-off through the process-shared condition variable. */
        pthread_mutex_lock(&s->mutex);
        s->ready = 1;
        pthread_cond_broadcast(&s->cond);
        while (s->ready != 2) check(pthread_cond_wait(&s->cond, &s->mutex) == 0, t, "child cond_wait", 0);
        pthread_mutex_unlock(&s->mutex);
    } else return 100;
    munmap(s, (size_t)size);
    return failures;
}

/* ---- parent tests ----------------------------------------------------------------------------- */
static int create_file(const char* path, long size) {
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -1;
    fchmod(fd, 0644);
    if (ftruncate(fd, size)) { close(fd); return -2; }
    return fd;
}

/* Boost.Interprocess on macOS: a file under $TMPDIR/boost_interprocess,
 * ftruncate, mmap(MAP_SHARED) in both processes, a spin lock in the segment
 * and a named semaphore. */
static void test_filemap(void) {
    const int before = failures; const char* t = "filemap";
    char dir[512] = {0}, path[600] = {0}, name[64] = {0}, text[32];
    const char* tmp = getenv("TMPDIR");
    append(dir, tmp && *tmp ? tmp : "/tmp");
    append(dir, "/boost_interprocess");
    check(mkdir(dir, 0777) == 0 || errno == EEXIST, t, "mkdir", errno);
    append(path, dir); append(path, "/alchemy-shm-model-"); append(path, number(text, getpid()));
    append(name, "/ashm-"); append(name, number(text, getpid()));
    int fd = create_file(path, 65536);
    check(fd >= 0, t, "create + ftruncate", fd);
    if (fd < 0) { done(t, before); return; }
    check(file_size(fd) == 65536, t, "fstat after ftruncate", file_size(fd));
    struct Shared* s = (struct Shared*)mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(s != MAP_FAILED, t, "mmap MAP_SHARED", errno);
    if (s == MAP_FAILED) { close(fd); unlink(path); done(t, before); return; }
    check(((unsigned long)s & 0x3fff) == 0, t, "16 KiB aligned", (long)((unsigned long)s & 0x3fff));
    s->magic = MAGIC;
    sem_unlink(name);
    sem_t* go = sem_open(name, O_CREAT | O_EXCL, 0644, 0);
    check(go != (sem_t*)-1 && go != 0, t, "sem_open", errno);
    pid_t pid = spawn("filemap", path, name);
    check(pid > 0, t, "posix_spawn", pid);
    check(wait_value(&s->stage, 1, 60000), t, "child mapped the file", (long)s->stage);
    sem_post(go);
    count(s);   /* concurrently with the child */
    check(reap(pid) == 0, t, "child status", 0);
    check(s->stage == 2, t, "child finished", (long)s->stage);
    check(s->counter == 2 * ROUNDS, t, "both processes counted under one lock", (long)s->counter);
    /* Stores through the mapping reach the file. */
    check(msync(s, 65536, MS_SYNC) == 0, t, "msync", errno);
    char byte = 0;
    check(lseek(fd, 40000, 0) == 40000 && read(fd, &byte, 1) == 1 && byte == 'C', t, "read(2) sees the child's store", byte);
    /* A shared mapping of a read-only descriptor cannot be writable. */
    int ro = open(path, O_RDONLY);
    check(mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == MAP_FAILED && errno == EACCES, t, "PROT_WRITE on O_RDONLY", errno);
    void* view = mmap(0, 65536, PROT_READ, MAP_SHARED, ro, 0);
    check(view != MAP_FAILED && ((struct Shared*)view)->counter == 2 * ROUNDS, t, "read-only view", errno);
    check(view == MAP_FAILED || mprotect(view, 65536, PROT_READ | PROT_WRITE) == -1, t, "mprotect write on a read-only view", errno);
    if (view != MAP_FAILED) munmap(view, 65536);
    close(ro);
    check(mmap((void*)s, 65536, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED && errno == EINVAL, t, "MAP_FIXED", errno);
    /* Growing a mapped file works; shrinking it is refused. */
    check(ftruncate(fd, 131072) == 0 && file_size(fd) == 131072, t, "grow while mapped", errno);
    check(ftruncate(fd, 4096) == -1 && errno == EBUSY, t, "shrink while mapped", errno);
    /* Unlinked while mapped: the pages stay; the name is free again. */
    check(unlink(path) == 0, t, "unlink while mapped", errno);
    s->turn = 77;
    check(s->turn == 77, t, "mapping after unlink", 0);
    int again = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    check(again >= 0, t, "the name can be created again", errno);
    if (again >= 0) { close(again); unlink(path); }
    check(munmap(s, 65536) == 0, t, "munmap", errno);
    close(fd);
    sem_close(go); sem_unlink(name);
    /* A file whose size is not a page multiple (Boost's 8-byte endpoint region). */
    int small = create_file(path, 8);
    check(small >= 0, t, "small file", small);
    if (small >= 0) {
        char* bytes = (char*)mmap(0, 8, PROT_READ | PROT_WRITE, MAP_SHARED, small, 0);
        check(bytes != MAP_FAILED, t, "mmap of an 8-byte file", errno);
        if (bytes != MAP_FAILED) {
            check(bytes[0] == 0 && bytes[7] == 0, t, "zero-filled", bytes[0]);
            bytes[3] = 'Q';
            char back = 0;
            check(lseek(small, 3, 0) == 3 && read(small, &back, 1) == 1 && back == 'Q', t, "store reaches the small file", back);
            check(file_size(small) == 8, t, "size unchanged by the mapping", file_size(small));
            munmap(bytes, 8);
        }
        close(small); unlink(path);
    }
    done(t, before);
}

/* POSIX shared memory objects: shm_open + ftruncate + mmap, ping-pong. */
static void test_shm(void) {
    const int before = failures; const char* t = "shm";
    char name[64] = {0}, text[32];
    append(name, "/Alchemy.Shm-"); append(name, number(text, getpid()));
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    check(fd >= 0, t, "shm_open create", errno);
    if (fd < 0) { done(t, before); return; }
    check(shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST, t, "O_EXCL", errno);
    check(file_size(fd) == 0, t, "new object is empty", file_size(fd));
    check(ftruncate(fd, 65536) == 0, t, "ftruncate", errno);
    struct Shared* s = (struct Shared*)mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(s != MAP_FAILED, t, "mmap", errno);
    if (s == MAP_FAILED) { shm_unlink(name); done(t, before); return; }
    s->magic = MAGIC;
    pid_t pid = spawn("shm", name, "-");
    check(pid > 0, t, "posix_spawn", pid);
    for (unsigned i = 0; i < PINGS && failures == before; ++i) {
        __atomic_store_n(&s->turn, 2 * i + 1, __ATOMIC_RELEASE);
        check(wait_value(&s->turn, 2 * i + 2, 60000), t, "parent waits for pong", (long)i);
    }
    check(reap(pid) == 0, t, "child status", 0);
    check(s->turn == 2 * PINGS, t, "ping-pong rounds", (long)s->turn);
    munmap(s, 65536);
    close(fd);
    check(shm_unlink(name) == 0, t, "shm_unlink", errno);
    check(shm_open(name, O_RDWR) == -1 && errno == ENOENT, t, "gone after shm_unlink", errno);
    check(shm_unlink(name) == -1 && errno == ENOENT, t, "second shm_unlink", errno);
    char longname[64] = "/0123456789012345678901234567890123";
    check(shm_open(longname, O_RDWR | O_CREAT, 0600) == -1, t, "name longer than PSHMNAMLEN", errno);
    done(t, before);
}

static struct Shared* shared_object(const char* t, char* name, int* fd) {
    char text[32];
    name[0] = 0;
    append(name, "/ashm-"); append(name, t); append(name, "-"); append(name, number(text, getpid()));
    shm_unlink(name);
    *fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    check(*fd >= 0, t, "shm_open", errno);
    if (*fd < 0) return 0;
    ftruncate(*fd, 65536);
    struct Shared* s = (struct Shared*)mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    check(s != MAP_FAILED, t, "mmap", errno);
    if (s == MAP_FAILED) return 0;
    s->magic = MAGIC;
    return s;
}

/* __ulock_wait/__ulock_wake on a shared word, across processes. */
static void test_ulock(void) {
    const int before = failures; const char* t = "ulock";
    char name[64]; int fd = -1;
    struct Shared* s = shared_object(t, name, &fd);
    if (!s) { done(t, before); return; }
    pid_t pid = spawn("ulock", name, "-");
    check(wait_value(&s->stage, 1, 60000), t, "child started", (long)s->stage);
    if (s->stage != 1) { reap(pid); munmap(s, 65536); close(fd); shm_unlink(name); return; }
    usleep(100000);   /* the child is (normally) blocked in __ulock_wait now */
    __atomic_store_n(&s->word, 1, __ATOMIC_RELEASE);
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &s->word, 0);
    /* Then the parent blocks until the child stores 2 and wakes it. */
    while (__atomic_load_n(&s->word, __ATOMIC_ACQUIRE) == 1) {
        int r = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &s->word, 1, 10 * 1000 * 1000);
        check(r >= 0, t, "parent __ulock_wait", r);
        if (r < 0) break;
    }
    check(s->word == 2, t, "parent sees the child's word", (long)s->word);
    check(reap(pid) == 0, t, "child status", 0);
    munmap(s, 65536); close(fd); shm_unlink(name);
    done(t, before);
}

/* PTHREAD_PROCESS_SHARED mutex and condition variable in a shared segment. */
static void test_pmutex(void) {
    const int before = failures; const char* t = "pmutex";
    char name[64]; int fd = -1;
    struct Shared* s = shared_object(t, name, &fd);
    if (!s) { done(t, before); return; }
    pthread_mutexattr_t ma; pthread_condattr_t ca;
    pthread_mutexattr_init(&ma);
    check(pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) == 0, t, "mutexattr_setpshared", 0);
    check(pthread_mutex_init(&s->mutex, &ma) == 0, t, "mutex_init", 0);
    pthread_mutexattr_destroy(&ma);
    pthread_condattr_init(&ca);
    check(pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED) == 0, t, "condattr_setpshared", 0);
    check(pthread_cond_init(&s->cond, &ca) == 0, t, "cond_init", 0);
    pthread_condattr_destroy(&ca);
    /* Held here: the child's trylock-free lock blocks until the parent unlocks. */
    check(pthread_mutex_lock(&s->mutex) == 0, t, "lock before spawn", 0);
    pid_t pid = spawn("pmutex", name, "-");
    check(wait_value(&s->stage, 1, 60000), t, "child started", (long)s->stage);
    if (s->stage != 1) { pthread_mutex_unlock(&s->mutex); reap(pid); munmap(s, 65536); close(fd); shm_unlink(name); return; }
    usleep(50000);
    check(s->guarded == 0, t, "child blocked while the parent holds the mutex", (long)s->guarded);
    pthread_mutex_unlock(&s->mutex);
    for (int i = 0; i < ROUNDS / 4; ++i) {
        pthread_mutex_lock(&s->mutex);
        unsigned v = s->guarded; s->guarded = v + 1;
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_lock(&s->mutex);
    while (s->ready != 1) check(pthread_cond_wait(&s->cond, &s->mutex) == 0, t, "parent cond_wait", 0);
    check(s->guarded == ROUNDS / 2, t, "mutual exclusion across processes", (long)s->guarded);
    s->ready = 2;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    check(reap(pid) == 0, t, "child status", 0);
    /* A timed wait with nobody signalling times out. */
    struct timespec now; gettimeofday(&now, 0);   /* tv_nsec holds microseconds here */
    struct timespec deadline = {now.tv_sec + 1, 0};
    pthread_mutex_lock(&s->mutex);
    check(pthread_cond_timedwait(&s->cond, &s->mutex, &deadline) == ETIMEDOUT, t, "cond_timedwait times out", 0);
    check(pthread_mutex_trylock(&s->mutex) == EBUSY, t, "trylock of a held mutex", 0);
    pthread_mutex_unlock(&s->mutex);
    check(pthread_mutex_destroy(&s->mutex) == 0 && pthread_cond_destroy(&s->cond) == 0, t, "destroy", 0);
    munmap(s, 65536); close(fd); shm_unlink(name);
    done(t, before);
}

int main(int argc, char** argv) {
    self = argv[0];
    if (argc >= 3 && same(argv[1], "child")) return child(argc, argv);
    if (argc < 2) { out(2, "usage: shm_model <test> | child <mode> ...\n"); return 2; }
    const char* test = argv[1];
    if (same(test, "filemap")) test_filemap();
    else if (same(test, "shm")) test_shm();
    else if (same(test, "ulock")) test_ulock();
    else if (same(test, "pmutex")) test_pmutex();
    else { out(1, "FAIL unknown test\n"); return 2; }
    return failures;
}
