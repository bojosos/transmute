#include <Foundation/Foundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
/* GUI fixture (QLab graphics group G6), headless: draws through the AppKit
 * drawing additions into a CGBitmapContext and checks the pixels (NSGradient,
 * NSShadow, NSAffineTransform, NSPatternColor, +[NSImage
 * imageWithSize:flipped:drawingHandler:]), checks NSColor HSB, the
 * CATransform3D functions, GLKQuaternionSlerp, CAAnimation delegate timing on
 * the run loop, the WebKit configuration objects and MTKTextureLoader.
 * Classes and functions are looked up at run time (objc_getClass, dlsym):
 * the fixture links only the base link-only identities.
 * Output: "qlab_graphics: <check> ok" lines, then "qlab_graphics: done". */

extern Class objc_getClass(const char *name);

typedef struct CGContext *CGContextRef;
typedef struct CGImage *CGImageRef;
typedef struct { CGFloat m11, m12, m13, m14, m21, m22, m23, m24, m31, m32, m33, m34, m41, m42, m43, m44; } CATransform3D;
typedef struct { float x, y, z, w; } Quaternion;

static int failures;
static void say(const char *text) { printf("qlab_graphics: %s\n", text); fflush(stdout); }
static void check(int condition, const char *what) {
    if (condition) return;
    fprintf(stderr, "qlab_graphics: FAILED %s\n", what);
    fflush(stderr);
    ++failures;
}
static int near(double a, double b, double tolerance) { return a - b <= tolerance && b - a <= tolerance; }
static void *symbol(const char *library, const char *name) {
    void *handle = dlopen(library, 1);
    void *found = handle ? dlsym(handle, name) : 0;
    if (!found) { fprintf(stderr, "qlab_graphics: missing %s\n", name); fflush(stderr); ++failures; }
    return found;
}
#define CG "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics"
#define CA "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore"

@protocol GraphicsContextAPI
+ (id)graphicsContextWithCGContext:(CGContextRef)context flipped:(BOOL)flipped;
+ (void)setCurrentContext:(id)context;
+ (void)saveGraphicsState;
+ (void)restoreGraphicsState;
@end
@protocol ColorAPI
+ (id)colorWithSRGBRed:(CGFloat)r green:(CGFloat)g blue:(CGFloat)b alpha:(CGFloat)a;
+ (id)colorWithCalibratedHue:(CGFloat)h saturation:(CGFloat)s brightness:(CGFloat)b alpha:(CGFloat)a;
+ (id)colorWithPatternImage:(id)image;
- (void)getRed:(CGFloat *)r green:(CGFloat *)g blue:(CGFloat *)b alpha:(CGFloat *)a;
- (void)getHue:(CGFloat *)h saturation:(CGFloat *)s brightness:(CGFloat *)b alpha:(CGFloat *)a;
- (void)setFill;
- (void)set;
@end
@protocol GradientAPI
- (id)initWithStartingColor:(id)start endingColor:(id)end;
- (id)initWithColorsAndLocations:(id)first, ...;
- (NSInteger)numberOfColorStops;
- (id)interpolatedColorAtLocation:(CGFloat)location;
- (void)drawInRect:(CGRect)rect angle:(CGFloat)angle;
@end
@protocol ShadowAPI
- (void)setShadowOffset:(CGSize)offset;
- (void)setShadowBlurRadius:(CGFloat)radius;
- (void)setShadowColor:(id)color;
- (CGSize)shadowOffset;
- (void)set;
@end
@protocol TransformAPI
+ (id)transform;
- (void)translateXBy:(CGFloat)x yBy:(CGFloat)y;
- (void)rotateByDegrees:(CGFloat)degrees;
- (CGPoint)transformPoint:(CGPoint)point;
- (void)concat;
@end
@protocol PathAPI
+ (id)bezierPathWithRect:(CGRect)rect;
- (void)fill;
@end
@protocol ImageAPI
+ (id)imageWithSize:(CGSize)size flipped:(BOOL)flipped drawingHandler:(BOOL (^)(CGRect))handler;
+ (id)imageWithSystemSymbolName:(id)name accessibilityDescription:(id)description;
- (id)initWithCGImage:(CGImageRef)image size:(CGSize)size;
- (void)drawInRect:(CGRect)rect;
@end
@protocol AnimationAPI
+ (id)animationWithKeyPath:(id)path;
- (void)setDuration:(double)duration;
- (void)setDelegate:(id)delegate;
- (void)setToValue:(id)value;
- (double)beginTime;
@end
@protocol LayerAPI
- (void)addAnimation:(id)animation forKey:(id)key;
- (id)animationForKey:(id)key;
- (NSArray *)animationKeys;
@end
@protocol TimingAPI
+ (id)functionWithName:(id)name;
- (void)getControlPointAtIndex:(size_t)index values:(float *)values;
@end
@protocol WebAPI
- (id)preferences;
- (BOOL)javaScriptEnabled;
- (id)userContentController;
- (void)addUserScript:(id)script;
- (NSArray *)userScripts;
- (id)initWithSource:(id)source injectionTime:(NSInteger)time forMainFrameOnly:(BOOL)main;
- (id)initWithFrame:(CGRect)frame configuration:(id)configuration;
- (id)configuration;
@end
@protocol TextureLoaderAPI
- (id)initWithDevice:(id)device;
- (id)newTextureWithCGImage:(CGImageRef)image options:(id)options error:(id *)error;
@end
@protocol CoreImageAPI
+ (id)imageWithCGImage:(CGImageRef)image;
+ (id)filterWithName:(id)name;
+ (id)contextWithOptions:(id)options;
+ (id)vectorWithX:(CGFloat)x Y:(CGFloat)y Z:(CGFloat)z W:(CGFloat)w;
+ (id)kernelWithFunctionName:(id)name fromMetalLibraryData:(id)data error:(id *)error;
- (void)setValue:(id)value forKey:(id)key;
- (id)valueForKey:(id)key;
- (id)outputImage;
- (CGRect)extent;
- (CGImageRef)createCGImage:(id)image fromRect:(CGRect)rect;
- (id)stringRepresentation;
- (id)attributes;
@end
@protocol RunLoopAPI
+ (id)currentRunLoop;
- (void)runUntilDate:(id)date;
@end

@interface AnimationWatcher : NSObject {
@public
    int started, stopped, finished;
}
@end
@implementation AnimationWatcher
- (void)animationDidStart:(id)animation { (void)animation; ++started; }
- (void)animationDidStop:(id)animation finished:(BOOL)flag { (void)animation; ++stopped; finished = flag; }
@end

/* A global block (no captures) built by hand: the fixture links no block runtime. */
struct GlobalBlock { void *isa; int flags; int reserved; void *invoke; void *descriptor; };
static struct { unsigned long reserved, size; } block_descriptor = {0, sizeof(struct GlobalBlock)};
static int handler_calls;
static BOOL draw_green(void *block, CGRect rect) {
    (void)block;
    ++handler_calls;
    id<ColorAPI> green = [(id<ColorAPI>)objc_getClass("NSColor") colorWithSRGBRed:0 green:1 blue:0 alpha:1];
    [green setFill];
    [(id<PathAPI>)[(Class)objc_getClass("NSBezierPath") bezierPathWithRect:rect] fill];
    return YES;
}

enum { W = 64, H = 64 };
static unsigned char pixels[W * H * 4];
static const unsigned char *pixel(int x, int y) { return pixels + ((H - 1 - y) * W + x) * 4; }   /* y up */

int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    say("start");
    CGContextRef (*bitmap_create)(void *, size_t, size_t, size_t, size_t, CGColorSpaceRef, uint32_t) = symbol(CG, "CGBitmapContextCreate");
    CGImageRef (*bitmap_image)(CGContextRef) = symbol(CG, "CGBitmapContextCreateImage");
    void (*fill_rect)(CGContextRef, CGRect) = symbol(CG, "CGContextFillRect");
    void (*fill_rgb)(CGContextRef, CGFloat, CGFloat, CGFloat, CGFloat) = symbol(CG, "CGContextSetRGBFillColor");
    void (*save)(CGContextRef) = symbol(CG, "CGContextSaveGState");
    void (*restore)(CGContextRef) = symbol(CG, "CGContextRestoreGState");
    CFStringRef *srgb_name = symbol(CG, "kCGColorSpaceSRGB");
    CGColorSpaceRef space = ((CGColorSpaceRef (*)(CFStringRef))symbol(CG, "CGColorSpaceCreateWithName"))(*srgb_name);
    CGContextRef context = bitmap_create(pixels, W, H, 8, W * 4, space, 1 /* premultiplied last */);
    if (!context) { fprintf(stderr, "qlab_graphics: no bitmap context\n"); return 4; }
    Class gc = objc_getClass("NSGraphicsContext");
    [(id<GraphicsContextAPI>)gc setCurrentContext:[(id<GraphicsContextAPI>)gc graphicsContextWithCGContext:context flipped:NO]];
    id<ColorAPI> colors = (id<ColorAPI>)objc_getClass("NSColor");
    id red = [colors colorWithSRGBRed:1 green:0 blue:0 alpha:1];
    id blue = [colors colorWithSRGBRed:0 green:0 blue:1 alpha:1];

    /* NSGradient: red to blue left to right across the bottom band. */
    id<GradientAPI> gradient = [(id<GradientAPI>)[objc_getClass("NSGradient") alloc] initWithStartingColor:red endingColor:blue];
    check(gradient && [gradient numberOfColorStops] == 2, "gradient stops");
    CGFloat r = 0, g = 0, b = 0, a = 0;
    [(id<ColorAPI>)[gradient interpolatedColorAtLocation:0.25] getRed:&r green:&g blue:&b alpha:&a];
    check(near(r, 0.75, 1e-9) && near(b, 0.25, 1e-9), "gradient interpolation");
    [gradient drawInRect:CGRectMake(0, 0, 64, 16) angle:0];
    check(pixel(1, 8)[0] > 240 && pixel(1, 8)[2] < 16, "gradient starts red");
    check(pixel(62, 8)[2] > 240 && pixel(62, 8)[0] < 16, "gradient ends blue");
    check(pixel(32, 8)[0] > 100 && pixel(32, 8)[0] < 155 && pixel(32, 8)[2] > 100, "gradient middle");
    check(pixel(32, 20)[3] == 0, "gradient stays in its rect");
    id<GradientAPI> listed = [(id<GradientAPI>)[objc_getClass("NSGradient") alloc]
        initWithColorsAndLocations:red, (CGFloat)0.0, blue, (CGFloat)0.5, red, (CGFloat)1.0, nil];
    check(listed && [listed numberOfColorStops] == 3, "initWithColorsAndLocations:");
    say("gradient ok");

    /* NSShadow: a hard black shadow 5 points right and down of a blue square. */
    save(context);
    id<ShadowAPI> shadow = [[objc_getClass("NSShadow") alloc] init];
    [shadow setShadowOffset:CGSizeMake(5, -5)];
    [shadow setShadowBlurRadius:0];
    [shadow setShadowColor:[colors colorWithSRGBRed:0 green:0 blue:0 alpha:1]];
    check([shadow shadowOffset].width == 5, "shadow offset");
    [shadow set];
    fill_rgb(context, 0, 0, 1, 1);
    fill_rect(context, CGRectMake(10, 30, 10, 10));
    restore(context);
    check(pixel(12, 35)[2] > 240, "shadowed square");
    check(pixel(23, 27)[3] > 240 && pixel(23, 27)[2] < 16, "shadow cast below right");
    check(pixel(8, 42)[3] == 0, "no shadow above left");
    say("shadow ok");

    /* NSAffineTransform: translate (30, 30) then rotate 90 degrees. */
    id<TransformAPI> transform = [(id<TransformAPI>)objc_getClass("NSAffineTransform") transform];
    [transform translateXBy:40 yBy:30];
    [transform rotateByDegrees:90];
    CGPoint moved = [transform transformPoint:CGPointMake(1, 0)];
    check(near(moved.x, 40, 1e-9) && near(moved.y, 31, 1e-9), "transformPoint:");
    save(context);
    [transform concat];
    fill_rgb(context, 0, 1, 0, 1);
    fill_rect(context, CGRectMake(0, 0, 4, 2));   /* lands at x 38..40, y 30..34 */
    restore(context);
    check(pixel(39, 32)[1] > 240 && pixel(41, 32)[3] == 0, "concat");
    say("affine ok");

    /* Pattern color: a 2x2 checker image (red / blue) tiled into a rect. */
    unsigned char cell[4 * 4 * 4];
    CGContextRef cell_context = bitmap_create(cell, 2, 2, 8, 8, space, 1);
    fill_rgb(cell_context, 1, 0, 0, 1); fill_rect(cell_context, CGRectMake(0, 0, 2, 2));
    fill_rgb(cell_context, 0, 0, 1, 1); fill_rect(cell_context, CGRectMake(0, 0, 1, 1)); fill_rect(cell_context, CGRectMake(1, 1, 1, 1));
    CGImageRef cell_image = bitmap_image(cell_context);
    id image = [(id<ImageAPI>)[objc_getClass("NSImage") alloc] initWithCGImage:cell_image size:CGSizeMake(2, 2)];
    id<ColorAPI> pattern = [colors colorWithPatternImage:image];
    [pattern setFill];
    [(id<PathAPI>)[(Class)objc_getClass("NSBezierPath") bezierPathWithRect:CGRectMake(48, 48, 8, 8)] fill];
    const unsigned char *p00 = pixel(48, 48), *p10 = pixel(49, 48), *p11 = pixel(49, 49);
    check(p00[3] > 240 && p10[3] > 240, "pattern fills");
    check((p00[0] > 200) != (p10[0] > 200) && (p00[0] > 200) == (p11[0] > 200), "pattern alternates");
    say("pattern ok");

    /* +[NSImage imageWithSize:flipped:drawingHandler:]: drawn on demand. */
    struct GlobalBlock block = {symbol("/usr/lib/libSystem.B.dylib", "_NSConcreteGlobalBlock"), 1 << 28, 0, (void *)draw_green, &block_descriptor};
    id drawn = [(id<ImageAPI>)objc_getClass("NSImage") imageWithSize:CGSizeMake(8, 8) flipped:NO drawingHandler:(BOOL (^)(CGRect))(void *)&block];
    check(handler_calls == 0, "drawing handler is lazy");
    [(id<ImageAPI>)drawn drawInRect:CGRectMake(4, 50, 8, 8)];
    check(handler_calls == 1 && pixel(7, 53)[1] > 240, "drawing handler image");
    check([(id<ImageAPI>)objc_getClass("NSImage") imageWithSystemSymbolName:[NSString stringWithUTF8String:"gear"] accessibilityDescription:nil] == nil,
          "no symbol images");
    say("images ok");

    /* NSColor HSB. */
    id orange = [colors colorWithCalibratedHue:30.0 / 360 saturation:1 brightness:1 alpha:0.5];
    [(id<ColorAPI>)orange getRed:&r green:&g blue:&b alpha:&a];
    check(near(r, 1, 1e-9) && near(g, 0.5, 1e-9) && near(b, 0, 1e-9) && near(a, 0.5, 1e-9), "HSB to RGB");
    CGFloat h = 0, s = 0, v = 0;
    [(id<ColorAPI>)orange getHue:&h saturation:&s brightness:&v alpha:&a];
    check(near(h, 30.0 / 360, 1e-9) && near(s, 1, 1e-9), "RGB to HSB");
    say("colors ok");

    /* CATransform3D functions and GLKQuaternionSlerp. */
    CATransform3D (*rotation)(CGFloat, CGFloat, CGFloat, CGFloat) = symbol(CA, "CATransform3DMakeRotation");
    CATransform3D (*translate)(CATransform3D, CGFloat, CGFloat, CGFloat) = symbol(CA, "CATransform3DTranslate");
    CATransform3D (*concat)(CATransform3D, CATransform3D) = symbol(CA, "CATransform3DConcat");
    CATransform3D (*invert)(CATransform3D) = symbol(CA, "CATransform3DInvert");
    bool (*is_identity)(CATransform3D) = symbol(CA, "CATransform3DIsIdentity");
    CATransform3D turned = translate(rotation(1.5707963267948966, 0, 0, 1), 10, 0, 0);
    check(near(turned.m12, 1, 1e-12) && near(turned.m21, -1, 1e-12) && near(turned.m42, 10, 1e-12), "CATransform3D rotate/translate");
    check(is_identity(concat(turned, invert(turned))), "CATransform3D invert/concat");
    Quaternion (*slerp)(Quaternion, Quaternion, float) = symbol("/System/Library/Frameworks/GLKit.framework/Versions/A/GLKit", "GLKQuaternionSlerp");
    Quaternion identity = *(Quaternion *)symbol("/System/Library/Frameworks/GLKit.framework/Versions/A/GLKit", "GLKQuaternionIdentity");
    Quaternion quarter = {0, 0, 0.70710678f, 0.70710678f};
    Quaternion mid = slerp(identity, quarter, 0.5f);
    check(near(mid.z, 0.38268343, 1e-5) && near(mid.w, 0.92387953, 1e-5), "GLKQuaternionSlerp");
    unsigned (*create_program)(void) = symbol("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL", "glCreateProgram");
    check(create_program() == 0, "glCreateProgram without a context");
    say("transforms ok");

    /* Animation timing: delegate callbacks from the run loop. */
    id timing = [(id<TimingAPI>)objc_getClass("CAMediaTimingFunction") functionWithName:[NSString stringWithUTF8String:"easeInEaseOut"]];
    float point[2] = {0, 0};
    [(id<TimingAPI>)timing getControlPointAtIndex:1 values:point];
    check(near(point[0], 0.42, 1e-6), "timing function control point");
    AnimationWatcher *watcher = [[AnimationWatcher alloc] init];
    id layer = [[objc_getClass("CALayer") alloc] init];
    id<AnimationAPI> animation = [(id<AnimationAPI>)objc_getClass("CABasicAnimation") animationWithKeyPath:[NSString stringWithUTF8String:"opacity"]];
    [animation setDuration:0.05];
    [animation setDelegate:watcher];
    [animation setToValue:[NSNumber numberWithDouble:0]];
    id key = [NSString stringWithUTF8String:"fade"];
    [(id<LayerAPI>)layer addAnimation:animation forKey:key];
    id added = [(id<LayerAPI>)layer animationForKey:key];
    check(added && added != animation && [(id<AnimationAPI>)added beginTime] > 0, "the layer keeps a timed copy");
    check(watcher->started == 0, "animationDidStart: is not synchronous");
    id<RunLoopAPI> loop = [(id<RunLoopAPI>)objc_getClass("NSRunLoop") currentRunLoop];
    for (int round = 0; round < 50 && !watcher->stopped; ++round)
        [loop runUntilDate:[(Class)objc_getClass("NSDate") dateWithTimeIntervalSinceNow:0.02]];
    check(watcher->started == 1 && watcher->stopped == 1 && watcher->finished, "animation delegate callbacks");
    check([(id<LayerAPI>)layer animationKeys] == nil, "removed on completion");
    say("animation ok");

    /* WebKit configuration objects. */
    id configuration = [[objc_getClass("WKWebViewConfiguration") alloc] init];
    check([(id<WebAPI>)[(id<WebAPI>)configuration preferences] javaScriptEnabled], "WKPreferences default");
    id script = [(id<WebAPI>)[objc_getClass("WKUserScript") alloc] initWithSource:[NSString stringWithUTF8String:"1"] injectionTime:0 forMainFrameOnly:YES];
    [(id<WebAPI>)[(id<WebAPI>)configuration userContentController] addUserScript:script];
    check([[(id<WebAPI>)[(id<WebAPI>)configuration userContentController] userScripts] count] == 1, "user scripts");
    id view = [(id<WebAPI>)[objc_getClass("WKWebView") alloc] initWithFrame:CGRectMake(0, 0, 10, 10) configuration:configuration];
    check(view && [(id<WebAPI>)view configuration] != nil, "WKWebView configuration");
    say("webkit ok");

    /* Core Image: invert the checker cell, render it and draw it at (56, 40). */
    id<CoreImageAPI> ci_cell = [(id<CoreImageAPI>)objc_getClass("CIImage") imageWithCGImage:cell_image];
    CGRect cell_extent = [ci_cell extent];
    check(cell_extent.size.width == 2 && cell_extent.size.height == 2, "CIImage extent");
    id<CoreImageAPI> inverter = [(id<CoreImageAPI>)objc_getClass("CIFilter") filterWithName:[NSString stringWithUTF8String:"CIColorInvert"]];
    check(inverter != nil, "CIColorInvert");
    [inverter setValue:ci_cell forKey:[NSString stringWithUTF8String:"inputImage"]];
    id inverted = [inverter outputImage];
    id<CoreImageAPI> ci_context = [(id<CoreImageAPI>)objc_getClass("CIContext") contextWithOptions:nil];
    CGImageRef rendered = [ci_context createCGImage:inverted fromRect:CGRectMake(0, 0, 2, 2)];
    check(rendered != 0, "createCGImage:fromRect:");
    void (*draw_image)(CGContextRef, CGRect, CGImageRef) = symbol(CG, "CGContextDrawImage");
    if (rendered) draw_image(context, CGRectMake(56, 40, 2, 2), rendered);
    const unsigned char *q00 = pixel(56, 40), *q10 = pixel(57, 40);
    /* red -> cyan, blue -> yellow */
    check(q00[3] > 240 && q10[3] > 240, "inverted pixels drawn");
    check((q00[0] < 16 && q00[1] > 240 && q00[2] > 240) || (q00[0] > 240 && q00[1] > 240 && q00[2] < 16), "inverted colors");
    check(q00[0] != q10[0], "inverted checker");
    check([(id<CoreImageAPI>)objc_getClass("CIFilter") filterWithName:[NSString stringWithUTF8String:"CIGaussianBlur"]] == nil,
          "unimplemented filters answer nil");
    id vector = [(id<CoreImageAPI>)objc_getClass("CIVector") vectorWithX:1 Y:2.5 Z:3 W:4];
    check(strcmp([[(id<CoreImageAPI>)vector stringRepresentation] UTF8String], "[1 2.5 3 4]") == 0, "CIVector string");
    id kernel_error = nil;
    check([(id<CoreImageAPI>)objc_getClass("CIKernel") kernelWithFunctionName:[NSString stringWithUTF8String:"k"]
                                                      fromMetalLibraryData:[NSData dataWithBytes:"x" length:1] error:&kernel_error] == nil &&
          kernel_error != nil, "metallib kernels fail with an error");
    say("core image ok");

    /* MTKTextureLoader: the gradient bitmap as a shared texture. */
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device) {
        id loader = [(id<TextureLoaderAPI>)[objc_getClass("MTKTextureLoader") alloc] initWithDevice:device];
        const char *metalkit = "/System/Library/Frameworks/MetalKit.framework/Versions/A/MetalKit";
        id storage_key = *(id *)symbol(metalkit, "MTKTextureLoaderOptionTextureStorageMode");
        id srgb_key = *(id *)symbol(metalkit, "MTKTextureLoaderOptionSRGB");
        id keys[2] = {storage_key, srgb_key};
        id values[2] = {[NSNumber numberWithInt:0 /* shared */], [NSNumber numberWithBool:NO]};
        id options = [NSDictionary dictionaryWithObjects:values forKeys:keys count:2];
        CGImageRef snapshot = bitmap_image(context);
        id error = nil;
        id<MTLTexture> texture = [(id<TextureLoaderAPI>)loader newTextureWithCGImage:snapshot options:options error:&error];
        check(texture != nil && [texture width] == W && [texture pixelFormat] == MTLPixelFormatRGBA8Unorm, "texture from CGImage");
        if (texture) {
            unsigned char row[W * 4];
            [texture getBytes:row bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, H - 1 - 8, W, 1) mipmapLevel:0];
            check(row[1 * 4] > 240 && row[62 * 4 + 2] > 240, "texture pixels (top row first)");
        }
        id private_texture = [(id<TextureLoaderAPI>)loader newTextureWithCGImage:snapshot options:nil error:&error];
        check(private_texture != nil && [(id<MTLTexture>)private_texture pixelFormat] == MTLPixelFormatRGBA8Unorm_sRGB, "private sRGB texture");
        id observer = nil;
        id (*all_devices)(id *, void *) = symbol("/System/Library/Frameworks/Metal.framework/Versions/A/Metal", "MTLCopyAllDevicesWithObserver");
        void (*remove_observer)(id) = symbol("/System/Library/Frameworks/Metal.framework/Versions/A/Metal", "MTLRemoveDeviceObserver");
        NSArray *devices = all_devices(&observer, 0);
        check(devices && [devices count] >= 1 && observer != nil, "MTLCopyAllDevicesWithObserver");
        remove_observer(observer);
        id (*display_device)(uint32_t) = symbol(CG, "CGDirectDisplayCopyCurrentMetalDevice");
        uint32_t (*main_display)(void) = symbol(CG, "CGMainDisplayID");
        check(main_display() == 0 || display_device(main_display()) != nil, "CGDirectDisplayCopyCurrentMetalDevice");
        say("metalkit ok");
    } else {
        say("metalkit skipped (no Metal device)");
    }

    [pool drain];
    if (failures) { fprintf(stderr, "qlab_graphics: %d checks failed\n", failures); return 1; }
    say("done");
    return 0;
}
