#ifndef ALCHEMY_FOUNDATION_H
#define ALCHEMY_FOUNDATION_H
/* Authored declarations for Alchemy's bounded Foundation guest ABI. */
#include "../../objc/AlchemyObjC.h"
#include <stdarg.h>
typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef unsigned short unichar;
typedef struct _NSRange { NSUInteger location, length; } NSRange;
typedef struct _NSOperatingSystemVersion {
    NSInteger majorVersion, minorVersion, patchVersion;
} NSOperatingSystemVersion;
@interface NSString : NSObject
+ (id)stringWithUTF8String:(const char *)bytes;
+ (id)stringWithString:(NSString *)string;
- (id)initWithUTF8String:(const char *)bytes;
- (id)initWithString:(NSString *)string;
- (id)initWithCharacters:(const unichar *)characters length:(NSUInteger)length;
- (NSUInteger)length;
- (const char *)UTF8String;
- (unichar)characterAtIndex:(NSUInteger)index;
- (NSInteger)compare:(NSString *)other;
- (BOOL)isEqualToString:(NSString *)other;
- (NSString *)stringByAppendingString:(NSString *)other;
@end
@interface NSNumber : NSObject
+ (id)numberWithInt:(int)value;
+ (id)numberWithLongLong:(long long)value;
+ (id)numberWithDouble:(double)value;
+ (id)numberWithBool:(BOOL)value;
- (id)initWithInt:(int)value;
- (long long)longLongValue;
- (int)intValue;
- (double)doubleValue;
- (BOOL)boolValue;
@end
@interface NSData : NSObject
+ (id)dataWithBytes:(const void *)bytes length:(NSUInteger)length;
- (id)initWithBytes:(const void *)bytes length:(NSUInteger)length;
- (NSUInteger)length;
- (const void *)bytes;
@end
@interface NSArray : NSObject
+ (id)array;
+ (id)arrayWithObject:(id)object;
+ (id)arrayWithObjects:(const id *)objects count:(NSUInteger)count;
- (id)initWithObjects:(const id *)objects count:(NSUInteger)count;
- (NSUInteger)count;
- (id)objectAtIndex:(NSUInteger)index;
@end
@interface NSDictionary : NSObject
+ (id)dictionary;
+ (id)dictionaryWithObjects:(const id *)objects forKeys:(const id *)keys count:(NSUInteger)count;
- (id)initWithObjects:(const id *)objects forKeys:(const id *)keys count:(NSUInteger)count;
- (NSUInteger)count;
- (id)objectForKey:(id)key;
@end
@interface NSAutoreleasePool : NSObject
- (void)drain;
@end
@interface NSProcessInfo : NSObject
+ (NSProcessInfo *)processInfo;
- (NSString *)processName;
- (int)processIdentifier;
- (NSArray *)arguments;
- (NSDictionary *)environment;
- (NSUInteger)processorCount;
- (NSUInteger)activeProcessorCount;
- (NSString *)operatingSystemVersionString;
- (NSOperatingSystemVersion)operatingSystemVersion;
- (BOOL)isOperatingSystemAtLeastVersion:(NSOperatingSystemVersion)version;
@end
typedef double NSTimeInterval;
@interface NSDate : NSObject
+ (id)date;
+ (id)distantPast;
+ (id)distantFuture;
+ (id)dateWithTimeIntervalSinceNow:(NSTimeInterval)seconds;
- (NSTimeInterval)timeIntervalSinceNow;
@end
typedef NSString *NSRunLoopMode;
extern NSRunLoopMode const NSDefaultRunLoopMode;
extern NSRunLoopMode const NSRunLoopCommonModes;
extern Class NSClassFromString(NSString *name);
extern SEL NSSelectorFromString(NSString *name);
extern NSString *NSStringFromClass(Class cls);
extern NSString *NSStringFromSelector(SEL selector);
extern void NSLog(NSString *format, ...);
extern void NSLogv(NSString *format, va_list arguments);
#endif
