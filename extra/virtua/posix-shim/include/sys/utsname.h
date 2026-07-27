/*
 * MVII baremetal POSIX shim - minimal <sys/utsname.h>.
 */
#ifndef _POSIX_SHIM_SYS_UTSNAME_H
#define _POSIX_SHIM_SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

int uname(struct utsname*);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_UTSNAME_H */
