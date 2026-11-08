/* SCDynamicStoreCopyProxies, served by Alchemy's SystemConfiguration provider.
 * Declarations written for Alchemy's guest runtimes (libcurl's macOS init calls it). */
#ifndef ALCHEMY_SC_DYNAMIC_STORE_COPY_SPECIFIC_H
#define ALCHEMY_SC_DYNAMIC_STORE_COPY_SPECIFIC_H
#include <CoreFoundation/CoreFoundation.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef const struct __SCDynamicStore* SCDynamicStoreRef;
CFDictionaryRef SCDynamicStoreCopyProxies(SCDynamicStoreRef store);
#ifdef __cplusplus
}
#endif
#endif
