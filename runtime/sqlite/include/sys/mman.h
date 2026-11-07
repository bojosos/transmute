/* Memory mapping, as the Darwin arm64 LP64 ABI declares it (the subset SQLite's unix VFS uses).
   Alchemy-authored declarations; the functions are served by Alchemy's libSystem provider. */
#ifndef ALCHEMY_SYS_MMAN_H
#define ALCHEMY_SYS_MMAN_H

#include <sys/types.h>

#define PROT_NONE 0x00
#define PROT_READ 0x01
#define PROT_WRITE 0x02
#define PROT_EXEC 0x04

#define MAP_SHARED 0x0001
#define MAP_PRIVATE 0x0002
#define MAP_FIXED 0x0010
#define MAP_ANON 0x1000
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void*)-1)

#define MS_ASYNC 0x0001
#define MS_INVALIDATE 0x0002
#define MS_SYNC 0x0010

void* mmap(void* address, size_t length, int protection, int flags, int descriptor, off_t offset);
int munmap(void* address, size_t length);
int mprotect(void* address, size_t length, int protection);
int msync(void* address, size_t length, int flags);

#endif
