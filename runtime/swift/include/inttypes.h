#ifndef ALCHEMY_SWIFT_INTTYPES_H
#define ALCHEMY_SWIFT_INTTYPES_H
/* Remaining C99 format macros for Darwin LP64. Independently authored. */
#include_next <inttypes.h>
#define PRIi8 "hhi"
#define PRIo8 "hho"
#define PRIX8 "hhX"
#define PRIi16 "hi"
#define PRIo16 "ho"
#define PRIX16 "hX"
#define PRIi32 "i"
#define PRIo32 "o"
#define PRIX32 "X"
#define PRIi64 "lli"
#define PRIo64 "llo"
#define PRIiPTR "li"
#define PRIoPTR "lo"
#define PRIXPTR "lX"
#define PRIdMAX "jd"
#define PRIiMAX "ji"
#define PRIuMAX "ju"
#define PRIxMAX "jx"
#define PRIXMAX "jX"
#define SCNd32 "d"
#define SCNu32 "u"
#define SCNd64 "lld"
#define SCNu64 "llu"
#define SCNx64 "llx"
#endif
