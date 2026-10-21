#ifndef ALCHEMY_SWIFT_FOUNDATION_H
#define ALCHEMY_SWIFT_FOUNDATION_H
/* Independently authored declarations of the published Foundation classes
   and protocols used to build the Swift runtime and Alchemy's Swift
   overlays (as the Clang module Foundation). Alchemy's Foundation provider
   implements these classes. No SDK content. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/NSObject.h>
#include <objc/NSObjCRuntime.h>
#include <CoreFoundation/CoreFoundation.h>

#define NS_ENUM(_type, _name) enum __attribute__((enum_extensibility(open))) _name : _type _name; \
    enum __attribute__((enum_extensibility(open))) _name : _type
#define NS_CLOSED_ENUM(_type, _name) enum __attribute__((enum_extensibility(closed))) _name : _type _name; \
    enum __attribute__((enum_extensibility(closed))) _name : _type
#define NS_OPTIONS(_type, _name) enum __attribute__((flag_enum, enum_extensibility(open))) _name : _type _name; \
    enum __attribute__((flag_enum, enum_extensibility(open))) _name : _type
#define NS_SWIFT_NAME(_name) __attribute__((swift_name(#_name)))
#define NS_REFINED_FOR_SWIFT __attribute__((swift_private))
#define NS_SWIFT_UNAVAILABLE(_msg) __attribute__((availability(swift, unavailable, message=_msg)))
#define NS_TYPED_EXTENSIBLE_ENUM __attribute__((swift_wrapper(struct)))
#define NS_DESIGNATED_INITIALIZER __attribute__((objc_designated_initializer))
#define NS_RETURNS_INNER_POINTER __attribute__((objc_returns_inner_pointer))
#ifndef NS_RETURNS_RETAINED
#define NS_RETURNS_RETAINED __attribute__((ns_returns_retained))
#endif
#ifndef NS_ROOT_CLASS
#define NS_ROOT_CLASS __attribute__((objc_root_class))
#endif
#if __has_feature(nullability)
#define NS_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define NS_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")
#else
#define NS_ASSUME_NONNULL_BEGIN
#define NS_ASSUME_NONNULL_END
#endif

typedef unsigned short unichar;
typedef struct _NSRange { NSUInteger location, length; } NSRange;
typedef NSRange* NSRangePointer;
typedef struct _NSZone NSZone;
typedef double NSTimeInterval;
typedef NSUInteger NSStringEncoding;
typedef struct _NSOperatingSystemVersion {
    NSInteger majorVersion, minorVersion, patchVersion;
} NSOperatingSystemVersion;

typedef NS_CLOSED_ENUM(NSInteger, NSComparisonResult) {
    NSOrderedAscending = -1L,
    NSOrderedSame,
    NSOrderedDescending
};

enum {
    NSASCIIStringEncoding = 1,
    NSUTF8StringEncoding = 4,
    NSISOLatin1StringEncoding = 5,
    NSUnicodeStringEncoding = 10,
    NSUTF16StringEncoding = NSUnicodeStringEncoding,
    NSUTF16BigEndianStringEncoding = 0x90000100,
    NSUTF16LittleEndianStringEncoding = 0x94000100,
    NSUTF32StringEncoding = 0x8c000100
};

#ifdef __OBJC__
NS_ASSUME_NONNULL_BEGIN
@class NSArray<ObjectType>, NSDictionary<KeyType, ObjectType>, NSData, NSNumber, NSSet<ObjectType>, NSCoder, NSEnumerator<ObjectType>,
    NSError, NSURL, NSDate, NSLocale, NSString;
typedef NSString* NSErrorDomain NS_TYPED_EXTENSIBLE_ENUM;
typedef NSString* NSErrorUserInfoKey NS_TYPED_EXTENSIBLE_ENUM;

@protocol NSCopying
- (id)copyWithZone:(nullable NSZone*)zone;
@end
@protocol NSMutableCopying
- (id)mutableCopyWithZone:(nullable NSZone*)zone;
@end
@protocol NSCoding
- (void)encodeWithCoder:(NSCoder*)coder;
- (nullable instancetype)initWithCoder:(NSCoder*)coder;
@end
@protocol NSSecureCoding <NSCoding>
@property (class, readonly) BOOL supportsSecureCoding;
@end
typedef struct {
    unsigned long state;
    id __unsafe_unretained _Nullable * _Nullable itemsPtr;
    unsigned long* _Nullable mutationsPtr;
    unsigned long extra[5];
} NSFastEnumerationState;
@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState*)state
                                  objects:(id __unsafe_unretained _Nullable [_Nonnull])buffer
                                    count:(NSUInteger)len;
@end

@interface NSString : NSObject <NSCopying, NSMutableCopying, NSSecureCoding>
@property (readonly) NSUInteger length;
- (unichar)characterAtIndex:(NSUInteger)index;
- (instancetype)init NS_DESIGNATED_INITIALIZER;
+ (instancetype)string;
+ (instancetype)stringWithString:(NSString*)string;
+ (nullable instancetype)stringWithUTF8String:(const char*)bytes;
- (nullable instancetype)initWithUTF8String:(const char*)bytes;
- (instancetype)initWithString:(NSString*)string;
- (nullable instancetype)initWithBytes:(const void*)bytes length:(NSUInteger)len encoding:(NSStringEncoding)encoding;
- (nullable instancetype)initWithData:(NSData*)data encoding:(NSStringEncoding)encoding;
- (instancetype)initWithCharacters:(const unichar*)characters length:(NSUInteger)length;
@property (nullable, readonly) const char* UTF8String NS_RETURNS_INNER_POINTER;
- (BOOL)isEqualToString:(NSString*)other;
- (NSComparisonResult)compare:(NSString*)other;
- (NSString*)stringByAppendingString:(NSString*)other;
- (nullable NSData*)dataUsingEncoding:(NSStringEncoding)encoding allowLossyConversion:(BOOL)lossy;
- (nullable NSData*)dataUsingEncoding:(NSStringEncoding)encoding;
- (BOOL)containsString:(NSString*)other;
- (void)getCharacters:(unichar*)buffer range:(NSRange)range;
@property (readonly, copy) NSString* decomposedStringWithCanonicalMapping;
@property (readonly, copy) NSString* precomposedStringWithCanonicalMapping;
@end

@interface NSNumber : NSObject <NSCopying>
@end

@interface NSError : NSObject <NSCopying, NSSecureCoding>
- (instancetype)initWithDomain:(NSErrorDomain)domain code:(NSInteger)code userInfo:(nullable NSDictionary<NSErrorUserInfoKey, id>*)dict NS_DESIGNATED_INITIALIZER;
+ (instancetype)errorWithDomain:(NSErrorDomain)domain code:(NSInteger)code userInfo:(nullable NSDictionary<NSErrorUserInfoKey, id>*)dict;
@property (readonly, copy) NSErrorDomain domain;
@property (readonly) NSInteger code;
@property (readonly, copy) NSDictionary<NSErrorUserInfoKey, id>* userInfo;
@property (readonly, copy) NSString* localizedDescription;
@end

@interface NSArray<ObjectType> : NSObject <NSCopying, NSMutableCopying, NSFastEnumeration>
@property (readonly) NSUInteger count;
- (ObjectType)objectAtIndex:(NSUInteger)index;
@end

@interface NSDictionary<KeyType, ObjectType> : NSObject <NSCopying, NSMutableCopying, NSFastEnumeration>
@property (readonly) NSUInteger count;
- (nullable ObjectType)objectForKey:(KeyType)aKey;
- (NSEnumerator<KeyType>*)keyEnumerator;
@end

@interface NSSet<ObjectType> : NSObject <NSCopying, NSMutableCopying, NSFastEnumeration>
@property (readonly) NSUInteger count;
@end

@interface NSEnumerator<ObjectType> : NSObject <NSFastEnumeration>
- (nullable ObjectType)nextObject;
@end

@interface NSNull : NSObject <NSCopying>
+ (NSNull*)null;
@end

@interface NSData : NSObject <NSCopying, NSMutableCopying, NSSecureCoding>
@property (readonly) NSUInteger length;
@property (readonly) const void* bytes NS_RETURNS_INNER_POINTER;
+ (instancetype)data;
+ (instancetype)dataWithBytes:(nullable const void*)bytes length:(NSUInteger)length;
- (instancetype)initWithBytes:(nullable const void*)bytes length:(NSUInteger)length;
- (instancetype)initWithBytesNoCopy:(void*)bytes length:(NSUInteger)length freeWhenDone:(BOOL)b;
- (void)getBytes:(void*)buffer range:(NSRange)range;
@end

@interface NSMutableData : NSData
@property (readonly) void* mutableBytes NS_RETURNS_INNER_POINTER;
- (void)appendBytes:(const void*)bytes length:(NSUInteger)length;
@end

@interface NSURL : NSObject <NSCopying, NSSecureCoding>
+ (nullable instancetype)URLWithString:(NSString*)URLString;
- (nullable instancetype)initWithString:(NSString*)URLString;
- (instancetype)initFileURLWithPath:(NSString*)path;
+ (NSURL*)fileURLWithPath:(NSString*)path;
@property (nullable, readonly, copy) NSString* absoluteString;
@property (nullable, readonly, copy) NSString* path;
@property (nullable, readonly, copy) NSString* lastPathComponent;
@property (nullable, readonly, copy) NSURL* URLByDeletingPathExtension;
@property (nullable, readonly, copy) NSURL* URLByDeletingLastPathComponent;
- (nullable NSURL*)URLByAppendingPathComponent:(NSString*)pathComponent;
@property (readonly, getter=isFileURL) BOOL fileURL;
@end

@interface NSDate : NSObject <NSCopying, NSSecureCoding>
@property (readonly) NSTimeInterval timeIntervalSinceReferenceDate;
- (instancetype)initWithTimeIntervalSinceReferenceDate:(NSTimeInterval)ti NS_DESIGNATED_INITIALIZER;
+ (instancetype)date;
@end

@interface NSLocale : NSObject <NSCopying, NSSecureCoding>
@property (class, readonly, copy) NSLocale* autoupdatingCurrentLocale;
@property (class, readonly, copy) NSLocale* currentLocale;
@property (readonly, copy) NSString* localeIdentifier;
- (instancetype)initWithLocaleIdentifier:(NSString*)string NS_DESIGNATED_INITIALIZER;
@end

@interface NSFileHandle : NSObject <NSSecureCoding>
- (nullable NSData*)readDataUpToLength:(NSUInteger)length error:(out NSError**)error;
- (BOOL)writeData:(NSData*)data error:(out NSError**)error;
- (BOOL)seekToEndReturningOffset:(out unsigned long long* _Nullable)offsetInFile error:(out NSError**)error;
@end

@interface NSPredicate : NSObject <NSSecureCoding, NSCopying>
+ (NSPredicate*)predicateWithFormat:(NSString*)predicateFormat argumentArray:(nullable NSArray*)arguments;
+ (NSPredicate*)predicateWithFormat:(NSString*)predicateFormat arguments:(va_list)argList;
@end

@interface NSBundle : NSObject
+ (NSBundle*)mainBundle;
@property (readonly, copy) NSString* bundlePath;
@property (nullable, readonly, copy) NSString* bundleIdentifier;
@end

NS_ROOT_CLASS
@interface NSProxy <NSObject> {
    Class isa;
}
+ (id)alloc;
+ (Class)class;
@end

@interface NSCoder : NSObject
@end
NS_ASSUME_NONNULL_END
#endif
#endif
