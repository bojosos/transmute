/* Alchemy process-model fixture (docs/ALCHEMY_PROCESS.md). Project-authored,
 * no SDK headers. One program plays both roles:
 *   proc_model child <mode> ...   a child: echo, env, cwd, exit, cat, fds, sleep, pid, term
 *   proc_model <test>             a parent test; prints "ok <test>" or "FAIL <test>: ..."
 * The exit status of a test is its failure count. Children are this same
 * program, exec'd by its guest path (argv[0]). */
typedef unsigned long size_t;
typedef long ssize_t;
typedef int pid_t;
typedef void* posix_spawn_file_actions_t;
typedef void* posix_spawnattr_t;
typedef struct { void* opaque; } FILE_opaque;
typedef FILE_opaque FILE;
struct pollfd { int fd; short events; short revents; };

extern int* __error(void);
extern char** environ;
extern ssize_t write(int, const void*, size_t);
extern ssize_t read(int, void*, size_t);
extern int close(int);
extern int pipe(int[2]);
extern int dup(int);
extern int dup2(int, int);
extern int fcntl(int, int, ...);
extern int open(const char*, int, ...);
extern int unlink(const char*);
extern int poll(struct pollfd*, unsigned, int);
extern pid_t fork(void);
extern pid_t vfork(void);
extern int execve(const char*, char* const[], char* const[]);
extern int execv(const char*, char* const[]);
extern void _exit(int) __attribute__((noreturn));
extern void exit(int) __attribute__((noreturn));
extern pid_t waitpid(pid_t, int*, int);
extern pid_t wait4(pid_t, int*, int, void*);
extern int kill(pid_t, int);
extern pid_t getpid(void);
extern pid_t getppid(void);
extern pid_t getpgrp(void);
extern char* getenv(const char*);
extern int setenv(const char*, const char*, int);
extern unsigned getuid(void);
extern unsigned geteuid(void);
extern int setuid(unsigned);
extern int setgid(unsigned);
extern char* getcwd(char*, size_t);
extern int chdir(const char*);
extern int usleep(unsigned);
extern int system(const char*);
extern FILE* popen(const char*, const char*);
extern int pclose(FILE*);
extern char* fgets(char*, int, FILE*);
extern size_t fwrite(const void*, size_t, size_t, FILE*);
extern int posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[], char* const[]);
extern int posix_spawnp(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[], char* const[]);
extern int posix_spawn_file_actions_init(posix_spawn_file_actions_t*);
extern int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*);
extern int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int);
extern int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int, int);
extern int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t*, int, const char*, int, int);
extern int posix_spawn_file_actions_addinherit_np(posix_spawn_file_actions_t*, int);
extern int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t*, const char*);
extern int posix_spawnattr_init(posix_spawnattr_t*);
extern int posix_spawnattr_destroy(posix_spawnattr_t*);
extern int posix_spawnattr_setflags(posix_spawnattr_t*, short);
extern int posix_spawnattr_setpgroup(posix_spawnattr_t*, pid_t);
extern int pthread_mutex_lock(void*);
struct kevent { unsigned long ident; short filter; unsigned short flags; unsigned int fflags; long data; void* udata; };
struct timespec { long tv_sec; long tv_nsec; };
extern void* mmap(void*, size_t, int, int, int, long);
extern int mprotect(void*, size_t, int);
extern void* pthread_self(void);
extern void* pthread_get_stackaddr_np(void*);
extern size_t pthread_get_stacksize_np(void*);
extern int kqueue(void);
extern int kevent(int, const struct kevent*, int, struct kevent*, int, const struct timespec*);
struct termios { unsigned long c_iflag, c_oflag, c_cflag, c_lflag; unsigned char c_cc[20]; unsigned long c_ispeed, c_ospeed; };
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
extern int posix_openpt(int);
extern int grantpt(int);
extern int unlockpt(int);
extern char* ptsname(int);
extern char* ttyname(int);
extern int isatty(int);
extern int ioctl(int, unsigned long, ...);
extern int tcgetattr(int, struct termios*);
extern int tcsetattr(int, int, const struct termios*);
extern pid_t tcgetpgrp(int);
extern int socketpair(int, int, int, int[2]);

#define errno (*__error())
enum { O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_NONBLOCK = 4, O_CREAT = 0x200, O_TRUNC = 0x400, O_CLOEXEC = 0x1000000 };
enum { F_DUPFD = 0, F_GETFD = 1, F_SETFD = 2, F_GETFL = 3, F_SETFL = 4, FD_CLOEXEC = 1 };
enum { EAGAIN = 35, ENOENT = 2, ECHILD = 10, ENOSYS = 78, EBADF = 9, WNOHANG = 1 };
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0x7f && ((s) & 0x7f) != 0)
#define WTERMSIG(s) ((s) & 0x7f)

static const char* self;   /* argv[0]: this program's guest path */
static int failures;

static size_t length(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
static int same(const char* a, const char* b) { while (*a && *a == *b) ++a, ++b; return *a == *b; }
static int starts(const char* a, const char* b) { while (*b && *a == *b) ++a, ++b; return !*b; }
static void out(int fd, const char* s) { write(fd, s, length(s)); }
static char* number(char* buffer, long value) {
    char digits[24]; int n = 0; int negative = value < 0; unsigned long v = negative ? -(unsigned long)value : (unsigned long)value;
    do digits[n++] = (char)('0' + v % 10); while ((v /= 10));
    int i = 0; if (negative) buffer[i++] = '-';
    while (n) buffer[i++] = digits[--n];
    buffer[i] = 0; return buffer;
}
static long parse(const char* s) { long v = 0; int negative = *s == '-'; if (negative) ++s; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return negative ? -v : v; }
static void check(int condition, const char* test, const char* what, long value) {
    if (condition) return;
    char text[32];
    ++failures;
    out(1, "FAIL "); out(1, test); out(1, ": "); out(1, what); out(1, " ("); out(1, number(text, value)); out(1, ")\n");
}
static void done(const char* test, int before) { if (failures == before) { out(1, "ok "); out(1, test); out(1, "\n"); } }
/* Reads until EOF (or `limit`), returns the byte count. */
static int read_all(int fd, char* buffer, int limit) {
    int total = 0;
    for (;;) { ssize_t n = read(fd, buffer + total, (size_t)(limit - 1 - total)); if (n <= 0) break; total += (int)n; if (total >= limit - 1) break; }
    buffer[total] = 0; return total;
}

/* ---- child modes ------------------------------------------------------------------------------ */
static int child(int argc, char** argv) {
    char text[512];
    const char* mode = argv[2];
    if (same(mode, "echo")) { for (int i = 3; i < argc; ++i) { out(1, argv[i]); out(1, i + 1 < argc ? " " : ""); } out(1, "\n"); return 0; }
    if (same(mode, "argv0")) { out(1, argv[0]); out(1, "\n"); return 0; }
    if (same(mode, "env")) { const char* v = getenv(argv[3]); out(1, v ? v : "(unset)"); out(1, "\n"); return 0; }
    if (same(mode, "cwd")) { out(1, getcwd(text, sizeof text) ? text : "(error)"); out(1, "\n"); return 0; }
    if (same(mode, "exit")) return (int)parse(argv[3]);
    if (same(mode, "cat")) { char b[256]; ssize_t n; while ((n = read(0, b, sizeof b)) > 0) write(1, b, (size_t)n); return 0; }
    if (same(mode, "fds")) {   /* lists open descriptors 0..31 */
        for (int fd = 0; fd < 32; ++fd) if (fcntl(fd, F_GETFD) != -1) { out(1, number(text, fd)); out(1, " "); }
        out(1, "\n"); return 0;
    }
    if (same(mode, "write")) { out((int)parse(argv[3]), argv[4]); return 0; }
    if (same(mode, "sleep")) { long ms = parse(argv[3]); while (ms > 0) { usleep(10000); ms -= 10; } return 0; }
    if (same(mode, "pid")) { out(1, "pid="); out(1, number(text, getpid())); out(1, " ppid="); out(1, number(text, getppid())); out(1, "\n"); return 0; }
    if (same(mode, "term")) { kill(getpid(), 15); return 99; }
    if (same(mode, "tty")) {   /* node-pty's spawn-helper on a terminal slave: reopen it by name, report it */
        enum { TIOCGWINSZ = 0x40087468 };
        struct winsize size = {0, 0, 0, 0};
        const char* name = ttyname(0);
        const int reopened = name ? open(name, O_RDWR) : -1;
        if (!isatty(0) || !isatty(1) || !name || !same(name, argv[3]) || reopened < 0 || !isatty(reopened)) return 3;
        close(reopened);
        if (ioctl(1, TIOCGWINSZ, &size) != 0) return 4;
        out(1, "tty "); out(1, number(text, size.ws_row)); out(1, "x"); out(1, number(text, size.ws_col)); out(1, "\n");
        return 0;
    }
    if (same(mode, "ids")) {   /* uid euid and an environment value */
        const char* v = argc > 3 ? getenv(argv[3]) : 0;
        out(1, number(text, (long)getuid())); out(1, " "); out(1, number(text, (long)geteuid())); out(1, " ");
        out(1, v ? v : "(unset)"); out(1, "\n"); return 0;
    }
    return 100;
}

/* ---- parent tests ----------------------------------------------------------------------------- */
static void test_pipe(void) {
    const int before = failures; const char* t = "pipe";
    int p[2]; char b[64];
    check(pipe(p) == 0, t, "pipe", errno);
    check(write(p[1], "hello", 5) == 5, t, "write", errno);
    struct pollfd q = {p[0], 1, 0};
    check(poll(&q, 1, 1000) == 1 && (q.revents & 1), t, "poll readable", q.revents);
    check(read(p[0], b, sizeof b) == 5 && b[0] == 'h' && b[4] == 'o', t, "read", 0);
    check(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0, t, "F_SETFL", errno);
    check((fcntl(p[0], F_GETFL) & O_NONBLOCK) != 0, t, "F_GETFL", fcntl(p[0], F_GETFL));
    check(read(p[0], b, sizeof b) == -1 && errno == EAGAIN, t, "nonblocking read EAGAIN", errno);
    q.revents = 0;
    check(poll(&q, 1, 0) == 0, t, "poll empty", q.revents);
    int d = dup(p[1]);
    check(d > 2 && write(d, "x", 1) == 1, t, "dup write", d);
    close(d); close(p[1]);
    check(read(p[0], b, sizeof b) == 1, t, "read after dup", 0);
    check(read(p[0], b, sizeof b) == 0, t, "EOF after writers close", errno);
    q.revents = 0;
    check(poll(&q, 1, 0) == 1 && (q.revents & 0x10), t, "POLLHUP", q.revents);
    close(p[0]);
    check(close(p[0]) == -1 && errno == EBADF, t, "double close", errno);
    done(t, before);
}

static void test_spawn(void) {
    const int before = failures; const char* t = "posix_spawn_dup2";
    int p[2]; pid_t pid = 0; int status = -1; char b[256];
    pipe(p);
    posix_spawn_file_actions_t actions;
    check(posix_spawn_file_actions_init(&actions) == 0, t, "init", 0);
    posix_spawn_file_actions_adddup2(&actions, p[1], 1);
    posix_spawn_file_actions_addclose(&actions, p[0]);
    posix_spawn_file_actions_addclose(&actions, p[1]);
    char* argv[] = {(char*)"proc-child", (char*)"child", (char*)"echo", (char*)"spawned", (char*)"with space", 0};
    int result = posix_spawn(&pid, self, &actions, 0, argv, environ);
    check(result == 0 && pid > 0, t, "posix_spawn", result);
    posix_spawn_file_actions_destroy(&actions);
    close(p[1]);
    read_all(p[0], b, sizeof b);
    close(p[0]);
    check(same(b, "spawned with space\n"), t, "captured stdout", (long)length(b));
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, t, "waitpid", status);
    /* argv[0] reaches the child as given; posix_spawnp finds the built-in sh on PATH. */
    pipe(p);
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, p[1], 1);
    char* argv0[] = {(char*)"custom-argv0", (char*)"child", (char*)"argv0", 0};
    posix_spawn(&pid, self, &actions, 0, argv0, environ);
    close(p[1]); read_all(p[0], b, sizeof b); close(p[0]);
    check(same(b, "custom-argv0\n"), t, "argv[0]", (long)length(b));
    waitpid(pid, &status, 0);
    pipe(p);
    char* shell[] = {(char*)"sh", (char*)"-c", (char*)"echo from-sh; exit 3", 0};
    check(posix_spawnp(&pid, "sh", &actions, 0, shell, environ) == 0, t, "posix_spawnp sh", 0);
    posix_spawn_file_actions_destroy(&actions);
    close(p[1]);
    /* The file actions duplicated the first pipe's write end: read nothing here. */
    close(p[0]);
    check(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 3, t, "sh exit status", status);
    /* A missing program fails with ENOENT. */
    char* missing[] = {(char*)"x", 0};
    check(posix_spawn(&pid, "/Volumes/C/no/such/program", 0, 0, missing, environ) == ENOENT, t, "missing program ENOENT", 0);
    /* addopen and addchdir_np: the child's stdout is a file in another directory. */
    /* Named per process: the test's variants may run at once in one directory. */
    char spawned[64] = "proc_model_spawn_", digits[24];
    {
        const char* n = number(digits, getpid());
        unsigned at = 0;
        while (spawned[at]) ++at;
        while (*n) spawned[at++] = *n++;
        for (const char* x = ".txt"; *x;) spawned[at++] = *x++;
        spawned[at] = 0;
    }
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, spawned, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char* cwd[] = {(char*)"proc-child", (char*)"child", (char*)"cwd", 0};
    check(posix_spawn(&pid, self, &actions, 0, cwd, environ) == 0, t, "addopen spawn", 0);
    posix_spawn_file_actions_destroy(&actions);
    waitpid(pid, &status, 0);
    int f = open(spawned, O_RDONLY);
    read_all(f, b, sizeof b); close(f); unlink(spawned);
    check(b[0] == '/', t, "addopen output", (long)length(b));
    done(t, before);
}

/* boost::process's POSIX executor sequence (its public source): pipe, FD_CLOEXEC on the
 * write end, fork, child: close(read end), execve, on failure report errno
 * through the pipe and _exit(1); parent: close(write end), read the pipe
 * (EOF means exec succeeded), waitpid. */
static int boost_launch(const char* program, char* const* argv, int* reported) {
    int p[2];
    *reported = 0;
    if (pipe(p) == -1) return -1;
    if (fcntl(p[1], F_SETFD, FD_CLOEXEC) == -1) return -1;
    int sink = -1;   /* written by the child in the parent's frame, as boost's executor does */
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        sink = p[1];
        close(p[0]);
        execve(program, argv, environ);
        int error = errno;
        write(sink, &error, sizeof error);
        close(p[1]);
        _exit(1);
    }
    close(p[1]);
    int error = 0;
    if (read(p[0], &error, sizeof error) == (ssize_t)sizeof error) *reported = error;
    close(p[0]);
    if (sink != -1) *reported = -1000;   /* the child's frame writes must not reach the parent */
    return pid;
}

static void test_boost(void) {
    const int before = failures; const char* t = "boost_fork_exec";
    int status = -1, reported = 0;
    char* argv[] = {(char*)"proc-child", (char*)"child", (char*)"exit", (char*)"0", 0};
    pid_t pid = boost_launch(self, argv, &reported);
    check(pid > 0, t, "fork", errno);
    check(reported == 0, t, "no exec error reported", reported);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, t, "child status", status);
    pid = boost_launch("/Volumes/C/no/such/program", argv, &reported);
    check(pid > 0, t, "fork (missing)", errno);
    check(reported == ENOENT, t, "exec error through the pipe", reported);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 1, t, "_exit(1) status", status);
    done(t, before);
}

static void test_fork_dup2(void) {
    const int before = failures; const char* t = "fork_dup2_exec";
    int p[2]; char b[128]; int status = -1;
    pipe(p);
    pid_t pid = vfork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]); close(p[1]);
        char* argv[] = {(char*)"proc-child", (char*)"child", (char*)"echo", (char*)"through", (char*)"dup2", 0};
        execv(self, argv);
        _exit(127);
    }
    check(pid > 0, t, "vfork", errno);
    close(p[1]);
    read_all(p[0], b, sizeof b);
    close(p[0]);
    check(same(b, "through dup2\n"), t, "child stdout", (long)length(b));
    check(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0, t, "status", status);
    /* The parent's own descriptors are unchanged: stdout still works, the pipe is closed. */
    check(fcntl(1, F_GETFD) != -1, t, "parent stdout open", errno);
    check(fcntl(p[1], F_GETFD) == -1, t, "parent pipe closed", 0);
    done(t, before);
}

/* A thread-local the fork child touches before exec: its first use on the
   child's thread runs __tlv_bootstrap's host slow path, which the fork guard
   must allow (the parent has not touched it either). */
static __thread int fork_local = 7;
static void test_fork_tls(void) {
    const int before = failures; const char* t = "fork_tls_exec";
    int status = -1;
    pid_t pid = fork();
    if (pid == 0) {
        fork_local += 1;
        if (fork_local != 8) _exit(3);
        char* argv[] = {(char*)"proc-child", (char*)"child", (char*)"exit", (char*)"0", 0};
        execv(self, argv);
        _exit(127);
    }
    check(pid > 0, t, "fork", errno);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, t, "status", status);
    check(fork_local == 7, t, "the parent's own value", fork_local);
    done(t, before);
}

/* EVFILT_PROC NOTE_EXIT (libuv's child watch): the kqueue reports a child's
   exit, with its wait status under NOTE_EXITSTATUS, before waitpid reaps it. */
static void test_kqueue_proc(void) {
    const int before = failures; const char* t = "kqueue_proc_exit";
    enum { EVFILT_PROC = -5, EV_ADD = 1, EV_ONESHOT = 0x10, EV_EOF = 0x8000, NOTE_EXIT = (int)0x80000000, NOTE_EXITSTATUS = 0x04000000, ESRCH = 3 };
    const int kq = kqueue();
    check(kq >= 0, t, "kqueue", errno);
    pid_t pid = 0; int status = -1;
    char* exit5[] = {(char*)"c", (char*)"child", (char*)"exit", (char*)"5", 0};
    check(posix_spawn(&pid, self, 0, 0, exit5, environ) == 0, t, "posix_spawn", 0);
    struct kevent change = {(unsigned long)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, (unsigned)(NOTE_EXIT | NOTE_EXITSTATUS), 0, 0}, event = {0};
    check(kevent(kq, &change, 1, 0, 0, 0) == 0, t, "EVFILT_PROC registration", errno);
    struct timespec ten = {10, 0};
    const int got = kevent(kq, 0, 0, &event, 1, &ten);
    check(got == 1 && event.ident == (unsigned long)pid && event.filter == EVFILT_PROC && (event.fflags & (unsigned)NOTE_EXIT) &&
          (event.flags & EV_EOF) && WEXITSTATUS(event.data) == 5, t, "NOTE_EXIT with the exit status", (long)event.data);
    check(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 5, t, "waitpid after NOTE_EXIT", status);
    struct kevent stranger = {999999, EVFILT_PROC, EV_ADD, (unsigned)NOTE_EXIT, 0, 0};
    check(kevent(kq, &stranger, 1, 0, 0, 0) == -1 && errno == ESRCH, t, "not a child: ESRCH", errno);
    close(kq);
    done(t, before);
}

/* Rust's runtime puts its main-thread guard page at the lowest page of the
   stack: mmap(MAP_FIXED | MAP_ANON) over it, then mprotect(PROT_NONE). */
static void test_stack_guard(void) {
    const int before = failures; const char* t = "stack_guard_page";
    enum { PROT_NONE = 0, PROT_RW = 3, MAP_PRIVATE = 2, MAP_FIXED = 0x10, MAP_ANON = 0x1000, PAGE = 16384 };
    void* thread = pthread_self();
    char* top = (char*)pthread_get_stackaddr_np(thread);
    const size_t size = pthread_get_stacksize_np(thread);
    char* lowest = top - size;
    check(size >= 2 * PAGE && ((unsigned long)lowest % PAGE) == 0, t, "stack bounds", (long)size);
    void* mapped = mmap(lowest, PAGE, PROT_RW, MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
    check(mapped == lowest, t, "mmap(MAP_FIXED) of the lowest stack page", errno);
    check(mapped == lowest && lowest[0] == 0 && lowest[PAGE - 1] == 0, t, "the page reads zeros", 0);
    check(mprotect(lowest, PAGE, PROT_NONE) == 0, t, "mprotect(PROT_NONE) guard", errno);
    char* live = (char*)&before - ((unsigned long)&before % PAGE);   /* the page this frame is on */
    check(mmap(live, PAGE, PROT_RW, MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0) == (void*)-1, t, "the live stack page is not replaced", 0);
    done(t, before);
}

/* node-pty's sequence: posix_openpt, the slave opened by its name, the
   window size and attributes, and a session-leader child on the slave whose
   output reaches the master with "\n" mapped to "\r\n". */
static int read_until(int fd, char* buffer, int limit, char last) {
    int total = 0;
    while (total < limit - 1) {
        struct pollfd q = {fd, 1, 0};
        if (poll(&q, 1, 10000) != 1) break;
        const ssize_t n = read(fd, buffer + total, 1);
        if (n <= 0) break;
        if (buffer[total++] == last) break;
    }
    buffer[total] = 0; return total;
}
static void test_pty(void) {
    const int before = failures; const char* t = "pty";
    enum { O_NOCTTY = 0x20000, TIOCSWINSZ = 0x80087467, TIOCGWINSZ = 0x40087468, TIOCPTYGNAME = 0x40807453, ENOTTY = 25,
           OPOST = 1, ICRNL = 0x100, TCSANOW = 0, POSIX_SPAWN_SETSID = 0x400 };
    char b[256], name[128];
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    check(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0, t, "posix_openpt, grantpt, unlockpt", errno);
    const char* path = ptsname(master);
    check(path && starts(path, "/dev/ttys"), t, "ptsname is /dev/ttys<n>", 0);
    int i = 0; if (path) for (; path[i] && i < 127; ++i) name[i] = path[i]; name[i] = 0;
    check(ioctl(master, TIOCPTYGNAME, b) == 0 && same(b, name), t, "TIOCPTYGNAME on the master", errno);
    struct winsize size = {24, 80, 0, 0};
    check(ioctl(master, TIOCSWINSZ, &size) == 0, t, "TIOCSWINSZ", errno);
    const int slave = open(name, O_RDWR | O_NOCTTY);
    check(slave >= 0 && isatty(master) == 1 && isatty(slave) == 1, t, "the slave opens; both ends are terminals", slave);
    check(ttyname(slave) && same(ttyname(slave), name), t, "ttyname of the slave", 0);
    check(ioctl(slave, TIOCPTYGNAME, b) == -1 && errno == ENOTTY, t, "TIOCPTYGNAME on the slave: ENOTTY", errno);
    struct winsize got = {0, 0, 0, 0};
    check(ioctl(slave, TIOCGWINSZ, &got) == 0 && got.ws_row == 24 && got.ws_col == 80, t, "the slave sees the window size", got.ws_col);
    struct termios attributes;
    check(tcgetattr(slave, &attributes) == 0 && (attributes.c_oflag & 3) == 3 && (attributes.c_iflag & ICRNL), t, "default attributes", (long)attributes.c_oflag);
    check(write(slave, "a\n", 2) == 2 && read_until(master, b, sizeof b, '\n') == 3 && same(b, "a\r\n"), t, "output ONLCR", b[0]);
    check(write(master, "b\r", 2) == 2 && read_until(slave, b, sizeof b, '\n') == 2 && same(b, "b\n"), t, "input ICRNL", b[0]);
    attributes.c_oflag &= ~(unsigned long)OPOST;
    check(tcsetattr(slave, TCSANOW, &attributes) == 0, t, "tcsetattr", errno);
    check(write(slave, "c\n", 2) == 2 && read_until(master, b, sizeof b, '\n') == 2 && same(b, "c\n"), t, "raw output", b[0]);
    attributes.c_oflag |= OPOST;
    check(tcsetattr(slave, TCSANOW, &attributes) == 0, t, "tcsetattr restore", errno);
    /* A session leader on the slave (POSIX_SPAWN_SETSID), as node-pty starts spawn-helper. */
    posix_spawn_file_actions_t actions; posix_spawnattr_t attr; pid_t pid = 0; int status = -1;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd < 3; ++fd) posix_spawn_file_actions_adddup2(&actions, slave, fd);
    posix_spawn_file_actions_addclose(&actions, slave);
    posix_spawn_file_actions_addclose(&actions, master);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    char* argv[] = {(char*)"c", (char*)"child", (char*)"tty", name, 0};
    check(posix_spawn(&pid, self, &actions, &attr, argv, environ) == 0, t, "posix_spawn on the slave", errno);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attr);
    close(slave);
    check(read_until(master, b, sizeof b, '\n') > 0 && same(b, "tty 24x80\r\n"), t, "the child's report through the terminal", b[0]);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, t, "child status", status);
    check(tcgetpgrp(master) == pid, t, "the session leader's group is the foreground group", tcgetpgrp(master));
    struct pollfd q = {master, 1, 0};
    check(poll(&q, 1, 10000) == 1 && read(master, b, sizeof b) == 0, t, "end-of-file once every slave is closed", q.revents);
    close(master);
    done(t, before);
}

static void test_wait(void) {
    const int before = failures; const char* t = "waitpid_status";
    int status = -1; pid_t pid;
    char* exit7[] = {(char*)"c", (char*)"child", (char*)"exit", (char*)"7", 0};
    posix_spawn(&pid, self, 0, 0, exit7, environ);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7, t, "exit 7", status);
    char* sleeper[] = {(char*)"c", (char*)"child", (char*)"sleep", (char*)"300", 0};
    posix_spawn(&pid, self, 0, 0, sleeper, environ);
    check(waitpid(pid, &status, WNOHANG) == 0, t, "WNOHANG while running", status);
    check(wait4(-1, &status, 0, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, t, "wait4(-1)", status);
    char* term[] = {(char*)"c", (char*)"child", (char*)"term", 0};
    posix_spawn(&pid, self, 0, 0, term, environ);
    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == 15, t, "SIGTERM to itself", status);
    check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, t, "no children ECHILD", errno);
    check(waitpid(getppid(), &status, 0) == -1 && errno == ECHILD, t, "not a child ECHILD", errno);
    done(t, before);
}

static void test_popen(void) {
    const int before = failures; const char* t = "popen";
    char command[600], b[128];
    int n = 0; const char* parts[] = {"'", self, "' child echo popen 'two words'", 0};
    for (int i = 0; parts[i]; ++i) for (const char* c = parts[i]; *c; ++c) command[n++] = *c;
    command[n] = 0;
    FILE* stream = popen(command, "r");
    check(stream != 0, t, "popen r", errno);
    if (stream) {
        check(fgets(b, sizeof b, stream) != 0 && same(b, "popen two words\n"), t, "fgets", (long)length(b));
        check(pclose(stream) == 0, t, "pclose", 0);
    }
    n = 0; const char* failing[] = {"'", self, "' child exit 3", 0};
    for (int i = 0; failing[i]; ++i) for (const char* c = failing[i]; *c; ++c) command[n++] = *c;
    command[n] = 0;
    stream = popen(command, "r");
    check(stream && pclose(stream) == (3 << 8), t, "pclose status", 0);
    stream = popen("exit 4", "w");
    check(stream && pclose(stream) == (4 << 8), t, "popen w", 0);
    check(system(0) != 0, t, "system(NULL)", 0);
    check(system("true && exit 5") == (5 << 8), t, "system status", 0);
    check(system("no-such-command-xyz 2>/dev/null") == (127 << 8), t, "command not found 127", 0);
    check(system("if true; then :; fi") == (2 << 8), t, "unsupported syntax fails explicitly", 0);
    done(t, before);
}

static void test_cloexec(void) {
    const int before = failures; const char* t = "cloexec";
    int p[2]; char b[256]; pid_t pid; int status;
    int keep = open("proc_model_keep.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int drop = open("proc_model_drop.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int flagged = dup(keep);
    fcntl(flagged, F_SETFD, FD_CLOEXEC);
    check(fcntl(drop, F_GETFD) == FD_CLOEXEC && fcntl(keep, F_GETFD) == 0, t, "F_GETFD", 0);
    pipe(p);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, p[1], 1);
    char* argv[] = {(char*)"c", (char*)"child", (char*)"fds", 0};
    posix_spawn(&pid, self, &actions, 0, argv, environ);
    close(p[1]); read_all(p[0], b, sizeof b); close(p[0]);
    waitpid(pid, &status, 0);
    char expect[64], text[16]; int n = 0;
    const char* items[] = {"0 1 2 ", number(text, keep), " \n", 0};
    for (int i = 0; items[i]; ++i) for (const char* c = items[i]; *c; ++c) expect[n++] = *c;
    expect[n] = 0;
    check(same(b, expect), t, "inherited set", (long)length(b));
    if (!same(b, expect)) { out(1, "  got: "); out(1, b); }
    /* POSIX_SPAWN_CLOEXEC_DEFAULT: only descriptors named by file actions. */
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, 0x4000);
    pipe(p);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, p[1], 1);
    posix_spawn_file_actions_addinherit_np(&actions, 2);
    posix_spawn(&pid, self, &actions, &attributes, argv, environ);
    close(p[1]); read_all(p[0], b, sizeof b); close(p[0]);
    waitpid(pid, &status, 0);
    check(same(b, "1 2 \n"), t, "CLOEXEC_DEFAULT", (long)length(b));
    if (!same(b, "1 2 \n")) { out(1, "  got: "); out(1, b); }
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(keep); close(drop); close(flagged);
    unlink("proc_model_keep.txt"); unlink("proc_model_drop.txt");
    done(t, before);
}

static void test_kill(void) {
    const int before = failures; const char* t = "kill_wait";
    pid_t pid; int status = -1;
    char* sleeper[] = {(char*)"c", (char*)"child", (char*)"sleep", (char*)"60000", 0};
    posix_spawn(&pid, self, 0, 0, sleeper, environ);
    check(kill(pid, 0) == 0, t, "kill 0 alive", errno);
    check(kill(pid, 15) == 0, t, "SIGTERM", errno);
    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == 15, t, "killed by SIGTERM", status);
    check(kill(pid, 0) == -1, t, "reaped child is gone", 0);
    /* A killed child is waitable at once (WNOHANG), not after the host has
     * torn its process down. */
    posix_spawn(&pid, self, 0, 0, sleeper, environ);
    check(kill(pid, 9) == 0, t, "SIGKILL", errno);
    status = -1;
    check(waitpid(pid, &status, 1) == pid && WIFSIGNALED(status) && WTERMSIG(status) == 9, t, "killed child reaped at once", status);
    {
        /* Reaped: gone for kill() (ESRCH), and its pid is not reused while
         * the host may still be ending the process. */
        const pid_t reaped = pid;
        errno = 0;
        check(kill(reaped, 0) == -1 && errno == 3, t, "a reaped child is ESRCH", errno);
        /* SIGTERM, then SIGKILL (base::EnsureProcessTerminated): reaped at
         * once, with the signal that ended it. */
        posix_spawn(&pid, self, 0, 0, sleeper, environ);
        check(pid != reaped, t, "the reaped pid is not reused", pid);
        check(kill(pid, 15) == 0 && kill(pid, 9) == 0, t, "SIGTERM then SIGKILL", errno);
        status = -1;
        check(waitpid(pid, &status, 1) == pid && WIFSIGNALED(status) && WTERMSIG(status) == 15, t, "SIGTERM'd then SIGKILLed child reaped at once", status);
        check(pid != reaped, t, "a second child does not get the reaped pid", pid);
    }
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, 2);   /* POSIX_SPAWN_SETPGROUP, group 0: its own */
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawn(&pid, self, 0, &attributes, sleeper, environ);
    posix_spawnattr_destroy(&attributes);
    check(kill(-pid, 9) == 0, t, "SIGKILL to the group", errno);
    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == 9, t, "killed by SIGKILL", status);
    check(kill(getpid(), 0) == 0, t, "kill self 0", errno);
    check(kill(getpid(), 20) == 0, t, "SIGCHLD ignored by default", errno);
    done(t, before);
}

static void test_violation(void) {
    const int before = failures; const char* t = "fork_without_exec_fails";
    long mutex[8] = {0x32aaaba7, 0};
    pid_t pid = fork();
    if (pid == 0) { pthread_mutex_lock(mutex); _exit(0); }
    check(pid == -1 && errno == ENOSYS, t, "unsupported call before exec", pid);
    pid = fork();
    if (pid == 0) _exit(0);
    check(pid == -1 && errno == ENOSYS, t, "_exit without exec", pid);
    done(t, before);
}

static void test_shell(void) {
    const int before = failures; const char* t = "shell";
    int p[2]; char b[256]; pid_t pid; int status;
    pipe(p);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, p[1], 1);
    /* -i -l, as VS Code resolves the user's environment: no startup files exist. */
    char* argv[] = {(char*)"sh", (char*)"-i", (char*)"-l", (char*)"-c",
        (char*)"A=x; echo \"$A\" '$A' ${B:-dflt}; false || echo or; true && echo and; echo piped | \"$0\" child cat; "
               "\"$0\" child env ALCHEMY_TEST_VALUE; ALCHEMY_TEST_VALUE=set \"$0\" child env ALCHEMY_TEST_VALUE",
        (char*)self, 0};
    posix_spawn(&pid, "/bin/sh", &actions, 0, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(p[1]); read_all(p[0], b, sizeof b); close(p[0]);
    waitpid(pid, &status, 0);
    const char* expect = "x $A dflt\nor\nand\npiped\n(unset)\nset\n";
    check(same(b, expect), t, "output", (long)length(b));
    if (!same(b, expect)) { out(1, "  got: "); out(1, b); }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, t, "status", status);
    done(t, before);
}

/* The shell with a socket pair as its standard output, as Node's
   child_process gives its children: the built-in echo and a program it
   runs both write to the socket. */
static void test_shell_socket(void) {
    const int before = failures; const char* t = "shell_socket";
    enum { AF_UNIX = 1, SOCK_STREAM = 1 };
    int s[2]; char b[256]; pid_t pid = 0; int status = -1;
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0, t, "socketpair", errno);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, s[1], 1);
    posix_spawn_file_actions_addclose(&actions, s[0]);
    posix_spawn_file_actions_addclose(&actions, s[1]);
    char* argv[] = {(char*)"sh", (char*)"-c", (char*)"echo from the shell; \"$0\" child echo from its child", (char*)self, 0};
    check(posix_spawn(&pid, "/bin/sh", &actions, 0, argv, environ) == 0, t, "posix_spawn", errno);
    posix_spawn_file_actions_destroy(&actions);
    close(s[1]);
    read_all(s[0], b, sizeof b); close(s[0]);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, t, "status", status);
    const char* expect = "from the shell\nfrom its child\n";
    check(same(b, expect), t, "output", (long)length(b));
    if (!same(b, expect)) { out(1, "  got: "); out(1, b); }
    done(t, before);
}

/* What an installer needs from the shell: command -v, getconf ARG_MAX,
   ps, and socketfilterfw (no application firewall: adding an app is a no-op). */
static void test_installer(void) {
    const int before = failures; const char* t = "installer";
    char b[512]; int total = 0;
    FILE* stream = popen("command -v echo; getconf ARG_MAX; ps ax -o pid,command; "
                         "/usr/libexec/ApplicationFirewall/socketfilterfw --add /Applications/Example.app", "r");
    check(stream != 0, t, "popen", errno);
    if (stream) {
        while (total < (int)sizeof b - 1 && fgets(b + total, (int)sizeof b - total, stream)) total += (int)length(b + total);
        check(pclose(stream) == 0, t, "pclose", 0);
    }
    b[total] = 0;
    const char* expect = "echo\n1048576\n  PID COMMAND\nAlchemy: no application firewall; nothing to change for /Applications/Example.app\n";
    check(same(b, expect), t, "output", (long)length(b));
    if (!same(b, expect)) { out(1, "  got: "); out(1, b); }
    check(system("/usr/libexec/ApplicationFirewall/socketfilterfw --bogus 2>/dev/null") == (1 << 8), t, "unsupported option", 0);
    done(t, before);
}

/* ALCHEMY_SUPERUSER=1 (the driver sets it): root identity, a fork child's
   credential and environment changes are its own, children of root stay root
   whatever environment exec passes. Needs the fork emulation (alchemy-fx). */
static void test_superuser(void) {
    const int before = failures; const char* t = "superuser";
    int p[2]; char b[128]; int status = -1;
    check(getuid() == 0 && geteuid() == 0, t, "root", (long)getuid());
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1); close(p[0]); close(p[1]);
        setenv("ALCHEMY_TEST_VALUE", "forked", 1);
        if (setgid(1000) != 0 || setuid(1000) != 0) _exit(126);
        char* argv[] = {(char*)"proc-child", (char*)"child", (char*)"ids", (char*)"ALCHEMY_TEST_VALUE", 0};
        execv(self, argv);
        _exit(127);
    }
    close(p[1]); read_all(p[0], b, sizeof b); close(p[0]);
    check(same(b, "1000 1000 forked\n"), t, "fork child identity and environment", (long)length(b));
    if (!same(b, "1000 1000 forked\n")) { out(1, "  got: "); out(1, b); }
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, t, "status", status);
    check(getuid() == 0 && getenv("ALCHEMY_TEST_VALUE") == 0, t, "parent unchanged", (long)getuid());
    /* A child spawned with an empty environment is still root. */
    pipe(p);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, p[1], 1);
    char* argv[] = {(char*)"c", (char*)"child", (char*)"ids", (char*)"ALCHEMY_TEST_VALUE", 0};
    char* empty[] = {0};
    posix_spawn(&pid, self, &actions, 0, argv, empty);
    posix_spawn_file_actions_destroy(&actions);
    close(p[1]); read_all(p[0], b, sizeof b); close(p[0]);
    check(same(b, "0 0 (unset)\n"), t, "spawned child is root", (long)length(b));
    if (!same(b, "0 0 (unset)\n")) { out(1, "  got: "); out(1, b); }
    waitpid(pid, &status, 0);
    done(t, before);
}

static void test_exec_in_place(int argc, char** argv) {
    /* Phase 2 (after exec): the pid printed before exec is still ours. */
    if (argc > 2) {
        char text[24];
        check(getpid() == parse(argv[2]), "exec_in_place", "pid preserved across exec", getpid());
        out(1, "exec pid "); out(1, number(text, getpid())); out(1, "\n");
        if (!failures) out(1, "ok exec_in_place\n");
        exit(failures ? 1 : 42);
    }
    char text[24];
    char* next[] = {(char*)self, (char*)"exec", number(text, getpid()), 0};
    execve(self, next, environ);
    check(0, "exec_in_place", "execve returned", errno);
}

int main(int argc, char** argv) {
    self = argv[0];
    if (argc >= 3 && same(argv[1], "child")) return child(argc, argv);
    if (argc < 2) { out(2, "usage: proc_model <test>|child <mode>\n"); return 2; }
    const char* test = argv[1];
    if (same(test, "pipe")) test_pipe();
    else if (same(test, "spawn")) test_spawn();
    else if (same(test, "boost")) test_boost();
    else if (same(test, "forkdup")) test_fork_dup2();
    else if (same(test, "forktls")) test_fork_tls();
    else if (same(test, "wait")) test_wait();
    else if (same(test, "kqproc")) test_kqueue_proc();
    else if (same(test, "stackguard")) test_stack_guard();
    else if (same(test, "pty")) test_pty();
    else if (same(test, "popen")) test_popen();
    else if (same(test, "cloexec")) test_cloexec();
    else if (same(test, "kill")) test_kill();
    else if (same(test, "violation")) test_violation();
    else if (same(test, "shell")) test_shell();
    else if (same(test, "shellsock")) test_shell_socket();
    else if (same(test, "installer")) test_installer();
    else if (same(test, "superuser")) test_superuser();
    else if (same(test, "exec")) test_exec_in_place(argc, argv);
    else if (starts(test, "all")) {
        test_pipe(); test_spawn(); test_boost(); test_fork_dup2(); test_wait(); test_popen(); test_cloexec(); test_kill(); test_shell();
    } else { out(2, "unknown test\n"); return 2; }
    return failures;
}
