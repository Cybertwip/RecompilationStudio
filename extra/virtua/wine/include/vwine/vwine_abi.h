// vwine_abi.h — the calling convention across the guest/host boundary.
//
// This header exists because running the guest NATIVELY, rather than
// interpreting it, makes the ARM procedure call standard a real problem instead
// of a detail an emulator would have absorbed.
//
// MVII builds every Virtua guest -mfloat-abi=soft: no FP instructions at all,
// and floating-point arguments passed in the core registers r0-r3 and on the
// stack. The guests we host were built the other way. VitaSDK targets
// `armv7-a -mfpu=neon -mfloat-abi=hard`, and AArch32 Horizon likewise, so their
// code passes float and double arguments in s0-s15/d0-d7 and expects results in
// s0/d0. Point a hard-float caller at a soft-float callee and the arguments are
// simply in the wrong registers: no fault, no diagnostic, just wrong numbers.
// For an integer/pointer API -- which is most of the kernel surface -- the two
// conventions are identical and nothing is wrong. It is exactly the calls
// carrying floats (sceGxmSetViewport, the matrix and audio-volume calls) that
// silently corrupt.
//
// The fix is per-function rather than per-file, and it is a supported clang/GCC
// feature on ARM: `__attribute__((pcs("aapcs-vfp")))` gives one function the
// VFP convention while the translation unit stays otherwise ordinary. That
// matters for linking, because it leaves the object's Tag_ABI_VFP_args at 0 --
// the same value soft-float objects carry -- so these modules still link
// against MVII's soft-float libc and libc++. Compiling a whole TU
// -mfloat-abi=hard would set the tag to 1 and ld.lld would refuse the link.
//
// The rule for anyone adding an HLE entry point:
//
//   * takes or returns float/double  ->  MUST be VWINE_GUEST_ABI, and the TU
//     must be compiled with a real FPU selected (-mfpu=neon-vfpv4
//     -mfloat-abi=softfp) or the attribute has no registers to name;
//   * integer and pointer only       ->  the attribute is harmless but
//     unnecessary; plain functions are already correct.
//
// When in doubt, apply it: on an integer-only signature it is a no-op.

#ifndef VWINE_ABI_H
#define VWINE_ABI_H

#if defined(__arm__)
#  if defined(__ARM_PCS_VFP)
// Already hard-float: the base convention for this TU is the one guests use, so
// the attribute would be redundant (and clang rejects pcs() that matches).
#    define VWINE_GUEST_ABI
#  else
#    define VWINE_GUEST_ABI __attribute__((pcs("aapcs-vfp")))
#  endif
#else
// Not ARM. The Vita and Horizon front-ends are ARMv7-only by construction --
// they execute guest ARM instructions natively, so there is no meaning to
// building them elsewhere -- but the registry and loader headers are also
// included by host-side unit tests, where this must simply disappear.
#  define VWINE_GUEST_ABI
#endif

#endif  // VWINE_ABI_H
