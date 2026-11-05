#ifndef ALCHEMY_QLAB_DISPATCH_H
#define ALCHEMY_QLAB_DISPATCH_H
/* libdispatch as the Swift ClangImporter sees it on macOS, for Alchemy's
   project-authored Dispatch overlay (graphics/swift/Dispatch): dispatch
   objects are Objective-C classes named OS_dispatch_* (imported under their
   Dispatch* Swift names), so the overlay's extensions mangle as Apple's
   ($sSo17OS_dispatch_queueC8DispatchE...). Independently authored from the
   published libdispatch interface; no SDK content. Alchemy's libSystem
   provider (darwin_dispatch.cpp) implements the functions; the two declared
   weak are not provided yet and the overlay checks for them. Queue
   attributes are passed as opaque pointers (immortal objects, no reference
   counting). */
#include <stdbool.h>
#include <stdint.h>
#include <os/log.h> /* OS_object */

#ifdef __OBJC__
__attribute__((objc_runtime_visible)) __attribute__((swift_name("DispatchObject")))
@interface OS_dispatch_object : OS_object
@end
__attribute__((objc_runtime_visible)) __attribute__((swift_name("DispatchQueue")))
@interface OS_dispatch_queue : OS_dispatch_object
@end
__attribute__((objc_runtime_visible)) __attribute__((swift_name("DispatchGroup")))
@interface OS_dispatch_group : OS_dispatch_object
@end
/* Dispatch sources, as Apple's headers declare them for Swift: the class OS_dispatch_source (DispatchSource), the
   protocol of the same name every source adopts (DispatchSourceProtocol), and one protocol per source type
   (OS_dispatch_source_vnode: DispatchSourceFileSystemObject). The overlay's extensions on them mangle as Apple's
   ($sSo18OS_dispatch_sourceP8DispatchE..., $sSo24OS_dispatch_source_vnodeP8DispatchE...). */
__attribute__((swift_name("DispatchSourceProtocol")))
@protocol OS_dispatch_source
@end
__attribute__((swift_name("DispatchSourceFileSystemObject")))
@protocol OS_dispatch_source_vnode <OS_dispatch_source>
@end
__attribute__((objc_runtime_visible)) __attribute__((swift_name("DispatchSource")))
@interface OS_dispatch_source : OS_dispatch_object <OS_dispatch_source, OS_dispatch_source_vnode>
@end
typedef OS_dispatch_object* dispatch_object_t;
typedef OS_dispatch_queue* dispatch_queue_t;
typedef OS_dispatch_group* dispatch_group_t;
typedef OS_dispatch_source* dispatch_source_t;
#endif

typedef void (^dispatch_block_t)(void);
/* An absolute time (dispatch_time, dispatch_after): DISPATCH_TIME_NOW 0, DISPATCH_TIME_FOREVER ~0. */
typedef uint64_t dispatch_time_t;

/* sys/qos.h */
typedef enum : unsigned int {
    QOS_CLASS_USER_INTERACTIVE = 0x21,
    QOS_CLASS_USER_INITIATED = 0x19,
    QOS_CLASS_DEFAULT = 0x15,
    QOS_CLASS_UTILITY = 0x11,
    QOS_CLASS_BACKGROUND = 0x09,
    QOS_CLASS_UNSPECIFIED = 0x00,
} qos_class_t;

#ifdef __OBJC__
#pragma clang assume_nonnull begin

/* DISPATCH_QUEUE_CONCURRENT and the main queue are global objects. */
extern struct dispatch_queue_attr_s _dispatch_queue_attr_concurrent;
extern struct dispatch_queue_s _dispatch_main_q;
__attribute__((swift_private)) static inline void* _alchemy_dispatch_queue_concurrent(void) {
    return &_dispatch_queue_attr_concurrent;
}
__attribute__((swift_private)) static inline void* _alchemy_dispatch_main_queue(void) {
    return &_dispatch_main_q;
}

dispatch_queue_t dispatch_queue_create(const char* _Nullable label, void* _Nullable attr)
    __attribute__((ns_returns_retained)) __attribute__((swift_name("DispatchQueue.init(__label:attr:)")));
void* _Nullable dispatch_queue_attr_make_with_qos_class(void* _Nullable attr, qos_class_t qos_class, int relative_priority)
    __attribute__((swift_private));
void* _Nullable dispatch_queue_attr_make_initially_inactive(void* _Nullable attr)
    __attribute__((weak_import)) __attribute__((swift_private));
void* _Nullable dispatch_queue_attr_make_with_autorelease_frequency(void* _Nullable attr, unsigned long frequency)
    __attribute__((weak_import)) __attribute__((swift_private));
__attribute__((swift_private)) static inline bool _alchemy_dispatch_has_initially_inactive(void) {
    return &dispatch_queue_attr_make_initially_inactive != 0;
}
__attribute__((swift_private)) static inline bool _alchemy_dispatch_has_autorelease_frequency(void) {
    return &dispatch_queue_attr_make_with_autorelease_frequency != 0;
}
void dispatch_set_target_queue(dispatch_object_t object, dispatch_queue_t _Nullable queue) __attribute__((swift_private));
void dispatch_async(dispatch_queue_t queue, dispatch_block_t block) __attribute__((swift_private));
void dispatch_barrier_async(dispatch_queue_t queue, dispatch_block_t block) __attribute__((swift_private));
void dispatch_group_async(dispatch_group_t group, dispatch_queue_t queue, dispatch_block_t block) __attribute__((swift_private));
void dispatch_group_enter(dispatch_group_t group) __attribute__((swift_private));
void dispatch_group_leave(dispatch_group_t group) __attribute__((swift_private));
const char* dispatch_queue_get_label(dispatch_queue_t _Nullable queue) __attribute__((swift_private));

/* Times, the global queues, group notification and work-item blocks (the Swift API of Maccy and Pika). */
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) __attribute__((swift_private));
void dispatch_after(dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t block) __attribute__((swift_private));
dispatch_queue_t dispatch_get_global_queue(intptr_t identifier, uintptr_t flags) __attribute__((swift_private));
void dispatch_group_notify(dispatch_group_t group, dispatch_queue_t queue, dispatch_block_t block) __attribute__((swift_private));

/* Dispatch blocks keep their identity only as pointers: Swift turns a block a C function returns into a Swift closure
   (and back into a new block when it passes it on), so the overlay holds dispatch_block_create's blocks as raw
   pointers and passes them through these raw-pointer declarations of the same functions (asm labels name them). */
void* _Nullable _alchemy_dispatch_block_create(unsigned long flags, dispatch_block_t block)
    __asm__("_dispatch_block_create") __attribute__((swift_private));
void _alchemy_dispatch_block_cancel(void* block) __asm__("_dispatch_block_cancel") __attribute__((swift_private));
intptr_t _alchemy_dispatch_block_testcancel(void* block) __asm__("_dispatch_block_testcancel") __attribute__((swift_private));
void _alchemy_block_release(const void* block) __asm__("__Block_release") __attribute__((swift_private));
void _alchemy_dispatch_after(dispatch_time_t when, dispatch_queue_t queue, void* block)
    __asm__("_dispatch_after") __attribute__((swift_private));
void _alchemy_dispatch_group_notify(dispatch_group_t group, dispatch_queue_t queue, void* block)
    __asm__("_dispatch_group_notify") __attribute__((swift_private));
void _alchemy_dispatch_source_set_event_handler(dispatch_source_t source, void* handler)
    __asm__("_dispatch_source_set_event_handler") __attribute__((swift_private));
void _alchemy_dispatch_source_set_cancel_handler(dispatch_source_t source, void* handler)
    __asm__("_dispatch_source_set_cancel_handler") __attribute__((swift_private));

/* Dispatch sources: DISPATCH_SOURCE_TYPE_VNODE is the address of a libdispatch data symbol. */
extern struct dispatch_source_type_s _dispatch_source_type_vnode;
__attribute__((swift_private)) static inline const void* _alchemy_dispatch_source_type_vnode(void) {
    return &_dispatch_source_type_vnode;
}
dispatch_source_t dispatch_source_create(const void* type, uintptr_t handle, uintptr_t mask, dispatch_queue_t _Nullable queue)
    __attribute__((ns_returns_retained)) __attribute__((swift_private));
void dispatch_source_set_event_handler(dispatch_source_t source, dispatch_block_t _Nullable handler) __attribute__((swift_private));
void dispatch_source_set_cancel_handler(dispatch_source_t source, dispatch_block_t _Nullable handler) __attribute__((swift_private));
unsigned long dispatch_source_get_data(dispatch_source_t source) __attribute__((swift_private));
void dispatch_source_cancel(dispatch_source_t source) __attribute__((swift_private));
void dispatch_resume(dispatch_object_t object) __attribute__((swift_private));

#pragma clang assume_nonnull end
#endif

#endif
