/* CFTimeZone, CFCalendar and the CFGregorianDate calls (foundation/value_classes.cpp,
 * calendar_provider.cpp): CFAbsoluteTime arguments and results travel in d0/d1,
 * CFRange results in x0/x1, CFGregorianDate in x0/x1, CFGregorianUnits by
 * reference and through x8, and CFCalendar's component descriptions take
 * variadic arguments, none of which the in-process tests can pass. Run by ctest
 * alchemy_gui_foundation_cf_time_fixture. */
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

typedef double CFAbsoluteTime;
typedef double CFTimeInterval;
typedef const void *CFTimeZoneRef;
typedef const void *CFCalendarRef;
typedef struct { int year; signed char month, day, hour, minute; double second; } CFGregorianDate;
typedef struct { int years, months, days, hours, minutes; double seconds; } CFGregorianUnits;

CFTimeZoneRef CFTimeZoneCreateWithName(CFAllocatorRef allocator, CFStringRef name, Boolean tryAbbrev);
CFTimeZoneRef CFTimeZoneCreateWithTimeIntervalFromGMT(CFAllocatorRef allocator, CFTimeInterval interval);
CFStringRef CFTimeZoneGetName(CFTimeZoneRef zone);
CFTimeInterval CFTimeZoneGetSecondsFromGMT(CFTimeZoneRef zone, CFAbsoluteTime at);
CFStringRef CFTimeZoneCopyAbbreviation(CFTimeZoneRef zone, CFAbsoluteTime at);
Boolean CFTimeZoneIsDaylightSavingTime(CFTimeZoneRef zone, CFAbsoluteTime at);
CFTimeInterval CFTimeZoneGetDaylightSavingTimeOffset(CFTimeZoneRef zone, CFAbsoluteTime at);
CFAbsoluteTime CFTimeZoneGetNextDaylightSavingTimeTransition(CFTimeZoneRef zone, CFAbsoluteTime at);
CFArrayRef CFTimeZoneCopyKnownNames(void);
CFDictionaryRef CFTimeZoneCopyAbbreviationDictionary(void);

CFCalendarRef CFCalendarCreateWithIdentifier(CFAllocatorRef allocator, CFStringRef identifier);
CFStringRef CFCalendarGetIdentifier(CFCalendarRef calendar);
void CFCalendarSetTimeZone(CFCalendarRef calendar, CFTimeZoneRef zone);
void CFCalendarSetFirstWeekday(CFCalendarRef calendar, CFIndex weekday);
CFIndex CFCalendarGetFirstWeekday(CFCalendarRef calendar);
Boolean CFCalendarComposeAbsoluteTime(CFCalendarRef calendar, CFAbsoluteTime *at, const char *components, ...);
Boolean CFCalendarDecomposeAbsoluteTime(CFCalendarRef calendar, CFAbsoluteTime at, const char *components, ...);
Boolean CFCalendarAddComponents(CFCalendarRef calendar, CFAbsoluteTime *at, CFOptionFlags options, const char *components, ...);
Boolean CFCalendarGetComponentDifference(CFCalendarRef calendar, CFAbsoluteTime start, CFAbsoluteTime result, CFOptionFlags options,
                                         const char *components, ...);
CFRange CFCalendarGetRangeOfUnit(CFCalendarRef calendar, CFOptionFlags smaller, CFOptionFlags bigger, CFAbsoluteTime at);
CFIndex CFCalendarGetOrdinalityOfUnit(CFCalendarRef calendar, CFOptionFlags smaller, CFOptionFlags bigger, CFAbsoluteTime at);
Boolean CFCalendarGetTimeRangeOfUnit(CFCalendarRef calendar, CFOptionFlags unit, CFAbsoluteTime at, CFAbsoluteTime *start, CFTimeInterval *length);
CFRange CFCalendarGetMaximumRangeOfUnit(CFCalendarRef calendar, CFOptionFlags unit);

CFGregorianDate CFAbsoluteTimeGetGregorianDate(CFAbsoluteTime at, CFTimeZoneRef zone);
CFAbsoluteTime CFGregorianDateGetAbsoluteTime(CFGregorianDate date, CFTimeZoneRef zone);
Boolean CFGregorianDateIsValid(CFGregorianDate date, CFOptionFlags units);
int CFAbsoluteTimeGetDayOfWeek(CFAbsoluteTime at, CFTimeZoneRef zone);
int CFAbsoluteTimeGetDayOfYear(CFAbsoluteTime at, CFTimeZoneRef zone);
int CFAbsoluteTimeGetWeekOfYear(CFAbsoluteTime at, CFTimeZoneRef zone);
CFAbsoluteTime CFAbsoluteTimeAddGregorianUnits(CFAbsoluteTime at, CFTimeZoneRef zone, CFGregorianUnits units);
CFGregorianUnits CFAbsoluteTimeGetDifferenceAsGregorianUnits(CFAbsoluteTime at1, CFAbsoluteTime at2, CFTimeZoneRef zone, CFOptionFlags units);

enum { unit_year = 1 << 2, unit_month = 1 << 3, unit_day = 1 << 4 };
static const CFAbsoluteTime january = 631152000.0;   /* 2021-01-01 00:00:00 UTC */
static const CFAbsoluteTime july = 646790400.0;      /* 2021-07-01 00:00:00 UTC */

static int failures;
static void check(int passed, const char *what) {
    if (!passed) {
        printf("FAIL %s\n", what);
        ++failures;
    }
}
static int text_is(CFStringRef string, const char *expected) {
    char buffer[128];
    return string && CFStringGetCString(string, buffer, sizeof buffer, kCFStringEncodingUTF8) && strcmp(buffer, expected) == 0;
}

int main(void) {
    printf("Alchemy CF time fixture: launched\n");
    /* Time zones. */
    CFTimeZoneRef york = CFTimeZoneCreateWithName(0, CFSTR("America/New_York"), 0);
    check(york && text_is(CFTimeZoneGetName(york), "America/New_York"), "CFTimeZoneCreateWithName");
    check(york && CFTimeZoneGetSecondsFromGMT(york, january) == -18000.0, "winter offset");
    check(york && CFTimeZoneGetSecondsFromGMT(york, july) == -14400.0, "summer offset");
    check(york && !CFTimeZoneIsDaylightSavingTime(york, january) && CFTimeZoneIsDaylightSavingTime(york, july), "daylight saving");
    check(york && CFTimeZoneGetDaylightSavingTimeOffset(york, july) == 3600.0, "daylight saving offset");
    check(york && text_is(CFTimeZoneCopyAbbreviation(york, january), "EST"), "abbreviation");
    check(york && CFTimeZoneGetNextDaylightSavingTimeTransition(york, january) == 637398000.0, "next transition (2021-03-14 07:00 UTC)");
    CFTimeZoneRef pacific = CFTimeZoneCreateWithName(0, CFSTR("PST"), 1);
    check(pacific && text_is(CFTimeZoneGetName(pacific), "America/Los_Angeles"), "a name from the abbreviation dictionary");
    CFTimeZoneRef singapore = CFTimeZoneCreateWithName(0, CFSTR("SGT"), 1);
    check(singapore && text_is(CFTimeZoneGetName(singapore), "Asia/Singapore"), "SGT through the dictionary");
    check(CFTimeZoneCreateWithName(0, CFSTR("SGT"), 0) == 0, "no abbreviation lookup without tryAbbrev");
    CFTimeZoneRef fixed = CFTimeZoneCreateWithTimeIntervalFromGMT(0, 3600.0);
    check(fixed && CFTimeZoneGetSecondsFromGMT(fixed, july) == 3600.0, "a fixed offset");
    CFArrayRef names = CFTimeZoneCopyKnownNames();
    check(names && CFArrayGetCount(names) > 300, "known names");
    CFDictionaryRef abbreviations = CFTimeZoneCopyAbbreviationDictionary();
    check(abbreviations && CFDictionaryGetCount(abbreviations) == 48, "the abbreviation dictionary");

    /* Calendars. */
    CFTimeZoneRef gmt = CFTimeZoneCreateWithName(0, CFSTR("GMT"), 0);
    CFCalendarRef calendar = CFCalendarCreateWithIdentifier(0, CFSTR("gregorian"));
    check(calendar && text_is(CFCalendarGetIdentifier(calendar), "gregorian"), "CFCalendarCreateWithIdentifier");
    CFCalendarSetTimeZone(calendar, gmt);
    CFCalendarSetFirstWeekday(calendar, 2);
    check(CFCalendarGetFirstWeekday(calendar) == 2, "first weekday");
    CFAbsoluteTime at = 0;
    check(CFCalendarComposeAbsoluteTime(calendar, &at, "yMdHms", 2021, 7, 1, 0, 0, 0) && at == july, "compose");
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    check(CFCalendarDecomposeAbsoluteTime(calendar, july + 3661.0, "yMdHms", &year, &month, &day, &hour, &minute, &second) &&
              year == 2021 && month == 7 && day == 1 && hour == 1 && minute == 1 && second == 1, "decompose");
    CFAbsoluteTime end_of_january = january + 30 * 86400.0;
    check(CFCalendarAddComponents(calendar, &end_of_january, 0, "M", 1) && end_of_january == january + 58 * 86400.0,
          "adding a month keeps the day in range (January 31 -> February 28)");
    month = day = -1;
    check(CFCalendarGetComponentDifference(calendar, january, july, 0, "Md", &month, &day) && month == 6 && day == 0, "difference");
    CFRange days = CFCalendarGetRangeOfUnit(calendar, unit_day, unit_month, january + 40 * 86400.0);
    check(days.location == 1 && days.length == 28, "the days of February 2021");
    check(CFCalendarGetOrdinalityOfUnit(calendar, unit_day, unit_year, july) == 182, "July 1 is day 182");
    CFAbsoluteTime start = 0;
    CFTimeInterval length = 0;
    check(CFCalendarGetTimeRangeOfUnit(calendar, unit_day, july + 5000.0, &start, &length) && start == july && length == 86400.0, "a day's time range");
    CFRange months = CFCalendarGetMaximumRangeOfUnit(calendar, unit_month);
    check(months.location == 1 && months.length == 12, "maximum range of months");

    /* CFGregorianDate. */
    CFGregorianDate date = CFAbsoluteTimeGetGregorianDate(july + 3661.5, 0);
    check(date.year == 2021 && date.month == 7 && date.day == 1 && date.hour == 1 && date.minute == 1 && date.second == 1.5, "CFAbsoluteTimeGetGregorianDate");
    check(CFGregorianDateGetAbsoluteTime(date, 0) == july + 3661.5, "CFGregorianDateGetAbsoluteTime");
    CFGregorianDate invalid = {2021, 2, 29, 0, 0, 0};
    check(CFGregorianDateIsValid(date, 63) && !CFGregorianDateIsValid(invalid, 4), "CFGregorianDateIsValid");
    check(CFAbsoluteTimeGetDayOfWeek(july, 0) == 4, "July 1 2021 is a Thursday (4)");
    check(CFAbsoluteTimeGetDayOfYear(july, 0) == 182, "day of year");
    check(CFAbsoluteTimeGetWeekOfYear(july, 0) == 26, "ISO week 26");
    CFGregorianUnits add = {0, 1, 0, 0, 0, 30.0};
    check(CFAbsoluteTimeAddGregorianUnits(january, 0, add) == january + 31 * 86400.0 + 30.0, "CFAbsoluteTimeAddGregorianUnits");
    CFGregorianUnits apart = CFAbsoluteTimeGetDifferenceAsGregorianUnits(july + 90.0, january, 0, 1 | 2 | 4 | 32);
    check(apart.years == 0 && apart.months == 6 && apart.days == 0 && apart.seconds == 90.0, "CFAbsoluteTimeGetDifferenceAsGregorianUnits");
    if (failures) {
        printf("Alchemy CF time fixture: %d failures\n", failures);
        return 1;
    }
    printf("Alchemy CF time fixture: complete\n");
    return 0;
}
