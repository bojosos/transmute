/* Declarations for the run loop guest fixture (owner: A2). Independently
 * written from Apple's public CoreFoundation, libdispatch and Objective-C
 * runtime documentation; no SDK headers. */
#ifndef ALCHEMY_RUNLOOP_FIXTURE_H
#define ALCHEMY_RUNLOOP_FIXTURE_H

typedef unsigned long size_t;
typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned char Boolean;
typedef long CFIndex;
typedef unsigned long CFOptionFlags;
typedef double CFTimeInterval;
typedef double CFAbsoluteTime;
typedef const void *CFTypeRef;
typedef const void *CFStringRef;
typedef const void *CFArrayRef;
typedef void *CFAllocatorRef;
typedef struct AlchemyRunLoop *CFRunLoopRef;
typedef struct AlchemyRunLoopTimer *CFRunLoopTimerRef;
typedef struct AlchemyRunLoopSource *CFRunLoopSourceRef;
typedef struct AlchemyRunLoopObserver *CFRunLoopObserverRef;
typedef void (*CFRunLoopTimerCallBack)(CFRunLoopTimerRef timer, void *info);
typedef void (*CFRunLoopObserverCallBack)(CFRunLoopObserverRef observer, CFOptionFlags activity, void *info);
typedef struct {
    CFIndex version;
    void *info;
    const void *(*retain)(const void *info);
    void (*release)(const void *info);
    CFStringRef (*copyDescription)(const void *info);
} CFRunLoopTimerContext;
typedef CFRunLoopTimerContext CFRunLoopObserverContext;
typedef struct {
    CFIndex version;
    void *info;
    const void *(*retain)(const void *info);
    void (*release)(const void *info);
    CFStringRef (*copyDescription)(const void *info);
    Boolean (*equal)(const void *info1, const void *info2);
    unsigned long (*hash)(const void *info);
    void (*schedule)(void *info, CFRunLoopRef rl, CFStringRef mode);
    void (*cancel)(void *info, CFRunLoopRef rl, CFStringRef mode);
    void (*perform)(void *info);
} CFRunLoopSourceContext;
enum { kCFRunLoopRunFinished = 1, kCFRunLoopRunStopped = 2, kCFRunLoopRunTimedOut = 3, kCFRunLoopRunHandledSource = 4 };
enum { kCFRunLoopEntry = 1, kCFRunLoopExit = 1 << 7 };

extern const CFStringRef kCFRunLoopDefaultMode;
extern const CFStringRef kCFRunLoopCommonModes;
CFAbsoluteTime CFAbsoluteTimeGetCurrent(void);
void CFRelease(CFTypeRef value);
CFIndex CFArrayGetCount(CFArrayRef array);
CFRunLoopRef CFRunLoopGetMain(void);
CFRunLoopRef CFRunLoopGetCurrent(void);
int CFRunLoopRunInMode(CFStringRef mode, CFTimeInterval seconds, Boolean returnAfterSourceHandled);
void CFRunLoopStop(CFRunLoopRef rl);
void CFRunLoopWakeUp(CFRunLoopRef rl);
void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef timer, CFStringRef mode);
CFRunLoopTimerRef CFRunLoopTimerCreate(CFAllocatorRef allocator, CFAbsoluteTime fireDate, CFTimeInterval interval,
                                       CFOptionFlags flags, CFIndex order, CFRunLoopTimerCallBack callout,
                                       CFRunLoopTimerContext *context);
CFRunLoopTimerRef CFRunLoopTimerCreateWithHandler(CFAllocatorRef allocator, CFAbsoluteTime fireDate, CFTimeInterval interval,
                                                  CFOptionFlags flags, CFIndex order, void (^block)(CFRunLoopTimerRef timer));
void CFRunLoopTimerInvalidate(CFRunLoopTimerRef timer);
Boolean CFRunLoopTimerIsValid(CFRunLoopTimerRef timer);
CFRunLoopSourceRef CFRunLoopSourceCreate(CFAllocatorRef allocator, CFIndex order, CFRunLoopSourceContext *context);
void CFRunLoopAddSource(CFRunLoopRef rl, CFRunLoopSourceRef source, CFStringRef mode);
void CFRunLoopSourceSignal(CFRunLoopSourceRef source);
void CFRunLoopSourceInvalidate(CFRunLoopSourceRef source);
CFRunLoopObserverRef CFRunLoopObserverCreateWithHandler(CFAllocatorRef allocator, CFOptionFlags activities, Boolean repeats,
                                                        CFIndex order, void (^block)(CFRunLoopObserverRef observer, CFOptionFlags activity));
void CFRunLoopAddObserver(CFRunLoopRef rl, CFRunLoopObserverRef observer, CFStringRef mode);
void CFRunLoopObserverInvalidate(CFRunLoopObserverRef observer);

typedef void *dispatch_queue_t;
typedef void *dispatch_group_t;
typedef void *dispatch_source_t;
typedef const void *dispatch_source_type_t;
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW 0UL
#define DISPATCH_TIME_FOREVER (~0UL)
extern char _dispatch_main_q[];
extern char _dispatch_queue_attr_concurrent[];
extern const char _dispatch_source_type_timer[];
extern const char _dispatch_source_type_data_add[];
dispatch_queue_t dispatch_get_global_queue(long identifier, unsigned long flags);
dispatch_queue_t dispatch_queue_create(const char *label, void *attributes);
void dispatch_async(dispatch_queue_t queue, void (^block)(void));
void dispatch_barrier_async(dispatch_queue_t queue, void (^block)(void));
void dispatch_barrier_sync(dispatch_queue_t queue, void (^block)(void));
void dispatch_apply(size_t iterations, dispatch_queue_t queue, void (^block)(size_t index));
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);
void dispatch_release(void *object);
void dispatch_resume(void *object);
dispatch_group_t dispatch_group_create(void);
void dispatch_group_async(dispatch_group_t group, dispatch_queue_t queue, void (^block)(void));
void dispatch_group_notify(dispatch_group_t group, dispatch_queue_t queue, void (^block)(void));
long dispatch_group_wait(dispatch_group_t group, dispatch_time_t timeout);
dispatch_source_t dispatch_source_create(dispatch_source_type_t type, uint64_t handle, unsigned long mask, dispatch_queue_t queue);
void dispatch_source_set_event_handler(dispatch_source_t source, void (^block)(void));
void dispatch_source_set_cancel_handler(dispatch_source_t source, void (^block)(void));
void dispatch_source_set_timer(dispatch_source_t source, dispatch_time_t start, uint64_t interval, uint64_t leeway);
void dispatch_source_cancel(dispatch_source_t source);
unsigned long dispatch_source_get_data(dispatch_source_t source);
void dispatch_source_merge_data(dispatch_source_t source, unsigned long value);

typedef struct objc_object *id;
typedef struct objc_selector *SEL;
id objc_getClass(const char *name);
SEL sel_registerName(const char *name);
void objc_msgSend(void);

int printf(const char *format, ...);
int usleep(unsigned int microseconds);
int pthread_create(unsigned long *thread, const void *attributes, void *(*start)(void *), void *argument);
int pthread_join(unsigned long thread, void **result);

#endif
