/*
 * MVII baremetal POSIX shim - minimal <netinet/tcp.h>.
 */
#ifndef _POSIX_SHIM_NETINET_TCP_H
#define _POSIX_SHIM_NETINET_TCP_H

#include <netinet/in.h>

#define TCP_NODELAY 1
#define TCP_MAXSEG 2
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6

#endif /* _POSIX_SHIM_NETINET_TCP_H */
