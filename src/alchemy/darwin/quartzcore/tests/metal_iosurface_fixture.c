/* alchemy-fx fixture (owner: g3, Chrome stage 4): IOSurface-backed
 * MTLTextures (-[MTLDevice newTextureWithDescriptor:iosurface:plane:]) as
 * Chrome's GPU process uses them, through objc_msgSend (no SDK headers).
 *
 * `metal_iosurface_fixture` (one process):
 *   1. a 60x32 'BGRA' surface (bytesPerRow 256 > 60 * 4) and a texture over
 *      it: -iosurface answers the surface, -iosurfacePlane 0;
 *   2. GPU writes, CPU reads: a render pass clears the texture green; after
 *      waitUntilCompleted the pages hold green in every texel, the row
 *      padding is untouched, and the seed moved;
 *   3. implicit synchronization: a clear to blue committed without waiting,
 *      then IOSurfaceLock: the pages hold blue;
 *   4. CPU writes, GPU reads: texels written between IOSurfaceLock and
 *      IOSurfaceUnlock are seen by a blit into a buffer and by a compute
 *      kernel that reads the texture (MSL texture2d::read);
 *   5. a texture view renders into the surface too, and answers its iosurface;
 *   6. nil for plane 1, for a pixel format of another texel size and for a
 *      texture larger than the surface;
 *   7. MTLSharedEvent handles: label, a keyed-archive round trip
 *      (NSSecureCoding), and -newSharedEventWithHandle: answering the event;
 *   8. two textures over one surface on one queue: the second reads what the
 *      first wrote in a command buffer committed just before (not completed);
 *   9. a command buffer that reads a texel, waits for an event and reads it
 *      again sees the CPU's write made before the signal (not with
 *      `staged`: without host-memory import the upload is a copy made at
 *      commit, see surface_textures.cpp);
 *  10. the texture and its 1184x1069 surface released while four command
 *      buffers clearing it are in flight: they complete (the pages stay
 *      mapped until the GPU is done), and a new surface works after them;
 *  11. a planar '420v' surface (a video frame: an R8 luma plane and a
 *      half-size RG8 chroma plane): CPU writes are seen through a texture
 *      over each plane, a GPU clear of the chroma plane lands in its pages
 *      and leaves the luma's alone, and plane 2 answers nil.
 * `metal_iosurface_fixture staged`: the same without step 9 (the test runs it
 *   with ALCHEMY_METAL_IOSURFACE_IMPORT=0).
 * `metal_iosurface_fixture cross`: the browser/GPU-process split. The parent
 *   (browser) creates the surface and sends its Mach port; the child (GPU
 *   process) makes the texture, clears it yellow, commits without waiting
 *   and reports; the parent's IOSurfaceLock waits for the child's GPU write
 *   and sees yellow. The parent writes texels; the child's next blit (an
 *   upload, as the shared seed moved) sees them. Then an MTLSharedEvent
 *   whose handle the parent archived into the same message: the child's
 *   command buffer waits on the GPU for value 1, clears cyan and signals 2;
 *   the parent sees nothing drawn and the value unchanged until its CPU
 *   signals 1, then waits for 2 and sees cyan.
 * `metal_iosurface_fixture bench <frames>`: the per-frame cost of the
 *   coherence copies at 1184x1069 (Chrome's window): each frame clears a
 *   plain texture, then an IOSurface-backed one, with waitUntilCompleted;
 *   prints both averages in microseconds.
 * `metal_iosurface_fixture present <capture directory, guest path>`: Chrome's
 *   GPU-process-to-browser presentation. The parent (browser) shows a cloaked
 *   window whose layer-backed view has a sublayer; its child (GPU process)
 *   renders the parent's IOSurface with Metal (magenta, then cyan) and
 *   reports each frame once committed, before it completes; the parent sets
 *   the surface as the sublayer's contents (then -setContentsChanged) and
 *   commits, and the window's composition in ALCHEMY_CONTENTS_CAPTURE shows
 *   each GPU frame (the compositor waits for the GPU write).
 * `metal_iosurface_fixture idle <seconds> bridge|plain`: a shared event with a
 *   handle (its process bridges it) or without, then the main thread blocks
 *   for <seconds>: the harness measures the process's CPU time (the bridge's
 *   idle cost).
 * Prints "metal iosurface: ok" (and "cross: ok", "bench: ..."); else exits
 * with the failing step. */
typedef unsigned mach_port_t;
typedef int kern_return_t;
typedef int pid_t;
typedef unsigned long long u64;
typedef unsigned long NSUInteger;
typedef const void* CFTypeRef;
typedef void* id;
typedef void* SEL;
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
extern u64 mach_absolute_time(void);
typedef struct { unsigned numer, denom; } timebase_t;
extern kern_return_t mach_timebase_info(timebase_t*);

extern const void* kCFTypeDictionaryKeyCallBacks;
extern const void* kCFTypeDictionaryValueCallBacks;
extern const void* kCFTypeArrayCallBacks;
extern CFTypeRef CFNumberCreate(CFTypeRef, long, const void*);
extern CFTypeRef CFArrayCreate(CFTypeRef, const void**, long, const void*);
extern CFTypeRef CFDictionaryCreate(CFTypeRef, const void**, const void**, long, const void*, const void*);
extern CFTypeRef CFStringCreateWithCString(CFTypeRef, const char*, unsigned);
extern CFTypeRef CFDataCreate(CFTypeRef, const unsigned char*, long);
extern const unsigned char* CFDataGetBytePtr(CFTypeRef);
extern long CFDataGetLength(CFTypeRef);
extern void CFRelease(CFTypeRef);

extern CFTypeRef kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat;
extern CFTypeRef kIOSurfacePlaneInfo, kIOSurfacePlaneWidth, kIOSurfacePlaneHeight, kIOSurfacePlaneBytesPerElement;
extern void* IOSurfaceGetBaseAddressOfPlane(CFTypeRef, unsigned long);
extern unsigned long IOSurfaceGetBytesPerRowOfPlane(CFTypeRef, unsigned long);
extern CFTypeRef IOSurfaceCreate(CFTypeRef);
extern mach_port_t IOSurfaceCreateMachPort(CFTypeRef);
extern CFTypeRef IOSurfaceLookupFromMachPort(mach_port_t);
extern void* IOSurfaceGetBaseAddress(CFTypeRef);
extern unsigned long IOSurfaceGetBytesPerRow(CFTypeRef);
extern unsigned IOSurfaceGetSeed(CFTypeRef);
extern int IOSurfaceLock(CFTypeRef, unsigned, unsigned*);
extern int IOSurfaceUnlock(CFTypeRef, unsigned, unsigned*);

extern id MTLCreateSystemDefaultDevice(void);
extern id objc_msgSend(id, SEL, ...);
extern id objc_getClass(const char*);
extern SEL sel_registerName(const char*);

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
#define NV12 0x34323076u   /* '420v' */
#define LOCK_READ_ONLY 1u
/* MTLPixelFormat, MTLTextureUsage, MTLLoadAction, MTLStoreAction (Metal headers' values). */
#define R8_UNORM 10u
#define RG8_UNORM 30u
#define BGRA8_UNORM 80u
#define RGBA16_FLOAT 115u
#define USAGE_SHADER_READ 1u
#define USAGE_RENDER_TARGET 4u
#define LOAD_CLEAR 2u
#define STORE_STORE 1u
#define STATUS_COMPLETED 4u
/* BGRA8 texels as little-endian words: B in the low byte. */
#define GREEN 0xff00ff00u
#define BLUE 0xff0000ffu
#define RED 0xffff0000u
#define YELLOW 0xffffff00u
#define CYAN 0xff00ffffu
#define PADDING 0x5a5a5a5au

typedef struct { double red, green, blue, alpha; } ClearColor;
typedef struct { NSUInteger x, y, z; } Triple;   /* MTLOrigin / MTLSize */

static SEL S(const char* name) { return sel_registerName(name); }
static id send0(id self, const char* selector) { return ((id (*)(id, SEL))objc_msgSend)(self, S(selector)); }
static id send1(id self, const char* selector, id a) { return ((id (*)(id, SEL, id))objc_msgSend)(self, S(selector), a); }
static id sendu(id self, const char* selector, NSUInteger a) { return ((id (*)(id, SEL, NSUInteger))objc_msgSend)(self, S(selector), a); }
static NSUInteger value0(id self, const char* selector) { return ((NSUInteger (*)(id, SEL))objc_msgSend)(self, S(selector)); }

static id string(const char* text) { return (id)CFStringCreateWithCString(0, text, 0x08000100u /* UTF-8 */); }

static CFTypeRef make_surface(long width, long height) {
    const long element = 4, format = (long)BGRA;
    const void* keys[4] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
    const void* values[4] = {CFNumberCreate(0, 4, &width), CFNumberCreate(0, 4, &height), CFNumberCreate(0, 4, &element),
                             CFNumberCreate(0, 4, &format)};
    CFTypeRef properties = CFDictionaryCreate(0, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    for (int index = 0; index < 4; ++index) CFRelease(values[index]);
    return surface;
}

/* kIOSurfacePlaneInfo: a width x height luma plane (1 byte) and a half-size chroma plane (2 bytes), rows as
 * IOSurface aligns them. */
static CFTypeRef make_planar_surface(long width, long height) {
    const long luma_bytes = 1, chroma_bytes = 2, half_width = width / 2, half_height = height / 2, format = (long)NV12;
    const void* plane_keys[3] = {kIOSurfacePlaneWidth, kIOSurfacePlaneHeight, kIOSurfacePlaneBytesPerElement};
    const void* luma_values[3] = {CFNumberCreate(0, 4, &width), CFNumberCreate(0, 4, &height), CFNumberCreate(0, 4, &luma_bytes)};
    const void* chroma_values[3] = {CFNumberCreate(0, 4, &half_width), CFNumberCreate(0, 4, &half_height),
                                    CFNumberCreate(0, 4, &chroma_bytes)};
    const void* planes[2] = {
        CFDictionaryCreate(0, plane_keys, luma_values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks),
        CFDictionaryCreate(0, plane_keys, chroma_values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    const void* keys[4] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfacePixelFormat, kIOSurfacePlaneInfo};
    const void* values[4] = {CFNumberCreate(0, 4, &width), CFNumberCreate(0, 4, &height), CFNumberCreate(0, 4, &format),
                             CFArrayCreate(0, planes, 2, &kCFTypeArrayCallBacks)};
    CFTypeRef properties = CFDictionaryCreate(0, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    for (int index = 0; index < 4; ++index) CFRelease(values[index]);
    for (int index = 0; index < 2; ++index) CFRelease(planes[index]);
    for (int index = 0; index < 3; ++index) { CFRelease(luma_values[index]); CFRelease(chroma_values[index]); }
    return surface;
}

static id descriptor(unsigned format, NSUInteger width, NSUInteger height, NSUInteger usage) {
    id value = ((id (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, int))objc_msgSend)(
        objc_getClass("MTLTextureDescriptor"), S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), format, width, height, 0);
    sendu(value, "setUsage:", usage);
    return value;
}

static id surface_texture(id device, unsigned format, NSUInteger width, NSUInteger height, CFTypeRef surface, NSUInteger plane) {
    return ((id (*)(id, SEL, id, CFTypeRef, NSUInteger))objc_msgSend)(device, S("newTextureWithDescriptor:iosurface:plane:"),
        descriptor(format, width, height, USAGE_SHADER_READ | USAGE_RENDER_TARGET), surface, plane);
}

/* A command buffer clearing `texture` to `color`, committed. */
static id clear(id queue, id texture, double red, double green, double blue) {
    id pass = send0(objc_getClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
    id attachment = sendu(send0(pass, "colorAttachments"), "objectAtIndexedSubscript:", 0);
    send1(attachment, "setTexture:", texture);
    sendu(attachment, "setLoadAction:", LOAD_CLEAR);
    sendu(attachment, "setStoreAction:", STORE_STORE);
    const ClearColor color = {red, green, blue, 1.0};
    ((void (*)(id, SEL, ClearColor))objc_msgSend)(attachment, S("setClearColor:"), color);
    id commands = send0(queue, "commandBuffer");
    id encoder = send1(commands, "renderCommandEncoderWithDescriptor:", pass);
    send0(encoder, "endEncoding");
    send0(commands, "commit");
    return commands;
}

static int finish(id commands) {
    send0(commands, "waitUntilCompleted");
    return value0(commands, "status") == STATUS_COMPLETED;
}

/* Every texel of a width x height BGRA surface is `texel`; the row padding
 * (when `padding`) still holds PADDING. */
static int surface_is(CFTypeRef surface, unsigned width, unsigned height, unsigned texel, int padding) {
    const unsigned long row = IOSurfaceGetBytesPerRow(surface);
    const unsigned char* base = (const unsigned char*)IOSurfaceGetBaseAddress(surface);
    for (unsigned y = 0; y < height; ++y) {
        const unsigned* line = (const unsigned*)(base + y * row);
        for (unsigned x = 0; x < width; ++x)
            if (line[x] != texel) return 0;
        if (padding)
            for (unsigned x = width; x < row / 4; ++x)
                if (line[x] != PADDING) return 0;
    }
    return 1;
}

/* Encodes a blit of texel (x, y) of `texture` to `buffer` + offset. */
static void blit_texel(id commands, id texture, unsigned x, unsigned y, id buffer, NSUInteger offset) {
    id blit = send0(commands, "blitCommandEncoder");
    const Triple origin = {x, y, 0}, size = {1, 1, 1};
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger, Triple, Triple, id, NSUInteger, NSUInteger, NSUInteger))objc_msgSend)(
        blit, S("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"),
        texture, 0, 0, origin, size, buffer, offset, 4, 4);
    send0(blit, "endEncoding");
}

/* Blit texel (x, y) of `texture` into a shared buffer and answer it. */
static unsigned gpu_texel(id device, id queue, id texture, unsigned x, unsigned y) {
    id buffer = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(device, S("newBufferWithLength:options:"), 256, 0);
    id commands = send0(queue, "commandBuffer");
    blit_texel(commands, texture, x, y, buffer, 0);
    send0(commands, "commit");
    if (!finish(commands)) return 0xdeadu;
    const unsigned value = *(const unsigned*)send0(buffer, "contents");
    send0(buffer, "release");
    return value;
}

static const char* kernel_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void texel(texture2d<float, access::read> source [[texture(0)]], device uint *out [[buffer(0)]],\n"
    "                  uint index [[thread_position_in_grid]]) {\n"
    "    float4 c = source.read(uint2(10 + index, 5));\n"
    "    uint4 b = uint4(c * 255.0 + 0.5);\n"
    "    out[index] = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);\n"
    "}\n";

/* Texels (10, 5) and (11, 5) read by a compute kernel, as packed RGBA words. */
static int shader_reads(id device, id queue, id texture, unsigned out[2]) {
    id error = 0;
    id library = ((id (*)(id, SEL, id, id, id*))objc_msgSend)(device, S("newLibraryWithSource:options:error:"), string(kernel_source), 0,
                                                             &error);
    if (!library) return 0;
    id function = send1(library, "newFunctionWithName:", string("texel"));
    id pipeline = ((id (*)(id, SEL, id, id*))objc_msgSend)(device, S("newComputePipelineStateWithFunction:error:"), function, &error);
    if (!pipeline) return 0;
    id buffer = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(device, S("newBufferWithLength:options:"), 64, 0);
    id commands = send0(queue, "commandBuffer");
    id encoder = send0(commands, "computeCommandEncoder");
    send1(encoder, "setComputePipelineState:", pipeline);
    ((void (*)(id, SEL, id, NSUInteger))objc_msgSend)(encoder, S("setTexture:atIndex:"), texture, 0);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))objc_msgSend)(encoder, S("setBuffer:offset:atIndex:"), buffer, 0, 0);
    const Triple groups = {1, 1, 1}, threads = {2, 1, 1};
    ((void (*)(id, SEL, Triple, Triple))objc_msgSend)(encoder, S("dispatchThreadgroups:threadsPerThreadgroup:"), groups, threads);
    send0(encoder, "endEncoding");
    send0(commands, "commit");
    if (!finish(commands)) return 0;
    const unsigned* words = (const unsigned*)send0(buffer, "contents");
    out[0] = words[0];
    out[1] = words[1];
    return 1;
}

static void write_texels(CFTypeRef surface) {
    IOSurfaceLock(surface, 0, 0);
    unsigned char* base = (unsigned char*)IOSurfaceGetBaseAddress(surface);
    unsigned* line = (unsigned*)(base + 5 * IOSurfaceGetBytesPerRow(surface));
    line[10] = 0x11223344u;   /* B 0x44 G 0x33 R 0x22 A 0x11 */
    line[11] = 0x80c04020u;
    IOSurfaceUnlock(surface, 0, 0);
}

static id archive(id object) {
    id error = 0;
    return ((id (*)(id, SEL, id, int, id*))objc_msgSend)(objc_getClass("NSKeyedArchiver"),
        S("archivedDataWithRootObject:requiringSecureCoding:error:"), object, 1, &error);
}

static id unarchive_handle(id data) {
    id error = 0;
    return ((id (*)(id, SEL, id, id, id*))objc_msgSend)(objc_getClass("NSKeyedUnarchiver"),
        S("unarchivedObjectOfClass:fromData:error:"), objc_getClass("MTLSharedEventHandle"), data, &error);
}

static int same_text(id a, const char* b) {
    const char* text = a ? (const char*)send0(a, "UTF8String") : 0;
    if (!text) return 0;
    while (*text && *text == *b) ++text, ++b;
    return *text == *b;
}

static int events(id device) {
    id event = send0(device, "newSharedEvent");
    if (!event) return 31;
    send1(event, "setLabel:", string("frames"));
    id handle = send0(event, "newSharedEventHandle");
    if (!handle || !same_text(send0(handle, "label"), "frames")) return 32;
    id data = archive(handle);
    if (!data || CFDataGetLength(data) <= 0) return 33;
    id decoded = unarchive_handle(data);
    if (!decoded || !same_text(send0(decoded, "label"), "frames")) return 34;
    id again = send1(device, "newSharedEventWithHandle:", decoded);
    if (again != event) return 35;   /* the same event while it lives */
    sendu(event, "setSignaledValue:", 7);
    if (value0(again, "signaledValue") != 7) return 36;
    send0(again, "release");
    send0(handle, "release");
    send0(event, "release");
    return 0;
}

static int local(int staged) {
    id device = MTLCreateSystemDefaultDevice();
    if (!device) { puts("metal iosurface: no Metal device (skipped)"); return 0; }
    id queue = send0(device, "newCommandQueue");
    /* 1. */
    CFTypeRef surface = make_surface(60, 32);
    if (!surface || IOSurfaceGetBytesPerRow(surface) != 256) return 1;
    {
        unsigned* words = (unsigned*)IOSurfaceGetBaseAddress(surface);
        for (unsigned index = 0; index < 256 * 32 / 4; ++index) words[index] = PADDING;
    }
    id texture = surface_texture(device, BGRA8_UNORM, 60, 32, surface, 0);
    if (!texture) return 2;
    if ((CFTypeRef)send0(texture, "iosurface") != surface || value0(texture, "iosurfacePlane") != 0) return 3;
    if (value0(texture, "width") != 60 || value0(texture, "height") != 32 || value0(texture, "pixelFormat") != BGRA8_UNORM) return 4;
    /* 2. */
    const unsigned seed = IOSurfaceGetSeed(surface);
    if (!finish(clear(queue, texture, 0, 1, 0))) return 5;
    if (!surface_is(surface, 60, 32, GREEN, 1)) return 6;
    if (IOSurfaceGetSeed(surface) == seed) return 7;
    /* 3. */
    id pending = clear(queue, texture, 0, 0, 1);
    if (IOSurfaceLock(surface, LOCK_READ_ONLY, 0) != 0) return 8;
    const int blue = surface_is(surface, 60, 32, BLUE, 1);
    IOSurfaceUnlock(surface, LOCK_READ_ONLY, 0);
    if (!blue) return 9;
    if (!finish(pending)) return 10;
    /* 4. */
    write_texels(surface);
    if (gpu_texel(device, queue, texture, 10, 5) != 0x11223344u || gpu_texel(device, queue, texture, 11, 5) != 0x80c04020u) return 11;
    if (gpu_texel(device, queue, texture, 12, 5) != BLUE) return 12;
    unsigned read[2] = {0, 0};
    if (!shader_reads(device, queue, texture, read)) return 13;
    if (read[0] != 0x11443322u || read[1] != 0x802040c0u) return 14;
    /* 5. */
    id view = sendu(texture, "newTextureViewWithPixelFormat:", BGRA8_UNORM);
    if (!view || (CFTypeRef)send0(view, "iosurface") != surface) return 15;
    if (!finish(clear(queue, view, 1, 0, 0))) return 16;
    if (!surface_is(surface, 60, 32, RED, 1)) return 17;
    /* 6. */
    if (surface_texture(device, BGRA8_UNORM, 60, 32, surface, 1)) return 18;
    if (surface_texture(device, RGBA16_FLOAT, 60, 32, surface, 0)) return 19;
    if (surface_texture(device, BGRA8_UNORM, 61, 32, surface, 0)) return 20;
    /* 7. */
    const int failed = events(device);
    if (failed) return failed;
    /* 8. */
    id other = surface_texture(device, BGRA8_UNORM, 60, 32, surface, 0);
    if (!other || gpu_texel(device, queue, other, 3, 3) != RED) return 21;   /* `other` now matches the pages */
    id pending_write = clear(queue, texture, 1, 1, 0);                       /* not waited for */
    if (gpu_texel(device, queue, other, 3, 3) != YELLOW) return 22;
    if (!finish(pending_write)) return 23;
    /* 9. */
    if (!staged) {
        id event = send0(device, "newSharedEvent");
        id buffer = ((id (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(device, S("newBufferWithLength:options:"), 256, 0);
        id commands = send0(queue, "commandBuffer");
        blit_texel(commands, other, 4, 4, buffer, 0);
        ((void (*)(id, SEL, id, u64))objc_msgSend)(commands, S("encodeWaitForEvent:value:"), event, 1);
        blit_texel(commands, other, 4, 4, buffer, 4);
        send0(commands, "commit");
        IOSurfaceLock(surface, 0, 0);
        ((unsigned*)((unsigned char*)IOSurfaceGetBaseAddress(surface) + 4 * IOSurfaceGetBytesPerRow(surface)))[4] = 0x01020304u;
        IOSurfaceUnlock(surface, 0, 0);
        sendu(event, "setSignaledValue:", 1);
        if (!finish(commands)) return 24;
        const unsigned* words = (const unsigned*)send0(buffer, "contents");
        if (words[1] != 0x01020304u) return 25;
        send0(buffer, "release");
        send0(event, "release");
    }
    send0(other, "release");
    /* 10. */
    {
        CFTypeRef transient = make_surface(1184, 1069);
        id gone = transient ? surface_texture(device, BGRA8_UNORM, 1184, 1069, transient, 0) : 0;
        if (!gone) return 26;
        id inflight[4];
        for (int index = 0; index < 4; ++index) inflight[index] = clear(queue, gone, 0.25 * index, 0, 1);
        send0(gone, "release");
        CFRelease(transient);
        for (int index = 0; index < 4; ++index)
            if (!finish(inflight[index])) return 27;
        CFTypeRef next = make_surface(1184, 1069);
        id fresh = next ? surface_texture(device, BGRA8_UNORM, 1184, 1069, next, 0) : 0;
        if (!fresh || !finish(clear(queue, fresh, 0, 1, 0))) return 28;
        IOSurfaceLock(next, LOCK_READ_ONLY, 0);
        const int green = surface_is(next, 1184, 1069, GREEN, 0);
        IOSurfaceUnlock(next, LOCK_READ_ONLY, 0);
        if (!green) return 29;
        send0(fresh, "release");
        CFRelease(next);
    }
    /* 11. */
    {
        CFTypeRef planar = make_planar_surface(40, 20);
        if (!planar) return 37;
        const unsigned long luma_row = IOSurfaceGetBytesPerRowOfPlane(planar, 0), chroma_row = IOSurfaceGetBytesPerRowOfPlane(planar, 1);
        IOSurfaceLock(planar, 0, 0);
        unsigned char* luma = (unsigned char*)IOSurfaceGetBaseAddressOfPlane(planar, 0);
        unsigned char* chroma = (unsigned char*)IOSurfaceGetBaseAddressOfPlane(planar, 1);
        for (unsigned y = 0; y < 20; ++y)
            for (unsigned x = 0; x < 40; ++x) luma[y * luma_row + x] = (unsigned char)(0x10 + y);
        for (unsigned y = 0; y < 10; ++y)
            for (unsigned x = 0; x < 20; ++x) {
                chroma[y * chroma_row + 2 * x] = 0x80;
                chroma[y * chroma_row + 2 * x + 1] = (unsigned char)(0x20 + x);
            }
        IOSurfaceUnlock(planar, 0, 0);
        id luma_texture = surface_texture(device, R8_UNORM, 40, 20, planar, 0);
        id chroma_texture = surface_texture(device, RG8_UNORM, 20, 10, planar, 1);
        if (!luma_texture || !chroma_texture) return 38;
        if (value0(chroma_texture, "iosurfacePlane") != 1) return 39;
        if ((gpu_texel(device, queue, luma_texture, 3, 5) & 0xffu) != 0x15u) return 40;
        if ((gpu_texel(device, queue, chroma_texture, 7, 4) & 0xffffu) != ((0x20u + 7) << 8 | 0x80u)) return 41;
        if (!finish(clear(queue, chroma_texture, 0.25, 0.75, 0))) return 42;
        IOSurfaceLock(planar, LOCK_READ_ONLY, 0);
        int cleared = 1;
        for (unsigned y = 0; y < 10; ++y)
            for (unsigned x = 0; x < 20; ++x)
                cleared &= chroma[y * chroma_row + 2 * x] == 0x40 && chroma[y * chroma_row + 2 * x + 1] == 0xbf;
        const int luma_kept = luma[19 * luma_row + 39] == 0x10 + 19;
        IOSurfaceUnlock(planar, LOCK_READ_ONLY, 0);
        if (!cleared) return 43;
        if (!luma_kept) return 44;
        if (surface_texture(device, R8_UNORM, 40, 20, planar, 2)) return 45;
        if (surface_texture(device, RG8_UNORM, 21, 10, planar, 1)) return 46;   /* larger than the chroma plane */
        send0(chroma_texture, "release");
        send0(luma_texture, "release");
        CFRelease(planar);
    }
    send0(view, "release");
    send0(texture, "release");
    CFRelease(surface);
    puts("metal iosurface: ok");
    return 0;
}

/* ---- two processes ---------------------------------------------------------------------- */

typedef struct { unsigned bits, size; mach_port_t remote, local, voucher; int id; } header_t;
typedef struct { mach_port_t name; unsigned pad1; unsigned short pad2; unsigned char disposition, type; } port_t;
typedef struct { header_t header; unsigned value; } small_t;
#pragma pack(push, 4)
typedef struct { header_t header; unsigned count; port_t port; unsigned id; unsigned length; unsigned char bytes[2048]; } surface_t;
#pragma pack(pop)
typedef struct { header_t header; unsigned words[640]; } buffer_t;

static void put_header(header_t* header, unsigned bits, unsigned size, mach_port_t remote, mach_port_t local, int id) {
    header->bits = bits;
    header->size = size;
    header->remote = remote;
    header->local = local;
    header->voucher = 0;
    header->id = id;
}

/* Sends `id` (value `value`) to `server` and waits for the reply. */
static int ask(mach_port_t server, mach_port_t reply, int id, unsigned value, buffer_t* answer) {
    union { small_t request; buffer_t answer; } message;
    put_header(&message.request.header, COPY_SEND | (MAKE_SEND_ONCE << 8), sizeof(small_t), server, reply, id);
    message.request.value = value;
    if (mach_msg(&message, SEND_MSG | RCV_MSG | RCV_TIMEOUT, sizeof(small_t), sizeof(buffer_t), reply, 30000, 0) != 0) return 0;
    *answer = message.answer;
    return 1;
}

static int answer(mach_port_t to, int id) {
    small_t message;
    put_header(&message.header, MOVE_SEND_ONCE, sizeof(message), to, 0, id);
    message.value = 0;
    return mach_msg(&message, SEND_MSG, sizeof(message), 0, 0, 0, 0) == 0;
}

/* The GPU process. */
static int gpu_child(const char* service) {
    mach_port_t server = 0, reply = 0;
    if (bootstrap_look_up(bootstrap_port, service, &server) != 0 || !server) return 41;
    if (mach_port_allocate(mach_task_self_, RIGHT_RECEIVE, &reply) != 0) return 42;
    buffer_t message;
    if (!ask(server, reply, 200, 0, &message)) return 43;
    const surface_t* got = (const surface_t*)&message;
    if (got->header.id != 201 || !(got->header.bits & COMPLEX) || got->port.disposition != PORT_SEND) return 44;
    /* `message` is reused below: keep what the reply carried. */
    const mach_port_t port = got->port.name;
    if (got->length > sizeof got->bytes) return 55;
    id data = (id)CFDataCreate(0, got->bytes, got->length);
    CFTypeRef surface = IOSurfaceLookupFromMachPort(port);
    if (!surface) return 45;
    id device = MTLCreateSystemDefaultDevice();
    if (!device) return 46;
    id queue = send0(device, "newCommandQueue");
    id texture = surface_texture(device, BGRA8_UNORM, 60, 32, surface, 0);
    if (!texture) return 47;
    /* Clear yellow and tell the browser before the GPU is done. */
    id commands = clear(queue, texture, 1, 1, 0);
    if (!ask(server, reply, 202, 0, &message) || message.header.id != 203) return 48;
    if (!finish(commands)) return 49;
    /* The browser wrote texels meanwhile: the next GPU read sees them. */
    if (gpu_texel(device, queue, texture, 10, 5) != 0x11223344u || gpu_texel(device, queue, texture, 12, 5) != YELLOW) return 50;
    if (!ask(server, reply, 204, 0, &message) || message.header.id != 205) return 51;
    /* The event phase: wait for 1 on the GPU, clear cyan, signal 2. */
    id event = send1(device, "newSharedEventWithHandle:", unarchive_handle(data));
    if (!event) return 52;
    id pass = send0(objc_getClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
    id attachment = sendu(send0(pass, "colorAttachments"), "objectAtIndexedSubscript:", 0);
    send1(attachment, "setTexture:", texture);
    sendu(attachment, "setLoadAction:", LOAD_CLEAR);
    sendu(attachment, "setStoreAction:", STORE_STORE);
    const ClearColor cyan = {0, 1, 1, 1};
    ((void (*)(id, SEL, ClearColor))objc_msgSend)(attachment, S("setClearColor:"), cyan);
    id waiting = send0(queue, "commandBuffer");
    ((void (*)(id, SEL, id, u64))objc_msgSend)(waiting, S("encodeWaitForEvent:value:"), event, 1);
    send0(send1(waiting, "renderCommandEncoderWithDescriptor:", pass), "endEncoding");
    ((void (*)(id, SEL, id, u64))objc_msgSend)(waiting, S("encodeSignalEvent:value:"), event, 2);
    send0(waiting, "commit");
    if (!ask(server, reply, 206, 0, &message) || message.header.id != 207) return 53;
    if (!finish(waiting)) return 54;
    send0(event, "release");
    send0(texture, "release");
    CFRelease(surface);
    mach_port_deallocate(mach_task_self_, port);
    return 0;
}

static char* decimal(char* out, unsigned long long value) {
    char digits[24];
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

static void pause_ms(unsigned milliseconds) {
    timebase_t timebase = {1, 1};
    mach_timebase_info(&timebase);
    if (!timebase.numer) timebase.numer = timebase.denom = 1;
    const u64 ticks = (u64)milliseconds * 1000000u * timebase.denom / timebase.numer;
    const u64 start = mach_absolute_time();
    while (mach_absolute_time() - start < ticks) {}
}

static int cross(const char* program) {
    char service[64] = "org.alchemy.test.metal-iosurface.";
    decimal(service + 33, (unsigned)getpid());
    mach_port_t server = 0;
    if (bootstrap_check_in(bootstrap_port, service, &server) != 0) return 61;
    CFTypeRef surface = make_surface(60, 32);
    if (!surface) return 62;
    id device = MTLCreateSystemDefaultDevice();
    if (!device) return 79;
    id event = send0(device, "newSharedEvent");
    id handle = event ? send0(event, "newSharedEventHandle") : 0;
    id data = handle ? archive(handle) : 0;
    if (!data || CFDataGetLength(data) > 2048) return 80;
    const mach_port_t port = IOSurfaceCreateMachPort(surface);
    if (!port) return 63;
    char* arguments[] = {(char*)program, "gpu", service, 0};
    pid_t pid = 0;
    if (posix_spawn(&pid, program, 0, 0, arguments, environ) != 0 || pid <= 0) return 64;
    buffer_t buffer;
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 30000, 0) != 0 || buffer.header.id != 200) return 65;
    surface_t reply;
    put_header(&reply.header, COMPLEX | MOVE_SEND_ONCE, sizeof(reply), buffer.header.remote, 0, 201);
    reply.count = 1;
    reply.port.name = port;
    reply.port.pad1 = 0;
    reply.port.pad2 = 0;
    reply.port.disposition = COPY_SEND;
    reply.port.type = 0;
    reply.id = 0;
    reply.length = (unsigned)CFDataGetLength(data);
    {
        const unsigned char* bytes = CFDataGetBytePtr(data);
        for (unsigned index = 0; index < reply.length; ++index) reply.bytes[index] = bytes[index];
    }
    if (mach_msg(&reply, SEND_MSG, sizeof(reply), 0, 0, 0, 0) != 0) return 66;
    /* The child committed its clear: IOSurfaceLock waits for it. */
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 30000, 0) != 0 || buffer.header.id != 202) return 67;
    if (IOSurfaceLock(surface, LOCK_READ_ONLY, 0) != 0) return 68;
    const int yellow = surface_is(surface, 60, 32, YELLOW, 0);
    IOSurfaceUnlock(surface, LOCK_READ_ONLY, 0);
    if (!yellow) return 69;
    write_texels(surface);
    if (!answer(buffer.header.remote, 203)) return 70;
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 30000, 0) != 0 || buffer.header.id != 204) return 71;
    if (!answer(buffer.header.remote, 205)) return 72;
    /* The event phase: the child's GPU waits for this process's signal. */
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 30000, 0) != 0 || buffer.header.id != 206) return 75;
    pause_ms(50);
    /* Not drawn yet (read without IOSurfaceLock, which would wait for it). */
    if (surface_is(surface, 60, 32, CYAN, 0) || value0(event, "signaledValue") != 0) return 76;
    const u64 signalled = mach_absolute_time();
    sendu(event, "setSignaledValue:", 1);
    if (!((int (*)(id, SEL, u64, u64))objc_msgSend)(event, S("waitUntilSignaledValue:timeoutMS:"), 2, 10000)) return 77;
    /* The round trip (this CPU signal to the child's GPU wait, its clear and
     * GPU signal, back to this wait) crosses processes twice: with the
     * bridges' wake-ups it is far below their 250 ms fallback timer. */
    u64 round_trip = mach_absolute_time() - signalled;
    {
        timebase_t timebase = {1, 1};
        mach_timebase_info(&timebase);
        if (timebase.denom) round_trip = round_trip * timebase.numer / timebase.denom / 1000000u;   /* ms */
        char text[96] = "metal iosurface: cross event round trip ms ";
        char* at = text;
        while (*at) ++at;
        decimal(at, round_trip);
        puts(text);
    }
    if (round_trip >= 1000) return 87;
    if (IOSurfaceLock(surface, LOCK_READ_ONLY, 0) != 0) return 68;
    const int cyan = surface_is(surface, 60, 32, CYAN, 0);
    IOSurfaceUnlock(surface, LOCK_READ_ONLY, 0);
    if (!cyan) return 78;
    if (!answer(buffer.header.remote, 207)) return 72;
    int status = -1;
    if (waitpid(pid, &status, 0) != pid) return 73;
    if (status != 0) return 74 + 0 * status;
    send0(handle, "release");
    send0(event, "release");
    CFRelease(surface);
    mach_port_deallocate(mach_task_self_, port);
    puts("metal iosurface: cross: ok");
    return 0;
}

/* ---- cost per frame ---------------------------------------------------------------------- */

static int bench(const char* count) {
    unsigned frames = 0;
    while (*count >= '0' && *count <= '9') frames = frames * 10 + (unsigned)(*count++ - '0');
    if (!frames) frames = 60;
    id device = MTLCreateSystemDefaultDevice();
    if (!device) return 81;
    id queue = send0(device, "newCommandQueue");
    CFTypeRef surface = make_surface(1184, 1069);
    id backed = surface_texture(device, BGRA8_UNORM, 1184, 1069, surface, 0);
    id plain = ((id (*)(id, SEL, id))objc_msgSend)(device, S("newTextureWithDescriptor:"),
                                                   descriptor(BGRA8_UNORM, 1184, 1069, USAGE_SHADER_READ | USAGE_RENDER_TARGET));
    if (!surface || !backed || !plain) return 82;
    /* Warm-up (pipeline creation, the import, first transitions). */
    if (!finish(clear(queue, plain, 0, 0, 0)) || !finish(clear(queue, backed, 0, 0, 0))) return 83;
    u64 plain_ticks = 0, backed_ticks = 0;
    for (unsigned frame = 0; frame < frames; ++frame) {
        const double shade = (double)(frame % 16) / 16.0;
        u64 start = mach_absolute_time();
        if (!finish(clear(queue, plain, shade, 0, 0))) return 84;
        u64 middle = mach_absolute_time();
        /* A CPU write each frame as well, so every frame uploads too. */
        write_texels(surface);
        if (!finish(clear(queue, backed, shade, 0, 0))) return 85;
        u64 end = mach_absolute_time();
        plain_ticks += middle - start;
        backed_ticks += end - middle;
    }
    timebase_t timebase = {1, 1};
    if (mach_timebase_info(&timebase) != 0 || !timebase.denom) return 86;
    plain_ticks = plain_ticks * timebase.numer / timebase.denom;   /* nanoseconds */
    backed_ticks = backed_ticks * timebase.numer / timebase.denom;
    char text[160] = "metal iosurface: bench 1184x1069 frames ";
    char* at = text;
    while (*at) ++at;
    at = decimal(at, frames);
    const char* part = " plain_us ";
    while (*part) *at++ = *part++;
    at = decimal(at, plain_ticks / frames / 1000);
    part = " backed_us ";
    while (*part) *at++ = *part++;
    at = decimal(at, backed_ticks / frames / 1000);
    puts(text);
    return 0;
}

/* ---- GPU-process frames into a browser window -------------------------------------------- */

typedef struct { double x, y, width, height; } Rect;
extern void* fopen(const char*, const char*);
extern unsigned long fread(void*, unsigned long, unsigned long, void*);
extern int fseek(void*, long, int);
extern int fclose(void*);
extern int unlink(const char*);

#define MAGENTA 0xffff00ffu

static char* append(char* out, const char* text) {
    while (*text) *out++ = *text++;
    *out = 0;
    return out;
}

/* The centre pixel of the capture of window `number` in `directory` (guest
 * path), as a little-endian BGRA word; 0 when there is none. */
static unsigned capture_centre(const char* directory, unsigned long number) {
    char path[512];
    char* at = append(path, directory);
    at = append(at, "/window-");
    at = decimal(at, number);
    append(at, ".bmp");
    void* file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char header[54];
    unsigned value = 0;
    if (fread(header, 1, sizeof header, file) == sizeof header && header[0] == 'B' && header[1] == 'M') {
        const int width = (int)(header[18] | header[19] << 8 | header[20] << 16 | (unsigned)header[21] << 24);
        int height = (int)(header[22] | header[23] << 8 | header[24] << 16 | (unsigned)header[25] << 24);
        if (height < 0) height = -height;   /* top-down */
        const long offset = 54 + ((long)(height / 2) * width + width / 2) * 4;
        unsigned char pixel[4];
        if (width > 0 && fseek(file, offset, 0) == 0 && fread(pixel, 1, 4, file) == 4)
            value = pixel[0] | pixel[1] << 8 | pixel[2] << 16 | (unsigned)pixel[3] << 24;
    }
    fclose(file);
    return value;
}

static void forget_capture(const char* directory, unsigned long number) {
    char path[512];
    char* at = append(path, directory);
    at = append(at, "/window-");
    at = decimal(at, number);
    append(at, ".bmp");
    unlink(path);
}

/* The GPU process: renders the browser's surface on the GPU and reports each
 * frame as soon as it is committed, before it completes (Chrome hands its
 * frames over once scheduled). */
static int present_gpu(const char* service) {
    mach_port_t server = 0, reply = 0;
    if (bootstrap_look_up(bootstrap_port, service, &server) != 0 || !server) return 141;
    if (mach_port_allocate(mach_task_self_, RIGHT_RECEIVE, &reply) != 0) return 142;
    buffer_t message;
    if (!ask(server, reply, 300, 0, &message)) return 143;
    const surface_t* got = (const surface_t*)&message;
    if (got->header.id != 301 || !(got->header.bits & COMPLEX) || got->port.disposition != PORT_SEND) return 144;
    const mach_port_t port = got->port.name;
    CFTypeRef surface = IOSurfaceLookupFromMachPort(port);
    id device = MTLCreateSystemDefaultDevice();
    if (!surface || !device) return 145;
    id queue = send0(device, "newCommandQueue");
    id texture = surface_texture(device, BGRA8_UNORM, 120, 80, surface, 0);
    if (!texture) return 146;
    id first = clear(queue, texture, 1, 0, 1);   /* magenta */
    if (!ask(server, reply, 302, 0, &message) || message.header.id != 303) return 147;
    if (!finish(first)) return 148;
    id second = clear(queue, texture, 0, 1, 1);  /* cyan */
    if (!ask(server, reply, 304, 0, &message) || message.header.id != 305) return 149;
    if (!finish(second)) return 150;
    send0(texture, "release");
    CFRelease(surface);
    mach_port_deallocate(mach_task_self_, port);
    return 0;
}

/* The browser: the GPU process's surface as a CALayer's contents in a
 * (cloaked) window; the composition, read back from the contents capture,
 * shows what the GPU process rendered. */
static int present(const char* program, const char* captures) {
    char service[64] = "org.alchemy.test.metal-present.";
    decimal(service + 31, (unsigned)getpid());
    mach_port_t server = 0;
    if (bootstrap_check_in(bootstrap_port, service, &server) != 0) return 121;
    id app = send0(objc_getClass("NSApplication"), "sharedApplication");
    send0(app, "finishLaunching");
    const Rect content = {100, 100, 120, 80}, bounds = {0, 0, 120, 80};
    id window = ((id (*)(id, SEL, Rect, NSUInteger, NSUInteger, int))objc_msgSend)(
        send0(objc_getClass("NSWindow"), "alloc"), S("initWithContentRect:styleMask:backing:defer:"), content, 1, 2, 0);
    if (!window) return 122;
    sendu(window, "setReleasedWhenClosed:", 0);
    id view = ((id (*)(id, SEL, Rect))objc_msgSend)(send0(objc_getClass("NSView"), "alloc"), S("initWithFrame:"), bounds);
    sendu(view, "setWantsLayer:", 1);
    send1(window, "setContentView:", view);
    send1(window, "makeKeyAndOrderFront:", 0);
    const unsigned long number = value0(window, "windowNumber");
    forget_capture(captures, number);
    id root = send0(view, "layer");
    if (!root) return 123;
    CFTypeRef surface = make_surface(120, 80);
    const mach_port_t port = surface ? IOSurfaceCreateMachPort(surface) : 0;
    if (!port) return 124;
    char* arguments[] = {(char*)program, "present-gpu", service, 0};
    pid_t pid = 0;
    if (posix_spawn(&pid, program, 0, 0, arguments, environ) != 0 || pid <= 0) return 125;
    buffer_t buffer;
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 30000, 0) != 0 || buffer.header.id != 300) return 126;
    surface_t reply;
    put_header(&reply.header, COMPLEX | MOVE_SEND_ONCE, sizeof(reply), buffer.header.remote, 0, 301);
    reply.count = 1;
    reply.port.name = port;
    reply.port.pad1 = 0;
    reply.port.pad2 = 0;
    reply.port.disposition = COPY_SEND;
    reply.port.type = 0;
    reply.id = 0;
    reply.length = 0;
    if (mach_msg(&reply, SEND_MSG, sizeof(reply), 0, 0, 0, 0) != 0) return 127;
    /* Frame 1: committed by the GPU process, maybe not completed yet. */
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 30000, 0) != 0 || buffer.header.id != 302) return 128;
    id layer = send0(send0(objc_getClass("CALayer"), "alloc"), "init");
    ((void (*)(id, SEL, Rect))objc_msgSend)(layer, S("setFrame:"), bounds);
    send0(objc_getClass("CATransaction"), "begin");
    send1(root, "addSublayer:", layer);
    send1(layer, "setContents:", (id)surface);
    send0(objc_getClass("CATransaction"), "commit");
    const unsigned first = capture_centre(captures, number);
    if (first != MAGENTA) return 129;
    if (!answer(buffer.header.remote, 303)) return 130;
    /* Frame 2: the same surface drawn again, then -setContentsChanged. */
    if (mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof(buffer), server, 30000, 0) != 0 || buffer.header.id != 304) return 131;
    send0(objc_getClass("CATransaction"), "begin");
    send0(layer, "setContentsChanged");
    send0(objc_getClass("CATransaction"), "commit");
    const unsigned second = capture_centre(captures, number);
    if (second != CYAN) return 132;
    if (!answer(buffer.header.remote, 305)) return 133;
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || status != 0) return 134;
    send1(window, "orderOut:", 0);
    send0(layer, "release");
    CFRelease(surface);
    mach_port_deallocate(mach_task_self_, port);
    puts("metal iosurface: present: ok");
    return 0;
}

static int idle(const char* seconds_text, int bridged) {
    unsigned seconds = 0;
    while (*seconds_text >= '0' && *seconds_text <= '9') seconds = seconds * 10 + (unsigned)(*seconds_text++ - '0');
    id device = MTLCreateSystemDefaultDevice();
    if (!device) return 91;
    id event = send0(device, "newSharedEvent");
    id handle = bridged ? send0(event, "newSharedEventHandle") : 0;
    if (!event || (bridged && !handle)) return 92;
    mach_port_t port = 0;
    if (mach_port_allocate(mach_task_self_, RIGHT_RECEIVE, &port) != 0) return 93;
    buffer_t buffer;
    /* Nothing is ever sent: the receive times out after `seconds`. */
    (void)mach_msg(&buffer, RCV_MSG | RCV_TIMEOUT, 0, sizeof buffer, port, seconds * 1000u, 0);
    puts("metal iosurface: idle done");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && same(argv[1], "idle")) return idle(argv[2], same(argv[3], "bridge"));
    if (argc == 3 && same(argv[1], "present")) return present(argv[0], argv[2]);
    if (argc == 3 && same(argv[1], "present-gpu")) return present_gpu(argv[2]);
    if (argc == 3 && same(argv[1], "gpu")) return gpu_child(argv[2]);
    if (argc == 2 && same(argv[1], "cross")) return cross(argv[0]);
    if (argc >= 2 && same(argv[1], "bench")) return bench(argc >= 3 ? argv[2] : "60");
    return local(argc == 2 && same(argv[1], "staged"));
}
