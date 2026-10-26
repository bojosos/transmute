/* Link-only symbols for the cf_time fixture. Alchemy supplies these implementations. */
long __CFConstantStringClassReference[4];
#define F(name) long name(void) { return 0; }
F(CFRelease) F(CFStringGetCString) F(CFArrayGetCount) F(CFDictionaryGetCount)
F(CFTimeZoneCreateWithName) F(CFTimeZoneCreateWithTimeIntervalFromGMT) F(CFTimeZoneGetName) F(CFTimeZoneGetSecondsFromGMT)
F(CFTimeZoneCopyAbbreviation) F(CFTimeZoneIsDaylightSavingTime) F(CFTimeZoneGetDaylightSavingTimeOffset)
F(CFTimeZoneGetNextDaylightSavingTimeTransition) F(CFTimeZoneCopyKnownNames) F(CFTimeZoneCopyAbbreviationDictionary)
F(CFCalendarCreateWithIdentifier) F(CFCalendarGetIdentifier) F(CFCalendarSetTimeZone) F(CFCalendarSetFirstWeekday)
F(CFCalendarGetFirstWeekday) F(CFCalendarComposeAbsoluteTime) F(CFCalendarDecomposeAbsoluteTime) F(CFCalendarAddComponents)
F(CFCalendarGetComponentDifference) F(CFCalendarGetRangeOfUnit) F(CFCalendarGetOrdinalityOfUnit) F(CFCalendarGetTimeRangeOfUnit)
F(CFCalendarGetMaximumRangeOfUnit)
F(CFAbsoluteTimeGetGregorianDate) F(CFGregorianDateGetAbsoluteTime) F(CFGregorianDateIsValid) F(CFAbsoluteTimeGetDayOfWeek)
F(CFAbsoluteTimeGetDayOfYear) F(CFAbsoluteTimeGetWeekOfYear) F(CFAbsoluteTimeAddGregorianUnits)
F(CFAbsoluteTimeGetDifferenceAsGregorianUnits)
