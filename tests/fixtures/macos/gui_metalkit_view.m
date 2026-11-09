#include "gui_fixture.h"
#include <MetalKit/MetalKit.h>
/* GUI fixture: MetalKit's MTKView on Alchemy's Metal, QuartzCore and AppKit.
 *   (1) a delegate draws on the view's own run-loop frames: a blue triangle
 *       on the view's red clear colour, through currentRenderPassDescriptor
 *       (with the depth attachment the view owns for depthStencilPixelFormat)
 *       and currentDrawable; the third frame is read back;
 *   (2) the window grows: the delegate hears the new drawable size;
 *   (3) a paused subclass draws in -drawRect: (green clear) when -draw is
 *       called.
 * Output: start, device, frame <n>, readback centre=r,g,b,a corner=r,g,b,a,
 * depth=<1|0>, size <w>x<h>, subclass corner=r,g,b,a, done. */

static const char *shader_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "vertex float4 view_vertex(uint index [[vertex_id]]) {\n"
    "    const float2 positions[3] = {float2(-0.5, -0.5), float2(0.5, -0.5), float2(0.0, 0.6)};\n"
    "    return float4(positions[index], 0.0, 1.0);\n"
    "}\n"
    "fragment float4 view_fragment() { return float4(0.0, 0.0, 1.0, 1.0); }\n";

static id<MTLRenderPipelineState> pipeline;
static id<MTLCommandQueue> queue;
static long frames;
static id<MTLBuffer> readback;
static NSUInteger readback_width, readback_height;
static BOOL had_depth;
static CGSize resized;

/* Copies the drawable's texture into `readback` before it is presented. */
static void copy_back(id<MTLCommandBuffer> commands, id<MTLTexture> texture, id<MTLDevice> device) {
    readback_width = [texture width];
    readback_height = [texture height];
    readback = [device newBufferWithLength:readback_width * readback_height * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    MTLOrigin origin = {0, 0, 0};
    MTLSize size = {readback_width, readback_height, 1};
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:size toBuffer:readback
        destinationOffset:0 destinationBytesPerRow:readback_width * 4 destinationBytesPerImage:readback_width * readback_height * 4];
    [blit endEncoding];
}

static void print_pixels(const char *label) {
    const unsigned char *pixels = (const unsigned char *)[readback contents];
    const unsigned char *centre = pixels + ((readback_height / 2) * readback_width + readback_width / 2) * 4;
    const unsigned char *corner = pixels + (2 * readback_width + 2) * 4;
    /* BGRA8 in memory, printed as RGBA. */
    gui_log("%s centre=%u,%u,%u,%u corner=%u,%u,%u,%u", label, centre[2], centre[1], centre[0], centre[3], corner[2],
            corner[1], corner[0], corner[3]);
}

@interface ViewDelegate : NSObject <MTKViewDelegate>
@end
@implementation ViewDelegate
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    resized = size;
}
- (void)drawInMTKView:(MTKView *)view {
    MTLRenderPassDescriptor *pass = [view currentRenderPassDescriptor];
    id<CAMetalDrawable> drawable = [view currentDrawable];
    if (!pass || !drawable) return;
    had_depth = [[pass depthAttachment] texture] != nil && [view depthStencilTexture] != nil;
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    ++frames;
    if (frames == 3) copy_back(commands, [drawable texture], [view device]);
    [commands presentDrawable:drawable];
    [commands commit];
    [commands waitUntilCompleted];
    if (frames <= 3) gui_log("frame %ld", frames);
}
@end

/* A subclass drawing itself: clears to green. */
@interface GreenView : MTKView
@end
@implementation GreenView
- (void)drawRect:(NSRect)rect {
    MTLRenderPassDescriptor *pass = [self currentRenderPassDescriptor];
    id<CAMetalDrawable> drawable = [self currentDrawable];
    if (!pass || !drawable) return;
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    copy_back(commands, [drawable texture], [self device]);
    [commands presentDrawable:drawable];
    [commands commit];
    [commands waitUntilCompleted];
}
@end

int main(int argc, char **argv) {
    gui_fixture_name = "gui_metalkit_view";
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    gui_log("start");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return gui_unsupported("MTLCreateSystemDefaultDevice returned nil");
    gui_log("device");
    queue = [device newCommandQueue];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:shader_source] options:nil
                                                    error:&error];
    if (!library) { fprintf(stderr, "gui_metalkit_view: newLibraryWithSource failed\n"); return 4; }
    MTLRenderPipelineDescriptor *description = [[MTLRenderPipelineDescriptor alloc] init];
    [description setVertexFunction:[library newFunctionWithName:[NSString stringWithUTF8String:"view_vertex"]]];
    [description setFragmentFunction:[library newFunctionWithName:[NSString stringWithUTF8String:"view_fragment"]]];
    [[[description colorAttachments] objectAtIndexedSubscript:0] setPixelFormat:MTLPixelFormatBGRA8Unorm];
    [description setDepthAttachmentPixelFormat:MTLPixelFormatDepth32Float];
    pipeline = [device newRenderPipelineStateWithDescriptor:description error:&error];
    if (!pipeline) { fprintf(stderr, "gui_metalkit_view: pipeline failed\n"); return 4; }

    NSApplication *application = [NSApplication sharedApplication];
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application finishLaunching];

    /* (1) Delegate drawing on the view's own frames. */
    MTKView *view = [[MTKView alloc] initWithFrame:NSMakeRect(0, 0, 256, 256) device:device];
    [view setColorPixelFormat:MTLPixelFormatBGRA8Unorm];
    [view setDepthStencilPixelFormat:MTLPixelFormatDepth32Float];
    [view setClearColor:MTLClearColorMake(1, 0, 0, 1)];
    [view setFramebufferOnly:NO];
    ViewDelegate *delegate = [[ViewDelegate alloc] init];
    [view setDelegate:delegate];
    NSWindow *window = gui_window([NSString stringWithUTF8String:"gui_metalkit_view"], NSMakeRect(160, 160, 256, 256), view);
    [application activateIgnoringOtherApps:YES];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:30];
    while (!(frames >= 3 && readback) && [deadline timeIntervalSinceNow] > 0) gui_pump(application, 0.05);
    if (!(frames >= 3 && readback)) {
        fprintf(stderr, "gui_metalkit_view: only %ld frames\n", frames);
        return 5;
    }
    print_pixels("readback");
    gui_log("depth=%d", had_depth ? 1 : 0);

    /* (2) Resize: the delegate hears the new drawable size. */
    [window setContentSize:NSMakeSize(400, 300)];
    deadline = [NSDate dateWithTimeIntervalSinceNow:10];
    while (resized.width < 400 && [deadline timeIntervalSinceNow] > 0) gui_pump(application, 0.05);
    gui_log("size %.0fx%.0f", resized.width, resized.height);
    [view setPaused:YES];

    /* (3) A paused subclass drawing in -drawRect: on -draw. */
    GreenView *green = [[GreenView alloc] initWithFrame:NSMakeRect(0, 0, 128, 128) device:device];
    [green setClearColor:MTLClearColorMake(0, 1, 0, 1)];
    [green setFramebufferOnly:NO];
    [green setPaused:YES];
    NSWindow *second = gui_window([NSString stringWithUTF8String:"gui_metalkit_view 2"], NSMakeRect(460, 160, 128, 128), green);
    gui_pump(application, 0.2);
    readback = nil;
    [green draw];
    if (!readback) { fprintf(stderr, "gui_metalkit_view: the subclass did not draw\n"); return 6; }
    print_pixels("subclass");
    [second close];
    [window close];
    gui_log("done");
    [pool drain];
    return 0;
}
