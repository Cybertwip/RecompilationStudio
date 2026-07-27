/*
 * MVII baremetal POSIX shim - minimal <sys/un.h>.
 */
#ifndef _POSIX_SHIM_SYS_UN_H
#define _POSIX_SHIM_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};

#endif /* _POSIX_SHIM_SYS_UN_H */
