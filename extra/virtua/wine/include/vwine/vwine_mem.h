// vwine_mem.h — guest memory for the Virtua in-process guest-OS layer.
//
// The whole reason this file is short is the MVII memory model. There is one
// flat identity map (OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers/mt6592_mmu.c
// installs a 1:1 short-descriptor section table), DRAM is mapped
// SECTION_NORMAL_WBWA with XN=0, and there are no per-process page tables. So:
//
//   * every byte a guest can address is already executable — there is no W^X to
//     defeat, no dual RW/RX alias to maintain, no "JIT region" to negotiate;
//   * `prot` is therefore bookkeeping, not enforcement. We record it because the
//     guest's own loader asks questions about it, not because the MMU will;
//   * what IS real, and what this file exists for, is CACHE COHERENCY. A
//     Cortex-A7 has a write-back D-cache and a separate I-cache with no
//     hardware coherency between them. Bytes freshly written through the data
//     side are not visible to instruction fetch until they are cleaned to the
//     point of unification and the I-cache lines are invalidated. Skip that and
//     you get the worst class of bug there is: it works, until the address you
//     land on happens to still be cached, and then it executes something else.
//
// So `vwine_map` is an allocator and `vwine_make_executable` is the barrier
// that matters. Callers that write guest code MUST call the latter before
// branching to it. There is no path where that is optional.
//
// Everything here goes through the POSIX shim (OS/MVII/Kernel/Shared/posix-shim)
// rather than the raw minos_user_abi vtable, for the same reason
// gba-to-mvii's runtime does: the shim is the supported surface, it is what
// PowerEngine keeps working, and reaching around it means re-deriving errno
// conventions by hand.

#ifndef VWINE_MEM_H
#define VWINE_MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One contiguous guest allocation. `base` is page-aligned and `length` is the
// rounded-up byte count actually reserved, which is what must be handed back to
// vwine_unmap — not the length originally requested.
typedef struct vwine_mapping {
    void*  base;
    size_t length;
} vwine_mapping;

// MVII's page granularity. Matches kUserMapPageSize in the kernel's
// minos_user_mmap so a guest asking for one page gets exactly one page.
#define VWINE_PAGE_SIZE 4096u

size_t vwine_page_round_up(size_t value);

// Reserve `length` bytes of readable/writable guest memory, zeroed.
//
// Returns 0 on success and a negative errno on failure, leaving *out zeroed.
// The memory is NOT yet safe to execute even though the MMU would permit it —
// see the note above, and call vwine_make_executable once the bytes are final.
int vwine_map(size_t length, vwine_mapping* out);

// Release a mapping obtained from vwine_map. Safe on a zeroed mapping.
void vwine_unmap(vwine_mapping* mapping);

// Make `length` bytes at `addr` safe to fetch as instructions.
//
// This is the D-cache-clean / I-cache-invalidate / branch-predictor-flush
// sequence, reached through mprotect(PROT_EXEC) so the kernel performs it at
// PL1 over the exact range. Call it after the LAST write to the range: after
// relocations are applied, after import thunks are patched, after any
// self-modification. Calling it twice is merely wasteful; calling it zero times
// is a latent, address-dependent crash.
//
// Returns 0 on success, negative errno otherwise. A failure here is fatal to
// the guest and must not be ignored — executing anyway is exactly the bug this
// prevents.
int vwine_make_executable(void* addr, size_t length);

// Make bytes just written through the data side safe to fetch as instructions:
// clean-to-PoU, invalidate I-cache, flush branch predictor, barrier.
//
// vwine_make_executable already calls this. It is exposed separately for the
// case that has no permission change at all -- patching a already-live image,
// such as a trampoline rewritten after the guest is running.
void vwine_sync_icache(void* addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif  // VWINE_MEM_H
