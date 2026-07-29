// gba_mvii_core.cpp — gba_mvii.h implemented on extra/gbarecomp.
//
// This is the seam between the MVII front-end and the emulation core. Above it,
// main.cpp knows about /dev/fb0, cooperative yields and eMMC writes and nothing
// about the GBA. Below it, gbarecomp knows about the GBA and nothing about
// MVII. The file exists so that neither has to learn the other.
//
// What the core actually is: gbarecomp statically recompiles the cartridge's
// ARM7TDMI code to C++ ahead of time, and the generated functions are compiled
// for Cortex-A7 and linked into this same .virtua. Running the game is calling
// them — runtime_dispatch(pc) looks the guest PC up in the generated dispatch
// table and calls the native function for it. There is no per-instruction
// decode loop on the hot path.
//
// Two consequences shape everything below.
//
//   * The runtime is a process-wide singleton. g_cpu is one register file, the
//     bus/PPU bindings are one pair of pointers, the dispatch table is one
//     table. So is the machine: a second create() before destroy() returns
//     NULL rather than handing back a second handle onto the same state.
//
//   * A guest PC with no generated function is a COVERAGE gap, not an error.
//     gbarecomp's rule (PRINCIPLES.md, "Honest self-healing") is that the
//     missed subtree is bridged through the reference interpreter, recorded,
//     and reported so the next recompile can close it — loud, never silent,
//     never fatal. On a device with no compiler the on-device half of that loop
//     is the bridge and the record; see gba_mvii_heal.cpp.
//
// Faithfulness note: every ordering here is taken from gbarecomp's own runner
// (src/runtime/runtime.cpp, run_game) rather than reinvented — bus wiring
// before the active-bus install, runtime_init before the CPU reset, the halt
// pump chunked to the next device event, the frame boundary keyed on
// VBlank-start. Where a piece of that runner is host-only (CLI, TOML, the
// window, the overlay recompiler) it is absent here, not reimplemented.

#include "gba_mvii.h"

#include "gba_mvii_heal.h"

#include "gba_audio.h"
#include "gba_bios.h"
#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "gba_rom_header.h"
#include "gba_save.h"

#include "bios_hle.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "self_heal.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

// One line of diagnostics, routed to the front-end's log rather than stdio.
// Deliberately not variadic: everything this file has to say is a fixed string
// plus at most one number, and a printf on this side of the seam would need a
// vsnprintf that the freestanding build has no reason to carry.
void diag(const char* text) {
    if (!text) return;
    gba_mvii_diag_write(reinterpret_cast<const uint8_t*>(text),
                        std::strlen(text));
}

void diag_hex(const char* prefix, uint32_t value) {
    char buf[96];
    std::size_t n = 0;
    for (const char* p = prefix; *p && n < sizeof(buf) - 12; ++p) buf[n++] = *p;
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        const uint32_t nib = (value >> shift) & 0xFu;
        buf[n++] = static_cast<char>(nib < 10 ? '0' + nib : 'a' + (nib - 10));
    }
    buf[n++] = '\n';
    gba_mvii_diag_write(reinterpret_cast<const uint8_t*>(buf), n);
}

}  // namespace

// ── the machine ────────────────────────────────────────────────────────────
//
// Named struct rather than an opaque cookie into a static, because the front-end
// holds it across every call and a mismatched handle should be a type error
// here rather than a wild pointer later.

struct GbaMvii {
    gba::GbaBios bios;
    gba::GbaBus  bus;
    gba::GbaPpu  ppu;

    // Owned outright: gba_mvii_create() takes the caller's rom_alloc buffer,
    // and GbaBus::set_rom keeps the pointer instead of copying, so it has to
    // outlive every bus access.
    uint8_t*    rom     = nullptr;
    std::size_t rom_len = 0;

    gba::SaveType save_type   = gba::SaveType::Unknown;
    uint32_t      backup_kind = GBA_MVII_BACKUP_NONE;

    bool     frame_ready = false;
    uint64_t frames      = 0;

    // Mono-to-stereo staging for drain_audio. A member rather than a local so
    // the 512-byte read never lands on MVII's 512 KB guest stack.
    int16_t  mono[1024];

    // Sampled by gba_mvii_prof_take. Scanline figures are deltas against
    // gbarecomp's cumulative counters; samples are counted here because this is
    // where they leave the mixer.
    uint64_t prof_scanline_ns_base    = 0;
    uint64_t prof_scanline_count_base = 0;
    uint64_t prof_bridge_insns_base   = 0;
    uint64_t prof_samples             = 0;
};

namespace {

// The runtime underneath is a singleton, so this is too. See the header note.
GbaMvii* g_machine = nullptr;

uint32_t backup_kind_of(gba::SaveType type) {
    switch (type) {
        case gba::SaveType::SRAM:     return GBA_MVII_BACKUP_SRAM;
        case gba::SaveType::Flash512: return GBA_MVII_BACKUP_FLASH64;
        case gba::SaveType::Flash1M:  return GBA_MVII_BACKUP_FLASH128;
        case gba::SaveType::EEPROM:   return GBA_MVII_BACKUP_EEPROM;
        case gba::SaveType::Unknown:  break;
    }
    return GBA_MVII_BACKUP_NONE;
}

// Wire the save chip the cartridge header asks for. Getting this wrong is not
// a cosmetic failure: a Gen3 game whose flash never answers IdentifyFlash
// leaves gFlashMemoryPresent FALSE, and AgbMain blanks the screen rather than
// booting. Sizes follow gbarecomp's runner exactly.
void configure_backup(GbaMvii* m) {
    switch (m->save_type) {
        case gba::SaveType::SRAM:
            m->bus.save().configure_sram(32 * 1024);
            break;
        case gba::SaveType::EEPROM:
            m->bus.save().configure_eeprom(8 * 1024);
            break;
        case gba::SaveType::Flash512:
            m->bus.save().configure_flash(0x10000u);
            break;
        case gba::SaveType::Flash1M:
            m->bus.save().configure_flash(0x20000u);
            break;
        case gba::SaveType::Unknown:
            break;
    }
    m->backup_kind = backup_kind_of(m->save_type);
}

// Advance the master clock to the next scheduled device event, no further.
// Mirrors run_game's pump_idle: the horizon is the soonest of the PPU's next
// edge, the next timer overflow and the next audio sample, so nothing is
// stepped over. Returns the cycles actually consumed.
uint32_t pump_idle(GbaMvii* m, uint32_t max_cycles) {
    uint32_t chunk = m->ppu.cycles_until_next_event();
    const uint32_t until_timer  = m->bus.io().cycles_until_next_timer_event();
    const uint32_t until_sample = m->bus.audio().cycles_until_next_sample();
    if (until_timer  < chunk) chunk = until_timer;
    if (until_sample < chunk) chunk = until_sample;
    if (chunk == 0 || chunk == 0xFFFFFFFFu) chunk = 1;
    if (chunk > max_cycles) chunk = max_cycles;
    runtime_tick(chunk);
    return chunk;
}

// One unit of forward progress: either a chunk of the halt pump or one
// dispatch into recompiled code.
void step_once(GbaMvii* m) {
    if (m->bus.io().halted()) {
        pump_idle(m, gba::GbaPpu::kCyclesPerFrame);
        return;
    }
    runtime_dispatch(g_cpu.R[15]);
}

// Run until the budget is spent or the PPU reaches VBlank-start (scanline
// 159->160). VBlank-start, NOT the scanline wrap at 227->0: the wrap is a whole
// frame of game logic later, and keying presentation off it puts the displayed
// image a frame behind everything else that reads this machine.
bool run_until_frame(GbaMvii* m, uint32_t budget) {
    const unsigned long long start = g_runtime_vblank_starts;
    for (uint32_t i = 0; i < budget; ++i) {
        step_once(m);
        if (g_runtime_vblank_starts != start) {
            m->frame_ready = true;
            ++m->frames;
            return true;
        }
    }
    return false;
}

}  // namespace

// ── ABI ────────────────────────────────────────────────────────────────────

extern "C" {

uint32_t gba_mvii_abi_version(void) { return GBA_MVII_ABI_VERSION; }

uint32_t gba_mvii_screen_width(void)  { return gba::GbaPpu::kScreenWidth; }
uint32_t gba_mvii_screen_height(void) { return gba::GbaPpu::kScreenHeight; }
uint32_t gba_mvii_audio_rate(void)    { return gba::GbaAudio::kDefaultSampleRate; }

size_t gba_mvii_bios_size(void) { return gba::GbaBios::kSize; }

uint8_t* gba_mvii_rom_alloc(size_t len) {
    if (len == 0) return nullptr;
    return new (std::nothrow) uint8_t[len];
}

void gba_mvii_rom_free(uint8_t* ptr, size_t /*len*/) {
    delete[] ptr;
}

GbaMvii* gba_mvii_create_with_bios(uint8_t* rom, size_t len,
                                   const uint8_t* bios, size_t bios_len) {
    // Ownership transferred on entry, so every failure below frees the ROM.
    // The contract says so precisely because half these paths are error paths.
    if (!rom || len < 0xC0) {
        delete[] rom;
        return nullptr;
    }
    if (g_machine) {
        diag("gba-core: a machine already exists; the runtime is a singleton\n");
        delete[] rom;
        return nullptr;
    }

    GbaMvii* m = new (std::nothrow) GbaMvii();
    if (!m) {
        delete[] rom;
        return nullptr;
    }
    m->rom = rom;
    m->rom_len = len;

    // The real BIOS, hash-gated. A wrong image is refused here rather than
    // misbehaving thousands of frames downstream where nothing points back to
    // it. No BIOS is not an error: the machine boots HLE below.
    bool real_bios = false;
    if (bios && bios_len) {
        std::string err;
        if (m->bios.load_from_bytes(bios, bios_len,
                                    gba::GbaBios::kExpectedSha1, &err)) {
            real_bios = true;
        } else {
            diag("gba-core: BIOS rejected: ");
            diag(err.c_str());
            diag("\n");
            delete m;
            return nullptr;
        }
    }
    if (real_bios) m->bus.set_bios(&m->bios);

    m->bus.set_rom(m->rom, m->rom_len);

    const gba::RomHeader header = gba::parse_rom(m->rom, m->rom_len);
    m->save_type = header.save_type;
    configure_backup(m);

    m->bus.io().set_ppu(&m->ppu);
    m->bus.io().set_bus(&m->bus);

    // Order from run_game: bind the bus/PPU, hand the bus to the generated-code
    // runtime, and only then reset the CPU — runtime_init is what makes the
    // bus accessors the recompiled code calls point at anything.
    gbarecomp::set_active_bus(&m->bus);
    gbarecomp::set_active_ppu(&m->ppu);
    runtime_init(&m->bus);

    // Reset, replicating run_game's reset_recomp_cpu(). That function is file-
    // local to gbarecomp's own runner, which this build does not compile (it is
    // a CLI/TOML/window monolith); tools/test_rom_runner inlines the same body
    // for the same reason.
    for (int i = 0; i < 16; ++i) g_cpu.R[i] = 0;
    for (unsigned i = 0; i < ARM_BANK_COUNT; ++i) {
        g_cpu.banked_sp[i]   = 0;
        g_cpu.banked_lr[i]   = 0;
        g_cpu.banked_spsr[i] = 0;
    }
    for (int i = 0; i < 5; ++i) {
        g_cpu.r8_12_user[i] = 0;
        g_cpu.r8_12_fiq[i]  = 0;
    }
    g_cpu.R[13] = 0x03007FE0u;
    g_cpu.cpsr  = CPSR_I_BIT | CPSR_F_BIT | 0x13u;  // SVC, interrupts masked
    g_cpu.banked_sp[ARM_BANK_SUPERVISOR] = 0x03007FE0u;
    g_cpu.banked_sp[ARM_BANK_IRQ]        = 0x03007FA0u;
    g_cpu.banked_sp[ARM_BANK_USER]       = 0x03007F00u;

    gbarecomp::self_heal_reset();
    gbarecomp::self_heal_set_program_identity(header.game_title.c_str(),
                                              header.game_code.c_str(),
                                              m->bios.sha1_hex().c_str());
    gbamvii::heal_reset();

    if (real_bios) {
        // LLE: reset vector, the real boot ROM, logo and chime included. This
        // is the faithful path and the one every accuracy question is answered
        // against.
        gba::bios_hle_set_mode(gba::BiosHleMode::Off);
        g_cpu.R[15] = 0x00000000u;
        diag("gba-core: BIOS present — booting the real boot ROM\n");
    } else {
        // No BIOS image. Synthesize the state the boot ROM leaves at cart
        // handoff and enter at the cartridge entry point, with the HLE SWI
        // layer installed for the calls the game makes on the way. This is a
        // reduced machine and says so: SWIs gbarecomp's HLE does not implement
        // still dispatch into the recompiled BIOS, which is only linked in when
        // one was recompiled into this build.
        gba::bios_hle_set_mode(gba::BiosHleMode::On);
        gba::bios_hle_boot_skip(0x08000000u);
        diag("gba-core: no BIOS image — HLE boot, entering the cartridge "
             "directly\n");
    }

    g_machine = m;
    return m;
}

GbaMvii* gba_mvii_create(uint8_t* rom, size_t len) {
    return gba_mvii_create_with_bios(rom, len, nullptr, 0);
}

void gba_mvii_destroy(GbaMvii* machine) {
    if (!machine) return;
    runtime_shutdown();
    gbarecomp::set_active_bus(nullptr);
    gbarecomp::set_active_ppu(nullptr);
    delete[] machine->rom;
    machine->rom = nullptr;
    if (g_machine == machine) g_machine = nullptr;
    delete machine;
}

size_t gba_mvii_rom_title(GbaMvii* machine, uint8_t* out, size_t cap) {
    if (!machine || !out || cap == 0) return 0;
    // Header title field: 12 bytes at 0xA0, space-padded, not NUL-terminated.
    const std::size_t kOff = 0xA0, kLen = 12;
    std::size_t n = 0;
    if (machine->rom_len >= kOff + kLen) {
        for (std::size_t i = 0; i < kLen && n + 1 < cap; ++i) {
            const uint8_t c = machine->rom[kOff + i];
            out[n++] = (c >= 0x20 && c < 0x7F) ? c : ' ';
        }
        // Trailing padding is noise in a window title.
        while (n > 0 && out[n - 1] == ' ') --n;
    }
    out[n] = 0;
    return n;
}

void gba_mvii_set_keys(GbaMvii* machine, uint16_t keyinput) {
    if (!machine) return;
    machine->bus.io().set_keyinput(keyinput);
}

uint32_t gba_mvii_run_steps(GbaMvii* machine, uint32_t steps) {
    if (!machine || steps == 0) return 0;
    if (machine->frame_ready) return 1;
    return run_until_frame(machine, steps) ? 1u : 0u;
}

#ifdef GBA_MVII_NATIVE_AOT
uint32_t gba_mvii_run_native_blocks(GbaMvii* machine, uint32_t blocks) {
    if (!machine || blocks == 0) return 0;
    if (machine->frame_ready) return 1;

    const uint32_t before = gbamvii::heal_miss_count();
    const bool frame = run_until_frame(machine, blocks);

    // Coverage is reported ahead of the frame because a gap is the more urgent
    // of the two: the frame will still be there on the next call (frame_ready
    // latches), whereas a miss that is not surfaced the moment it happens gets
    // lost in the thousands of dispatches that follow.
    if (gbamvii::heal_miss_count() != before) return 2;
    return frame ? 1u : 0u;
}

uint32_t gba_mvii_native_block_count(void) {
    return gbamvii::static_entry_count();
}

uint32_t gba_mvii_native_miss_key(void) {
    return gbamvii::heal_last_miss_key();
}

uint32_t gba_mvii_native_miss_count(void) {
    return gbamvii::heal_miss_count();
}

uint64_t gba_mvii_native_interpreted_insns(void) {
    return gbarecomp::self_heal_interpreted_insns();
}
#endif

void gba_mvii_frame_consume(GbaMvii* machine) {
    if (machine) machine->frame_ready = false;
}

uint64_t gba_mvii_frames(GbaMvii* machine) {
    return machine ? machine->frames : 0;
}

uint64_t gba_mvii_cycles(GbaMvii* /*machine*/) {
    // The master clock is the runtime's, not the machine's — there is only one
    // of each, and runtime_tick is the single writer for both backends.
    return g_runtime_cycles;
}

void gba_mvii_probe(GbaMvii* machine, uint32_t* out) {
    if (!out) return;
    for (uint32_t i = 0; i < GBA_MVII_PROBE_WORDS; ++i) out[i] = 0;
    if (!machine) return;

    gba::GbaIo& io = machine->bus.io();
    const uint16_t ie  = io.read16(0x200);
    const uint16_t irf = io.read16(0x202);
    const uint16_t ime = io.read16(0x208);
    const bool pending = (ie & irf) != 0;

    uint32_t flags = 0;
    if (io.halted())                     flags |= GBA_MVII_PROBE_HALTED;
    if (machine->frame_ready)            flags |= GBA_MVII_PROBE_FRAME_READY;
    if (g_cpu.cpsr & CPSR_T_BIT)         flags |= GBA_MVII_PROBE_THUMB;
    if (machine->bios.loaded())          flags |= GBA_MVII_PROBE_REAL_BIOS;
    if (pending)                         flags |= GBA_MVII_PROBE_IRQ_PENDING;
    if (ime & 1u)                        flags |= GBA_MVII_PROBE_IME;
    // The line is only live when the CPU would actually take it: pending, IME
    // on, and CPSR.I clear. That distinction is the whole point of having both
    // bits — "an interrupt is waiting" and "the CPU can see it" are different
    // faults and look identical from outside without it.
    if (pending && (ime & 1u) && !(g_cpu.cpsr & CPSR_I_BIT))
        flags |= GBA_MVII_PROBE_IRQ_LINE;

    out[0]  = g_cpu.R[15];
    out[1]  = g_cpu.cpsr;
    out[2]  = static_cast<uint32_t>(g_runtime_cycles & 0xFFFFFFFFull);
    out[3]  = static_cast<uint32_t>(g_runtime_cycles >> 32);
    out[4]  = static_cast<uint32_t>(machine->frames);
    out[5]  = io.read16(0x000);            // DISPCNT
    out[6]  = machine->ppu.vcount();
    out[7]  = flags;
    out[8]  = ie;
    out[9]  = irf;
    out[10] = g_cpu.R[13];
    out[11] = g_cpu.R[14];
}

const uint8_t* gba_mvii_framebuffer(GbaMvii* machine) {
    if (!machine) return nullptr;
    if (!machine->ppu.has_latched_framebuffer()) return nullptr;
    return machine->ppu.latched_framebuffer();
}

size_t gba_mvii_drain_audio(GbaMvii* machine, int16_t* out, size_t cap) {
    if (!machine || !out || cap < 2) return 0;

    // Mono in, interleaved stereo out: half the caller's capacity, and never
    // more than the staging buffer holds.
    std::size_t want = cap / 2;
    if (want > sizeof(machine->mono) / sizeof(machine->mono[0]))
        want = sizeof(machine->mono) / sizeof(machine->mono[0]);

    const std::size_t got =
        machine->bus.audio().drain_samples(machine->mono, want);
    for (std::size_t i = 0; i < got; ++i) {
        out[i * 2 + 0] = machine->mono[i];
        out[i * 2 + 1] = machine->mono[i];
    }
    machine->prof_samples += got;
    return got * 2;
}

uint32_t gba_mvii_backup_kind(GbaMvii* machine) {
    return machine ? machine->backup_kind : GBA_MVII_BACKUP_NONE;
}

size_t gba_mvii_save_size(GbaMvii* machine) {
    if (!machine) return 0;
    gba::GbaSave& save = machine->bus.save();
    if (save.sram_enabled())   return save.sram_size();
    if (save.eeprom_enabled()) return save.eeprom_size();
    if (save.flash_enabled())  return save.flash_size();
    return 0;
}

size_t gba_mvii_save_read(GbaMvii* machine, uint8_t* out, size_t cap) {
    if (!machine || !out || cap == 0) return 0;
    gba::GbaSave& save = machine->bus.save();

    std::vector<uint8_t> bytes;
    if (save.sram_enabled())        bytes = save.sram_bytes();
    else if (save.eeprom_enabled()) bytes = save.eeprom_bytes();
    else if (save.flash_enabled())  bytes = save.flash_bytes();
    else return 0;

    const std::size_t n = bytes.size() < cap ? bytes.size() : cap;
    std::memcpy(out, bytes.data(), n);
    return n;
}

size_t gba_mvii_save_load(GbaMvii* machine, const uint8_t* data, size_t len) {
    if (!machine || !data || len == 0) return 0;
    gba::GbaSave& save = machine->bus.save();

    bool ok = false;
    if (save.sram_enabled()) {
        if (len > save.sram_size()) return 0;
        ok = save.load_sram_bytes(data, len);
    } else if (save.eeprom_enabled()) {
        if (len > save.eeprom_size()) return 0;
        ok = save.load_eeprom_bytes(data, len);
    } else if (save.flash_enabled()) {
        if (len > save.flash_size()) return 0;
        ok = save.load_flash_bytes(data, len);
    }
    if (!ok) return 0;

    // Loading is not the guest writing: leaving the dirty flag set would make
    // the front-end flush the file it just read back to eMMC.
    save.clear_dirty();
    return len;
}

uint64_t gba_mvii_save_hash(GbaMvii* machine) {
    if (!machine) return 0;
    gba::GbaSave& save = machine->bus.save();

    std::vector<uint8_t> bytes;
    if (save.sram_enabled())        bytes = save.sram_bytes();
    else if (save.eeprom_enabled()) bytes = save.eeprom_bytes();
    else if (save.flash_enabled())  bytes = save.flash_bytes();
    else return 0;

    uint64_t h = 1469598103934665603ull;  // FNV-1a 64, offset basis
    for (uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

uint32_t gba_mvii_prof_calibrate(void) {
    // Measure the host clock against itself. The renderer timing below reads it
    // twice per visible scanline — ~19200 reads a second — so a caller that
    // sees an implausible ns/read figure knows to distrust the split rather
    // than the emulator.
    constexpr uint32_t kReads = 256;
    const uint64_t t0 = gba_mvii_host_now_us();
    uint64_t sink = 0;
    for (uint32_t i = 0; i < kReads; ++i) sink += gba_mvii_host_now_us();
    const uint64_t t1 = gba_mvii_host_now_us();
    if (sink == 0) diag("gba-core: host clock reads returned zero\n");

    g_runtime_phase_prof = 1;
    if (g_machine) {
        g_machine->prof_scanline_ns_base    = g_prof_scanline_ns;
        g_machine->prof_scanline_count_base = g_prof_scanline_count;
        g_machine->prof_bridge_insns_base   =
            gbarecomp::self_heal_interpreted_insns();
        g_machine->prof_samples             = 0;
    }
    return static_cast<uint32_t>(((t1 - t0) * 1000ull) / kReads);
}

void gba_mvii_prof_take(uint32_t* out) {
    if (!out) return;
    for (int i = 0; i < 4; ++i) out[i] = 0;

    GbaMvii* m = g_machine;
    if (!m) return;

    const uint64_t ns    = g_prof_scanline_ns    - m->prof_scanline_ns_base;
    const uint64_t lines = g_prof_scanline_count - m->prof_scanline_count_base;
    const uint64_t insns = gbarecomp::self_heal_interpreted_insns() -
                           m->prof_bridge_insns_base;

    out[0] = static_cast<uint32_t>(ns / 1000ull);
    out[1] = static_cast<uint32_t>(lines);
    out[2] = static_cast<uint32_t>(m->prof_samples);
    out[3] = static_cast<uint32_t>(insns);

    m->prof_scanline_ns_base    = g_prof_scanline_ns;
    m->prof_scanline_count_base = g_prof_scanline_count;
    m->prof_bridge_insns_base   = gbarecomp::self_heal_interpreted_insns();
    m->prof_samples             = 0;
}

}  // extern "C"

// ── coverage reporting ─────────────────────────────────────────────────────

namespace gbamvii {

void report_coverage() {
    const uint32_t misses = heal_miss_count();
    if (misses == 0) {
        // The entry count is printed alongside the verdict, not instead of it,
        // because zero misses on its own is ambiguous: a build with an empty
        // dispatch table that never executed an instruction would also report
        // zero. Naming how much recompiled code was actually linked makes the
        // clean result self-evidencing rather than merely an absence.
        diag("gba-core: coverage FULLY STATIC — no guest PC was interpreted\n");
        diag_hex("gba-core:   recompiled dispatch entries: ", static_entry_count());
        return;
    }
    diag("gba-core: coverage NOT STATIC — the recompiled corpus has gaps\n");
    diag_hex("gba-core:   distinct guest PCs bridged: ", misses);
    diag_hex("gba-core:   most recent (bit0 = thumb): ", heal_last_miss_key());
    diag_hex("gba-core:   instructions interpreted:   ",
             static_cast<uint32_t>(gbarecomp::self_heal_interpreted_insns()));
    diag("gba-core:   feed these back through the recompiler config; a build "
         "with any of them left is not done\n");
}

}  // namespace gbamvii
