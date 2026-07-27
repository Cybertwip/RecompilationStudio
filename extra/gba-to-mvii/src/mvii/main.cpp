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
// So this is the runtime, and the emulator inside it is the Rust port: the
// gba-core and armv4t crates from extra/gba-rust, vendored under rust/ and
// ported to no_std so they link into a Virtua executable (see rust/Cargo.toml).
// The C++ here is only MVII — devices, scheduling, files — and it reaches the
// core through the dozen calls in include/gba_mvii.h. Keeping the boundary that
// narrow is the point: the emulation stays the reference implementation, byte
// for byte, and every syscall this program makes is in this directory.
//
// Interpreted, not recompiled, and that is a deliberate trade. The recompiler
// needs an offline per-game C-emission pass; the interpreter runs whatever ROM
// is staged, which is what "drop a package in System/Applications and it plays"
// requires. Speed on a Cortex-A7 at 845 MHz is the open question, and the
// reason the loop reports its frame rate.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "gba_mvii.h"
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

// Save-flush policy. See flush_save_if_settled() for why this exists at all.
constexpr uint64_t kSaveProbeUs  = 1000000ull;   // how often we look
constexpr uint64_t kSaveSettleUs = 1000000ull;   // quiet before we write
constexpr uint64_t kSaveMinGapUs = 5000000ull;   // never more often than this

// 65536 Hz stereo is ~2200 interleaved values a frame; this holds one frame
// with room to spare, and lives inside the heap-allocated Runtime.
constexpr std::size_t kAudioBufferSamples = 4096;

// ── small file helpers ─────────────────────────────────────────────────────

bool write_whole_file(const std::string& path, const uint8_t* data, std::size_t len) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < len) {
        const ssize_t wrote = ::write(fd, data + done, len - done);
        if (wrote <= 0) { ::close(fd); return false; }
        done += static_cast<std::size_t>(wrote);
    }
    ::close(fd);
    return true;
}

bool read_whole_file(const char* path, std::vector<uint8_t>& out) {
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0 || ::lseek(fd, 0, SEEK_SET) != 0) { ::close(fd); return false; }
    out.assign(static_cast<std::size_t>(end), 0);
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t got = ::read(fd, out.data() + done, out.size() - done);
        if (got <= 0) break;
        done += static_cast<std::size_t>(got);
    }
    ::close(fd);
    out.resize(done);
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
    ~Runtime();

    // Devices first, ROM second. A Virtua guest is invisible to the shell until
    // it has a surface, so opening /dev/fb0 before the 16 MB cartridge read is
    // the difference between "a window that says it failed" and "a process that
    // vanished" — which is the report this runtime exists to answer.
    void open_devices();
    bool load(int argc, char** argv);
    int  run();

    // Paint a flat colour and hold it, so a fatal startup error is visible on
    // the device rather than only on a serial cable.
    void hold_failure();

private:
    bool load_rom(const std::string& path);
    void present_frame();
    void pump_audio();
    void flush_save_if_settled(uint64_t now);
    void write_save();

    GbaMvii* machine_ = nullptr;

    std::string save_path_;
    bool     save_supported_     = false;
    bool     save_writable_      = true;
    uint64_t save_hash_          = 0;   // last hash we observed
    uint64_t save_written_hash_  = 0;   // last hash we actually wrote
    uint64_t save_dirty_since_us_ = 0;
    uint64_t save_last_write_us_  = 0;
    uint64_t save_next_probe_us_  = 0;

    gbamvii::Video video_;
    gbamvii::Input input_;
    gbamvii::Audio audio_;

    uint64_t frames_  = 0;
    bool     running_ = true;

    int16_t audio_buffer_[kAudioBufferSamples];
};

Runtime::~Runtime() {
    if (machine_) {
        gba_mvii_destroy(machine_);
        machine_ = nullptr;
    }
}

// Read the cartridge straight into storage the core owns.
//
// The alternative — read into a vector of ours, then hand the bytes over —
// holds the ROM twice at the moment of the copy. Final Fantasy VI is a 16 MB
// cartridge and MVII gives a guest 32 MB when the kernel heap is healthy,
// degrading toward 4 MB when it is not (allocate_user_heap in
// mvii_arm_process_manager.cpp). One buffer is the difference between loading
// and failing to.
bool Runtime::load_rom(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end <= 0 || ::lseek(fd, 0, SEEK_SET) != 0) { ::close(fd); return false; }
    const std::size_t size = static_cast<std::size_t>(end);
    if (size < 0xC0) {
        logf("gba: '%s' is %u bytes — too small to be a cartridge\n",
             path.c_str(), static_cast<unsigned>(size));
        ::close(fd);
        return false;
    }

    uint8_t* rom = gba_mvii_rom_alloc(size);
    if (!rom) {
        logf("gba: cannot allocate %u bytes for the cartridge\n",
             static_cast<unsigned>(size));
        ::close(fd);
        return false;
    }

    std::size_t done = 0;
    while (done < size) {
        // A 16 MB ROM is ~256 reads off eMMC. Offer a turn between them so the
        // shell keeps compositing while the cartridge loads.
        const std::size_t want = size - done < 64u * 1024u ? size - done : 64u * 1024u;
        const ssize_t got = ::read(fd, rom + done, want);
        if (got < 0) {
            logf("gba: read error on '%s' (errno %d)\n", path.c_str(), errno);
            ::close(fd);
            gba_mvii_rom_free(rom, size);
            return false;
        }
        if (got == 0) break;  // short file; the tail stays zero, as open bus
        done += static_cast<std::size_t>(got);
        gbamvii::yield_now();
    }
    ::close(fd);
    if (done < size) {
        logf("gba: '%s' ended at %u of %u bytes\n", path.c_str(),
             static_cast<unsigned>(done), static_cast<unsigned>(size));
    }

    // Ownership of `rom` transfers here whether or not the machine is built.
    machine_ = gba_mvii_create(rom, size);
    if (!machine_) {
        logf("gba: the core refused the cartridge\n");
        return false;
    }
    return true;
}

bool Runtime::load(int argc, char** argv) {
    if (gba_mvii_abi_version() != GBA_MVII_ABI_VERSION) {
        // A stale object file that links but disagrees about a signature fails
        // far more quietly than one that refuses to start.
        logf("gba: core ABI %u, runtime expects %u — refusing to run\n",
             static_cast<unsigned>(gba_mvii_abi_version()),
             static_cast<unsigned>(GBA_MVII_ABI_VERSION));
        return false;
    }

    // The shell launches a package with argv[0] = the full path of the .virtua
    // file, so the ROM staged beside it is the package directory + "game.gba".
    // An explicit argv[1] overrides that, which is what the desktop tools pass.
    std::string rom_path;
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        rom_path = argv[1];
    } else {
        rom_path = directory_of(argc > 0 ? argv[0] : nullptr) + "game.gba";
    }

    if (!load_rom(rom_path)) {
        // Fall back to the working directory, which is what the old stub
        // assumed and what a hand-run package may still rely on.
        if (!load_rom("game.gba")) {
            logf("gba: cannot open ROM '%s' (errno %d)\n", rom_path.c_str(), errno);
            return false;
        }
        rom_path = "game.gba";
    }

    uint8_t title[16] = {};
    gba_mvii_rom_title(machine_, title, sizeof(title));

    static const char* const kBackupNames[] = {"none", "SRAM", "Flash 64K",
                                               "Flash 128K", "EEPROM"};
    const uint32_t backup = gba_mvii_backup_kind(machine_);
    const std::size_t save_size = gba_mvii_save_size(machine_);
    logf("gba: %s — save %s (%u bytes)\n",
         reinterpret_cast<const char*>(title),
         backup < 5 ? kBackupNames[backup] : "?",
         static_cast<unsigned>(save_size));

    save_supported_ = save_size != 0;
    if (save_supported_) {
        const std::size_t dot = rom_path.find_last_of('.');
        save_path_ = (dot == std::string::npos ? rom_path : rom_path.substr(0, dot)) + ".sav";
        std::vector<uint8_t> save;
        if (read_whole_file(save_path_.c_str(), save) && !save.empty()) {
            const std::size_t took = gba_mvii_save_load(machine_, save.data(), save.size());
            logf("gba: loaded %u bytes of save data\n", static_cast<unsigned>(took));
        }
        save_hash_ = gba_mvii_save_hash(machine_);
        save_written_hash_ = save_hash_;
    }
    return true;
}

void Runtime::present_frame() {
    ++frames_;
    video_.present(gba_mvii_framebuffer(machine_));
    gba_mvii_frame_consume(machine_);
    if (!input_.poll()) running_ = false;
    gba_mvii_set_keys(machine_, input_.keyinput());
}

void Runtime::pump_audio() {
    for (;;) {
        // Drained even with no DAC: the core's queue is unbounded, and an
        // emulator that never empties it grows for as long as it runs.
        const std::size_t got = audio_.enabled()
            ? gba_mvii_drain_audio(machine_, audio_buffer_, kAudioBufferSamples)
            : gba_mvii_drain_audio(machine_, nullptr, 0);
        if (got == 0) return;
        audio_.submit(audio_buffer_, got);
        if (got < kAudioBufferSamples) return;
    }
}

void Runtime::write_save() {
    if (!save_supported_ || !save_writable_ || save_path_.empty()) return;
    const std::size_t size = gba_mvii_save_size(machine_);
    if (size == 0) return;

    std::vector<uint8_t> bytes(size, 0);
    const std::size_t got = gba_mvii_save_read(machine_, bytes.data(), bytes.size());
    if (got == 0) return;
    bytes.resize(got);

    if (!write_whole_file(save_path_, bytes.data(), bytes.size())) {
        logf("gba: cannot write '%s' (errno %d); saves are disabled\n",
             save_path_.c_str(), errno);
        save_writable_ = false;
        return;
    }
    save_written_hash_ = gba_mvii_save_hash(machine_);
    save_hash_ = save_written_hash_;
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
// mirroring every change to disk would be exactly the freeze the rule forbids,
// and never writing at all would make the runtime useless for the RPGs it is
// most obviously for.
//
// So: look at the chip once a second (an FNV pass over at most 128 KB, which
// costs nothing next to a frame), write only once it has stopped changing for a
// second, and never more often than every five. A save burst produces one write
// a few seconds after the player leaves the save menu; a game that does not
// save produces none. If that still costs a visible hitch, the honest fix is to
// drop to writing on exit only — but a save that survives the player closing
// the window is worth one stall a game session.
void Runtime::flush_save_if_settled(uint64_t now) {
    if (!save_supported_ || !save_writable_) return;
    if (now < save_next_probe_us_) return;
    save_next_probe_us_ = now + kSaveProbeUs;

    const uint64_t hash = gba_mvii_save_hash(machine_);
    if (hash != save_hash_) {
        // Still changing: slide the settle point forward.
        save_hash_ = hash;
        save_dirty_since_us_ = now;
        return;
    }
    if (save_dirty_since_us_ == 0) return;
    if (hash == save_written_hash_) { save_dirty_since_us_ = 0; return; }
    if (now - save_dirty_since_us_ < kSaveSettleUs) return;
    if (save_last_write_us_ != 0 && now - save_last_write_us_ < kSaveMinGapUs) return;
    write_save();
}

void Runtime::open_devices() {
    const bool have_video = video_.open(static_cast<int>(gba_mvii_screen_width()),
                                        static_cast<int>(gba_mvii_screen_height()));
    const bool have_input = input_.open();
    logf("gba: devices — fb0 %s, input0 %s\n",
         have_video ? "ok" : "FAILED", have_input ? "ok" : "FAILED");
    if (have_video) {
        // A first present as soon as the surface exists. The shell composites
        // whatever the guest last presented, so this is what puts the window on
        // screen before the cartridge read starts.
        video_.fill(0x10, 0x10, 0x18);
    }
}

void Runtime::hold_failure() {
    if (!video_.opened()) return;
    const uint64_t until = gbamvii::now_us() + 4000000ull;
    while (gbamvii::now_us() < until) {
        video_.fill(0x60, 0x10, 0x10);
        if (!input_.poll()) break;
        gbamvii::sleep_until(gbamvii::now_us() + 33000ull);
    }
}

int Runtime::run() {
    const uint32_t rate = gba_mvii_audio_rate();
    audio_.open(rate, 2);   // the core mixes interleaved stereo
    gba_mvii_set_keys(machine_, input_.keyinput());

    logf("gba: running (%ux%u, audio %u Hz %s)\n",
         static_cast<unsigned>(gba_mvii_screen_width()),
         static_cast<unsigned>(gba_mvii_screen_height()),
         static_cast<unsigned>(rate), audio_.enabled() ? "stereo" : "off");

    uint64_t next_frame_us = gbamvii::now_us() + kFrameUs;
    uint64_t report_us     = gbamvii::now_us() + 5000000ull;
    uint64_t report_frames = 0;
    save_next_probe_us_    = gbamvii::now_us() + kSaveProbeUs;

    while (running_) {
        // Bounded work, then a turn for everything else. run_steps returns as
        // soon as the PPU finishes a frame, so this never overshoots one.
        const uint32_t frame_ready = gba_mvii_run_steps(machine_, kStepsPerYield);
        gbamvii::yield_now();
        if (!frame_ready) continue;

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
    if (save_supported_ && save_writable_ &&
        gba_mvii_save_hash(machine_) != save_written_hash_) {
        write_save();
    }
    return 0;
}

}  // namespace

// extern "C" is load-bearing. Under -ffreestanding clang stops treating `main`
// as the reserved hosted entry point and mangles it like any other function, so
// a plain `int main(...)` here links as _Z4mainiPPc and Dash's crt.s — which
// does `.extern main` / `.set virtua, main` — finds nothing.
extern "C" int main(int argc, char** argv) {
    // Say something before touching anything. If this line does not reach the
    // serial console, the image did not start — which is a different failure
    // from a runtime that started and then gave up, and the two were
    // indistinguishable when the first thing main() did was blow the stack.
    logf("gba: gba-to-mvii starting (argc=%d)\n", argc);

    // Runtime is heap-allocated on purpose. It is small now that the machine
    // itself lives behind a Rust handle, but MVII gives a guest a 512 KB stack
    // (kUserStackSize in mvii_arm_process_manager.cpp) against 32 MB of heap,
    // and the previous C++ core put 670368 bytes of EWRAM/VRAM/framebuffer in
    // this object. As a local it overflowed the stack in main()'s prologue: no
    // window, no log, process gone — indistinguishable from the /dev/native0
    // stub it replaced. Keep it off the stack.
    Runtime* runtime = new (std::nothrow) Runtime();
    if (!runtime) {
        logf("gba: out of memory allocating the runtime (%u bytes)\n",
             static_cast<unsigned>(sizeof(Runtime)));
        return 1;
    }
    runtime->open_devices();
    if (!runtime->load(argc, argv)) {
        runtime->hold_failure();
        delete runtime;
        return 1;
    }
    const int rc = runtime->run();
    delete runtime;
    return rc;
}
