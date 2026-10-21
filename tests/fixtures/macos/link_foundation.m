#include <Foundation/Foundation.h>
/* Metadata supplies class imports to LLD; guest execution uses authored providers. */
#pragma clang diagnostic ignored "-Wincomplete-implementation"
@implementation NSString
@end
@implementation NSNumber
@end
@implementation NSData
@end
@implementation NSArray
@end
@implementation NSDictionary
@end
@implementation NSAutoreleasePool
@end
@implementation NSProcessInfo
@end
@implementation NSDate
@end
NSRunLoopMode const NSDefaultRunLoopMode = (NSRunLoopMode)0;
NSRunLoopMode const NSRunLoopCommonModes = (NSRunLoopMode)0;
unsigned long __CFConstantStringClassReference[5];
Class NSClassFromString(NSString *name) { (void)name; return (Class)0; }
SEL NSSelectorFromString(NSString *name) { (void)name; return (SEL)0; }
NSString *NSStringFromClass(Class cls) { (void)cls; return (NSString *)nil; }
NSString *NSStringFromSelector(SEL selector) { (void)selector; return (NSString *)nil; }
void NSLog(NSString *format, ...) { (void)format; }
void NSLogv(NSString *format, va_list arguments) { (void)format; (void)arguments; }
