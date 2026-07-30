// horizon_svc.h — the guest's `svc` instructions, serviced.
//
// This is the seam. The guest's ARM code runs untranslated on the Cortex-A7
// right up until it executes `svc #imm`; that traps to MVII's vector, which
// hands the caller's register frame to the handler installed here, and this
// file turns that frame into a call to horizon_kernel.h. Nothing is rewritten,
// nothing is recompiled -- see minos_svc.h for why the guest runs in System
// mode and why that is what makes the trap survivable.
//
// The SVC numbers are the `svc` immediate. That is not an assumption: horizon-
// linux dispatches on `esr & ESR_ELx_xVC_IMM_MASK`
// (Reference/horizon-linux/arch/arm64/kernel/syscall.c), which is the same
// field MVII's vector decodes into minos_svc_frame::imm. The number-to-service
// table is transcribed from
// Reference/horizon-linux/arch/arm64/include/asm/horizon/unistd.h.
//
// ── ABI DERIVATIONS: read this before trusting a value ─────────────────────
//
// Horizon's SVC ABI is documented, implemented and tested for AArch64. The
// AArch32 form is not: horizon-linux has no 32-bit table at all -- its compat
// entry path is literally marked `// horizon TODO 32-bit syscalls` -- and there
// is no other in-tree implementation to check against. Every 32-bit specific
// decision below is therefore DERIVED, not transcribed, and each one is listed
// here with the reasoning and with what would prove it wrong. They are
// collected in one place on purpose: a derivation buried at its use site is
// indistinguishable from a fact.
//
//   D1. ARGUMENT REGISTERS. AArch64 passes SVC arguments in x0..x7 and returns
//       in x0 (Result) and x1.. (values). The derivation is the obvious
//       parallel: r0..r7, returning in r0 and r1.. . Falsified by: essentially
//       any call at all producing garbage in argument 1, which would be
//       immediate and total rather than subtle.
//
//   D2. 64-BIT ARGUMENTS OCCUPY EVEN-ALIGNED REGISTER PAIRS, low word first.
//       This is the AAPCS rule, which is what a 32-bit Horizon toolchain's
//       hand-written SVC stubs would have been written against. It only
//       CHANGES anything for three services -- svcWaitSynchronization,
//       svcWaitProcessWideKeyAtomic and svcGetInfo -- where the 64-bit value
//       lands after an odd number of preceding words: aligned puts it in
//       r4:r5, dense packing would put it in r3:r4. Everywhere else (sleep
//       timeouts in r0:r1, core masks in r2:r3) the two rules agree, so the
//       exposure is narrow and named. Falsified by: a timeout that should be
//       milliseconds arriving as an astronomical or negative figure. Those
//       three call sites log their raw r0..r7 the FIRST time each is reached,
//       exactly once per process, so a real guest run settles it -- see
//       log_ambiguous_once.
//
//   D3. MemoryInfo IS THE ILP32 FORM: 32-bit addr and size, not the 64-bit
//       fields an AArch64 guest sees. A 32-bit process cannot address more
//       than 32 bits, and the structure is the guest's own type -- the kernel
//       writes whatever that process's headers declare. Field ORDER follows
//       Reference/horizon-linux/kernel/horizon/sys.c (addr, size, state, attr,
//       perm, ipc_refcount, device_refcount, padding); note that libnx
//       documents the two refcounts in the opposite order, and horizon-linux
//       is the in-tree source so it wins. Falsified by: a guest walking the
//       address space with svcQueryMemory failing to terminate, because it
//       would be reading a size out of the wrong word.
//
// Nothing here silently absorbs an unknown. An SVC this file does not
// implement logs its number and its whole frame and terminates the guest,
// because the alternative -- returning a plausible Result for a service that
// did not happen -- produces a guest that misbehaves later, somewhere else,
// for no visible reason.

#ifndef HORIZON_SVC_H
#define HORIZON_SVC_H

#include <stdbool.h>

#include "minos_svc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the handler with MVII. Must be called before any guest code runs;
// with no handler installed the vector is the fatal trap it has always been.
//
// Returns false with the reason logged (an MVII too old to have the hook, or a
// non-ARM host, where the whole mechanism is meaningless).
bool horizon_svc_install(void);

// Remove it again. Safe to call when nothing was installed.
void horizon_svc_uninstall(void);

// The handler itself. Public only so it can be tested directly with a
// synthesised frame; the guest reaches it through the vector.
void horizon_svc_handler(minos_svc_frame* frame);

// Name of an SVC number, or NULL when it is not one Horizon defines. Used in
// refusal messages so an unimplemented service is reported by name rather than
// as a bare number.
const char* horizon_svc_name(unsigned number);

#ifdef __cplusplus
}
#endif

#endif  // HORIZON_SVC_H
