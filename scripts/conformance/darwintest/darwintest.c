/* Runtime of the conformance darwintest harness (darwintest.h). Project-authored, MIT. */
#include "darwintest.h"
#include "darwintest_multiprocess.h"
#include "darwintest_utils.h"

#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAX_CASES 256
static struct { const char *name; _t_fn fn; int helper; } cases[MAX_CASES];
static int case_count;
static const char *current;
static const char *self_path;
static int failures, quiet, mayfail, expectfail, with_errno;
static void (*atend[16])(void);
static int atend_count;

void _t_register(const char *name, _t_fn fn, int helper) {
    if (case_count < MAX_CASES) {
        cases[case_count].name = name;
        cases[case_count].fn = fn;
        cases[case_count].helper = helper;
        case_count++;
    }
}

const char *_t_name(void) { return current ? current : ""; }

static void vmessage(const char *prefix, const char *fmt, va_list ap) {
    fputs(prefix, stdout);
    if (fmt) vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static void finish(void) __attribute__((noreturn));
static void finish(void) {
    while (atend_count > 0) atend[--atend_count]();
    if (failures) {
        printf("[FAIL] %s: %d failure(s)\n", _t_name(), failures);
        fflush(stdout);
        exit(1);
    }
    printf("[PASS] %s\n", _t_name());
    fflush(stdout);
    exit(0);
}

void _t_end(void) { finish(); }

/* One check: applies T_MAYFAIL / T_EXPECTFAIL / T_WITH_ERRNO, which cover the next check only. */
static void outcome(int ok, int assert, const char *file, int line, const char *what, const char *values, int err,
                    const char *fmt, va_list ap) {
    int expected_fail = expectfail, may = mayfail, show_errno = with_errno;
    expectfail = mayfail = with_errno = quiet = 0;
    if (expected_fail) ok = 1;
    if (ok) return;
    printf("[%s] %s:%d: %s%s%s", may ? "MAYFAIL" : "FAIL", file, line, what, values ? " " : "", values ? values : "");
    if (show_errno || err) printf(" (errno %d: %s)", err, strerror(err));
    if (fmt) {
        fputs(": ", stdout);
        vfprintf(stdout, fmt, ap);
    }
    fputc('\n', stdout);
    fflush(stdout);
    if (may) return;
    failures++;
    if (assert) finish();
}

void _t_result(int ok, int assert, int has_values, const char *file, int line, const char *expr, long long lhs,
               long long rhs, const char *fmt, ...) {
    char values[96];
    if (has_values) snprintf(values, sizeof values, "(%lld vs %lld)", lhs, rhs);
    int err = with_errno ? errno : 0;
    va_list ap;
    va_start(ap, fmt);
    outcome(ok, assert, file, line, expr, has_values ? values : NULL, err, fmt, ap);
    va_end(ap);
}

void _t_result_str(int ok, int assert, const char *file, int line, const char *expr, const char *lhs, const char *rhs,
                   const char *fmt, ...) {
    char values[512];
    snprintf(values, sizeof values, "(\"%s\" vs \"%s\")", lhs ? lhs : "(null)", rhs ? rhs : "(null)");
    va_list ap;
    va_start(ap, fmt);
    outcome(ok, assert, file, line, expr, values, 0, fmt, ap);
    va_end(ap);
}

void _t_result_errno(int ok, int assert, const char *file, int line, const char *expr, int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    outcome(ok, assert, file, line, expr, NULL, ok ? 0 : err, fmt, ap);
    va_end(ap);
}

void _t_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vmessage("[LOG] ", fmt, ap);
    va_end(ap);
}

void _t_pass(const char *fmt, ...) {
    quiet = 0;
    va_list ap;
    va_start(ap, fmt);
    vmessage("[PASSED] ", fmt, ap);
    va_end(ap);
}

void _t_fail(int end, const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    outcome(0, end, file, line, "T_FAIL", NULL, 0, fmt, ap);
    va_end(ap);
}

void _t_skip(const char *fmt, ...) {
    printf("[SKIP] %s: ", _t_name());
    va_list ap;
    va_start(ap, fmt);
    if (fmt) vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
    while (atend_count > 0) atend[--atend_count]();
    exit(0);
}

void _t_quiet(void) { quiet = 1; }
void _t_mayfail(void) { mayfail = 1; }
void _t_expectfail(void) { expectfail = 1; }
void _t_with_errno(void) { with_errno = 1; }
void _t_setup(int begin) { (void)begin; }

void _t_atend(void (*fn)(void)) {
    if (atend_count < (int)(sizeof atend / sizeof atend[0])) atend[atend_count++] = fn;
}

/* ---- darwintest_utils.h ---------------------------------------------------------------------- */
const char *dt_tmpdir(void) {
    static char path[1024];
    if (!path[0]) {
        const char *base = getenv("TMPDIR");
        snprintf(path, sizeof path, "%s/dt.%s.%d", base && *base ? base : "/tmp", _t_name(), (int)getpid());
        mkdir(path, 0755);
    }
    return path;
}

int dt_ncpu(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }

int dt_launch_tool(pid_t *pid, char **argv, bool start_suspended, const char *stdout_path, const char *stderr_path) {
    (void)start_suspended;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (stdout_path) posix_spawn_file_actions_addopen(&actions, 1, stdout_path, 0x0601 /* O_WRONLY|O_CREAT|O_TRUNC */, 0644);
    if (stderr_path) posix_spawn_file_actions_addopen(&actions, 2, stderr_path, 0x0601, 0644);
    int err = posix_spawn(pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return err;
}

bool dt_waitpid(pid_t pid, int *exit_status, int *signal_out, int timeout) {
    time_t deadline = time(NULL) + (timeout > 0 ? timeout : 3600);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 || time(NULL) > deadline) {
            if (r == 0) kill(pid, SIGKILL);
            return false;
        }
        usleep(10000);
    }
    if (exit_status) *exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (signal_out) *signal_out = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void dt_print_hex_dump(const unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) printf("%02x%s", buf[i], (i % 16 == 15 || i + 1 == len) ? "\n" : " ");
}

void dt_timer_start(const char *name) { (void)name; }
void dt_timer_stop(const char *name) { (void)name; }

/* ---- darwintest_multiprocess.h --------------------------------------------------------------- */
dt_helper_t dt_child_helper(const char *name) { return (dt_helper_t){name, NULL}; }
dt_helper_t dt_child_helper_args(const char *name, char **args) { return (dt_helper_t){name, args}; }

void dt_run_helpers(dt_helper_t *helpers, size_t count, int timeout) {
    pid_t pids[16];
    for (size_t i = 0; i < count && i < 16; i++) {
        char *argv[32] = {(char *)self_path, (char *)helpers[i].name};
        int n = 2;
        for (char **a = helpers[i].args; a && *a && n < 31; a++) argv[n++] = *a;
        argv[n] = NULL;
        int err = posix_spawn(&pids[i], self_path, NULL, NULL, argv, environ);
        _t_result_errno(err == 0, 1, __FILE__, __LINE__, "posix_spawn(helper)", err, "%s", helpers[i].name);
    }
    for (size_t i = 0; i < count && i < 16; i++) {
        int code = 0, sig = 0;
        bool ok = dt_waitpid(pids[i], &code, &sig, timeout);
        _t_result(ok, 0, 0, __FILE__, __LINE__, "helper exited 0", 0, 0, "%s (exit %d, signal %d)", helpers[i].name,
                  code, sig);
    }
}

/* ---- main ------------------------------------------------------------------------------------ */
int main(int argc, char **argv) {
    self_path = argv[0];
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        for (int i = 0; i < case_count; i++)
            if (!cases[i].helper) printf("%s\n", cases[i].name);
        return 0;
    }
    const char *want = argv[1];
    if (want[0] == '-' && want[1] == 'n' && argc > 2) {  /* darwintest's `-n name` */
        want = argv[2];
        argv++;
        argc--;
    }
    for (int i = 0; i < case_count; i++) {
        if (strcmp(cases[i].name, want) == 0) {
            current = cases[i].name;
            cases[i].fn(argc - 2, argv + 2);  /* the body's argv starts after the case name */
            finish();
        }
    }
    fprintf(stderr, "darwintest: no test named %s\n", want);
    return 2;
}
