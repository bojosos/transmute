#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern const char *dlerror(void);

int main(void) {
    void *scope = (void *)-2L;
    CFStringRef (*create)(CFAllocatorRef, const char *, CFStringEncoding) =
        (CFStringRef (*)(CFAllocatorRef, const char *, CFStringEncoding))dlsym(scope, "CFStringCreateWithCString");
    void (*release)(CFTypeRef) = (void (*)(CFTypeRef))dlsym(scope, "CFRelease");
    void *(*get_class)(const char *) = (void *(*)(const char *))dlsym(scope, "objc_getClass");
    void *(*from_string)(CFStringRef) = (void *(*)(CFStringRef))dlsym(scope, "NSClassFromString");
    if (!create || !release || !get_class || !from_string) return 1;
    CFStringRef name = create(0, "NSString", kCFStringEncodingUTF8);
    if (!name || from_string(name) != get_class("NSString") ||
        dlsym(scope, "OBJC_CLASS_$_NSString") != get_class("NSString")) return 2;
    release(name);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", 2);
    if (!cf || !dlsym(cf, "CFStringGetLength") || dlsym(cf, "objc_getClass") || !dlerror() || dlclose(cf)) return 3;
    puts("Alchemy object provider lookup: 27");
    return 0;
}
