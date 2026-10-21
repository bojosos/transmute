#ifndef ALCHEMY_SWIFT_PTHREAD_H
#define ALCHEMY_SWIFT_PTHREAD_H
/* Darwin pthread extensions used by the Swift runtime, layered over the
   project's base declarations. Independently authored; no SDK content. */
#include_next <pthread.h>
#include <sched.h>
#ifdef __cplusplus
extern "C" {
#endif
int pthread_main_np(void);
int pthread_threadid_np(pthread_t thread, uint64_t* thread_id);
int pthread_getname_np(pthread_t thread, char* name, size_t length);
int pthread_setname_np(const char* name);
void* pthread_get_stackaddr_np(pthread_t thread);
size_t pthread_get_stacksize_np(pthread_t thread);
int pthread_key_init_np(int key, void (*destructor)(void*));
#ifdef __cplusplus
}
#endif
#endif
