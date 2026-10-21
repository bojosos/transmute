#include <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
/* Metal-on-Vulkan probe (owner: A5): the guest-visible Metal interface end
 * to end on alchemy-fx: device queries (including an MTLSize result through
 * x8), buffers, a blit fill/copy with a completed handler, texture upload and
 * readback, a render pass that only clears, sampler and depth/stencil states,
 * shared events, presentDrawable: of a guest drawable (the -present message
 * after the submission) and the library path (a compile error is an NSError,
 * not a crash). Without a Vulkan 1.3 device the probe reports "no device". */

/* Selectors beyond the shared declaration header. */
@protocol AlchemyProbeDevice <MTLDevice>
- (BOOL)supportsFamily:(NSInteger)family;
- (NSUInteger)argumentBuffersSupport;
- (BOOL)hasUnifiedMemory;
- (uint64_t)recommendedMaxWorkingSetSize;
- (uint64_t)registryID;
- (id)newSharedEvent;
- (id)newDepthStencilStateWithDescriptor:(id)descriptor;
@end
@protocol AlchemyProbeBlit <MTLBlitCommandEncoder>
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value;
@end
@protocol AlchemyProbeCommandBuffer <MTLCommandBuffer>
- (void)encodeSignalEvent:(id)event value:(uint64_t)value;
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(double)time;
@end
@protocol AlchemyProbeEvent
- (uint64_t)signaledValue;
@end
@class Protocol;
static inline NSRange NSMakeRange(NSUInteger location, NSUInteger length) {
    NSRange range = {location, length};
    return range;
}
@interface NSObject (AlchemyProbe)
- (BOOL)conformsToProtocol:(Protocol *)protocol;
- (id)localizedDescription;
- (const char *)UTF8String;
- (id)frontFaceStencil;
- (void)setDepthWriteEnabled:(BOOL)value;
- (void)setDepthCompareFunction:(NSUInteger)value;
- (void)setStencilCompareFunction:(NSUInteger)value;
- (void)setMinFilter:(NSUInteger)value;
- (void)setMagFilter:(NSUInteger)value;
- (BOOL)isDepthWriteEnabled;
@end
Class objc_getClass(const char *name);

/* A global block literal written to the Blocks ABI (the link stubs have no
 * block runtime; Alchemy's _Block_copy returns global blocks unchanged). */
struct probe_descriptor { unsigned long reserved, size; };
struct probe_block {
    void *isa;
    int flags, reserved;
    void (*invoke)(struct probe_block *, id);
    const struct probe_descriptor *descriptor;
};
static char probe_isa[32];
static volatile int completed_handlers;
static void on_completed(struct probe_block *block, id buffer) {
    (void)block;
    if (buffer) ++completed_handlers;
}
static const struct probe_descriptor descriptor = {0, sizeof(struct probe_block)};
static struct probe_block completed_block = {probe_isa, 1 << 28, 0, on_completed, &descriptor};

/* A guest drawable: Metal presents it by message once the command buffer is
 * submitted. */
static volatile int presented;
static volatile double presented_time;
@interface ProbeDrawable : NSObject <MTLDrawable> {
    id<MTLTexture> _texture;
}
@end
@implementation ProbeDrawable
- (id)initWithTexture:(id<MTLTexture>)texture { _texture = texture; return self; }
- (id<MTLTexture>)texture { return _texture; }
- (void)present { ++presented; }
- (void)presentAtTime:(double)time { presented_time = time; ++presented; }
@end

static int fail(const char *what) {
    printf("Alchemy Metal probe: FAIL %s\n", what);
    return 1;
}

int main(void) {
    id<AlchemyProbeDevice> device = (id<AlchemyProbeDevice>)MTLCreateSystemDefaultDevice();
    if (!device) {
        puts("Alchemy Metal probe: no device");
        return 0;
    }
    if (![(id)device conformsToProtocol:@protocol(MTLDevice)]) return fail("MTLDevice conformance");
    MTLSize threads = [device maxThreadsPerThreadgroup];
    printf("Alchemy Metal probe: device %s, threadgroup %lux%lux%lu, tier %lu, unified %d, Mac2 %d, Apple7 %d\n",
           [[device name] UTF8String], (unsigned long)threads.width, (unsigned long)threads.height,
           (unsigned long)threads.depth, (unsigned long)[device argumentBuffersSupport], (int)[device hasUnifiedMemory],
           (int)[device supportsFamily:2002], (int)[device supportsFamily:1007]);
    if (threads.width < 256 || [device recommendedMaxWorkingSetSize] == 0 || [device registryID] == 0)
        return fail("device limits");

    /* Buffers and a blit with a completed handler. */
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> source = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> destination = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    if (!queue || !source || !destination || [source length] != 4096) return fail("buffers");
    unsigned char *bytes = (unsigned char *)[source contents];
    for (int index = 0; index < 4096; ++index) bytes[index] = (unsigned char)(index * 3);
    id<AlchemyProbeCommandBuffer> commands = (id<AlchemyProbeCommandBuffer>)[queue commandBuffer];
    id<AlchemyProbeBlit> blit = (id<AlchemyProbeBlit>)[commands blitCommandEncoder];
    [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:2048];
    [blit fillBuffer:destination range:NSMakeRange(2049, 1000) value:0x7e];
    [blit endEncoding];
    [commands addCompletedHandler:(MTLCommandBufferHandler)(void *)&completed_block];
    [commands commit];
    [commands waitUntilCompleted];
    for (long spin = 0; spin < 200000000 && completed_handlers == 0; ++spin) {
    }
    const unsigned char *copied = (const unsigned char *)[destination contents];
    if ([commands status] != MTLCommandBufferStatusCompleted) return fail("blit status");
    if (copied[100] != (unsigned char)300 || copied[2047] != (unsigned char)(2047 * 3) || copied[2049] != 0x7e ||
        copied[3048] != 0x7e || copied[3049] != 0)
        return fail("blit contents");
    if (completed_handlers != 1) return fail("completed handler");
    puts("Alchemy Metal probe: blit and completed handler");

    /* Texture upload and readback. */
    MTLTextureDescriptor *texture_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    [texture_descriptor setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
    [texture_descriptor setStorageMode:MTLStorageModeManaged];
    id<MTLTexture> texture = [device newTextureWithDescriptor:texture_descriptor];
    if (!texture || [texture width] != 16 || [texture pixelFormat] != MTLPixelFormatRGBA8Unorm) return fail("texture");
    unsigned char pixels[16 * 16 * 4], back[16 * 16 * 4];
    for (int index = 0; index < (int)sizeof(pixels); ++index) pixels[index] = (unsigned char)(index ^ 0x5a);
    [texture replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:pixels bytesPerRow:64];
    memset(back, 0, sizeof(back));
    [texture getBytes:back bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
    if (memcmp(pixels, back, sizeof(pixels)) != 0) return fail("texture round trip");
    puts("Alchemy Metal probe: texture round trip");

    /* A render pass that clears, read back with a blit. */
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor *color = [[pass colorAttachments] objectAtIndexedSubscript:0];
    [color setTexture:texture];
    [color setLoadAction:MTLLoadActionClear];
    [color setStoreAction:MTLStoreActionStore];
    [color setClearColor:MTLClearColorMake(1.0, 0.5, 0.0, 1.0)];
    commands = (id<AlchemyProbeCommandBuffer>)[queue commandBuffer];
    id<MTLRenderCommandEncoder> render = [commands renderCommandEncoderWithDescriptor:pass];
    if (!render) return fail("render encoder");
    [render endEncoding];
    id<MTLBuffer> readback = [device newBufferWithLength:16 * 16 * 4 options:MTLResourceStorageModeShared];
    blit = (id<AlchemyProbeBlit>)[commands blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:(MTLOrigin){0, 0, 0}
               sourceSize:MTLSizeMake(16, 16, 1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:64
    destinationBytesPerImage:64 * 16];
    [blit endEncoding];
    id event = [device newSharedEvent];
    [commands encodeSignalEvent:event value:7];
    [commands commit];
    [commands waitUntilCompleted];
    const unsigned char *cleared = (const unsigned char *)[readback contents];
    if (cleared[0] != 255 || cleared[1] < 127 || cleared[1] > 128 || cleared[2] != 0 || cleared[3] != 255)
        return fail("clear color");
    if ([(id<AlchemyProbeEvent>)event signaledValue] != 7) return fail("shared event");
    puts("Alchemy Metal probe: render pass clear and shared event");

    /* presentDrawable: sends -present (-presentAtTime:) after the submission. */
    ProbeDrawable *drawable = [[ProbeDrawable alloc] initWithTexture:texture];
    commands = (id<AlchemyProbeCommandBuffer>)[queue commandBuffer];
    render = [commands renderCommandEncoderWithDescriptor:pass];
    [render endEncoding];
    [commands presentDrawable:drawable];
    if (presented != 0) return fail("present before commit");
    [commands commit];
    if (presented != 1) return fail("present at commit");
    commands = (id<AlchemyProbeCommandBuffer>)[queue commandBuffer];
    [commands presentDrawable:drawable atTime:1.5];
    [commands commit];
    [commands waitUntilCompleted];
    if (presented != 2 || presented_time != 1.5) return fail("presentDrawable:atTime:");
    puts("Alchemy Metal probe: drawable presented");

    /* States and the library path. */
    MTLSamplerDescriptor *sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    [(id)sampler_descriptor setMinFilter:1];
    [(id)sampler_descriptor setMagFilter:1];
    if (![device newSamplerStateWithDescriptor:sampler_descriptor]) return fail("sampler state");
    id depth_descriptor = [[(id)objc_getClass("MTLDepthStencilDescriptor") alloc] init];
    [depth_descriptor setDepthWriteEnabled:YES];
    [depth_descriptor setDepthCompareFunction:1];
    [[depth_descriptor frontFaceStencil] setStencilCompareFunction:7];
    if (![depth_descriptor isDepthWriteEnabled] || ![device newDepthStencilStateWithDescriptor:depth_descriptor])
        return fail("depth/stencil state");
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "kernel void probe(device uint *out [[buffer(0)]], constant uint &scale [[buffer(1)]],\n"
         "                  uint id [[thread_position_in_grid]]) { out[id] = id * scale + 1; }\n"
         "struct ProbeVertex { float4 position [[position]]; float4 color; };\n"
         "vertex ProbeVertex probe_vertex(uint vid [[vertex_id]], constant float2 *positions [[buffer(0)]]) {\n"
         "    ProbeVertex out; out.position = float4(positions[vid], 0.0, 1.0);\n"
         "    out.color = float4(0.0, 1.0, 0.25, 1.0); return out; }\n"
         "fragment float4 probe_fragment(ProbeVertex in [[stage_in]]) { return in.color; }\n"
                                                  options:nil error:&error];
    if (library) puts("Alchemy Metal probe: library compiled");
    else if (error) printf("Alchemy Metal probe: library error: %s\n", [[(id)error localizedDescription] UTF8String]);
    else return fail("library without error");

    /* With a compiler: the kernel through a compute pipeline, setBytes: and
     * dispatchThreads:. */
    if (library) {
        id<MTLFunction> function = [library newFunctionWithName:@"probe"];
        if (!function) return fail("newFunctionWithName:");
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            printf("Alchemy Metal probe: pipeline error: %s\n", error ? [[(id)error localizedDescription] UTF8String] : "nil");
            return fail("compute pipeline");
        }
        id<MTLBuffer> out = [device newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
        memset([out contents], 0, 64 * 4);
        const unsigned scale = 5;
        commands = (id<AlchemyProbeCommandBuffer>)[queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:out offset:0 atIndex:0];
        [compute setBytes:&scale length:sizeof(scale) atIndex:1];
        [compute dispatchThreads:MTLSizeMake(32, 1, 1) threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
        [compute endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        const unsigned *values = (const unsigned *)[out contents];
        for (unsigned index = 0; index < 32; ++index)
            if (values[index] != index * scale + 1) {
                printf("Alchemy Metal probe: out[%u] = %u\n", index, values[index]);
                return fail("kernel output");
            }
        if (values[32] != 0) return fail("kernel wrote outside the grid");
        puts("Alchemy Metal probe: kernel dispatched");

        /* A render pipeline (vertex + fragment, stage_in) drawing a
         * full-target triangle, read back with a blit. */
        MTLRenderPipelineDescriptor *render_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        [render_descriptor setVertexFunction:[library newFunctionWithName:@"probe_vertex"]];
        [render_descriptor setFragmentFunction:[library newFunctionWithName:@"probe_fragment"]];
        [[[render_descriptor colorAttachments] objectAtIndexedSubscript:0] setPixelFormat:MTLPixelFormatRGBA8Unorm];
        id<MTLRenderPipelineState> render_pipeline = [device newRenderPipelineStateWithDescriptor:render_descriptor error:&error];
        if (!render_pipeline) {
            printf("Alchemy Metal probe: pipeline error: %s\n", error ? [[(id)error localizedDescription] UTF8String] : "nil");
            return fail("render pipeline");
        }
        const float positions[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
        [color setClearColor:MTLClearColorMake(0.0, 0.0, 0.0, 0.0)];
        commands = (id<AlchemyProbeCommandBuffer>)[queue commandBuffer];
        render = [commands renderCommandEncoderWithDescriptor:pass];
        [render setRenderPipelineState:render_pipeline];
        [render setVertexBytes:positions length:sizeof(positions) atIndex:0];
        [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [render endEncoding];
        blit = (id<AlchemyProbeBlit>)[commands blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:(MTLOrigin){0, 0, 0}
                   sourceSize:MTLSizeMake(16, 16, 1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:64
        destinationBytesPerImage:64 * 16];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        const unsigned char *drawn = (const unsigned char *)[readback contents] + (8 * 16 + 8) * 4;
        if (drawn[0] != 0 || drawn[1] != 255 || drawn[2] < 63 || drawn[2] > 64 || drawn[3] != 255) {
            printf("Alchemy Metal probe: pixel %d %d %d %d\n", drawn[0], drawn[1], drawn[2], drawn[3]);
            return fail("triangle pixel");
        }
        puts("Alchemy Metal probe: triangle drawn");
    }
    puts("Alchemy Metal probe: done");
    return 0;
}
