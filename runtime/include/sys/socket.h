/* BSD sockets with Darwin's layouts and values (sa_len first; SOL_SOCKET 0xffff), as in
 * Apple's open-source xnu bsd/sys/socket.h. Served by Alchemy's libSystem provider. */
#ifndef ALCHEMY_SYS_SOCKET_H
#define ALCHEMY_SYS_SOCKET_H
#include "types.h"
#include "uio.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned char sa_family_t;
typedef unsigned int socklen_t;

struct sockaddr {
    unsigned char sa_len;
    sa_family_t sa_family;
    char sa_data[14];
};
struct sockaddr_storage {
    unsigned char ss_len;
    sa_family_t ss_family;
    char __ss_pad1[6];
    long long __ss_align;
    char __ss_pad2[112];
};
struct linger { int l_onoff; int l_linger; };
struct msghdr {
    void* msg_name;
    socklen_t msg_namelen;
    struct iovec* msg_iov;
    int msg_iovlen;
    void* msg_control;
    socklen_t msg_controllen;
    int msg_flags;
};

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL AF_UNIX
#define AF_INET 2
#define AF_INET6 30
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX AF_UNIX
#define PF_LOCAL AF_LOCAL
#define PF_INET AF_INET
#define PF_INET6 AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_SEQPACKET 5

#define SOL_SOCKET 0xffff
#define SO_DEBUG 0x0001
#define SO_ACCEPTCONN 0x0002
#define SO_REUSEADDR 0x0004
#define SO_KEEPALIVE 0x0008
#define SO_DONTROUTE 0x0010
#define SO_BROADCAST 0x0020
#define SO_LINGER 0x0080
#define SO_OOBINLINE 0x0100
#define SO_REUSEPORT 0x0200
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
#define SO_SNDLOWAT 0x1003
#define SO_RCVLOWAT 0x1004
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define SO_ERROR 0x1007
#define SO_TYPE 0x1008
#define SO_NREAD 0x1020
#define SO_NOSIGPIPE 0x1022

#define MSG_OOB 0x1
#define MSG_PEEK 0x2
#define MSG_DONTROUTE 0x4
#define MSG_EOR 0x8
#define MSG_TRUNC 0x10
#define MSG_CTRUNC 0x20
#define MSG_WAITALL 0x40
#define MSG_DONTWAIT 0x80
#define MSG_NOSIGNAL 0x80000

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define SOMAXCONN 128

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int fds[2]);
int bind(int fd, const struct sockaddr* address, socklen_t length);
int connect(int fd, const struct sockaddr* address, socklen_t length);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* __restrict address, socklen_t* __restrict length);
int getsockname(int fd, struct sockaddr* __restrict address, socklen_t* __restrict length);
int getpeername(int fd, struct sockaddr* __restrict address, socklen_t* __restrict length);
int getsockopt(int fd, int level, int name, void* __restrict value, socklen_t* __restrict length);
int setsockopt(int fd, int level, int name, const void* value, socklen_t length);
ssize_t send(int fd, const void* buffer, size_t length, int flags);
ssize_t sendto(int fd, const void* buffer, size_t length, int flags, const struct sockaddr* to, socklen_t to_length);
ssize_t sendmsg(int fd, const struct msghdr* message, int flags);
ssize_t recv(int fd, void* buffer, size_t length, int flags);
ssize_t recvfrom(int fd, void* __restrict buffer, size_t length, int flags, struct sockaddr* __restrict from,
                 socklen_t* __restrict from_length);
ssize_t recvmsg(int fd, struct msghdr* message, int flags);
int shutdown(int fd, int how);
#ifdef __cplusplus
}
#endif
#endif
