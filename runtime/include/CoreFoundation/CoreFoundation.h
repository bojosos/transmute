#ifndef ALCHEMY_CORE_FOUNDATION_H
#define ALCHEMY_CORE_FOUNDATION_H
/* Independently authored declarations for Alchemy's bounded CF profile. */
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned char Boolean;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef unsigned long CFHashCode;
typedef unsigned long CFOptionFlags;
typedef unsigned int CFStringEncoding;
typedef unsigned short UniChar;
typedef const void *CFTypeRef;
typedef const void *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef struct __CFString *CFMutableStringRef;
typedef const struct __CFData *CFDataRef;
typedef struct __CFData *CFMutableDataRef;
typedef const struct __CFNumber *CFNumberRef;
typedef const struct __CFBoolean *CFBooleanRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFArray *CFMutableArrayRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFURL *CFURLRef;
typedef struct { CFIndex location, length; } CFRange;
static inline CFRange CFRangeMake(CFIndex location, CFIndex length) { CFRange r = {location, length}; return r; }
typedef enum { kCFCompareLessThan = -1, kCFCompareEqualTo = 0, kCFCompareGreaterThan = 1 } CFComparisonResult;
typedef enum {
 kCFNumberSInt8Type=1, kCFNumberSInt16Type, kCFNumberSInt32Type, kCFNumberSInt64Type,
 kCFNumberFloat32Type, kCFNumberFloat64Type, kCFNumberCharType, kCFNumberShortType,
 kCFNumberIntType, kCFNumberLongType, kCFNumberLongLongType, kCFNumberFloatType,
 kCFNumberDoubleType, kCFNumberCFIndexType, kCFNumberNSIntegerType, kCFNumberCGFloatType
} CFNumberType;
typedef enum { kCFURLPOSIXPathStyle=0, kCFURLHFSPathStyle=1, kCFURLWindowsPathStyle=2 } CFURLPathStyle;
enum { kCFStringEncodingASCII=0x0600, kCFStringEncodingUTF8=0x08000100,
 kCFStringEncodingUTF16=0x0100, kCFStringEncodingUTF16BE=0x10000100, kCFStringEncodingUTF16LE=0x14000100 };
typedef const void *(*CFArrayRetainCallBack)(CFAllocatorRef, const void *);
typedef void (*CFArrayReleaseCallBack)(CFAllocatorRef, const void *);
typedef CFStringRef (*CFArrayCopyDescriptionCallBack)(const void *);
typedef Boolean (*CFArrayEqualCallBack)(const void *, const void *);
typedef struct { CFIndex version; CFArrayRetainCallBack retain; CFArrayReleaseCallBack release;
 CFArrayCopyDescriptionCallBack copyDescription; CFArrayEqualCallBack equal; } CFArrayCallBacks;
typedef struct { CFIndex version; CFArrayRetainCallBack retain; CFArrayReleaseCallBack release;
 CFArrayCopyDescriptionCallBack copyDescription; CFArrayEqualCallBack equal; CFHashCode (*hash)(const void *); } CFDictionaryKeyCallBacks;
typedef CFArrayCallBacks CFDictionaryValueCallBacks;
extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFAllocatorRef kCFAllocatorSystemDefault;
extern const CFBooleanRef kCFBooleanTrue, kCFBooleanFalse;
extern const CFNumberRef kCFNumberNaN, kCFNumberPositiveInfinity, kCFNumberNegativeInfinity;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
#define CFSTR(s) ((CFStringRef)__builtin___CFStringMakeConstantString(s))
CFTypeRef CFRetain(CFTypeRef); void CFRelease(CFTypeRef);
CFIndex CFGetRetainCount(CFTypeRef); CFTypeID CFGetTypeID(CFTypeRef);
CFAllocatorRef CFGetAllocator(CFTypeRef); Boolean CFEqual(CFTypeRef, CFTypeRef); CFHashCode CFHash(CFTypeRef);
CFStringRef CFCopyDescription(CFTypeRef); CFStringRef CFCopyTypeIDDescription(CFTypeID);
CFTypeID CFStringGetTypeID(void); CFTypeID CFDataGetTypeID(void); CFTypeID CFNumberGetTypeID(void);
CFTypeID CFBooleanGetTypeID(void); CFTypeID CFArrayGetTypeID(void); CFTypeID CFDictionaryGetTypeID(void); CFTypeID CFURLGetTypeID(void);
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, CFStringEncoding);
CFStringRef CFStringCreateWithBytes(CFAllocatorRef, const unsigned char *, CFIndex, CFStringEncoding, Boolean);
CFStringRef CFStringCreateWithCharacters(CFAllocatorRef, const UniChar *, CFIndex);
CFStringRef CFStringCreateCopy(CFAllocatorRef, CFStringRef);
CFStringRef CFStringCreateWithSubstring(CFAllocatorRef, CFStringRef, CFRange);
CFIndex CFStringGetLength(CFStringRef); UniChar CFStringGetCharacterAtIndex(CFStringRef, CFIndex);
void CFStringGetCharacters(CFStringRef, CFRange, UniChar *);
const UniChar *CFStringGetCharactersPtr(CFStringRef);
const char *CFStringGetCStringPtr(CFStringRef, CFStringEncoding);
Boolean CFStringGetCString(CFStringRef, char *, CFIndex, CFStringEncoding);
CFIndex CFStringGetBytes(CFStringRef, CFRange, CFStringEncoding, unsigned char, Boolean, unsigned char *, CFIndex, CFIndex *);
CFIndex CFStringGetMaximumSizeForEncoding(CFIndex, CFStringEncoding);
CFComparisonResult CFStringCompare(CFStringRef, CFStringRef, CFOptionFlags);
CFDataRef CFDataCreate(CFAllocatorRef, const unsigned char *, CFIndex);
CFDataRef CFDataCreateCopy(CFAllocatorRef, CFDataRef);
CFMutableDataRef CFDataCreateMutable(CFAllocatorRef, CFIndex);
CFMutableDataRef CFDataCreateMutableCopy(CFAllocatorRef, CFIndex, CFDataRef);
CFIndex CFDataGetLength(CFDataRef); const unsigned char *CFDataGetBytePtr(CFDataRef);
unsigned char *CFDataGetMutableBytePtr(CFMutableDataRef); void CFDataGetBytes(CFDataRef, CFRange, unsigned char *);
void CFDataAppendBytes(CFMutableDataRef, const unsigned char *, CFIndex);
void CFDataSetLength(CFMutableDataRef, CFIndex); void CFDataIncreaseLength(CFMutableDataRef, CFIndex);
void CFDataReplaceBytes(CFMutableDataRef, CFRange, const unsigned char *, CFIndex);
void CFDataDeleteBytes(CFMutableDataRef, CFRange);
CFNumberRef CFNumberCreate(CFAllocatorRef, CFNumberType, const void *);
CFNumberType CFNumberGetType(CFNumberRef); CFIndex CFNumberGetByteSize(CFNumberRef);
Boolean CFNumberIsFloatType(CFNumberRef); Boolean CFNumberGetValue(CFNumberRef, CFNumberType, void *);
CFComparisonResult CFNumberCompare(CFNumberRef, CFNumberRef, void *);
Boolean CFBooleanGetValue(CFBooleanRef);
CFArrayRef CFArrayCreate(CFAllocatorRef, const void **, CFIndex, const CFArrayCallBacks *);
CFArrayRef CFArrayCreateCopy(CFAllocatorRef, CFArrayRef);
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef, CFIndex, const CFArrayCallBacks *);
CFMutableArrayRef CFArrayCreateMutableCopy(CFAllocatorRef, CFIndex, CFArrayRef);
CFIndex CFArrayGetCount(CFArrayRef); const void *CFArrayGetValueAtIndex(CFArrayRef, CFIndex);
void CFArrayGetValues(CFArrayRef, CFRange, const void **); Boolean CFArrayContainsValue(CFArrayRef, CFRange, const void *);
void CFArrayAppendValue(CFMutableArrayRef, const void *); void CFArraySetValueAtIndex(CFMutableArrayRef, CFIndex, const void *);
void CFArrayInsertValueAtIndex(CFMutableArrayRef, CFIndex, const void *);
void CFArrayRemoveValueAtIndex(CFMutableArrayRef, CFIndex); void CFArrayRemoveAllValues(CFMutableArrayRef);
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef, const void **, const void **, CFIndex, const CFDictionaryKeyCallBacks *, const CFDictionaryValueCallBacks *);
CFDictionaryRef CFDictionaryCreateCopy(CFAllocatorRef, CFDictionaryRef);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex, const CFDictionaryKeyCallBacks *, const CFDictionaryValueCallBacks *);
CFMutableDictionaryRef CFDictionaryCreateMutableCopy(CFAllocatorRef, CFIndex, CFDictionaryRef);
CFIndex CFDictionaryGetCount(CFDictionaryRef); const void *CFDictionaryGetValue(CFDictionaryRef, const void *);
Boolean CFDictionaryGetValueIfPresent(CFDictionaryRef, const void *, const void **);
Boolean CFDictionaryContainsKey(CFDictionaryRef, const void *); Boolean CFDictionaryContainsValue(CFDictionaryRef, const void *);
void CFDictionaryGetKeysAndValues(CFDictionaryRef, const void **, const void **);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
void CFDictionaryAddValue(CFMutableDictionaryRef, const void *, const void *);
void CFDictionaryReplaceValue(CFMutableDictionaryRef, const void *, const void *);
void CFDictionaryRemoveValue(CFMutableDictionaryRef, const void *); void CFDictionaryRemoveAllValues(CFMutableDictionaryRef);
CFURLRef CFURLCreateWithFileSystemPath(CFAllocatorRef, CFStringRef, CFURLPathStyle, Boolean);
CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef, const unsigned char *, CFIndex, Boolean);
CFStringRef CFURLCopyFileSystemPath(CFURLRef, CFURLPathStyle);
Boolean CFURLGetFileSystemRepresentation(CFURLRef, Boolean, unsigned char *, CFIndex);
Boolean CFURLHasDirectoryPath(CFURLRef); CFURLRef CFURLCopyAbsoluteURL(CFURLRef); CFStringRef CFURLGetString(CFURLRef);
#ifdef __cplusplus
}
#endif
#endif
