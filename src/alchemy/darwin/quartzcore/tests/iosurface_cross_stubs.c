/* Link-only identities for the cross-process IOSurface fixture (owner: w3).
 * One source, one dylib per identity (-DCROSS_STUB_SYSTEM or
 * -DCROSS_STUB_IOSURFACE). The bodies are never mapped: Alchemy's providers
 * replace each image by its install name. Project-authored; no Apple
 * declarations are used. */
#define LINK_ONLY(name) long name(void) { return -777; }

#if defined(CROSS_STUB_SYSTEM)
char** environ;
unsigned mach_task_self_ = 0;
unsigned bootstrap_port = 0;
LINK_ONLY(mach_port_allocate)
LINK_ONLY(mach_port_deallocate)
LINK_ONLY(mach_msg)
LINK_ONLY(bootstrap_check_in)
LINK_ONLY(bootstrap_look_up)
LINK_ONLY(posix_spawn)
LINK_ONLY(waitpid)
LINK_ONLY(getpid)
LINK_ONLY(puts)
#elif defined(CROSS_STUB_IOSURFACE)
void* kIOSurfaceWidth;
void* kIOSurfaceHeight;
void* kIOSurfaceBytesPerElement;
void* kIOSurfacePixelFormat;
LINK_ONLY(IOSurfaceCreate)
LINK_ONLY(IOSurfaceCreateMachPort)
LINK_ONLY(IOSurfaceLookupFromMachPort)
LINK_ONLY(IOSurfaceLookup)
LINK_ONLY(IOSurfaceGetID)
LINK_ONLY(IOSurfaceGetBaseAddress)
LINK_ONLY(IOSurfaceGetBytesPerRow)
LINK_ONLY(IOSurfaceGetAllocSize)
LINK_ONLY(IOSurfaceGetWidth)
LINK_ONLY(IOSurfaceGetHeight)
LINK_ONLY(IOSurfaceGetPixelFormat)
LINK_ONLY(IOSurfaceLock)
LINK_ONLY(IOSurfaceUnlock)
LINK_ONLY(IOSurfaceIncrementUseCount)
LINK_ONLY(IOSurfaceDecrementUseCount)
LINK_ONLY(IOSurfaceGetUseCount)
#else
#error "select a stub with -DCROSS_STUB_*"
#endif
