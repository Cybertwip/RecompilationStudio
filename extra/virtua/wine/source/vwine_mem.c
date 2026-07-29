#include "vwine/vwine_mem.h"
#include "vwine/vwine_log.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

// Does [base, base+length) lie wholly inside the 32-bit address space the guest
// can name? Always true on the ARMv7 target; see the note at the use site.
static int vwine_fits_32bit(const void* base, size_t length)
{
    const uintptr_t start = (uintptr_t)base;
    const uintptr_t end = start + (uintptr_t)length;
    if (end < start) return 0;
    if (sizeof(void*) <= 4) return 1;
    return end <= (uintptr_t)0xFFFFFFFFu;
}

size_t vwine_page_round_up(size_t value)
{
    const size_t page = (size_t)VWINE_PAGE_SIZE;
    if (value == 0) return page;
    // Refuse the wrap rather than returning a small number for a huge request,
    // which would allocate a page and then be memcpy'd into for 4 GB.
    if (value > (size_t)-1 - (page - 1u)) return 0;
    return (value + (page - 1u)) & ~(page - 1u);
}

int vwine_map(size_t length, vwine_mapping* out)
{
    if (!out) return -EINVAL;
    out->base = NULL;
    out->length = 0;
    if (length == 0) return -EINVAL;

    const size_t rounded = vwine_page_round_up(length);
    if (rounded == 0) return -ENOMEM;

    // PROT_EXEC is requested up front even though the bytes are not written
    // yet. On MVII that costs nothing (the mapping is executable regardless)
    // and it keeps the request honest about what the range is for; the
    // coherency work is still done separately by vwine_make_executable, which
    // is the call that actually has to happen after the writes.
    const int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    void* base = mmap(NULL, rounded, prot, flags, -1, 0);
    if (base == MAP_FAILED || base == NULL) {
        const int saved = errno;
        vwine_logf("vwine: mmap(%zu) failed (errno %d)\n", rounded, saved);
        return saved > 0 ? -saved : -ENOMEM;
    }

    // The guest is a 32-bit image and every pointer inside it -- every
    // R_ARM_RELATIVE slot, every GOT entry, every function pointer it stores --
    // is 32 bits wide. So the image MUST live entirely below 4 GB, or those
    // stores silently truncate and the guest starts dereferencing addresses
    // with the top bits missing. On the device this is free: MVII is a 32-bit
    // ARMv7 kernel and there is no address above 4 GB to be handed. It costs
    // something only on a 64-bit host, which is where the loader's unit tests
    // run -- and a check that only ever fires under test is still the check
    // that documents the invariant the target relies on.
    if (sizeof(void*) > 4) {
        if (!vwine_fits_32bit(base, rounded)) {
            // Retry against hints in the low 4 GB. Not MAP_FIXED: that would
            // evict whatever already lives there. A hint the kernel declines is
            // simply another failed attempt.
            static const uintptr_t hints[] = {
                0x20000000u, 0x30000000u, 0x40000000u, 0x50000000u,
                0x60000000u, 0x70000000u, 0x10000000u,
            };
            const size_t hint_count = sizeof(hints) / sizeof(hints[0]);
            for (size_t i = 0; i < hint_count; ++i) {
                munmap(base, rounded);
                base = mmap((void*)hints[i], rounded, prot, flags, -1, 0);
                if (base == MAP_FAILED || base == NULL) { base = MAP_FAILED; continue; }
                if (vwine_fits_32bit(base, rounded)) break;
            }
            if (base == MAP_FAILED || base == NULL ||
                !vwine_fits_32bit(base, rounded)) {
                if (base != MAP_FAILED && base != NULL) munmap(base, rounded);
                vwine_logf("vwine: could not place %zu bytes below 4 GB; a "
                           "32-bit guest image cannot be mapped there\n", rounded);
                return -ENOMEM;
            }
        }
    }

    // The kernel's minos_user_mmap already zeroes, and so does the Dash-local
    // mmap in llvm_libc_stubs.cpp, but a loader that relies on someone else's
    // zeroing to produce a correct .bss is one refactor away from shipping
    // uninitialised guest globals. Cheap certainty.
    memset(base, 0, rounded);

    out->base = base;
    out->length = rounded;
    return 0;
}

void vwine_unmap(vwine_mapping* mapping)
{
    if (!mapping || !mapping->base || mapping->length == 0) return;
    (void)munmap(mapping->base, mapping->length);
    mapping->base = NULL;
    mapping->length = 0;
}

int vwine_make_executable(void* addr, size_t length)
{
    if (!addr || length == 0) return -EINVAL;

    // Round outward to page boundaries. mprotect is defined on whole pages, and
    // a segment that starts mid-page (which happens whenever a loader packs
    // several ELF segments into one allocation) would otherwise leave its first
    // bytes unsynced -- the exact partial-range failure that makes this class
    // of bug intermittent.
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end = start + (uintptr_t)length;
    if (end < start) return -EINVAL;
    start &= ~(uintptr_t)(VWINE_PAGE_SIZE - 1u);
    end = (end + (VWINE_PAGE_SIZE - 1u)) & ~(uintptr_t)(VWINE_PAGE_SIZE - 1u);

    if (mprotect((void*)start, (size_t)(end - start),
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        const int saved = errno;
        vwine_logf("vwine: mprotect(%p, %zu, RWX) failed (errno %d)\n",
                   (void*)start, (size_t)(end - start), saved);
        return saved > 0 ? -saved : -EACCES;
    }

    vwine_sync_icache((void*)start, (size_t)(end - start));
    return 0;
}

// The part that actually makes freshly written bytes safe to execute.
//
// A Cortex-A7 has a write-back D-cache and a separate I-cache with no coherency
// between them, so the loader's stores can still be sitting dirty in the data
// side while the instruction side holds whatever previously occupied those
// addresses. The architecturally required sequence is: clean each line to the
// point of unification, barrier, invalidate the matching I-cache lines, flush
// the branch predictor, then barrier again so the following instruction fetches
// see the new state.
//
// This is done here rather than left to the kernel's mprotect on purpose. The
// kernel does perform it (see the icache maintenance in the MVII mmap path),
// but a loader whose correctness depends on a permission call having a
// side effect it does not name in its contract is a loader that breaks the day
// someone optimises that path. Doing it explicitly costs one cache walk over an
// image we just finished writing, which is noise next to loading it.
void vwine_sync_icache(void* addr, size_t length)
{
    if (!addr || length == 0) return;

#if defined(__arm__)
    // Line size comes from CTR: bits 19:16 hold log2 of the minimum D-cache
    // line in words, bits 3:0 the same for the I-cache. Reading it beats
    // assuming 32 bytes -- too large a stride silently skips lines.
    uint32_t ctr = 0;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
    const size_t dline = (size_t)4u << ((ctr >> 16) & 0xFu);
    const size_t iline = (size_t)4u << (ctr & 0xFu);

    uintptr_t start = (uintptr_t)addr;
    const uintptr_t end = start + (uintptr_t)length;

    for (uintptr_t p = start & ~(uintptr_t)(dline - 1u); p < end; p += dline)
        __asm__ volatile("mcr p15, 0, %0, c7, c11, 1" :: "r"(p) : "memory");  // DCCMVAU
    __asm__ volatile("dsb ish" ::: "memory");

    for (uintptr_t p = start & ~(uintptr_t)(iline - 1u); p < end; p += iline)
        __asm__ volatile("mcr p15, 0, %0, c7, c5, 1" :: "r"(p) : "memory");   // ICIMVAU
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" :: "r"(0) : "memory");       // BPIALL
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
#elif defined(__GNUC__)
    // Host builds of the loader tests. Nothing here executes guest ARM code, so
    // this only has to be correct for the host's own sake.
    __builtin___clear_cache((char*)addr, (char*)addr + length);
#else
    (void)addr;
    (void)length;
#endif
}
