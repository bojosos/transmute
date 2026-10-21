/* Link-only libc identity for the GUI fixture apps (install name
 * /usr/lib/system/libsystem_c.dylib, which Alchemy resolves as libSystem).
 * It names the stdio and string functions the fixtures use beyond the base
 * libSystem link stub. Never mapped: Alchemy's libSystem provider supplies
 * the real implementations by install name. */
void *__stdoutp;
void *__stderrp;
#define LINK_ONLY(name) int name(void) { return -777; }
LINK_ONLY(fflush)
LINK_ONLY(fprintf)
LINK_ONLY(vfprintf)
LINK_ONLY(strcmp)
LINK_ONLY(strtol)
