#ifndef ALCHEMY_MALLOC_MALLOC_H
#define ALCHEMY_MALLOC_MALLOC_H
/* Published Darwin malloc introspection declarations. Alchemy's libSystem
   provider implements them. Independently authored; no SDK content. */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _malloc_zone_t malloc_zone_t;
size_t malloc_size(const void* ptr);
size_t malloc_good_size(size_t size);
malloc_zone_t* malloc_default_zone(void);
malloc_zone_t* malloc_zone_from_ptr(const void* ptr);
void* malloc_zone_malloc(malloc_zone_t* zone, size_t size);
void* malloc_zone_calloc(malloc_zone_t* zone, size_t count, size_t size);
void* malloc_zone_realloc(malloc_zone_t* zone, void* ptr, size_t size);
void* malloc_zone_memalign(malloc_zone_t* zone, size_t alignment, size_t size);
void malloc_zone_free(malloc_zone_t* zone, void* ptr);
#ifdef __cplusplus
}
#endif
#endif
