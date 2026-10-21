/* Project-authored link-only libSystem identities for the cross-process Mach
 * fixture. Alchemy supplies these at run time; the bodies are never mapped. */
#define LINK_ONLY(name) int name(void) { return -777; }
char** environ;
unsigned mach_task_self_ = 0;
unsigned bootstrap_port = 0;
LINK_ONLY(mach_port_allocate)
LINK_ONLY(mach_port_insert_right)
LINK_ONLY(mach_port_request_notification)
LINK_ONLY(mach_msg)
LINK_ONLY(bootstrap_check_in)
LINK_ONLY(bootstrap_look_up)
LINK_ONLY(task_get_special_port)
LINK_ONLY(posix_spawnattr_init)
LINK_ONLY(posix_spawnattr_setspecialport_np)
LINK_ONLY(posix_spawn)
LINK_ONLY(waitpid)
LINK_ONLY(getpid)
LINK_ONLY(puts)
LINK_ONLY(pipe)
LINK_ONLY(write)
LINK_ONLY(read)
LINK_ONLY(fileport_makeport)
LINK_ONLY(fileport_makefd)
LINK_ONLY(mach_make_memory_entry_64)
LINK_ONLY(mach_vm_map)
LINK_ONLY(pid_for_task)
LINK_ONLY(task_policy_set)
LINK_ONLY(task_policy_get)
LINK_ONLY(task_info)
