#include "../minos_user_abi.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>

extern "C" const minos_user_abi* minos_current_user_abi;

namespace {

static inline const minos_user_abi* current_abi() {
    return minos_current_user_abi;
}

template <typename Ret, typename... Args>
static inline Ret call_or_fail(Ret fallback, Ret (*fn)(Args...), Args... args) {
    return fn ? fn(args...) : fallback;
}

} // namespace

extern "C" const minos_user_abi* minos_current_user_abi = nullptr;

extern "C" long sys_open(const char* path, int flags, int mode) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->open_fn, path, flags, mode) : -38L;
}

extern "C" long sys_close(int fd) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->close_fn, fd) : -38L;
}

extern "C" long sys_read(int fd, void* buf, size_t count) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->read_fn, fd, buf, count) : -38L;
}

extern "C" long sys_write(int fd, const void* buf, size_t count) {
    const minos_user_abi* abi = current_abi();
    if (abi && abi->write_fn && count > 64u * 1024u * 1024u) {
        char diagnostic[224];
        const int length = __builtin_snprintf(
            diagnostic, sizeof(diagnostic),
            "virtua-sys-write: rejected fd=%d buf=%p count=%llu caller=%p self=%p\n", fd, buf,
            static_cast<unsigned long long>(count), __builtin_return_address(0), (void *)&sys_write);
        if (length > 0) abi->write_fn(2, diagnostic, static_cast<size_t>(length));
        return -75;
    }
    return abi ? call_or_fail(-38L, abi->write_fn, fd, buf, count) : -38L;
}

extern "C" long sys_lseek(int fd, long offset, int whence) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->lseek_fn, fd, offset, whence) : -38L;
}

extern "C" long sys_fstat(int fd, void* st) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->fstat_fn, fd, st) : -38L;
}

extern "C" long sys_mkdir(const char* path, int mode) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->mkdir_fn, path, mode) : -38L;
}

extern "C" long sys_stat(const char* path, void* st) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->stat_fn, path, st) : -38L;
}

extern "C" long sys_unlink(const char* path) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->unlink_fn, path) : -38L;
}

extern "C" long sys_dup3(int oldfd, int newfd, int flags) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->dup3_fn, oldfd, newfd, flags) : -38L;
}

extern "C" long sys_rename(const char* oldpath, const char* newpath) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->rename_fn, oldpath, newpath) : -38L;
}

extern "C" long sys_getcwd(char* buf, size_t size) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->getcwd_fn, buf, size) : -38L;
}

extern "C" long sys_chdir(const char* path) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->chdir_fn, path) : -38L;
}

extern "C" long sys_opendir(const char* path) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->opendir_fn, path) : -38L;
}

extern "C" long sys_readdir(long handle, void* out_dirent) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->readdir_fn, handle, out_dirent) : -38L;
}

extern "C" long sys_closedir(long handle) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->closedir_fn, handle) : -38L;
}

extern "C" long sys_pipe2(int* fds, int flags) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->pipe2_fn, fds, flags) : -38L;
}

extern "C" long sys_gettimeofday(void* tv, void* tz) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->gettimeofday_fn, tv, tz) : -38L;
}

extern "C" long sys_set_menu_bar_enabled(int enabled) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->set_menu_bar_enabled_fn, enabled) : -38L;
}

extern "C" void* dlopen(const char* path, int flags) {
    const minos_user_abi* abi = current_abi();
    if (!abi || !abi->dynamic_open_fn) return nullptr;
    const long handle = abi->dynamic_open_fn(path, flags);
    return handle > 0 ? reinterpret_cast<void*>(static_cast<uintptr_t>(handle)) : nullptr;
}

extern "C" void* dlsym(void* handle, const char* name) {
    const minos_user_abi* abi = current_abi();
    if (!abi || !abi->dynamic_symbol_fn) return nullptr;
    return abi->dynamic_symbol_fn(static_cast<long>(reinterpret_cast<uintptr_t>(handle)), name);
}

extern "C" int dlclose(void* handle) {
    const minos_user_abi* abi = current_abi();
    if (!abi || !abi->dynamic_close_fn) return -38;
    return static_cast<int>(abi->dynamic_close_fn(static_cast<long>(reinterpret_cast<uintptr_t>(handle))));
}

extern "C" long sys_socket(int domain, int type, int protocol) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->socket_fn, domain, type, protocol) : -38L;
}

extern "C" long sys_connect(int fd, const void* addr, uint32_t addrlen) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->connect_fn, fd, addr, addrlen) : -38L;
}

extern "C" long sys_bind(int fd, const void* addr, uint32_t addrlen) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->bind_fn, fd, addr, addrlen) : -38L;
}

extern "C" long sys_listen(int fd, int backlog) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->listen_fn, fd, backlog) : -38L;
}

extern "C" long sys_accept(int fd, void* addr, uint32_t* addrlen) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->accept_fn, fd, addr, addrlen) : -38L;
}

extern "C" long sys_send(int fd, const void* buf, size_t count, int flags) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->send_fn, fd, buf, count, flags) : -38L;
}

extern "C" long sys_recv(int fd, void* buf, size_t count, int flags) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->recv_fn, fd, buf, count, flags) : -38L;
}

extern "C" long sys_sendto(int fd, const void* buf, size_t count, int flags, const void* addr, uint32_t addrlen) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->sendto_fn, fd, buf, count, flags, addr, addrlen) : -38L;
}

extern "C" long sys_recvfrom(int fd, void* buf, size_t count, int flags, void* addr, uint32_t* addrlen) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->recvfrom_fn, fd, buf, count, flags, addr, addrlen) : -38L;
}

extern "C" long sys_getsockname(int fd, void* addr, uint32_t* addrlen) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->getsockname_fn, fd, addr, addrlen) : -38L;
}

extern "C" long sys_poll(void* fds, size_t nfds, int timeout_ms) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->poll_fn, fds, nfds, timeout_ms) : -38L;
}

extern "C" long sys_select(int nfds, void* readfds, void* writefds, void* exceptfds, void* timeout) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->select_fn, nfds, readfds, writefds, exceptfds, timeout) : -38L;
}

extern "C" long sys_pread(int fd, void* buf, size_t count, long offset) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->pread_fn, fd, buf, count, offset) : -38L;
}

extern "C" long sys_pwrite(int fd, const void* buf, size_t count, long offset) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->pwrite_fn, fd, buf, count, offset) : -38L;
}

extern "C" long sys_mmap(size_t addr, size_t length, int prot, int flags, int fd, long offset) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->mmap_fn, addr, length, prot, flags, fd, offset) : -38L;
}

extern "C" long sys_munmap(size_t addr, size_t length) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->munmap_fn, addr, length) : -38L;
}

extern "C" long sys_thread_create(uintptr_t* thread, uintptr_t entry, uintptr_t arg, size_t stack_size) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->thread_create_fn, thread, entry, arg, stack_size) : -38L;
}

extern "C" long sys_thread_join(uintptr_t thread, void** retval) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->thread_join_fn, thread, retval) : -38L;
}

extern "C" long sys_thread_detach(uintptr_t thread) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->thread_detach_fn, thread) : -38L;
}

extern "C" uintptr_t sys_thread_self(void) {
    const minos_user_abi* abi = current_abi();
    return abi && abi->thread_self_fn ? abi->thread_self_fn() : 1;
}

extern "C" long sys_thread_yield(void) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->thread_yield_fn) : -38L;
}

extern "C" long sys_sleep_until_us(uint64_t deadline_us) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->sleep_until_us_fn, deadline_us) : -38L;
}

extern "C" long sys_setsockopt(int fd, int level, int optname, const void* optval, uint32_t optlen) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->setsockopt_fn, fd, level, optname, optval, optlen) : -38L;
}

extern "C" long sys_getsockopt(int fd, int level, int optname, void* optval, uint32_t* optlen) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->getsockopt_fn, fd, level, optname, optval, optlen) : -38L;
}

extern "C" long sys_shutdown(int fd, int how) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->shutdown_fn, fd, how) : -38L;
}

extern "C" long sys_host_exec(const char* tool, int argc, const char* const* argv) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(-38L, abi->host_exec_fn, tool, argc, argv) : -38L;
}

extern "C" int ioctl(int fd, unsigned long request, ...) {
    const minos_user_abi* abi = current_abi();
    void* arg = nullptr;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void*);
    va_end(ap);
    return abi ? static_cast<int>(call_or_fail(-38L, abi->ioctl_fn, fd, request, arg)) : -38;
}

extern "C" size_t brk(size_t addr) {
    const minos_user_abi* abi = current_abi();
    return abi ? call_or_fail(static_cast<size_t>(-1), abi->brk_fn, addr)
               : static_cast<size_t>(-1);
}

extern "C" void sys_exit(int code) {
    const minos_user_abi* abi = current_abi();
    if (abi && abi->exit_fn) {
        abi->exit_fn(code);
    }
    for (;;) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
#elif defined(__arm__) || defined(__aarch64__)
        __asm__ volatile("yield" ::: "memory");
#else
        __asm__ volatile("" ::: "memory");
#endif
    }
}
