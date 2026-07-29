#include "vwine/vwine_registry.h"
#include "vwine/vwine_log.h"

#include <string.h>

void* vwine_registry_resolve_nid(const vwine_registry* registry,
                                 uint32_t library_nid, uint32_t function_nid)
{
    if (!registry || !registry->libraries) return NULL;
    for (size_t i = 0; i < registry->library_count; ++i) {
        const vwine_library* lib = &registry->libraries[i];
        // A library NID of 0 in the request means "search everywhere"; a
        // library NID of 0 in the entry means the library is name-keyed and
        // cannot be excluded by NID, so it stays in the search either way.
        if (library_nid && lib->nid && lib->nid != library_nid) continue;
        if (!lib->exports) continue;
        for (size_t j = 0; j < lib->export_count; ++j) {
            if (lib->exports[j].nid == function_nid && lib->exports[j].address)
                return lib->exports[j].address;
        }
    }
    return NULL;
}

void* vwine_registry_resolve_name(const vwine_registry* registry,
                                  const char* library_name, const char* symbol)
{
    if (!registry || !registry->libraries || !symbol) return NULL;
    for (size_t i = 0; i < registry->library_count; ++i) {
        const vwine_library* lib = &registry->libraries[i];
        if (library_name && lib->name && strcmp(lib->name, library_name) != 0)
            continue;
        if (!lib->exports) continue;
        for (size_t j = 0; j < lib->export_count; ++j) {
            const vwine_export* e = &lib->exports[j];
            if (e->name && e->address && strcmp(e->name, symbol) == 0)
                return e->address;
        }
    }
    return NULL;
}

const char* vwine_registry_name_for_nid(const vwine_registry* registry,
                                        uint32_t library_nid,
                                        uint32_t function_nid)
{
    if (!registry || !registry->libraries) return NULL;
    for (size_t i = 0; i < registry->library_count; ++i) {
        const vwine_library* lib = &registry->libraries[i];
        if (library_nid && lib->nid && lib->nid != library_nid) continue;
        if (!lib->exports) continue;
        for (size_t j = 0; j < lib->export_count; ++j) {
            if (lib->exports[j].nid == function_nid)
                return lib->exports[j].name;
        }
    }
    return NULL;
}

// ── unresolved-import accounting ───────────────────────────────────────────

void vwine_missing_reset(vwine_missing_set* set)
{
    if (!set) return;
    set->count = 0;
    set->total = 0;
}

void vwine_missing_add(vwine_missing_set* set, const char* library,
                       const char* symbol, uint32_t library_nid,
                       uint32_t function_nid)
{
    if (!set) return;
    ++set->total;

    // Deduplicate. One missing symbol imported from four translation units in
    // the guest is one thing to implement, and a report that says so four times
    // buries the other three misses.
    for (size_t i = 0; i < set->count; ++i) {
        const vwine_missing_import* e = &set->entries[i];
        if (function_nid && e->function_nid == function_nid &&
            e->library_nid == library_nid)
            return;
        if (!function_nid && symbol && e->symbol && strcmp(e->symbol, symbol) == 0)
            return;
    }

    // Past the cap we keep counting but stop storing: `total` still tells the
    // truth about how much is missing, which is the number that decides whether
    // this title is close or nowhere near.
    if (set->count >= VWINE_MAX_REPORTED_MISSING) return;

    vwine_missing_import* e = &set->entries[set->count++];
    e->library = library;
    e->symbol = symbol;
    e->library_nid = library_nid;
    e->function_nid = function_nid;
}

void vwine_missing_report(const vwine_missing_set* set, const char* image_name)
{
    if (!set || set->total == 0) return;

    vwine_logf("vwine: %s needs %zu import%s this build does not implement:\n",
               image_name ? image_name : "the guest image", set->total,
               set->total == 1 ? "" : "s");
    for (size_t i = 0; i < set->count; ++i) {
        const vwine_missing_import* e = &set->entries[i];
        if (e->symbol && e->function_nid) {
            vwine_logf("  %s::%s (NID 0x%08x)\n",
                       e->library ? e->library : "?", e->symbol,
                       (unsigned)e->function_nid);
        } else if (e->symbol) {
            vwine_logf("  %s::%s\n", e->library ? e->library : "?", e->symbol);
        } else {
            // The usual Vita case: the module names a NID we have never seen,
            // so there is no name to print. The library NID still identifies
            // which module to go implement, and the function NID is directly
            // searchable against the public NID databases.
            vwine_logf("  library 0x%08x function 0x%08x\n",
                       (unsigned)e->library_nid, (unsigned)e->function_nid);
        }
    }
    if (set->total > set->count) {
        vwine_logf("  ... and %zu more\n", set->total - set->count);
    }
}
