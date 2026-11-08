#ifndef ALCHEMY_SWIFT_CORE_FOUNDATION_H
#define ALCHEMY_SWIFT_CORE_FOUNDATION_H
/* CoreFoundation umbrella for Swift runtime builds: the project base profile
   plus the standard C headers the published umbrella makes visible.
   Independently authored; no SDK content. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include_next <CoreFoundation/CoreFoundation.h>

/* Property-list data and the errors it reports, beyond the base profile. */
typedef struct __CFError* CFErrorRef;
typedef CFTypeRef CFPropertyListRef;
typedef CFIndex CFPropertyListFormat;
enum {
    kCFPropertyListOpenStepFormat = 1,
    kCFPropertyListXMLFormat_v1_0 = 100,
    kCFPropertyListBinaryFormat_v1_0 = 200
};
CFDataRef CFPropertyListCreateData(CFAllocatorRef allocator, CFPropertyListRef propertyList, CFPropertyListFormat format,
                                   CFOptionFlags options, CFErrorRef* error);
CFPropertyListRef CFPropertyListCreateWithData(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags options,
                                               CFPropertyListFormat* format, CFErrorRef* error);
#endif
