/* Project-authored link-only libSystem identities for fxbench. Alchemy supplies these at runtime;
 * these bodies are never mapped. */
#define LINK_ONLY(name) int name(void) { return -777; }
LINK_ONLY(printf)
LINK_ONLY(memcpy)
LINK_ONLY(memset)
LINK_ONLY(memcmp)
LINK_ONLY(strlen)
LINK_ONLY(mach_absolute_time)
LINK_ONLY(mach_timebase_info)
LINK_ONLY(pthread_create)
LINK_ONLY(pthread_join)
