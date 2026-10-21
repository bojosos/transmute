#include "alchemy_objc_fixture.h"
struct LargeResult { unsigned long a, b, c; };
static int destroyed, initialized, loaded;
@interface Arithmetic : NSObject { unsigned long _base; }
- (id)initWithBase:(unsigned long)value;
- (unsigned long)sum:(unsigned long)a b:(unsigned long)b c:(unsigned long)c d:(unsigned long)d
    e:(unsigned long)e f:(unsigned long)f g:(unsigned long)g h:(unsigned long)h;
- (double)scale:(double)value;
- (struct LargeResult)large:(unsigned long)value;
@end
@implementation Arithmetic
+ (void)load { ++loaded; }
+ (void)initialize { ++initialized; }
- (id)initWithBase:(unsigned long)value { _base = value; return self; }
- (unsigned long)sum:(unsigned long)a b:(unsigned long)b c:(unsigned long)c d:(unsigned long)d
    e:(unsigned long)e f:(unsigned long)f g:(unsigned long)g h:(unsigned long)h {
    return _base + a + b + c + d + e + f + g + h;
}
- (double)scale:(double)value { return value * 2.0; }
- (struct LargeResult)large:(unsigned long)value { return (struct LargeResult){value, value + 1, value + 2}; }
- (void)dealloc { ++destroyed; [super dealloc]; }
@end
int main(void) {
    if (loaded != 1 || initialized != 0) return 1;
    Arithmetic *object = [[Arithmetic alloc] initWithBase:6];
    if (initialized != 1 || class_getInstanceSize([Arithmetic class]) != 16) return 2;
    if ([object sum:1 b:2 c:3 d:4 e:5 f:6 g:7 h:8] != 42 || [object scale:13.5] != 27.0) return 3;
    struct LargeResult result = [object large:25];
    if (result.a != 25 || result.b != 26 || result.c != 27) return 4;
    Arithmetic *nothing = nil;
    if ([nothing scale:7.0] != 0.0 || [nothing sum:1 b:2 c:3 d:4 e:5 f:6 g:7 h:8] != 0) return 5;
    [object release];
    if (destroyed != 1) return 6;
    puts("Objective-C dispatch: 42 27");
    return 0;
}
