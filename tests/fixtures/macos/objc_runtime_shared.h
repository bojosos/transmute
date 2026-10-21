#include "../../../runtime/objc/AlchemyObjC.h"
typedef struct { double a,b,c; } ObjcHfa;
typedef struct { long a,b,c; } ObjcLarge;
@interface AlchemyGreeter : NSObject
@property (class, readonly) long loads;
@property (nonatomic, readonly) long answer;
+ (long)initializations;
- (long)value:(long)value;
- (double)floating:(double)value other:(double)other;
- (ObjcHfa)hfa:(ObjcHfa)value;
- (ObjcLarge)large:(long)value;
- (long)sum:(long)a b:(long)b c:(long)c d:(long)d e:(long)e f:(long)f g:(long)g h:(long)h;
@end
extern long alchemy_objc_destroyed;
extern id alchemy_objc_make(void);
