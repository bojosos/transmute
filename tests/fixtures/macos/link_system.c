/* Project-authored link-only declarations. Alchemy supplies these at runtime. */
int printf(const char *format, ...) { (void)format; return -777; }
int puts(const char *text) { (void)text; return -777; }
int alchemy_missing_export(void) { return -777; }

/* Link identities only. These bodies are never mapped by Alchemy. */
#define LINK_ONLY(name) int name(void) { return -777; }
LINK_ONLY(pthread_create)
LINK_ONLY(pthread_join)
LINK_ONLY(pthread_detach)
LINK_ONLY(pthread_self)
LINK_ONLY(pthread_exit)
LINK_ONLY(pthread_mutex_lock)
LINK_ONLY(pthread_mutex_unlock)
LINK_ONLY(pthread_mutex_trylock)
LINK_ONLY(pthread_mutex_init)
LINK_ONLY(pthread_mutex_destroy)
LINK_ONLY(pthread_cond_wait)
LINK_ONLY(pthread_cond_signal)
LINK_ONLY(pthread_cond_broadcast)
LINK_ONLY(pthread_key_create)
LINK_ONLY(pthread_key_delete)
LINK_ONLY(pthread_setspecific)
LINK_ONLY(pthread_getspecific)
LINK_ONLY(pthread_once)
LINK_ONLY(dlopen)
LINK_ONLY(dlsym)
LINK_ONLY(dlclose)
LINK_ONLY(dlerror)
LINK_ONLY(dladdr)
LINK_ONLY(__cxa_atexit)
