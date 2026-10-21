#include <Metal/Metal.h>
#include <stdio.h>
/* GUI fixture (c), headless: an MSL compute kernel squares 4096 integers
 * from a shared buffer into a private buffer, a blit copies the result back
 * into a shared buffer, and the host verifies every element.
 * Output: start, device <name>, checksum=<n> ok, done. */

@protocol FixtureError
- (NSString *)localizedDescription;
@end

static const char *kernel_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void square_plus_one(device const uint *input [[buffer(0)]], device uint *output [[buffer(1)]],\n"
    "                            constant uint &count [[buffer(2)]], uint index [[thread_position_in_grid]]) {\n"
    "    if (index < count) output[index] = input[index] * input[index] + 1u;\n"
    "}\n";

static void say(const char *text) { printf("gui_metal_compute: %s\n", text); fflush(stdout); }

int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    say("start");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        fprintf(stderr, "gui_metal_compute: unsupported: MTLCreateSystemDefaultDevice returned nil\n");
        return 2;
    }
    NSString *name = [device name];
    printf("gui_metal_compute: device %s\n", name ? [name UTF8String] : "(unnamed)");
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kernel_source] options:nil
                                                    error:&error];
    if (!library) {
        NSString *text = error ? [(id<FixtureError>)error localizedDescription] : nil;
        fprintf(stderr, "gui_metal_compute: newLibraryWithSource failed: %s\n", text ? [text UTF8String] : "(no error)");
        return 4;
    }
    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:"square_plus_one"]];
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) { fprintf(stderr, "gui_metal_compute: compute pipeline failed\n"); return 4; }

    enum { count = 4096 };
    uint32_t values[count];
    for (uint32_t index = 0; index < count; ++index) values[index] = index * 7u + 3u;
    id<MTLBuffer> input = [device newBufferWithBytes:values length:sizeof values options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:sizeof values options:MTLResourceStorageModePrivate];
    id<MTLBuffer> readback = [device newBufferWithLength:sizeof values options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    uint32_t total = count;
    [encoder setBytes:&total length:sizeof total atIndex:2];
    NSUInteger width = [pipeline threadExecutionWidth];
    if (width == 0 || width > 256) width = 64;
    [encoder dispatchThreadgroups:MTLSizeMake((count + width - 1) / width, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    [encoder endEncoding];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:output sourceOffset:0 toBuffer:readback destinationOffset:0 size:sizeof values];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if ([commands status] != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "gui_metal_compute: command buffer status %lu\n", (unsigned long)[commands status]);
        return 5;
    }
    const uint32_t *results = (const uint32_t *)[readback contents];
    unsigned long long checksum = 0;
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t wanted = values[index] * values[index] + 1u;
        if (results[index] != wanted) {
            fprintf(stderr, "gui_metal_compute: element %u is %u, expected %u\n", index, results[index], wanted);
            return 6;
        }
        checksum += results[index];
    }
    printf("gui_metal_compute: checksum=%llu ok\n", checksum);
    say("done");
    [pool drain];
    return 0;
}
