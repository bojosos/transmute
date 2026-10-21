#include <Metal/Metal.h>
/* Link-only Metal identity (install name
 * /System/Library/Frameworks/Metal.framework/Versions/A/Metal). Never mapped:
 * Alchemy's Metal provider replaces it by install name. */
#pragma clang diagnostic ignored "-Wincomplete-implementation"
@implementation MTLRenderPassColorAttachmentDescriptor
@end
@implementation MTLRenderPassColorAttachmentDescriptorArray
@end
@implementation MTLRenderPassDescriptor
@end
@implementation MTLRenderPipelineColorAttachmentDescriptor
@end
@implementation MTLRenderPipelineColorAttachmentDescriptorArray
@end
@implementation MTLRenderPipelineDescriptor
@end
@implementation MTLTextureDescriptor
@end
@implementation MTLCompileOptions
@end
@implementation MTLSamplerDescriptor
@end
id<MTLDevice> MTLCreateSystemDefaultDevice(void) { return nil; }
NSArray *MTLCopyAllDevices(void) { return (NSArray *)nil; }
