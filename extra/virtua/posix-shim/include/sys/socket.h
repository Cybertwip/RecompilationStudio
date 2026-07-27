/*
 * MVII baremetal POSIX shim - minimal <sys/socket.h>.
 */
#ifndef _POSIX_SHIM_SYS_SOCKET_H
#define _POSIX_SHIM_SYS_SOCKET_H

#include <stddef.h>
#include <sys/select.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_padding[126];
};

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL AF_UNIX
#define AF_INET 2
#define AF_INET6 10

#define PF_UNIX AF_UNIX
#define PF_LOCAL AF_LOCAL
#define PF_INET AF_INET
#define PF_INET6 AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_SEQPACKET 5

#define SOCK_NONBLOCK 00004000
#define SOCK_CLOEXEC 02000000

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define SOMAXCONN 128

#define MSG_OOB 0x0001
#define MSG_PEEK 0x0002
#define MSG_DONTROUTE 0x0004
#define MSG_TRUNC 0x0020
#define MSG_DONTWAIT 0x0040
#define MSG_WAITALL 0x0100
#define MSG_CTRUNC 0x0008
#define MSG_NOSIGNAL 0x4000

#define SOL_SOCKET 1
#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER 13
#define SO_REUSEPORT 15
#define SO_PEERCRED 17
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_ACCEPTCONN 30

int socket(int, int, int);
int connect(int, const struct sockaddr*, socklen_t);
int bind(int, const struct sockaddr*, socklen_t);
int listen(int, int);
int accept(int, struct sockaddr*, socklen_t*);
int getsockname(int, struct sockaddr*, socklen_t*);
int setsockopt(int, int, int, const void*, socklen_t);
int getsockopt(int, int, int, void*, socklen_t*);
int shutdown(int, int);
ssize_t send(int, const void*, size_t, int);
ssize_t recv(int, void*, size_t, int);
ssize_t sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
ssize_t recvfrom(int, void*, size_t, int, struct sockaddr*, socklen_t*);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_SOCKET_H */
