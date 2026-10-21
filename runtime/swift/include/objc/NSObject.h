#ifndef ALCHEMY_OBJC_NSOBJECT_H
#define ALCHEMY_OBJC_NSOBJECT_H
/* Independently authored declarations of the published NSObject root class
   and protocol. Alchemy's libobjc provider implements NSObject. No SDK
   content. */
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/NSObjCRuntime.h>
#ifdef __OBJC__
@class NSString, NSMethodSignature, NSInvocation;
struct _NSZone;
@protocol NSObject
- (BOOL)isEqual:(id)object;
@property (readonly) NSUInteger hash;
@property (readonly) Class superclass;
- (Class)class;
- (instancetype)self;
- (id)performSelector:(SEL)aSelector;
- (id)performSelector:(SEL)aSelector withObject:(id)object;
- (id)performSelector:(SEL)aSelector withObject:(id)object1 withObject:(id)object2;
- (BOOL)isProxy;
- (BOOL)isKindOfClass:(Class)aClass;
- (BOOL)isMemberOfClass:(Class)aClass;
- (BOOL)conformsToProtocol:(Protocol*)aProtocol;
- (BOOL)respondsToSelector:(SEL)aSelector;
- (instancetype)retain;
- (oneway void)release;
- (instancetype)autorelease;
- (NSUInteger)retainCount;
- (struct _NSZone*)zone;
@property (readonly, copy) NSString* description;
@optional
@property (readonly, copy) NSString* debugDescription;
@end

OBJC_ROOT_CLASS
@interface NSObject <NSObject> {
    Class isa;
}
+ (void)load;
+ (void)initialize;
- (instancetype)init;
+ (instancetype)new;
+ (instancetype)allocWithZone:(struct _NSZone*)zone;
+ (instancetype)alloc;
- (void)dealloc;
- (id)copy;
- (id)mutableCopy;
+ (id)copyWithZone:(struct _NSZone*)zone;
+ (id)mutableCopyWithZone:(struct _NSZone*)zone;
+ (BOOL)instancesRespondToSelector:(SEL)aSelector;
+ (BOOL)conformsToProtocol:(Protocol*)protocol;
- (IMP)methodForSelector:(SEL)aSelector;
+ (IMP)instanceMethodForSelector:(SEL)aSelector;
- (void)doesNotRecognizeSelector:(SEL)aSelector;
- (id)forwardingTargetForSelector:(SEL)aSelector;
- (void)forwardInvocation:(NSInvocation*)anInvocation;
- (NSMethodSignature*)methodSignatureForSelector:(SEL)aSelector;
+ (NSMethodSignature*)instanceMethodSignatureForSelector:(SEL)aSelector;
- (BOOL)allowsWeakReference;
- (BOOL)retainWeakReference;
+ (BOOL)isSubclassOfClass:(Class)aClass;
+ (BOOL)resolveClassMethod:(SEL)sel;
+ (BOOL)resolveInstanceMethod:(SEL)sel;
+ (NSUInteger)hash;
+ (Class)superclass;
+ (Class)class;
+ (NSString*)description;
+ (NSString*)debugDescription;
@end
#endif
#endif
