#include <Foundation/Foundation.h>
#include <stdio.h>
/* Foundation's exceptions and message forwarding under alchemy-fx
 * (darwin/foundation/exceptions.cpp, invocation.cpp): -[NSException raise]
 * and +raise:format: throw through the guest libc++abi to @catch
 * (NSException *) with their name and reason, @finally runs, and a message no
 * method answers reaches -forwardInvocation: through Foundation's forward
 * handler, its return value becoming the message's result. */

@interface NSException : NSObject
+ (id)exceptionWithName:(NSString *)name reason:(NSString *)reason userInfo:(id)userInfo;
+ (void)raise:(NSString *)name format:(NSString *)format, ...;
- (void)raise;
- (NSString *)name;
- (NSString *)reason;
@end
@interface NSMethodSignature : NSObject
+ (id)signatureWithObjCTypes:(const char *)types;
@end
@interface NSInvocation : NSObject
- (SEL)selector;
- (void)getArgument:(void *)buffer atIndex:(NSInteger)index;
- (void)setReturnValue:(void *)buffer;
@end
@interface NSObject (ExceptionsFixture)
- (id)methodSignatureForSelector:(SEL)selector;
- (long)tripled:(long)value;
- (double)halved:(double)value;
@end

@interface Forwarder : NSObject
@end
@implementation Forwarder
- (id)methodSignatureForSelector:(SEL)selector {
    if (selector == @selector(tripled:)) return [NSMethodSignature signatureWithObjCTypes:"q@:q"];
    if (selector == @selector(halved:)) return [NSMethodSignature signatureWithObjCTypes:"d@:d"];
    return [super methodSignatureForSelector:selector];
}
- (void)forwardInvocation:(NSInvocation *)invocation {
    if ([invocation selector] == @selector(halved:)) {
        double real = 0;
        [invocation getArgument:&real atIndex:2];
        real /= 2;
        [invocation setReturnValue:&real];
        return;
    }
    long integer = 0;
    [invocation getArgument:&integer atIndex:2];
    integer *= 3;
    [invocation setReturnValue:&integer];
}
@end

static int finally_runs;

__attribute__((noinline)) static void raise_range(int depth) {
    if (depth == 0) [NSException raise:@"NSRangeException" format:@"index %d beyond bounds", 7];
    raise_range(depth - 1);
}

int main(void) {
    printf("Alchemy Foundation exceptions fixture: launched\n");
    int caught = 0;
    @try {
        [[NSException exceptionWithName:@"NSGenericException" reason:@"plain" userInfo:nil] raise];
    } @catch (NSException *exception) {
        if (![[exception name] isEqualToString:@"NSGenericException"] || ![[exception reason] isEqualToString:@"plain"]) return 1;
        caught += 1;
    } @finally {
        ++finally_runs;
    }
    @try {
        raise_range(3);
    } @catch (NSException *exception) {
        if (![[exception name] isEqualToString:@"NSRangeException"]) return 2;
        if (![[exception reason] isEqualToString:@"index 7 beyond bounds"]) return 3;
        caught += 10;
    }
    Forwarder *forwarder = [Forwarder new];
    if ([forwarder tripled:14] != 42) return 4;
    if ([forwarder halved:5.0] != 2.5) return 5;
    caught += 100;
    printf("Alchemy Foundation exceptions fixture: %d %d\n", caught, finally_runs);
    printf("Alchemy Foundation exceptions fixture: complete\n");
    return 0;
}
