#ifndef ALCHEMY_OBJC_API_H
#define ALCHEMY_OBJC_API_H
/* Independently authored declarations of the published Objective-C runtime
   interface, for building guest code against Alchemy's libobjc provider.
   No SDK content. */
#include <stddef.h>
#include <stdint.h>
#include <Availability.h>
#ifdef __cplusplus
#define OBJC_EXTERN extern "C"
#else
#define OBJC_EXTERN extern
#endif
#define OBJC_EXPORT OBJC_EXTERN __attribute__((visibility("default")))
#define OBJC_IMPORT extern
#define OBJC_ROOT_CLASS __attribute__((objc_root_class))
#define OBJC_AVAILABLE(...)
#define OBJC_OSX_AVAILABLE_OTHERS_UNAVAILABLE(...)
#define OBJC_UNAVAILABLE(...)
#define OBJC_ARC_UNAVAILABLE
#define OBJC2_UNAVAILABLE
#define OBJC_ISA_AVAILABILITY
#define OBJC_INLINE inline
#define OBJC_RETURNS_RETAINED __attribute__((ns_returns_retained))
#endif
