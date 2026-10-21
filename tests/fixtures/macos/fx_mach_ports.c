/* alchemy-fx engine fixture: the in-process Mach IPC namespace
 * (src/alchemy/darwin/libsystem/mach) on real guest threads: senders blocked on
 * a full queue, a kevent64 EVFILT_MACHPORT loop with direct receive (as
 * Chrome's MessagePumpKqueue uses it), a send-once request/reply between
 * threads, a receive right moved to another thread in a message, and memory
 * entries mapped twice with mach_vm_map (shared aliases written from another
 * thread). No SDK headers. Prints "fx mach ports: ok" and returns 0, else
 * returns the failed step number. */
typedef unsigned mach_port_t;
typedef int kern_return_t;
typedef unsigned long long u64;
typedef void* pthread_t;
extern mach_port_t mach_task_self_;
extern kern_return_t mach_port_allocate(mach_port_t, int, mach_port_t*);
extern kern_return_t mach_port_insert_right(mach_port_t, mach_port_t, mach_port_t, int);
extern kern_return_t mach_port_mod_refs(mach_port_t, mach_port_t, int, int);
extern kern_return_t mach_port_deallocate(mach_port_t, mach_port_t);
extern kern_return_t mach_msg(void*, int, unsigned, unsigned, mach_port_t, unsigned, mach_port_t);
extern kern_return_t mach_make_memory_entry_64(mach_port_t, u64*, u64, int, mach_port_t*, mach_port_t);
extern kern_return_t mach_vm_map(mach_port_t, u64*, u64, u64, int, mach_port_t, u64, int, int, int, unsigned);
extern kern_return_t vm_deallocate(mach_port_t, unsigned long, unsigned long);
extern int kqueue(void);
extern int kevent64(int, const void*, int, void*, int, unsigned, const void*);
extern int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern int puts(const char*);

#define RIGHT_RECEIVE 1
#define MOVE_RECEIVE 16
#define MOVE_SEND_ONCE 18
#define COPY_SEND 19
#define MAKE_SEND 20
#define MAKE_SEND_ONCE 21
#define SEND_MSG 1
#define RCV_MSG 2
#define RCV_TIMEOUT 0x100
#define COMPLEX 0x80000000u
#define EVFILT_MACHPORT (-8)
#define EV_ADD 1

typedef struct { unsigned bits, size; mach_port_t remote, local, voucher; int id; } header_t;
typedef struct { header_t header; unsigned value; } small_t;
typedef struct { header_t header; unsigned value; unsigned trailer[20]; } small_rcv_t;
typedef struct { header_t header; unsigned count; mach_port_t name; unsigned pad1; unsigned short pad2; unsigned char disposition, type; } port_msg_t;
typedef struct { port_msg_t message; unsigned trailer[20]; } port_rcv_t;
typedef struct { u64 ident; short filter; unsigned short flags; unsigned fflags; long long data; u64 udata; u64 ext[2]; } kevent64_t;

#define SENDERS 4
#define PER_SENDER 100

static mach_port_t queue_port, service_port;

static kern_return_t send_value(mach_port_t port, int disposition, int id, unsigned value) {
    small_t message;
    message.header.bits = (unsigned)disposition;
    message.header.size = sizeof(small_t);
    message.header.remote = port;
    message.header.local = 0;
    message.header.voucher = 0;
    message.header.id = id;
    message.value = value;
    return mach_msg(&message, SEND_MSG, sizeof(small_t), 0, 0, 0, 0);
}

static void* sender(void* argument) {
    const unsigned base = (unsigned)(unsigned long)argument;
    for (unsigned index = 0; index < PER_SENDER; ++index)
        if (send_value(queue_port, COPY_SEND, (int)(base * 1000 + index), index + 1) != 0) return (void*)1;
    return 0;
}

/* Request/reply: receive one request and answer through its send-once right. */
static void* server(void* argument) {
    (void)argument;
    small_rcv_t request;
    if (mach_msg(&request, RCV_MSG, 0, sizeof(request), service_port, 0, 0) != 0) return (void*)1;
    if ((request.header.bits & 0x1f) != MOVE_SEND_ONCE || request.header.id != 500) return (void*)2;
    if (send_value(request.header.remote, MOVE_SEND_ONCE, 501, request.value * 2) != 0) return (void*)3;
    return 0;
}

/* Receives a receive right in a message, then a message on that right. */
static void* adopter(void* argument) {
    (void)argument;
    port_rcv_t carrier;
    if (mach_msg(&carrier, RCV_MSG, 0, sizeof(carrier), service_port, 0, 0) != 0) return (void*)1;
    if (!(carrier.message.header.bits & COMPLEX) || carrier.message.disposition != MOVE_RECEIVE) return (void*)2;
    small_rcv_t message;
    if (mach_msg(&message, RCV_MSG | RCV_TIMEOUT, 0, sizeof(message), carrier.message.name, 5000, 0) != 0) return (void*)3;
    if (message.value != 77) return (void*)4;
    if (mach_port_mod_refs(mach_task_self_, carrier.message.name, RIGHT_RECEIVE, -1) != 0) return (void*)5;
    return 0;
}

static u64* mapping;
static void* writer(void* argument) {
    (void)argument;
    for (unsigned index = 0; index < 4096; ++index) mapping[index] = (u64)index * 3 + 1;
    return 0;
}

int main(void) {
    const mach_port_t task = mach_task_self_;
    pthread_t threads[SENDERS];
    /* 1. Senders block on a full queue; a kevent64 loop receives directly. */
    if (mach_port_allocate(task, RIGHT_RECEIVE, &queue_port) != 0) return 1;
    if (mach_port_insert_right(task, queue_port, queue_port, MAKE_SEND) != 0) return 2;
    const int kq = kqueue();
    if (kq < 0) return 3;
    small_rcv_t buffer;
    kevent64_t change, event;
    change.ident = queue_port;
    change.filter = EVFILT_MACHPORT;
    change.flags = EV_ADD;
    change.fflags = RCV_MSG;
    change.data = 0;
    change.udata = 7;
    change.ext[0] = (u64)(unsigned long)&buffer;
    change.ext[1] = sizeof(buffer);
    if (kevent64(kq, &change, 1, 0, 0, 0, 0) != 0) return 4;
    for (unsigned index = 0; index < SENDERS; ++index)
        if (pthread_create(&threads[index], 0, sender, (void*)(unsigned long)index) != 0) return 5;
    unsigned long sum = 0;
    for (unsigned count = 0; count < SENDERS * PER_SENDER; ++count) {
        if (kevent64(kq, 0, 0, &event, 1, 0, 0) != 1) return 6;
        if (event.filter != EVFILT_MACHPORT || event.fflags != 0 || event.udata != 7) return 7;
        if (buffer.header.local != queue_port) return 8;
        sum += buffer.value;
    }
    for (unsigned index = 0; index < SENDERS; ++index) {
        void* result = 0;
        if (pthread_join(threads[index], &result) != 0 || result) return 9;
    }
    if (sum != SENDERS * (PER_SENDER * (PER_SENDER + 1) / 2)) return 10;
    /* 2. A request with a send-once reply right, served by another thread. */
    mach_port_t reply;
    if (mach_port_allocate(task, RIGHT_RECEIVE, &service_port) != 0 || mach_port_allocate(task, RIGHT_RECEIVE, &reply) != 0) return 11;
    pthread_t serving;
    if (pthread_create(&serving, 0, server, 0) != 0) return 12;
    small_t request;
    request.header.bits = MAKE_SEND | (MAKE_SEND_ONCE << 8);
    request.header.size = sizeof(request);
    request.header.remote = service_port;
    request.header.local = reply;
    request.header.voucher = 0;
    request.header.id = 500;
    request.value = 21;
    if (mach_msg(&request, SEND_MSG, sizeof(request), 0, 0, 0, 0) != 0) return 13;
    small_rcv_t answer;
    if (mach_msg(&answer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(answer), reply, 5000, 0) != 0) return 14;
    if (answer.header.id != 501 || answer.value != 42) return 15;
    void* served = 0;
    if (pthread_join(serving, &served) != 0 || served) return 16;
    /* 3. A receive right moves to another thread in a message. */
    mach_port_t moved;
    if (mach_port_allocate(task, RIGHT_RECEIVE, &moved) != 0) return 17;
    if (mach_port_insert_right(task, moved, moved, MAKE_SEND) != 0) return 18;
    pthread_t adopting;
    if (pthread_create(&adopting, 0, adopter, 0) != 0) return 19;
    port_msg_t carrier;
    carrier.header.bits = COMPLEX | MAKE_SEND;
    carrier.header.size = sizeof(carrier);
    carrier.header.remote = service_port;
    carrier.header.local = 0;
    carrier.header.voucher = 0;
    carrier.header.id = 600;
    carrier.count = 1;
    carrier.name = moved;
    carrier.pad1 = 0;
    carrier.pad2 = 0;
    carrier.disposition = MOVE_RECEIVE;
    carrier.type = 0;
    if (mach_msg(&carrier, SEND_MSG, sizeof(carrier), 0, 0, 0, 0) != 0) return 20;
    if (send_value(moved, COPY_SEND, 601, 77) != 0) return 21;
    void* adopted = 0;
    if (pthread_join(adopting, &adopted) != 0 || adopted) return 22;
    /* The receive right died with the adopter's mod_refs: our send right is dead. */
    if (send_value(moved, COPY_SEND, 602, 1) != 0x10000003) return 23;
    if (mach_port_deallocate(task, moved) != 0) return 24;
    /* 4. A named memory entry mapped twice: shared pages across threads. */
    u64 size = 4096 * 8;
    mach_port_t entry;
    if (mach_make_memory_entry_64(task, &size, 0, 0x020000 | 3, &entry, 0) != 0) return 25;
    u64 first = 0, second = 0;
    if (mach_vm_map(task, &first, size, 0, 1, entry, 0, 0, 3, 3, 2) != 0) return 26;
    if (mach_vm_map(task, &second, size, 0, 1, entry, 0, 0, 1, 1, 2) != 0) return 27;
    if (first == second) return 28;
    mapping = (u64*)(unsigned long)first;
    pthread_t writing;
    if (pthread_create(&writing, 0, writer, 0) != 0 || pthread_join(writing, 0) != 0) return 29;
    const volatile u64* view = (const volatile u64*)(unsigned long)second;
    for (unsigned index = 0; index < 4096; ++index)
        if (view[index] != (u64)index * 3 + 1) return 30;
    if (mach_port_deallocate(task, entry) != 0) return 31;
    if (vm_deallocate(task, (unsigned long)first, (unsigned long)size) != 0) return 32;
    if (view[100] != 301) return 33;
    if (vm_deallocate(task, (unsigned long)second, (unsigned long)size) != 0) return 34;
    puts("fx mach ports: ok");
    return 0;
}
