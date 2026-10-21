#include "objc_runtime_shared.h"
extern int printf(const char *, ...);
extern int strcmp(const char *, const char *);
extern void free(void *);
typedef struct objc_property *objc_property_t;
extern Class object_getClass(id object);
extern objc_property_t class_getProperty(Class cls, const char *name);
extern objc_property_t *class_copyPropertyList(Class cls, unsigned int *count);
extern const char *property_getName(objc_property_t property);
extern const char *property_getAttributes(objc_property_t property);
@interface AlchemyDerived : AlchemyGreeter { id child; }
- (void)adopt:(id)object;
@end
@implementation AlchemyDerived
- (long)value:(long)value { return [super value:value] + 13; }
- (void)adopt:(id)object { child = object; }
@end
@interface AlchemyGreeter (Category)
@property (nonatomic, readonly) long categoryAnswer;
- (long)categoryValue;
@end
@implementation AlchemyGreeter (Category)
- (long)categoryAnswer { return 42; }
- (long)categoryValue { return 42; }
@end
// Declared properties: the class's own and its categories' (categories
// first), found through superclasses; a host class and an unknown name
// answer NULL; class properties live on the metaclass.
static int properties(void) {
    Class greeter = [AlchemyGreeter class], derived = [AlchemyDerived class];
    objc_property_t answer = class_getProperty(greeter, "answer");
    if (!answer || strcmp(property_getName(answer), "answer") || strcmp(property_getAttributes(answer), "Tq,R,N")) return 11;
    if (class_getProperty(derived, "answer") != answer) return 12;
    objc_property_t category = class_getProperty(derived, "categoryAnswer");
    if (!category || strcmp(property_getName(category), "categoryAnswer") || strcmp(property_getAttributes(category), "Tq,R,N")) return 13;
    if (class_getProperty(greeter, "missing") || class_getProperty([NSObject class], "answer") || class_getProperty(greeter, "contextId")) return 14;
    objc_property_t loads = class_getProperty(object_getClass((id)greeter), "loads");
    if (!loads || strcmp(property_getName(loads), "loads") || strcmp(property_getAttributes(loads), "Tq,R") || class_getProperty(greeter, "loads")) return 15;
    unsigned int count = 0;
    objc_property_t *list = class_copyPropertyList(greeter, &count);
    if (count != 2 || !list || list[0] != category || list[1] != answer || list[2]) return 16;
    free(list);
    if (class_copyPropertyList(derived, &count) || count) return 17;
    return 0;
}
int main(void) {
    @autoreleasepool {
        if ([AlchemyGreeter loads] != 1 || [AlchemyGreeter initializations] != 1) return 1;
        AlchemyDerived *object = [[AlchemyDerived alloc] init];
        if ([object value:7] != 27 || [object categoryValue] != 42) return 2;
        if ([object floating:1.25 other:2.5] != 3.75) return 3;
        ObjcHfa hfa = [object hfa:(ObjcHfa){1,2,3}];
        if (hfa.a != 2 || hfa.b != 4 || hfa.c != 6) return 4;
        ObjcLarge large = [object large:25];
        if (large.a != 25 || large.b != 26 || large.c != 27) return 5;
        if ([object sum:1 b:2 c:3 d:4 e:5 f:6 g:7 h:8] != 36) return 6;
        AlchemyGreeter *nothing = nil;
        if ([nothing value:9] || [nothing floating:3 other:4]) return 7;
        [object adopt:alchemy_objc_make()];
        if (![object isKindOfClass:[AlchemyGreeter class]]) return 8;
        if (![object respondsToSelector:@selector(categoryValue)]) return 9;
        if ([object answer] != 42 || [object categoryAnswer] != 42) return 18;
    }
    const int declared = properties();
    if (declared) return declared;
    if (alchemy_objc_destroyed != 2) return 10;
    printf("Objective-C runtime: 27 42\n");
    return 0;
}
