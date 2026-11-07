/* The CoreFoundation subset that libdispatch's test harness (tests/bsdtests.c) uses for its
 * CFError helpers. Only the conformance build includes it; the declarations follow CF's public
 * ABI (CFIndex is long, CFStringEncoding is UInt32, kCFStringEncodingUTF8 is 0x08000100). */
#ifndef CONFORMANCE_COREFOUNDATION_H
#define CONFORMANCE_COREFOUNDATION_H

#include <stdbool.h>
#include <sys/cdefs.h>

typedef long CFIndex;
typedef unsigned char Boolean;
typedef unsigned int CFStringEncoding;
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef struct __CFError *CFErrorRef;

#define kCFStringEncodingUTF8 0x08000100

__BEGIN_DECLS
CFStringRef CFErrorCopyDescription(CFErrorRef);
CFIndex CFErrorGetCode(CFErrorRef);
Boolean CFStringGetCString(CFStringRef, char *, CFIndex, CFStringEncoding);
void CFRelease(CFTypeRef);
__END_DECLS

#endif
