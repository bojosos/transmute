#ifndef ALCHEMY_SYS_TIME_H
#define ALCHEMY_SYS_TIME_H
#include "../alchemy_c.h"
#ifdef __cplusplus
extern "C" {
#endif
int utimes(const char*, const struct timeval[2]);
#ifdef __cplusplus
}
#endif
#endif
