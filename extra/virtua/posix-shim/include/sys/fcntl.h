/*
 * MVII baremetal POSIX shim — minimal <sys/fcntl.h>.
 * On most modern systems <sys/fcntl.h> just forwards to <fcntl.h>; here it's
 * intentionally empty since Virtua/fcntl.h defines its own O_* constants
 * before including this header. Keep it as an include guard to satisfy the
 * #include directive without dragging in unrelated declarations.
 */
#ifndef _POSIX_SHIM_SYS_FCNTL_H
#define _POSIX_SHIM_SYS_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef F_DUPFD
#define F_DUPFD 0
#endif
#ifndef F_GETFD
#define F_GETFD 1
#endif
#ifndef F_SETFD
#define F_SETFD 2
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef F_GETLK
#define F_GETLK 5
#endif
#ifndef F_SETLK
#define F_SETLK 6
#endif
#ifndef F_SETLKW
#define F_SETLKW 7
#endif

#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif

int open(const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_FCNTL_H */
