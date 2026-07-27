/*
 * MVII baremetal POSIX shim — minimal <sys/types.h>.
 */
#ifndef _POSIX_SHIM_SYS_TYPES_H
#define _POSIX_SHIM_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Width-correct so these agree with the sysroot's own <sys/types.h> on both
 * LP64 (riscv64 / x86_64: __PTRDIFF_TYPE__ = long, __INT64_TYPE__ = long — the
 * historical values) and ILP32 (armv7: ssize_t = int, off_t = long long).
 * Matching the canonical types makes any double-typedef a legal identical
 * redefinition rather than a "different types" error. */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef __PTRDIFF_TYPE__ ssize_t;
#endif

#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef __INT64_TYPE__ off_t;
#endif

#ifndef _OFF64_T_DEFINED
#define _OFF64_T_DEFINED
typedef __INT64_TYPE__ off64_t;
#endif

#ifndef _USECONDS_T_DEFINED
#define _USECONDS_T_DEFINED
typedef unsigned long useconds_t;
#endif

typedef int      pid_t;
typedef unsigned mode_t;
typedef unsigned uid_t;
typedef unsigned gid_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef long          blkcnt_t;
typedef long          blksize_t;
typedef long          time_t_shim;

#ifndef _U_CHAR_DEFINED
#define _U_CHAR_DEFINED
typedef unsigned char u_char;
#endif

#ifndef _U_SHORT_DEFINED
#define _U_SHORT_DEFINED
typedef unsigned short u_short;
#endif

#ifndef _U_INT_DEFINED
#define _U_INT_DEFINED
typedef unsigned int u_int;
#endif

#ifndef _U_LONG_DEFINED
#define _U_LONG_DEFINED
typedef unsigned long u_long;
#endif

#ifndef _CADDR_T_DEFINED
#define _CADDR_T_DEFINED
typedef char *        caddr_t;
#endif

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_TYPES_H */
