/* Project-authored link-only libSystem identities for the alchemy-fx engine
 * fixtures. Alchemy supplies these at runtime; the bodies are never mapped. */
#define LINK_ONLY(name) int name(void) { return -777; }
LINK_ONLY(puts)
LINK_ONLY(exit)
LINK_ONLY(abort)
LINK_ONLY(usleep)
LINK_ONLY(sched_yield)
LINK_ONLY(pthread_create)
LINK_ONLY(pthread_join)
LINK_ONLY(pthread_detach)
LINK_ONLY(pthread_exit)
LINK_ONLY(pthread_attr_init)
LINK_ONLY(pthread_attr_destroy)
LINK_ONLY(pthread_attr_setstacksize)
LINK_ONLY(pthread_mutex_lock)
LINK_ONLY(pthread_mutex_unlock)
LINK_ONLY(pthread_cond_wait)
LINK_ONLY(pthread_cond_broadcast)
LINK_ONLY(dispatch_semaphore_create)
LINK_ONLY(dispatch_semaphore_wait)
LINK_ONLY(dispatch_semaphore_signal)
LINK_ONLY(vm_allocate)
LINK_ONLY(vm_deallocate)
LINK_ONLY(vm_protect)
LINK_ONLY(vm_remap)
LINK_ONLY(mmap)
LINK_ONLY(munmap)
LINK_ONLY(mprotect)
LINK_ONLY(madvise)
LINK_ONLY(__error)
LINK_ONLY(mach_vm_region_recurse)
LINK_ONLY(mach_vm_region)
LINK_ONLY(open)
LINK_ONLY(close)
LINK_ONLY(pwrite)
LINK_ONLY(pread)
LINK_ONLY(unlink)
LINK_ONLY(getpid)
LINK_ONLY(mach_vm_purgable_control)
LINK_ONLY(mach_port_allocate)
LINK_ONLY(mach_port_insert_right)
LINK_ONLY(mach_port_mod_refs)
LINK_ONLY(mach_port_deallocate)
LINK_ONLY(mach_msg)
LINK_ONLY(mach_make_memory_entry_64)
LINK_ONLY(mach_vm_map)
LINK_ONLY(kqueue)
LINK_ONLY(kevent64)
LINK_ONLY(poll)
LINK_ONLY(pipe)
LINK_ONLY(_exit)
LINK_ONLY(dlopen)
LINK_ONLY(dlsym)
LINK_ONLY(pthread_once)
LINK_ONLY(_tlv_bootstrap)
LINK_ONLY(sys_icache_invalidate)
LINK_ONLY(pthread_jit_write_protect_np)
unsigned mach_task_self_ = 0;
/* Pure memory/string calls (fx_leaf_calls), with their own prototypes. */
void* memcpy(void* d, const void* s, unsigned long n) { (void)s; (void)n; return d; }
void* memmove(void* d, const void* s, unsigned long n) { (void)s; (void)n; return d; }
void bcopy(const void* s, void* d, unsigned long n) { (void)s; (void)d; (void)n; }
void* memset(void* d, int c, unsigned long n) { (void)c; (void)n; return d; }
void bzero(void* d, unsigned long n) { (void)d; (void)n; }
int memcmp(const void* x, const void* y, unsigned long n) { (void)x; (void)y; (void)n; return -777; }
unsigned long strlen(const char* s) { (void)s; return 0; }
void __chkstk_darwin(void) {}
