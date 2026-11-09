#ifndef ALCHEMY_METALKIT_H
#define ALCHEMY_METALKIT_H
/* MetalKit as the guest fixtures use it: MTKView and MTKViewDelegate.
 * Project-authored from Apple's public MetalKit documentation; no SDK
 * content. Alchemy's MetalKit provider (src/alchemy/darwin/metal/metalkit.cpp)
 * implements it. */
#include "../AppKit/AppKit.h"
#include "../Metal/Metal.h"
#include "../QuartzCore/QuartzCore.h"

@protocol MTKViewDelegate;

@interface MTKView : NSView
- (instancetype)initWithFrame:(NSRect)frame device:(id<MTLDevice>)device;
- (id<MTLDevice>)device;
- (void)setDevice:(id<MTLDevice>)device;
- (id<MTKViewDelegate>)delegate;
- (void)setDelegate:(id<MTKViewDelegate>)delegate;
- (id<CAMetalDrawable>)currentDrawable;
- (MTLRenderPassDescriptor *)currentRenderPassDescriptor;
- (MTLPixelFormat)colorPixelFormat;
- (void)setColorPixelFormat:(MTLPixelFormat)format;
- (MTLPixelFormat)depthStencilPixelFormat;
- (void)setDepthStencilPixelFormat:(MTLPixelFormat)format;
- (NSUInteger)sampleCount;
- (void)setSampleCount:(NSUInteger)count;
- (MTLClearColor)clearColor;
- (void)setClearColor:(MTLClearColor)color;
- (double)clearDepth;
- (void)setClearDepth:(double)depth;
- (BOOL)framebufferOnly;
- (void)setFramebufferOnly:(BOOL)value;
- (BOOL)isPaused;
- (void)setPaused:(BOOL)paused;
- (BOOL)enableSetNeedsDisplay;
- (void)setEnableSetNeedsDisplay:(BOOL)value;
- (NSInteger)preferredFramesPerSecond;
- (void)setPreferredFramesPerSecond:(NSInteger)value;
- (CGSize)drawableSize;
- (void)setDrawableSize:(CGSize)size;
- (id<MTLTexture>)depthStencilTexture;
- (void)draw;
- (void)releaseDrawables;
@end

@protocol MTKViewDelegate
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size;
- (void)drawInMTKView:(MTKView *)view;
@end

#endif
