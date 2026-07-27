/*
 * MVII baremetal POSIX shim — minimal <machine/syscall.h>.
 *
 * Newlib's <machine/syscall.h> defines SYS_* numbers used by reent's
 * libgloss-based syscall dispatch. llvm-libc baremetal does not need
 * these (we route I/O through the shim and direct platform calls), but
 * legacy code (K210/Drivers/syscalls.h, Virtua/sysnum.h) still includes
 * the header. Provide the bare minimum SYS_* numbers so it compiles.
 */
#ifndef _POSIX_SHIM_MACHINE_SYSCALL_H
#define _POSIX_SHIM_MACHINE_SYSCALL_H

#define SYS_exit         1
#define SYS_exit_group   94
#define SYS_close        3
#define SYS_read         4
#define SYS_write        5
#define SYS_open         6
#define SYS_link         9
#define SYS_unlink      10
#define SYS_unlinkat    35
#define SYS_lseek       19
#define SYS_getpid      20
#define SYS_getuid      174
#define SYS_geteuid     175
#define SYS_getgid      176
#define SYS_getegid     177
#define SYS_kill        37
#define SYS_fstat       62
#define SYS_fstatat     79
#define SYS_faccessat   48
#define SYS_stat        38
#define SYS_lstat       39
#define SYS_isatty      29
#define SYS_sbrk        45
#define SYS_brk         214
#define SYS_dup         23
#define SYS_dup3        24
#define SYS_fcntl       25
#define SYS_ioctl       29
#define SYS_writev      66
#define SYS_pread       67
#define SYS_pwrite      68
#define SYS_getcwd      17
#define SYS_chdir       49
#define SYS_fchdir      50
#define SYS_chroot      51
#define SYS_getdents    61
#define SYS_mkdir       83
#define SYS_access      33
#define SYS_mmap        222
#define SYS_munmap      215
#define SYS_mremap      216
#define SYS_gettimeofday 78
#define SYS_time        201
#define SYS_times       43
#define SYS_uname       160
#define SYS_rt_sigaction 134
#define SYS_getmainvars  2011

#endif /* _POSIX_SHIM_MACHINE_SYSCALL_H */
