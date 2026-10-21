#ifndef ALCHEMY_METAL_H
#define ALCHEMY_METAL_H
/* Project-authored declarations for Alchemy's bounded Metal guest ABI (GUI
 * fixtures: device, queue, buffers, textures, libraries, pipelines and
 * encoders). Selectors, layouts and numeric values follow the public
 * interface; no Apple header text is used. Extend as fixtures need. */
#include "../Foundation/Foundation.h"
#include <stddef.h>
#include <stdint.h>

@class NSError;
typedef NSUInteger MTLPixelFormat;
enum {
    MTLPixelFormatInvalid = 0, MTLPixelFormatR8Unorm = 10, MTLPixelFormatR32Float = 55,
    MTLPixelFormatRGBA8Unorm = 70, MTLPixelFormatRGBA8Unorm_sRGB = 71, MTLPixelFormatBGRA8Unorm = 80,
    MTLPixelFormatBGRA8Unorm_sRGB = 81, MTLPixelFormatRGBA16Float = 115, MTLPixelFormatRGBA32Float = 125,
    MTLPixelFormatDepth32Float = 252
};
typedef NSUInteger MTLResourceOptions;
enum {
    MTLResourceCPUCacheModeDefaultCache = 0, MTLResourceStorageModeShared = 0 << 4,
    MTLResourceStorageModeManaged = 1 << 4, MTLResourceStorageModePrivate = 2 << 4
};
typedef NSUInteger MTLStorageMode;
enum { MTLStorageModeShared = 0, MTLStorageModeManaged = 1, MTLStorageModePrivate = 2, MTLStorageModeMemoryless = 3 };
typedef NSUInteger MTLTextureUsage;
enum { MTLTextureUsageShaderRead = 1, MTLTextureUsageShaderWrite = 2, MTLTextureUsageRenderTarget = 4 };
typedef NSUInteger MTLLoadAction;
enum { MTLLoadActionDontCare = 0, MTLLoadActionLoad = 1, MTLLoadActionClear = 2 };
typedef NSUInteger MTLStoreAction;
enum { MTLStoreActionDontCare = 0, MTLStoreActionStore = 1 };
typedef NSUInteger MTLPrimitiveType;
enum { MTLPrimitiveTypePoint = 0, MTLPrimitiveTypeLine = 1, MTLPrimitiveTypeLineStrip = 2,
       MTLPrimitiveTypeTriangle = 3, MTLPrimitiveTypeTriangleStrip = 4 };
typedef NSUInteger MTLIndexType;
enum { MTLIndexTypeUInt16 = 0, MTLIndexTypeUInt32 = 1 };
typedef NSUInteger MTLCommandBufferStatus;
enum { MTLCommandBufferStatusNotEnqueued = 0, MTLCommandBufferStatusEnqueued = 1,
       MTLCommandBufferStatusCommitted = 2, MTLCommandBufferStatusScheduled = 3,
       MTLCommandBufferStatusCompleted = 4, MTLCommandBufferStatusError = 5 };

typedef struct { double red, green, blue, alpha; } MTLClearColor;
static inline MTLClearColor MTLClearColorMake(double r, double g, double b, double a) {
    MTLClearColor c = {r, g, b, a}; return c;
}
typedef struct { double originX, originY, width, height, znear, zfar; } MTLViewport;
typedef struct { NSUInteger width, height, depth; } MTLSize;
static inline MTLSize MTLSizeMake(NSUInteger w, NSUInteger h, NSUInteger d) { MTLSize s = {w, h, d}; return s; }
typedef struct { NSUInteger x, y, z; } MTLOrigin;
typedef struct { MTLOrigin origin; MTLSize size; } MTLRegion;
static inline MTLRegion MTLRegionMake2D(NSUInteger x, NSUInteger y, NSUInteger w, NSUInteger h) {
    MTLRegion r = {{x, y, 0}, {w, h, 1}}; return r;
}

@protocol MTLDevice, MTLCommandBuffer, MTLTexture, MTLFunction;

@protocol MTLResource
- (id<MTLDevice>)device;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
@end
@protocol MTLBuffer <MTLResource>
- (void *)contents;
- (NSUInteger)length;
- (void)didModifyRange:(NSRange)range;
@end
@protocol MTLTexture <MTLResource>
- (NSUInteger)width;
- (NSUInteger)height;
- (MTLPixelFormat)pixelFormat;
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)bytesPerRow;
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bytesPerRow fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level;
@end
@protocol MTLSamplerState
@end
@protocol MTLDepthStencilState
@end
@protocol MTLFunction
- (NSString *)name;
@end
@protocol MTLLibrary
- (id<MTLFunction>)newFunctionWithName:(NSString *)name;
- (NSArray *)functionNames;
@end
@protocol MTLRenderPipelineState
@end
@protocol MTLComputePipelineState
- (NSUInteger)maxTotalThreadsPerThreadgroup;
- (NSUInteger)threadExecutionWidth;
@end
@protocol MTLDrawable
- (void)present;
@end

@protocol MTLCommandEncoder
- (void)endEncoding;
- (void)setLabel:(NSString *)label;
@end
@protocol MTLRenderCommandEncoder <MTLCommandEncoder>
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state;
- (void)setViewport:(MTLViewport)viewport;
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)offset;
@end
@protocol MTLComputeCommandEncoder <MTLCommandEncoder>
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads;
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)group;
@end
@protocol MTLBlitCommandEncoder <MTLCommandEncoder>
- (void)copyFromBuffer:(id<MTLBuffer>)source sourceOffset:(NSUInteger)sourceOffset toBuffer:(id<MTLBuffer>)destination
     destinationOffset:(NSUInteger)destinationOffset size:(NSUInteger)size;
- (void)copyFromTexture:(id<MTLTexture>)source sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
           sourceOrigin:(MTLOrigin)origin sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)destination
      destinationOffset:(NSUInteger)offset destinationBytesPerRow:(NSUInteger)bytesPerRow
    destinationBytesPerImage:(NSUInteger)bytesPerImage;
- (void)synchronizeResource:(id<MTLResource>)resource;
@end

@class MTLRenderPassDescriptor, MTLRenderPipelineDescriptor, MTLTextureDescriptor, MTLCompileOptions,
    MTLSamplerDescriptor;
typedef void (^MTLCommandBufferHandler)(id<MTLCommandBuffer> buffer);
@protocol MTLCommandBuffer
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor;
- (id<MTLComputeCommandEncoder>)computeCommandEncoder;
- (id<MTLBlitCommandEncoder>)blitCommandEncoder;
- (void)presentDrawable:(id<MTLDrawable>)drawable;
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler;
- (void)commit;
- (void)waitUntilCompleted;
- (MTLCommandBufferStatus)status;
- (NSError *)error;
@end
@protocol MTLCommandQueue
- (id<MTLCommandBuffer>)commandBuffer;
@end
@protocol MTLDevice
- (NSString *)name;
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options;
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options;
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor;
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor;
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error;
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                                             error:(NSError **)error;
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error;
- (MTLSize)maxThreadsPerThreadgroup;
@end

@interface MTLRenderPassColorAttachmentDescriptor : NSObject
- (void)setTexture:(id<MTLTexture>)texture;
- (void)setLoadAction:(MTLLoadAction)action;
- (void)setStoreAction:(MTLStoreAction)action;
- (void)setClearColor:(MTLClearColor)color;
@end
@interface MTLRenderPassColorAttachmentDescriptorArray : NSObject
- (MTLRenderPassColorAttachmentDescriptor *)objectAtIndexedSubscript:(NSUInteger)index;
@end
@interface MTLRenderPassDescriptor : NSObject
+ (MTLRenderPassDescriptor *)renderPassDescriptor;
- (MTLRenderPassColorAttachmentDescriptorArray *)colorAttachments;
@end
@interface MTLRenderPipelineColorAttachmentDescriptor : NSObject
- (void)setPixelFormat:(MTLPixelFormat)format;
@end
@interface MTLRenderPipelineColorAttachmentDescriptorArray : NSObject
- (MTLRenderPipelineColorAttachmentDescriptor *)objectAtIndexedSubscript:(NSUInteger)index;
@end
@interface MTLRenderPipelineDescriptor : NSObject
- (void)setVertexFunction:(id<MTLFunction>)function;
- (void)setFragmentFunction:(id<MTLFunction>)function;
- (MTLRenderPipelineColorAttachmentDescriptorArray *)colorAttachments;
@end
@interface MTLTextureDescriptor : NSObject
+ (MTLTextureDescriptor *)texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format width:(NSUInteger)width
                                                      height:(NSUInteger)height mipmapped:(BOOL)mipmapped;
- (void)setUsage:(MTLTextureUsage)usage;
- (void)setStorageMode:(MTLStorageMode)mode;
@end
@interface MTLCompileOptions : NSObject
@end
@interface MTLSamplerDescriptor : NSObject
@end

id<MTLDevice> MTLCreateSystemDefaultDevice(void);
NSArray *MTLCopyAllDevices(void);
#endif
