/*
 * MVII baremetal POSIX shim -- minimal <dirent.h>.
 * llvm-libc baremetal does not ship POSIX directory headers.
 */
#ifndef _POSIX_SHIM_DIRENT_H
#define _POSIX_SHIM_DIRENT_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14
#endif

typedef struct DIR DIR;

struct dirent {
    ino_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

DIR* opendir(const char* name);
DIR* fdopendir(int fd);
struct dirent* readdir(DIR* dirp);
int closedir(DIR* dirp);
int dirfd(DIR* dirp);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_DIRENT_H */
