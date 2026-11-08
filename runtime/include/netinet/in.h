/* Internet addresses with Darwin's layouts and values (sin_len first; AF_INET6 30), as in
 * Apple's open-source xnu bsd/netinet/in.h and bsd/netinet6/in6.h. */
#ifndef ALCHEMY_NETINET_IN_H
#define ALCHEMY_NETINET_IN_H
#include "../sys/socket.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned short in_port_t;
typedef unsigned int in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    unsigned char sin_len;
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};
struct in6_addr {
    union {
        unsigned char __u6_addr8[16];
        unsigned short __u6_addr16[8];
        unsigned int __u6_addr32[4];
    } __u6_addr;
};
#define s6_addr __u6_addr.__u6_addr8
struct sockaddr_in6 {
    unsigned char sin6_len;
    sa_family_t sin6_family;
    in_port_t sin6_port;
    unsigned int sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned int sin6_scope_id;
};
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT {{{0}}}
#define IN6ADDR_LOOPBACK_INIT {{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}}

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58
#define IPPROTO_RAW 255

#define INADDR_ANY ((in_addr_t)0x00000000)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

#define IP_TOS 3
#define IP_TTL 4
#define IPV6_UNICAST_HOPS 4
#define IPV6_V6ONLY 27
#define IPV6_TCLASS 36

#define IN6_IS_ADDR_UNSPECIFIED(a) \
    ((a)->__u6_addr.__u6_addr32[0] == 0 && (a)->__u6_addr.__u6_addr32[1] == 0 && \
     (a)->__u6_addr.__u6_addr32[2] == 0 && (a)->__u6_addr.__u6_addr32[3] == 0)
#define IN6_IS_ADDR_LINKLOCAL(a) ((a)->s6_addr[0] == 0xfe && ((a)->s6_addr[1] & 0xc0) == 0x80)
#define IN6_IS_ADDR_V4MAPPED(a) \
    ((a)->__u6_addr.__u6_addr32[0] == 0 && (a)->__u6_addr.__u6_addr32[1] == 0 && \
     (a)->__u6_addr.__u6_addr32[2] == __builtin_bswap32(0x0000ffffu))

#define htons(x) __builtin_bswap16((unsigned short)(x))
#define ntohs(x) __builtin_bswap16((unsigned short)(x))
#define htonl(x) __builtin_bswap32((unsigned int)(x))
#define ntohl(x) __builtin_bswap32((unsigned int)(x))
#ifdef __cplusplus
}
#endif
#endif
