#include "alchemy_objc_fixture.h"
@interface AlchemyLeaf : NSObject
- (unsigned long)value;
@end
extern int alchemy_leaf_loads;
extern int alchemy_leaf_initializations;
extern id alchemy_leaf_create(void);
