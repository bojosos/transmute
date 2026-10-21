#include "gui_fixture.h"
/* GUI fixture (d): an NSView implementing NSTextInputClient (Blender's
 * CocoaMetalView pattern: keyDown: -> interpretKeyEvents:) that prints the
 * committed text, marked (composing) text and commands it receives.
 *
 *   gui_text_input                   self-test: posts the keys h, i, Return
 *   gui_text_input --interactive     waits for real typing or an IME (the
 *     [--timeout <seconds>]          harness), ends on Return or the timeout
 *
 * Output lines:
 *   gui_text_input: insertText "<text>" buffer "<all committed text>"
 *   gui_text_input: setMarkedText "<text>" selected=<location>,<length>
 *   gui_text_input: unmarkText
 *   gui_text_input: command <selector>
 *   gui_text_input: commit buffer "<text>"      (on insertNewline:)
 *   gui_text_input: done */

#define FIXTURE_NOT_FOUND ((NSUInteger)0x7fffffffffffffffULL)

@protocol FixtureAttributedString
- (NSString *)string;
@end

static BOOL finished;

@interface TextView : NSView <NSTextInputClient> {
    char buffer[1024];
    unsigned long length;
    NSString *marked;
    NSRange marked_selection;
}
@end

static const char *utf8_of(id text) {
    if (!text) return "";
    NSString *string = [text isKindOfClass:[NSString class]] ? (NSString *)text
                                                              : [(id<FixtureAttributedString>)text string];
    const char *value = string ? [string UTF8String] : 0;
    return value ? value : "";
}

@implementation TextView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event { [self interpretKeyEvents:[NSArray arrayWithObject:event]]; }
- (void)insertText:(id)string replacementRange:(NSRange)replacement {
    const char *text = utf8_of(string);
    for (const char *cursor = text; *cursor && length + 1 < sizeof buffer; ++cursor) buffer[length++] = *cursor;
    buffer[length] = 0;
    if (marked) { [marked release]; marked = nil; }
    gui_log("insertText \"%s\" buffer \"%s\"", text, buffer);
}
- (void)doCommandBySelector:(SEL)selector {
    NSString *name = NSStringFromSelector(selector);
    gui_log("command %s", name ? [name UTF8String] : "?");
    if (strcmp(name ? [name UTF8String] : "", "insertNewline:") == 0) {
        gui_log("commit buffer \"%s\"", buffer);
        finished = YES;
    }
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)selected replacementRange:(NSRange)replacement {
    if (marked) [marked release];
    marked = [[NSString alloc] initWithUTF8String:utf8_of(string)];
    marked_selection = selected;
    gui_log("setMarkedText \"%s\" selected=%lu,%lu", [marked UTF8String], (unsigned long)selected.location,
            (unsigned long)selected.length);
}
- (void)unmarkText {
    if (marked) { [marked release]; marked = nil; }
    gui_log("unmarkText");
}
- (NSRange)selectedRange { NSRange range = {length, 0}; return range; }
- (NSRange)markedRange {
    NSRange range = {FIXTURE_NOT_FOUND, 0};
    if (marked) { range.location = length; range.length = [marked length]; }
    return range;
}
- (BOOL)hasMarkedText { return marked != nil; }
- (id)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRange *)actual { return nil; }
- (NSArray *)validAttributesForMarkedText { return [NSArray array]; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRange *)actual {
    /* The caret sits at the view's bottom-left corner, in screen space. */
    NSRect frame = [[self window] frame];
    return NSMakeRect(frame.origin.x + 8, frame.origin.y + 8, 1, 16);
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point { return FIXTURE_NOT_FOUND; }
@end

int main(int argc, char **argv) {
    gui_fixture_name = "gui_text_input";
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    gui_log("start");
    BOOL interactive = gui_has_argument(argc, argv, "--interactive");
    long timeout = gui_argument_value(argc, argv, "--timeout", 60);

    NSApplication *application = [NSApplication sharedApplication];
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application finishLaunching];
    TextView *view = [[TextView alloc] initWithFrame:NSMakeRect(0, 0, 360, 120)];
    NSWindow *window = gui_window([NSString stringWithUTF8String:"gui_text_input"], NSMakeRect(200, 200, 360, 120), view);
    [application activateIgnoringOtherApps:YES];
    gui_pump(application, 0.1);
    gui_log("ready");
    if (!interactive) {
        gui_post_key(application, window, 4, "h", 0);     /* kVK_ANSI_H */
        gui_post_key(application, window, 34, "i", 0);    /* kVK_ANSI_I */
        gui_post_key(application, window, 36, "\r", 0);   /* kVK_Return */
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
