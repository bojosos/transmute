#ifndef ALCHEMY_FCNTL_H
#define ALCHEMY_FCNTL_H
#include "alchemy_c.h"
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_NONBLOCK 4
#define O_APPEND 8
#define O_CREAT 0x200
#define O_TRUNC 0x400
#define O_EXCL 0x800
#define O_NOFOLLOW 0x100
#define O_DIRECTORY 0x100000
#define O_CLOEXEC 0x1000000
#define AT_FDCWD (-2)
#define AT_SYMLINK_NOFOLLOW 0x20
#define AT_REMOVEDIR 0x80
#ifdef __cplusplus
extern "C" {
#endif
int open(const char*, int, ...);
int openat(int, const char*, int, ...);
int fcntl(int, int, ...);
int unlinkat(int, const char*, int);
#ifdef __cplusplus
}
#endif
#endif
