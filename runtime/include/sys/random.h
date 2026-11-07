/* getentropy(2): up to 256 bytes from the system random source (Alchemy's libSystem provider). */
#ifndef ALCHEMY_SYS_RANDOM_H
#define ALCHEMY_SYS_RANDOM_H
#include "../alchemy_c.h"
#ifdef __cplusplus
extern "C" {
#endif
int getentropy(void* buffer, size_t size);
#ifdef __cplusplus
}
#endif
#endif
