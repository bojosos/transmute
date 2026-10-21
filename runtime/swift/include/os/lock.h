#ifndef ALCHEMY_OS_LOCK_H
#define ALCHEMY_OS_LOCK_H
/* Independently authored declarations of the published os_unfair_lock API.
   Alchemy's libSystem provider implements these functions. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct os_unfair_lock_s { uint32_t _os_unfair_lock_opaque; } os_unfair_lock, *os_unfair_lock_t;
#define OS_UNFAIR_LOCK_INIT ((os_unfair_lock){0})
void os_unfair_lock_lock(os_unfair_lock_t lock);
int os_unfair_lock_trylock(os_unfair_lock_t lock);
void os_unfair_lock_unlock(os_unfair_lock_t lock);
void os_unfair_lock_assert_owner(const os_unfair_lock* lock);
void os_unfair_lock_assert_not_owner(const os_unfair_lock* lock);
#ifdef __cplusplus
}
#endif
#endif
