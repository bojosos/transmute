#ifndef ALCHEMY_WCHAR_H
#define ALCHEMY_WCHAR_H
#include "alchemy_c.h"
#include "sys/_types/_mbstate_t.h"
// Darwin: __darwin_wint_t is int. libc++ mangles char_traits<wchar_t>::int_type
// into exported names (basic_istream<wchar_t>::ignore(long, int)), so it must
// match the macOS ABI that guest binaries import.
typedef int wint_t;
#define WEOF ((wint_t)-1)
#define WCHAR_MIN (-2147483647 - 1)
#define WCHAR_MAX 2147483647
#ifdef __cplusplus
extern "C" {
#endif
int mbtowc(wchar_t*, const char*, size_t);
int wctomb(char*, wchar_t);
size_t mbstowcs(wchar_t*, const char*, size_t);
size_t wcstombs(char*, const wchar_t*, size_t);
size_t mbrlen(const char*, size_t, mbstate_t*);
size_t mbrtowc(wchar_t*, const char*, size_t, mbstate_t*);
size_t wcrtomb(char*, wchar_t, mbstate_t*);
size_t mbsrtowcs(wchar_t*, const char**, size_t, mbstate_t*);
size_t wcsrtombs(char*, const wchar_t**, size_t, mbstate_t*);
int mbsinit(const mbstate_t*);
wchar_t* wcscpy(wchar_t*, const wchar_t*);
wchar_t* wcsncpy(wchar_t*, const wchar_t*, size_t);
wchar_t* wcscat(wchar_t*, const wchar_t*);
wchar_t* wcsncat(wchar_t*, const wchar_t*, size_t);
int wcscmp(const wchar_t*, const wchar_t*);
int wcsncmp(const wchar_t*, const wchar_t*, size_t);
int wcscoll(const wchar_t*, const wchar_t*);
size_t wcsxfrm(wchar_t*, const wchar_t*, size_t);
size_t wcslen(const wchar_t*);
wchar_t* wcschr(const wchar_t*, wchar_t);
wchar_t* wcsrchr(const wchar_t*, wchar_t);
wchar_t* wcsstr(const wchar_t*, const wchar_t*);
wchar_t* wcspbrk(const wchar_t*, const wchar_t*);
size_t wcsspn(const wchar_t*, const wchar_t*);
size_t wcscspn(const wchar_t*, const wchar_t*);
wchar_t* wcstok(wchar_t*, const wchar_t*, wchar_t**);
wchar_t* wmemchr(const wchar_t*, wchar_t, size_t);
int wmemcmp(const wchar_t*, const wchar_t*, size_t);
wchar_t* wmemcpy(wchar_t*, const wchar_t*, size_t);
wchar_t* wmemmove(wchar_t*, const wchar_t*, size_t);
wchar_t* wmemset(wchar_t*, wchar_t, size_t);
float wcstof(const wchar_t*, wchar_t**);
double wcstod(const wchar_t*, wchar_t**);
long double wcstold(const wchar_t*, wchar_t**);
long wcstol(const wchar_t*, wchar_t**, int);
unsigned long wcstoul(const wchar_t*, wchar_t**, int);
long long wcstoll(const wchar_t*, wchar_t**, int);
unsigned long long wcstoull(const wchar_t*, wchar_t**, int);
int fwide(FILE*, int);
wint_t fgetwc(FILE*);
wint_t getwc(FILE*);
wint_t getwchar(void);
wint_t fputwc(wchar_t, FILE*);
wint_t putwc(wchar_t, FILE*);
wint_t putwchar(wchar_t);
wchar_t* fgetws(wchar_t*, int, FILE*);
int fputws(const wchar_t*, FILE*);
wint_t ungetwc(wint_t, FILE*);
int fwprintf(FILE*, const wchar_t*, ...);
int wprintf(const wchar_t*, ...);
int swprintf(wchar_t*, size_t, const wchar_t*, ...);
int vfwprintf(FILE*, const wchar_t*, va_list);
int vwprintf(const wchar_t*, va_list);
int vswprintf(wchar_t*, size_t, const wchar_t*, va_list);
int fwscanf(FILE*, const wchar_t*, ...);
int wscanf(const wchar_t*, ...);
int swscanf(const wchar_t*, const wchar_t*, ...);
int vfwscanf(FILE*, const wchar_t*, va_list);
int vwscanf(const wchar_t*, va_list);
int vswscanf(const wchar_t*, const wchar_t*, va_list);
size_t wcsftime(wchar_t*, size_t, const wchar_t*, const struct tm*);
#ifdef __cplusplus
}
#endif
#endif
