#include <Foundation/Foundation.h>
#include <stdio.h>
/* NSThread under alchemy-fx (foundation/threads_and_localizations.cpp): threads made with
 * -initWithTarget:selector:object:, -initWithBlock: and a subclass's own -main, each started
 * with -start on its own pthread. Covered: the work runs once on another thread, which
 * +currentThread names; the argument arrives; -isExecuting / -isFinished; -setStackSize:;
 * NSThreadWillExitNotification for each thread. Project-authored; classes are looked up by
 * name and the runtime calls resolve by flat lookup. */

typedef struct objc_selector *SEL_;
typedef id (*Imp)(id, SEL, ...);
extern Class objc_allocateClassPair(Class superclass, const char *name, unsigned long extra);
extern void objc_registerClassPair(Class cls);
extern BOOL class_addMethod(Class cls, SEL name, Imp imp, const char *types);
extern int usleep(unsigned int microseconds);
extern int pthread_main_np(void);

@interface NSObject (ThreadFixture)
+ (id)currentThread;
+ (id)defaultCenter;
- (id)initWithTarget:(id)target selector:(SEL)selector object:(id)argument;
- (id)initWithBlock:(void (^)(void))block;
- (void)start;
- (BOOL)isExecuting;
- (BOOL)isFinished;
- (void)setStackSize:(NSUInteger)size;
- (NSUInteger)stackSize;
- (void)addObserver:(id)observer selector:(SEL)selector name:(id)name object:(id)object;
- (void)removeObserver:(id)observer;
@end

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("Alchemy NSThread fixture: FAIL %s\n", what); failures++; }
}
static volatile int target_runs, block_runs, main_runs, exits, off_main;
static id volatile target_thread, main_thread_seen;
static id volatile received;

@interface ThreadWork : NSObject
@end
@implementation ThreadWork
- (void)work:(id)argument {
    received = argument;
    target_thread = [NSClassFromString(@"NSThread") currentThread];
    if (!pthread_main_np()) __atomic_add_fetch(&off_main, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&target_runs, 1, __ATOMIC_SEQ_CST);
}
- (void)threadWillExit:(id)notification { __atomic_add_fetch(&exits, 1, __ATOMIC_SEQ_CST); }
@end

static id subclass_main(id self, SEL command) {
    main_thread_seen = [NSClassFromString(@"NSThread") currentThread] == self ? self : nil;
    __atomic_add_fetch(&main_runs, 1, __ATOMIC_SEQ_CST);
    return nil;
}

static int wait_until(volatile int *flag, int value) {
    for (int tries = 0; tries < 1000 && __atomic_load_n(flag, __ATOMIC_SEQ_CST) < value; ++tries) usleep(10000);
    return __atomic_load_n(flag, __ATOMIC_SEQ_CST) >= value;
}
static int wait_finished(id thread) {
    for (int tries = 0; tries < 1000 && ![thread isFinished]; ++tries) usleep(10000);
    return [thread isFinished];
}

int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    puts("Alchemy NSThread fixture: launched");
    Class thread_class = NSClassFromString(@"NSThread");
    check(thread_class != nil, "NSThread");
    ThreadWork *work = [[ThreadWork alloc] init];
    [[NSClassFromString(@"NSNotificationCenter") defaultCenter] addObserver:work selector:@selector(threadWillExit:)
                                                                       name:@"NSThreadWillExitNotification" object:nil];

    // A target and selector with an argument.
    id payload = @"payload";
    id first = [[thread_class alloc] initWithTarget:work selector:@selector(work:) object:payload];
    check(![first isExecuting] && ![first isFinished], "a new thread is neither executing nor finished");
    [first start];
    check(wait_until(&target_runs, 1) && wait_finished(first), "the target's selector ran and the thread finished");
    check(received == payload, "the argument arrived");
    check(target_thread == first, "+currentThread on the started thread is the thread object");
    check(off_main == 1, "the work ran off the main thread");

    // A block.
    id second = [[thread_class alloc] initWithBlock:^{ __atomic_add_fetch(&block_runs, 1, __ATOMIC_SEQ_CST); }];
    [second start];
    check(wait_until(&block_runs, 1) && wait_finished(second), "the block ran");

    // A subclass with its own -main, and a stack size.
    Class subclass = objc_allocateClassPair(thread_class, "AlchemyFixtureThread", 0);
    class_addMethod(subclass, @selector(main), (Imp)subclass_main, "v@:");
    objc_registerClassPair(subclass);
    id third = [[subclass alloc] init];
    [third setStackSize:1u << 20];
    check([third stackSize] == 1u << 20, "-setStackSize:");
    [third start];
    check(wait_until(&main_runs, 1) && wait_finished(third), "the subclass's -main ran");
    check(main_thread_seen == third, "+currentThread inside -main");

    check(wait_until(&exits, 3), "NSThreadWillExitNotification for each thread");
    check(target_runs == 1 && block_runs == 1 && main_runs == 1, "each thread ran its work once");
    [[NSClassFromString(@"NSNotificationCenter") defaultCenter] removeObserver:work];
    [first release];
    [second release];
    [third release];
    [work release];
    [pool drain];
    if (failures) { printf("Alchemy NSThread fixture: %d failure(s)\n", failures); return 1; }
    puts("Alchemy NSThread fixture: complete");
    return 0;
}
