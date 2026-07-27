/*
 * MVII baremetal POSIX shim - minimal <sys/wait.h>.
 */
#ifndef _POSIX_SHIM_SYS_WAIT_H
#define _POSIX_SHIM_SYS_WAIT_H

#include <sys/types.h>
#include <sys/resource.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef WUNTRACED
#define WUNTRACED 2
#endif
#ifndef WCONTINUED
#define WCONTINUED 8
#endif

#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WTERMSIG(status) ((status) & 0x7f)
#define WSTOPSIG(status) WEXITSTATUS(status)
#define WIFEXITED(status) (WTERMSIG(status) == 0)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0 && ((status) & 0x7f) != 0x7f)
#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WIFCONTINUED(status) ((status) == 0xffff)

pid_t wait(int*);
pid_t waitpid(pid_t, int*, int);
pid_t wait4(pid_t, int*, int, struct rusage*);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_WAIT_H */
