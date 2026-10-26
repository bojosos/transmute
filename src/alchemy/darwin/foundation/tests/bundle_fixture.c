/* Loading a bundle's executable under alchemy-fx (foundation_extra.cpp's
 * dlopen of NSBundle executables, bundles.cpp's Mach-O architectures,
 * corefoundation/cf_extra.cpp's CFBundle calls): the AlchemyPlugin.bundle
 * built next to this program (bundle_plugin.c) is preflighted, loaded, its
 * function and data symbols are found and used, and it is unloaded. The
 * bundle's guest path is the first argument. Run by ctest
 * alchemy_gui_foundation_bundle_fixture. */
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
    if (failures) {
        printf("Alchemy bundle fixture: %d failures\n", failures);
        return 1;
    }
    printf("Alchemy bundle fixture: complete\n");
    return 0;
}
