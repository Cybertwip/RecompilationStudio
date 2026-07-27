/*
 * MVII baremetal POSIX shim — <time.h> wrapper.
 *
 * Just chains to llvm-libc's sysroot <time.h>. The `time(time_t*)`
 * entrypoint is provided by Kernel/Kendryte/Virtio/Drivers/syscalls.cpp
 * (it isn't in llvm-libc's baremetal entrypoint set, but the prototype
 * already lives in <ctime>'s `using ::time` declaration as soon as
 * something in the TU pulls in a declaration).
 */
#ifndef _POSIX_SHIM_TIME_H
#define _POSIX_SHIM_TIME_H

#include_next <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* llvm-libc baremetal does not declare these; syscalls.cpp implements
 * them. Make the prototypes visible so <ctime>'s using-declaration can
 * resolve. */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#ifndef TIME_MONOTONIC
#define TIME_MONOTONIC 2
#endif

#ifdef __cplusplus
int    clock_getres(clockid_t clock_id, struct timespec *tp) noexcept;
#else
int    clock_getres(clockid_t clock_id, struct timespec *tp);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_TIME_H */
