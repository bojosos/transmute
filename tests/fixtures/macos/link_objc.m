#include "alchemy_objc_fixture.h"
/* Linking declarations only. Alchemy replaces this library by install identity. */
@implementation NSObject
+ (id)alloc { return nil; }
+ (id)new { return nil; }
+ (Class)class { return (Class)0; }
- (id)init { return self; }
- (Class)class { return (Class)0; }
- (id)retain { return self; }
- (oneway void)release {}
- (id)autorelease { return self; }
- (void)dealloc {}
- (BOOL)isEqual:(id)other { return self == other; }
- (NSUInteger)hash { return (NSUInteger)self; }
- (id)copy { return self; }
@end
unsigned long _objc_empty_cache[2];
id objc_msgSend(id receiver, SEL selector, ...) { (void)selector; return receiver; }
id objc_msgSendSuper2(void *receiver, SEL selector, ...) { (void)receiver; (void)selector; return nil; }
id objc_retain(id object) { return object; }
void objc_release(id object) { (void)object; }
id objc_autorelease(id object) { return object; }
void objc_storeStrong(id *slot, id value) { *slot = value; }
id objc_retainAutoreleasedReturnValue(id object) { return object; }
id objc_autoreleaseReturnValue(id object) { return object; }
id objc_retainAutoreleaseReturnValue(id object) { return object; }
id objc_retainAutorelease(id object) { return object; }
id objc_unsafeClaimAutoreleasedReturnValue(id object) { return object; }
id objc_alloc(Class cls) { (void)cls; return nil; }
id objc_alloc_init(Class cls) { (void)cls; return nil; }
id objc_allocWithZone(Class cls) { (void)cls; return nil; }
Class objc_opt_class(id object) { (void)object; return (Class)0; }
BOOL objc_opt_isKindOfClass(id object, Class cls) { (void)object; (void)cls; return NO; }
BOOL objc_opt_respondsToSelector(id object, SEL selector) { (void)object; (void)selector; return NO; }
void *objc_autoreleasePoolPush(void) { return (void *)0; }
void objc_autoreleasePoolPop(void *pool) { (void)pool; }
Class objc_getClass(const char *name) { (void)name; return (Class)0; }
SEL sel_registerName(const char *name) { (void)name; return (SEL)0; }
const char *class_getName(Class cls) { (void)cls; return ""; }
unsigned long class_getInstanceSize(Class cls) { (void)cls; return 0; }
Class object_getClass(id object) { (void)object; return (Class)0; }
void *class_getProperty(Class cls, const char *name) { (void)cls; (void)name; return (void *)0; }
void **class_copyPropertyList(Class cls, unsigned int *count) { (void)cls; (void)count; return (void **)0; }
const char *property_getName(void *property) { (void)property; return ""; }
const char *property_getAttributes(void *property) { (void)property; return ""; }
id objc_storeWeak(id *slot, id value) { *slot = value; return value; }
void objc_exception_throw(id exception) { (void)exception; }
