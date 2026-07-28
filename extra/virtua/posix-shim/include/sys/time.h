/*
 * MVII baremetal POSIX shim — minimal <sys/time.h>.
 * llvm-libc baremetal does not ship POSIX <sys/time.h>.
 */
#ifndef _POSIX_SHIM_SYS_TIME_H
#define _POSIX_SHIM_SYS_TIME_H

#include <sys/types.h>
#include <time.h>
/* llvm-libc baremetal already defines `struct timeval` (sysroot include
 * path provides it via llvm-libc-types/struct_timeval.h). Reuse it. */
#include <llvm-libc-types/struct_timeval.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _STRUCT_TIMEZONE_DEFINED
#define _STRUCT_TIMEZONE_DEFINED
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
#endif

int settimeofday(const struct timeval *tv, const struct timezone *tz);
int utimes(const char *path, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_TIME_H */
