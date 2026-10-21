#ifndef ALCHEMY_OBJC_INTERNAL_H
#define ALCHEMY_OBJC_INTERNAL_H
/* Independently authored declarations of the libobjc entry points that the
   open-source Swift runtime calls (its interop contract with libobjc).
   Alchemy's libobjc provider implements them. No SDK content. */
#include <objc/objc.h>
#include <objc/runtime.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
OBJC_EXPORT id objc_retain(id obj);
OBJC_EXPORT void objc_release(id obj);
OBJC_EXPORT id objc_autorelease(id obj);
OBJC_EXPORT id objc_retainAutoreleasedReturnValue(id obj);
OBJC_EXPORT id objc_autoreleaseReturnValue(id obj);
OBJC_EXPORT id objc_retainAutoreleaseReturnValue(id obj);
OBJC_EXPORT id objc_retainAutorelease(id obj);
OBJC_EXPORT void* objc_autoreleasePoolPush(void);
OBJC_EXPORT void objc_autoreleasePoolPop(void* pool);
OBJC_EXPORT id objc_alloc(Class cls);
OBJC_EXPORT id objc_allocWithZone(Class cls);
OBJC_EXPORT id objc_alloc_init(Class cls);
OBJC_EXPORT id objc_opt_new(Class cls);
OBJC_EXPORT id objc_opt_self(id obj);
OBJC_EXPORT Class objc_opt_class(id obj);
OBJC_EXPORT BOOL objc_opt_isKindOfClass(id obj, Class cls);
OBJC_EXPORT BOOL objc_opt_respondsToSelector(id obj, SEL sel);
OBJC_EXPORT id objc_initWeak(id* location, id value);
OBJC_EXPORT id objc_initWeakOrNil(id* location, id value);
OBJC_EXPORT void objc_destroyWeak(id* location);
OBJC_EXPORT id objc_loadWeakRetained(id* location);
OBJC_EXPORT void objc_copyWeak(id* to, id* from);
OBJC_EXPORT void objc_moveWeak(id* to, id* from);
OBJC_EXPORT BOOL objc_isUniquelyReferenced(id obj);
OBJC_EXPORT id _objc_rootAutorelease(id obj);
OBJC_EXPORT Class objc_readClassPair(Class cls, const struct objc_image_info* info);
OBJC_EXPORT void objc_setSuperclass(Class cls, Class newSuper);
OBJC_EXPORT BOOL _objc_supportsLazyRealization(void);
typedef void (*_objc_func_classCopyFixupHandler)(Class oldClass, Class newClass);
OBJC_EXPORT void _objc_setClassCopyFixupHandler(_objc_func_classCopyFixupHandler handler);
OBJC_EXPORT void objc_storeStrong(id* location, id obj);
OBJC_EXPORT id objc_getProperty(id self, SEL _cmd, ptrdiff_t offset, BOOL atomic);
OBJC_EXPORT void objc_setProperty(id self, SEL _cmd, ptrdiff_t offset, id newValue, BOOL atomic, signed char shouldCopy);
OBJC_EXPORT int objc_sync_enter(id obj);
OBJC_EXPORT int objc_sync_exit(id obj);
#ifdef __cplusplus
}
#endif
#endif
