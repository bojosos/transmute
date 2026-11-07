/* File-system statistics, as the Darwin arm64 LP64 ABI declares them (64-bit inode struct statfs,
   2168 bytes). Alchemy-authored declarations; the functions are served by Alchemy's libSystem
   provider. */
#ifndef ALCHEMY_SYS_MOUNT_H
#define ALCHEMY_SYS_MOUNT_H

#include <stdint.h>
#include <sys/types.h>

typedef struct { int32_t val[2]; } fsid_t;

#define MFSTYPENAMELEN 16
#define MAXPATHLEN 1024
#define MNT_RDONLY 0x00000001
#define MNT_LOCAL 0x00001000

struct statfs {
    uint32_t f_bsize;
    int32_t f_iosize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    fsid_t f_fsid;
    uid_t f_owner;
    uint32_t f_type;
    uint32_t f_flags;
    uint32_t f_fssubtype;
    char f_fstypename[MFSTYPENAMELEN];
    char f_mntonname[MAXPATHLEN];
    char f_mntfromname[MAXPATHLEN];
    uint32_t f_flags_ext;
    uint32_t f_reserved[7];
};

int statfs(const char* path, struct statfs* buffer);
int fstatfs(int descriptor, struct statfs* buffer);

#endif
