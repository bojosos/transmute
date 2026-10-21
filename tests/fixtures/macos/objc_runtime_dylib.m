#include "objc_runtime_shared.h"
long alchemy_objc_destroyed;
static long loads, initializations;
@implementation AlchemyGreeter
+ (void)load { ++loads; }
+ (void)initialize { ++initializations; }
+ (long)loads { return loads; }
+ (long)initializations { return initializations; }
- (long)answer { return 42; }
- (long)value:(long)value { return value + 7; }
- (double)floating:(double)value other:(double)other { return value + other; }
- (ObjcHfa)hfa:(ObjcHfa)value { value.a += 1; value.b += 2; value.c += 3; return value; }
- (ObjcLarge)large:(long)value { return (ObjcLarge){value, value + 1, value + 2}; }
- (long)sum:(long)a b:(long)b c:(long)c d:(long)d e:(long)e f:(long)f g:(long)g h:(long)h { return a+b+c+d+e+f+g+h; }
- (void)dealloc { ++alchemy_objc_destroyed; }
@end
id alchemy_objc_make(void) { return [[AlchemyGreeter alloc] init]; }
