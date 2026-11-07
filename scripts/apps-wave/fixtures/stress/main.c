/* Toolchain stress fixture: threads, file I/O, mmap (anonymous, private and shared file), signals, fork and
   posix_spawn. Built by guest clang into objects + a static library (llvm-ar) and linked by ld64.lld.
   Prints one line per check and exits 0 only if every check passed. */
#include "decls.h"

static int failures;
static volatile int usr1_seen, alarm_seen;

static void check(const char *name, int ok, long detail) {
    printf("%-14s %s (%ld)\n", name, ok ? "ok" : "FAIL", detail);
    if (!ok) failures++;
}

static void *worker(void *argument) {
    long index = (long)argument;
    return (void *)util_sum_squares(1000 * (index + 1));
}

static void on_usr1(int signal) { usr1_seen = signal; }
static void on_alarm(int signal) { alarm_seen = signal; }

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == 'c') return 42;   /* spawned child */

    /* threads */
    pthread_t threads[4];
    long total = 0;
    for (long i = 0; i < 4; i++) pthread_create(&threads[i], 0, worker, (void *)i);
    for (int i = 0; i < 4; i++) { void *result; pthread_join(threads[i], &result); total += (long)result; }
    check("threads", total == util_sum_squares(1000) + util_sum_squares(2000) + util_sum_squares(3000) +
                                util_sum_squares(4000), total);

    /* file I/O */
    const char *path = "/tmp/alchemy-stress.bin";
    unsigned char data[4096];
    for (int i = 0; i < 4096; i++) data[i] = (unsigned char)(i * 7);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ssize_t written = write(fd, data, sizeof data);
    close(fd);
    unsigned char back[4096];
    fd = open(path, O_RDONLY);
    ssize_t got = read(fd, back, sizeof back);
    close(fd);
    check("file-io", written == 4096 && got == 4096 && util_checksum(back, 4096) == util_checksum(data, 4096), got);

    /* mmap: anonymous, private file, shared file written back */
    char *anon = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    long anon_ok = anon != MAP_FAILED;
    if (anon_ok) { anon[0] = 1; anon[(1 << 20) - 1] = 2; anon_ok = anon[0] + anon[(1 << 20) - 1] == 3; munmap(anon, 1 << 20); }
    check("mmap-anon", anon_ok, anon_ok);
    fd = open(path, O_RDWR);
    unsigned char *mapped = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    long shared_ok = mapped != MAP_FAILED && util_checksum(mapped, 4096) == util_checksum(data, 4096);
    if (mapped != MAP_FAILED) { mapped[0] = 0xEE; munmap(mapped, 4096); }
    close(fd);
    fd = open(path, O_RDONLY);
    read(fd, back, 1);
    close(fd);
    check("mmap-shared", shared_ok && back[0] == 0xEE, back[0]);
    unlink(path);

    /* signals: a handler for kill(getpid()) and for alarm() */
    struct darwin_sigaction action = {on_usr1, 0, 0};
    sigaction(SIGUSR1, &action, 0);
    kill(getpid(), SIGUSR1);
    check("signal-kill", usr1_seen == SIGUSR1, usr1_seen);
    action.handler = on_alarm;
    sigaction(SIGALRM, &action, 0);
    alarm(1);
    for (long spin = 0; !alarm_seen && spin < 4000000000L; spin++) {}
    check("signal-alarm", alarm_seen == SIGALRM, alarm_seen);

    /* fork + waitpid */
    pid_t child = fork();
    if (child == 0) _exit(7);
    int status = 0;
    waitpid(child, &status, 0);
    check("fork-wait", child > 0 && (status & 0177) == 0 && ((status >> 8) & 0xff) == 7, (status >> 8) & 0xff);

    /* posix_spawn of this program */
    char *child_argv[] = {argv[0], "child", 0};
    pid_t spawned = 0;
    int spawn_error = posix_spawn(&spawned, argv[0], 0, 0, child_argv, environ);
    status = 0;
    if (!spawn_error) waitpid(spawned, &status, 0);
    check("posix-spawn", !spawn_error && ((status >> 8) & 0xff) == 42, spawn_error ? spawn_error : (status >> 8) & 0xff);

    char line[64];
    util_format_count(line, sizeof line, "failures", failures);
    printf("%s\n", line);
    return failures ? 1 : 0;
}
