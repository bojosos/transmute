#include <Foundation/Foundation.h>
extern int puts(const char *);
extern int strcmp(const char *, const char *);
extern int getpid(void);
typedef unsigned long pthread_t;
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
static int destroyed;
@interface FoundationPoolProbe : NSObject
@end
@implementation FoundationPoolProbe
- (void)dealloc { __atomic_add_fetch(&destroyed, 1, __ATOMIC_SEQ_CST); [super dealloc]; }
@end
static void *worker(void *argument) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    for (int index = 0; index < 7; ++index) [[[FoundationPoolProbe alloc] init] autorelease];
    if (![[NSProcessInfo processInfo] processorCount]) return (void *)1;
    [pool drain];
    return argument;
}
int main(int argc, char **argv) {
    NSAutoreleasePool *outer = [[NSAutoreleasePool alloc] init];
    NSProcessInfo *info = [NSProcessInfo processInfo];
    if (info != [NSProcessInfo processInfo] || [info processIdentifier] != getpid()) return 1;
    NSArray *arguments = [info arguments];
    if ([arguments count] != (NSUInteger)argc || strcmp([[arguments objectAtIndex:0] UTF8String], argv[0])) return 2;
    if (![[info processName] length] || ![info processorCount] || ![info activeProcessorCount]) return 3;
    NSOperatingSystemVersion version = [info operatingSystemVersion];
    if (version.majorVersion != 14 || version.minorVersion || version.patchVersion) return 4;
    NSOperatingSystemVersion earlier = {10, 15, 7}, same = {14, 0, 0}, newer = {14, 0, 1};
    if (![info isOperatingSystemAtLeastVersion:earlier] || ![info isOperatingSystemAtLeastVersion:same] ||
        [info isOperatingSystemAtLeastVersion:newer]) return 5;
    if (strcmp([[info operatingSystemVersionString] UTF8String], "Alchemy macOS 14.0.0 compatibility profile")) return 6;
    NSAutoreleasePool *inner = [[NSAutoreleasePool alloc] init];
    [[[FoundationPoolProbe alloc] init] autorelease];
    [inner release];
    if (destroyed != 1) return 7;
    [[[FoundationPoolProbe alloc] init] autorelease];
    pthread_t first, second;
    if (pthread_create(&first, 0, worker, (void *)27) || pthread_create(&second, 0, worker, (void *)42)) return 8;
    void *a, *b;
    if (pthread_join(first, &a) || pthread_join(second, &b) || a != (void *)27 || b != (void *)42 || destroyed != 15) return 9;
    [outer drain];
    if (destroyed != 16) return 10;
    puts("Foundation process services: 27");
    return 0;
}
