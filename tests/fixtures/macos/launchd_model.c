/* The session launchd and launchctl (src/alchemy/darwin/launchd/).
 * Project-authored, no SDK headers. One program plays every role:
 *   launchd_model <test>              a parent test; prints "ok <test>" or "FAIL <test>: ..."
 *   launchd_model child job <label>   a job: reports itself on stdout, then waits
 * Tests: launchctl (bootstrap, list, print, kickstart, kill, bootout, submit,
 * remove, setenv/getenv, the system domain, refused subcommands, through
 * posix_spawn and system()). The exit status is the failure count. */
typedef unsigned long size_t;
typedef long ssize_t;
typedef int pid_t;
typedef void* posix_spawn_file_actions_t;

extern int* __error(void);
extern char** environ;
extern ssize_t write(int, const void*, size_t);
extern ssize_t read(int, void*, size_t);
extern int close(int);
extern int pipe(int[2]);
extern int open(const char*, int, ...);
extern int unlink(const char*);
extern void _exit(int) __attribute__((noreturn));
extern pid_t waitpid(pid_t, int*, int);
extern pid_t getpid(void);
extern pid_t getppid(void);
extern unsigned getuid(void);
extern char* getenv(const char*);
extern int usleep(unsigned);
extern int system(const char*);
extern int proc_pidpath(int, void*, unsigned);
extern int posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*, const void*, char* const[], char* const[]);
extern int posix_spawn_file_actions_init(posix_spawn_file_actions_t*);
extern int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*);
extern int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int, int);
extern int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t*, int);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x200
#define O_TRUNC 0x400

static char self[1024];
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
static void copy(char* target, const char* source) { while ((*target++ = *source++)) {} }
static void append(char* target, const char* source) { copy(target + length(target), source); }
static const char* find(const char* text, const char* part) {
    for (; *text; ++text) if (starts(text, part)) return text;
    return 0;
}
static void check(int ok, const char* test, const char* what, long value) {
    if (ok) return;
    char text[24];
    ++failures;
    out(1, "FAIL "); out(1, test); out(1, ": "); out(1, what); out(1, " ("); out(1, number(text, value)); out(1, ")\n");
}
static void done(const char* test, int before) { if (failures == before) { out(1, "ok "); out(1, test); out(1, "\n"); } }
/* "key=value" in a line of space-separated fields. */
static long field(const char* line, const char* key) {
    const size_t n = length(key);
    for (const char* p = line; *p; ++p) {
        if ((p == line || p[-1] == ' ') && starts(p, key) && p[n] == '=') return parse(p + n + 1);
    }
    return -999999;
}

static int write_file(const char* path, const char* text) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    write(fd, text, length(text));
    close(fd);
    return 0;
}
static int read_file(const char* path, char* buffer, int limit) {
    buffer[0] = 0;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) { const ssize_t n = read(fd, buffer + total, (size_t)(limit - 1 - total)); if (n <= 0) break; total += (int)n; }
    buffer[total] = 0;
    close(fd);
    return total;
}

/* Runs /bin/launchctl with `args` (null-terminated, argv[0] included); its
 * stdout and stderr land in `output`. Returns the exit status, or -1. */
static int launchctl(char* const args[], char* output, int limit) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    pid_t pid = -1;
    const int error = posix_spawn(&pid, "/bin/launchctl", &actions, 0, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    int total = 0;
    output[0] = 0;
    if (!error) for (;;) { const ssize_t n = read(fds[0], output + total, (size_t)(limit - 1 - total)); if (n <= 0) break; total += (int)n; }
    output[total] = 0;
    close(fds[0]);
    if (error) return -1;
    int status = -1;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return (status & 0x7f) ? 128 + (status & 0x7f) : (status >> 8) & 0xff;
}

/* Polls `path` until it holds `part` (up to `ms`). */
static int wait_for(const char* path, const char* part, char* buffer, int limit, long ms) {
    for (; ms > 0; ms -= 50) {
        if (read_file(path, buffer, limit) > 0 && find(buffer, part)) return 1;
        usleep(50000);
    }
    return 0;
}

static void job_plist(char* text, const char* label, const char* marker) {
    text[0] = 0;
    append(text, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n");
    append(text, "<key>Label</key><string>"); append(text, label); append(text, "</string>\n");
    append(text, "<key>ProgramArguments</key><array><string>"); append(text, self);
    append(text, "</string><string>child</string><string>job</string><string>"); append(text, label); append(text, "</string></array>\n");
    append(text, "<key>RunAtLoad</key><true/>\n");
    append(text, "<key>EnvironmentVariables</key><dict><key>ALCHEMY_TEST_VALUE</key><string>from-plist</string></dict>\n");
    append(text, "<key>StandardOutPath</key><string>"); append(text, marker); append(text, "</string>\n");
    append(text, "<!-- a key launchd accepts that has no effect here -->\n<key>ProcessType</key><string>Background</string>\n");
    append(text, "</dict>\n</plist>\n");
}

static void test_launchctl(void) {
    const int before = failures; const char* t = "launchctl";
    char uid[24], pidtext[24], label[128], label2[128], domain[64], service[192], plist[1024], marker[1024], marker2[1024];
    char text[8192], body[4096], output[8192];
    number(uid, (long)getuid());
    number(pidtext, (long)getpid());
    const char* tmp = getenv("TMPDIR");
    check(tmp != 0 && tmp[0] == '/', t, "TMPDIR is a guest path", 0);
    if (!tmp) return;
    copy(label, "com.alchemy.test.job"); append(label, pidtext);
    copy(label2, "com.alchemy.test.submitted"); append(label2, pidtext);
    copy(domain, "gui/"); append(domain, uid);
    copy(service, domain); append(service, "/"); append(service, label);
    copy(plist, tmp); if (plist[length(plist) - 1] != '/') append(plist, "/"); append(plist, label); append(plist, ".plist");
    copy(marker, tmp); if (marker[length(marker) - 1] != '/') append(marker, "/"); append(marker, label); append(marker, ".out");
    copy(marker2, tmp); if (marker2[length(marker2) - 1] != '/') append(marker2, "/"); append(marker2, label2); append(marker2, ".out");
    unlink(marker); unlink(marker2);
    job_plist(body, label, marker);
    check(write_file(plist, body) == 0, t, "write the job plist", 0);

    /* bootstrap: the job runs at load, with launchd as its parent and its environment. */
    char* bootstrap[] = {(char*)"launchctl", (char*)"bootstrap", domain, plist, 0};
    int status = launchctl(bootstrap, output, sizeof output);
    check(status == 0, t, "bootstrap gui/<uid> <plist>", status);
    if (status != 0) { out(1, output); }
    check(wait_for(marker, "job ", text, sizeof text, 30000), t, "the job ran at load", 0);
    const long job_pid = field(text, "pid");
    check(job_pid > 0 && field(text, "ppid") == 1, t, "the job's parent is launchd (pid 1)", field(text, "ppid"));
    check(field(text, "env") == 1, t, "EnvironmentVariables reach the job", field(text, "env"));
    check(field(text, "service") == 1, t, "XPC_SERVICE_NAME is the label", field(text, "service"));

    /* list and print */
    char* list[] = {(char*)"launchctl", (char*)"list", 0};
    status = launchctl(list, output, sizeof output);
    check(status == 0 && starts(output, "PID\tStatus\tLabel\n"), t, "list header", status);
    char row[192]; number(row, job_pid); append(row, "\t0\t"); append(row, label);
    check(find(output, row) != 0, t, "list shows the running job", job_pid);
    char* print[] = {(char*)"launchctl", (char*)"print", service, 0};
    status = launchctl(print, output, sizeof output);
    check(status == 0 && find(output, "state = running") && find(output, "ProcessType"), t, "print <service>: running, ignored key listed", status);
    char* list_one[] = {(char*)"launchctl", (char*)"list", label, 0};
    status = launchctl(list_one, output, sizeof output);
    check(status == 0 && find(output, "\"PID\" = "), t, "list <label>", status);

    /* a second bootstrap of the same label */
    status = launchctl(bootstrap, output, sizeof output);
    check(status == 37, t, "bootstrap of a loaded service: 37", status);

    /* kickstart -k -p: a new process (StandardOutPath appends: the marker gains a line) */
    char* kickstart[] = {(char*)"launchctl", (char*)"kickstart", (char*)"-k", (char*)"-p", service, 0};
    status = launchctl(kickstart, output, sizeof output);
    const long new_pid = parse(output);
    check(status == 0 && new_pid > 0 && new_pid != job_pid, t, "kickstart -k -p prints a new pid", new_pid);
    char restarted[64]; copy(restarted, "job pid="); append(restarted, number(pidtext, new_pid)); append(restarted, " ");
    check(wait_for(marker, restarted, text, sizeof text, 30000), t, "the restarted job reports the new pid", new_pid);

    /* kill TERM: the job ends by the signal (no KeepAlive: it stays down) */
    char* killjob[] = {(char*)"launchctl", (char*)"kill", (char*)"TERM", service, 0};
    status = launchctl(killjob, output, sizeof output);
    check(status == 0, t, "kill TERM <service>", status);
    char down[192]; copy(down, "-\t-15\t"); append(down, label);
    int seen = 0;
    for (int i = 0; i < 600 && !seen; ++i) { launchctl(list, output, sizeof output); seen = find(output, down) != 0; if (!seen) usleep(100000); }
    check(seen, t, "list shows the job down with status -15", 0);

    /* bootout */
    char* bootout[] = {(char*)"launchctl", (char*)"bootout", service, 0};
    status = launchctl(bootout, output, sizeof output);
    check(status == 0, t, "bootout <service>", status);
    status = launchctl(bootout, output, sizeof output);
    check(status == 113, t, "bootout of an unknown service: 113", status);

    /* the system domain needs root */
    char* system_domain[] = {(char*)"launchctl", (char*)"bootstrap", (char*)"system", plist, 0};
    status = launchctl(system_domain, output, sizeof output);
    check(status == 1 && find(output, "Operation not permitted"), t, "bootstrap system: EPERM", status);

    /* submit and remove (a submitted job is kept alive) */
    char* submit[] = {(char*)"launchctl", (char*)"submit", (char*)"-l", label2, (char*)"-o", marker2, (char*)"--", self, (char*)"child", (char*)"job", label2, 0};
    status = launchctl(submit, output, sizeof output);
    check(status == 0, t, "submit", status);
    check(wait_for(marker2, "job ", text, sizeof text, 30000), t, "the submitted job runs", 0);
    char* remove[] = {(char*)"launchctl", (char*)"remove", label2, 0};
    status = launchctl(remove, output, sizeof output);
    check(status == 0, t, "remove", status);

    /* the session's environment */
    char* setenv_args[] = {(char*)"launchctl", (char*)"setenv", (char*)"ALCHEMY_LAUNCHD_TEST", (char*)"value 1", 0};
    check(launchctl(setenv_args, output, sizeof output) == 0, t, "setenv", 0);
    char* getenv_args[] = {(char*)"launchctl", (char*)"getenv", (char*)"ALCHEMY_LAUNCHD_TEST", 0};
    status = launchctl(getenv_args, output, sizeof output);
    check(status == 0 && starts(output, "value 1\n"), t, "getenv", status);

    /* informational and refused subcommands */
    char* error_args[] = {(char*)"launchctl", (char*)"error", (char*)"113", 0};
    status = launchctl(error_args, output, sizeof output);
    check(status == 0 && starts(output, "Could not find specified service"), t, "error 113", status);
    char* managerpid[] = {(char*)"launchctl", (char*)"managerpid", 0};
    status = launchctl(managerpid, output, sizeof output);
    check(status == 0 && starts(output, "1\n"), t, "managerpid", status);
    char* blame[] = {(char*)"launchctl", (char*)"blame", service, 0};
    status = launchctl(blame, output, sizeof output);
    check(status == 45 && find(output, "not supported under Alchemy"), t, "blame is refused by name", status);

    /* through the shell */
    status = system("launchctl list > /dev/null");
    check(status == 0, t, "system(\"launchctl list\")", status);
    unlink(plist); unlink(marker); unlink(marker2);
    done(t, before);
}

static int child(int argc, char** argv) {
    if (argc >= 4 && same(argv[2], "job")) {
        char text[256], digits[24];
        const char* value = getenv("ALCHEMY_TEST_VALUE");
        const char* name = getenv("XPC_SERVICE_NAME");
        text[0] = 0;
        append(text, "job pid="); append(text, number(digits, getpid()));
        append(text, " ppid="); append(text, number(digits, getppid()));
        append(text, " env="); append(text, value && same(value, "from-plist") ? "1" : "0");
        append(text, " service="); append(text, name && same(name, argv[3]) ? "1" : "0");
        append(text, "\n");
        out(1, text);
        for (int i = 0; i < 600; ++i) usleep(100000);   /* until launchd ends it */
        return 0;
    }
    out(2, "unknown child mode\n");
    return 2;
}

int main(int argc, char** argv) {
    if (proc_pidpath(getpid(), self, sizeof self) <= 0) copy(self, argv[0]);
    if (argc >= 3 && same(argv[1], "child")) return child(argc, argv);
    if (argc < 2) { out(2, "usage: launchd_model <test>|child <mode>\n"); return 2; }
    if (same(argv[1], "launchctl")) test_launchctl();
    else { out(2, "unknown test\n"); return 2; }
    return failures;
}
