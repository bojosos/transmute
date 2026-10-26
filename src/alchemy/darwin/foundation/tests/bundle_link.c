/* Link-only symbols for the bundle fixture. Alchemy supplies these implementations. */
long __CFConstantStringClassReference[4];
#define F(name) long name(void) { return 0; }
F(CFStringCreateWithCString) F(CFStringGetCString) F(CFURLCreateWithFileSystemPath) F(CFArrayGetCount) F(CFArrayGetValueAtIndex)
F(CFNumberGetValue) F(CFBundleCreate) F(CFBundleGetIdentifier) F(CFBundlePreflightExecutable) F(CFBundleIsExecutableLoaded)
F(CFBundleLoadExecutable) F(CFBundleUnloadExecutable) F(CFBundleGetFunctionPointerForName) F(CFBundleGetDataPointerForName)
F(CFBundleCopyExecutableArchitectures)
