/* The project's guest <time.h> plus the XSI time-zone globals ICU's platform layer reads, as the
   Darwin arm64 LP64 ABI declares them. Alchemy-authored declarations. */
#ifndef ALCHEMY_ICU_TIME_H
#define ALCHEMY_ICU_TIME_H
#include_next <time.h>

#ifdef __cplusplus
extern "C" {
#endif
void tzset(void);
extern long timezone;
extern char* tzname[2];
#ifdef __cplusplus
}
#endif

#endif
