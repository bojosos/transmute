/* Address conversion (Darwin <arpa/inet.h>). */
#ifndef ALCHEMY_ARPA_INET_H
#define ALCHEMY_ARPA_INET_H
#include "../netinet/in.h"
#ifdef __cplusplus
extern "C" {
#endif
in_addr_t inet_addr(const char* text);
char* inet_ntoa(struct in_addr address);
const char* inet_ntop(int family, const void* __restrict source, char* __restrict destination, socklen_t size);
int inet_pton(int family, const char* __restrict source, void* __restrict destination);
#ifdef __cplusplus
}
#endif
#endif
