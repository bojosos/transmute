#ifndef ALCHEMY_APPKIT_H
#define ALCHEMY_APPKIT_H
/* Project-authored declarations for Alchemy's bounded AppKit guest ABI (GUI
 * fixtures). Selectors, layouts and numeric values follow the public
 * interface; no Apple header text is used. Extend as fixtures need. */
#include "../Foundation/Foundation.h"
#include "../CoreGraphics/CoreGraphics.h"

typedef CGPoint NSPoint;
typedef CGSize NSSize;
typedef CGRect NSRect;
static inline NSRect NSMakeRect(CGFloat x, CGFloat y, CGFloat w, CGFloat h) { return CGRectMake(x, y, w, h); }
static inline NSPoint NSMakePoint(CGFloat x, CGFloat y) { return CGPointMake(x, y); }
static inline NSSize NSMakeSize(CGFloat w, CGFloat h) { return CGSizeMake(w, h); }

@class NSWindow, NSView, NSEvent, NSScreen, NSMenu, NSArray, CALayer;

typedef NSUInteger NSWindowStyleMask;
enum {
    NSWindowStyleMaskBorderless = 0, NSWindowStyleMaskTitled = 1 << 0, NSWindowStyleMaskClosable = 1 << 1,
    NSWindowStyleMaskMiniaturizable = 1 << 2, NSWindowStyleMaskResizable = 1 << 3,
    NSWindowStyleMaskFullSizeContentView = 1 << 15
};
typedef NSUInteger NSBackingStoreType;
enum { NSBackingStoreBuffered = 2 };
typedef NSUInteger NSEventType;
enum {
    NSEventTypeLeftMouseDown = 1, NSEventTypeLeftMouseUp = 2, NSEventTypeRightMouseDown = 3,
    NSEventTypeRightMouseUp = 4, NSEventTypeMouseMoved = 5, NSEventTypeLeftMouseDragged = 6,
    NSEventTypeKeyDown = 10, NSEventTypeKeyUp = 11, NSEventTypeFlagsChanged = 12,
    NSEventTypeApplicationDefined = 15, NSEventTypeScrollWheel = 22,
    NSEventTypeRightMouseDragged = 7, NSEventTypeMouseEntered = 8, NSEventTypeMouseExited = 9,
    NSEventTypeOtherMouseDown = 25, NSEventTypeOtherMouseUp = 26, NSEventTypeOtherMouseDragged = 27
};
typedef unsigned long long NSEventMask;
#define NSEventMaskAny (~(NSEventMask)0)
typedef NSUInteger NSEventModifierFlags;
enum {
    NSEventModifierFlagShift = 1 << 17, NSEventModifierFlagControl = 1 << 18,
    NSEventModifierFlagOption = 1 << 19, NSEventModifierFlagCommand = 1 << 20
};
typedef NSInteger NSApplicationActivationPolicy;
enum { NSApplicationActivationPolicyRegular = 0, NSApplicationActivationPolicyAccessory = 1,
       NSApplicationActivationPolicyProhibited = 2 };
typedef NSUInteger NSApplicationTerminateReply;
enum { NSTerminateCancel = 0, NSTerminateNow = 1, NSTerminateLater = 2 };

@interface NSResponder : NSObject
- (BOOL)acceptsFirstResponder;
- (void)mouseDown:(NSEvent *)event;
- (void)mouseUp:(NSEvent *)event;
- (void)mouseMoved:(NSEvent *)event;
- (void)mouseDragged:(NSEvent *)event;
- (void)rightMouseDown:(NSEvent *)event;
- (void)rightMouseUp:(NSEvent *)event;
- (void)rightMouseDragged:(NSEvent *)event;
- (void)otherMouseDown:(NSEvent *)event;
- (void)otherMouseUp:(NSEvent *)event;
- (void)scrollWheel:(NSEvent *)event;
- (void)keyDown:(NSEvent *)event;
- (void)keyUp:(NSEvent *)event;
- (void)flagsChanged:(NSEvent *)event;
- (void)interpretKeyEvents:(NSArray *)events;
@end

@protocol NSApplicationDelegate
@optional
- (void)applicationWillFinishLaunching:(id)notification;
- (void)applicationDidFinishLaunching:(id)notification;
- (NSApplicationTerminateReply)applicationShouldTerminate:(id)sender;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(id)sender;
@end

@interface NSApplication : NSResponder
+ (NSApplication *)sharedApplication;
- (void)setDelegate:(id)delegate;
- (id)delegate;
- (BOOL)setActivationPolicy:(NSApplicationActivationPolicy)policy;
- (void)activateIgnoringOtherApps:(BOOL)flag;
- (void)finishLaunching;
- (void)run;
- (void)stop:(id)sender;
- (void)terminate:(id)sender;
- (NSEvent *)nextEventMatchingMask:(NSEventMask)mask untilDate:(NSDate *)expiration inMode:(NSRunLoopMode)mode
                           dequeue:(BOOL)dequeue;
- (void)sendEvent:(NSEvent *)event;
- (void)postEvent:(NSEvent *)event atStart:(BOOL)flag;
- (NSWindow *)keyWindow;
- (NSArray *)windows;
- (void)setMainMenu:(NSMenu *)menu;
@end
extern NSApplication *NSApp;

@interface NSEvent : NSObject
+ (NSEvent *)otherEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                      timestamp:(NSTimeInterval)time windowNumber:(NSInteger)windowNumber context:(id)context
                        subtype:(short)subtype data1:(NSInteger)data1 data2:(NSInteger)data2;
+ (NSEvent *)keyEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                     timestamp:(NSTimeInterval)time windowNumber:(NSInteger)windowNumber context:(id)context
                    characters:(NSString *)characters charactersIgnoringModifiers:(NSString *)unmodified
                     isARepeat:(BOOL)repeat keyCode:(unsigned short)keyCode;
+ (NSEvent *)mouseEventWithType:(NSEventType)type location:(NSPoint)location modifierFlags:(NSEventModifierFlags)flags
                       timestamp:(NSTimeInterval)time windowNumber:(NSInteger)windowNumber context:(id)context
                     eventNumber:(NSInteger)number clickCount:(NSInteger)clicks pressure:(float)pressure;
+ (NSEventModifierFlags)modifierFlags;
+ (NSPoint)mouseLocation;
- (NSEventType)type;
- (NSEventModifierFlags)modifierFlags;
- (NSTimeInterval)timestamp;
- (NSWindow *)window;
- (NSInteger)windowNumber;
- (NSPoint)locationInWindow;
- (unsigned short)keyCode;
- (NSString *)characters;
- (NSString *)charactersIgnoringModifiers;
- (BOOL)isARepeat;
- (NSInteger)clickCount;
- (NSInteger)buttonNumber;
- (CGFloat)deltaX;
- (CGFloat)deltaY;
- (CGFloat)scrollingDeltaX;
- (CGFloat)scrollingDeltaY;
- (BOOL)hasPreciseScrollingDeltas;
- (float)pressure;
- (NSInteger)data1;
@end

@interface NSView : NSResponder
- (id)initWithFrame:(NSRect)frame;
- (NSRect)frame;
- (NSRect)bounds;
- (NSWindow *)window;
- (void)setWantsLayer:(BOOL)flag;
- (BOOL)wantsLayer;
- (void)setLayer:(CALayer *)layer;
- (CALayer *)layer;
- (CALayer *)makeBackingLayer;
- (void)setNeedsDisplay:(BOOL)flag;
- (void)drawRect:(NSRect)dirtyRect;
- (NSRect)convertRectToBacking:(NSRect)rect;
- (NSPoint)convertPoint:(NSPoint)point fromView:(NSView *)view;
- (void)addSubview:(NSView *)view;
@end

/* NSTextInputClient (text input from key events and input methods). */
@protocol NSTextInputClient
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange;
- (void)doCommandBySelector:(SEL)selector;
- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange;
- (void)unmarkText;
- (NSRange)selectedRange;
- (NSRange)markedRange;
- (BOOL)hasMarkedText;
- (id)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRange *)actualRange;
- (NSArray *)validAttributesForMarkedText;
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRange *)actualRange;
- (NSUInteger)characterIndexForPoint:(NSPoint)point;
@end

@protocol NSWindowDelegate
@optional
- (BOOL)windowShouldClose:(NSWindow *)sender;
- (void)windowWillClose:(id)notification;
- (void)windowDidResize:(id)notification;
- (void)windowDidMove:(id)notification;
- (void)windowDidBecomeKey:(id)notification;
- (void)windowDidResignKey:(id)notification;
- (void)windowDidChangeBackingProperties:(id)notification;
- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize;
@end

@interface NSWindow : NSResponder
- (id)initWithContentRect:(NSRect)contentRect styleMask:(NSWindowStyleMask)style backing:(NSBackingStoreType)backing
                    defer:(BOOL)flag;
- (void)setTitle:(NSString *)title;
- (NSString *)title;
- (void)setDelegate:(id)delegate;
- (id)delegate;
- (void)setContentView:(NSView *)view;
- (NSView *)contentView;
- (NSRect)frame;
- (NSRect)contentRectForFrameRect:(NSRect)frame;
- (NSRect)frameRectForContentRect:(NSRect)content;
- (void)setFrame:(NSRect)frame display:(BOOL)flag;
- (void)setContentSize:(NSSize)size;
- (CGFloat)backingScaleFactor;
- (NSRect)convertRectToBacking:(NSRect)rect;
- (NSInteger)windowNumber;
- (NSScreen *)screen;
- (void)makeKeyAndOrderFront:(id)sender;
- (void)orderFront:(id)sender;
- (void)orderOut:(id)sender;
- (void)close;
- (BOOL)isVisible;
- (BOOL)isKeyWindow;
- (void)setReleasedWhenClosed:(BOOL)flag;
- (void)setAcceptsMouseMovedEvents:(BOOL)flag;
- (BOOL)makeFirstResponder:(NSResponder *)responder;
- (void)center;
@end

@interface NSScreen : NSObject
+ (NSScreen *)mainScreen;
+ (NSArray *)screens;
- (NSRect)frame;
- (NSRect)visibleFrame;
- (CGFloat)backingScaleFactor;
@end

@interface NSMenu : NSObject
- (id)initWithTitle:(NSString *)title;
@end
#endif
