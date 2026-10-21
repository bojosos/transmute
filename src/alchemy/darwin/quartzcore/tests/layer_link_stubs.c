/* Link-only identities for the Qt support fixture (owner: qt-vfb). One
 * source, one dylib per identity (selected by -DQT_STUB_*). The bodies are
 * never mapped: Alchemy's providers replace each image by its install name.
 * Project-authored; no Apple declarations are used. */
#define LINK_ONLY(name) long name(void) { return -777; }
#define LINK_DATA(name) void *name;

#if defined(QT_STUB_IOSURFACE)
LINK_ONLY(IOSurfaceCreate)
LINK_ONLY(IOSurfaceAlignProperty)
LINK_ONLY(IOSurfaceLock)
LINK_ONLY(IOSurfaceUnlock)
LINK_ONLY(IOSurfaceGetBaseAddress)
LINK_ONLY(IOSurfaceGetBytesPerRow)
LINK_ONLY(IOSurfaceGetAllocSize)
LINK_ONLY(IOSurfaceGetWidth)
LINK_ONLY(IOSurfaceGetHeight)
LINK_ONLY(IOSurfaceIsInUse)
LINK_ONLY(IOSurfaceSetValue)
LINK_DATA(kIOSurfaceWidth)
LINK_DATA(kIOSurfaceHeight)
LINK_DATA(kIOSurfaceBytesPerElement)
LINK_DATA(kIOSurfaceBytesPerRow)
LINK_DATA(kIOSurfaceAllocSize)
LINK_DATA(kIOSurfacePixelFormat)
#elif defined(QT_STUB_COREVIDEO)
LINK_ONLY(CVDisplayLinkCreateWithCGDisplay)
LINK_ONLY(CVDisplayLinkSetOutputCallback)
LINK_ONLY(CVDisplayLinkStart)
LINK_ONLY(CVDisplayLinkStop)
LINK_ONLY(CVDisplayLinkIsRunning)
LINK_ONLY(CVDisplayLinkGetNominalOutputVideoRefreshPeriod)
LINK_ONLY(CVDisplayLinkRelease)
#elif defined(QT_STUB_DISPATCH)
LINK_ONLY(dispatch_source_create)
LINK_ONLY(dispatch_set_context)
LINK_ONLY(dispatch_source_set_event_handler_f)
LINK_ONLY(dispatch_resume)
LINK_ONLY(dispatch_source_merge_data)
LINK_ONLY(dispatch_source_get_data)
LINK_DATA(_dispatch_source_type_data_add)
LINK_DATA(_dispatch_main_q)
#else
#error "select a stub with -DQT_STUB_*"
#endif
