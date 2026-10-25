/* Link-only symbols for the cf_strings fixture. Alchemy supplies these implementations. */
long __CFConstantStringClassReference[4];
#define F(name) long name(void) { return 0; }
F(CFRelease) F(CFStringGetLength) F(CFStringCreateWithCharacters) F(CFStringCreateMutable) F(CFStringAppendFormat)
F(CFStringFind) F(CFStringGetRangeOfComposedCharactersAtIndex) F(CFStringGetDoubleValue)
