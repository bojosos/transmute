#ifndef ALCHEMY_COREGRAPHICS_H
#define ALCHEMY_COREGRAPHICS_H
/* Project-authored declarations for Alchemy's bounded CoreGraphics guest ABI
 * (GUI fixtures). Layouts and numeric values follow the public interface;
 * no Apple header text is used. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../CoreFoundation/CoreFoundation.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef double CGFloat;
typedef struct CGPoint { CGFloat x, y; } CGPoint;
typedef struct CGSize { CGFloat width, height; } CGSize;
typedef struct CGRect { CGPoint origin; CGSize size; } CGRect;
static inline CGPoint CGPointMake(CGFloat x, CGFloat y) { CGPoint p = {x, y}; return p; }
static inline CGSize CGSizeMake(CGFloat width, CGFloat height) { CGSize s = {width, height}; return s; }
static inline CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat width, CGFloat height) {
    CGRect r = {{x, y}, {width, height}}; return r;
}
typedef uint32_t CGDirectDisplayID;
typedef int32_t CGError;
enum { kCGErrorSuccess = 0, kCGErrorFailure = 1000, kCGErrorIllegalArgument = 1001, kCGErrorNoneAvailable = 1011 };
typedef const struct CGColorSpace *CGColorSpaceRef;
extern const CFStringRef kCGColorSpaceSRGB;
extern const CFStringRef kCGColorSpaceDisplayP3;
extern const CFStringRef kCGColorSpaceExtendedLinearSRGB;

CGError CGGetActiveDisplayList(uint32_t maxDisplays, CGDirectDisplayID *activeDisplays, uint32_t *displayCount);
CGDirectDisplayID CGMainDisplayID(void);
CGRect CGDisplayBounds(CGDirectDisplayID display);
size_t CGDisplayPixelsWide(CGDirectDisplayID display);
size_t CGDisplayPixelsHigh(CGDirectDisplayID display);
CGError CGWarpMouseCursorPosition(CGPoint newCursorPosition);
CGError CGAssociateMouseAndMouseCursorPosition(bool connected);
CGError CGDisplayMoveCursorToPoint(CGDirectDisplayID display, CGPoint point);
CGColorSpaceRef CGColorSpaceCreateWithName(CFStringRef name);
CGColorSpaceRef CGColorSpaceRetain(CGColorSpaceRef space);
void CGColorSpaceRelease(CGColorSpaceRef space);

/* Colors and images (Core Foundation types), as the Swift CoreGraphics and SwiftUI overlays use them. */
typedef struct CGColor *CGColorRef;
typedef struct CGImage *CGImageRef;
CGColorRef CGColorCreateGenericGray(CGFloat gray, CGFloat alpha);
CGColorRef CGColorCreateGenericRGB(CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha);
CGColorRef CGColorGetConstantColor(CFStringRef colorName);
size_t CGColorGetNumberOfComponents(CGColorRef color);
const CGFloat *CGColorGetComponents(CGColorRef color);
CGFloat CGColorGetAlpha(CGColorRef color);
extern const CFStringRef kCGColorWhite;
extern const CFStringRef kCGColorBlack;
extern const CFStringRef kCGColorClear;
/* Line caps and joins, with the published values. */
typedef enum __attribute__((enum_extensibility(open))) CGLineCap : int32_t {
    kCGLineCapButt = 0, kCGLineCapRound = 1, kCGLineCapSquare = 2
} CGLineCap;
typedef enum __attribute__((enum_extensibility(open))) CGLineJoin : int32_t {
    kCGLineJoinMiter = 0, kCGLineJoinRound = 1, kCGLineJoinBevel = 2
} CGLineJoin;
#ifdef __cplusplus
}
#endif
#endif
