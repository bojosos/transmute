#include "alchemy_objc_fixture.h"

/* Protocol metadata is supported: canonical @protocol identity, inheritance and
   class/category adoption. Objective-C exceptions remain an explicit runtime failure. */
@interface Protocol : NSObject
- (const char *)name;
- (BOOL)conformsTo:(Protocol *)other;
@end
@interface NSObject (AlchemyProtocols)
+ (BOOL)conformsToProtocol:(Protocol *)protocol;
- (BOOL)conformsToProtocol:(Protocol *)protocol;
@end
@protocol AlchemyBaseProtocol
- (unsigned long)value;
@end
@protocol AlchemyDerivedProtocol <AlchemyBaseProtocol>
- (unsigned long)other;
@end
@protocol AlchemyCategoryProtocol
@end
@interface AlchemyAdopter : NSObject <AlchemyDerivedProtocol>
@end
@implementation AlchemyAdopter
- (unsigned long)value { return 27; }
- (unsigned long)other { return 42; }
@end
@interface AlchemyAdopter (AlchemyExtra) <AlchemyCategoryProtocol>
@end
@implementation AlchemyAdopter (AlchemyExtra)
@end
@interface AlchemyLeaf : AlchemyAdopter
@end
@implementation AlchemyLeaf
@end
extern void objc_exception_throw(id exception);

int main(void) {
    Protocol *base = @protocol(AlchemyBaseProtocol);
    id leaf = [[AlchemyLeaf alloc] init];
    int conforms = [leaf conformsToProtocol:base] && [AlchemyLeaf conformsToProtocol:@protocol(AlchemyCategoryProtocol)] &&
        ![NSObject conformsToProtocol:base];
    int inherits = [@protocol(AlchemyDerivedProtocol) conformsTo:base] && ![base conformsTo:@protocol(AlchemyDerivedProtocol)];
    int named = strcmp([base name], "AlchemyBaseProtocol") == 0;
    printf("Objective-C protocols: %d %d %d %lu\n", conforms, inherits, named, [leaf value]);
    objc_exception_throw(leaf);
    puts("unsupported metadata executed");
    return 0;
}
