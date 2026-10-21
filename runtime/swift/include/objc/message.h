#ifndef ALCHEMY_OBJC_MESSAGE_H
#define ALCHEMY_OBJC_MESSAGE_H
/* Independently authored declarations of the published Objective-C runtime
   interface. No SDK content. */
#include <objc/objc.h>
#include <objc/runtime.h>
struct objc_super { id receiver; Class super_class; };
#ifdef __cplusplus
extern "C" {
#endif
OBJC_EXPORT void objc_msgSend(void);
OBJC_EXPORT void objc_msgSendSuper(void);
OBJC_EXPORT void objc_msgSendSuper2(void);
#ifdef __cplusplus
}
#endif
#endif
