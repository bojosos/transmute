// MIT-licensed, project-authored Alchemy fixture (owner: A1): Blocks are
// Objective-C objects (messages to global, stack and heap blocks reach the
// Blocks runtime) and objc_sync_enter/objc_sync_exit monitors are recursive
// and owner-checked. Manual reference counting.
#include "alchemy_objc_fixture.h"
@interface NSObject (AlchemyBlocks)
- (BOOL)respondsToSelector:(SEL)selector;
- (BOOL)isKindOfClass:(Class)cls;
- (unsigned long)retainCount;
@end
extern void *_Block_copy(const void *block);
extern void _Block_release(const void *block);
extern int objc_sync_enter(id object);
extern int objc_sync_exit(id object);
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);
typedef long (^Adder)(long);
static long calls;
static long apply(Adder adder, long value) { return adder(value); }
int main(void) {
    long base = 40;
    Adder global = ^long(long value) { return value + 1; };
    Adder stack = ^long(long value) { ++calls; return value + base; };
    if (![(id)stack respondsToSelector:@selector(copy)] || ![(id)global isKindOfClass:[NSObject class]]) return 1;
    if (strcmp(class_getName([(id)stack class]), "__NSStackBlock__") != 0 ||
        strcmp(class_getName([(id)global class]), "__NSGlobalBlock__") != 0) return 2;
    if ([(id)global copy] != (id)global) return 3;
    Adder heap = [(id)stack copy];
    if ((void *)heap == (void *)stack || apply(heap, 2) != 42) return 4;
    if (strcmp(class_getName([(id)heap class]), "__NSMallocBlock__") != 0) return 5;
    if ([(id)heap retain] != (id)heap || [(id)heap retainCount] != 2) return 6;
    [(id)heap release];
    void *pool = objc_autoreleasePoolPush();
    [[(id)heap retain] autorelease];
    if ([(id)heap retainCount] != 2) return 7;
    objc_autoreleasePoolPop(pool);
    if ([(id)heap retainCount] != 1) return 8;
    Adder again = _Block_copy(heap);
    if ((void *)again != (void *)heap || [(id)heap retainCount] != 2) return 9;
    _Block_release(again);
    [(id)heap release];
    if ([(id)stack retain] != (id)stack) return 10;
    [(id)stack release];
    id lock = [[NSObject alloc] init];
    if (objc_sync_enter(lock) || objc_sync_enter(lock) || objc_sync_exit(lock) || objc_sync_exit(lock)) return 11;
    if (objc_sync_exit(lock) != -1 || objc_sync_enter(nil) || objc_sync_exit(nil)) return 12;
    [lock release];
    printf("Objective-C blocks: %ld %ld\n", apply(global, 26), calls);
    return 0;
}
