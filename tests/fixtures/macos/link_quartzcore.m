#include <QuartzCore/QuartzCore.h>
/* Link-only QuartzCore identity (install name
 * /System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore).
 * Never mapped: Alchemy's QuartzCore provider replaces it by install name. */
#pragma clang diagnostic ignored "-Wincomplete-implementation"
@implementation CALayer
@end
@implementation CAMetalLayer
@end
@implementation CATransaction
@end
CFTimeInterval CACurrentMediaTime(void) { return 0; }
