#ifndef ALCHEMY_METAL_H
#define ALCHEMY_METAL_H
/* The Metal protocols, classes and types the project-authored Metal overlay
   (src/alchemy/darwin/swift/Metal) extends or calls, as the Swift
   ClangImporter sees them, so the overlay's extensions mangle as Apple's
   ($sSo24MTLRenderCommandEncoderP5MetalE...). Independently authored from
   Apple's published Metal documentation; no SDK content. Only what the
   overlay and its fixture use is declared. Everything is reached through the
   Objective-C runtime (Alchemy's Metal provider, src/alchemy/darwin/metal);
   the overlay calls the array forms under their own Swift names
   (__alchemy...) and the public Swift API is the overlay's. */
#include <stdint.h>
#include <Foundation/Foundation.h>

typedef NS_ENUM(NSUInteger, MTLPixelFormat) {
    MTLPixelFormatInvalid = 0,
    MTLPixelFormatR32Uint = 53,
    MTLPixelFormatRGBA8Unorm = 70,
    MTLPixelFormatBGRA8Unorm = 80,
};
typedef NS_ENUM(NSUInteger, MTLTextureType) {
    MTLTextureType1D = 0,
    MTLTextureType2D = 2,
    MTLTextureType2DArray = 3,
};
typedef NS_ENUM(NSUInteger, MTLDataType) {
    MTLDataTypeNone = 0,
    MTLDataTypeFloat = 3,
    MTLDataTypeInt = 29,
    MTLDataTypeUInt = 33,
    MTLDataTypeBool = 53,
};
typedef NS_OPTIONS(NSUInteger, MTLResourceUsage) {
    MTLResourceUsageRead = 1 << 0,
    MTLResourceUsageWrite = 1 << 1,
};
typedef NS_OPTIONS(NSUInteger, MTLRenderStages) {
    MTLRenderStageVertex = 1 << 0,
    MTLRenderStageFragment = 1 << 1,
};
typedef NS_OPTIONS(NSUInteger, MTLResourceOptions) {
    MTLResourceStorageModeShared = 0,
};
typedef NS_OPTIONS(NSUInteger, MTLTextureUsage) {
    MTLTextureUsageShaderRead = 1 << 0,
    MTLTextureUsageShaderWrite = 1 << 1,
    MTLTextureUsageRenderTarget = 1 << 2,
    MTLTextureUsagePixelFormatView = 1 << 4,
};
typedef NS_ENUM(NSUInteger, MTLLoadAction) {
    MTLLoadActionDontCare = 0,
    MTLLoadActionLoad = 1,
    MTLLoadActionClear = 2,
};
typedef NS_ENUM(NSUInteger, MTLStoreAction) {
    MTLStoreActionDontCare = 0,
    MTLStoreActionStore = 1,
};

typedef struct { double originX, originY, width, height, znear, zfar; } MTLViewport;
typedef struct { NSUInteger x, y, width, height; } MTLScissorRect;
typedef struct { float x, y; } MTLSamplePosition;

#ifdef __OBJC__
NS_ASSUME_NONNULL_BEGIN

@protocol MTLDevice;

@protocol MTLResource <NSObject>
@property (readonly) id<MTLDevice> device;
@end

@protocol MTLBuffer <MTLResource>
@property (readonly) NSUInteger length;
- (void*)contents;
- (void)didModifyRange:(NSRange)range NS_SWIFT_NAME(__alchemyDidModify(_:));
- (void)addDebugMarker:(NSString*)marker range:(NSRange)range NS_SWIFT_NAME(__alchemyAddDebugMarker(_:range:));
@end

@protocol MTLTexture <MTLResource>
@property (readonly) MTLPixelFormat pixelFormat;
- (nullable id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)pixelFormat
                                             textureType:(MTLTextureType)textureType
                                                  levels:(NSRange)levelRange
                                                  slices:(NSRange)sliceRange
    NS_SWIFT_NAME(__alchemyTextureView(pixelFormat:textureType:levels:slices:));
@end

@protocol MTLSamplerState <NSObject>
@end
@protocol MTLHeap <NSObject>
@end
@protocol MTLRenderPipelineState <NSObject>
@end
@protocol MTLComputePipelineState <NSObject>
@end
@protocol MTLIndirectCommandBuffer <MTLResource>
- (void)resetWithRange:(NSRange)range NS_SWIFT_NAME(__alchemyReset(_:));
@end

@protocol MTLCommandEncoder <NSObject>
- (void)endEncoding;
@end

@protocol MTLRenderCommandEncoder <MTLCommandEncoder>
- (void)setViewports:(const MTLViewport[_Nonnull])viewports count:(NSUInteger)count
    NS_SWIFT_NAME(__alchemySetViewports(_:count:));
- (void)setScissorRects:(const MTLScissorRect[_Nonnull])scissorRects count:(NSUInteger)count
    NS_SWIFT_NAME(__alchemySetScissorRects(_:count:));
- (void)setVertexBuffers:(const id<MTLBuffer> _Nullable[_Nonnull])buffers offsets:(const NSUInteger[_Nonnull])offsets
               withRange:(NSRange)range NS_SWIFT_NAME(__alchemySetVertexBuffers(_:offsets:range:));
- (void)setVertexTextures:(const id<MTLTexture> _Nullable[_Nonnull])textures withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetVertexTextures(_:range:));
- (void)setVertexSamplerStates:(const id<MTLSamplerState> _Nullable[_Nonnull])samplers withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetVertexSamplerStates(_:range:));
- (void)setVertexSamplerStates:(const id<MTLSamplerState> _Nullable[_Nonnull])samplers
                  lodMinClamps:(const float[_Nonnull])lodMinClamps
                  lodMaxClamps:(const float[_Nonnull])lodMaxClamps
                     withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetVertexSamplerStates(_:lodMinClamps:lodMaxClamps:range:));
- (void)setFragmentBuffers:(const id<MTLBuffer> _Nullable[_Nonnull])buffers offsets:(const NSUInteger[_Nonnull])offsets
                 withRange:(NSRange)range NS_SWIFT_NAME(__alchemySetFragmentBuffers(_:offsets:range:));
- (void)setFragmentTextures:(const id<MTLTexture> _Nullable[_Nonnull])textures withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetFragmentTextures(_:range:));
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> _Nullable[_Nonnull])samplers withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetFragmentSamplerStates(_:range:));
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> _Nullable[_Nonnull])samplers
                    lodMinClamps:(const float[_Nonnull])lodMinClamps
                    lodMaxClamps:(const float[_Nonnull])lodMaxClamps
                       withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetFragmentSamplerStates(_:lodMinClamps:lodMaxClamps:range:));
- (void)useResources:(const id<MTLResource> _Nonnull[_Nonnull])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage
    NS_SWIFT_NAME(__alchemyUseResources(_:count:usage:));
- (void)useResources:(const id<MTLResource> _Nonnull[_Nonnull])resources count:(NSUInteger)count
               usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages
    NS_SWIFT_NAME(__alchemyUseResources(_:count:usage:stages:));
- (void)useHeaps:(const id<MTLHeap> _Nonnull[_Nonnull])heaps count:(NSUInteger)count NS_SWIFT_NAME(__alchemyUseHeaps(_:count:));
- (void)useHeaps:(const id<MTLHeap> _Nonnull[_Nonnull])heaps count:(NSUInteger)count stages:(MTLRenderStages)stages
    NS_SWIFT_NAME(__alchemyUseHeaps(_:count:stages:));
- (void)memoryBarrierWithResources:(const id<MTLResource> _Nonnull[_Nonnull])resources count:(NSUInteger)count
                       afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before
    NS_SWIFT_NAME(__alchemyMemoryBarrier(resources:count:after:before:));
@end

@protocol MTLComputeCommandEncoder <MTLCommandEncoder>
- (void)setBuffers:(const id<MTLBuffer> _Nullable[_Nonnull])buffers offsets:(const NSUInteger[_Nonnull])offsets
         withRange:(NSRange)range NS_SWIFT_NAME(__alchemySetBuffers(_:offsets:range:));
- (void)setTextures:(const id<MTLTexture> _Nullable[_Nonnull])textures withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetTextures(_:range:));
- (void)setSamplerStates:(const id<MTLSamplerState> _Nullable[_Nonnull])samplers withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetSamplerStates(_:range:));
- (void)setSamplerStates:(const id<MTLSamplerState> _Nullable[_Nonnull])samplers
            lodMinClamps:(const float[_Nonnull])lodMinClamps
            lodMaxClamps:(const float[_Nonnull])lodMaxClamps
               withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetSamplerStates(_:lodMinClamps:lodMaxClamps:range:));
- (void)useResources:(const id<MTLResource> _Nonnull[_Nonnull])resources count:(NSUInteger)count usage:(MTLResourceUsage)usage
    NS_SWIFT_NAME(__alchemyUseResources(_:count:usage:));
- (void)useHeaps:(const id<MTLHeap> _Nonnull[_Nonnull])heaps count:(NSUInteger)count NS_SWIFT_NAME(__alchemyUseHeaps(_:count:));
- (void)memoryBarrierWithResources:(const id<MTLResource> _Nonnull[_Nonnull])resources count:(NSUInteger)count
    NS_SWIFT_NAME(__alchemyMemoryBarrier(resources:count:));
@end

@protocol MTLBlitCommandEncoder <MTLCommandEncoder>
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value
    NS_SWIFT_NAME(__alchemyFill(buffer:range:value:));
@end

@protocol MTLArgumentEncoder <NSObject>
- (void)setBuffers:(const id<MTLBuffer> _Nullable[_Nonnull])buffers offsets:(const NSUInteger[_Nonnull])offsets
         withRange:(NSRange)range NS_SWIFT_NAME(__alchemySetBuffers(_:offsets:range:));
- (void)setTextures:(const id<MTLTexture> _Nullable[_Nonnull])textures withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetTextures(_:range:));
- (void)setSamplerStates:(const id<MTLSamplerState> _Nullable[_Nonnull])samplers withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetSamplerStates(_:range:));
- (void)setRenderPipelineStates:(const id<MTLRenderPipelineState> _Nullable[_Nonnull])pipelines withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetRenderPipelineStates(_:range:));
- (void)setComputePipelineStates:(const id<MTLComputePipelineState> _Nullable[_Nonnull])pipelines withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetComputePipelineStates(_:range:));
- (void)setIndirectCommandBuffers:(const id<MTLIndirectCommandBuffer> _Nullable[_Nonnull])buffers withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetIndirectCommandBuffers(_:range:));
@end

__attribute__((objc_runtime_visible))
@interface MTLFunctionConstantValues : NSObject
- (void)setConstantValues:(const void*)values type:(MTLDataType)type withRange:(NSRange)range
    NS_SWIFT_NAME(__alchemySetConstantValues(_:type:range:));
@end

__attribute__((objc_runtime_visible))
@interface MTLTextureDescriptor : NSObject
+ (MTLTextureDescriptor*)texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pixelFormat width:(NSUInteger)width
                                                     height:(NSUInteger)height mipmapped:(BOOL)mipmapped
    NS_SWIFT_NAME(texture2DDescriptor(pixelFormat:width:height:mipmapped:));
@property (nonatomic) MTLTextureUsage usage;
@end

__attribute__((objc_runtime_visible))
@interface MTLRenderPassColorAttachmentDescriptor : NSObject
@property (nullable, nonatomic, strong) id<MTLTexture> texture;
@property (nonatomic) MTLLoadAction loadAction;
@property (nonatomic) MTLStoreAction storeAction;
@end
__attribute__((objc_runtime_visible))
@interface MTLRenderPassColorAttachmentDescriptorArray : NSObject
- (MTLRenderPassColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)index;
@end
__attribute__((objc_runtime_visible))
@interface MTLRenderPassDescriptor : NSObject
+ (MTLRenderPassDescriptor*)renderPassDescriptor NS_SWIFT_NAME(renderPassDescriptor());
@property (readonly) MTLRenderPassColorAttachmentDescriptorArray* colorAttachments;
@end

@protocol MTLCommandBuffer <NSObject>
- (nullable id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)descriptor
    NS_SWIFT_NAME(makeRenderCommandEncoder(descriptor:));
- (nullable id<MTLComputeCommandEncoder>)computeCommandEncoder NS_SWIFT_NAME(makeComputeCommandEncoder());
- (nullable id<MTLBlitCommandEncoder>)blitCommandEncoder NS_SWIFT_NAME(makeBlitCommandEncoder());
- (void)commit;
- (void)waitUntilCompleted;
@end
@protocol MTLCommandQueue <NSObject>
- (nullable id<MTLCommandBuffer>)commandBuffer NS_SWIFT_NAME(makeCommandBuffer());
@end

@protocol MTLDevice <NSObject>
@property (readonly) NSString* name;
- (nullable id<MTLCommandQueue>)newCommandQueue NS_SWIFT_NAME(makeCommandQueue());
- (nullable id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
    NS_SWIFT_NAME(makeBuffer(length:options:));
- (nullable id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor NS_SWIFT_NAME(makeTexture(descriptor:));
- (void)getDefaultSamplePositions:(MTLSamplePosition*)positions count:(NSUInteger)count
    NS_SWIFT_NAME(__alchemyGetDefaultSamplePositions(_:count:));
@end

id<MTLDevice> _Nullable MTLCreateSystemDefaultDevice(void);

NS_ASSUME_NONNULL_END
#endif

#endif
