/* poll(2) with Darwin's event values (xnu bsd/sys/poll.h). */
#ifndef ALCHEMY_POLL_H
#define ALCHEMY_POLL_H
#ifdef __cplusplus
extern "C" {
#endif
struct pollfd { int fd; short events; short revents; };
typedef unsigned int nfds_t;
#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLRDNORM 0x0040
#define POLLWRNORM POLLOUT
#define POLLRDBAND 0x0080
#define POLLWRBAND 0x0100
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
int poll(struct pollfd* fds, nfds_t count, int timeout);
#ifdef __cplusplus
}
#endif
#endif
