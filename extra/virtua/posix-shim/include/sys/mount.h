/*
 * MVII baremetal POSIX shim - minimal <sys/mount.h>.
 */
#ifndef _POSIX_SHIM_SYS_MOUNT_H
#define _POSIX_SHIM_SYS_MOUNT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MNT_LOCAL
#define MNT_LOCAL 0x00001000
#endif

struct statfs {
    unsigned long f_bsize;
    unsigned long f_iosize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    unsigned long f_fsid;
    unsigned long f_owner;
    unsigned long f_type;
    unsigned long f_flags;
    unsigned long f_namelen;
    unsigned long f_frsize;
};

int statfs(const char*, struct statfs*);
int fstatfs(int, struct statfs*);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_MOUNT_H */
