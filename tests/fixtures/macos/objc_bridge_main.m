#include "objc_leaf.h"
@interface AlchemyDerived : AlchemyLeaf
@end
@implementation AlchemyDerived
- (unsigned long)value { return [super value] + 15; }
@end
int main(void) {
    if (alchemy_leaf_loads != 1) return 1;
    id leaf = alchemy_leaf_create();
    AlchemyDerived *derived = [[AlchemyDerived alloc] init];
    if ([leaf value] != 12 || [derived value] != 27 || alchemy_leaf_initializations != 2) return 2;
    if (strcmp(class_getName([derived class]), "AlchemyDerived") != 0) return 3;
    [leaf release]; [derived release];
    puts("Objective-C dylib: 12 27");
    return 0;
}
