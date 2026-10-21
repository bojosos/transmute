#ifndef ALCHEMY_GUI_FIXTURE_H
#define ALCHEMY_GUI_FIXTURE_H
/* Shared helpers for the project-authored GUI fixture apps (gui_*.m).
 * Every line a fixture prints is "<name>: <message>", flushed at once so the
 * harness (scripts/alchemy_gui_harness.ps1) can follow stdout live. */
#include <AppKit/AppKit.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *gui_fixture_name = "gui";

static void gui_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void gui_log(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    printf("%s: ", gui_fixture_name);
    vfprintf(stdout, format, arguments);
    printf("\n");
    va_end(arguments);
    fflush(stdout);
}

/* A required capability is missing: report it as an explicit unsupported
 * stop (the fixture test accepts that until the providers exist). */
static int gui_unsupported(const char *what) {
    fprintf(stderr, "%s: unsupported: %s\n", gui_fixture_name, what);
    fflush(stderr);
    return 2;
}

static BOOL gui_has_argument(int argc, char **argv, const char *flag) {
    for (int index = 1; index < argc; ++index)
        if (strcmp(argv[index], flag) == 0) return YES;
    return NO;
}

static long gui_argument_value(int argc, char **argv, const char *flag, long fallback) {
    for (int index = 1; index + 1 < argc; ++index)
        if (strcmp(argv[index], flag) == 0) return strtol(argv[index + 1], 0, 10);
    return fallback;
}

/* Dispatches every queued event, waiting up to `seconds` for the first. */
static void gui_pump(NSApplication *application, NSTimeInterval seconds) {
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    for (;;) {
        NSEvent *event = [application nextEventMatchingMask:NSEventMaskAny untilDate:until
                                                     inMode:NSDefaultRunLoopMode dequeue:YES];
        if (!event) break;
        [application sendEvent:event];
        until = [NSDate distantPast];
    }
}

/* A titled, resizable window with `view` as its content view, shown and
 * made key. */
static NSWindow *gui_window(NSString *title, NSRect content, NSView *view) {
    NSWindow *window = [[NSWindow alloc] initWithContentRect:content
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                  NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:title];
    [window setReleasedWhenClosed:NO];
    [window setContentView:view];
    [window makeFirstResponder:view];
    [window makeKeyAndOrderFront:nil];
    return window;
}

/* Posts a synthetic key press (down + up) to the key window. */
static void gui_post_key(NSApplication *application, NSWindow *window, unsigned short key_code, const char *characters,
                         NSEventModifierFlags flags) {
    NSString *text = [NSString stringWithUTF8String:characters];
    for (int phase = 0; phase < 2; ++phase) {
        NSEvent *event = [NSEvent keyEventWithType:phase ? NSEventTypeKeyUp : NSEventTypeKeyDown
                                          location:NSMakePoint(0, 0) modifierFlags:flags timestamp:0
                                      windowNumber:[window windowNumber] context:nil characters:text
                       charactersIgnoringModifiers:text isARepeat:NO keyCode:key_code];
        [application postEvent:event atStart:NO];
    }
}
#endif
