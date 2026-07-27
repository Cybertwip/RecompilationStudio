/*
 * MVII baremetal POSIX shim - minimal <sys/mman.h>.
 */
#ifndef _POSIX_SHIM_SYS_MMAN_H
#define _POSIX_SHIM_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void*)-1)

#define MS_SYNC 0x0004
#define MS_ASYNC 0x0001
#define MS_INVALIDATE 0x0002

#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4

void* mmap(void*, size_t, int, int, int, off_t);
int munmap(void*, size_t);
int mprotect(void*, size_t, int);
int msync(void*, size_t, int);
int madvise(void*, size_t, int);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SYS_MMAN_H */
