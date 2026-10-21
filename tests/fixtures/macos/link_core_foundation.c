/* Link-only symbols for project fixtures. Alchemy supplies these implementations. */
long __CFConstantStringClassReference[4];
void *kCFAllocatorDefault, *kCFAllocatorSystemDefault, *kCFBooleanTrue, *kCFBooleanFalse;
void *kCFNumberNaN, *kCFNumberPositiveInfinity, *kCFNumberNegativeInfinity;
char kCFTypeArrayCallBacks[40], kCFTypeDictionaryKeyCallBacks[48], kCFTypeDictionaryValueCallBacks[40];
#define F(name) long name(void) { return 0; }
F(CFRetain) F(CFRelease) F(CFGetRetainCount) F(CFGetTypeID) F(CFGetAllocator) F(CFEqual) F(CFHash)
F(CFCopyDescription) F(CFCopyTypeIDDescription)
F(CFStringGetTypeID) F(CFDataGetTypeID) F(CFNumberGetTypeID) F(CFBooleanGetTypeID)
F(CFArrayGetTypeID) F(CFDictionaryGetTypeID) F(CFURLGetTypeID)
F(CFStringCreateWithCString) F(CFStringCreateWithBytes) F(CFStringCreateWithCharacters)
F(CFStringCreateCopy) F(CFStringCreateWithSubstring) F(CFStringGetLength) F(CFStringGetCharacterAtIndex)
F(CFStringGetCharacters) F(CFStringGetCharactersPtr) F(CFStringGetCStringPtr) F(CFStringGetCString)
F(CFStringGetBytes) F(CFStringGetMaximumSizeForEncoding) F(CFStringCompare)
F(CFDataCreate) F(CFDataCreateCopy) F(CFDataCreateMutable) F(CFDataCreateMutableCopy)
F(CFDataGetLength) F(CFDataGetBytePtr) F(CFDataGetMutableBytePtr) F(CFDataGetBytes)
F(CFDataAppendBytes) F(CFDataSetLength) F(CFDataIncreaseLength) F(CFDataReplaceBytes) F(CFDataDeleteBytes)
F(CFNumberCreate) F(CFNumberGetType) F(CFNumberGetByteSize) F(CFNumberIsFloatType)
F(CFNumberGetValue) F(CFNumberCompare) F(CFBooleanGetValue)
F(CFArrayCreate) F(CFArrayCreateCopy) F(CFArrayCreateMutable) F(CFArrayCreateMutableCopy)
F(CFArrayGetCount) F(CFArrayGetValueAtIndex) F(CFArrayGetValues) F(CFArrayContainsValue)
F(CFArrayAppendValue) F(CFArraySetValueAtIndex) F(CFArrayInsertValueAtIndex)
F(CFArrayRemoveValueAtIndex) F(CFArrayRemoveAllValues)
F(CFDictionaryCreate) F(CFDictionaryCreateCopy) F(CFDictionaryCreateMutable) F(CFDictionaryCreateMutableCopy)
F(CFDictionaryGetCount) F(CFDictionaryGetValue) F(CFDictionaryGetValueIfPresent)
F(CFDictionaryContainsKey) F(CFDictionaryContainsValue) F(CFDictionaryGetKeysAndValues)
F(CFDictionarySetValue) F(CFDictionaryAddValue) F(CFDictionaryReplaceValue)
F(CFDictionaryRemoveValue) F(CFDictionaryRemoveAllValues)
F(CFURLCreateWithFileSystemPath) F(CFURLCreateFromFileSystemRepresentation)
F(CFURLCopyFileSystemPath) F(CFURLGetFileSystemRepresentation) F(CFURLHasDirectoryPath)
F(CFURLCopyAbsoluteURL) F(CFURLGetString)
