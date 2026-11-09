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
- (instancetype)initWithObjects:(const id _Nonnull[_Nullable])objects forKeys:(const id _Nonnull[_Nullable])keys
                          count:(NSUInteger)count;
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

/* The classes behind the Swift value types applications reach on launch
   (Notification, CharacterSet, UUID, URLComponents, the Combine publishers,
   key-value observing). */
typedef NS_OPTIONS(NSUInteger, NSStringCompareOptions) {
    NSCaseInsensitiveSearch = 1,
    NSLiteralSearch = 2,
    NSBackwardsSearch = 4,
    NSAnchoredSearch = 8,
    NSNumericSearch = 64,
    NSDiacriticInsensitiveSearch = 128,
    NSWidthInsensitiveSearch = 256,
    NSForcedOrderingSearch = 512,
    NSRegularExpressionSearch = 1024
};
typedef NS_OPTIONS(NSUInteger, NSDataReadingOptions) {
    NSDataReadingMappedIfSafe = 1UL << 0,
    NSDataReadingUncached = 1UL << 1,
    NSDataReadingMappedAlways = 1UL << 3
};
typedef NS_OPTIONS(NSUInteger, NSDataWritingOptions) {
    NSDataWritingAtomic = 1UL << 0,
    NSDataWritingWithoutOverwriting = 1UL << 1
};
typedef NS_OPTIONS(NSUInteger, NSKeyValueObservingOptions) {
    NSKeyValueObservingOptionNew = 0x01,
    NSKeyValueObservingOptionOld = 0x02,
    NSKeyValueObservingOptionInitial = 0x04,
    NSKeyValueObservingOptionPrior = 0x08
};
typedef NS_ENUM(NSUInteger, NSKeyValueChange) {
    NSKeyValueChangeSetting = 1,
    NSKeyValueChangeInsertion = 2,
    NSKeyValueChangeRemoval = 3,
    NSKeyValueChangeReplacement = 4
};
typedef NSString* NSKeyValueChangeKey NS_TYPED_EXTENSIBLE_ENUM;
extern NSKeyValueChangeKey const NSKeyValueChangeKindKey;
extern NSKeyValueChangeKey const NSKeyValueChangeNewKey;
extern NSKeyValueChangeKey const NSKeyValueChangeOldKey;
extern NSKeyValueChangeKey const NSKeyValueChangeIndexesKey;
extern NSKeyValueChangeKey const NSKeyValueChangeNotificationIsPriorKey;

@interface NSObject (NSKeyValueObserving)
- (void)observeValueForKeyPath:(nullable NSString*)keyPath ofObject:(nullable id)object
                        change:(nullable NSDictionary<NSKeyValueChangeKey, id>*)change context:(nullable void*)context;
- (void)addObserver:(NSObject*)observer forKeyPath:(NSString*)keyPath options:(NSKeyValueObservingOptions)options
            context:(nullable void*)context;
- (void)removeObserver:(NSObject*)observer forKeyPath:(NSString*)keyPath context:(nullable void*)context;
- (void)willChangeValueForKey:(NSString*)key;
- (void)didChangeValueForKey:(NSString*)key;
@end

@interface NSString (NSStringExtensionMethods)
- (instancetype)initWithFormat:(NSString*)format arguments:(va_list)argList;
- (instancetype)initWithFormat:(NSString*)format locale:(nullable id)locale arguments:(va_list)argList;
@property (readonly, copy) NSString* capitalizedString;
- (NSComparisonResult)caseInsensitiveCompare:(NSString*)string;
- (NSString*)stringByReplacingOccurrencesOfString:(NSString*)target withString:(NSString*)replacement
                                          options:(NSStringCompareOptions)options range:(NSRange)searchRange;
- (BOOL)writeToURL:(NSURL*)url atomically:(BOOL)useAuxiliaryFile encoding:(NSStringEncoding)enc error:(NSError**)error;
@end

@interface NSNumber (NSNumberCreation)
+ (NSNumber*)numberWithBool:(BOOL)value;
+ (NSNumber*)numberWithInteger:(NSInteger)value;
+ (NSNumber*)numberWithFloat:(float)value;
+ (NSNumber*)numberWithDouble:(double)value;
@property (readonly) BOOL boolValue;
@property (readonly) NSInteger integerValue;
@property (readonly) float floatValue;
@property (readonly) double doubleValue;
@end

@interface NSURL (NSURLComponentAccessors)
- (nullable NSURL*)URLByAppendingPathComponent:(NSString*)pathComponent isDirectory:(BOOL)isDirectory;
@property (nullable, readonly, copy) NSString* scheme;
@property (nullable, readonly, copy) NSString* host;
@property (nullable, readonly, copy) NSString* fragment;
@property (nullable, readonly, copy) NSArray<NSString*>* pathComponents;
@end

@interface NSData (NSDataFiles)
+ (nullable instancetype)dataWithContentsOfURL:(NSURL*)url options:(NSDataReadingOptions)readOptionsMask error:(NSError**)errorPtr;
- (BOOL)writeToURL:(NSURL*)url options:(NSDataWritingOptions)writeOptionsMask error:(NSError**)errorPtr;
@end

@interface NSDate (NSDateDistantDates)
@property (class, readonly, copy) NSDate* distantFuture;
@property (class, readonly, copy) NSDate* distantPast;
@end

@interface NSBundle (NSBundleLocalization)
- (NSString*)localizedStringForKey:(NSString*)key value:(nullable NSString*)value table:(nullable NSString*)tableName;
@end

@class NSNotification;
typedef NSString* NSNotificationName NS_TYPED_EXTENSIBLE_ENUM;

@interface NSNotification : NSObject <NSCopying, NSCoding>
@property (readonly, copy) NSNotificationName name;
@property (nullable, readonly, retain) id object;
@property (nullable, readonly, copy) NSDictionary* userInfo;
- (instancetype)initWithName:(NSNotificationName)name object:(nullable id)object userInfo:(nullable NSDictionary*)userInfo
    NS_DESIGNATED_INITIALIZER;
+ (instancetype)notificationWithName:(NSNotificationName)aName object:(nullable id)anObject userInfo:(nullable NSDictionary*)aUserInfo;
@end

@interface NSOperationQueue : NSObject
@end

@interface NSNotificationCenter : NSObject
@property (class, readonly, strong) NSNotificationCenter* defaultCenter;
- (id<NSObject>)addObserverForName:(nullable NSNotificationName)name object:(nullable id)obj queue:(nullable NSOperationQueue*)queue
                        usingBlock:(void (^)(NSNotification* notification))block;
- (void)removeObserver:(id)observer;
- (void)postNotification:(NSNotification*)notification;
- (void)postNotificationName:(NSNotificationName)aName object:(nullable id)anObject userInfo:(nullable NSDictionary*)aUserInfo;
@end

@interface NSCharacterSet : NSObject <NSCopying, NSMutableCopying, NSSecureCoding>
@property (class, readonly, copy) NSCharacterSet* whitespaceCharacterSet;
@property (class, readonly, copy) NSCharacterSet* whitespaceAndNewlineCharacterSet;
@property (class, readonly, copy) NSCharacterSet* newlineCharacterSet;
@property (class, readonly, copy) NSCharacterSet* decimalDigitCharacterSet;
@property (class, readonly, copy) NSCharacterSet* alphanumericCharacterSet;
@property (class, readonly, copy) NSCharacterSet* URLQueryAllowedCharacterSet;
+ (NSCharacterSet*)characterSetWithCharactersInString:(NSString*)aString;
- (BOOL)longCharacterIsMember:(uint32_t)theLongChar;
@property (readonly, copy) NSCharacterSet* invertedSet;
@end

@interface NSMutableCharacterSet : NSCharacterSet
- (void)addCharactersInString:(NSString*)aString;
- (void)removeCharactersInString:(NSString*)aString;
- (void)formUnionWithCharacterSet:(NSCharacterSet*)otherSet;
@end

@interface NSString (NSCharacterSetMethods)
- (NSString*)stringByTrimmingCharactersInSet:(NSCharacterSet*)set;
- (NSArray<NSString*>*)componentsSeparatedByCharactersInSet:(NSCharacterSet*)separator NS_SWIFT_NAME(components(separatedBy:));
- (nullable NSString*)stringByAddingPercentEncodingWithAllowedCharacters:(NSCharacterSet*)allowedCharacters;
@end

@interface NSUUID : NSObject <NSCopying, NSSecureCoding>
- (instancetype)initWithUUIDBytes:(const unsigned char*)bytes;
- (void)getUUIDBytes:(unsigned char*)uuid;
@property (readonly, copy) NSString* UUIDString;
@end

@interface NSURLQueryItem : NSObject <NSSecureCoding, NSCopying>
+ (instancetype)queryItemWithName:(NSString*)name value:(nullable NSString*)value;
@property (readonly) NSString* name;
@property (nullable, readonly) NSString* value;
@end

@interface NSURLComponents : NSObject <NSCopying>
+ (nullable instancetype)componentsWithURL:(NSURL*)url resolvingAgainstBaseURL:(BOOL)resolve;
@property (nullable, readonly, copy) NSURL* URL;
@property (nullable, copy) NSString* scheme;
@property (nullable, copy) NSString* host;
@property (copy) NSString* path;
@property (nullable, copy) NSString* query;
@property (nullable, copy) NSString* fragment;
@property (nullable, copy) NSArray<NSURLQueryItem*>* queryItems;
@end

typedef NSString* NSRunLoopMode NS_TYPED_EXTENSIBLE_ENUM;
extern NSRunLoopMode const NSDefaultRunLoopMode;
extern NSRunLoopMode const NSRunLoopCommonModes;

@interface NSTimer : NSObject
+ (NSTimer*)timerWithTimeInterval:(NSTimeInterval)interval repeats:(BOOL)repeats block:(void (^)(NSTimer* timer))block;
- (void)invalidate;
@property NSTimeInterval tolerance;
@end

@interface NSRunLoop : NSObject
@property (class, readonly, strong) NSRunLoop* currentRunLoop;
@property (class, readonly, strong) NSRunLoop* mainRunLoop;
- (void)addTimer:(NSTimer*)timer forMode:(NSRunLoopMode)mode;
- (void)runUntilDate:(NSDate*)limitDate;
@end

@interface NSKeyedUnarchiver : NSCoder
+ (nullable id)unarchivedObjectOfClass:(Class)cls fromData:(NSData*)data error:(NSError**)error NS_REFINED_FOR_SWIFT;
+ (nullable id)unarchiveTopLevelObjectWithData:(NSData*)data error:(NSError**)error NS_REFINED_FOR_SWIFT;
@end

@interface NSUserDefaults : NSObject
@property (class, readonly, strong) NSUserDefaults* standardUserDefaults;
- (void)registerDefaults:(NSDictionary*)registrationDictionary;
- (nullable id)objectForKey:(NSString*)defaultName;
- (NSInteger)integerForKey:(NSString*)defaultName;
- (BOOL)boolForKey:(NSString*)defaultName;
@end

void NSLogv(NSString* format, va_list args);
NS_ASSUME_NONNULL_END
#endif
#endif
