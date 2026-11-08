/* Scatter/gather I/O (Darwin <sys/uio.h>). */
#ifndef ALCHEMY_SYS_UIO_H
#define ALCHEMY_SYS_UIO_H
#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif
struct iovec { void* iov_base; size_t iov_len; };
ssize_t readv(int fd, const struct iovec* vector, int count);
ssize_t writev(int fd, const struct iovec* vector, int count);
#ifdef __cplusplus
}
#endif
#endif
