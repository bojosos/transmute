/* Interface names and indexes (Darwin <net/if.h>, the name-index part). */
#ifndef ALCHEMY_NET_IF_H
#define ALCHEMY_NET_IF_H
#ifdef __cplusplus
extern "C" {
#endif
#define IF_NAMESIZE 16
#define IFNAMSIZ IF_NAMESIZE
unsigned int if_nametoindex(const char* name);
char* if_indextoname(unsigned int index, char* name);
#ifdef __cplusplus
}
#endif
#endif
