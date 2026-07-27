/*
 * MVII baremetal POSIX shim - <limits.h> additions.
 */
#ifndef _POSIX_SHIM_LIMITS_H
#define _POSIX_SHIM_LIMITS_H

#include_next <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef OPEN_MAX
#define OPEN_MAX 256
#endif

#endif /* _POSIX_SHIM_LIMITS_H */
