#include <AppKit/AppKit.h>
#include <stdio.h>
#include <string.h>
/* AppKit guest fixture (owner: A4). Real ARM64 guest subclasses of the host
 * AppKit classes, as Blender has (BlenderWindow, CocoaMetalView,
 * CocoaAppDelegate): the principal NSApplication subclass, delegate
 * callbacks, the responder chain from a host NSView up to a guest view,
 * guest overrides consulted by host code (-isFlipped, -frame with an NSRect
 * return, -sendEvent:), NSTextInputClient key bindings with NSRange
 * arguments, stack-packed +keyEventWithType: arguments and the event queue.
 * Needs no window server. */

@interface NSResponder (AlchemyFixture)
- (NSResponder *)nextResponder;
- (void)interpretKeyEvents:(NSArray *)events;
@end
@interface NSView (AlchemyFixture)
- (NSView *)hitTest:(NSPoint)point;
- (NSView *)superview;
- (NSRect)convertRect:(NSRect)rect toView:(NSView *)view;
@end
@interface NSEvent (AlchemyFixture)
+ (NSEvent *)keyEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                    timestamp:(NSTimeInterval)time windowNumber:(NSInteger)number context:(id)context
                   characters:(NSString *)characters charactersIgnoringModifiers:(NSString *)unmodified
                    isARepeat:(BOOL)repeat keyCode:(unsigned short)code;
@end

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("Alchemy AppKit fixture: FAIL %s\n", what); failures++; }
}

@interface FixtureDelegate : NSObject { @public int will, did; }
@end
@implementation FixtureDelegate
- (void)applicationWillFinishLaunching:(id)notification { will++; }
- (void)applicationDidFinishLaunching:(id)notification { did += will; }   /* records the order */
@end

@interface FixtureApplication : NSApplication { @public int sent; }
@end
@implementation FixtureApplication
- (void)sendEvent:(NSEvent *)event { sent++; [super sendEvent:event]; }
@end

@interface Container : NSView {
@public
    int keys, inserted, frames;
    unsigned short code;
    NSRange range;
    char text[16];
}
@end
@implementation Container
- (BOOL)isFlipped { return YES; }
- (NSRect)frame { frames++; return [super frame]; }
- (void)keyDown:(NSEvent *)event { keys++; code = [event keyCode]; }
- (void)insertText:(id)string replacementRange:(NSRange)replacement {
    const char *bytes = [string UTF8String];
    unsigned long index = 0;
    for (; bytes[index] && index + 1 < sizeof(text); ++index) text[index] = bytes[index];
    text[index] = 0;
    inserted++;
    range = replacement;
}
- (void)doCommandBySelector:(SEL)selector {}
@end

int main(void) {
    FixtureApplication *app = (FixtureApplication *)[FixtureApplication sharedApplication];
    check(app == (FixtureApplication *)NSApp && [app isKindOfClass:[FixtureApplication class]], "principal class");
    FixtureDelegate *delegate = [[FixtureDelegate alloc] init];
    [app setDelegate:delegate];
    [app finishLaunching];
    check(delegate->will == 1 && delegate->did == 1, "launch callbacks in order");
    puts("Alchemy AppKit fixture: launched");

    Container *container = [[Container alloc] initWithFrame:NSMakeRect(10, 20, 200, 100)];
    NSView *child = [[NSView alloc] initWithFrame:NSMakeRect(5, 5, 50, 40)];
    [container addSubview:child];
    check([child superview] == container && [child nextResponder] == container, "view hierarchy");
    /* The container is flipped: the child covers y 5..45 from its top, which
     * is y 75..115 of the container's (unflipped) superview space. */
    NSView *hit = [container hitTest:NSMakePoint(30, 100)];
    check(hit == child, "hitTest: through a flipped guest view");
    check(container->frames > 0, "host code read the guest -frame override");
    check([container hitTest:NSMakePoint(30, 30)] == container, "hitTest: beside the child");
    check([container hitTest:NSMakePoint(30, 125)] == nil, "hitTest: outside the view");
    /* The unflipped child's bottom-left is at y 45 of the flipped container. */
    NSRect converted = [child convertRect:NSMakeRect(0, 0, 10, 10) toView:container];
    check(converted.origin.x == 5 && converted.origin.y == 35 && converted.size.width == 10 && converted.size.height == 10,
          "-convertRect:toView: across a flipped superview");
    NSRect backing = [container convertRectToBacking:NSMakeRect(0, 0, 3, 4)];
    check(backing.size.width > 0 && backing.size.width * 4 == backing.size.height * 3, "-convertRectToBacking:");

    NSEvent *key = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSMakePoint(0, 0) modifierFlags:0 timestamp:1
                                windowNumber:0 context:nil characters:[NSString stringWithUTF8String:"x"]
                 charactersIgnoringModifiers:[NSString stringWithUTF8String:"x"] isARepeat:NO keyCode:7];
    check(key != nil && [key keyCode] == 7 && ![key isARepeat], "+keyEventWithType: stack arguments");
    [child keyDown:key];
    check(container->keys == 1 && container->code == 7, "responder chain from a host view to its guest superview");
    [container interpretKeyEvents:[NSArray arrayWithObject:key]];
    check(container->inserted == 1 && strcmp(container->text, "x") == 0 && container->range.location == (NSUInteger)0x7fffffffffffffff &&
          container->range.length == 0, "-interpretKeyEvents: -> insertText:replacementRange:");

    [app postEvent:key atStart:NO];
    NSEvent *back = [app nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                        inMode:[NSString stringWithUTF8String:"kCFRunLoopDefaultMode"] dequeue:YES];
    check(back == key, "event queue");
    [app sendEvent:back];
    check(app->sent == 1, "guest -sendEvent: override");
    if (failures) return 1;
    puts("Alchemy AppKit fixture: complete");
    return 0;
}
