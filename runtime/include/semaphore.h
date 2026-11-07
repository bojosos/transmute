/* Independently authored Darwin <semaphore.h> declarations (POSIX named
   semaphores; LP64). */
#ifndef ALCHEMY_SEMAPHORE_H
#define ALCHEMY_SEMAPHORE_H
#include "alchemy_c.h"
typedef int sem_t;
#define SEM_FAILED ((sem_t *)-1)
#ifdef __cplusplus
extern "C" {
#endif
sem_t *sem_open(const char *, int, ...);
int sem_close(sem_t *);
int sem_unlink(const char *);
int sem_wait(sem_t *);
int sem_trywait(sem_t *);
int sem_post(sem_t *);
int sem_init(sem_t *, int, unsigned int);
int sem_destroy(sem_t *);
int sem_getvalue(sem_t *, int *);
#ifdef __cplusplus
}
#endif
#endif
