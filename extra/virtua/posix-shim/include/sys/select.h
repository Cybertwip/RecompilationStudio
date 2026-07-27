/*
 * MVII baremetal POSIX shim - minimal <sys/select.h>.
 */
#ifndef _POSIX_SHIM_SYS_SELECT_H
#define _POSIX_SHIM_SYS_SELECT_H

#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef unsigned long fd_mask;
#define _POSIX_SHIM_NFDBITS ((int)(8 * sizeof(fd_mask)))

typedef struct fd_set {
    fd_mask fds_bits[(FD_SETSIZE + _POSIX_SHIM_NFDBITS - 1) / _POSIX_SHIM_NFDBITS];
} fd_set;

#define FD_ZERO(set) do { \
    fd_set* _fdset = (set); \
    for (int _fd_i = 0; _fd_i < (int)(sizeof(_fdset->fds_bits) / sizeof(_fdset->fds_bits[0])); ++_fd_i) { \
        _fdset->fds_bits[_fd_i] = 0; \
    } \
} while (0)

#define FD_SET(fd, set) do { \
    int _fd = (fd); \
    if (_fd >= 0 && _fd < FD_SETSIZE) { \
        (set)->fds_bits[_fd / _POSIX_SHIM_NFDBITS] |= ((fd_mask)1 << (_fd % _POSIX_SHIM_NFDBITS)); \
    } \
} while (0)

#define FD_CLR(fd, set) do { \
    int _fd = (fd); \
    if (_fd >= 0 && _fd < FD_SETSIZE) { \
        (set)->fds_bits[_fd / _POSIX_SHIM_NFDBITS] &= ~((fd_mask)1 << (_fd % _POSIX_SHIM_NFDBITS)); \
    } \
} while (0)

#define FD_ISSET(fd, set) \
    ((fd) >= 0 && (fd) < FD_SETSIZE && (((set)->fds_bits[(fd) / _POSIX_SHIM_NFDBITS] & \
    ((fd_mask)1 << ((fd) % _POSIX_SHIM_NFDBITS))) != 0))

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_SELECT_H */
