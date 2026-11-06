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

/* ---- MachServices: on-demand launch and check-in (L2) ------------------------------------------- */
typedef unsigned mach_port_t;
extern mach_port_t bootstrap_port;
extern int bootstrap_look_up(mach_port_t, const char*, mach_port_t*);
extern int bootstrap_check_in(mach_port_t, const char*, mach_port_t*);
extern mach_port_t mach_reply_port(void);
extern int mach_msg(void*, int, unsigned, unsigned, mach_port_t, unsigned, mach_port_t);

#define MACH_SEND_MSG 1
#define MACH_RCV_MSG 2
#define MACH_RCV_TIMEOUT 0x100
#define COPY_SEND 19
#define MAKE_SEND_ONCE 21
#define MOVE_SEND_ONCE 18
#define RCV_AUDIT (3u << 24)

typedef struct { unsigned bits, size; mach_port_t remote, local, voucher; int id; } header_t;
typedef struct { header_t header; unsigned value; } small_t;
typedef struct { header_t header; unsigned words[64]; } buffer_t;

/* The job: checks in the service, answers each request with its pid, forever. */
static int mach_job(const char* service) {
    mach_port_t port = 0;
    if (bootstrap_check_in(bootstrap_port, service, &port) != 0 || !port) { out(2, "check-in failed\n"); return 3; }
    for (;;) {
        buffer_t buffer;
        if (mach_msg(&buffer, MACH_RCV_MSG, 0, sizeof buffer, port, 0, 0) != 0) return 4;
        small_t reply;
        reply.header.bits = MOVE_SEND_ONCE;
        reply.header.size = sizeof reply;
        reply.header.remote = buffer.header.remote;
        reply.header.local = 0;
        reply.header.voucher = 0;
        reply.header.id = buffer.header.id + 1;
        reply.value = (unsigned)getpid();
        if (mach_msg(&reply, MACH_SEND_MSG, sizeof reply, 0, 0, 0, 0) != 0) return 5;
    }
}

/* Sends request `id` to `server` and waits for the job's answer: its pid, or -1. */
static long mach_ask(mach_port_t server, int id) {
    const mach_port_t reply = mach_reply_port();
    union { small_t request; buffer_t answer; } message;
    message.request.header.bits = COPY_SEND | (MAKE_SEND_ONCE << 8);
    message.request.header.size = sizeof(small_t);
    message.request.header.remote = server;
    message.request.header.local = reply;
    message.request.header.voucher = 0;
    message.request.header.id = id;
    message.request.value = 0;
    if (mach_msg(&message, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT, sizeof(small_t), sizeof(buffer_t), reply, 90000, 0) != 0) return -1;
    if (message.answer.header.id != id + 1) return -2;
    return (long)message.answer.words[0];
}

static void test_mach(void) {
    const int before = failures; const char* t = "mach";
    char uid[24], pidtext[24], label[128], service_name[128], domain[64], service[192], plist[1024], body[4096], output[8192];
    number(uid, (long)getuid());
    number(pidtext, (long)getpid());
    const char* tmp = getenv("TMPDIR");
    if (!tmp) { check(0, t, "TMPDIR", 0); return; }
    copy(label, "com.alchemy.test.mach"); append(label, pidtext);
    copy(service_name, label); append(service_name, ".service");
    copy(domain, "gui/"); append(domain, uid);
    copy(service, domain); append(service, "/"); append(service, label);
    copy(plist, tmp); if (plist[length(plist) - 1] != '/') append(plist, "/"); append(plist, label); append(plist, ".plist");
    body[0] = 0;
    append(body, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n");
    append(body, "<key>Label</key><string>"); append(body, label); append(body, "</string>\n");
    append(body, "<key>ProgramArguments</key><array><string>"); append(body, self);
    append(body, "</string><string>child</string><string>mach-job</string><string>"); append(body, service_name); append(body, "</string></array>\n");
    append(body, "<key>MachServices</key><dict><key>"); append(body, service_name); append(body, "</key><true/></dict>\n");
    append(body, "</dict>\n</plist>\n");
    check(write_file(plist, body) == 0, t, "write the job plist", 0);
    char* bootstrap[] = {(char*)"launchctl", (char*)"bootstrap", domain, plist, 0};
    int status = launchctl(bootstrap, output, sizeof output);
    check(status == 0, t, "bootstrap a job with MachServices", status);
    /* Not running yet; its service is found all the same (another process tree). */
    char* list_one[] = {(char*)"launchctl", (char*)"list", label, 0};
    status = launchctl(list_one, output, sizeof output);
    check(status == 0 && !find(output, "\"PID\""), t, "the job does not run before a message", status);
    mach_port_t server = 0;
    const int found = bootstrap_look_up(bootstrap_port, service_name, &server);
    check(found == 0 && server != 0, t, "bootstrap_look_up of the job's service", found);
    /* The first message launches the job; it checks in, receives it and answers. */
    const long first = server ? mach_ask(server, 700) : -1;
    check(first > 0, t, "the job launched on demand answers", first);
    status = launchctl(list_one, output, sizeof output);
    char pid_field[48]; copy(pid_field, "\"PID\" = "); number(pid_field + length(pid_field), first);
    check(status == 0 && find(output, pid_field) != 0, t, "the answering process is the job", first);
    /* A second message goes straight to the job (the port moved there). */
    const long second = server ? mach_ask(server, 710) : -1;
    check(second == first, t, "a second request reaches the same job", second);
    char* bootout[] = {(char*)"launchctl", (char*)"bootout", service, 0};
    status = launchctl(bootout, output, sizeof output);
    check(status == 0, t, "bootout", status);
    mach_port_t gone = 0;
    check(bootstrap_look_up(bootstrap_port, service_name, &gone) != 0, t, "the service is gone after bootout", 0);
    unlink(plist);
    done(t, before);
}

/* ---- Sockets: launch_activate_socket (L2) ----------------------------------------------------- */
extern int launch_activate_socket(const char*, int**, size_t*);
extern void free(void*);
extern void* launch_msg(void*);
extern void* launch_data_new_string(const char*);
extern void* launch_data_dict_lookup(void*, const char*);
extern int launch_data_get_type(void*);
extern unsigned long launch_data_array_get_count(void*);
extern void* launch_data_array_get_index(void*, unsigned long);
extern int launch_data_get_fd(void*);
extern void launch_data_free(void*);

/* The job: reports its Sockets entry, then waits until launchd ends it. */
static int socket_job(void) {
    char text[256], digits[24];
    int* fds = 0; size_t count = 0;
    const int error = launch_activate_socket("Listeners", &fds, &count);
    int* none = 0; size_t none_count = 0;
    const int missing = launch_activate_socket("NoSuchKey", &none, &none_count);
    text[0] = 0;
    append(text, "sockets error="); append(text, number(digits, error));
    append(text, " count="); append(text, number(digits, (long)count));
    append(text, " fd="); append(text, number(digits, count ? fds[0] : -1));
    append(text, " missing="); append(text, number(digits, missing));
    /* The legacy CheckIn: Label (a string) and Sockets.Listeners[0] (the same descriptor). */
    void* request = launch_data_new_string("CheckIn");
    void* reply = launch_msg(request);
    void* label = reply ? launch_data_dict_lookup(reply, "Label") : 0;
    void* sockets = reply ? launch_data_dict_lookup(reply, "Sockets") : 0;
    void* listeners = sockets ? launch_data_dict_lookup(sockets, "Listeners") : 0;
    const long first = listeners && launch_data_array_get_count(listeners) == 1 ? launch_data_get_fd(launch_data_array_get_index(listeners, 0)) : -1;
    append(text, " checkin_label="); append(text, number(digits, label ? launch_data_get_type(label) : -1));
    append(text, " checkin_fd="); append(text, number(digits, first));
    if (reply) launch_data_free(reply);
    launch_data_free(request);
    append(text, "\n");
    out(1, text);
    if (fds) free(fds);
    for (int i = 0; i < 600; ++i) usleep(100000);
    return 0;
}

static void test_sockets(void) {
    const int before = failures; const char* t = "sockets";
    char uid[24], pidtext[24], label[128], domain[64], service[192], plist[1024], marker[1024], path[1024], body[4096], text[4096], output[8192];
    number(uid, (long)getuid());
    number(pidtext, (long)getpid());
    const char* tmp = getenv("TMPDIR");
    if (!tmp) { check(0, t, "TMPDIR", 0); return; }
    /* Not a job: ESRCH. */
    int* fds = 0; size_t count = 0;
    check(launch_activate_socket("Listeners", &fds, &count) == 3, t, "launch_activate_socket outside a job: ESRCH", 0);
    copy(label, "com.alchemy.test.sockets"); append(label, pidtext);
    copy(domain, "gui/"); append(domain, uid);
    copy(service, domain); append(service, "/"); append(service, label);
    copy(plist, tmp); if (plist[length(plist) - 1] != '/') append(plist, "/"); append(plist, label); append(plist, ".plist");
    copy(marker, tmp); if (marker[length(marker) - 1] != '/') append(marker, "/"); append(marker, label); append(marker, ".out");
    copy(path, tmp); if (path[length(path) - 1] != '/') append(path, "/"); append(path, "lx"); append(path, pidtext); append(path, ".sock");
    unlink(marker);
    body[0] = 0;
    append(body, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n");
    append(body, "<key>Label</key><string>"); append(body, label); append(body, "</string>\n");
    append(body, "<key>ProgramArguments</key><array><string>"); append(body, self);
    append(body, "</string><string>child</string><string>socket-job</string><string>x</string></array>\n");
    append(body, "<key>RunAtLoad</key><true/>\n");
    append(body, "<key>StandardOutPath</key><string>"); append(body, marker); append(body, "</string>\n");
    append(body, "<key>Sockets</key><dict><key>Listeners</key><dict><key>SockPathName</key><string>"); append(body, path);
    append(body, "</string></dict></dict>\n</dict>\n</plist>\n");
    check(write_file(plist, body) == 0, t, "write the job plist", 0);
    char* bootstrap[] = {(char*)"launchctl", (char*)"bootstrap", domain, plist, 0};
    int status = launchctl(bootstrap, output, sizeof output);
    check(status == 0, t, "bootstrap a job with Sockets", status);
    check(wait_for(marker, "sockets ", text, sizeof text, 60000), t, "the job reports its sockets", 0);
    check(field(text, "error") == 0 && field(text, "count") == 1 && field(text, "fd") == 3, t, "Listeners: one socket at descriptor 3", field(text, "fd"));
    check(field(text, "missing") == 2, t, "an unknown key: ENOENT", field(text, "missing"));
    check(field(text, "checkin_label") == 7 && field(text, "checkin_fd") == 3, t, "launch_msg CheckIn: Label and Sockets", field(text, "checkin_fd"));
    char* bootout[] = {(char*)"launchctl", (char*)"bootout", service, 0};
    status = launchctl(bootout, output, sizeof output);
    check(status == 0, t, "bootout", status);
    unlink(plist); unlink(marker); unlink(path);
    done(t, before);
}

/* ---- XPC over Mach (L3) ---------------------------------------------------------------------- */
typedef void* xpc_object_t;
extern xpc_object_t xpc_connection_create_mach_service(const char*, void*, unsigned long long);
extern void xpc_connection_set_event_handler(xpc_object_t, void*);
extern void xpc_connection_resume(xpc_object_t);
extern void xpc_connection_cancel(xpc_object_t);
extern void xpc_connection_send_message(xpc_object_t, xpc_object_t);
extern void xpc_connection_send_message_with_reply(xpc_object_t, xpc_object_t, void*, void*);
extern xpc_object_t xpc_connection_send_message_with_reply_sync(xpc_object_t, xpc_object_t);
extern int xpc_connection_get_pid(xpc_object_t);
extern xpc_object_t xpc_dictionary_create(const char* const*, const xpc_object_t*, size_t);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern xpc_object_t xpc_dictionary_get_remote_connection(xpc_object_t);
extern void xpc_dictionary_set_string(xpc_object_t, const char*, const char*);
extern void xpc_dictionary_set_int64(xpc_object_t, const char*, long long);
extern void xpc_dictionary_set_fd(xpc_object_t, const char*, int);
extern const char* xpc_dictionary_get_string(xpc_object_t, const char*);
extern long long xpc_dictionary_get_int64(xpc_object_t, const char*);
extern int xpc_dictionary_dup_fd(xpc_object_t, const char*);
extern const void* xpc_get_type(xpc_object_t);
extern void xpc_release(xpc_object_t);
extern const char _xpc_type_dictionary, _xpc_type_connection, _xpc_type_error;
extern const char _xpc_error_connection_interrupted, _xpc_error_connection_invalid;
extern void* _NSConcreteGlobalBlock[];

/* Global blocks written out by hand (no -fblocks): isa, BLOCK_IS_GLOBAL, invoke, descriptor. */
struct block_descriptor { unsigned long reserved, size; };
struct block { void* isa; int flags; int reserved; void* invoke; struct block_descriptor* descriptor; };
static struct block_descriptor block_size = {0, sizeof(struct block)};
#define GLOBAL_BLOCK(function) { _NSConcreteGlobalBlock, 1 << 28, 0, (void*)(function), &block_size }

/* The service: echoes value + 1, its pid, and 4 bytes read from a passed fd; "quit" exits. */
static void service_message(struct block* self, xpc_object_t event) {
    (void)self;
    if (xpc_get_type(event) != &_xpc_type_dictionary) return;
    const char* op = xpc_dictionary_get_string(event, "op");
    if (op && same(op, "quit")) _exit(0);
    xpc_object_t reply = xpc_dictionary_create_reply(event);
    if (!reply) return;
    xpc_dictionary_set_int64(reply, "pid", getpid());
    xpc_dictionary_set_int64(reply, "value", xpc_dictionary_get_int64(event, "value") + 1);
    char text[8] = {0};
    const int fd = xpc_dictionary_dup_fd(event, "fd");
    if (fd >= 0) { read(fd, text, 4); close(fd); }
    xpc_dictionary_set_string(reply, "read", text);
    xpc_connection_send_message(xpc_dictionary_get_remote_connection(event), reply);
    xpc_release(reply);
}
static struct block service_message_block = GLOBAL_BLOCK(service_message);
static void service_peer(struct block* self, xpc_object_t peer) {
    (void)self;
    if (xpc_get_type(peer) != &_xpc_type_connection) return;
    xpc_connection_set_event_handler(peer, &service_message_block);
    xpc_connection_resume(peer);
}
static struct block service_peer_block = GLOBAL_BLOCK(service_peer);
static int xpc_service(const char* name) {
    xpc_object_t listener = xpc_connection_create_mach_service(name, 0, 1 /* XPC_CONNECTION_MACH_SERVICE_LISTENER */);
    xpc_connection_set_event_handler(listener, &service_peer_block);
    xpc_connection_resume(listener);
    for (;;) usleep(100000);
}

static volatile const void* client_error;
static void client_event(struct block* self, xpc_object_t event) { (void)self; if (xpc_get_type(event) == &_xpc_type_error) client_error = event; }
static struct block client_event_block = GLOBAL_BLOCK(client_event);
static volatile long long async_value = -1;
static void client_reply(struct block* self, xpc_object_t reply) {
    (void)self;
    async_value = xpc_get_type(reply) == &_xpc_type_dictionary ? xpc_dictionary_get_int64(reply, "value") : -2;
}
static struct block client_reply_block = GLOBAL_BLOCK(client_reply);

static xpc_object_t echo_request(long long value, int fd) {
    xpc_object_t message = xpc_dictionary_create(0, 0, 0);
    xpc_dictionary_set_string(message, "op", "echo");
    xpc_dictionary_set_int64(message, "value", value);
    if (fd >= 0) xpc_dictionary_set_fd(message, "fd", fd);
    return message;
}

static void test_xpc(void) {
    const int before = failures; const char* t = "xpc";
    char uid[24], pidtext[24], label[128], name[160], domain[64], service[192], plist[1024], body[4096], output[8192];
    number(uid, (long)getuid());
    number(pidtext, (long)getpid());
    const char* tmp = getenv("TMPDIR");
    if (!tmp) { check(0, t, "TMPDIR", 0); return; }
    copy(label, "com.alchemy.test.xpc"); append(label, pidtext);
    copy(name, label); append(name, ".service");
    copy(domain, "gui/"); append(domain, uid);
    copy(service, domain); append(service, "/"); append(service, label);
    copy(plist, tmp); if (plist[length(plist) - 1] != '/') append(plist, "/"); append(plist, label); append(plist, ".plist");
    body[0] = 0;
    append(body, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n");
    append(body, "<key>Label</key><string>"); append(body, label); append(body, "</string>\n");
    append(body, "<key>ProgramArguments</key><array><string>"); append(body, self);
    append(body, "</string><string>child</string><string>xpc-service</string><string>"); append(body, name); append(body, "</string></array>\n");
    append(body, "<key>MachServices</key><dict><key>"); append(body, name); append(body, "</key><true/></dict>\n");
    append(body, "<key>ThrottleInterval</key><integer>1</integer>\n");
    append(body, "</dict>\n</plist>\n");
    check(write_file(plist, body) == 0, t, "write the job plist", 0);
    char* bootstrap[] = {(char*)"launchctl", (char*)"bootstrap", domain, plist, 0};
    int status = launchctl(bootstrap, output, sizeof output);
    check(status == 0, t, "bootstrap an XPC service job", status);

    xpc_object_t connection = xpc_connection_create_mach_service(name, 0, 0);
    xpc_connection_set_event_handler(connection, &client_event_block);
    xpc_connection_resume(connection);
    /* A synchronous request launches the job; the reply carries its pid and what it read from our pipe. */
    int fds[2];
    pipe(fds);
    write(fds[1], "abcd", 4);
    close(fds[1]);
    xpc_object_t request = echo_request(41, fds[0]);
    close(fds[0]);
    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(connection, request);
    xpc_release(request);
    const int is_dictionary = xpc_get_type(reply) == &_xpc_type_dictionary;
    check(is_dictionary, t, "send_message_with_reply_sync: a dictionary", 0);
    long long first_pid = 0;
    if (is_dictionary) {
        first_pid = xpc_dictionary_get_int64(reply, "pid");
        const char* read_back = xpc_dictionary_get_string(reply, "read");
        check(xpc_dictionary_get_int64(reply, "value") == 42, t, "the service answered value + 1", (long)xpc_dictionary_get_int64(reply, "value"));
        check(first_pid > 0 && first_pid != getpid(), t, "the service is another process", (long)first_pid);
        check(read_back && same(read_back, "abcd"), t, "an fd travelled in the message", 0);
        check(xpc_connection_get_pid(connection) == first_pid, t, "xpc_connection_get_pid is the peer", xpc_connection_get_pid(connection));
    }
    xpc_release(reply);
    /* An asynchronous reply. */
    request = echo_request(99, -1);
    xpc_connection_send_message_with_reply(connection, request, 0, &client_reply_block);
    xpc_release(request);
    for (int i = 0; i < 600 && async_value == -1; ++i) usleep(50000);
    check(async_value == 100, t, "send_message_with_reply: the reply block", (long)async_value);
    /* The service exits: the connection is interrupted, and the next request reaches a new run. */
    xpc_object_t quit = xpc_dictionary_create(0, 0, 0);
    xpc_dictionary_set_string(quit, "op", "quit");
    xpc_connection_send_message(connection, quit);
    xpc_release(quit);
    for (int i = 0; i < 600 && client_error != (const void*)&_xpc_error_connection_interrupted; ++i) usleep(50000);
    check(client_error == (const void*)&_xpc_error_connection_interrupted, t, "the service's exit interrupts the connection", 0);
    request = echo_request(7, -1);
    reply = xpc_connection_send_message_with_reply_sync(connection, request);
    xpc_release(request);
    const long long second_pid = xpc_get_type(reply) == &_xpc_type_dictionary ? xpc_dictionary_get_int64(reply, "pid") : -1;
    check(second_pid > 0 && second_pid != first_pid && xpc_dictionary_get_int64(reply, "value") == 8, t, "after an interruption a new run answers", (long)second_pid);
    xpc_release(reply);
    /* Cancel: invalid last. */
    client_error = 0;
    xpc_connection_cancel(connection);
    for (int i = 0; i < 200 && client_error != (const void*)&_xpc_error_connection_invalid; ++i) usleep(50000);
    check(client_error == (const void*)&_xpc_error_connection_invalid, t, "cancel ends with XPC_ERROR_CONNECTION_INVALID", 0);
    xpc_release(connection);
    char* bootout[] = {(char*)"launchctl", (char*)"bootout", service, 0};
    status = launchctl(bootout, output, sizeof output);
    check(status == 0, t, "bootout", status);
    unlink(plist);
    done(t, before);
}

/* ---- Bundled XPC services (L4): run from LaunchdModel.app/Contents/MacOS ---------------------------- */
extern xpc_object_t xpc_connection_create(const char*, void*);

static long long bundled_ask(xpc_object_t connection, long long value, char* service, char* extra, long long* ppid) {
    xpc_object_t request = xpc_dictionary_create(0, 0, 0);
    xpc_dictionary_set_int64(request, "value", value);
    xpc_object_t reply = xpc_connection_send_message_with_reply_sync(connection, request);
    xpc_release(request);
    long long pid = -1;
    if (xpc_get_type(reply) == &_xpc_type_dictionary && xpc_dictionary_get_int64(reply, "value") == value + 1) {
        pid = xpc_dictionary_get_int64(reply, "pid");
        if (ppid) *ppid = xpc_dictionary_get_int64(reply, "ppid");
        const char* s = xpc_dictionary_get_string(reply, "service");
        const char* e = xpc_dictionary_get_string(reply, "extra");
        if (service) copy(service, s ? s : "");
        if (extra) copy(extra, e ? e : "");
    } else if (xpc_get_type(reply) == &_xpc_type_error) {
        pid = reply == (void*)&_xpc_error_connection_invalid ? -3 : -4;
    }
    xpc_release(reply);
    return pid;
}

static void test_xpc_bundle(void) {
    const int before = failures; const char* t = "xpc-bundle";
    char service[128] = {0}, extra[128] = {0};
    long long ppid = 0;
    xpc_object_t first = xpc_connection_create("com.alchemy.test.echo", 0);
    xpc_connection_set_event_handler(first, &client_event_block);
    xpc_connection_resume(first);
    const long long pid = bundled_ask(first, 1, service, extra, &ppid);
    check(pid > 0 && pid != getpid(), t, "the bundled service answers from its own process", (long)pid);
    check(ppid == getpid(), t, "the service is this application's child", (long)ppid);
    check(same(service, "com.alchemy.test.echo"), t, "XPC_SERVICE_NAME is its bundle identifier", 0);
    check(same(extra, "from-info-plist"), t, "XPCService EnvironmentVariables reach it", 0);
    /* A second connection reaches the same instance (ServiceType Application). */
    xpc_object_t second = xpc_connection_create("com.alchemy.test.echo", 0);
    xpc_connection_set_event_handler(second, &client_event_block);
    xpc_connection_resume(second);
    check(bundled_ask(second, 10, 0, 0, 0) == pid, t, "one instance serves the application", 0);
    /* A name no bundle has: invalid. */
    xpc_object_t none = xpc_connection_create("com.alchemy.test.none", 0);
    xpc_connection_set_event_handler(none, &client_event_block);
    xpc_connection_resume(none);
    check(bundled_ask(none, 3, 0, 0, 0) == -3, t, "an unknown service is XPC_ERROR_CONNECTION_INVALID", 0);
    xpc_connection_cancel(first);
    xpc_connection_cancel(second);
    xpc_connection_cancel(none);
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
    if (argc >= 4 && same(argv[2], "mach-job")) return mach_job(argv[3]);
    if (argc >= 3 && same(argv[2], "socket-job")) return socket_job();
    if (argc >= 4 && same(argv[2], "xpc-service")) return xpc_service(argv[3]);
    out(2, "unknown child mode\n");
    return 2;
}

int main(int argc, char** argv) {
    if (proc_pidpath(getpid(), self, sizeof self) <= 0) copy(self, argv[0]);
    if (argc >= 3 && same(argv[1], "child")) return child(argc, argv);
    if (argc < 2) { out(2, "usage: launchd_model <test>|child <mode>\n"); return 2; }
    if (same(argv[1], "launchctl")) test_launchctl();
    else if (same(argv[1], "mach")) test_mach();
    else if (same(argv[1], "sockets")) test_sockets();
    else if (same(argv[1], "xpc")) test_xpc();
    else if (same(argv[1], "xpc-bundle")) test_xpc_bundle();
    else { out(2, "unknown test\n"); return 2; }
    return failures;
}
