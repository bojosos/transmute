#include <AppKit/AppKit.h>
/* Link-only AppKit identity (install name
 * /System/Library/Frameworks/AppKit.framework/Versions/C/AppKit). Metadata
 * supplies class and symbol imports to LLD; Alchemy never maps this image and
 * guest execution uses its authored providers. */
#pragma clang diagnostic ignored "-Wincomplete-implementation"
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
@implementation NSResponder
@end
@implementation NSApplication
@end
@implementation NSEvent
@end
@implementation NSView
@end
@implementation NSWindow
@end
@implementation NSScreen
@end
@implementation NSMenu
@end
NSApplication *NSApp;
