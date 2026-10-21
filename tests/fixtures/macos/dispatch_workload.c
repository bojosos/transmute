#include <dispatch/dispatch.h>
/* Explicit guest declarations, without an Apple SDK or host objects. */
extern int pthread_create(unsigned long *, const void *, void *(*)(void *), void *);
extern int pthread_join(unsigned long, void **);
extern int printf(const char *, ...);
static dispatch_semaphore_t start, complete;
static int answer;
static void *worker(void *value) {
    (void)value;
    if (dispatch_semaphore_wait(start, DISPATCH_TIME_FOREVER)) return (void *)1;
    answer = 27;
    dispatch_semaphore_signal(complete);
    return 0;
}
int main(void) {
    unsigned long thread;
    void *result = (void *)1;
    start = dispatch_semaphore_create(0);
    complete = dispatch_semaphore_create(0);
    if (!start || !complete || !dispatch_semaphore_wait(start, DISPATCH_TIME_NOW)) return 1;
    dispatch_retain(start);
    dispatch_release(start);
    if (pthread_create(&thread, 0, worker, 0)) return 2;
    dispatch_semaphore_signal(start);
    if (dispatch_semaphore_wait(complete, dispatch_time(DISPATCH_TIME_NOW, 5LL * 1000000000)) ||
        pthread_join(thread, &result) || result || answer != 27) return 3;
    if (!dispatch_semaphore_wait(complete, DISPATCH_TIME_NOW)) return 4;
    dispatch_release(start);
    dispatch_release(complete);
    printf("Guest dispatch semaphores: %d\n", answer);
    return 0;
}
