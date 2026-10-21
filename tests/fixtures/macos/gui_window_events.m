#include "gui_fixture.h"
/* GUI fixture (a): NSApplication + NSWindow + an NSView subclass that echoes
 * every key, mouse, scroll and resize event it receives to stdout.
 *
 *   gui_window_events                 self-test: posts key and mouse events,
 *                                     resizes the window, quits on its own 'q'
 *   gui_window_events --interactive   waits for real input (the harness's
 *     [--timeout <seconds>]           SendInput) and quits on the 'q' key, a
 *                                     window close or the timeout (exit 3)
 *
 * Output lines (stable, parsed by tests and the harness):
 *   gui_window_events: ready window=<w>x<h> scale=<s>
 *   gui_window_events: key down keyCode=<n> chars=<c> mods=0x<m> repeat=<0|1>
 *   gui_window_events: key up keyCode=<n>
 *   gui_window_events: flags mods=0x<m>
 *   gui_window_events: mouse <down|up|moved|dragged> button=<n> clicks=<n> at <x>,<y>
 *   gui_window_events: scroll dx=<x> dy=<y> precise=<0|1>
 *   gui_window_events: resize <w>x<h>
 *   gui_window_events: done */

static BOOL finished;
static NSView *echo_view;

@interface EchoView : NSView
@end
@implementation EchoView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)mouse:(const char *)what event:(NSEvent *)event {
    NSPoint point = [event locationInWindow];
    gui_log("mouse %s button=%ld clicks=%ld at %.0f,%.0f", what, (long)[event buttonNumber], (long)[event clickCount],
            point.x, point.y);
}
- (void)mouseDown:(NSEvent *)event { [self mouse:"down" event:event]; }
- (void)mouseUp:(NSEvent *)event { [self mouse:"up" event:event]; }
- (void)rightMouseDown:(NSEvent *)event { [self mouse:"down" event:event]; }
- (void)rightMouseUp:(NSEvent *)event { [self mouse:"up" event:event]; }
- (void)otherMouseDown:(NSEvent *)event { [self mouse:"down" event:event]; }
- (void)otherMouseUp:(NSEvent *)event { [self mouse:"up" event:event]; }
- (void)mouseMoved:(NSEvent *)event { [self mouse:"moved" event:event]; }
- (void)mouseDragged:(NSEvent *)event { [self mouse:"dragged" event:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self mouse:"dragged" event:event]; }
- (void)scrollWheel:(NSEvent *)event {
    gui_log("scroll dx=%.2f dy=%.2f precise=%d", [event scrollingDeltaX], [event scrollingDeltaY],
            [event hasPreciseScrollingDeltas] ? 1 : 0);
}
- (void)keyDown:(NSEvent *)event {
    NSString *characters = [event characters];
    const char *text = characters ? [characters UTF8String] : "";
    gui_log("key down keyCode=%u chars=%s mods=0x%lx repeat=%d", (unsigned)[event keyCode], text ? text : "",
            (unsigned long)[event modifierFlags], [event isARepeat] ? 1 : 0);
    if ([event keyCode] == 12) finished = YES;   /* kVK_ANSI_Q */
}
- (void)keyUp:(NSEvent *)event { gui_log("key up keyCode=%u", (unsigned)[event keyCode]); }
- (void)flagsChanged:(NSEvent *)event { gui_log("flags mods=0x%lx", (unsigned long)[event modifierFlags]); }
@end

@interface EchoDelegate : NSObject <NSWindowDelegate, NSApplicationDelegate>
@end
@implementation EchoDelegate
- (void)windowDidResize:(id)notification {
    NSRect content = [echo_view frame];
    gui_log("resize %.0fx%.0f", content.size.width, content.size.height);
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    gui_log("close requested");
    finished = YES;
    return YES;
}
- (void)applicationDidFinishLaunching:(id)notification { gui_log("launched"); }
@end

int main(int argc, char **argv) {
    gui_fixture_name = "gui_window_events";
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    gui_log("start");
    BOOL interactive = gui_has_argument(argc, argv, "--interactive");
    long timeout = gui_argument_value(argc, argv, "--timeout", 60);

    NSApplication *application = [NSApplication sharedApplication];
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    EchoDelegate *delegate = [[EchoDelegate alloc] init];
    [application setDelegate:delegate];
    [application finishLaunching];

    EchoView *view = [[EchoView alloc] initWithFrame:NSMakeRect(0, 0, 480, 320)];
    echo_view = view;
    NSWindow *window = gui_window([NSString stringWithUTF8String:"gui_window_events"], NSMakeRect(120, 120, 480, 320), view);
    [window setDelegate:delegate];
    [window setAcceptsMouseMovedEvents:YES];
    [application activateIgnoringOtherApps:YES];
    gui_pump(application, 0.1);
    NSRect content = [view frame];
    gui_log("ready window=%.0fx%.0f scale=%.1f", content.size.width, content.size.height, [window backingScaleFactor]);

    if (!interactive) {
        /* Self-test: a resize, a click in the view and keys, ending with 'q'. */
        [window setContentSize:NSMakeSize(500, 340)];
        gui_pump(application, 0.05);
        NSInteger number = [window windowNumber];
        for (int phase = 0; phase < 2; ++phase) {
            NSEvent *click = [NSEvent mouseEventWithType:phase ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDown
                                                location:NSMakePoint(40, 30) modifierFlags:0 timestamp:0
                                            windowNumber:number context:nil eventNumber:0 clickCount:1
                                                pressure:phase ? 0.0f : 1.0f];
            [application postEvent:click atStart:NO];
        }
        gui_post_key(application, window, 0, "a", 0);    /* kVK_ANSI_A */
        gui_post_key(application, window, 12, "q", 0);   /* kVK_ANSI_Q */
    }
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:(NSTimeInterval)timeout];
    while (!finished && [deadline timeIntervalSinceNow] > 0) gui_pump(application, 0.05);
    if (!finished) {
        gui_log("timeout");
        return 3;
    }
    [window close];
    gui_log("done");
    [pool drain];
    return 0;
}
