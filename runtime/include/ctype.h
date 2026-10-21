#ifndef ALCHEMY_CTYPE_H
#define ALCHEMY_CTYPE_H
typedef unsigned int __uint32_t;
#include "runetype.h"
#define _CTYPE_A 0x00000100UL
#define _CTYPE_C 0x00000200UL
#define _CTYPE_D 0x00000400UL
#define _CTYPE_G 0x00000800UL
#define _CTYPE_L 0x00001000UL
#define _CTYPE_P 0x00002000UL
#define _CTYPE_S 0x00004000UL
#define _CTYPE_U 0x00008000UL
#define _CTYPE_X 0x00010000UL
#define _CTYPE_B 0x00020000UL
#define _CTYPE_R 0x00040000UL
#ifdef __cplusplus
extern "C" {
#endif
int isalnum(int); int isalpha(int); int isblank(int); int iscntrl(int);
int isdigit(int); int isgraph(int); int islower(int); int isprint(int);
int ispunct(int); int isspace(int); int isupper(int); int isxdigit(int);
int tolower(int); int toupper(int);
int isascii(int); int toascii(int);
#ifdef __cplusplus
}
#endif
#endif
