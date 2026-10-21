#include "objc_leaf.h"
static int plugin_loads;
@interface AlchemyPlugin : AlchemyLeaf
@end
@implementation AlchemyPlugin
+ (void)load { ++plugin_loads; }
- (unsigned long)value { return [super value] + 30; }
@end
@interface AlchemyLeaf (PluginAddition)
- (unsigned long)pluginValue;
@end
@implementation AlchemyLeaf (PluginAddition)
+ (void)load { ++plugin_loads; }
- (unsigned long)pluginValue { return [self value] + 15; }
@end
Class alchemy_plugin_class(void) { return [AlchemyPlugin class]; }
int alchemy_plugin_loads(void) { return plugin_loads; }
