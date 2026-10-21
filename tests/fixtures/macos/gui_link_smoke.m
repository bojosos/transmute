#include <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>
#include <stdio.h>
/* Wave-0 GUI link smoke: links against the AppKit, Metal, QuartzCore and
 * CoreGraphics link stubs and touches one interface of each. Until the GUI
 * providers exist, +[NSApplication sharedApplication] fails explicitly
 * ("unsupported Objective-C message"); the test accepts that stop or a
 * complete run. */
int main(void) {
    uint32_t displays = 99;
    if (CGGetActiveDisplayList(0, 0, &displays) != kCGErrorSuccess) return 2;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    printf("Alchemy GUI link: displays %u, device %s\n", displays, device ? "present" : "none");
    if (![CAMetalLayer class] || ![NSWindow class]) return 3;
    NSApplication *application = [NSApplication sharedApplication];
    if (!application || NSApp != application) return 4;
    puts("Alchemy GUI link: application");
    return 0;
}
