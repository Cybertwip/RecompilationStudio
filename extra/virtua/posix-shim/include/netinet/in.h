/*
 * MVII baremetal POSIX shim - minimal <netinet/in.h>.
 */
#ifndef _POSIX_SHIM_NETINET_IN_H
#define _POSIX_SHIM_NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct in6_addr {
    union {
        uint8_t __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
};

#define s6_addr __in6_u.__u6_addr8

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41

#define INADDR_ANY ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE ((in_addr_t)0xffffffff)

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

#define IN6_IS_ADDR_UNSPECIFIED(addr)                                                                                  \
    (((const struct in6_addr*)(addr))->s6_addr[0] == 0 && ((const struct in6_addr*)(addr))->s6_addr[1] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[2] == 0 && ((const struct in6_addr*)(addr))->s6_addr[3] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[4] == 0 && ((const struct in6_addr*)(addr))->s6_addr[5] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[6] == 0 && ((const struct in6_addr*)(addr))->s6_addr[7] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[8] == 0 && ((const struct in6_addr*)(addr))->s6_addr[9] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[10] == 0 && ((const struct in6_addr*)(addr))->s6_addr[11] == 0 &&        \
     ((const struct in6_addr*)(addr))->s6_addr[12] == 0 && ((const struct in6_addr*)(addr))->s6_addr[13] == 0 &&        \
     ((const struct in6_addr*)(addr))->s6_addr[14] == 0 && ((const struct in6_addr*)(addr))->s6_addr[15] == 0)

#define IN6_IS_ADDR_LOOPBACK(addr)                                                                                     \
    (((const struct in6_addr*)(addr))->s6_addr[0] == 0 && ((const struct in6_addr*)(addr))->s6_addr[1] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[2] == 0 && ((const struct in6_addr*)(addr))->s6_addr[3] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[4] == 0 && ((const struct in6_addr*)(addr))->s6_addr[5] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[6] == 0 && ((const struct in6_addr*)(addr))->s6_addr[7] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[8] == 0 && ((const struct in6_addr*)(addr))->s6_addr[9] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[10] == 0 && ((const struct in6_addr*)(addr))->s6_addr[11] == 0 &&        \
     ((const struct in6_addr*)(addr))->s6_addr[12] == 0 && ((const struct in6_addr*)(addr))->s6_addr[13] == 0 &&        \
     ((const struct in6_addr*)(addr))->s6_addr[14] == 0 && ((const struct in6_addr*)(addr))->s6_addr[15] == 1)

#define IN6_IS_ADDR_V4MAPPED(addr)                                                                                     \
    (((const struct in6_addr*)(addr))->s6_addr[0] == 0 && ((const struct in6_addr*)(addr))->s6_addr[1] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[2] == 0 && ((const struct in6_addr*)(addr))->s6_addr[3] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[4] == 0 && ((const struct in6_addr*)(addr))->s6_addr[5] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[6] == 0 && ((const struct in6_addr*)(addr))->s6_addr[7] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[8] == 0 && ((const struct in6_addr*)(addr))->s6_addr[9] == 0 &&          \
     ((const struct in6_addr*)(addr))->s6_addr[10] == 0xff && ((const struct in6_addr*)(addr))->s6_addr[11] == 0xff)

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;

uint32_t htonl(uint32_t hostlong);
uint16_t htons(uint16_t hostshort);
uint32_t ntohl(uint32_t netlong);
uint16_t ntohs(uint16_t netshort);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_NETINET_IN_H */
