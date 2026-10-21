#ifndef ALCHEMY_STATVFS_H
#define ALCHEMY_STATVFS_H
#include "types.h"
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
struct statvfs {
    unsigned long f_bsize, f_frsize;
    fsblkcnt_t f_blocks, f_bfree, f_bavail;
    fsfilcnt_t f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};
#ifdef __cplusplus
extern "C" {
#endif
int statvfs(const char*, struct statvfs*);
#ifdef __cplusplus
}
#endif
#endif
