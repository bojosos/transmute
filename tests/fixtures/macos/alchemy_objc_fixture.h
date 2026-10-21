#ifndef ALCHEMY_OBJC_FIXTURE_H
#define ALCHEMY_OBJC_FIXTURE_H
/* Project-authored declarations for the bounded Alchemy compatibility profile. */
typedef _Bool BOOL;
typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef unsigned short unichar;
typedef struct objc_class *Class;
typedef struct objc_selector *SEL;
#define nil ((id)0)
#define YES ((BOOL)1)
#define NO ((BOOL)0)

__attribute__((objc_root_class))
@interface NSObject { Class isa; }
+ (id)alloc;
+ (id)new;
+ (Class)class;
- (id)init;
- (Class)class;
- (id)retain;
- (oneway void)release;
- (id)autorelease;
- (void)dealloc;
- (BOOL)isEqual:(id)other;
- (NSUInteger)hash;
- (id)copy;
@end
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
extern Class NSClassFromString(NSString *name);
extern SEL NSSelectorFromString(NSString *name);
extern NSString *NSStringFromClass(Class cls);
extern NSString *NSStringFromSelector(SEL selector);
extern int puts(const char *text);
extern int printf(const char *format, ...);
extern int strcmp(const char *left, const char *right);
extern Class objc_getClass(const char *name);
extern SEL sel_registerName(const char *name);
extern const char *class_getName(Class cls);
extern unsigned long class_getInstanceSize(Class cls);
extern void *dlopen(const char *path, int flags);
extern void *dlsym(void *handle, const char *name);
extern int dlclose(void *handle);
extern const char *dlerror(void);
typedef const void *CFTypeRef;
typedef const void *CFStringRef;
typedef const void *CFArrayRef;
typedef const void *CFDictionaryRef;
typedef const void *CFDataRef;
typedef long CFIndex;
typedef unsigned int CFStringEncoding;
extern CFTypeRef CFRetain(CFTypeRef object);
extern void CFRelease(CFTypeRef object);
extern CFIndex CFGetRetainCount(CFTypeRef object);
extern unsigned char CFEqual(CFTypeRef left, CFTypeRef right);
extern unsigned long CFHash(CFTypeRef object);
extern CFIndex CFStringGetLength(CFStringRef string);
extern unsigned char CFStringGetCString(CFStringRef string, char *buffer, CFIndex capacity, CFStringEncoding encoding);
extern CFIndex CFArrayGetCount(CFArrayRef array);
extern const void *CFArrayGetValueAtIndex(CFArrayRef array, CFIndex index);
extern CFIndex CFDictionaryGetCount(CFDictionaryRef dictionary);
extern const void *CFDictionaryGetValue(CFDictionaryRef dictionary, const void *key);
extern CFIndex CFDataGetLength(CFDataRef data);
extern const unsigned char *CFDataGetBytePtr(CFDataRef data);
#endif
