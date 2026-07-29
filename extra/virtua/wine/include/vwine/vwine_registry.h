// vwine_registry.h — what the guest's imports bind to.
//
// This is the WINE part of the design. The guest executable is native ARMv7
// code and runs as-is; what it cannot do is call its own operating system,
// because that operating system is not here. Every import it names has to
// resolve to a host function that reimplements the documented behaviour on MVII
// devices. This file is the table that decides which.
//
// Two naming schemes, because the two guests disagree:
//
//   * Vita imports are numeric. A module lists (library NID, function NID)
//     pairs -- NIDs being truncated SHA-1 hashes of the symbol name -- so
//     resolution is a pair of 32-bit comparisons and the human-readable name is
//     carried only for diagnostics.
//   * Horizon imports are ordinary ELF symbol names ("__nx_svc_..." and the
//     nnSdk C++ mangled names), so resolution is a string compare.
//
// A registry entry supplies whichever key its front-end uses and leaves the
// other zero/NULL.
//
// ── The rule that matters ──────────────────────────────────────────────────
//
// An unresolved import is a HARD FAILURE, reported by name, and never a stub.
//
// This is not a stylistic preference. The tempting alternative -- bind unknown
// imports to a function that returns 0 -- produces a guest that starts, runs,
// and then misbehaves somewhere far away from the missing call, with nothing in
// the log connecting the two. That is how you spend a week on a rendering bug
// that is actually an unimplemented memory-block allocator. Refusing at load
// time costs one line of output and names the exact symbol.
//
// vwine_registry_resolve therefore returns NULL for "not implemented", the
// loader collects every NULL, and the run is refused with the full list. A
// missing import is a to-do item with an address, not a runtime surprise.

#ifndef VWINE_REGISTRY_H
#define VWINE_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One host implementation of one guest symbol.
//
// `address` is the native ARMv7 entry point the guest will branch to. It is
// called with the guest's own convention, so anything with a float in its
// signature must have been declared VWINE_GUEST_ABI -- see vwine_abi.h.
typedef struct vwine_export {
    uint32_t    nid;      // Vita function NID; 0 when this entry is name-keyed.
    const char* name;     // Symbol name; NULL when this entry is NID-keyed.
                          // Always worth filling in for NID entries too: it is
                          // what the unresolved-import report prints.
    void*       address;
} vwine_export;

// A guest library: a namespace for the exports above.
typedef struct vwine_library {
    const char*         name;          // "SceLibKernel", "nn::os", ...
    uint32_t            nid;           // Vita library NID; 0 for name-keyed.
    const vwine_export* exports;
    size_t              export_count;
} vwine_library;

// The registry is assembled by the front-end at startup from its module list
// and stays constant for the life of the process. It is deliberately not
// mutable after guest entry: a guest that could add imports at runtime would
// defeat the load-time completeness check that is this file's whole point.
typedef struct vwine_registry {
    const vwine_library* libraries;
    size_t               library_count;
} vwine_registry;

// Resolve a Vita-style numeric import. Returns NULL when unimplemented.
//
// `library_nid` may be 0, in which case every library is searched for the
// function NID -- some modules import by function NID alone, and NIDs are wide
// enough that a collision across libraries is not a practical concern.
void* vwine_registry_resolve_nid(const vwine_registry* registry,
                                 uint32_t library_nid, uint32_t function_nid);

// Resolve a Horizon-style symbol name. Returns NULL when unimplemented.
//
// `library_name` may be NULL to search every library, which is the normal case
// for ELF symbol resolution: the guest names a symbol, not the module that
// should provide it.
void* vwine_registry_resolve_name(const vwine_registry* registry,
                                  const char* library_name, const char* symbol);

// Best-effort human name for a NID, for the unresolved-import report. Returns
// NULL when the registry has never heard of it, which is the common case and is
// exactly the information the report is conveying.
const char* vwine_registry_name_for_nid(const vwine_registry* registry,
                                        uint32_t library_nid,
                                        uint32_t function_nid);

// ── unresolved-import accounting ───────────────────────────────────────────
//
// Collected across a whole load rather than thrown on first miss, because the
// useful output is the entire list. Bring-up proceeds by implementing what this
// prints; stopping at the first one turns a single pass into twenty.
#define VWINE_MAX_REPORTED_MISSING 64

typedef struct vwine_missing_import {
    const char* library;   // may be NULL
    const char* symbol;    // may be NULL when only a NID is known
    uint32_t    library_nid;
    uint32_t    function_nid;
} vwine_missing_import;

typedef struct vwine_missing_set {
    vwine_missing_import entries[VWINE_MAX_REPORTED_MISSING];
    size_t               count;     // entries actually stored
    size_t               total;     // misses seen, including those past the cap
} vwine_missing_set;

void vwine_missing_reset(vwine_missing_set* set);
void vwine_missing_add(vwine_missing_set* set, const char* library,
                       const char* symbol, uint32_t library_nid,
                       uint32_t function_nid);

// Print the collected misses. Emits nothing when the set is empty, so callers
// can call it unconditionally.
void vwine_missing_report(const vwine_missing_set* set, const char* image_name);

#ifdef __cplusplus
}
#endif

#endif  // VWINE_REGISTRY_H
