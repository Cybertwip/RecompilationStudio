// horizon_image.h — turning an NRO/NSO into a mapped, relocated ARM32 image.
//
// Horizon does not ship ELF files. A module is an NRO0 or NSO0 container: three
// segments (text, rodata, data), a bss size, and -- the part that matters here
// -- a MOD0 header whose `dynamic_offset` points at an ordinary ELF `.dynamic`
// table. So the loading is in two halves:
//
//   1. container -> a flat image. Segment placement is explicit in the
//      container (NSO gives each segment a memory location and may LZ4-compress
//      it; NRO maps contiguously at its own file offsets), so there is no
//      program-header walk to do. That is this file.
//   2. .dynamic -> relocated code. Once `dynamic` is located, the work is
//      identical to any other ARM32 dynamic image, so it goes through
//      virtua-wine (vwine_image_scan_dynamic + vwine_elf_relocate) rather than
//      a second implementation.
//
// ── the 64-bit question ────────────────────────────────────────────────────
//
// Nearly all Switch software is AArch64, and a Cortex-A7 cannot execute a
// single AArch64 instruction. A 64-bit module is not a harder case here, it is
// a different project. A loader handed Horizon's own process metadata gets the
// answer for free -- it is one byte, `is_64bit` in struct horizon_hdr
// (Reference/horizon-linux/include/uapi/linux/horizon.h) -- but that structure
// is a handoff format a host loader synthesises, not something that travels
// with the program. A bare NRO/NSO, which is what Switch software ships as and
// the only input here, carries no such field. So
// horizon_image_probe_arch decides from the container itself and refuses by
// name. Two independent signals have to agree:
//
//   * the first instruction. Every module begins with an unconditional branch
//     to its entry: 0xEA...... in ARM32 (cond=AL, B), or (word >> 26) == 0x05
//     in AArch64. The encodings do not overlap.
//   * the shape of `.dynamic`. ARM32 uses 8-byte Elf32_Dyn entries and
//     DT_REL/DT_RELSZ; AArch64 uses 16-byte Elf64_Dyn entries and
//     DT_RELA/DT_RELASZ.
//
// If they disagree, the answer is "I do not know" and the load is refused --
// which is the point. Guessing here means mapping 64-bit code and branching
// into it, and the crash that follows says nothing about why.

#ifndef HORIZON_IMAGE_H
#define HORIZON_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "vwine/vwine_elf.h"
#include "vwine/vwine_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HORIZON_NRO_MAGIC 0x304F524Eu  // 'NRO0'
#define HORIZON_NSO_MAGIC 0x304F534Eu  // 'NSO0'
#define HORIZON_MOD0_MAGIC 0x30444F4Du // 'MOD0'

typedef enum horizon_container {
    HORIZON_CONTAINER_UNKNOWN = 0,
    HORIZON_CONTAINER_NRO,
    HORIZON_CONTAINER_NSO,
} horizon_container;

typedef enum horizon_arch {
    HORIZON_ARCH_UNKNOWN = 0,
    HORIZON_ARCH_ARM32,
    HORIZON_ARCH_AARCH64,
} horizon_arch;

// A loaded module. `image` is virtua-wine's view of it and is what
// vwine_elf_relocate / vwine_image_finalize / vwine_image_run_initializers
// operate on; the rest is Horizon-specific bookkeeping the SVC layer needs
// (QueryMemory has to be able to describe the code region, and the module
// object address is what a crash report would key on).
typedef struct horizon_module {
    vwine_image       image;
    horizon_container container;
    horizon_arch      arch;

    uintptr_t text_base;   size_t text_size;
    uintptr_t rodata_base; size_t rodata_size;
    uintptr_t data_base;   size_t data_size;   // includes .bss
    uintptr_t bss_base;    size_t bss_size;

    uintptr_t module_object;  // MOD0's module_offset, biased; 0 when absent
} horizon_module;

// Identify the container without parsing it. `size` is the whole blob.
horizon_container horizon_image_identify(const void* data, size_t size);
const char* horizon_container_text(horizon_container container);
const char* horizon_arch_text(horizon_arch arch);

// Decide whether a *decoded* image is ARM32 or AArch64.
//
// `image_base` is the flat image (post-decompression), `dynamic` its .dynamic
// table or NULL. Returns HORIZON_ARCH_UNKNOWN when the two signals disagree or
// neither is conclusive, having logged which one said what.
horizon_arch horizon_image_probe_arch(const uint8_t* image_base, size_t image_size,
                                      const void* dynamic);

// Parse, decompress, map and relocate `data` into `out`.
//
// Refuses -- with the reason logged and nothing mapped -- an AArch64 module, an
// unrecognised container, or a truncated one. On success the image is mapped
// and relocated but NOT yet executable and NOT yet initialised: the caller runs
// vwine_image_finalize and vwine_image_run_initializers, in that order, once it
// has decided the missing-import set is empty.
//
// `registry` and `missing` are handed straight to vwine_elf_relocate; a
// single-module homebrew NRO imports nothing and can pass an empty registry,
// but it must still pass `missing` and must still refuse the run if anything
// lands in it.
//
// Returns 0, or a negative errno.
int horizon_image_load(const void* data, size_t size, const char* name,
                       const vwine_registry* registry, vwine_missing_set* missing,
                       horizon_module* out);

void horizon_image_release(horizon_module* module);

// LZ4 block decompression, exposed because NSO segment decoding is the only
// caller and testing it separately is worth more than hiding it.
//
// Returns the number of bytes written, or -1 on any malformed input. Never
// writes past `dst_capacity` and never reads past `src + src_size`.
long horizon_lz4_decompress_block(const uint8_t* src, size_t src_size,
                                  uint8_t* dst, size_t dst_capacity);

#ifdef __cplusplus
}
#endif

#endif  // HORIZON_IMAGE_H
