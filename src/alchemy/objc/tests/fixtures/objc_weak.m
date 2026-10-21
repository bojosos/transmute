// MIT-licensed, project-authored Alchemy fixture (owner: A1): ARC weak
// references (objc_initWeak, objc_loadWeakRetained, objc_copyWeak,
// objc_destroyWeak) are cleared by deallocation, and ARC block retains
// (objc_retainBlock) copy blocks to the heap.
#include "alchemy_objc_fixture.h"
static int destroyed;
@interface Tracked : NSObject
@end
@implementation Tracked
- (void)dealloc { ++destroyed; }
@end
typedef long (^Getter)(void);
static Getter make_getter(long seed) { return ^long(void) { return seed + 1; }; }
static Getter keep;
static int exercise(long *loaded) {
    __weak id weak;
    @autoreleasepool {
        id strong = [[Tracked alloc] init];
        weak = strong;
        __weak id copied = weak;
        if (weak != strong || copied != strong) return 1;
        *loaded = weak == strong ? 1 : 0;
        keep = make_getter(41);
        strong = nil;
    }
    if (destroyed != 1 || weak != nil) return 2;
    return 0;
}
int main(void) {
    long loaded = 0;
    const int result = exercise(&loaded);
    if (result) return result;
    printf("Objective-C weak: %ld %ld %d\n", keep(), loaded, destroyed);
    return 0;
}
