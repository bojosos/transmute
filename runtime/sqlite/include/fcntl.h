/* The project's guest <fcntl.h> plus POSIX advisory record locks and the Darwin fcntl commands
   SQLite's unix VFS uses, as the Darwin arm64 LP64 ABI declares them. Alchemy-authored. */
#ifndef ALCHEMY_SQLITE_FCNTL_H
#define ALCHEMY_SQLITE_FCNTL_H
#include_next <fcntl.h>
#include <sys/types.h>

#define O_NOCTTY 0x20000
#define FD_CLOEXEC 1
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 7
#define F_SETLK 8
#define F_SETLKW 9
#define F_FULLFSYNC 51
#define F_BARRIERFSYNC 85
#define F_RDLCK 1
#define F_UNLCK 2
#define F_WRLCK 3

struct flock {
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
    short l_type;
    short l_whence;
};

#endif
