/*
 * MVII baremetal POSIX shim — <setjmp.h> wrapper.
 *
 * llvm-libc baremetal/RISC-V ships <setjmp.h> with the jmp_buf type but
 * does NOT declare setjmp/longjmp (the entrypoints are not enabled in
 * the baremetal entrypoint set, and the symbols are not in libc.a).
 *
 * We provide:
 *   - the prototypes here, and
 *   - a tiny RV64GC LP64D assembly implementation in src/posix_setjmp.S
 *
 * The jmp_buf layout MUST stay in sync with llvm-libc's __jmp_buf, namely:
 *   offset  0  : __pc        (long)
 *   offset  8  : __regs[12]  (12*long = s0..s11)
 *   offset 104 : __sp        (long)
 *   offset 112 : __fpregs[12](12*double = fs0..fs11)
 *   total      : 208 bytes
 */
#ifndef _POSIX_SHIM_SETJMP_H
#define _POSIX_SHIM_SETJMP_H

/* Pull in llvm-libc's <setjmp.h> first so jmp_buf is defined. The shim's
 * include path is searched BEFORE SYSTEM, so use #include_next to reach
 * the sysroot-provided header. */
#include_next <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

int  setjmp(jmp_buf env) __attribute__((__returns_twice__));
void longjmp(jmp_buf env, int val) __attribute__((__noreturn__));

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_SETJMP_H */
