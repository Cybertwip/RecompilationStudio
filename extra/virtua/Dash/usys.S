#include "sysnum.h"

# -----------------------------------------------------------------------------
# Syscall Macro
# -----------------------------------------------------------------------------

.macro SYSCALL name, num
.global \name
\name:
    li a7, \num
    ecall
    ret
.endm

# -----------------------------------------------------------------------------
# Wrappers with sys_ prefix (called by libc.cpp)
# -----------------------------------------------------------------------------

# Process Control
SYSCALL sys_exit,          SYS_exit
SYSCALL sys_exit_group,    SYS_exit_group
SYSCALL sys_getpid,        SYS_getpid
SYSCALL sys_getuid,        SYS_getuid
SYSCALL sys_geteuid,       SYS_geteuid
SYSCALL sys_getgid,        SYS_getgid
SYSCALL sys_getegid,       SYS_getegid
SYSCALL sys_kill,          SYS_kill
SYSCALL sys_rt_sigaction,  SYS_rt_sigaction
SYSCALL sys_times,         SYS_times
SYSCALL sys_uname,         SYS_uname

# File Operations
SYSCALL sys_read,          SYS_read
SYSCALL sys_write,         SYS_write
SYSCALL sys_writev,        SYS_writev
SYSCALL sys_pread,         SYS_pread
SYSCALL sys_pwrite,        SYS_pwrite
SYSCALL sys_close,         SYS_close
SYSCALL sys_lseek,         SYS_lseek
SYSCALL sys_ioctl,         SYS_ioctl
SYSCALL sys_dup,           SYS_dup
SYSCALL sys_fcntl,         SYS_fcntl

# Filesystem Management
SYSCALL sys_getcwd,        SYS_getcwd
SYSCALL sys_chdir,         SYS_chdir
SYSCALL sys_fchdir,        SYS_fchdir
SYSCALL sys_chroot,        SYS_chroot
SYSCALL sys_getdents,      SYS_getdents

# File Stat/Access/Mode
SYSCALL sys_fstat,         SYS_fstat
SYSCALL sys_fstatat,       SYS_fstatat
SYSCALL sys_faccessat,     SYS_faccessat
SYSCALL sys_fchmod,        SYS_fchmod
SYSCALL sys_fchmodat,      SYS_fchmodat
SYSCALL sys_fchown,        SYS_fchown
SYSCALL sys_fchownat,      SYS_fchownat

# Memory
SYSCALL sys_brk,           SYS_brk
SYSCALL sys_mmap,          SYS_mmap
SYSCALL sys_munmap,        SYS_munmap
SYSCALL sys_mremap,        SYS_mremap

# Time
SYSCALL sys_gettimeofday,  SYS_gettimeofday
SYSCALL sys_time,          SYS_time

# Legacy / Semihosting handling
SYSCALL sys_open,          SYS_open
SYSCALL sys_openat,        SYS_openat
SYSCALL sys_link,          SYS_link
SYSCALL sys_unlink,        SYS_unlink
SYSCALL sys_unlinkat,      SYS_unlinkat
SYSCALL sys_mkdir,         SYS_mkdir
SYSCALL sys_mkdirat,       SYS_mkdirat
SYSCALL sys_access,        SYS_access
SYSCALL sys_stat,          SYS_stat
SYSCALL sys_lstat,         SYS_lstat

# Note: SYS_chmod and SYS_chown were not explicitly defined in the provided 
# C headers but requested in the assembly list. If your kernel supports them,
# ensure the ID matches. Otherwise, libc often implements them via fchmodat/fchownat.
# Generating stubs using the logic defined above.
SYSCALL sys_chmod,         SYS_chmod
SYSCALL sys_chown,         SYS_chown

# -----------------------------------------------------------------------------
# Direct wrappers (no sys_ prefix) - Commonly needed by Newlib/GLIBC
# -----------------------------------------------------------------------------
SYSCALL ioctl,   SYS_ioctl
SYSCALL brk,     SYS_brk
SYSCALL dup,     SYS_dup
SYSCALL kill,    SYS_kill
SYSCALL open,    SYS_open
SYSCALL close,   SYS_close
SYSCALL read,    SYS_read
SYSCALL write,   SYS_write
SYSCALL lseek,   SYS_lseek
SYSCALL fstat,   SYS_fstat
SYSCALL exit,    SYS_exit