/*
 * MVII baremetal POSIX shim - minimal <sys/ioctl.h>.
 */
#ifndef _POSIX_SHIM_SYS_IOCTL_H
#define _POSIX_SHIM_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413
#define FIONREAD 0x541B

int ioctl(int, unsigned long, ...);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_IOCTL_H */
