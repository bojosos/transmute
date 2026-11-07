#ifndef ALCHEMY_SWIFT_DISPATCH_H
#define ALCHEMY_SWIFT_DISPATCH_H
/* Published libdispatch declarations used by the Swift runtime and
   _Concurrency's dispatch executor, layered over the project base profile.
   Independently authored; no SDK content. */
#include_next <dispatch/dispatch.h>
#include <stddef.h>
#include <sys/qos.h>
#ifdef __cplusplus
/* libdispatch's C++ object model: every dispatch type derives from
   dispatch_object_s, and the queue kinds derive from dispatch_queue_s. */
struct dispatch_object_s {};
struct dispatch_queue_s : dispatch_object_s {};
struct dispatch_queue_global_s : dispatch_queue_s {};
struct dispatch_queue_main_s : dispatch_queue_s {};
struct dispatch_queue_attr_s : dispatch_object_s {};
struct dispatch_source_s : dispatch_object_s {};
extern "C" {
#endif
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
#define USEC_PER_SEC 1000000ull
typedef intptr_t dispatch_once_t;
typedef void (*dispatch_function_t)(void*);
typedef qos_class_t dispatch_qos_class_t;
typedef struct dispatch_queue_s* dispatch_queue_t;
typedef struct dispatch_queue_global_s* dispatch_queue_global_t;
typedef struct dispatch_queue_main_s* dispatch_queue_main_t;
typedef struct dispatch_queue_attr_s* dispatch_queue_attr_t;
typedef struct dispatch_source_s* dispatch_source_t;
typedef const struct dispatch_source_type_s* dispatch_source_type_t;
extern struct dispatch_queue_s _dispatch_main_q;
#define dispatch_get_main_queue() ((dispatch_queue_main_t)&_dispatch_main_q)
extern struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent;
#define DISPATCH_QUEUE_SERIAL ((dispatch_queue_attr_t)0)
#define DISPATCH_QUEUE_CONCURRENT (&_dispatch_queue_attr_concurrent)
extern const struct dispatch_source_type_s _dispatch_source_type_timer;
#define DISPATCH_SOURCE_TYPE_TIMER (&_dispatch_source_type_timer)
void dispatch_once_f(dispatch_once_t* predicate, void* context, dispatch_function_t function);
dispatch_queue_global_t dispatch_get_global_queue(intptr_t identifier, uintptr_t flags);
dispatch_queue_t dispatch_queue_create(const char* label, dispatch_queue_attr_t attr);
dispatch_queue_attr_t dispatch_queue_attr_make_with_qos_class(dispatch_queue_attr_t attr, dispatch_qos_class_t qos,
    int relative_priority);
void dispatch_async_f(dispatch_queue_t queue, void* context, dispatch_function_t work);
void dispatch_sync_f(dispatch_queue_t queue, void* context, dispatch_function_t work);
void dispatch_after_f(dispatch_time_t when, dispatch_queue_t queue, void* context, dispatch_function_t work);
void dispatch_assert_queue(dispatch_queue_t queue);
void dispatch_main(void) __attribute__((noreturn));
dispatch_source_t dispatch_source_create(dispatch_source_type_t type, uintptr_t handle, uintptr_t mask,
    dispatch_queue_t queue);
void dispatch_source_set_timer(dispatch_source_t source, dispatch_time_t start, uint64_t interval, uint64_t leeway);
void dispatch_source_set_event_handler_f(dispatch_source_t source, dispatch_function_t handler);
void dispatch_source_cancel(dispatch_source_t source);
void dispatch_set_context(void* object, void* context);
void* dispatch_get_context(void* object);
void dispatch_activate(void* object);
void dispatch_resume(void* object);
void dispatch_suspend(void* object);
#ifdef __BLOCKS__
typedef void (^dispatch_block_t)(void);
void dispatch_async(dispatch_queue_t queue, dispatch_block_t block);
void dispatch_after(dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t block);
#endif
#ifdef __cplusplus
}
#endif
#endif
