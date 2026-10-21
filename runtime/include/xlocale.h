#ifndef ALCHEMY_XLOCALE_H
#define ALCHEMY_XLOCALE_H
#include "locale.h"
#include "ctype.h"
#include "wctype.h"
#define MB_CUR_MAX_L(locale) ((size_t)__mb_cur_max_l(locale))
#define MB_CUR_MAX ((size_t)__mb_cur_max())
#ifdef __cplusplus
extern "C" {
#endif
int __mb_cur_max_l(locale_t);
int __mb_cur_max(void);
#define ALCHEMY_CTYPE_LOCALE(name) int name##_l(int, locale_t);
ALCHEMY_CTYPE_LOCALE(isalnum) ALCHEMY_CTYPE_LOCALE(isalpha) ALCHEMY_CTYPE_LOCALE(isblank)
ALCHEMY_CTYPE_LOCALE(iscntrl) ALCHEMY_CTYPE_LOCALE(isdigit) ALCHEMY_CTYPE_LOCALE(isgraph)
ALCHEMY_CTYPE_LOCALE(islower) ALCHEMY_CTYPE_LOCALE(isprint) ALCHEMY_CTYPE_LOCALE(ispunct)
ALCHEMY_CTYPE_LOCALE(isspace) ALCHEMY_CTYPE_LOCALE(isupper) ALCHEMY_CTYPE_LOCALE(isxdigit)
ALCHEMY_CTYPE_LOCALE(tolower) ALCHEMY_CTYPE_LOCALE(toupper)
#undef ALCHEMY_CTYPE_LOCALE
#define ALCHEMY_WCTYPE_LOCALE(name) int name##_l(wint_t, locale_t);
ALCHEMY_WCTYPE_LOCALE(iswalnum) ALCHEMY_WCTYPE_LOCALE(iswalpha) ALCHEMY_WCTYPE_LOCALE(iswblank)
ALCHEMY_WCTYPE_LOCALE(iswcntrl) ALCHEMY_WCTYPE_LOCALE(iswdigit) ALCHEMY_WCTYPE_LOCALE(iswgraph)
ALCHEMY_WCTYPE_LOCALE(iswlower) ALCHEMY_WCTYPE_LOCALE(iswprint) ALCHEMY_WCTYPE_LOCALE(iswpunct)
ALCHEMY_WCTYPE_LOCALE(iswspace) ALCHEMY_WCTYPE_LOCALE(iswupper) ALCHEMY_WCTYPE_LOCALE(iswxdigit)
#undef ALCHEMY_WCTYPE_LOCALE
wint_t towlower_l(wint_t, locale_t); wint_t towupper_l(wint_t, locale_t);
int iswctype_l(wint_t, wctype_t, locale_t);
int strcoll_l(const char*, const char*, locale_t);
size_t strxfrm_l(char*, const char*, size_t, locale_t);
int wcscoll_l(const wchar_t*, const wchar_t*, locale_t);
size_t wcsxfrm_l(wchar_t*, const wchar_t*, size_t, locale_t);
float strtof_l(const char*, char**, locale_t);
double strtod_l(const char*, char**, locale_t);
long double strtold_l(const char*, char**, locale_t);
long long strtoll_l(const char*, char**, int, locale_t);
unsigned long long strtoull_l(const char*, char**, int, locale_t);
wint_t btowc_l(int, locale_t); int wctob_l(wint_t, locale_t);
size_t wcsnrtombs_l(char*, const wchar_t**, size_t, size_t, mbstate_t*, locale_t);
size_t wcrtomb_l(char*, wchar_t, mbstate_t*, locale_t);
size_t mbsnrtowcs_l(wchar_t*, const char**, size_t, size_t, mbstate_t*, locale_t);
size_t mbrtowc_l(wchar_t*, const char*, size_t, mbstate_t*, locale_t);
int mbtowc_l(wchar_t*, const char*, size_t, locale_t);
size_t mbrlen_l(const char*, size_t, mbstate_t*, locale_t);
struct lconv* localeconv_l(locale_t);
size_t mbsrtowcs_l(wchar_t*, const char**, size_t, mbstate_t*, locale_t);
int snprintf_l(char*, size_t, locale_t, const char*, ...);
int asprintf_l(char**, locale_t, const char*, ...);
int sscanf_l(const char*, locale_t, const char*, ...);
size_t strftime_l(char*, size_t, const char*, const struct tm*, locale_t);
#ifdef __cplusplus
}
#endif
#endif
