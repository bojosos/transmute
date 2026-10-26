/* Link-only identities for the IOSurface-backed MTLTexture fixture (owner:
 * g3). One source, one dylib per identity (-DMETAL_IOSURFACE_STUB_SYSTEM or
 * -DMETAL_IOSURFACE_STUB_IOSURFACE). The bodies are never mapped: Alchemy's
 * providers replace each image by its install name. Project-authored; no
 * Apple declarations are used. */
#define LINK_ONLY(name) long name(void) { return -777; }

#if defined(METAL_IOSURFACE_STUB_SYSTEM)
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
LINK_ONLY(mach_absolute_time)
LINK_ONLY(mach_timebase_info)
LINK_ONLY(fopen)
LINK_ONLY(fread)
LINK_ONLY(fseek)
LINK_ONLY(fclose)
LINK_ONLY(unlink)
void* memcpy(void* destination, const void* source, unsigned long size) { (void)source; (void)size; return destination; }
#elif defined(METAL_IOSURFACE_STUB_IOSURFACE)
void* kIOSurfaceWidth;
void* kIOSurfaceHeight;
void* kIOSurfaceBytesPerElement;
void* kIOSurfacePixelFormat;
void* kIOSurfacePlaneInfo;
void* kIOSurfacePlaneWidth;
void* kIOSurfacePlaneHeight;
void* kIOSurfacePlaneBytesPerElement;
LINK_ONLY(IOSurfaceCreate)
LINK_ONLY(IOSurfaceCreateMachPort)
LINK_ONLY(IOSurfaceLookupFromMachPort)
LINK_ONLY(IOSurfaceGetBaseAddress)
LINK_ONLY(IOSurfaceGetBytesPerRow)
LINK_ONLY(IOSurfaceGetBaseAddressOfPlane)
LINK_ONLY(IOSurfaceGetBytesPerRowOfPlane)
LINK_ONLY(IOSurfaceGetSeed)
LINK_ONLY(IOSurfaceLock)
LINK_ONLY(IOSurfaceUnlock)
#else
#error "select a stub with -DMETAL_IOSURFACE_STUB_*"
#endif
