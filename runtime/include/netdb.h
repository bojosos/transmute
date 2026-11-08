/* Name resolution with Darwin's layouts and values (Libc include/netdb.h: ai_canonname before
 * ai_addr). Served by Alchemy's libSystem provider. */
#ifndef ALCHEMY_NETDB_H
#define ALCHEMY_NETDB_H
#include "netinet/in.h"
#ifdef __cplusplus
extern "C" {
#endif
struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    char* ai_canonname;
    struct sockaddr* ai_addr;
    struct addrinfo* ai_next;
};
struct hostent {
    char* h_name;
    char** h_aliases;
    int h_addrtype;
    int h_length;
    char** h_addr_list;
};
struct servent {
    char* s_name;
    char** s_aliases;
    int s_port;
    char* s_proto;
};
#define h_addr h_addr_list[0]

#define AI_PASSIVE 0x00000001
#define AI_CANONNAME 0x00000002
#define AI_NUMERICHOST 0x00000004
#define AI_ALL 0x00000100
#define AI_V4MAPPED_CFG 0x00000200
#define AI_ADDRCONFIG 0x00000400
#define AI_V4MAPPED 0x00000800
#define AI_NUMERICSERV 0x00001000
#define AI_DEFAULT (AI_V4MAPPED_CFG | AI_ADDRCONFIG)

#define EAI_ADDRFAMILY 1
#define EAI_AGAIN 2
#define EAI_BADFLAGS 3
#define EAI_FAIL 4
#define EAI_FAMILY 5
#define EAI_MEMORY 6
#define EAI_NODATA 7
#define EAI_NONAME 8
#define EAI_SERVICE 9
#define EAI_SOCKTYPE 10
#define EAI_SYSTEM 11
#define EAI_OVERFLOW 14

#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define NI_NOFQDN 0x00000001
#define NI_NUMERICHOST 0x00000002
#define NI_NAMEREQD 0x00000004
#define NI_NUMERICSERV 0x00000008
#define NI_DGRAM 0x00000010

int getaddrinfo(const char* __restrict node, const char* __restrict service, const struct addrinfo* __restrict hints,
                struct addrinfo** __restrict result);
void freeaddrinfo(struct addrinfo* list);
const char* gai_strerror(int code);
int getnameinfo(const struct sockaddr* __restrict address, socklen_t length, char* __restrict host, socklen_t host_length,
                char* __restrict service, socklen_t service_length, int flags);
struct hostent* gethostbyname(const char* name);
struct servent* getservbyname(const char* name, const char* protocol);
#ifdef __cplusplus
}
#endif
#endif
