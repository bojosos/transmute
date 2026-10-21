#ifndef ALCHEMY_OS_LOG_H
#define ALCHEMY_OS_LOG_H
/* Independently authored declarations of the published os_log interface, in
   its Objective-C object form (os_log_t is an OS_os_log instance), for the
   Clang module os that Alchemy's os overlay imports. Alchemy's libSystem
   provider implements these functions. No SDK content. */
#include <stdbool.h>
#include <stdint.h>
#include <objc/NSObject.h>

#ifdef __OBJC__
__attribute__((objc_runtime_visible))
@interface OS_object : NSObject
@end
__attribute__((objc_runtime_visible)) __attribute__((swift_name("OSLog")))
@interface OS_os_log : OS_object
@end
typedef OS_os_log* os_log_t;
#else
typedef struct os_log_s* os_log_t;
#endif

typedef enum : uint8_t {
    OS_LOG_TYPE_DEFAULT = 0x00,
    OS_LOG_TYPE_INFO = 0x01,
    OS_LOG_TYPE_DEBUG = 0x02,
    OS_LOG_TYPE_ERROR = 0x10,
    OS_LOG_TYPE_FAULT = 0x11
} os_log_type_t;

#ifdef __cplusplus
extern "C" {
#endif
os_log_t _Nonnull os_log_create(const char* _Nonnull subsystem, const char* _Nonnull category) __attribute__((ns_returns_retained));
bool os_log_type_enabled(os_log_t _Nonnull oslog, os_log_type_t type);
void _os_log_impl(void* _Nonnull dso, os_log_t _Nonnull log, os_log_type_t type, const char* _Nonnull format,
                  uint8_t* _Nonnull buffer, uint32_t size);
extern struct mach_header __dso_handle;
#ifdef __cplusplus
}
#endif
#endif
