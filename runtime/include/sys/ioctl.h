/* Independently authored Darwin <sys/ioctl.h> declarations. */
#ifndef ALCHEMY_SYS_IOCTL_H
#define ALCHEMY_SYS_IOCTL_H
#include "alchemy_c.h"
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int, unsigned long, ...);
#ifdef __cplusplus
}
#endif
#endif
