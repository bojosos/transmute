#ifndef ALCHEMY_MACTYPES_H
#define ALCHEMY_MACTYPES_H
/* Independently authored Darwin <MacTypes.h> basic types (LP64). */
#include <stdint.h>
typedef uint8_t UInt8;
typedef int8_t SInt8;
typedef uint16_t UInt16;
typedef int16_t SInt16;
typedef uint32_t UInt32;
typedef int32_t SInt32;
typedef uint64_t UInt64;
typedef int64_t SInt64;
typedef SInt16 OSErr;
typedef SInt32 OSStatus;
typedef UInt32 FourCharCode;
typedef FourCharCode OSType;
typedef unsigned char Boolean;
typedef float Float32;
typedef double Float64;
typedef unsigned long ByteCount;
typedef unsigned long ItemCount;
typedef UInt16 UniChar;
typedef unsigned char Str255[256];
typedef unsigned char *StringPtr;
typedef const unsigned char *ConstStringPtr;
typedef void *Ptr;
typedef Ptr *Handle;
typedef long Size;
enum { noErr = 0 };
#endif
