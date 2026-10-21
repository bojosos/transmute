#include "alchemy_objc_fixture.h"
typedef unsigned long pthread_t;
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
static int destroyed;
@interface PoolProbe : NSObject
@end
@implementation PoolProbe
- (void)dealloc { __atomic_add_fetch(&destroyed, 1, __ATOMIC_SEQ_CST); [super dealloc]; }
@end
static void *worker(void *argument) {
    @autoreleasepool {
        for (int i = 0; i != 20; ++i) [[[PoolProbe alloc] init] autorelease];
    }
    return argument;
}
int main(void) {
    @autoreleasepool {
        [[[PoolProbe alloc] init] autorelease];
        pthread_t first, second;
        if (pthread_create(&first, 0, worker, (void *)27) || pthread_create(&second, 0, worker, (void *)42)) return 1;
        void *a, *b;
        if (pthread_join(first, &a) || pthread_join(second, &b) || a != (void *)27 || b != (void *)42) return 2;
        if (__atomic_load_n(&destroyed, __ATOMIC_SEQ_CST) != 40) return 3;
    }
    if (destroyed != 41) return 4;
    puts("Objective-C worker pools: 41");
    return 0;
}
