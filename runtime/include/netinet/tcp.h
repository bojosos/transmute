/* TCP socket options with Darwin's values (xnu bsd/netinet/tcp.h). */
#ifndef ALCHEMY_NETINET_TCP_H
#define ALCHEMY_NETINET_TCP_H
#define TCP_NODELAY 0x01
#define TCP_MAXSEG 0x02
#define TCP_KEEPALIVE 0x10
#define TCP_CONNECTIONTIMEOUT 0x20
#define TCP_KEEPINTVL 0x101
#define TCP_KEEPCNT 0x102
#define TCP_FASTOPEN 0x105
#endif
