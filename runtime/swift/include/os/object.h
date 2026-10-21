#ifndef ALCHEMY_OS_OBJECT_H
#define ALCHEMY_OS_OBJECT_H
/* Published os_object declaration macros, in their non-Objective-C form:
   OS objects are opaque C structure pointers. Independently authored; no SDK
   content. */
#define OS_OBJECT_USE_OBJC 0
#define OS_OBJECT_DECL_CLASS(name) typedef struct name##_s* name##_t
#define OS_OBJECT_DECL(name, ...) OS_OBJECT_DECL_CLASS(name)
#define OS_OBJECT_RETURNS_RETAINED
#define OS_OBJECT_CONSUMED
#define OS_EXPORT extern __attribute__((visibility("default")))
#define OS_NOTHROW __attribute__((__nothrow__))
#define OS_WARN_RESULT __attribute__((__warn_unused_result__))
#define OS_NONNULL1 __attribute__((__nonnull__(1)))
#define OS_NONNULL_ALL __attribute__((__nonnull__))
#ifdef __cplusplus
extern "C" {
#endif
void* os_retain(void* object);
void os_release(void* object);
#ifdef __cplusplus
}
#endif
#endif
