#ifndef ALCHEMY_PTHREAD_H
#define ALCHEMY_PTHREAD_H
#include "alchemy_c.h"
typedef struct _opaque_pthread_mutex_t { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct _opaque_pthread_cond_t { long sig; char opaque[40]; } pthread_cond_t;
typedef struct _opaque_pthread_rwlock_t { long sig; char opaque[192]; } pthread_rwlock_t;
typedef struct _opaque_pthread_once_t { long sig; char opaque[8]; } pthread_once_t;
typedef struct _opaque_pthread_mutexattr_t { long sig; char opaque[8]; } pthread_mutexattr_t;
typedef struct _opaque_pthread_condattr_t { long sig; char opaque[8]; } pthread_condattr_t;
typedef struct _opaque_pthread_rwlockattr_t { long sig; char opaque[16]; } pthread_rwlockattr_t;
typedef struct _opaque_pthread_attr_t { long sig; char opaque[56]; } pthread_attr_t;
struct alchemy_pthread_cleanup_record {
    void (*routine)(void*);
    void* argument;
    struct alchemy_pthread_cleanup_record* next;
};
typedef struct _opaque_pthread_t {
    long signature;
    struct alchemy_pthread_cleanup_record* cleanup;
    char opaque[8176];
}* pthread_t;
typedef unsigned long pthread_key_t;
typedef unsigned int mach_port_t;
#define PTHREAD_MUTEX_INITIALIZER {0x32aaaba7, {0}}
#define PTHREAD_COND_INITIALIZER {0x3cb0b1bb, {0}}
#define PTHREAD_RWLOCK_INITIALIZER {0x2da8b3b4, {0}}
#define PTHREAD_ONCE_INIT {0x30b1bcba, {0}}
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE 2
#define PTHREAD_KEYS_MAX 512
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#ifdef __cplusplus
extern "C" {
#endif
int pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
int pthread_detach(pthread_t);
pthread_t pthread_self(void);
mach_port_t pthread_mach_thread_np(pthread_t);
int pthread_equal(pthread_t, pthread_t);
void pthread_exit(void*) __attribute__((noreturn));
int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
int pthread_mutex_destroy(pthread_mutex_t*);
int pthread_mutex_lock(pthread_mutex_t*);
int pthread_mutex_trylock(pthread_mutex_t*);
int pthread_mutex_unlock(pthread_mutex_t*);
int pthread_mutexattr_init(pthread_mutexattr_t*);
int pthread_mutexattr_destroy(pthread_mutexattr_t*);
int pthread_mutexattr_settype(pthread_mutexattr_t*, int);
int pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
int pthread_cond_destroy(pthread_cond_t*);
int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
int pthread_cond_signal(pthread_cond_t*);
int pthread_cond_broadcast(pthread_cond_t*);
int pthread_rwlock_init(pthread_rwlock_t*, const pthread_rwlockattr_t*);
int pthread_rwlock_destroy(pthread_rwlock_t*);
int pthread_rwlock_rdlock(pthread_rwlock_t*);
int pthread_rwlock_wrlock(pthread_rwlock_t*);
int pthread_rwlock_tryrdlock(pthread_rwlock_t*);
int pthread_rwlock_trywrlock(pthread_rwlock_t*);
int pthread_rwlock_unlock(pthread_rwlock_t*);
int pthread_once(pthread_once_t*, void (*)(void));
int pthread_key_create(pthread_key_t*, void (*)(void*));
int pthread_key_delete(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void*);
void* pthread_getspecific(pthread_key_t);
/* Project-authored macro helpers. Stock Darwin cleanup records use the same
   three LP64 fields and public handle offset, without importing these helpers. */
void __alchemy_pthread_cleanup_push(struct alchemy_pthread_cleanup_record*, void (*)(void*), void*);
void __alchemy_pthread_cleanup_pop(struct alchemy_pthread_cleanup_record*, int);
#ifdef __cplusplus
}
#endif
#define pthread_cleanup_push(routine, argument) do { \
    struct alchemy_pthread_cleanup_record _alchemy_cleanup_registration; \
    __alchemy_pthread_cleanup_push(&_alchemy_cleanup_registration, (routine), (argument));
#define pthread_cleanup_pop(execute) \
    __alchemy_pthread_cleanup_pop(&_alchemy_cleanup_registration, (execute)); \
    } while (0)
#endif
