#ifndef ALCHEMY_GETSECT_H
#define ALCHEMY_GETSECT_H
/* Published Mach-O section lookup declarations. Independently authored. */
#include <stdint.h>
#include <mach-o/loader.h>
#ifdef __cplusplus
extern "C" {
#endif
uint8_t* getsectiondata(const struct mach_header_64* mhp, const char* segname, const char* sectname, unsigned long* size);
uint8_t* getsegmentdata(const struct mach_header_64* mhp, const char* segname, unsigned long* size);
const struct section_64* getsectbynamefromheader_64(const struct mach_header_64* mhp, const char* segname, const char* sectname);
#ifdef __cplusplus
}
#endif
#endif
