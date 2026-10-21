/* Run loop and dispatch guest fixture (owner: A2): CFRunLoop timers, sources
 * and observers, main-queue wakes, NSTimer / NSRunLoop / delayed performs and
 * dispatch groups, barriers, apply and timer sources, as compiled guest code
 * on alchemy-fx. Prints "runloop fixture: passed" on success. */
#include "runloop_fixture.h"

#define CHECK(condition, message) \
    do { if (!(condition)) { printf("runloop fixture: FAIL %s (line %d)\n", message, __LINE__); return 1; } } while (0)

typedef id (*send_id)(id, SEL);
typedef id (*send_id_double)(id, SEL, double);
typedef void (*send_void_id)(id, SEL, id);
typedef unsigned char (*send_bool_id_id)(id, SEL, id, id);
typedef id (*send_timer_block)(id, SEL, double, unsigned char, void (^)(id));
typedef void (*send_perform_delay)(id, SEL, SEL, id, double);
typedef unsigned char (*send_bool)(id, SEL);

static int order[4], order_count;
static volatile int flag, ticks, observed, notified, timer_ticks, cancelled;
static long counter, before, barrier_saw, apply_sum;
static CFRunLoopSourceRef worker_source;

static void timer_callout(CFRunLoopTimerRef timer, void *info) { (void)timer; order[order_count++] = (int)(long)info; }
static void source_perform(void *info) { flag = (int)(long)info; }

static void *post_main_queue(void *argument) {
    (void)argument;
    usleep(50000);
    dispatch_async((dispatch_queue_t)_dispatch_main_q, ^{ flag = 41; });
    return 0;
}
static void *signal_source(void *argument) {
    (void)argument;
    usleep(50000);
    CFRunLoopSourceSignal(worker_source);
    CFRunLoopWakeUp(CFRunLoopGetMain());
    return 0;
}

static int run_loop_tests(void) {
    CFRunLoopRef main = CFRunLoopGetMain();
    CHECK(main && main == CFRunLoopGetCurrent(), "CFRunLoopGetMain");
    /* Timers fire by date, not by creation order. */
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    const double offsets[3] = {0.03, 0.01, 0.02};
    CFRunLoopTimerRef timers[3];
    for (int index = 0; index < 3; ++index) {
        CFRunLoopTimerContext context = {0, (void *)(long)(index + 1), 0, 0, 0};
        timers[index] = CFRunLoopTimerCreate(0, now + offsets[index], 0, 0, 0, timer_callout, &context);
        CFRunLoopAddTimer(main, timers[index], kCFRunLoopDefaultMode);
    }
    CHECK(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.15, 0) == kCFRunLoopRunTimedOut, "timed run");
    CHECK(order_count == 3 && order[0] == 2 && order[1] == 3 && order[2] == 1, "timer order");
    for (int index = 0; index < 3; ++index) {
        CHECK(!CFRunLoopTimerIsValid(timers[index]), "fired one-shot timer still valid");
        CFRelease(timers[index]);
    }
    /* A repeating handler timer stops the loop on its third fire. */
    CFRunLoopTimerRef repeating = CFRunLoopTimerCreateWithHandler(0, CFAbsoluteTimeGetCurrent() + 0.005, 0.005, 0, 0,
        ^(CFRunLoopTimerRef timer) { if (++ticks == 3) { CFRunLoopTimerInvalidate(timer); CFRunLoopStop(CFRunLoopGetMain()); } });
    CFRunLoopAddTimer(main, repeating, kCFRunLoopCommonModes);
    CHECK(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 5, 0) == kCFRunLoopRunStopped && ticks == 3, "repeating timer and stop");
    CFRelease(repeating);
    /* Main-queue work from another thread wakes a waiting loop. */
    unsigned long thread;
    CHECK(pthread_create(&thread, 0, post_main_queue, 0) == 0, "pthread_create");
    CHECK(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 5, 1) == kCFRunLoopRunHandledSource && flag == 41, "main queue wake");
    CHECK(pthread_join(thread, 0) == 0, "pthread_join");
    /* A version-0 source signalled from another thread. */
    CFRunLoopSourceContext context = {0, (void *)42L, 0, 0, 0, 0, 0, 0, 0, source_perform};
    worker_source = CFRunLoopSourceCreate(0, 0, &context);
    CFRunLoopAddSource(main, worker_source, kCFRunLoopDefaultMode);
    CHECK(pthread_create(&thread, 0, signal_source, 0) == 0, "pthread_create");
    CHECK(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 5, 1) == kCFRunLoopRunHandledSource && flag == 42, "source signal");
    CHECK(pthread_join(thread, 0) == 0, "pthread_join");
    CFRunLoopSourceInvalidate(worker_source);
    CFRelease(worker_source);
    /* Observers see entry and exit. */
    CFRunLoopObserverRef observer = CFRunLoopObserverCreateWithHandler(0, kCFRunLoopEntry | kCFRunLoopExit, 1, 0,
        ^(CFRunLoopObserverRef value, CFOptionFlags activity) { (void)value; observed |= (int)activity; });
    CFRunLoopAddObserver(main, observer, kCFRunLoopCommonModes);
    CHECK(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, 0) == kCFRunLoopRunTimedOut, "poll run");
    CHECK(observed == (kCFRunLoopEntry | kCFRunLoopExit), "observer activities");
    CFRunLoopObserverInvalidate(observer);
    CFRelease(observer);
    return 0;
}

static int foundation_tests(void) {
    static volatile int fired;
    id timer_class = objc_getClass("NSTimer"), run_loop_class = objc_getClass("NSRunLoop"), date_class = objc_getClass("NSDate");
    CHECK(timer_class && run_loop_class && date_class, "Foundation classes");
    id timer = ((send_timer_block)objc_msgSend)(timer_class, sel_registerName("scheduledTimerWithTimeInterval:repeats:block:"),
                                                 0.01, 0, ^(id value) { (void)value; ++fired; });
    CHECK(timer != 0, "scheduled NSTimer");
    id loop = ((send_id)objc_msgSend)(run_loop_class, sel_registerName("currentRunLoop"));
    CHECK(loop == ((send_id)objc_msgSend)(run_loop_class, sel_registerName("mainRunLoop")), "NSRunLoop identity");
    id until = ((send_id_double)objc_msgSend)(date_class, sel_registerName("dateWithTimeIntervalSinceNow:"), 0.1);
    ((send_void_id)objc_msgSend)(loop, sel_registerName("runUntilDate:"), until);
    CHECK(fired == 1, "NSTimer block fired");
    /* -performSelector:withObject:afterDelay: on a mutable array. */
    id array = ((send_id)objc_msgSend)(objc_getClass("NSMutableArray"), sel_registerName("array"));
    ((send_perform_delay)objc_msgSend)(array, sel_registerName("performSelector:withObject:afterDelay:"),
                                       sel_registerName("addObject:"), array, 0.01);
    until = ((send_id_double)objc_msgSend)(date_class, sel_registerName("dateWithTimeIntervalSinceNow:"), 0.1);
    CHECK(((send_bool_id_id)objc_msgSend)(loop, sel_registerName("runMode:beforeDate:"), (id)kCFRunLoopDefaultMode, until),
          "runMode:beforeDate:");
    CHECK(CFArrayGetCount(array) == 1, "delayed perform");
    return 0;
}

static int dispatch_tests(void) {
    dispatch_queue_t global = dispatch_get_global_queue(0, 0);
    /* Groups. */
    dispatch_group_t group = dispatch_group_create();
    for (int index = 0; index < 4; ++index)
        dispatch_group_async(group, global, ^{ usleep(5000); __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST); });
    CHECK(dispatch_group_wait(group, DISPATCH_TIME_FOREVER) == 0 && counter == 4, "group wait");
    dispatch_group_async(group, global, ^{ usleep(20000); });
    dispatch_group_notify(group, (dispatch_queue_t)_dispatch_main_q, ^{ notified = 1; });
    dispatch_release(group);
    for (int pass = 0; pass < 100 && !notified; ++pass) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, 1);
    CHECK(notified, "group notify on the main queue");
    /* Barriers. */
    dispatch_queue_t queue = dispatch_queue_create("fixture.concurrent", _dispatch_queue_attr_concurrent);
    for (int index = 0; index < 3; ++index)
        dispatch_async(queue, ^{ usleep(10000); __atomic_fetch_add(&before, 1, __ATOMIC_SEQ_CST); });
    dispatch_barrier_async(queue, ^{ barrier_saw = __atomic_load_n(&before, __ATOMIC_SEQ_CST); });
    dispatch_barrier_sync(queue, ^{});
    CHECK(barrier_saw == 3, "barrier ran before earlier work finished");
    dispatch_release(queue);
    /* dispatch_apply. */
    dispatch_apply(100, global, ^(size_t index) { __atomic_fetch_add(&apply_sum, (long)index, __ATOMIC_SEQ_CST); });
    CHECK(apply_sum == 4950, "dispatch_apply");
    /* A timer source on the main queue drives the run loop. */
    dispatch_source_t source = dispatch_source_create((dispatch_source_type_t)_dispatch_source_type_timer, 0, 0,
                                                      (dispatch_queue_t)_dispatch_main_q);
    CHECK(source != 0, "timer source");
    dispatch_source_set_event_handler(source, ^{ if (++timer_ticks == 3) dispatch_source_cancel(source); });
    dispatch_source_set_cancel_handler(source, ^{ cancelled = 1; CFRunLoopStop(CFRunLoopGetMain()); });
    dispatch_source_set_timer(source, dispatch_time(DISPATCH_TIME_NOW, 10000000), 10000000, 1000000);
    dispatch_resume(source);
    CHECK(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 5, 0) == kCFRunLoopRunStopped, "timer source run");
    CHECK(timer_ticks == 3 && cancelled, "timer source ticks and cancel");
    dispatch_release(source);
    return 0;
}

int main(void) {
    printf("runloop fixture: start\n");
    if (run_loop_tests() || foundation_tests() || dispatch_tests()) return 1;
    printf("runloop fixture: passed\n");
    return 0;
}
