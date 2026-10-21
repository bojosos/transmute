/* Link-only identities for the AppKit controls guest fixture (owner: AppKit).
 * One source, one dylib per framework (-DCONTROLS_STUB_APPKIT /
 * -DCONTROLS_STUB_CARBON). The bodies are never mapped: Alchemy's providers
 * replace each image by its install name. Project-authored. */
#if defined(CONTROLS_STUB_APPKIT)
#include "controls_fixture.h"
#pragma clang diagnostic ignored "-Wincomplete-implementation"
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
@implementation NSResponder
@end
@implementation NSApplication
@end
@implementation NSView
@end
@implementation NSControl
@end
@implementation NSSlider
@end
@implementation NSStepper
@end
@implementation NSTextField
@end
@implementation NSComboBox
@end
@implementation NSPrintInfo
@end
@implementation NSPasteboardItem
@end
NSApplication *NSApp;
#elif defined(CONTROLS_STUB_CARBON)
#define LINK_ONLY(name) long name(void) { return -777; }
LINK_ONLY(HIShapeCreateWithRect)
LINK_ONLY(HIShapeCreateMutableCopy)
LINK_ONLY(HIShapeCreateIntersection)
LINK_ONLY(HIShapeUnionWithRect)
LINK_ONLY(HIShapeOffset)
LINK_ONLY(HIShapeDifference)
LINK_ONLY(HIShapeGetBounds)
LINK_ONLY(HIShapeContainsPoint)
LINK_ONLY(HIShapeIsRectangular)
LINK_ONLY(HIShapeIsEmpty)
#else
#error "select a stub with -DCONTROLS_STUB_*"
#endif
