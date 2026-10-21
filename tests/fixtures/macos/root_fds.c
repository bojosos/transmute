/* Root-process descriptor fixture (alchemy --fd N=MODE:TARGET, darwin_process.hpp).
 * Project-authored, no SDK headers; links against proc_link_system.dylib.
 *   root_fds echo         fd 3 read-only, fd 4 write-only: reads 3 to EOF, writes
 *                         "got:<data>" to 4, closes 4 (the host reader sees EOF)
 *   root_fds append       fd 4 is O_WRONLY|O_APPEND: writes "more\n"
 *   root_fds rw           fd 3 is O_RDWR: reads it, writes "rw:<data>" back at its end
 *   root_fds inherit      a posix_spawn'd child (no file actions) writes to fd 4 first
 *   root_fds absent       without --fd, descriptors 3 and 4 are not open
 *   root_fds child-write  (the inherit child) writes "child\n" to fd 4
 * Prints "ok <test>" or "FAIL <test>: ..." on stdout; the status is the failure count. */
typedef unsigned long size_t;
typedef long ssize_t;
typedef int pid_t;
typedef void* posix_spawn_file_actions_t;
typedef void* posix_spawnattr_t;

extern int* __error(void);
extern char** environ;
extern ssize_t write(int, const void*, size_t);
extern ssize_t read(int, void*, size_t);
extern int close(int);
extern int fcntl(int, int, ...);
extern pid_t waitpid(pid_t, int*, int);
extern int posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[], char* const[]);

enum { F_GETFD = 1, F_GETFL = 3, O_ACCMODE = 3, O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_APPEND = 8, EBADF = 9 };

static size_t length(const char* text) { size_t n = 0; while (text[n]) ++n; return n; }
static int same(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void say(const char* text) { write(1, text, length(text)); }
static int failures;
static void fail(const char* test, const char* why) { say("FAIL "); say(test); say(": "); say(why); say("\n"); ++failures; }

/* Reads fd to end of file; returns the byte count or -1. */
static ssize_t read_all(int fd, char* buffer, size_t size) {
    size_t total = 0;
    for (;;) {
        if (total == size) return -1;
        const ssize_t got = read(fd, buffer + total, size - total);
        if (got < 0) return -1;
        if (got == 0) return (ssize_t)total;
        total += (size_t)got;
    }
}

static int write_all(int fd, const char* data, size_t size) {
    while (size) {
        const ssize_t put = write(fd, data, size);
        if (put <= 0) return -1;
        data += put; size -= (size_t)put;
    }
    return 0;
}

static int access_mode(int fd) { const int flags = fcntl(fd, F_GETFL); return flags < 0 ? -1 : (flags & O_ACCMODE); }

static void echo(const char* test) {
    if (access_mode(3) != O_RDONLY) fail(test, "fd 3 is not O_RDONLY");
    if (access_mode(4) != O_WRONLY) fail(test, "fd 4 is not O_WRONLY");
    char data[4096];
    const ssize_t got = read_all(3, data, sizeof(data));
    if (got < 0) { fail(test, "read of fd 3 failed"); return; }
    /* Read-only and write-only ends refuse the other direction with EBADF. */
    *__error() = 0;
    if (write(3, "x", 1) != -1 || *__error() != EBADF) fail(test, "write to the read-only fd 3 did not fail with EBADF");
    *__error() = 0;
    if (read(4, data + got, 1) != -1 || *__error() != EBADF) fail(test, "read of the write-only fd 4 did not fail with EBADF");
    if (write_all(4, "got:", 4) || write_all(4, data, (size_t)got)) fail(test, "write to fd 4 failed");
    if (close(3) != 0) fail(test, "close of fd 3 failed");
    if (close(4) != 0) fail(test, "close of fd 4 failed");
    *__error() = 0;
    if (write(4, "x", 1) != -1 || *__error() != EBADF) fail(test, "fd 4 is still open after close");
    if (fcntl(3, F_GETFD) != -1) fail(test, "fd 3 is still open after close");
}

static void append(const char* test) {
    const int flags = fcntl(4, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_WRONLY || !(flags & O_APPEND)) fail(test, "fd 4 is not O_WRONLY|O_APPEND");
    if (write_all(4, "more\n", 5)) fail(test, "write to fd 4 failed");
    close(4);
}

static void rw(const char* test) {
    if (access_mode(3) != O_RDWR) fail(test, "fd 3 is not O_RDWR");
    char data[4096];
    const ssize_t got = read_all(3, data, sizeof(data));
    if (got < 0) { fail(test, "read of fd 3 failed"); return; }
    if (write_all(3, "rw:", 3) || write_all(3, data, (size_t)got)) fail(test, "write to fd 3 failed");
    close(3);
}

static void inherit(const char* test, const char* self) {
    char* const argv[] = {(char*)self, (char*)"child-write", 0};
    pid_t pid = 0;
    if (posix_spawn(&pid, self, 0, 0, argv, environ) != 0) { fail(test, "posix_spawn failed"); return; }
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || status != 0) fail(test, "the child did not exit 0");
    if (write_all(4, "parent\n", 7)) fail(test, "write to fd 4 failed");
    close(4);
}

static void absent(const char* test) {
    *__error() = 0;
    if (fcntl(3, F_GETFD) != -1 || *__error() != EBADF) fail(test, "fd 3 is open without --fd");
    *__error() = 0;
    if (fcntl(4, F_GETFD) != -1 || *__error() != EBADF) fail(test, "fd 4 is open without --fd");
}

int main(int argc, char** argv) {
    if (argc < 2) { say("usage: root_fds <test>\n"); return 1; }
    const char* test = argv[1];
    if (same(test, "child-write")) return write_all(4, "child\n", 6) ? 1 : 0;
    if (same(test, "echo")) echo(test);
    else if (same(test, "append")) append(test);
    else if (same(test, "rw")) rw(test);
    else if (same(test, "inherit")) inherit(test, argv[0]);
    else if (same(test, "absent")) absent(test);
    else { fail(test, "unknown test"); return 1; }
    if (!failures) { say("ok "); say(test); say("\n"); }
    return failures;
}
