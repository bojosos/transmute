/* Local (AF_UNIX) socket addresses with Darwin's layout. */
#ifndef ALCHEMY_SYS_UN_H
#define ALCHEMY_SYS_UN_H
#include "socket.h"
struct sockaddr_un {
    unsigned char sun_len;
    sa_family_t sun_family;
    char sun_path[104];
};
#endif
