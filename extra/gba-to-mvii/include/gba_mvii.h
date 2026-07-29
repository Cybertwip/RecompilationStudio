// gba_mvii.h — the C ABI of the emulation core.
//
// The core is extra/gbarecomp: a static ARM7TDMI-to-C++ recompiler and the
// runtime its output calls into. Its libraries are compiled for Cortex-A7 and
// linked straight into this .virtua (see ../CMakeLists.txt); src/core/
// implements the functions below on top of them. Everything on this side of the
// line is MVII: devices, scheduling, files. Nothing crosses it but the calls
// here.
//
// Contract notes that are not obvious from the signatures:
//
//   * Execution is stepped, not framed. gba_mvii_run_steps() runs a bounded
//     number of guest instructions and returns; the caller yields to MVII
//     between calls. There is no run_frame() on purpose — the kernel's
//     cooperative round is 120 Hz and must not be hostage to the emulated
//     frame rate.
//
//   * The ROM buffer belongs to the core from the start. Ask for it with
//     gba_mvii_rom_alloc(), read the file straight into it, and hand it to
//     gba_mvii_create(), which takes ownership. Reading into a buffer of our
//     own and copying would peak at twice the cartridge size, and a 16 MB
//     cartridge against a 32 MB guest heap does not have that to spare. The
//     bus keeps the pointer rather than copying, so it stays live for as long
//     as the machine does.
//
//   * The frame is RGB888, three bytes per pixel in R,G,B order. That is what
//     gbarecomp's PPU writes (src/runtime/color_lut.h, to_rgb888), and it is
//     the same channel order as the RGBA8 surface /dev/fb0 hands out, so
//     presenting is a 3-to-4 byte widen with no channel shuffling and no
//     5-to-8 expansion on the frame path.
//
//   * ONE machine at a time. gbarecomp's runtime is a process-wide singleton
//     (g_cpu, the active bus/PPU, the dispatch tables), so a second
//     gba_mvii_create() before gba_mvii_destroy() returns NULL rather than
//     quietly aliasing the first machine's state.
//
// The library also *calls* three functions that this side must define:
// gba_mvii_diag_write(), gba_mvii_host_epoch() and gba_mvii_host_now_us().
// See mvii_platform.cpp.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque machine handle.
typedef struct GbaMvii GbaMvii;

// Bumped whenever anything below changes shape. Check it at startup: a stale
// object that links but disagrees about a signature fails far more quietly
// than one that refuses to start.
// 2: core moved from the Rust port to extra/gbarecomp, and the frame changed
//    from BGR555 to the PPU's native RGB888.
#define GBA_MVII_ABI_VERSION 2u

uint32_t gba_mvii_abi_version(void);

uint32_t gba_mvii_screen_width(void);
uint32_t gba_mvii_screen_height(void);
uint32_t gba_mvii_audio_rate(void);

// ── cartridge ──────────────────────────────────────────────────────────────

// Storage for a len-byte ROM, owned by the core. NULL on failure.
uint8_t* gba_mvii_rom_alloc(size_t len);
// Release a buffer that never reached gba_mvii_create().
void gba_mvii_rom_free(uint8_t* ptr, size_t len);

// Take ownership of a gba_mvii_rom_alloc() buffer and build the machine.
// Ownership transfers even when the call returns NULL.
//
// With no BIOS image the machine boots HLE: gbarecomp synthesizes the state the
// real BIOS leaves at cart handoff and jumps to the cartridge entry point. SWIs
// the HLE layer implements are serviced in the runtime; the rest still dispatch
// into the recompiled BIOS, which is only present when one was recompiled into
// this build. Prefer gba_mvii_create_with_bios.
GbaMvii* gba_mvii_create(uint8_t* rom, size_t len);
// As above, but runs a real BIOS image. `bios` is copied, must be exactly
// 16384 bytes, and is SHA-1 checked against the canonical dump — a wrong image
// is refused here rather than misbehaving thousands of frames later. Returns
// NULL (having taken the ROM) if the check fails.
GbaMvii* gba_mvii_create_with_bios(uint8_t* rom, size_t len,
                                   const uint8_t* bios, size_t bios_len);
// Bytes a real BIOS image must be, so the caller can size its read and reject
// an obviously-wrong file before allocating.
size_t gba_mvii_bios_size(void);
void gba_mvii_destroy(GbaMvii* machine);

// The 12-byte header title at 0xA0, NUL-terminated, non-printables replaced.
// Returns the length written, excluding the terminator.
size_t gba_mvii_rom_title(GbaMvii* machine, uint8_t* out, size_t cap);

// ── execution ──────────────────────────────────────────────────────────────

// KEYINPUT, active low: a set bit means released, 0x3FF means nothing held.
void gba_mvii_set_keys(GbaMvii* machine, uint16_t keyinput);

// Run at most `steps` instructions, stopping early when a frame completes.
// Returns 1 if a completed frame is waiting to be presented.
uint32_t gba_mvii_run_steps(GbaMvii* machine, uint32_t steps);

#ifdef GBA_MVII_NATIVE_AOT
// The statically recompiled path: every dispatch runs a gbarecomp-generated
// native function. Returns 0 for bounded progress, 1 when a frame completed,
// and 2 when this call bridged at least one guest PC that the static corpus
// does not cover.
//
// 2 is a report, not a stop. gbarecomp's doctrine on a dispatch miss is honest
// self-healing: the missed subtree is executed by the reference interpreter,
// recorded, and fed back as a reviewed recompiler-config proposal — never
// silently absorbed and never fatal. The handheld has no compiler, so the
// on-device half of that loop is the recording; the caller should name the
// address and keep playing, because the alternative is killing a running game
// over a gap that the next recompile closes.
uint32_t gba_mvii_run_native_blocks(GbaMvii* machine, uint32_t blocks);
uint32_t gba_mvii_native_block_count(void);
// Guest address with bit 0 = Thumb for the most recent coverage miss.
uint32_t gba_mvii_native_miss_key(void);
// Distinct guest PCs bridged this session, and guest instructions interpreted
// across all of them. Both are 0 on a fully static run — which is the only
// result that counts as covered.
uint32_t gba_mvii_native_miss_count(void);
uint64_t gba_mvii_native_interpreted_insns(void);
#endif

// Acknowledge that frame, after presenting it.
void gba_mvii_frame_consume(GbaMvii* machine);

uint64_t gba_mvii_frames(GbaMvii* machine);
uint64_t gba_mvii_cycles(GbaMvii* machine);

// Liveness snapshot, for when the runtime is executing and drawing nothing.
// The handheld's only channel back is a log line, and three very different
// faults look identical from outside: a clock that never advances, a clock
// that advances while the PPU never completes a pass, and a guest halted on an
// interrupt that never arrives. These twelve words tell them apart.
//
// 0 pc, 1 cpsr, 2/3 clock lo/hi, 4 frames, 5 DISPCNT, 6 scanline, 7 flags,
// 8 IE, 9 IF, 10 sp, 11 lr.
#define GBA_MVII_PROBE_WORDS 12u
enum {
    GBA_MVII_PROBE_HALTED      = 1u << 0,
    GBA_MVII_PROBE_FRAME_READY = 1u << 1,
    GBA_MVII_PROBE_THUMB       = 1u << 2,
    GBA_MVII_PROBE_REAL_BIOS   = 1u << 3,
    GBA_MVII_PROBE_IRQ_LINE    = 1u << 4,
    GBA_MVII_PROBE_IRQ_PENDING = 1u << 5,
    GBA_MVII_PROBE_IME         = 1u << 6,
};
void gba_mvii_probe(GbaMvii* machine, uint32_t* out /* [12] */);

// ── video ──────────────────────────────────────────────────────────────────

// 240x160 RGB888 — three bytes per pixel in R,G,B order, row-major, tightly
// packed (115200 bytes). NULL before the first completed frame.
//
// This is the PPU's own latched frame: gbarecomp renders each visible scanline
// as it is reached and latches the result at VBlank start, so what comes back
// is the image as of the end of the visible period, not whatever VRAM holds by
// the time we get around to looking. Valid until the next run call.
const uint8_t* gba_mvii_framebuffer(GbaMvii* machine);

// ── audio ──────────────────────────────────────────────────────────────────

// Move up to `cap` interleaved stereo samples out and drop them from the
// queue; returns how many moved (always even — L and R move together). Call it
// every frame even with no DAC, because the queue behind it is bounded and
// letting it fill costs the mixer work with nowhere to put the result.
//
// gbarecomp's mixer is mono, so each of its samples is written to both
// channels. Duplicating here rather than opening the DAC single-channel keeps
// the device configuration the same whatever the core does with stereo later.
size_t gba_mvii_drain_audio(GbaMvii* machine, int16_t* out, size_t cap);

// ── backup medium ──────────────────────────────────────────────────────────

enum {
    GBA_MVII_BACKUP_NONE = 0,
    GBA_MVII_BACKUP_SRAM = 1,
    GBA_MVII_BACKUP_FLASH64 = 2,
    GBA_MVII_BACKUP_FLASH128 = 3,
    GBA_MVII_BACKUP_EEPROM = 4,
};

uint32_t gba_mvii_backup_kind(GbaMvii* machine);
size_t gba_mvii_save_size(GbaMvii* machine);
size_t gba_mvii_save_read(GbaMvii* machine, uint8_t* out, size_t cap);
size_t gba_mvii_save_load(GbaMvii* machine, const uint8_t* data, size_t len);
// FNV-1a over the save medium — the cheap "did anything change?" that decides
// whether a flush to eMMC is worth stalling the machine for.
uint64_t gba_mvii_save_hash(GbaMvii* machine);

// ── profiling ──────────────────────────────────────────────────────────────

// Drain the core's cost counters, accumulated since the previous take:
//
//   out[0]  microseconds inside the PPU scanline renderer
//   out[1]  scanlines rendered
//   out[2]  audio samples produced
//   out[3]  guest instructions executed by the self-heal interpreter bridge
//
// out[0]/out[1] are the part of the per-frame cost that does NOT scale with how
// much guest code ran, and they are invisible to the caller's own timers
// because they happen under the run calls. out[3] is zero on a fully static
// run; anything else is the interpreter making up for a gap in the recompiled
// corpus, and it is the first thing to look at when frame time is wrong.
//
// There is deliberately no "microseconds generating audio" figure: gbarecomp's
// mixer is not instrumented, and a zero there would be indistinguishable from
// free.
//
// Call calibrate once before the first emulated frame. It measures the host
// clock's own cost, arms the renderer timing (which is off by default and
// costs two clock reads per visible line), zeroes the counters, and returns
// the measured cost in ns/read.
uint32_t gba_mvii_prof_calibrate(void);
void gba_mvii_prof_take(uint32_t* out /* [4] */);

// ── hooks this side must provide ───────────────────────────────────────────

// Diagnostic text from the core (its eprintln!). Not NUL-terminated.
void gba_mvii_diag_write(const uint8_t* ptr, size_t len);

// Local wall-clock time in seconds on a proleptic-Gregorian timeline, for the
// cartridge RTC. Negative means "no clock", and the core falls back to a fixed
// epoch.
int64_t gba_mvii_host_epoch(void);

// Monotonic microseconds, the same clock the runtime times frames with, for
// the profiling counters above.
uint64_t gba_mvii_host_now_us(void);

#ifdef __cplusplus
}  // extern "C"
#endif
