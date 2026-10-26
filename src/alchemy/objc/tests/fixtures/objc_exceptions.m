// MIT-licensed, project-authored Alchemy fixture: Objective-C exceptions are
// C++ ABI exceptions. @throw unwinds through guest frames to the matching
// @catch (by class, then id), @finally runs on both paths, a bare @throw
// rethrows, and a thrown object is released once its handler finishes.
#include "alchemy_objc_fixture.h"
@interface NSObject (AlchemyExceptions)
- (BOOL)isKindOfClass:(Class)cls;
- (unsigned long)retainCount;
@end
@interface Thrown : NSObject
@end
@implementation Thrown
@end
@interface Other : NSObject
@end
@implementation Other
@end
@interface Subclass : Thrown
@end
@implementation Subclass
@end

static int finally_runs;
static Thrown *kept;

__attribute__((noinline)) static int thrower(int depth) {
    if (depth == 0) @throw kept;
    return thrower(depth - 1) + 1;
}

__attribute__((noinline)) static int guarded(void) {
    int result = 0;
    @try {
        result = thrower(3);
    } @finally {
        ++finally_runs;   // runs while the exception passes through
    }
    return result;
}

int main(void) {
    int caught = 0;
    kept = [Thrown new];
    @try {
        guarded();
    } @catch (Other *other) {
        return 1;
    } @catch (Thrown *thrown) {
        if (thrown != kept || ![thrown isKindOfClass:[Thrown class]]) return 2;
        caught += 1;
    } @finally {
        ++finally_runs;
    }
    @try {
        @throw [Other new];
    } @catch (id any) {
        caught += 10;
    }
    @try {
        @try {
            @throw [Subclass new];
        } @catch (Thrown *thrown) {
            @throw;   // rethrows the same exception
        }
    } @catch (Subclass *again) {
        caught += 100;
    }
    @try {
        caught += 1000;
    } @finally {
        ++finally_runs;   // no exception: the normal path
    }
    // Every handler has finished: the exceptions no longer hold their objects.
    if ([kept retainCount] != 1) return 3;
    printf("Objective-C exceptions: %d %d\n", caught, finally_runs);
    return 0;
}
