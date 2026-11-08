/* select(2); fd_set comes from <sys/types.h>, as on Darwin. */
#ifndef ALCHEMY_SYS_SELECT_H
#define ALCHEMY_SYS_SELECT_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif
int select(int count, fd_set* __restrict read, fd_set* __restrict write, fd_set* __restrict error,
           struct timeval* __restrict timeout);
#ifdef __cplusplus
}
#endif
#endif
