/*
 * MVII baremetal POSIX shim - minimal <net/if.h>.
 */
#ifndef _POSIX_SHIM_NET_IF_H
#define _POSIX_SHIM_NET_IF_H

#ifdef __cplusplus
extern "C" {
#endif

#define IF_NAMESIZE 16

#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000

unsigned if_nametoindex(const char* ifname);
char* if_indextoname(unsigned ifindex, char* ifname);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_NET_IF_H */
