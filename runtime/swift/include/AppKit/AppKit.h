#ifndef ALCHEMY_SWIFT_APPKIT_H
#define ALCHEMY_SWIFT_APPKIT_H
/* AppKit for Swift runtime builds: the classes the project's Swift overlays
   (SwiftUI) use, over this directory's Foundation module. It is separate from
   runtime/include/AppKit, which builds on the other Foundation header.
   Written from Apple's public AppKit documentation; no SDK content. Each
   declaration promises only what Alchemy's AppKit provider does (fw-ui reviews
   changes):
   - -[NSApplication run] runs the real event loop;
   - NSApplication's delegate is not retained, as AppKit's isn't; it is an
     unretained reference, not a zeroing one, so the app keeps its delegate
     alive while it is set;
   - -[NSView drawRect:] is the software drawing path;
   - -[NSViewController view] loads the controller's nib or storyboard view and
     fails explicitly without one;
   - -[NSColor CGColor] and -[NSImage initWithSize:] are real. */
#import <Foundation/Foundation.h>
#include <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

typedef CGPoint NSPoint;
typedef CGSize NSSize;
typedef CGRect NSRect;

/* CoreText's font type (a Core Foundation type), which AppKit brings in. It moves to a CoreText module when one
   exists, which can repeat this identical typedef. */
typedef const struct __CTFont *CTFontRef;

@class NSWindow;

@interface NSResponder : NSObject
- (BOOL)acceptsFirstResponder;
@end

@protocol NSApplicationDelegate <NSObject>
@optional
- (void)applicationWillFinishLaunching:(NSNotification*)notification;
- (void)applicationDidFinishLaunching:(NSNotification*)notification;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(id)sender;
@end

@interface NSApplication : NSResponder
@property (class, readonly, strong) NSApplication* sharedApplication;
@property (nullable, weak) id<NSApplicationDelegate> delegate;
- (void)finishLaunching;
- (void)run;
- (void)terminate:(nullable id)sender;
@end

@interface NSView : NSResponder
- (instancetype)initWithFrame:(NSRect)frameRect;
- (nullable instancetype)initWithCoder:(NSCoder*)coder;
@property NSRect frame;
@property (readonly) NSRect bounds;
@property (nullable, readonly, weak) NSWindow* window;
- (void)setNeedsDisplay:(BOOL)needsDisplay;
- (void)drawRect:(NSRect)dirtyRect;
- (void)addSubview:(NSView*)view;
@end

@interface NSViewController : NSResponder
- (instancetype)initWithNibName:(nullable NSString*)nibNameOrNil bundle:(nullable NSBundle*)nibBundleOrNil;
- (nullable instancetype)initWithCoder:(NSCoder*)coder;
@property (strong) NSView* view;
@end

@interface NSWindow : NSResponder
@end

@interface NSColor : NSObject
@property (readonly) CGColorRef CGColor;
@end

@interface NSImage : NSObject
- (instancetype)initWithSize:(NSSize)size;
@end

NS_ASSUME_NONNULL_END
#endif
