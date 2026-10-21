typedef unsigned long size_t;
typedef void* pthread_t;
typedef unsigned long pthread_key_t;
typedef struct { long sig; char opaque[56]; } pthread_mutex_t;
typedef struct { long sig; char opaque[40]; } pthread_cond_t;
extern int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern int pthread_mutex_lock(pthread_mutex_t*);
extern int pthread_mutex_unlock(pthread_mutex_t*);
extern int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern int pthread_cond_broadcast(pthread_cond_t*);
extern int pthread_key_create(pthread_key_t*, void (*)(void*));
extern int pthread_setspecific(pthread_key_t, const void*);
extern void* pthread_getspecific(pthread_key_t);
extern int puts(const char*);
static pthread_mutex_t mutex = {0x32aaaba7, {0}};
static pthread_cond_t condition = {0x3cb0b1bb, {0}};
static pthread_key_t key;
static int ready;
static int completed;
static void* worker(void* argument) {
    if (pthread_setspecific(key, argument)) return (void*)1;
    if (pthread_mutex_lock(&mutex)) return (void*)2;
    while (!ready) if (pthread_cond_wait(&condition, &mutex)) return (void*)3;
    ++completed;
    if (pthread_mutex_unlock(&mutex)) return (void*)4;
    return pthread_getspecific(key);
}
int main(void) {
    pthread_t a, b;
    void* ra;
    void* rb;
    if (pthread_key_create(&key, 0)) return 1;
    if (pthread_create(&a, 0, worker, (void*)27)) return 2;
    if (pthread_create(&b, 0, worker, (void*)42)) return 3;
    if (pthread_mutex_lock(&mutex)) return 4;
    ready = 1;
    if (pthread_cond_broadcast(&condition)) return 5;
    if (pthread_mutex_unlock(&mutex)) return 6;
    if (pthread_join(a, &ra) || pthread_join(b, &rb)) return 7;
    if (ra != (void*)27 || rb != (void*)42 || completed != 2) return 8;
    if (pthread_getspecific(key) != 0) return 9;
    puts("Darwin threads: 27 42");
    return 0;
}
