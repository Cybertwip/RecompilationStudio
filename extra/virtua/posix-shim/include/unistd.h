/*
 * MVII baremetal POSIX shim — minimal <unistd.h>.
 * llvm-libc baremetal does not ship POSIX headers.
 */
#ifndef _POSIX_SHIM_UNISTD_H
#define _POSIX_SHIM_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

/* access() mode bits */
#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     pipe(int pipefd[2]);
off_t   lseek(int fd, off_t offset, int whence);
off64_t lseek64(int fd, off64_t offset, int whence);
int     unlink(const char *path);
int     symlink(const char *target, const char *linkpath);
int     link(const char *target, const char *linkpath);
int     access(const char *path, int mode);
int     isatty(int fd);
int     gethostname(char *name, size_t len);
pid_t   getpid(void);
uid_t   getuid(void);
gid_t   getgid(void);
pid_t   fork(void);
int     execv(const char *path, char *const argv[]);
int     execl(const char *path, const char *arg, ...);
int     execvp(const char *file, char *const argv[]);
int     execve(const char *path, char *const argv[], char *const envp[]);
pid_t   setsid(void);
pid_t   getsid(pid_t pid);
void    _exit(int status);
unsigned int alarm(unsigned int seconds);
int     kill(pid_t pid, int sig);
unsigned int sleep(unsigned int seconds);
int     usleep(useconds_t usec);
int     rmdir(const char *path);
long    sysconf(int name);
long    pathconf(const char *path, int name);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int     ftruncate(int fd, off_t length);
int     ftruncate64(int fd, off64_t length);
int     truncate(const char *path, off_t length);
char   *getcwd(char *buf, size_t size);
int     chdir(const char *path);
int     unlinkat(int dirfd, const char *path, int flags);

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 1
#endif
#ifndef _SC_PAGE_SIZE
#define _SC_PAGE_SIZE 2
#endif
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE _SC_PAGE_SIZE
#endif
#ifndef _SC_ARG_MAX
#define _SC_ARG_MAX 3
#endif
#ifndef _SC_GETPW_R_SIZE_MAX
#define _SC_GETPW_R_SIZE_MAX 4
#endif
#ifndef _PC_PATH_MAX
#define _PC_PATH_MAX 1
#endif

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_UNISTD_H */
