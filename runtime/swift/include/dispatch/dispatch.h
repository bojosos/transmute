#ifndef ALCHEMY_SWIFT_DISPATCH_H
#define ALCHEMY_SWIFT_DISPATCH_H
/* Published libdispatch declarations used by the Swift runtime, layered over
   the project base profile. Independently authored; no SDK content. */
#include_next <dispatch/dispatch.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef intptr_t dispatch_once_t;
typedef void (*dispatch_function_t)(void*);
typedef struct dispatch_queue_s* dispatch_queue_t;
typedef struct dispatch_queue_global_s* dispatch_queue_global_t;
void dispatch_once_f(dispatch_once_t* predicate, void* context, dispatch_function_t function);
dispatch_queue_global_t dispatch_get_global_queue(intptr_t identifier, uintptr_t flags);
void dispatch_async_f(dispatch_queue_t queue, void* context, dispatch_function_t work);
void dispatch_sync_f(dispatch_queue_t queue, void* context, dispatch_function_t work);
#ifdef __cplusplus
}
#endif
#endif
