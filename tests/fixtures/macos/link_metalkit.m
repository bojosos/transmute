#include <MetalKit/MetalKit.h>
/* Link-only MetalKit identity (install name
 * /System/Library/Frameworks/MetalKit.framework/Versions/A/MetalKit). Never
 * mapped: Alchemy's MetalKit provider replaces it by install name. */
#pragma clang diagnostic ignored "-Wincomplete-implementation"
@implementation MTKView
@end
