/* alchemy-fx fixture: Mach IPC between two Alchemy processes (the parent and
 * its posix_spawn'd copy), as Chrome's MachPortRendezvousServer uses it:
 *   1. the parent checks in a service; the child looks it up through the
 *      process tree's bootstrap registry;
 *   2. a special port set with posix_spawnattr_setspecialport_np reaches the
 *      child (task_get_special_port);
 *   3. the child's request carries a reply send-once right and a send right;
 *      the parent sees the child's pid in the audit trailer;
 *   4. the reply carries a send right and a receive right whose port already
 *      has a queued message (the message moves with the port);
 *   5. messages flow both ways on the received rights;
 *   6. when the child exits the parent gets a dead-name notification for the
 *      child's port and a no-senders notification for its own;
 *   7. a fileport for a pipe's read end travels in the reply; the child turns
 *      it into a descriptor and reads what the parent wrote;
 *   8. a named memory entry travels in the reply: both processes map it and
 *      see each other's stores (shared pages, not copies);
 *   9. the child's answer carries 128 KiB of out-of-line memory (large enough
 *      to travel in a host section) and an audit token with a pid version;
 *  10. the child's task port reaches the parent, which sets and reads the
 *      child's task policy and reads its task_info through it.
 * No SDK headers. Prints "fx mach cross: ok"; else exits with the step. */
typedef unsigned mach_port_t;
typedef int kern_return_t;
typedef int pid_t;
typedef void* posix_spawnattr_t;
extern mach_port_t mach_task_self_;
extern mach_port_t bootstrap_port;
extern char** environ;
extern kern_return_t mach_port_allocate(mach_port_t, int, mach_port_t*);
extern kern_return_t mach_port_insert_right(mach_port_t, mach_port_t, mach_port_t, int);
extern kern_return_t mach_port_request_notification(mach_port_t, mach_port_t, int, unsigned, mach_port_t, int, mach_port_t*);
extern kern_return_t mach_msg(void*, int, unsigned, unsigned, mach_port_t, unsigned, mach_port_t);
extern kern_return_t bootstrap_check_in(mach_port_t, const char*, mach_port_t*);
extern kern_return_t bootstrap_look_up(mach_port_t, const char*, mach_port_t*);
extern kern_return_t task_get_special_port(mach_port_t, int, mach_port_t*);
extern int posix_spawnattr_init(posix_spawnattr_t*);
extern int posix_spawnattr_setspecialport_np(posix_spawnattr_t*, mach_port_t, int);
extern int posix_spawn(pid_t*, const char*, void*, posix_spawnattr_t*, char* const[], char* const[]);
extern pid_t waitpid(pid_t, int*, int);
extern pid_t getpid(void);
extern int puts(const char*);
extern int pipe(int*);
extern long write(int, const void*, unsigned long);
extern long read(int, void*, unsigned long);
extern int fileport_makeport(int, mach_port_t*);
extern int fileport_makefd(mach_port_t);
typedef unsigned long long u64;
extern kern_return_t mach_make_memory_entry_64(mach_port_t, u64*, u64, int, mach_port_t*, mach_port_t);
extern kern_return_t mach_vm_map(mach_port_t, u64*, u64, u64, int, mach_port_t, u64, int, int, int, unsigned);
extern kern_return_t pid_for_task(mach_port_t, int*);
extern kern_return_t task_policy_set(mach_port_t, unsigned, int*, unsigned);
extern kern_return_t task_policy_get(mach_port_t, unsigned, int*, unsigned*, unsigned*);
extern kern_return_t task_info(mach_port_t, unsigned, int*, unsigned*);

#define RIGHT_RECEIVE 1
#define MOVE_RECEIVE 16
#define MOVE_SEND_ONCE 18
#define COPY_SEND 19
#define MAKE_SEND 20
#define MAKE_SEND_ONCE 21
#define SEND_MSG 1
#define RCV_MSG 2
#define RCV_TIMEOUT 0x100
#define RCV_AUDIT (3u << 24)
#define COMPLEX 0x80000000u
#define TASK_ACCESS_PORT 9
#define NOTIFY_NO_SENDERS 0106
#define NOTIFY_DEAD_NAME 0110

typedef struct { unsigned bits, size; mach_port_t remote, local, voucher; int id; } header_t;
typedef struct { mach_port_t name; unsigned pad1; unsigned short pad2; unsigned char disposition, type; } port_t;
typedef struct { header_t header; unsigned value; } small_t;
typedef struct { header_t header; unsigned count; port_t port; } one_port_t;
typedef struct { header_t header; unsigned count; port_t ports[4]; } two_ports_t;
typedef struct { header_t header; unsigned count; port_t ports[2]; } request_t;
#pragma pack(push, 4)
typedef struct { void* address; unsigned char deallocate, copy, pad, type; unsigned size; } ool_desc_t;
typedef struct { header_t header; unsigned count; ool_desc_t ool; unsigned value; } ool_t;
#pragma pack(pop)
#define OOL_SIZE 131072
static unsigned char payload[OOL_SIZE];
/* Receive buffers: room for the message and an audit trailer (52 bytes). */
typedef struct { header_t header; unsigned words[64]; } buffer_t;

static void put_header(header_t* header, unsigned bits, unsigned size, mach_port_t remote, mach_port_t local, int id) {
    header->bits = bits;
    header->size = size;
    header->remote = remote;
    header->local = local;
    header->voucher = 0;
    header->id = id;
}

static kern_return_t send_value(mach_port_t port, int id, unsigned value) {
    small_t message;
    put_header(&message.header, COPY_SEND, sizeof(message), port, 0, id);
    message.value = value;
    return mach_msg(&message, SEND_MSG, sizeof(message), 0, 0, 0, 0);
}

static kern_return_t receive(buffer_t* buffer, mach_port_t port) {
    return mach_msg(buffer, RCV_MSG | RCV_TIMEOUT | RCV_AUDIT, 0, sizeof(*buffer), port, 20000, 0);
}

/* The audit token's val[index] in a received message's trailer (5: pid,
 * 7: pid version). */
static unsigned audit_word(const buffer_t* buffer, int index) {
    const unsigned char* trailer = (const unsigned char*)buffer + buffer->header.size;
    return *(const unsigned*)(trailer + 20 + index * 4);
}
static unsigned audit_pid(const buffer_t* buffer) { return audit_word(buffer, 5); }

static void port_descriptor(port_t* port, mach_port_t name, unsigned char disposition) {
    port->name = name;
    port->pad1 = 0;
    port->pad2 = 0;
    port->disposition = disposition;
    port->type = 0;
}

static int child(const char* service) {
    const mach_port_t task = mach_task_self_;
    mach_port_t special = 0, server = 0, own = 0, reply = 0;
    if (task_get_special_port(task, TASK_ACCESS_PORT, &special) != 0 || !special) return 21;
    if (send_value(special, 300, 3) != 0) return 22;
    if (bootstrap_look_up(bootstrap_port, service, &server) != 0 || !server) return 23;
    if (mach_port_allocate(task, RIGHT_RECEIVE, &own) != 0 || mach_port_allocate(task, RIGHT_RECEIVE, &reply) != 0) return 24;
    /* The request: a reply send-once right, a send right to `own` and this task's port. */
    union { request_t request; buffer_t answer; } message;
    put_header(&message.request.header, COMPLEX | COPY_SEND | (MAKE_SEND_ONCE << 8), sizeof(request_t), server, reply, 100);
    message.request.count = 2;
    port_descriptor(&message.request.ports[0], own, MAKE_SEND);
    port_descriptor(&message.request.ports[1], task, COPY_SEND);
    if (mach_msg(&message, SEND_MSG | RCV_MSG | RCV_TIMEOUT, sizeof(request_t), sizeof(buffer_t), reply, 20000, 0) != 0) return 25;
    const two_ports_t* got = (const two_ports_t*)&message.answer;
    if (got->header.id != 101 || !(got->header.bits & COMPLEX) || got->count != 4) return 26;
    if (got->ports[0].disposition != 17 || got->ports[1].disposition != 16 || got->ports[2].disposition != 17) return 27;
    const int fd = fileport_makefd(got->ports[2].name);
    char text[8] = {0};
    if (fd < 0 || read(fd, text, 7) != 7 || text[0] != 'a' || text[6] != 'y') return 31;
    u64 shared = 0;
    if (mach_vm_map(task, &shared, 16384, 0, 1, got->ports[3].name, 0, 0, 3, 3, 2) != 0 || !shared) return 32;
    volatile unsigned* words = (volatile unsigned*)(unsigned long)shared;
    if (words[0] != 0x1234) return 33;   /* the parent's store */
    words[2] = 0x5678;                   /* seen by the parent */
    const mach_port_t back = got->ports[0].name, moved = got->ports[1].name;
    buffer_t buffer;
    if (receive(&buffer, moved) != 0 || buffer.header.id != 400) return 28;   /* queued before the move */
    if (receive(&buffer, own) != 0 || buffer.header.id != 500 || buffer.words[0] != 77) return 29;
    /* The parent set this task's category through its task port. */
    int role = -1;
    unsigned count = 1, fallback = 0;
    if (task_policy_get(task, 1, &role, &count, &fallback) != 0 || role != 2 || fallback != 0) return 34;
    ool_t answer;
    for (unsigned at = 0; at < OOL_SIZE; ++at) payload[at] = (unsigned char)(at * 7 + 3);
    put_header(&answer.header, COMPLEX | COPY_SEND, sizeof(answer), back, 0, 501);
    answer.count = 1;
    answer.ool.address = payload;
    answer.ool.deallocate = 0;
    answer.ool.copy = 1;   /* MACH_MSG_VIRTUAL_COPY */
    answer.ool.pad = 0;
    answer.ool.type = 1;   /* MACH_MSG_OOL_DESCRIPTOR */
    answer.ool.size = OOL_SIZE;
    answer.value = 78;
    if (mach_msg(&answer, SEND_MSG, sizeof(answer), 0, 0, 0, 0) != 0) return 30;
    return 0;
}

static char* decimal(char* out, unsigned value) {
    char digits[12];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) *out++ = digits[--count];
    *out = 0;
    return out;
}

static int same(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

int main(int argc, char** argv) {
    if (argc == 3 && same(argv[1], "child")) return child(argv[2]);
    const mach_port_t task = mach_task_self_;
    char service[64] = "org.alchemy.test.cross.";
    decimal(service + 23, (unsigned)getpid());
    mach_port_t server = 0, special = 0, movable = 0, back = 0, notify = 0, previous = 0;
    if (bootstrap_check_in(bootstrap_port, service, &server) != 0) return 1;
    if (mach_port_allocate(task, RIGHT_RECEIVE, &special) != 0 || mach_port_insert_right(task, special, special, MAKE_SEND) != 0) return 2;
    if (mach_port_allocate(task, RIGHT_RECEIVE, &movable) != 0 || mach_port_insert_right(task, movable, movable, MAKE_SEND) != 0) return 3;
    if (send_value(movable, 400, 4) != 0) return 4;
    if (mach_port_allocate(task, RIGHT_RECEIVE, &back) != 0 || mach_port_allocate(task, RIGHT_RECEIVE, &notify) != 0) return 5;
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0 || posix_spawnattr_setspecialport_np(&attributes, special, TASK_ACCESS_PORT) != 0) return 6;
    char* arguments[] = {argv[0], "child", service, 0};
    pid_t pid = 0;
    if (posix_spawn(&pid, argv[0], 0, &attributes, arguments, environ) != 0 || pid <= 0) return 7;
    buffer_t buffer;
    /* 2. The special port. */
    if (receive(&buffer, special) != 0 || buffer.header.id != 300 || audit_pid(&buffer) != (unsigned)pid) return 8;
    /* 3. The request, from the child's pid, with a send-once reply right. */
    if (receive(&buffer, server) != 0) return 9;
    if (buffer.header.id != 100 || (buffer.header.bits & 0x1f) != MOVE_SEND_ONCE || audit_pid(&buffer) != (unsigned)pid) return 10;
    const request_t* request = (const request_t*)&buffer;
    if (!(request->header.bits & COMPLEX) || request->count != 2 || request->ports[0].disposition != 17 ||
        request->ports[1].disposition != 17) return 11;
    const mach_port_t peer = request->ports[0].name, child_task = request->ports[1].name;
    /* 4. The reply: a send right to `back`, the receive right of `movable`. */
    two_ports_t reply;
    put_header(&reply.header, COMPLEX | MOVE_SEND_ONCE, sizeof(reply), request->header.remote, 0, 101);
    reply.count = 4;
    port_descriptor(&reply.ports[0], back, MAKE_SEND);
    port_descriptor(&reply.ports[1], movable, MOVE_RECEIVE);
    int fds[2];
    mach_port_t fileport = 0;
    if (pipe(fds) != 0 || write(fds[1], "alchemy", 7) != 7 || fileport_makeport(fds[0], &fileport) != 0 || !fileport) return 20;
    port_descriptor(&reply.ports[2], fileport, 17);   /* MACH_MSG_TYPE_MOVE_SEND */
    u64 size = 16384, shared = 0;
    mach_port_t entry = 0;
    if (mach_make_memory_entry_64(task, &size, 0, 0x020000 | 3, &entry, 0) != 0) return 21;
    if (mach_vm_map(task, &shared, size, 0, 1, entry, 0, 0, 3, 3, 2) != 0 || !shared) return 22;
    volatile unsigned* words = (volatile unsigned*)(unsigned long)shared;
    words[0] = 0x1234;
    port_descriptor(&reply.ports[3], entry, COPY_SEND);
    if (mach_msg(&reply, SEND_MSG, sizeof(reply), 0, 0, 0, 0) != 0) return 12;
    /* 6. Notifications for when the child goes away. */
    if (mach_port_request_notification(task, peer, NOTIFY_DEAD_NAME, 0, notify, MAKE_SEND_ONCE, &previous) != 0) return 13;
    if (mach_port_request_notification(task, back, NOTIFY_NO_SENDERS, 1, notify, MAKE_SEND_ONCE, &previous) != 0) return 14;
    /* 10. The child's task port: its pid, its policy, its task_info. */
    int task_pid = 0, values[16], role = 2;
    unsigned count = 1, fallback = 0;   /* in: FALSE asks for the current policy */
    if (pid_for_task(child_task, &task_pid) != 0 || task_pid != pid) return 40;
    if (task_policy_set(child_task, 1, &role, 1) != 0) return 41;   /* TASK_BACKGROUND_APPLICATION */
    values[0] = -1;
    if (task_policy_get(child_task, 1, values, &count, &fallback) != 0 || values[0] != 2 || fallback != 0 || count != 1) return 42;
    count = 10;   /* TASK_BASIC_INFO_64: resident size at byte 12 */
    if (task_info(child_task, 5, values, &count) != 0 || count != 10 || !*(const u64*)((const char*)values + 12)) return 43;
    count = 12;   /* TASK_POWER_INFO */
    if (task_info(child_task, 21, values, &count) != 0 || count != 12) return 44;
    /* 5. Messages both ways. */
    if (send_value(peer, 500, 77) != 0) return 15;
    /* 9. Out-of-line memory through a section; the sender's pid version. */
    if (receive(&buffer, back) != 0 || buffer.header.id != 501 || audit_pid(&buffer) != (unsigned)pid) return 16;
    const ool_t* answer = (const ool_t*)&buffer;
    if (!(answer->header.bits & COMPLEX) || answer->count != 1 || answer->ool.size != OOL_SIZE || !answer->ool.address ||
        answer->value != 78) return 24;
    const unsigned char* bytes = (const unsigned char*)answer->ool.address;
    for (unsigned at = 0; at < OOL_SIZE; ++at)
        if (bytes[at] != (unsigned char)(at * 7 + 3)) return 25;
    if (!audit_word(&buffer, 7)) return 26;
    if (words[2] != 0x5678) return 23;   /* the child's store, through shared pages */
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || status != 0) return 17;
    int dead = 0, unsent = 0;
    for (int count = 0; count < 2; ++count) {
        if (receive(&buffer, notify) != 0) return 18;
        if (buffer.header.id == NOTIFY_DEAD_NAME && buffer.words[2] == peer) ++dead;
        if (buffer.header.id == NOTIFY_NO_SENDERS) ++unsent;
    }
    if (dead != 1 || unsent != 1) return 19;
    puts("fx mach cross: ok");
    return 0;
}
