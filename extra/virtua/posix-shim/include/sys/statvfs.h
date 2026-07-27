/*
 * MVII baremetal POSIX shim - minimal <sys/statvfs.h>.
 */
#ifndef _POSIX_SHIM_SYS_STATVFS_H
#define _POSIX_SHIM_SYS_STATVFS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MNT_LOCAL
#define MNT_LOCAL 0x00001000
#endif

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;
    unsigned long f_ffree;
    unsigned long f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_flags;
    unsigned long f_namemax;
};

int statvfs(const char*, struct statvfs*);
int fstatvfs(int, struct statvfs*);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_STATVFS_H */
