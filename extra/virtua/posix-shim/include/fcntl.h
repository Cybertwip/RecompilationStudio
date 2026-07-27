/*
 * MVII baremetal POSIX shim — minimal <fcntl.h>.
 *
 * llvm-libc baremetal does not install a top-level <fcntl.h>; provide
 * the constants and prototypes that userland code (e.g. Virtua's Dash
 * runtime in External/Virtua/Dash/libDash.c) expects.  The values match
 * the Linux/glibc layout, which is also what the kernel's syscall
 * surface implements.
 */
#ifndef _POSIX_SHIM_FCNTL_H
#define _POSIX_SHIM_FCNTL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef O_RDONLY
#define O_RDONLY    0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY    0x0001
#endif
#ifndef O_RDWR
#define O_RDWR      0x0002
#endif
#ifndef O_ACCMODE
#define O_ACCMODE   0x0003
#endif
#ifndef O_CREAT
#define O_CREAT     0x0040
#endif
#ifndef O_EXCL
#define O_EXCL      0x0080
#endif
#ifndef O_NOCTTY
#define O_NOCTTY    0x0100
#endif
#ifndef O_TRUNC
#define O_TRUNC     0x0200
#endif
#ifndef O_APPEND
#define O_APPEND    0x0400
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK  0x0800
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW  0x20000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC   0x80000
#endif

#ifndef AT_FDCWD
#define AT_FDCWD    (-100)
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

#ifndef F_DUPFD
#define F_DUPFD  0
#endif
#ifndef F_GETFD
#define F_GETFD  1
#endif
#ifndef F_SETFD
#define F_SETFD  2
#endif
#ifndef F_GETFL
#define F_GETFL  3
#endif
#ifndef F_SETFL
#define F_SETFL  4
#endif
#ifndef F_GETLK
#define F_GETLK  5
#endif
#ifndef F_SETLK
#define F_SETLK  6
#endif
#ifndef F_SETLKW
#define F_SETLKW 7
#endif

#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif

#ifndef F_RDLCK
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#endif

struct flock {
    short l_type;
    short l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_FCNTL_H */
