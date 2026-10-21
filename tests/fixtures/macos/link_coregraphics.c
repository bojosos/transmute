#include <CoreGraphics/CoreGraphics.h>
/* Link-only CoreGraphics identity (install name
 * /System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics).
 * These bodies are never mapped: Alchemy's providers replace the image. */
const CFStringRef kCGColorSpaceSRGB;
const CFStringRef kCGColorSpaceDisplayP3;
const CFStringRef kCGColorSpaceExtendedLinearSRGB;
CGError CGGetActiveDisplayList(uint32_t maxDisplays, CGDirectDisplayID *activeDisplays, uint32_t *displayCount) {
    (void)maxDisplays; (void)activeDisplays; (void)displayCount; return kCGErrorFailure;
}
CGDirectDisplayID CGMainDisplayID(void) { return 0; }
CGRect CGDisplayBounds(CGDirectDisplayID display) { (void)display; return CGRectMake(0, 0, 0, 0); }
size_t CGDisplayPixelsWide(CGDirectDisplayID display) { (void)display; return 0; }
size_t CGDisplayPixelsHigh(CGDirectDisplayID display) { (void)display; return 0; }
CGError CGWarpMouseCursorPosition(CGPoint point) { (void)point; return kCGErrorFailure; }
CGError CGAssociateMouseAndMouseCursorPosition(bool connected) { (void)connected; return kCGErrorFailure; }
CGError CGDisplayMoveCursorToPoint(CGDirectDisplayID display, CGPoint point) {
    (void)display; (void)point; return kCGErrorFailure;
}
CGColorSpaceRef CGColorSpaceCreateWithName(CFStringRef name) { (void)name; return (CGColorSpaceRef)0; }
CGColorSpaceRef CGColorSpaceRetain(CGColorSpaceRef space) { return space; }
void CGColorSpaceRelease(CGColorSpaceRef space) { (void)space; }
