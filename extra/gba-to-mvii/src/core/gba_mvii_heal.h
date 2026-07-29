// gba_mvii_heal.h — the handheld's half of gbarecomp's self-healing loop.
//
// gbarecomp dispatches a guest PC in three tiers: the statically recompiled
// tables, then a Stage-2 tier of overlays it compiled at runtime, then the
// reference interpreter as a bridge. Stage 2 works by emitting C++, invoking a
// host compiler and dlopen'ing the result. MVII has no compiler and no dynamic
// loader for host code, so that tier cannot exist here.
//
// It does not follow that the tier can be left undefined. gbarecomp's doctrine
// (PRINCIPLES.md, "Honest self-healing" and "Coverage honesty is load-bearing")
// is that a miss is bridged, RECORDED and REPORTED — never silently absorbed.
// The interpreter bridge already works on-device. What is missing is the
// recording, and the miss path calls overlay_request_compile() at exactly the
// point where the (pc, thumb) of every miss is in hand. So this file implements
// the Stage-2 surface as an honest no-tier: it records, and it answers "nothing
// healed" to everything else, which is true. A build that reported healed
// overlays it never had would be worse than one with no tier at all.
//
// The counters below are what the device can send home. The other half of the
// loop — feeding the addresses back through the recompiler so the next build's
// static corpus covers them — happens on a workstation.

#pragma once

#include <cstdint>

namespace gbamvii {

// Clear the tally. Called once per machine bring-up.
void heal_reset();

// Distinct guest PCs bridged this session. Zero is the only value that means
// the static corpus covered the run.
uint32_t heal_miss_count();

// The most recently bridged guest address, with bit 0 set for Thumb. Zero when
// nothing has missed.
uint32_t heal_last_miss_key();

// Entries in the recompiled dispatch tables (cartridge + BIOS). A sanity check
// that the generated corpus is actually linked in: zero means the .virtua has
// no recompiled code at all and every dispatch will bridge.
uint32_t static_entry_count();

// Write the coverage verdict to the diagnostic log. Called at shutdown.
void report_coverage();

}  // namespace gbamvii
