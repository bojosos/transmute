/* CoreFoundation string calls whose results come back in x1 or d0 (a CFRange's
 * length, a double), which the in-process tests cannot see, and the variadic
 * CFStringAppendFormat. Run by ctest alchemy_gui_corefoundation_strings_fixture. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef CFOptionFlags CFStringCompareFlags;
CFRange CFStringFind(CFStringRef string, CFStringRef find, CFStringCompareFlags options);
CFRange CFStringGetRangeOfComposedCharactersAtIndex(CFStringRef string, CFIndex index);
double CFStringGetDoubleValue(CFStringRef string);
CFMutableStringRef CFStringCreateMutable(CFAllocatorRef allocator, CFIndex maximum);
void CFStringAppendFormat(CFMutableStringRef string, CFDictionaryRef options, CFStringRef format, ...);

static int failures;
static void check(int passed, const char *what) {
    if (!passed) {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

int main(void) {
    printf("Alchemy CF strings fixture: launched\n");
    CFRange range = CFStringFind(CFSTR("Hello World"), CFSTR("World"), 0);
    check(range.location == 6 && range.length == 5, "CFStringFind range");
    range = CFStringFind(CFSTR("Hello"), CFSTR("xyz"), 0);
    check(range.location == -1 && range.length == 0, "CFStringFind without a match");
    const UniChar composed[] = {'x', 'e', 0x0301, 'y'};
    CFStringRef text = CFStringCreateWithCharacters(0, composed, 4);
    range = CFStringGetRangeOfComposedCharactersAtIndex(text, 2);
    check(range.location == 1 && range.length == 2, "a composed character sequence");
    CFRelease(text);
    check(CFStringGetDoubleValue(CFSTR("  -2.5e3xyz")) == -2500.0, "CFStringGetDoubleValue");
    check(CFStringGetDoubleValue(CFSTR("abc")) == 0.0, "CFStringGetDoubleValue without a number");
    check(CFStringGetDoubleValue(CFSTR("1e999")) > 1e308, "CFStringGetDoubleValue overflow");
    CFMutableStringRef formatted = CFStringCreateMutable(0, 0);
    CFStringAppendFormat(formatted, 0, CFSTR("%d-%d"), 7, 42);
    check(CFStringGetLength(formatted) == 4, "CFStringAppendFormat with variadic arguments");
    CFRelease(formatted);
    if (failures) {
        printf("Alchemy CF strings fixture: %d failures\n", failures);
        return 1;
    }
    printf("Alchemy CF strings fixture: complete\n");
    return 0;
}
