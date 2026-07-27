#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minos_user_abi {
    long   (*open_fn)(const char* path, int flags, int mode);
    long   (*close_fn)(int fd);
    long   (*read_fn)(int fd, void* buf, size_t count);
    long   (*write_fn)(int fd, const void* buf, size_t count);
    long   (*lseek_fn)(int fd, long offset, int whence);
    long   (*fstat_fn)(int fd, void* st);
    long   (*mkdir_fn)(const char* path, int mode);
    long   (*ioctl_fn)(int fd, unsigned long request, void* arg);
    size_t (*brk_fn)(size_t addr);
    long   (*gettimeofday_fn)(void* tv, void* tz);
    void   (*exit_fn)(int code);
    long   (*set_menu_bar_enabled_fn)(int enabled);
    long   (*dynamic_open_fn)(const char* path, int flags);
    void*  (*dynamic_symbol_fn)(long handle, const char* name);
    long   (*dynamic_close_fn)(long handle);
    long   (*stat_fn)(const char* path, void* st);
    long   (*unlink_fn)(const char* path);
    long   (*getcwd_fn)(char* buf, size_t size);
    long   (*chdir_fn)(const char* path);
    long   (*opendir_fn)(const char* path);
    long   (*readdir_fn)(long handle, void* out_dirent);
    long   (*closedir_fn)(long handle);
    long   (*socket_fn)(int domain, int type, int protocol);
    long   (*connect_fn)(int fd, const void* addr, uint32_t addrlen);
    long   (*bind_fn)(int fd, const void* addr, uint32_t addrlen);
    long   (*listen_fn)(int fd, int backlog);
    long   (*accept_fn)(int fd, void* addr, uint32_t* addrlen);
    long   (*send_fn)(int fd, const void* buf, size_t count, int flags);
    long   (*recv_fn)(int fd, void* buf, size_t count, int flags);
    long   (*setsockopt_fn)(int fd, int level, int optname, const void* optval, uint32_t optlen);
    long   (*getsockopt_fn)(int fd, int level, int optname, void* optval, uint32_t* optlen);
    long   (*shutdown_fn)(int fd, int how);
    long   (*host_exec_fn)(const char* tool, int argc, const char* const* argv);
    long   (*rename_fn)(const char* oldpath, const char* newpath);
    long   (*pipe2_fn)(int* fds, int flags);
    long   (*dup3_fn)(int oldfd, int newfd, int flags);
    long   (*sendto_fn)(int fd, const void* buf, size_t count, int flags, const void* addr, uint32_t addrlen);
    long   (*recvfrom_fn)(int fd, void* buf, size_t count, int flags, void* addr, uint32_t* addrlen);
    long   (*getsockname_fn)(int fd, void* addr, uint32_t* addrlen);
    long   (*poll_fn)(void* fds, size_t nfds, int timeout_ms);
    long   (*select_fn)(int nfds, void* readfds, void* writefds, void* exceptfds, void* timeout);
    long   (*pread_fn)(int fd, void* buf, size_t count, long offset);
    long   (*pwrite_fn)(int fd, const void* buf, size_t count, long offset);
    long   (*mmap_fn)(size_t addr, size_t length, int prot, int flags, int fd, long offset);
    long   (*munmap_fn)(size_t addr, size_t length);
    long   (*thread_create_fn)(uintptr_t* thread, uintptr_t entry, uintptr_t arg, size_t stack_size);
    long   (*thread_join_fn)(uintptr_t thread, void** retval);
    long   (*thread_detach_fn)(uintptr_t thread);
    uintptr_t (*thread_self_fn)(void);
    long   (*thread_yield_fn)(void);
    long   (*sleep_until_us_fn)(uint64_t deadline_us);
} minos_user_abi;

const minos_user_abi* minos_get_user_abi(void);

typedef struct virtua_dirent {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  d_reserved;
    char     d_name[256];
} virtua_dirent;

#ifndef RTLD_LAZY
#define RTLD_LAZY 1
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 2
#endif

void* dlopen(const char* path, int flags);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);

#ifdef __cplusplus
}
#endif
