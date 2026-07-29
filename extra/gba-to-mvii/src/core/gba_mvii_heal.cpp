// gba_mvii_heal.cpp — see gba_mvii_heal.h.
//
// Implements gbarecomp's Stage-2 overlay surface for a target that has no
// compiler. Every function here is a truthful answer to a question about a tier
// that does not exist, plus the one thing the device CAN do: record.

#include "gba_mvii_heal.h"

#include "overlay_loader.h"

#include <cstdint>
#include <cstring>

// The recompiled dispatch tables, emitted by tools/gba_recompile and compiled
// into this .virtua. Declared rather than included because the generated header
// is a build artifact whose path depends on the game being packaged.
extern "C" const unsigned kDispatchTableLen;
extern "C" const unsigned kBiosDispatchTableLen;

namespace {

// Distinct missed PCs, in first-seen order. Fixed capacity because this runs on
// a 512 KB guest stack inside a cooperative scheduler, and because a run that
// overflows it has already failed the coverage gate many times over — the exact
// count past the cap is not the interesting number. Overflow is counted, not
// dropped silently.
constexpr uint32_t kMaxDistinct = 512;

uint32_t g_keys[kMaxDistinct];
uint32_t g_count      = 0;   // distinct keys held in g_keys
uint32_t g_overflow   = 0;   // distinct keys we had no room for
uint32_t g_last_key   = 0;

bool seen(uint32_t key) {
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_keys[i] == key) return true;
    }
    return false;
}

}  // namespace

namespace gbamvii {

void heal_reset() {
    g_count    = 0;
    g_overflow = 0;
    g_last_key = 0;
    std::memset(g_keys, 0, sizeof(g_keys));
}

uint32_t heal_miss_count()    { return g_count + g_overflow; }
uint32_t heal_last_miss_key() { return g_last_key; }

uint32_t static_entry_count() {
    return static_cast<uint32_t>(kDispatchTableLen) +
           static_cast<uint32_t>(kBiosDispatchTableLen);
}

}  // namespace gbamvii

// ── the Stage-2 surface ────────────────────────────────────────────────────

// Hot path, consulted on every static-table miss before the bridge. There is
// nothing healed here and there never will be, so this is the cheapest honest
// answer: no overlay ran, fall through to the interpreter bridge.
extern "C" int overlay_try_dispatch(uint32_t /*pc*/, int /*thumb*/) {
    return 0;
}

namespace gbarecomp {

void overlay_loader_init(const std::string& /*cache_root*/,
                         const std::string& /*image_sha1*/,
                         const gba::GbaBios* /*bios*/) {
    // Nothing to initialize: no cache directory, no worker thread, no compiler.
}

void overlay_loader_shutdown() {}

// Called from the Stage-1 miss path (runtime_arm_default_aborts.cpp) with the
// address and mode of the miss, immediately after it is logged. On a host this
// enqueues a background compile. Here it is the recording — the only part of
// the heal loop the handheld can run.
//
// Returning false says "not dispatchable now", which is the truth, and sends
// the caller into the interpreter bridge. The game keeps running.
bool overlay_request_compile(uint32_t pc, bool thumb) {
    const uint32_t key = (pc & ~1u) | (thumb ? 1u : 0u);
    g_last_key = key;
    if (!seen(key)) {
        if (g_count < kMaxDistinct) g_keys[g_count++] = key;
        else                        ++g_overflow;
    }
    return false;
}

void overlay_drain_ready() {
    // No worker, so nothing ever becomes ready.
}

bool overlay_query(uint32_t /*pc*/, bool /*thumb*/, uint64_t* native_calls) {
    if (native_calls) *native_calls = 0;
    return false;
}

uint64_t overlay_game_thread_compile_ns() { return 0; }

void overlay_counters(uint64_t* healed_native, uint64_t* native_calls_total,
                      uint64_t* inflight, uint64_t* failed) {
    // All zero, and honestly so: nothing healed, nothing is in flight, and
    // nothing FAILED to compile either — no compile was ever attempted. The
    // distinction matters, which is why the miss count lives in
    // heal_miss_count() and not in `failed`: a caller reading these numbers is
    // asking about the overlay tier, and this build has none.
    if (healed_native)      *healed_native      = 0;
    if (native_calls_total) *native_calls_total = 0;
    if (inflight)           *inflight           = 0;
    if (failed)             *failed             = 0;
}

bool overlay_enabled()     { return false; }
bool overlay_was_enabled() { return false; }

}  // namespace gbarecomp
