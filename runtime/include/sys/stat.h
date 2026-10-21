#ifndef ALCHEMY_STAT_H
#define ALCHEMY_STAT_H
#include "types.h"
struct stat {
    dev_t st_dev;
    mode_t st_mode;
    nlink_t st_nlink;
    ino_t st_ino;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    struct timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    off_t st_size;
    blkcnt_t st_blocks;
    blksize_t st_blksize;
    unsigned int st_flags, st_gen;
    int st_lspare;
    long long st_qspare[2];
};
#define st_atime st_atimespec.tv_sec
#define st_mtime st_mtimespec.tv_sec
#define st_ctime st_ctimespec.tv_sec
#define S_IFMT 0170000
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)
#ifdef __cplusplus
extern "C" {
#endif
int stat(const char*, struct stat*);
int lstat(const char*, struct stat*);
int fstat(int, struct stat*);
int chmod(const char*, mode_t);
int fchmod(int, mode_t);
int fchmodat(int, const char*, mode_t, int);
int mkdir(const char*, mode_t);
mode_t umask(mode_t);
#ifdef __cplusplus
}
static_assert(sizeof(struct stat) == 144, "Darwin stat layout");
#endif
#endif
