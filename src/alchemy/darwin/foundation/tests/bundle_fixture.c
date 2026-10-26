/* Loading a bundle's executable under alchemy-fx (foundation_extra.cpp's
 * dlopen of NSBundle executables, bundles.cpp's Mach-O architectures,
 * corefoundation/cf_extra.cpp's CFBundle calls): the AlchemyPlugin.bundle
 * built next to this program (bundle_plugin.c) is preflighted, loaded, its
 * function and data symbols are found and used, and it is unloaded. Then
 * as a CFPlugIn (corefoundation/cf_plugin.cpp): its Info.plist factory is
 * found by type and creates an instance (reloading the executable), and a
 * factory registered at run time joins it. The bundle's guest path is the
 * first argument. Run by ctest alchemy_gui_foundation_bundle_fixture. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef const struct __CFBundle *CFBundleRef;
typedef struct __CFError *CFErrorRef;
CFBundleRef CFBundleCreate(CFAllocatorRef allocator, CFURLRef url);
CFStringRef CFBundleGetIdentifier(CFBundleRef bundle);
Boolean CFBundlePreflightExecutable(CFBundleRef bundle, CFErrorRef *error);
Boolean CFBundleIsExecutableLoaded(CFBundleRef bundle);
Boolean CFBundleLoadExecutable(CFBundleRef bundle);
void CFBundleUnloadExecutable(CFBundleRef bundle);
void *CFBundleGetFunctionPointerForName(CFBundleRef bundle, CFStringRef name);
void *CFBundleGetDataPointerForName(CFBundleRef bundle, CFStringRef name);
CFArrayRef CFBundleCopyExecutableArchitectures(CFBundleRef bundle);
typedef const struct __CFUUID *CFUUIDRef;
typedef struct { unsigned char byte[16]; } CFUUIDBytes;
typedef CFBundleRef CFPlugInRef;
typedef void *(*CFPlugInFactoryFunction)(CFAllocatorRef allocator, CFUUIDRef type);
CFUUIDRef CFUUIDCreateFromString(CFAllocatorRef allocator, CFStringRef text);
CFUUIDBytes CFUUIDGetUUIDBytes(CFUUIDRef uuid);
CFPlugInRef CFPlugInCreate(CFAllocatorRef allocator, CFURLRef url);
CFBundleRef CFPlugInGetBundle(CFPlugInRef plugIn);
CFPlugInRef CFBundleGetPlugIn(CFBundleRef bundle);
CFTypeID CFBundleGetTypeID(void);
CFTypeID CFPlugInGetTypeID(void);
CFArrayRef CFPlugInFindFactoriesForPlugInType(CFUUIDRef type);
void *CFPlugInInstanceCreate(CFAllocatorRef allocator, CFUUIDRef factory, CFUUIDRef type);
Boolean CFPlugInRegisterFactoryFunction(CFUUIDRef factory, CFPlugInFactoryFunction function);
Boolean CFPlugInRegisterPlugInType(CFUUIDRef factory, CFUUIDRef type);

struct plugin_instance { int magic; };
static struct plugin_instance local_instance = {99};
static void *local_factory(CFAllocatorRef allocator, CFUUIDRef type) { (void)allocator; (void)type; return &local_instance; }
static int same_uuid(CFUUIDRef a, CFUUIDRef b) {
    CFUUIDBytes x = CFUUIDGetUUIDBytes(a), y = CFUUIDGetUUIDBytes(b);
    for (int index = 0; index < 16; ++index) if (x.byte[index] != y.byte[index]) return 0;
    return 1;
}

static int failures;
static void check(int passed, const char *what) {
    if (!passed) {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

int main(int argc, char **argv) {
    printf("Alchemy bundle fixture: launched\n");
    if (argc < 2) {
        printf("FAIL no bundle path\n");
        return 1;
    }
    CFStringRef path = CFStringCreateWithCString(0, argv[1], kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(0, path, kCFURLPOSIXPathStyle, 1);
    CFBundleRef bundle = CFBundleCreate(0, url);
    check(bundle != 0, "CFBundleCreate");
    if (!bundle) return 1;
    char identifier[64] = {0};
    check(CFStringGetCString(CFBundleGetIdentifier(bundle), identifier, sizeof identifier, kCFStringEncodingUTF8) &&
              identifier[0] == 'o', "the bundle's identifier");
    CFArrayRef architectures = CFBundleCopyExecutableArchitectures(bundle);
    int found_arm64 = 0;
    for (CFIndex index = 0; architectures && index < CFArrayGetCount(architectures); ++index) {
        int type = 0;
        CFNumberGetValue((CFNumberRef)CFArrayGetValueAtIndex(architectures, index), kCFNumberSInt32Type, &type);
        if (type == 0x0100000c) found_arm64 = 1;
    }
    check(found_arm64, "an arm64 executable");
    CFErrorRef error = 0;
    check(CFBundlePreflightExecutable(bundle, &error) && !error, "CFBundlePreflightExecutable");
    check(!CFBundleIsExecutableLoaded(bundle), "not loaded before CFBundleLoadExecutable");
    check(CFBundleLoadExecutable(bundle) && CFBundleIsExecutableLoaded(bundle), "CFBundleLoadExecutable");
    int (*answer)(void) = (int (*)(void))CFBundleGetFunctionPointerForName(bundle, CFSTR("plugin_answer"));
    check(answer && answer() == 42, "a function from the bundle");
    int *data = (int *)CFBundleGetDataPointerForName(bundle, CFSTR("plugin_data"));
    check(data && *data == 7, "data from the bundle");
    if (data) *data = 8;
    int *again = (int *)CFBundleGetDataPointerForName(bundle, CFSTR("plugin_data"));
    check(again == data && answer && answer() == 43, "the loaded image's own data");
    CFBundleUnloadExecutable(bundle);
    check(!CFBundleIsExecutableLoaded(bundle), "CFBundleUnloadExecutable");

    /* The same bundle as a CFPlugIn. */
    CFPlugInRef plugin = CFPlugInCreate(0, url);
    check(plugin == bundle && CFPlugInGetBundle(plugin) == bundle && CFBundleGetPlugIn(bundle) == plugin, "CFPlugInCreate: the plug-in is its bundle");
    check(CFGetTypeID(bundle) == CFBundleGetTypeID() && CFPlugInGetTypeID() == CFBundleGetTypeID(), "CFBundleGetTypeID");
    CFUUIDRef type = CFUUIDCreateFromString(0, CFSTR("D5A67F41-5A5F-4F7A-9E0E-1A2B3C4D5E6F"));
    CFUUIDRef factory = CFUUIDCreateFromString(0, CFSTR("0b1c2d3e-4f50-4612-8a34-56789abcdef0"));
    check(type && factory && CFUUIDCreateFromString(0, CFSTR("not a uuid")) == 0, "CFUUIDCreateFromString");
    CFArrayRef factories = CFPlugInFindFactoriesForPlugInType(type);
    check(factories && CFArrayGetCount(factories) == 1 && same_uuid((CFUUIDRef)CFArrayGetValueAtIndex(factories, 0), factory),
          "the Info.plist factory for the type");
    struct plugin_instance *made = (struct plugin_instance *)CFPlugInInstanceCreate(0, factory, type);
    check(made && made->magic == 1234 && CFBundleIsExecutableLoaded(bundle), "CFPlugInInstanceCreate loads the plug-in and calls its factory");
    CFUUIDRef extra = CFUUIDCreateFromString(0, CFSTR("11111111-2222-3333-4444-555555555555"));
    check(CFPlugInRegisterFactoryFunction(extra, local_factory) && CFPlugInRegisterPlugInType(extra, type), "a factory registered at run time");
    factories = CFPlugInFindFactoriesForPlugInType(type);
    check(factories && CFArrayGetCount(factories) == 2, "both factories for the type");
    made = (struct plugin_instance *)CFPlugInInstanceCreate(0, extra, type);
    check(made && made->magic == 99, "an instance from the run-time factory");
    if (failures) {
        printf("Alchemy bundle fixture: %d failures\n", failures);
        return 1;
    }
    printf("Alchemy bundle fixture: complete\n");
    return 0;
}
