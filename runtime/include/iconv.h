/* Character-set conversion: the GNU libiconv interface that Darwin ships as /usr/lib/libiconv.2.dylib,
   served by Alchemy's libiconv provider. Declarations written for Alchemy's guest runtimes. */
#ifndef ALCHEMY_ICONV_H
#define ALCHEMY_ICONV_H
#include "alchemy_c.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void* iconv_t;
iconv_t iconv_open(const char* tocode, const char* fromcode);
size_t iconv(iconv_t cd, char** __restrict inbuf, size_t* __restrict inbytesleft,
             char** __restrict outbuf, size_t* __restrict outbytesleft);
int iconv_close(iconv_t cd);
#ifdef __cplusplus
}
#endif
#endif
