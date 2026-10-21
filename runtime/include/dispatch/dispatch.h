#ifndef ALCHEMY_DISPATCH_H
#define ALCHEMY_DISPATCH_H
/* Independently authored declarations for the bounded semaphore profile. */
#include <stdint.h>
struct timespec;
typedef void *dispatch_semaphore_t;
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW UINT64_C(0)
#define DISPATCH_TIME_FOREVER UINT64_MAX
#define DISPATCH_WALLTIME_NOW (UINT64_MAX - UINT64_C(1))
#ifdef __cplusplus
extern "C" {
#endif
dispatch_semaphore_t dispatch_semaphore_create(intptr_t value);
intptr_t dispatch_semaphore_wait(dispatch_semaphore_t semaphore, dispatch_time_t timeout);
intptr_t dispatch_semaphore_signal(dispatch_semaphore_t semaphore);
void dispatch_retain(void *object);
void dispatch_release(void *object);
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);
dispatch_time_t dispatch_walltime(const struct timespec *when, int64_t delta);
#ifdef __cplusplus
}
#endif
#endif
