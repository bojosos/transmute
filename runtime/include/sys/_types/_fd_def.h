/* Darwin's fd_set (FD_SETSIZE 1024, 32-bit words), which <sys/types.h> provides on Darwin. */
#ifndef ALCHEMY_FD_DEF_H
#define ALCHEMY_FD_DEF_H
#define FD_SETSIZE 1024
#define __DARWIN_NFDBITS 32
typedef struct fd_set { int fds_bits[FD_SETSIZE / __DARWIN_NFDBITS]; } fd_set;
#define FD_SET(n, p) ((p)->fds_bits[(unsigned long)(n) / __DARWIN_NFDBITS] |= (int)(1u << ((unsigned long)(n) % __DARWIN_NFDBITS)))
#define FD_CLR(n, p) ((p)->fds_bits[(unsigned long)(n) / __DARWIN_NFDBITS] &= ~(int)(1u << ((unsigned long)(n) % __DARWIN_NFDBITS)))
#define FD_ISSET(n, p) (((p)->fds_bits[(unsigned long)(n) / __DARWIN_NFDBITS] & (int)(1u << ((unsigned long)(n) % __DARWIN_NFDBITS))) != 0)
#define FD_ZERO(p) __builtin_memset((p), 0, sizeof(*(p)))
#endif
