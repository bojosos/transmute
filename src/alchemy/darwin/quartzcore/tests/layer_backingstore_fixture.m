#include "../../../../../tests/fixtures/macos/gui_fixture.h"
#include <QuartzCore/QuartzCore.h>
/* Qt-free fixture of Qt 6's Cocoa backing-store pattern (QNSView +
 * QCALayerBackingStore + QCocoaScreen's display link), project-authored:
 *
 *  - a layer-backed view (wantsLayer YES) that draws through -updateLayer
 *    (wantsUpdateLayer YES), is flipped, and keeps -drawRect: for the
 *    non-layer path like QNSView;
 *  - two IOSurface back buffers (IOSurfaceCreate with Qt's property set,
 *    aligned bytes per row and alloc size, 'BGRA'), painted under
 *    IOSurfaceLock/Unlock and flushed with layer.contents = surface,
 *    switching buffers when the front one IsInUse;
 *  - a CVDisplayLink for the window's display whose callback (on the link
 *    thread) marshals to the main queue through a DATA_ADD dispatch source,
 *    where each delivered "update request" paints and flushes a new frame.
 *
 * The painted frame is a left half red, a right half blue and a white bar
 * at the top whose width grows with the frame number, so a capture shows
 * which frame was presented. Output: start, surface <w>x<h> bpr=<n>
 * alloc=<n>, updateLayer <n>, display link <hz>, frames <n> ticks <n>,
 * pixel probes from the last back buffer, done.
 *
 *   layer_backingstore_fixture [--frames <n>]   (default 30) */

typedef struct __IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceLockOptions;
extern const CFStringRef kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow,
    kIOSurfaceAllocSize, kIOSurfacePixelFormat;
IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
size_t IOSurfaceAlignProperty(CFStringRef property, size_t value);
int IOSurfaceLock(IOSurfaceRef surface, IOSurfaceLockOptions options, uint32_t *seed);
int IOSurfaceUnlock(IOSurfaceRef surface, IOSurfaceLockOptions options, uint32_t *seed);
void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef surface);
size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
size_t IOSurfaceGetWidth(IOSurfaceRef surface);
size_t IOSurfaceGetHeight(IOSurfaceRef surface);
Boolean IOSurfaceIsInUse(IOSurfaceRef surface);
void IOSurfaceSetValue(IOSurfaceRef surface, CFStringRef key, CFTypeRef value);

typedef struct __CVDisplayLink *CVDisplayLinkRef;
typedef int32_t CVReturn;
typedef uint64_t CVOptionFlags;
typedef struct {
    uint32_t version; int32_t videoTimeScale; int64_t videoTime; uint64_t hostTime; double rateScalar;
    int64_t videoRefreshPeriod; char smpte[24]; uint64_t flags; uint64_t reserved;
} CVTimeStamp;
typedef struct { int64_t timeValue; int32_t timeScale; int32_t flags; } CVTime;
typedef CVReturn (*CVDisplayLinkOutputCallback)(CVDisplayLinkRef, const CVTimeStamp *, const CVTimeStamp *, CVOptionFlags,
                                                CVOptionFlags *, void *);
CVReturn CVDisplayLinkCreateWithCGDisplay(CGDirectDisplayID display, CVDisplayLinkRef *link);
CVReturn CVDisplayLinkSetOutputCallback(CVDisplayLinkRef link, CVDisplayLinkOutputCallback callback, void *context);
CVReturn CVDisplayLinkStart(CVDisplayLinkRef link);
CVReturn CVDisplayLinkStop(CVDisplayLinkRef link);
Boolean CVDisplayLinkIsRunning(CVDisplayLinkRef link);
CVTime CVDisplayLinkGetNominalOutputVideoRefreshPeriod(CVDisplayLinkRef link);
void CVDisplayLinkRelease(CVDisplayLinkRef link);

typedef struct dispatch_source_s *dispatch_source_t;
typedef struct dispatch_queue_s *dispatch_queue_t;
typedef const struct dispatch_source_type_s *dispatch_source_type_t;
extern const struct dispatch_source_type_s _dispatch_source_type_data_add;
extern struct dispatch_queue_s _dispatch_main_q;
dispatch_source_t dispatch_source_create(dispatch_source_type_t type, uintptr_t handle, unsigned long mask, dispatch_queue_t queue);
void dispatch_set_context(void *object, void *context);
void dispatch_source_set_event_handler_f(dispatch_source_t source, void (*handler)(void *));
void dispatch_resume(void *object);
void dispatch_source_merge_data(dispatch_source_t source, unsigned long value);
unsigned long dispatch_source_get_data(dispatch_source_t source);

@interface CALayer (QtBackingStoreFixture)
- (void)setContents:(id)contents;
- (id)contents;
@end

static IOSurfaceRef buffers[2];
static int back;                  /* index of the back buffer */
static int frames_painted;
static int update_layer_calls;
static volatile int link_ticks;   /* incremented on the display-link thread */
static int requests_delivered;
static NSView *the_view;
static CGFloat scale = 1;

static IOSurfaceRef create_surface(size_t width, size_t height) {
    const int32_t bytes_per_element = 4;
    const size_t bytes_per_row = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * bytes_per_element);
    const size_t total = IOSurfaceAlignProperty(kIOSurfaceAllocSize, height * bytes_per_row);
    const int32_t format = 0x42475241; /* 'BGRA' */
    int64_t values[6] = {(int64_t)width, (int64_t)height, bytes_per_element, (int64_t)bytes_per_row, (int64_t)total, format};
    const void *keys[6] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow,
                           kIOSurfaceAllocSize, kIOSurfacePixelFormat};
    const void *numbers[6];
    for (int index = 0; index < 6; ++index) numbers[index] = CFNumberCreate(0, kCFNumberSInt64Type, &values[index]);
    CFDictionaryRef properties = CFDictionaryCreate(0, keys, numbers, 6, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
    for (int index = 0; index < 6; ++index) CFRelease(numbers[index]);
    IOSurfaceRef surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    return surface;
}

/* QCALayerBackingStore::beginPaint/endPaint/flush. */
static void paint_and_flush(void) {
    const NSRect bounds = [the_view bounds];
    const size_t width = (size_t)(bounds.size.width * scale), height = (size_t)(bounds.size.height * scale);
    for (int index = 0; index < 2; ++index) {
        if (buffers[index] && (IOSurfaceGetWidth(buffers[index]) != width || IOSurfaceGetHeight(buffers[index]) != height)) {
            CFRelease(buffers[index]);
            buffers[index] = 0;
        }
        if (!buffers[index]) {
            buffers[index] = create_surface(width, height);
            if (!buffers[index]) { gui_log("IOSurfaceCreate failed"); return; }
            if (index == 0)
                gui_log("surface %zux%zu bpr=%zu alloc=%zu", width, height, IOSurfaceGetBytesPerRow(buffers[0]),
                        IOSurfaceGetAllocSize(buffers[0]));
        }
    }
    if (IOSurfaceIsInUse(buffers[back])) back ^= 1;
    IOSurfaceRef surface = buffers[back];
    uint32_t seed = 0;
    IOSurfaceLock(surface, 0, &seed);
    uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(surface);
    const size_t stride = IOSurfaceGetBytesPerRow(surface);
    const size_t bar = (size_t)(frames_painted + 1) * width / 64;
    for (size_t y = 0; y < height; ++y) {
        uint32_t *row = (uint32_t *)(base + y * stride);
        for (size_t x = 0; x < width; ++x) {
            uint32_t pixel = x < width / 2 ? 0xffff0000u : 0xff0000ffu;   /* ARGB in a little-endian word = B,G,R,A bytes */
            if (y < height / 8 && x < bar) pixel = 0xffffffffu;
            row[x] = pixel;
        }
    }
    IOSurfaceUnlock(surface, 0, &seed);
    [[the_view layer] setContents:(id)surface];
    ++frames_painted;
}

@interface QtLikeView : NSView
@end
@implementation QtLikeView
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return [super makeBackingLayer]; }
- (void)updateLayer {
    ++update_layer_calls;
    paint_and_flush();
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    gui_log("drawRect: called on a layer-backed view");
}
@end

static dispatch_source_t update_source;

static void deliver_update_requests(void *context) {
    (void)context;
    ++requests_delivered;
    paint_and_flush();
}

static CVReturn display_link_callback(CVDisplayLinkRef link, const CVTimeStamp *now, const CVTimeStamp *output,
                                      CVOptionFlags flags, CVOptionFlags *flags_out, void *context) {
    (void)link; (void)output; (void)flags; (void)flags_out; (void)context;
    if (!now || !(now->flags & 2)) return -6660;
    __atomic_add_fetch(&link_ticks, 1, __ATOMIC_SEQ_CST);
    dispatch_source_merge_data(update_source, 1);
    return 0;
}

int main(int argc, char **argv) {
    gui_fixture_name = "layer_backingstore_fixture";
    const long frames = gui_argument_value(argc, argv, "--frames", 30);
    gui_log("start");
    NSApplication *application = [NSApplication sharedApplication];
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application finishLaunching];
    QtLikeView *view = [[QtLikeView alloc] initWithFrame:NSMakeRect(0, 0, 320, 200)];
    the_view = view;
    [view setWantsLayer:YES];
    NSWindow *window = gui_window(@"Qt backing store fixture", NSMakeRect(120, 120, 320, 200), view);
    scale = [window backingScaleFactor];
    gui_log("window %ld scale %.1f layer %s", (long)[window windowNumber], scale, [view layer] ? "yes" : "no");
    gui_pump(application, 0.5);
    gui_log("updateLayer %d", update_layer_calls);
    if (update_layer_calls == 0) return gui_unsupported("AppKit never sent -updateLayer to a layer-backed view");

    update_source = dispatch_source_create(&_dispatch_source_type_data_add, 0, 0, &_dispatch_main_q);
    dispatch_source_set_event_handler_f(update_source, deliver_update_requests);
    dispatch_resume(update_source);
    CVDisplayLinkRef link = 0;
    if (CVDisplayLinkCreateWithCGDisplay(CGMainDisplayID(), &link) != 0 || !link)
        return gui_unsupported("CVDisplayLinkCreateWithCGDisplay");
    CVDisplayLinkSetOutputCallback(link, display_link_callback, 0);
    const CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
    gui_log("display link %.0f Hz", period.timeValue ? (double)period.timeScale / (double)period.timeValue : 0.0);
    CVDisplayLinkStart(link);
    const int start_frames = frames_painted;
    for (int round = 0; round < 400 && frames_painted - start_frames < frames; ++round) gui_pump(application, 0.05);
    CVDisplayLinkStop(link);
    gui_log("running after stop %d", CVDisplayLinkIsRunning(link) ? 1 : 0);
    gui_log("frames %d ticks %d requests %d", frames_painted - start_frames, link_ticks, requests_delivered);
    gui_pump(application, 0.2);
    /* Probe the last flushed buffer like a capture would. */
    IOSurfaceRef surface = (IOSurfaceRef)[[view layer] contents];
    const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(surface);
    const size_t stride = IOSurfaceGetBytesPerRow(surface), width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface);
    const uint8_t *left = base + (height / 2) * stride + (width / 4) * 4;
    const uint8_t *right = base + (height / 2) * stride + (3 * width / 4) * 4;
    gui_log("left=%u,%u,%u,%u right=%u,%u,%u,%u", left[2], left[1], left[0], left[3], right[2], right[1], right[0], right[3]);
    CVDisplayLinkRelease(link);
    [window close];
    gui_log("done");
    return 0;
}
