// main.cpp — the GBA runtime MVII actually runs.
//
// What this replaces. The package generator used to emit a ~1 KB freestanding
// stub (extra/virtua/native/gba_native_main.c) that opened /dev/native0 and
// asked MVII to execute the cartridge itself. MVII has no such device — the
// _open dispatch in syscalls.cpp offers exactly /dev/input0, /dev/fb0,
// /dev/dac0, /dev/gpu0, /dev/net0, /dev/sysprefs and /dev/thermal0 — so the
// open failed, the stub returned 122, and the window opened and closed. There
// was never a GBA runtime on the device; there was a request for one.
//
// So this is the runtime. It is the gba++ interpreter core (armv4t decode/IR/
// interpreter plus the whole gba/ device model) driven by a run loop written
// against MVII's three device nodes, with the BIOS entry points synthesized in
// src/mvii/bios_hle.cpp because MVII packages ship no BIOS.
//
// Interpreted, not recompiled, and that is a deliberate trade. gba++'s
// recompiler is much faster but needs an offline per-game C-emission pass; the
// interpreter runs whatever ROM is staged, which is what "drop a package in
// System/Applications and it plays" requires. Speed on a Cortex-A7 at 845 MHz
// is the open question and the reason the loop reports its frame rate.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "arm_decode.h"
#include "cpu_state.h"
#include "interpreter.h"
#include "thumb_decode.h"

#include "gba_audio.h"
#include "gba_bus.h"
#include "gba_io.h"
#include "gba_irq.h"
#include "gba_ppu.h"
#include "gba_rom_header.h"
#include "gba_save.h"

#include "bios_hle.h"
#include "mvii_platform.h"

namespace {

using gbamvii::logf;

// ── configuration ──────────────────────────────────────────────────────────

// One guest instruction in ~118 host ones is the optimistic case for an
// interpreted ARM7TDMI on this part, so 256 steps is on the order of a tenth of
// a millisecond — well inside a 120 Hz round, and cheap enough that the yield's
// own cost stays under a percent. See the note on yield_now(): the decision of
// whether the slice is actually spent belongs to the kernel, not to us.
constexpr uint32_t kStepsPerYield = 256;

// The GBA's real refresh: 16777216 / (308 * 228 * 4) = 59.727 Hz.
constexpr uint64_t kFrameUs = 16743;

// The word a real GBA leaves latched in the BIOS open-bus register once the
// BIOS hands control to the cartridge (mem[0x190], the SWI-return path).
constexpr uint32_t kPostBootBiosOpenBus = 0xE3A02004u;

// Save-flush policy. See flush_save_if_settled() for why this exists at all.
constexpr uint64_t kSaveSettleUs   = 1000000ull;   // quiet before we write
constexpr uint64_t kSaveMinGapUs   = 5000000ull;   // never more often than this

// ── small file helpers ─────────────────────────────────────────────────────

bool read_whole_file(const char* path, std::vector<uint8_t>& out) {
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    out.clear();
    uint8_t chunk[64 * 1024];
    for (;;) {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got < 0) { ::close(fd); return false; }
        if (got == 0) break;
        out.insert(out.end(), chunk, chunk + got);
        // A 16 MB ROM is ~256 reads off eMMC. Offer a turn between them so the
        // shell keeps compositing while the cartridge loads.
        gbamvii::yield_now();
    }
    ::close(fd);
    return true;
}

bool write_whole_file(const std::string& path, const std::vector<uint8_t>& data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t wrote = ::write(fd, data.data() + done, data.size() - done);
        if (wrote <= 0) { ::close(fd); return false; }
        done += static_cast<std::size_t>(wrote);
    }
    ::close(fd);
    return true;
}

std::string directory_of(const char* path) {
    if (!path) return std::string();
    const std::string s(path);
    const std::size_t slash = s.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return s.substr(0, slash + 1);
}

// ── the machine ────────────────────────────────────────────────────────────

class Runtime {
public:
    bool load(int argc, char** argv);
    int  run();

private:
    void pump(uint32_t cycles);       // advance every non-CPU device
    bool step();                      // one CPU instruction; false = give up
    void present_frame();
    void pump_audio();
    void flush_save_if_settled(uint64_t now);
    void write_save();
    std::vector<uint8_t> current_save_bytes() const;

    gba::GbaBus  bus_;
    gba::GbaPpu  ppu_;
    armv4t::CPUState cpu_{};

    std::vector<uint8_t> rom_;
    std::string save_path_;
    bool        save_supported_ = false;
    bool        save_writable_  = true;
    uint64_t    save_dirty_since_us_ = 0;
    uint64_t    save_last_write_us_  = 0;

    gbamvii::Video video_;
    gbamvii::Input input_;
    gbamvii::Audio audio_;

    uint64_t cycles_    = 0;
    uint64_t frames_    = 0;
    uint32_t steps_     = 0;
    bool     running_   = true;
    bool     frame_ready_ = false;

    // Drained from the core in whatever size the ring happens to hold; 2048
    // samples at 32768 Hz is 62 ms, comfortably more than one frame's worth.
    int16_t audio_buffer_[2048];
};

bool Runtime::load(int argc, char** argv) {
    // The shell launches a package with argv[0] = the full path of the .virtua
    // file, so the ROM staged beside it is the package directory + "game.gba".
    // An explicit argv[1] overrides that, which is what the desktop tools pass.
    std::string rom_path;
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        rom_path = argv[1];
    } else {
        rom_path = directory_of(argc > 0 ? argv[0] : nullptr) + "game.gba";
    }

    if (!read_whole_file(rom_path.c_str(), rom_)) {
        // Fall back to the working directory, which is what the old stub
        // assumed and what a hand-run package may still rely on.
        if (!read_whole_file("game.gba", rom_)) {
            logf("gba: cannot open ROM '%s' (errno %d)\n", rom_path.c_str(), errno);
            return false;
        }
        rom_path = "game.gba";
    }
    if (rom_.size() < 0xC0) {
        logf("gba: '%s' is %u bytes — too small to be a cartridge\n",
             rom_path.c_str(), static_cast<unsigned>(rom_.size()));
        return false;
    }

    const gba::RomHeader header = gba::parse_rom(rom_.data(), rom_.size());
    logf("gba: %s [%s] %u MB, save=%s\n",
         header.game_title.c_str(), header.game_code.c_str(),
         static_cast<unsigned>(rom_.size() / (1024 * 1024)),
         gba::save_type_name(header.save_type));

    bus_.set_rom(rom_.data(), rom_.size());
    bus_.set_bios(nullptr);           // standalone HLE: the region is never executed
    bus_.set_bios_open_bus(kPostBootBiosOpenBus);
    bus_.io().set_ppu(&ppu_);
    bus_.io().set_bus(&bus_);

    // Save chip. Sizes match gba++'s runtime.cpp defaults.
    switch (header.save_type) {
    case gba::SaveType::SRAM:
        bus_.save().configure_sram(32 * 1024);
        save_supported_ = true;
        break;
    case gba::SaveType::EEPROM:
        bus_.save().configure_eeprom(8 * 1024);
        save_supported_ = true;
        break;
    case gba::SaveType::Flash512:
        bus_.save().configure_flash(0x10000u);
        save_supported_ = true;
        break;
    case gba::SaveType::Flash1M:
        bus_.save().configure_flash(0x20000u);
        save_supported_ = true;
        break;
    case gba::SaveType::Unknown:
    default:
        // No signature in the image. Give it SRAM anyway: a game that writes to
        // 0x0E000000 without advertising a chip is far more common than one
        // that would be harmed by the region existing.
        bus_.save().configure_sram(32 * 1024);
        save_supported_ = true;
        break;
    }

    if (save_supported_) {
        const std::size_t dot = rom_path.find_last_of('.');
        save_path_ = (dot == std::string::npos ? rom_path : rom_path.substr(0, dot)) + ".sav";
        std::vector<uint8_t> save;
        if (read_whole_file(save_path_.c_str(), save) && !save.empty()) {
            if (bus_.save().sram_enabled())        bus_.save().load_sram_bytes(save.data(), save.size());
            else if (bus_.save().eeprom_enabled()) bus_.save().load_eeprom_bytes(save.data(), save.size());
            else if (bus_.save().flash_enabled())  bus_.save().load_flash_bytes(save.data(), save.size());
            logf("gba: loaded %u bytes of save data\n", static_cast<unsigned>(save.size()));
        }
        bus_.save().clear_dirty();
    }

    gbamvii::bios_hle_bind(&cpu_, &bus_);
    gbamvii::bios_hle_boot_skip(header.entry_is_branch && header.entry_target != 0
                                    ? header.entry_target
                                    : 0x08000000u);
    return true;
}

void Runtime::pump(uint32_t cycles) {
    uint32_t remaining = cycles;
    while (remaining != 0) {
        // Never step past the next thing that wants to happen; otherwise a
        // long instruction swallows a timer overflow or an audio sample.
        uint32_t chunk = remaining;
        const uint32_t until_sample = bus_.audio().cycles_until_next_sample();
        const uint32_t until_timer  = bus_.io().cycles_until_next_timer_event();
        if (until_sample < chunk) chunk = until_sample;
        if (until_timer  < chunk) chunk = until_timer;
        if (chunk == 0) chunk = 1;

        cycles_ += chunk;
        bus_.audio().tick(chunk);
        bus_.io().tick_timers(chunk);

        const uint16_t dispstat   = bus_.io().dispstat();
        const uint16_t vc_compare = static_cast<uint16_t>((dispstat >> 8) & 0xFFu);
        const gba::GbaPpu::TickEvents ev = ppu_.tick(chunk, vc_compare);

        if (ev.hblank_started && ppu_.vcount() < gba::GbaPpu::kLinesVisible) {
            ppu_.render_scanline(ppu_.vcount(), bus_.io().read16(0x000), bus_.io().raw(),
                                 bus_.vram_ptr(), bus_.oam_ptr(), bus_.pal_ptr());
        }
        if (ev.vblank_started) {
            ppu_.mark_framebuffer_latched();
            frame_ready_ = true;
        }
        if (ev.vblank_started && (dispstat & 0x0008u)) bus_.io().request_irq(gba::GbaIo::IrqVBlank);
        if (ev.hblank_started && (dispstat & 0x0010u)) bus_.io().request_irq(gba::GbaIo::IrqHBlank);
        if (ev.vcount_matched && (dispstat & 0x0020u)) bus_.io().request_irq(gba::GbaIo::IrqVCount);

        remaining -= chunk;
    }
}

bool Runtime::step() {
    // 1. The HLE IRQ handler returning. The dispatcher parked kHleIrqReturn in
    //    LR, so the game branching to LR lands us here with nothing executed at
    //    that address — which is the point, since it is inside the BIOS region
    //    we never execute.
    if (gbamvii::bios_hle_irq_epilogue()) return true;

    // 2. A new interrupt. Checked before the halt below so a source that became
    //    pending during the last instruction is taken immediately.
    if (bus_.io().irq_pending() && !cpu_.cpsr.i) {
        if (bus_.io().halted()) bus_.io().clear_halt();
        if (gbamvii::bios_hle_irq_enter(cpu_.R[15])) return true;
    }

    // 3. HALT / STOP. Nothing to interpret until an interrupt arrives, so run
    //    the devices forward in whatever chunk the PPU says is safe. The yield
    //    inside matters more here than anywhere else: a game in HALT is a game
    //    doing nothing, and the rest of the box should get all of it.
    if (bus_.io().halted()) {
        while (running_ && bus_.io().halted() && !bus_.io().irq_pending()) {
            uint32_t chunk = ppu_.cycles_until_next_event();
            if (chunk == 0) chunk = 1;
            pump(chunk);
            if (frame_ready_) present_frame();
            gbamvii::yield_now();
            if (!input_.poll()) running_ = false;
        }
        if (bus_.io().irq_pending() && !cpu_.cpsr.i) {
            pump(gba::kIrqWakeDelayCycles);
            bus_.io().clear_halt();
            gbamvii::bios_hle_irq_enter(cpu_.R[15]);
        }
        return true;
    }

    // 4. Fetch, decode, execute.
    const uint32_t pc = cpu_.R[15];
    armv4t::Instr insn{};
    if (cpu_.thumb) {
        insn = armv4t::ThumbDecoder::decode(bus_.read16(pc), pc);
    } else {
        insn = armv4t::ArmDecoder::decode(bus_.read32(pc), pc);
    }

    uint32_t insn_cycles = 1;
    const armv4t::Interpreter::Result r =
        armv4t::Interpreter::step(cpu_, bus_, insn, &insn_cycles);

    if (r == armv4t::Interpreter::Result::Swi) {
        // step() leaves PC on the SWI itself; the HLE expects it already past,
        // because the two wait SWIs rewind it by one instruction to re-execute
        // themselves after the halt. The BIOS reads the comment field from the
        // low byte in THUMB and from bits 23..16 in ARM.
        cpu_.R[15] = pc + (cpu_.thumb ? 2u : 4u);
        const uint32_t swi = cpu_.thumb ? (insn.swi_imm & 0xFFu)
                                        : ((insn.swi_imm >> 16) & 0xFFu);
        insn_cycles += gbamvii::bios_hle_swi(swi);
    }

    pump(insn_cycles);

    if (r == armv4t::Interpreter::Result::Undefined ||
        r == armv4t::Interpreter::Result::NotImplemented) {
        logf("gba: unhandled instruction %08x at %08x (%s) — stopping\n",
             insn.raw, pc, cpu_.thumb ? "thumb" : "arm");
        return false;
    }
    return true;
}

void Runtime::present_frame() {
    frame_ready_ = false;
    ++frames_;
    if (ppu_.has_latched_framebuffer()) {
        video_.present(ppu_.latched_framebuffer());
    }
    if (!input_.poll()) running_ = false;
    bus_.io().set_keyinput(input_.keyinput());
}

void Runtime::pump_audio() {
    if (!audio_.enabled()) return;
    for (;;) {
        const std::size_t got =
            bus_.audio().drain_samples(audio_buffer_,
                                       sizeof(audio_buffer_) / sizeof(audio_buffer_[0]));
        if (got == 0) return;
        audio_.submit(audio_buffer_, got);
        if (got < sizeof(audio_buffer_) / sizeof(audio_buffer_[0])) return;
    }
}

std::vector<uint8_t> Runtime::current_save_bytes() const {
    if (bus_.save().sram_enabled())   return bus_.save().sram_bytes();
    if (bus_.save().eeprom_enabled()) return bus_.save().eeprom_bytes();
    if (bus_.save().flash_enabled())  return bus_.save().flash_bytes();
    return std::vector<uint8_t>();
}

void Runtime::write_save() {
    if (!save_supported_ || !save_writable_ || save_path_.empty()) return;
    const std::vector<uint8_t> bytes = current_save_bytes();
    if (bytes.empty()) return;
    if (!write_whole_file(save_path_, bytes)) {
        logf("gba: cannot write '%s' (errno %d); saves are disabled\n",
             save_path_.c_str(), errno);
        save_writable_ = false;
        return;
    }
    bus_.save().clear_dirty();
    save_last_write_us_ = gbamvii::now_us();
    save_dirty_since_us_ = 0;
}

// The one place this runtime touches storage while a game is running, and it is
// a deliberate exception to the rule that it must not.
//
// The rule exists because MVII's eMMC writes are slow enough to stall the whole
// box: anything on the frame path that touches storage freezes the OS along
// with the app. A GBA save chip, though, is written by the game constantly —
// Final Fantasy VI rewrites SRAM through a whole save-menu interaction — so
// mirroring every dirty flag to disk would be exactly the freeze the rule
// forbids, and never writing at all would make the runtime useless for the RPGs
// it is most obviously for.
//
// So: write only once the game has stopped touching the chip for a second, and
// never more often than every five. A save burst produces one 8-128 KB write a
// few seconds after the player leaves the save menu, and a game that does not
// save produces none. If that still costs a visible hitch, the honest fix is to
// drop to writing on exit only — but a save that survives the player closing
// the window is worth one stall a game session.
void Runtime::flush_save_if_settled(uint64_t now) {
    if (!save_supported_ || !save_writable_) return;
    if (bus_.save().dirty()) {
        if (save_dirty_since_us_ == 0) save_dirty_since_us_ = now;
        // Keep sliding the settle point while writes keep coming.
        bus_.save().clear_dirty();
        save_dirty_since_us_ = now;
        return;
    }
    if (save_dirty_since_us_ == 0) return;
    if (now - save_dirty_since_us_ < kSaveSettleUs) return;
    if (save_last_write_us_ != 0 && now - save_last_write_us_ < kSaveMinGapUs) return;
    write_save();
}

int Runtime::run() {
    video_.open(static_cast<int>(gba::GbaPpu::kScreenWidth),
                static_cast<int>(gba::GbaPpu::kScreenHeight));
    input_.open();
    audio_.open(bus_.audio().sample_rate(), 1);   // the core mixes down to mono
    bus_.io().set_keyinput(input_.keyinput());

    logf("gba: running (%ux%u, audio %u Hz)\n",
         static_cast<unsigned>(gba::GbaPpu::kScreenWidth),
         static_cast<unsigned>(gba::GbaPpu::kScreenHeight),
         static_cast<unsigned>(bus_.audio().sample_rate()));

    uint64_t next_frame_us = gbamvii::now_us() + kFrameUs;
    uint64_t report_us     = gbamvii::now_us() + 5000000ull;
    uint64_t report_frames = 0;

    while (running_) {
        if (!step()) break;

        if (++steps_ >= kStepsPerYield) {
            steps_ = 0;
            gbamvii::yield_now();
        }

        if (!frame_ready_) continue;

        present_frame();
        pump_audio();

        const uint64_t now = gbamvii::now_us();
        flush_save_if_settled(now);

        // Pace to the GBA's refresh when we are ahead of it, and simply carry on
        // when we are behind — which on this part is the case we should expect.
        // sleep_until() parks the process, so an early frame hands its slack to
        // the rest of the box rather than spinning it away.
        if (now < next_frame_us) {
            gbamvii::sleep_until(next_frame_us);
            next_frame_us += kFrameUs;
        } else {
            // Do not let the deadline drift into the past and turn every
            // subsequent frame into an instant one.
            next_frame_us = now + kFrameUs;
        }

        if (now >= report_us) {
            logf("gba: %u fps\n", static_cast<unsigned>((frames_ - report_frames) / 5));
            report_frames = frames_;
            report_us = now + 5000000ull;
        }
    }

    logf("gba: stopped after %u frames\n", static_cast<unsigned>(frames_));
    if (save_supported_ && save_writable_) write_save();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Runtime runtime;
    if (!runtime.load(argc, argv)) return 1;
    return runtime.run();
}
