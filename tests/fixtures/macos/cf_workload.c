#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

static int check(Boolean condition) { return condition ? 0 : 1; }
int main(void) {
    int failures = 0;
    const CFStringRef literal = CFSTR("A\U0001f600\u00e9");
    failures += check(CFStringGetLength(literal) == 4);
    CFStringRef text = CFStringCreateWithCString(0, "A\xf0\x9f\x98\x80\xc3\xa9", kCFStringEncodingUTF8);
    failures += check(text && CFEqual(text, literal) && CFHash(text) == CFHash(literal));
    char encoded[32]; CFIndex used = 0;
    failures += check(CFStringGetBytes(text, CFRangeMake(0, 4), kCFStringEncodingUTF8, 0, 0,
        (unsigned char *)encoded, sizeof(encoded), &used) == 4 && used == 7);
    failures += check(CFStringGetCString(text, encoded, sizeof(encoded), kCFStringEncodingUTF8) &&
        strcmp(encoded, "A\xf0\x9f\x98\x80\xc3\xa9") == 0);
    const unsigned char malformed[] = {0xc0, 0x80};
    failures += check(!CFStringCreateWithBytes(0, malformed, 2, kCFStringEncodingUTF8, 0));
    long amount = 27, result = 0;
    CFNumberRef number = CFNumberCreate(0, kCFNumberLongType, &amount);
    failures += check(CFNumberGetValue(number, kCFNumberLongType, &result) && result == 27);
    const unsigned char raw[] = {0, 27, 255, 42};
    CFMutableDataRef data = CFDataCreateMutable(0, 0); CFDataAppendBytes(data, raw, 4);
    CFDataSetLength(data, 6); failures += check(CFDataGetLength(data) == 6 && CFDataGetBytePtr(data)[5] == 0);
    CFMutableArrayRef array = CFArrayCreateMutable(0, 0, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(array, text); CFArrayAppendValue(array, number); CFArrayAppendValue(array, data);
    CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(0, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(dictionary, CFSTR("items"), array);
    CFRelease(text); CFRelease(number); CFRelease(data); CFRelease(array);
    array = (CFMutableArrayRef)CFDictionaryGetValue(dictionary, CFSTR("items"));
    failures += check(CFArrayGetCount(array) == 3 && CFEqual(CFArrayGetValueAtIndex(array, 0), literal));
    CFDictionaryRef copy = CFDictionaryCreateCopy(0, dictionary);
    failures += check(CFEqual(copy, dictionary) && CFHash(copy) == CFHash(dictionary));
    CFRelease(copy); CFRelease(dictionary);
    CFURLRef url = CFURLCreateWithFileSystemPath(0, CFSTR("relative file.txt"), kCFURLPOSIXPathStyle, 0);
    CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
    failures += check(CFStringGetCString(path, encoded, 1, kCFStringEncodingUTF8) == 0);
    unsigned char filesystem[4096];
    failures += check(CFURLGetFileSystemRepresentation(url, 1, filesystem, sizeof(filesystem)) &&
        strstr((const char *)filesystem, "relative file.txt") != 0);
    CFRelease(path); CFRelease(url);
    failures += check(CFBooleanGetValue(kCFBooleanTrue) && !CFBooleanGetValue(kCFBooleanFalse));
    if (failures) return 100 + failures;
    puts("CoreFoundation objects: 27 4 6");
    return 0;
}
