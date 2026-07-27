// gba_mvii.h — the C ABI of the Rust emulation core.
//
// The emulator itself is the Rust port under rust/ (gba-core + armv4t, from
// extra/gba-rust). Everything on this side of the line is MVII: devices,
// scheduling, files. Nothing crosses it but the calls below.
//
// Contract notes that are not obvious from the signatures:
//
//   * Execution is stepped, not framed. gba_mvii_run_steps() runs a bounded
//     number of guest instructions and returns; the caller yields to MVII
//     between calls. There is no run_frame() on purpose — the kernel's
//     cooperative round is 120 Hz and must not be hostage to the emulated
//     frame rate.
//
//   * The ROM buffer belongs to Rust from the start. Ask for it with
//     gba_mvii_rom_alloc(), read the file straight into it, and hand it to
//     gba_mvii_create(), which takes ownership. Reading into a buffer of our
//     own and copying would peak at twice the cartridge size, and a 16 MB
//     cartridge against a 32 MB guest heap does not have that to spare.
//
//   * The frame is BGR555 (red in the LOW five bits) — the GBA's own format,
//     not the framebuffer's. Converting is this side's job.
//
// The library also *calls* two functions that this side must define:
// gba_mvii_diag_write() and gba_mvii_host_epoch(). See mvii_platform.cpp.

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
#define GBA_MVII_ABI_VERSION 1u

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
GbaMvii* gba_mvii_create(uint8_t* rom, size_t len);
// As above, but runs a real BIOS image instead of the HLE. `bios` is copied.
GbaMvii* gba_mvii_create_with_bios(uint8_t* rom, size_t len,
                                   const uint8_t* bios, size_t bios_len);
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

// 240x160 BGR555, row-major, tightly packed. Valid until the next
// gba_mvii_run_steps().
const uint16_t* gba_mvii_framebuffer(GbaMvii* machine);

// ── audio ──────────────────────────────────────────────────────────────────

// Move up to `cap` interleaved stereo samples out and drop them from the
// queue; returns how many moved. Call it every frame even with no DAC — the
// queue is unbounded.
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

// ── hooks this side must provide ───────────────────────────────────────────

// Diagnostic text from the core (its eprintln!). Not NUL-terminated.
void gba_mvii_diag_write(const uint8_t* ptr, size_t len);

// Local wall-clock time in seconds on a proleptic-Gregorian timeline, for the
// cartridge RTC. Negative means "no clock", and the core falls back to a fixed
// epoch.
int64_t gba_mvii_host_epoch(void);

#ifdef __cplusplus
}  // extern "C"
#endif
