/* MIT-licensed, project-authored declarations for Alchemy's bounded guest ABI. */
#ifndef ALCHEMY_OBJC_H
#define ALCHEMY_OBJC_H
typedef struct objc_class *Class;
typedef struct objc_selector *SEL;
#ifdef __cplusplus
typedef bool BOOL;
#else
typedef _Bool BOOL;
#endif
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#define nil ((id)0)
#define Nil ((Class)0)
__attribute__((objc_root_class))
@interface NSObject { Class isa; }
+ (id)alloc;
+ (id)allocWithZone:(void *)zone;
+ (id)new;
+ (Class)class;
+ (Class)superclass;
- (id)init;
- (void)dealloc;
- (Class)class;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (unsigned long)retainCount;
- (BOOL)isEqual:(id)object;
- (unsigned long)hash;
- (BOOL)respondsToSelector:(SEL)selector;
- (BOOL)isKindOfClass:(Class)cls;
@end
#endif
