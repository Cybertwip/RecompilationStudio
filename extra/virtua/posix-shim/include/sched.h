/* MVII baremetal POSIX shim - minimal <sched.h>. */
#ifndef _POSIX_SHIM_SCHED_H
#define _POSIX_SHIM_SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

int sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SCHED_H */
