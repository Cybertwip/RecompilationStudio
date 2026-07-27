/*
 * MVII baremetal POSIX shim - minimal <sys/resource.h>.
 */
#ifndef _POSIX_SHIM_SYS_RESOURCE_H
#define _POSIX_SHIM_SYS_RESOURCE_H

#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long rlim_t;

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
};

#define RLIM_INFINITY ((rlim_t)-1)
#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1

int getrlimit(int, struct rlimit*);
int setrlimit(int, const struct rlimit*);
int getrusage(int, struct rusage*);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_RESOURCE_H */
