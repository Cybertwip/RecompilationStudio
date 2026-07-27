#pragma once

/*
 * Virtua user-space open() flag bits.
 *
 * These MUST match the Linux/x86_64 fcntl values that the rest of the stack
 * speaks: the posix-shim header clang is built against
 * (OS/MVII/Kernel/Shared/posix-shim/include/fcntl.h), the host launcher's
 * translateLinuxOpenFlags(), and the on-device kernel syscall shim.  An earlier
 * revision of this header invented its own bit layout (O_RDONLY=2, O_WRONLY=4,
 * O_CREAT=0x200, ...), which made Dash-built code disagree with clang and the
 * loader: clang would pass a Linux O_CREAT (0x40) that Dash's open() wrapper
 * did not recognise, so the variadic mode argument was dropped and freshly
 * created files (e.g. clang's object output) ended up mode 000.  Keep these in
 * lockstep with Linux so every component agrees on the wire format.
 */

#ifndef O_RDONLY
#define O_RDONLY    0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY    0x0001
#endif
#ifndef O_RDWR
#define O_RDWR      0x0002
#endif
#ifndef O_ACCMODE
#define O_ACCMODE   0x0003
#endif
#ifndef O_CREAT
#define O_CREAT     0x0040
#endif
/* POSIX spells the create flag without a trailing "E"; some portable code
 * expects O_CREATE.  Mirror O_CREAT so both spellings resolve identically. */
#ifndef O_CREATE
#define O_CREATE    O_CREAT
#endif
#ifndef O_EXCL
#define O_EXCL      0x0080
#endif
#ifndef O_NOCTTY
#define O_NOCTTY    0x0100
#endif
#ifndef O_TRUNC
#define O_TRUNC     0x0200
#endif
#ifndef O_APPEND
#define O_APPEND    0x0400
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK  0x0800
#endif
#ifndef O_NDELAY
#define O_NDELAY    O_NONBLOCK
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW  0x20000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC   0x80000
#endif
#ifndef AT_FDCWD
#define AT_FDCWD    (-100)
#endif

#include <sys/fcntl.h>
