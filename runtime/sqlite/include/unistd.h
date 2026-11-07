/* The project's guest <unistd.h> plus the POSIX file calls SQLite's unix VFS uses, as the Darwin
   arm64 LP64 ABI declares them. Alchemy-authored declarations. */
#ifndef ALCHEMY_SQLITE_UNISTD_H
#define ALCHEMY_SQLITE_UNISTD_H
#include_next <unistd.h>

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define _SC_PAGESIZE 29
#define _SC_PAGE_SIZE _SC_PAGESIZE

int access(const char* path, int mode);
ssize_t pread(int descriptor, void* buffer, size_t count, off_t offset);
ssize_t pwrite(int descriptor, const void* buffer, size_t count, off_t offset);
int fchown(int descriptor, uid_t owner, gid_t group);
int fsync(int descriptor);
int getpagesize(void);

#endif
