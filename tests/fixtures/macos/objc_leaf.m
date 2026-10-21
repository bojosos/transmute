#include "objc_leaf.h"
int alchemy_leaf_loads, alchemy_leaf_initializations;
@implementation AlchemyLeaf
+ (void)load { ++alchemy_leaf_loads; }
+ (void)initialize { ++alchemy_leaf_initializations; }
- (unsigned long)value { return 12; }
@end
id alchemy_leaf_create(void) { return [[AlchemyLeaf alloc] init]; }
