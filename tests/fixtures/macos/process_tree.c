/* Chrome support, libSystem process level (c2, docs/ALCHEMY_CHROME_AUDIT.md
 * stage 5). Project-authored, no SDK headers. One program plays every role:
 *   process_tree <test>              a parent test; prints "ok <test>" or "FAIL <test>: ..."
 *   process_tree child <mode> ...    a helper process
 * Tests: identity (pids across posix_spawn, proc_pidinfo, sysctl KERN_PROC_PID
 * and proc_pidpath of other processes), watch (a dispatch PROC exit source
 * on a process that is not a child), locks (fcntl record locks and flock
 * between processes), dev (devfs nodes). The exit status is the failure count. */
typedef unsigned long size_t;
typedef long ssize_t;
typedef int pid_t;
typedef long long off_t;
typedef unsigned long long uint64_t;
typedef void* posix_spawn_file_actions_t;

struct flock { off_t l_start; off_t l_len; pid_t l_pid; short l_type; short l_whence; };

extern int* __error(void);
extern char** environ;
extern ssize_t write(int, const void*, size_t);
extern ssize_t read(int, void*, size_t);
extern int close(int);
extern int pipe(int[2]);
extern int dup(int);
extern int fcntl(int, int, ...);
extern int open(const char*, int, ...);
extern int unlink(const char*);
extern int flock(int, int);
extern void _exit(int) __attribute__((noreturn));
extern pid_t waitpid(pid_t, int*, int);
extern int kill(pid_t, int);
extern pid_t getpid(void);
extern pid_t getppid(void);
extern unsigned getuid(void);
extern char* getenv(const char*);
extern int usleep(unsigned);
extern int stat(const char*, void*);
extern int lstat(const char*, void*);
extern int access(const char*, int);
extern int sysctl(int*, unsigned, void*, size_t*, void*, size_t);
extern int proc_pidinfo(int, int, uint64_t, void*, int);
extern int proc_pidpath(int, void*, unsigned);
extern int posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*, const void*, char* const[], char* const[]);
extern int posix_spawn_file_actions_init(posix_spawn_file_actions_t*);
extern int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*);
extern int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int, int);
extern int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int);
/* libdispatch (function-pointer variants: no blocks) */
typedef void* dispatch_object_t;
extern const char _dispatch_source_type_proc;
extern void* dispatch_get_global_queue(long, unsigned long);
extern void* dispatch_source_create(const void*, unsigned long, unsigned long, void*);
extern void dispatch_set_context(void*, void*);
extern void dispatch_source_set_event_handler_f(void*, void (*)(void*));
extern void dispatch_resume(void*);
extern void dispatch_source_cancel(void*);
extern unsigned long dispatch_source_get_data(void*);
extern void* dispatch_semaphore_create(long);
extern long dispatch_semaphore_wait(void*, uint64_t);
extern long dispatch_semaphore_signal(void*);
extern uint64_t dispatch_time(uint64_t, long long);

#define errno (*__error())
#define F_GETLK 7
#define F_SETLK 8
#define F_SETLKW 9
#define F_RDLCK 1
#define F_UNLCK 2
#define F_WRLCK 3
#define O_RDWR 2
#define O_CREAT 0x200
#define O_TRUNC 0x400

static const char* self;
static int failures;

static size_t length(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
static void out(int fd, const char* s) { write(fd, s, length(s)); }
static int same(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static int starts(const char* s, const char* p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static long parse(const char* s) { long v = 0; int neg = *s == '-'; if (neg) ++s; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }
static char* number(char* buffer, long value) {
    char tmp[24]; int n = 0; unsigned long v = value < 0 ? (unsigned long)-value : (unsigned long)value;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    int i = 0; if (value < 0) buffer[i++] = '-';
    while (n) buffer[i++] = tmp[--n];
    buffer[i] = 0; return buffer;
}
static void check(int ok, const char* test, const char* what, long value) {
    if (ok) return;
    char text[24];
    ++failures;
    out(1, "FAIL "); out(1, test); out(1, ": "); out(1, what); out(1, " ("); out(1, number(text, value)); out(1, ")\n");
}
static void done(const char* test, int before) { if (failures == before) { out(1, "ok "); out(1, test); out(1, "\n"); } }
static unsigned u32(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned u16(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static void copy(char* target, const char* source) { while ((*target++ = *source++)) {} }
static void append(char* target, const char* source) { copy(target + length(target), source); }

/* Reads one line (without the newline) from `fd`. */
static int read_line(int fd, char* buffer, int limit) {
    int total = 0;
    while (total < limit - 1) { char c; if (read(fd, &c, 1) != 1) break; if (c == '\n') break; buffer[total++] = c; }
    buffer[total] = 0; return total;
}
/* "key=value" in a line of space-separated fields. */
static long field(const char* line, const char* key) {
    const size_t n = length(key);
    for (const char* p = line; *p; ++p) {
        if ((p == line || p[-1] == ' ') && starts(p, key) && p[n] == '=') return parse(p + n + 1);
    }
    return -999999;
}

/* Spawns `self child <mode> args...` with stdin from `input` (or -1) and stdout to `output` (or -1). */
static pid_t spawn(const char* mode, const char* a, const char* b, int input, int output, int other1, int other2) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (input >= 0) posix_spawn_file_actions_adddup2(&actions, input, 0);
    if (output >= 0) posix_spawn_file_actions_adddup2(&actions, output, 1);
    if (other1 >= 0) posix_spawn_file_actions_addclose(&actions, other1);
    if (other2 >= 0) posix_spawn_file_actions_addclose(&actions, other2);
    char* argv[] = {(char*)self, (char*)"child", (char*)mode, (char*)a, (char*)b, 0};
    if (!a) argv[3] = 0; else if (!b) argv[4] = 0;
    pid_t pid = -1;
    const int error = posix_spawn(&pid, self, &actions, 0, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return error ? -error : pid;
}

static int kinfo(pid_t pid, unsigned char* buffer, size_t* size) {
    int mib[4] = {1, 14, 1, pid};   /* CTL_KERN, KERN_PROC, KERN_PROC_PID */
    *size = 648;
    return sysctl(mib, 4, buffer, size, 0, 0);
}

static int set_lock(int fd, int command, short type, off_t start, off_t len) {
    struct flock lock = {start, len, 0, type, 0};
    return fcntl(fd, command, &lock);
}
static struct flock get_lock(int fd, short type, off_t start, off_t len) {
    struct flock lock = {start, len, -1, type, 0};
    if (fcntl(fd, F_GETLK, &lock) != 0) lock.l_type = -1;
    return lock;
}

/* ---- helper processes ------------------------------------------------------------------------- */
static int child(int argc, char** argv) {
    char text[512], digits[24];
    const char* mode = argv[2];
    if (same(mode, "ident")) {
        /* Our identity as we see it, and our parent's as proc_pidinfo/sysctl report it. */
        unsigned char bsd[136], info[648]; size_t size = 0;
        text[0] = 0;
        append(text, "pid="); append(text, number(digits, getpid()));
        append(text, " ppid="); append(text, number(digits, getppid()));
        const int got = proc_pidinfo(getppid(), 3, 0, bsd, sizeof bsd);
        append(text, " parent_bsd_pid="); append(text, number(digits, got == 136 ? (long)u32(bsd + 12) : -1));
        append(text, " own_kinfo_ppid="); append(text, number(digits, kinfo(getpid(), info, &size) == 0 && size == 648 ? (long)u32(info + 560) : -1));
        append(text, "\n");
        out(1, text);
        while (read(0, text, sizeof text) > 0) {}   /* until the parent closes our input */
        return 0;
    }
    if (same(mode, "sleep")) { long ms = parse(argv[3]); while (ms > 0) { usleep(10000); ms -= 10; } return 0; }
    if (same(mode, "watch")) {
        /* A dispatch PROC exit source on a sibling (not our child). */
        const pid_t target = (pid_t)parse(argv[3]);
        unsigned char bsd[136];
        const int alive = kill(target, 0) == 0;
        const int info = proc_pidinfo(target, 3, 0, bsd, sizeof bsd) == 136 && (pid_t)u32(bsd + 16) == getppid();
        void* semaphore = dispatch_semaphore_create(0);
        void* source = dispatch_source_create(&_dispatch_source_type_proc, (unsigned long)target, 0x80000000UL, dispatch_get_global_queue(0, 0));
        if (!source) { out(1, "nosource\n"); return 1; }
        dispatch_set_context(source, semaphore);
        dispatch_source_set_event_handler_f(source, (void (*)(void*))dispatch_semaphore_signal);
        dispatch_resume(source);
        text[0] = 0;
        append(text, "armed alive="); append(text, number(digits, alive));
        append(text, " info="); append(text, number(digits, info)); append(text, "\n");
        out(1, text);
        const long waited = dispatch_semaphore_wait(semaphore, dispatch_time(0, 30000000000LL));
        out(1, waited == 0 ? "exited\n" : "timeout\n");
        out(1, kill(target, 0) == -1 && errno == 3 ? "gone\n" : "still\n");
        return 0;
    }
    if (same(mode, "locks")) {
        const char* path = argv[3];
        const pid_t parent = getppid();
        const int fd = open(path, O_RDWR);
        if (fd < 0) { out(1, "FAIL open\n"); return 1; }
        struct flock seen = get_lock(fd, F_WRLCK, 0, 10);
        int a = seen.l_type == F_WRLCK && seen.l_pid == parent && seen.l_start == 0 && seen.l_len == 40 && seen.l_whence == 0;
        seen = get_lock(fd, F_RDLCK, 45, 10);
        int b = seen.l_type == F_UNLCK;
        int c = set_lock(fd, F_SETLK, F_WRLCK, 0, 10) == -1 && errno == 35;
        int d = set_lock(fd, F_SETLK, F_WRLCK, 45, 10) == 0;
        int e = set_lock(fd, F_SETLK, F_RDLCK, 70, 5) == -1 && errno == 35;   /* the parent's [60,100) write lock */
        int f = flock(fd, 2 | 4) == -1 && errno == 35;                      /* LOCK_EX|LOCK_NB: the parent holds LOCK_EX */
        text[0] = 0;
        append(text, "phase1 a="); append(text, number(digits, a)); append(text, " b="); append(text, number(digits, b));
        append(text, " c="); append(text, number(digits, c)); append(text, " d="); append(text, number(digits, d));
        append(text, " e="); append(text, number(digits, e)); append(text, " f="); append(text, number(digits, f)); append(text, "\n");
        out(1, text);
        /* Phase 2: the parent closes a duplicate of its descriptor (dropping its
           record locks) and its flock, then writes a line. */
        read_line(0, text, sizeof text);
        int g = set_lock(fd, F_SETLKW, F_WRLCK, 0, 10) == 0;
        int h = flock(fd, 2 | 4) == 0;
        text[0] = 0;
        append(text, "phase2 g="); append(text, number(digits, g)); append(text, " h="); append(text, number(digits, h)); append(text, "\n");
        out(1, text);
        read_line(0, text, sizeof text);   /* hold the locks until told to end */
        return 0;
    }
    return 100;
}

/* ---- tests ------------------------------------------------------------------------------------ */
static void test_identity(void) {
    const int before = failures; const char* t = "identity";
    int in[2], outp[2]; char line[256];
    pipe(in); pipe(outp);
    const pid_t pid = spawn("ident", 0, 0, in[0], outp[1], in[1], outp[0]);
    close(in[0]); close(outp[1]);
    check(pid > 0, t, "posix_spawn", pid);
    read_line(outp[0], line, sizeof line);
    check(field(line, "pid") == pid, t, "child getpid == spawned pid", field(line, "pid"));
    check(field(line, "ppid") == getpid(), t, "child getppid == parent getpid", field(line, "ppid"));
    check(field(line, "parent_bsd_pid") == getpid(), t, "child proc_pidinfo(getppid) pbi_pid", field(line, "parent_bsd_pid"));
    check(field(line, "own_kinfo_ppid") == getpid(), t, "child sysctl KERN_PROC_PID e_ppid", field(line, "own_kinfo_ppid"));
    /* The parent's view of the running child. */
    unsigned char bsd[136], shortinfo[64], task[96], info[648]; size_t size = 0;
    check(proc_pidinfo(pid, 3, 0, bsd, sizeof bsd) == 136, t, "proc_pidinfo PROC_PIDTBSDINFO", errno);
    check(u32(bsd + 12) == (unsigned)pid && u32(bsd + 16) == (unsigned)getpid(), t, "pbi_pid/pbi_ppid", (long)u32(bsd + 16));
    check(u32(bsd + 20) == getuid(), t, "pbi_uid", (long)u32(bsd + 20));
    check(starts((const char*)bsd + 48, "process_tree"), t, "pbi_comm", bsd[48]);
    check(u32(bsd + 120) > 1600000000U, t, "pbi_start_tvsec", (long)u32(bsd + 120));
    check(proc_pidinfo(pid, 13, 0, shortinfo, sizeof shortinfo) == 64 && u32(shortinfo) == (unsigned)pid && u32(shortinfo + 4) == (unsigned)getpid(),
          t, "proc_pidinfo PROC_PIDT_SHORTBSDINFO", (long)u32(shortinfo + 4));
    check(proc_pidinfo(pid, 4, 0, task, sizeof task) == 96 && u32(task + 8) != 0, t, "proc_pidinfo PROC_PIDTASKINFO resident size", errno);
    check(kinfo(pid, info, &size) == 0 && size == 648, t, "sysctl KERN_PROC_PID size", (long)size);
    check(u32(info + 40) == (unsigned)pid && u32(info + 560) == (unsigned)getpid() && starts((const char*)info + 243, "process_tree"),
          t, "kinfo_proc p_pid/e_ppid/p_comm", (long)u32(info + 560));
    char own[1024], other[1024];
    check(proc_pidpath(getpid(), own, sizeof own) > 0 && proc_pidpath(pid, other, sizeof other) > 0 && same(own, other), t, "proc_pidpath of the child", errno);
    check(kill(pid, 0) == 0, t, "kill(child, 0)", errno);
    close(in[1]);   /* the child ends */
    int status = -1;
    check(waitpid(pid, &status, 0) == pid && status == 0, t, "child status", status);
    check(proc_pidinfo(pid, 3, 0, bsd, sizeof bsd) == 0 && errno == 3, t, "proc_pidinfo of a reaped child: ESRCH", errno);
    check(kinfo(pid, info, &size) == 0 && size == 0, t, "sysctl of a reaped child: nothing", (long)size);
    close(outp[0]);
    done(t, before);
}

static void test_watch(void) {
    const int before = failures; const char* t = "watch";
    char digits[24], line[256];
    const pid_t sleeper = spawn("sleep", "60000", 0, -1, -1, -1, -1);
    check(sleeper > 0, t, "spawn sleeper", sleeper);
    int outp[2]; pipe(outp);
    const pid_t watcher = spawn("watch", number(digits, sleeper), 0, -1, outp[1], outp[0], -1);
    close(outp[1]);
    check(watcher > 0, t, "spawn watcher", watcher);
    read_line(outp[0], line, sizeof line);
    check(starts(line, "armed"), t, "watcher armed its source", line[0]);
    check(field(line, "alive") == 1, t, "watcher: kill(sibling, 0)", field(line, "alive"));
    check(field(line, "info") == 1, t, "watcher: proc_pidinfo(sibling) pbi_ppid", field(line, "info"));
    usleep(200000);
    check(kill(sleeper, 9) == 0, t, "kill sleeper", errno);
    int status = -1;
    check(waitpid(sleeper, &status, 0) == sleeper && (status & 0x7f) == 9, t, "sleeper killed by SIGKILL", status);
    read_line(outp[0], line, sizeof line);
    check(same(line, "exited"), t, "watcher saw DISPATCH_PROC_EXIT of its sibling", line[0]);
    read_line(outp[0], line, sizeof line);
    check(same(line, "gone"), t, "watcher: kill(dead sibling, 0) ESRCH", line[0]);
    check(waitpid(watcher, &status, 0) == watcher && status == 0, t, "watcher status", status);
    close(outp[0]);
    done(t, before);
}

static void test_locks(void) {
    const int before = failures; const char* t = "locks";
    char path[1024], digits[24], line[256];
    const char* directory = getenv("TMPDIR");
    copy(path, directory ? directory : "/tmp"); if (path[length(path) - 1] != '/') append(path, "/");
    append(path, "c2-locks-"); append(path, number(digits, getpid()));
    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, t, "open", errno);
    /* Own locks: write [0,100), unlock [40,60) -> [0,40) and [60,100). */
    check(set_lock(fd, F_SETLK, F_WRLCK, 0, 100) == 0, t, "F_SETLK write", errno);
    check(set_lock(fd, F_SETLK, F_UNLCK, 40, 20) == 0, t, "F_SETLK unlock middle", errno);
    check(set_lock(fd, F_SETLK, F_RDLCK, 0, 10) == 0, t, "own lock replaced (write -> read)", errno);
    check(set_lock(fd, F_SETLK, F_WRLCK, 0, 10) == 0, t, "own lock replaced (read -> write)", errno);
    struct flock own = get_lock(fd, F_WRLCK, 0, 0);
    check(own.l_type == F_UNLCK, t, "F_GETLK ignores own locks", own.l_type);
    check(flock(fd, 2 | 4) == 0, t, "flock LOCK_EX|LOCK_NB", errno);
    struct flock bad = {0, 10, 0, 7, 0};
    check(fcntl(fd, F_SETLK, &bad) == -1 && errno == 22, t, "bad l_type EINVAL", errno);
    int in[2], outp[2]; pipe(in); pipe(outp);
    const pid_t pid = spawn("locks", path, 0, in[0], outp[1], in[1], outp[0]);
    close(in[0]); close(outp[1]);
    read_line(outp[0], line, sizeof line);
    check(field(line, "a") == 1, t, "child F_GETLK sees the parent's write lock (pid, range)", field(line, "a"));
    check(field(line, "b") == 1, t, "child F_GETLK finds the unlocked gap", field(line, "b"));
    check(field(line, "c") == 1, t, "child F_SETLK on a locked range: EAGAIN", field(line, "c"));
    check(field(line, "d") == 1, t, "child F_SETLK in the gap", field(line, "d"));
    check(field(line, "e") == 1, t, "child read lock vs the parent's write lock: EAGAIN", field(line, "e"));
    check(field(line, "f") == 1, t, "child flock LOCK_NB vs the parent's LOCK_EX: EWOULDBLOCK", field(line, "f"));
    /* The child's lock in the gap conflicts with us now. */
    struct flock theirs = get_lock(fd, F_WRLCK, 40, 20);
    check(theirs.l_type == F_WRLCK && theirs.l_pid == pid && theirs.l_start == 45 && theirs.l_len == 10, t, "parent F_GETLK sees the child's lock", theirs.l_pid);
    /* Closing any descriptor of the file drops all our record locks. */
    const int other = dup(fd);
    close(other);
    flock(fd, 8);   /* LOCK_UN */
    out(in[1], "go\n");
    read_line(outp[0], line, sizeof line);
    check(field(line, "g") == 1, t, "child F_SETLKW after the parent's close", field(line, "g"));
    check(field(line, "h") == 1, t, "child flock after the parent's LOCK_UN", field(line, "h"));
    check(set_lock(fd, F_SETLK, F_RDLCK, 0, 5) == -1 && errno == 35, t, "parent blocked by the child's lock", errno);
    out(in[1], "end\n");   /* the child ends; its locks go with it */
    check(set_lock(fd, F_SETLKW, F_WRLCK, 0, 0) == 0, t, "F_SETLKW (whole file) after the child ended", errno);
    int status = -1;
    check(waitpid(pid, &status, 0) == pid && status == 0, t, "child status", status);
    close(in[1]); close(outp[0]);
    close(fd);
    unlink(path);
    done(t, before);
}

static void test_dev(void) {
    const int before = failures; const char* t = "dev";
    unsigned char st[144];
    check(lstat("/dev/console", st) == 0, t, "lstat /dev/console", errno);
    check(u16(st + 4) == 020600 && u32(st + 16) == getuid(), t, "/dev/console crw------- owned by the console user", (long)u16(st + 4));
    check(stat("/dev/tty", st) == 0 && u16(st + 4) == 020666 && u32(st + 16) == 0 && u32(st + 24) == (2U << 24), t, "/dev/tty crw-rw-rw- root 2,0", (long)u16(st + 4));
    check(stat("/dev/null", st) == 0 && u16(st + 4) == 020666 && u32(st + 16) == 0 && u32(st + 24) == ((3U << 24) | 2U), t, "/dev/null crw-rw-rw- root 3,2", (long)u16(st + 4));
    check(lstat("/dev/null", st) == 0 && u16(st + 4) == 020666, t, "lstat /dev/null", (long)u16(st + 4));
    check(stat("/dev/urandom", st) == 0 && (u16(st + 4) & 0170000) == 020000 && u32(st + 24) == ((14U << 24) | 1U), t, "/dev/urandom character device 14,1", (long)u16(st + 4));
    check(stat("/dev/random", st) == 0 && u32(st + 24) == (14U << 24), t, "/dev/random 14,0", (long)u32(st + 24));
    const int null_fd = open("/dev/null", O_RDWR);
    check(null_fd >= 0, t, "open /dev/null", errno);
    unsigned char fst[144];
    check(fcntl(null_fd, 1) >= 0 && stat("/dev/null", st) == 0, t, "stat again", errno);
    close(null_fd);
    check(access("/dev/console", 6) == 0, t, "access /dev/console R|W", errno);
    check(access("/dev/tty", 6) == 0, t, "access /dev/tty R|W", errno);
    check(open("/dev/tty", O_RDWR) == -1 && errno == 6, t, "open /dev/tty: ENXIO", errno);
    const int random_fd = open("/dev/random", 0);
    check(random_fd >= 0 && read(random_fd, fst, 16) == 16, t, "read /dev/random", errno);
    if (random_fd >= 0) close(random_fd);
    done(t, before);
}

/* launchctl asuser <uid> <program>: the updater starts its net worker this way. */
static void test_launchctl(void) {
    const int before = failures; const char* t = "launchctl";
    char uid[24], other[24], line[256];
    number(uid, (long)getuid()); number(other, (long)getuid() + 1);
    int in[2], outp[2]; pipe(in); pipe(outp);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, outp[1], 1);
    posix_spawn_file_actions_addclose(&actions, in[1]);
    posix_spawn_file_actions_addclose(&actions, outp[0]);
    char* argv[] = {(char*)"launchctl", (char*)"asuser", uid, (char*)self, (char*)"child", (char*)"ident", 0};
    pid_t pid = -1;
    const int error = posix_spawn(&pid, "/bin/launchctl", &actions, 0, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]); close(outp[1]);
    check(error == 0 && pid > 0, t, "posix_spawn /bin/launchctl asuser <own uid>", error);
    read_line(outp[0], line, sizeof line);
    check(field(line, "pid") == pid && field(line, "ppid") == getpid(), t, "the program runs with the spawned pid", field(line, "pid"));
    close(in[1]);
    int status = -1;
    check(waitpid(pid, &status, 0) == pid && status == 0, t, "status", status);
    close(outp[0]);
    char* foreign[] = {(char*)"launchctl", (char*)"asuser", other, (char*)self, (char*)"child", (char*)"exit", (char*)"0", 0};
    check(posix_spawn(&pid, "/bin/launchctl", 0, 0, foreign, environ) == 1, t, "asuser another uid: EPERM", 0);
    char* list[] = {(char*)"launchctl", (char*)"list", 0};
    check(posix_spawn(&pid, "/bin/launchctl", 0, 0, list, environ) == 8, t, "other launchctl commands: ENOEXEC", 0);
    check(access("/bin/launchctl", 1) == 0, t, "access(/bin/launchctl, X_OK)", errno);
    done(t, before);
}

int main(int argc, char** argv) {
    self = argv[0];
    if (argc >= 3 && same(argv[1], "child")) return child(argc, argv);
    if (argc < 2) { out(2, "usage: process_tree <test>|child <mode>\n"); return 2; }
    const char* test = argv[1];
    if (same(test, "identity")) test_identity();
    else if (same(test, "watch")) test_watch();
    else if (same(test, "locks")) test_locks();
    else if (same(test, "dev")) test_dev();
    else if (same(test, "launchctl")) test_launchctl();
    else { out(2, "unknown test\n"); return 2; }
    return failures;
}
