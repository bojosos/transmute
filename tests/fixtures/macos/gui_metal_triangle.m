#include "gui_fixture.h"
#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>
/* GUI fixture (b): a CAMetalLayer-backed view (the layer-hosting pattern of
 * Blender's CocoaMetalView) rendering a red MSL triangle on a blue clear,
 * compiled at run time with newLibraryWithSource:. The last frame is also
 * copied from the drawable texture into a shared buffer, and two pixels are
 * printed (RGBA): the centre (inside the triangle) and the top-left corner.
 *
 * Like Blender, the fixture keeps drawableSize = view bounds x scale every
 * frame, where the scale is the window's backingScaleFactor (a real DPI
 * change) or a simulated backing scale (--scale, or the 'd' key), so the
 * presenter sees resizes and DPI changes as new drawable sizes.
 *
 *   gui_metal_triangle [--frames <n>]            renders n frames (default 5)
 *       [--resize]                               after half the frames, resizes
 *                                                the window to 400x300 points
 *       [--scale <s>]                            then (or at once without
 *                                                --resize) simulates backing
 *                                                scale s (integer)
 *   gui_metal_triangle --interactive [--timeout <s>]
 *                                                keeps rendering until the
 *                                                window closes, 'q' or the
 *                                                timeout; 'd' toggles a
 *                                                simulated backing scale of 2
 * Output: start, ready drawable=<w>x<h>, drawable=<w>x<h> scale=<s> (every
 * size change), frame <i>, centre=r,g,b,a corner=r,g,b,a, done. */

@protocol FixtureError
- (NSString *)localizedDescription;
@end

static const char *shader_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Varyings { float4 position [[position]]; float4 color; };\n"
    "vertex Varyings triangle_vertex(uint index [[vertex_id]], constant float2 *positions [[buffer(0)]]) {\n"
    "    Varyings out;\n"
    "    out.position = float4(positions[index], 0.0, 1.0);\n"
    "    out.color = float4(1.0, 0.0, 0.0, 1.0);\n"
    "    return out;\n"
    "}\n"
    "fragment float4 triangle_fragment(Varyings in [[stage_in]]) { return in.color; }\n";

static BOOL closed;
static CGFloat simulated_scale;   /* 0: the window's backingScaleFactor */

@interface TriangleView : NSView
@end
@implementation TriangleView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event {
    const char *characters = [[event charactersIgnoringModifiers] UTF8String];
    if (!characters) return;
    if (characters[0] == 'd') {
        simulated_scale = simulated_scale > 0 ? 0 : 2;
        gui_log("simulated scale %s", simulated_scale > 0 ? "2" : "off");
    } else if (characters[0] == 'q') {
        closed = YES;
    }
}
@end

@interface TriangleDelegate : NSObject <NSWindowDelegate>
@end
@implementation TriangleDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    closed = YES;
    return YES;
}
@end

static void print_error(const char *what, NSError *error) {
    NSString *text = error ? [(id<FixtureError>)error localizedDescription] : nil;
    fprintf(stderr, "gui_metal_triangle: %s failed: %s\n", what, text ? [text UTF8String] : "(no error)");
}

/* drawableSize = bounds x scale, as Blender's GHOST_ContextMTL updates it. */
static CGSize track_drawable(CAMetalLayer *layer, NSWindow *window, NSView *view) {
    CGFloat scale = simulated_scale > 0 ? simulated_scale : [window backingScaleFactor];
    NSRect bounds = [view bounds];
    CGSize wanted = CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
    CGSize current = [layer drawableSize];
    if (wanted.width >= 1 && wanted.height >= 1 && (wanted.width != current.width || wanted.height != current.height ||
                                                    [layer contentsScale] != scale)) {
        [layer setContentsScale:scale];
        [layer setDrawableSize:wanted];
        current = [layer drawableSize];
        gui_log("drawable=%.0fx%.0f scale=%.1f", current.width, current.height, scale);
    }
    return current;
}

int main(int argc, char **argv) {
    gui_fixture_name = "gui_metal_triangle";
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    gui_log("start");
    BOOL interactive = gui_has_argument(argc, argv, "--interactive");
    long frames = gui_argument_value(argc, argv, "--frames", 5);
    long timeout = gui_argument_value(argc, argv, "--timeout", 60);
    BOOL resize = gui_has_argument(argc, argv, "--resize");
    long scale_step = gui_argument_value(argc, argv, "--scale", 0);
    long step_frame = (resize || scale_step > 0) && !interactive ? frames / 2 : -1;

    /* Layer setup as Blender's GHOST_ContextMTL does it, before any window. */
    CAMetalLayer *layer = [CAMetalLayer layer];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [layer setEdgeAntialiasingMask:0];
    [layer setMasksToBounds:NO];
    [layer setOpaque:YES];
    [layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
    [layer setFramebufferOnly:NO];   /* the last frame is read back with a blit */
    [layer setPresentsWithTransaction:NO];
    [layer removeAllAnimations];
    [CATransaction commit];
    gui_log("layer pixelFormat=%lu framebufferOnly=%d opaque=%d maximumDrawableCount=%lu",
            (unsigned long)[layer pixelFormat], [layer framebufferOnly] ? 1 : 0, [layer isOpaque] ? 1 : 0,
            (unsigned long)[layer maximumDrawableCount]);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return gui_unsupported("MTLCreateSystemDefaultDevice returned nil");
    [layer setDevice:device];

    NSApplication *application = [NSApplication sharedApplication];
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application finishLaunching];
    TriangleView *view = [[TriangleView alloc] initWithFrame:NSMakeRect(0, 0, 256, 256)];
    [view setWantsLayer:YES];
    [view setLayer:layer];
    NSWindow *window = gui_window([NSString stringWithUTF8String:"gui_metal_triangle"], NSMakeRect(160, 160, 256, 256), view);
    TriangleDelegate *delegate = [[TriangleDelegate alloc] init];
    [window setDelegate:delegate];
    [application activateIgnoringOtherApps:YES];
    gui_pump(application, 0.1);
    CGFloat scale = [window backingScaleFactor];
    CGSize drawable_size = track_drawable(layer, window, view);
    gui_log("ready drawable=%.0fx%.0f scale=%.1f", drawable_size.width, drawable_size.height, scale);

    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:shader_source] options:nil
                                                    error:&error];
    if (!library) { print_error("newLibraryWithSource", error); return 4; }
    MTLRenderPipelineDescriptor *description = [[MTLRenderPipelineDescriptor alloc] init];
    [description setVertexFunction:[library newFunctionWithName:[NSString stringWithUTF8String:"triangle_vertex"]]];
    [description setFragmentFunction:[library newFunctionWithName:[NSString stringWithUTF8String:"triangle_fragment"]]];
    [[[description colorAttachments] objectAtIndexedSubscript:0] setPixelFormat:MTLPixelFormatBGRA8Unorm];
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:description error:&error];
    if (!pipeline) { print_error("newRenderPipelineStateWithDescriptor", error); return 4; }
    id<MTLCommandQueue> queue = [device newCommandQueue];

    id<MTLBuffer> readback = nil;
    NSUInteger width = 0, height = 0;
    const float positions[6] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.6f};
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:(NSTimeInterval)timeout];
    long rendered = 0;
    for (long frame = 0; interactive ? (!closed && [deadline timeIntervalSinceNow] > 0) : frame < frames; ++frame) {
        NSAutoreleasePool *frame_pool = [[NSAutoreleasePool alloc] init];
        if (frame == step_frame) {
            if (resize) {
                [window setContentSize:NSMakeSize(400, 300)];
                NSRect bounds = [view bounds];
                gui_log("resized view=%.0fx%.0f", bounds.size.width, bounds.size.height);
            }
            if (scale_step > 0) simulated_scale = (CGFloat)scale_step;
        }
        gui_pump(application, 0);
        /* The window closed while pumping: the layer left it. */
        if (closed) { [frame_pool drain]; break; }
        drawable_size = track_drawable(layer, window, view);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) { gui_log("frame %ld: no drawable", frame); [frame_pool drain]; continue; }
        BOOL last = !interactive && frame == frames - 1;
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassColorAttachmentDescriptor *color = [[pass colorAttachments] objectAtIndexedSubscript:0];
        [color setTexture:[drawable texture]];
        [color setLoadAction:MTLLoadActionClear];
        [color setStoreAction:MTLStoreActionStore];
        [color setClearColor:MTLClearColorMake(0, 0, 1, 1)];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBytes:positions length:sizeof positions atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        if (last) {
            width = [[drawable texture] width];
            height = [[drawable texture] height];
            readback = [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            MTLOrigin origin = {0, 0, 0};
            MTLSize size = {width, height, 1};
            [blit copyFromTexture:[drawable texture] sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:size
                         toBuffer:readback destinationOffset:0 destinationBytesPerRow:width * 4
          destinationBytesPerImage:width * height * 4];
            [blit endEncoding];
        }
        [commands presentDrawable:drawable];
        [commands commit];
        [commands waitUntilCompleted];
        if ([commands status] != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "gui_metal_triangle: command buffer status %lu\n", (unsigned long)[commands status]);
            return 5;
        }
        if (!interactive || frame < 3 || frame % 60 == 0) gui_log("frame %ld", frame);
        ++rendered;
        [frame_pool drain];
    }
    if (!interactive && readback) {
        const unsigned char *pixels = (const unsigned char *)[readback contents];
        const unsigned char *centre = pixels + ((height / 2) * width + width / 2) * 4;
        const unsigned char *corner = pixels + (2 * width + 2) * 4;
        /* BGRA8 in memory, printed as RGBA. */
        gui_log("readback %lux%lu", (unsigned long)width, (unsigned long)height);
        gui_log("centre=%u,%u,%u,%u corner=%u,%u,%u,%u", centre[2], centre[1], centre[0], centre[3], corner[2], corner[1],
                corner[0], corner[3]);
    }
    gui_log("rendered %ld frames", rendered);
    [window close];
    gui_log("done");
    [pool drain];
    return 0;
}
