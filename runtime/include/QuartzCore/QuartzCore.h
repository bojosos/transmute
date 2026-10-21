#ifndef ALCHEMY_QUARTZCORE_H
#define ALCHEMY_QUARTZCORE_H
/* Project-authored declarations for Alchemy's bounded QuartzCore guest ABI
 * (GUI fixtures: CALayer, CAMetalLayer and drawables). Selectors, layouts and
 * numeric values follow the public interface; no Apple header text is used. */
#include "../Foundation/Foundation.h"
#include "../CoreGraphics/CoreGraphics.h"
#include "../Metal/Metal.h"

typedef double CFTimeInterval;
CFTimeInterval CACurrentMediaTime(void);

@interface CALayer : NSObject
+ (id)layer;
- (CGRect)frame;
- (void)setFrame:(CGRect)frame;
- (CGRect)bounds;
- (void)setBounds:(CGRect)bounds;
- (CGFloat)contentsScale;
- (void)setContentsScale:(CGFloat)scale;
- (id)delegate;
- (void)setDelegate:(id)delegate;
- (void)setNeedsDisplay;
- (void)setOpaque:(BOOL)opaque;
- (BOOL)isOpaque;
- (void)setMasksToBounds:(BOOL)flag;
- (void)setEdgeAntialiasingMask:(unsigned int)mask;
- (void)removeAllAnimations;
- (CALayer *)superlayer;
- (NSArray *)sublayers;
- (void)addSublayer:(CALayer *)layer;
- (void)removeFromSuperlayer;
@end

@class CAMetalLayer;
@protocol CAMetalDrawable <MTLDrawable>
- (id<MTLTexture>)texture;
- (CAMetalLayer *)layer;
@end

@interface CAMetalLayer : CALayer
- (id<MTLDevice>)device;
- (void)setDevice:(id<MTLDevice>)device;
- (MTLPixelFormat)pixelFormat;
- (void)setPixelFormat:(MTLPixelFormat)format;
- (CGSize)drawableSize;
- (void)setDrawableSize:(CGSize)size;
- (BOOL)framebufferOnly;
- (void)setFramebufferOnly:(BOOL)flag;
- (BOOL)displaySyncEnabled;
- (void)setDisplaySyncEnabled:(BOOL)flag;
- (NSUInteger)maximumDrawableCount;
- (void)setMaximumDrawableCount:(NSUInteger)count;
- (BOOL)presentsWithTransaction;
- (void)setPresentsWithTransaction:(BOOL)flag;
- (BOOL)allowsNextDrawableTimeout;
- (void)setAllowsNextDrawableTimeout:(BOOL)flag;
- (CGColorSpaceRef)colorspace;
- (void)setColorspace:(CGColorSpaceRef)space;
- (BOOL)wantsExtendedDynamicRangeContent;
- (void)setWantsExtendedDynamicRangeContent:(BOOL)flag;
- (id<CAMetalDrawable>)nextDrawable;
@end

@interface CATransaction : NSObject
+ (void)begin;
+ (void)commit;
+ (void)flush;
+ (void)setDisableActions:(BOOL)flag;
@end
#endif
