/* alchemy-fx fixture (owner: w3): an IOSurface between two Alchemy processes
 * (the parent and its posix_spawn'd copy), as Chrome's GPU process hands its
 * frames to the browser:
 *   1. the parent creates a 64x32 'BGRA' surface, writes a pixel, takes a use
 *      count and sends IOSurfaceCreateMachPort's send right in a reply;
 *   2. the child looks the surface up from the received right: the same
 *      IOSurfaceID, geometry and format, the parent's pixel, IOSurfaceLookup
 *      by the ID answers it, and the use count is the surface's (the
 *      parent's use shows: 1);
 *   3. the child writes a pixel and takes a use; the parent reads the pixel
 *      (shared pages, not a copy) and sees both uses (IOSurfaceIsInUse in a
 *      producer sees its consumer's use); the child drops its use, and both
 *      sides release their surface and their port.
 * No SDK headers. Prints "iosurface cross: ok"; else exits with the step. */
typedef unsigned mach_port_t;
typedef int kern_return_t;
typedef int pid_t;
typedef unsigned long long u64;
typedef const void* CFTypeRef;
extern mach_port_t mach_task_self_;
extern mach_port_t bootstrap_port;
extern char** environ;
extern kern_return_t mach_port_allocate(mach_port_t, int, mach_port_t*);
extern kern_return_t mach_port_deallocate(mach_port_t, mach_port_t);
extern kern_return_t mach_msg(void*, int, unsigned, unsigned, mach_port_t, unsigned, mach_port_t);
extern kern_return_t bootstrap_check_in(mach_port_t, const char*, mach_port_t*);
extern kern_return_t bootstrap_look_up(mach_port_t, const char*, mach_port_t*);
extern int posix_spawn(pid_t*, const char*, void*, void*, char* const[], char* const[]);
extern pid_t waitpid(pid_t, int*, int);
extern pid_t getpid(void);
extern int puts(const char*);

extern const void* kCFTypeDictionaryKeyCallBacks;
extern const void* kCFTypeDictionaryValueCallBacks;
extern CFTypeRef CFNumberCreate(CFTypeRef, long, const void*);
extern CFTypeRef CFDictionaryCreate(CFTypeRef, const void**, const void**, long, const void*, const void*);
extern void CFRelease(CFTypeRef);

extern CFTypeRef kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat;
extern CFTypeRef IOSurfaceCreate(CFTypeRef);
extern mach_port_t IOSurfaceCreateMachPort(CFTypeRef);
extern CFTypeRef IOSurfaceLookupFromMachPort(mach_port_t);
extern CFTypeRef IOSurfaceLookup(unsigned);
extern unsigned IOSurfaceGetID(CFTypeRef);
extern void* IOSurfaceGetBaseAddress(CFTypeRef);
extern unsigned long IOSurfaceGetBytesPerRow(CFTypeRef);
extern unsigned long IOSurfaceGetWidth(CFTypeRef);
extern unsigned long IOSurfaceGetHeight(CFTypeRef);
extern unsigned IOSurfaceGetPixelFormat(CFTypeRef);
extern int IOSurfaceLock(CFTypeRef, unsigned, unsigned*);
extern int IOSurfaceUnlock(CFTypeRef, unsigned, unsigned*);
extern void IOSurfaceIncrementUseCount(CFTypeRef);
extern void IOSurfaceDecrementUseCount(CFTypeRef);
extern int IOSurfaceGetUseCount(CFTypeRef);

#define RIGHT_RECEIVE 1
#define PORT_SEND 17
#define MOVE_SEND_ONCE 18
#define COPY_SEND 19
#define MAKE_SEND_ONCE 21
#define SEND_MSG 1
#define RCV_MSG 2
#define RCV_TIMEOUT 0x100
#define COMPLEX 0x80000000u
#define BGRA 0x42475241u

typedef struct { unsigned bits, size; mach_port_t remote, local, voucher; int id; } header_t;
typedef struct { mach_port_t name; unsigned pad1; unsigned short pad2; unsigned char disposition, type; } port_t;
typedef struct { header_t header; unsigned value; } small_t;
#pragma pack(push, 4)
typedef struct { header_t header; unsigned count; port_t port; unsigned id; } surface_t;
#pragma pack(pop)
typedef struct { header_t header; unsigned words[64]; } buffer_t;

static void put_header(header_t* header, unsigned bits, unsigned size, mach_port_t remote, mach_port_t local, int id) {
    header->bits = bits;
    header->size = size;
    header->remote = remote;
    header->local = local;
    header->voucher = 0;
    header->id = id;
}

static int child(const char* service) {
    const mach_port_t task = mach_task_self_;
    mach_port_t server = 0, reply = 0;
    if (bootstrap_look_up(bootstrap_port, service, &server) != 0 || !server) return 21;
    if (mach_port_allocate(task, RIGHT_RECEIVE, &reply) != 0) return 22;
    union { header_t request; buffer_t answer; } message;
    put_header(&message.request, COPY_SEND | (MAKE_SEND_ONCE << 8), sizeof(header_t), server, reply, 100);
    if (mach_msg(&message, SEND_MSG | RCV_MSG | RCV_TIMEOUT, sizeof(header_t), sizeof(buffer_t), reply, 20000, 0) != 0) return 23;
    const surface_t* got = (const surface_t*)&message.answer;
    if (got->header.id != 101 || !(got->header.bits & COMPLEX) || got->count != 1 || got->port.disposition != PORT_SEND) return 24;
    const mach_port_t port = got->port.name;
    const unsigned id = got->id;
    CFTypeRef surface = IOSurfaceLookupFromMachPort(port);
    if (!surface) return 25;
    if (IOSurfaceGetID(surface) != id) return 26;
    if (IOSurfaceGetWidth(surface) != 64 || IOSurfaceGetHeight(surface) != 32 || IOSurfaceGetPixelFormat(surface) != BGRA ||
        IOSurfaceGetBytesPerRow(surface) < 256) return 27;
    if (IOSurfaceGetUseCount(surface) != 1) return 28;   /* the parent's use shows here */
    CFTypeRef by_id = IOSurfaceLookup(id);
    if (by_id != surface) return 29;
    CFRelease(by_id);
    if (IOSurfaceLock(surface, 0, 0) != 0) return 30;
    volatile unsigned* pixels = (volatile unsigned*)IOSurfaceGetBaseAddress(surface);
    if (!pixels || pixels[0] != 0xa0a0a0a0u) return 31;   /* the parent's store */
    pixels[1] = 0xb1b1b1b1u;                               /* read by the parent */
    if (IOSurfaceUnlock(surface, 0, 0) != 0) return 32;
    IOSurfaceIncrementUseCount(surface);                   /* seen by the parent */
    if (IOSurfaceGetUseCount(surface) != 2) return 35;
    /* Tell the parent, and wait until it has looked (103 on the send-once reply). */
    union { small_t done; buffer_t answer; } step;
    put_header(&step.done.header, COPY_SEND | (MAKE_SEND_ONCE << 8), sizeof(small_t), server, reply, 102);
    step.done.value = id;
    if (mach_msg(&step, SEND_MSG | RCV_MSG | RCV_TIMEOUT, sizeof(small_t), sizeof(buffer_t), reply, 20000, 0) != 0) return 33;
    if (step.answer.header.id != 103) return 36;
    IOSurfaceDecrementUseCount(surface);
    if (IOSurfaceGetUseCount(surface) != 1) return 37;
    CFRelease(surface);
    if (mach_port_deallocate(task, port) != 0) return 34;
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
    char service[64] = "org.alchemy.test.iosurface.";
    decimal(service + 27, (unsigned)getpid());
    mach_port_t server = 0;
    if (bootstrap_check_in(bootstrap_port, service, &server) != 0) return 1;
    /* 1. The surface. */
    const long width = 64, height = 32, element = 4, format = (long)BGRA;
    const void* keys[4] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
    const void* values[4] = {CFNumberCreate(0, 4, &width), CFNumberCreate(0, 4, &height), CFNumberCreate(0, 4, &element),
                             CFNumberCreate(0, 4, &format)};
    CFTypeRef properties = CFDictionaryCreate(0, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef surface = IOSurfaceCreate(properties);
    if (!surface) return 2;
    CFRelease(properties);
    for (int index = 0; index < 4; ++index) CFRelease(values[index]);
    const unsigned id = IOSurfaceGetID(surface);
    if (!id) return 3;
    volatile unsigned* pixels = (volatile unsigned*)IOSurfaceGetBaseAddress(surface);
    pixels[0] = 0xa0a0a0a0u;
    IOSurfaceIncrementUseCount(surface);
    const mach_port_t port = IOSurfaceCreateMachPort(surface);
    if (!port) return 4;
    char* arguments[] = {argv[0], "child", service, 0};
    pid_t pid = 0;
    if (posix_spawn(&pid, argv[0], 0, 0, arguments, environ) != 0 || pid <= 0) return 5;
    /* The child's request, answered with the surface's port. */
    buffer_t buffer;
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 20000, 0) != 0 || buffer.header.id != 100) return 6;
    surface_t reply;
    put_header(&reply.header, COMPLEX | MOVE_SEND_ONCE, sizeof(reply), buffer.header.remote, 0, 101);
    reply.count = 1;
    reply.port.name = port;
    reply.port.pad1 = 0;
    reply.port.pad2 = 0;
    reply.port.disposition = COPY_SEND;
    reply.port.type = 0;
    reply.id = id;
    if (mach_msg(&reply, SEND_MSG, sizeof(reply), 0, 0, 0, 0) != 0) return 7;
    /* 3. The child's store, seen here. */
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 20000, 0) != 0 || buffer.header.id != 102 ||
        buffer.words[0] != id) return 8;
    if (pixels[1] != 0xb1b1b1b1u) return 9;
    /* The child's use shows here: a producer sees its consumer's use. */
    if (IOSurfaceGetUseCount(surface) != 2) return 14;
    small_t looked;
    put_header(&looked.header, MOVE_SEND_ONCE, sizeof(looked), buffer.header.remote, 0, 103);
    looked.value = 0;
    if (mach_msg(&looked, SEND_MSG, sizeof(looked), 0, 0, 0, 0) != 0) return 15;
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || status != 0) return 10;
    if (IOSurfaceGetUseCount(surface) != 1) return 11;
    IOSurfaceDecrementUseCount(surface);
    if (IOSurfaceGetUseCount(surface) != 0) return 12;
    CFRelease(surface);
    if (mach_port_deallocate(task, port) != 0) return 13;
    puts("iosurface cross: ok");
    return 0;
}
