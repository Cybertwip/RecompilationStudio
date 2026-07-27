#ifndef _POSIX_SHIM_FTW_H
#define _POSIX_SHIM_FTW_H

#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FTW {
    int base;
    int level;
};

#ifndef FTW_F
#define FTW_F 0
#define FTW_D 1
#define FTW_DNR 2
#define FTW_DP 3
#define FTW_NS 4
#define FTW_SL 5
#define FTW_SLN 6
#endif

#ifndef FTW_PHYS
#define FTW_PHYS 0x01
#define FTW_MOUNT 0x02
#define FTW_DEPTH 0x04
#define FTW_CHDIR 0x08
#endif

typedef int (*__ftw_func_t)(const char*, const struct stat*, int, struct FTW*);

int nftw(const char* path, __ftw_func_t fn, int fd_limit, int flags);

#ifdef __cplusplus
}
#endif

#endif
