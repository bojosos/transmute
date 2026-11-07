/* The project's guest <errno.h> plus the Darwin error numbers SQLite's unix VFS tests.
   Alchemy-authored. */
#ifndef ALCHEMY_SQLITE_ERRNO_H
#define ALCHEMY_SQLITE_ERRNO_H
#include_next <errno.h>

#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define EACCES 13
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define ENOSPC 28
#define EROFS 30
#define ENOTSUP 45
#define ELOOP 62
#define ENAMETOOLONG 63
#define ENOLCK 77

#endif
