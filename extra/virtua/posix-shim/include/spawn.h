#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int __reserved;
} posix_spawn_file_actions_t;

typedef struct {
    int __reserved;
} posix_spawnattr_t;

#ifndef POSIX_SPAWN_SETPGROUP
#define POSIX_SPAWN_SETPGROUP 0x02
#endif

int posix_spawn(pid_t* pid,
                const char* path,
                const posix_spawn_file_actions_t* file_actions,
                const posix_spawnattr_t* attrp,
                char* const argv[],
                char* const envp[]);
int posix_spawnp(pid_t* pid,
                 const char* file,
                 const posix_spawn_file_actions_t* file_actions,
                 const posix_spawnattr_t* attrp,
                 char* const argv[],
                 char* const envp[]);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t* file_actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* file_actions);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* file_actions, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* file_actions, int fd, int newfd);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* file_actions,
                                     int fd,
                                     const char* path,
                                     int oflag,
                                     mode_t mode);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t* file_actions, const char* path);
int posix_spawnattr_init(posix_spawnattr_t* attr);
int posix_spawnattr_destroy(posix_spawnattr_t* attr);
int posix_spawnattr_setflags(posix_spawnattr_t* attr, short flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t* attr, pid_t pgroup);

#ifdef __cplusplus
}
#endif
