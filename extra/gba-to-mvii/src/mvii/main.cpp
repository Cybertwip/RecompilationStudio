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
// So this is the runtime, and the emulator inside it is extra/gbarecomp,
// cross-compiled for Cortex-A7 as a curated static library (see the
// gbarecomp-mvii target in ../../CMakeLists.txt) — the same sources the desktop
// build uses, not a port and not a second implementation. The C++ here is only
// MVII — devices, scheduling, files — and it reaches the core through the
// couple of dozen calls in include/gba_mvii.h. Keeping the boundary that narrow
// is the point: the emulation stays the reference implementation, byte for
// byte, and every syscall this program makes is in this directory.
//
// Recompiled, with the interpreter as a bridge — and the distinction is
// reported, never hidden. Packaging a game runs gbarecomp's static recompiler
// and compiles its output into this image, so cartridge code executes as native
// ARM. Anything the recompiler could not reach at build time (indirect branches
// into data, code written at runtime) misses the dispatch tables and is bridged
// by the reference interpreter, which is correct but slow. Those misses are
// counted, named in the log and totalled at exit by gbamvii::report_coverage():
// a build with no recompiled corpus at all says NOT STATIC rather than quietly
// interpreting a whole game. Speed on a Cortex-A7 at 845 MHz is the open
// question, and the reason the loop reports its frame rate.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "gba_mvii.h"
#include "gba_mvii_heal.h"
#include "mvii_platform.h"

namespace {

using gbamvii::logf;

// ── configuration ──────────────────────────────────────────────────────────

// How much emulation runs between two offers of the CPU back to MVII.
//
// This is a rate, and the rate that matters is yields per second, not per
// frame. MVII's slice is ~4.2 ms (`slice=` in the shell's sched report) and the
// contract is that everything else on the box gets its turn at 120 Hz or
// better; the guest satisfies both by asking several times per slice, and
// nothing is bought by asking a hundred times per slice.
//
// A GBA frame is about 150k ARM7TDMI instructions (280,896 cycles at ~1.9
// cycles each), so 256 was ~585 yields *per frame* — one every 60 us of guest
// work, roughly seventy per slice. Each one is a call through the ABI table
// into the kernel, a GPT read across the APB (uncached, and the counter that
// mt6592_timer.c's own comment notes nobody had ever costed per frame) and a
// walk of the kernel thread table, so seventy per slice is a measurable tax on
// an interpreter that is already the thing setting the frame rate.
//
// 2048 asks roughly every 470 us: eight or nine times per slice, ~2000 times a
// second — still an order of magnitude above the 120 Hz floor, with a
// worst-case slice overrun of about a tenth of a slice. See the note on
// yield_now(): whether the slice is actually spent stays the kernel's decision,
// and this only changes how often it is asked.
#if defined(GBA_MVII_NATIVE_AOT)
// One generated block per dispatch (the Studio emitter disables cross-block
// goto chaining), so this is a real cooperative bound rather than a hint.
constexpr uint32_t kBlocksPerYield = 256;
#else
constexpr uint32_t kStepsPerYield = 2048;
#endif

// Consecutive yields with no frame in sight before the loop stops trusting the
// frame boundary to poll input. See the note at the use site.
//
// Denominated in instructions rather than in slices so that it keeps meaning
// the same thing when the constant above moves: a quarter of a million
// instructions is well past two frames' worth of work, so this never fires
// while the emulator is making progress. The max() keeps it at one slice
// minimum if kStepsPerYield ever grows past the whole budget.
constexpr uint32_t kStallPollInstructions = 262144;
#if defined(GBA_MVII_NATIVE_AOT)
// Roughly 8-16 guest instructions per generated block in normal cartridge
// code, so 128 dispatch slices remains comfortably beyond two GBA frames.
constexpr uint32_t kStallPollSlices = 128;
#else
constexpr uint32_t kStallPollSlices =
    kStallPollInstructions / kStepsPerYield > 1 ? kStallPollInstructions / kStepsPerYield : 1;
#endif

// How many distinct coverage misses get named in the log before the runtime
// stops listing them and only counts.
//
// A coverage miss is a genuine finding — an address the recompiler did not
// translate, which the interpreter then bridged — and the address is the whole
// value of the report, because it is what a `game.toml` proposal is keyed to.
// So the first ones are printed in full. But stderr here is a serial console at
// a byte at a time, and a cartridge whose entry point itself missed will miss
// on nearly every block: printing all of those would cost more time than the
// emulation and would bury the first, most diagnostic one under thousands of
// lines. The count keeps going after the listing stops, and report_coverage()
// prints the total, so nothing is lost — only the middle of the list.
//
// AOT builds only, because only those get a per-call miss signal back from the
// core. A build with no recompiled corpus misses on literally every dispatch;
// listing addresses there would say nothing that the one-line NOT STATIC
// verdict at exit does not already say better.
#if defined(GBA_MVII_NATIVE_AOT)
constexpr uint32_t kMaxReportedMisses = 32;
#endif

// The GBA's real refresh: 16777216 / (308 * 228 * 4) = 59.727 Hz.
constexpr uint64_t kFrameUs = 16743;

// How often a stalled machine describes itself. Slow on purpose: stderr goes
// out the serial console a byte at a time here.
constexpr uint64_t kStallReportUs = 2000000ull;

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

std::string file_name_of(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ends_with_fold(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

// Names a cartridge plausibly arrives under. `.gba` is what the packager
// stages; the rest are what a hand-copied folder tends to contain.
bool looks_like_a_cartridge(const std::string& name) {
    return ends_with_fold(name, ".gba") || ends_with_fold(name, ".agb") ||
           ends_with_fold(name, ".bin") || ends_with_fold(name, ".rom");
}

// Every regular name in `dir`, in whatever order the filesystem gives them.
//
// MVII's readdir reaches the guest through Dash, which leaves d_type at
// DT_UNKNOWN — the shim's <dirent.h> does not define _DIRENT_HAVE_D_TYPE, so
// the wire value is dropped on the way in. Nothing here may test it; the only
// way to tell a file from a directory is to try opening it.
std::vector<std::string> list_directory(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = ::opendir(dir.empty() ? "." : dir.c_str());
    if (!d) return names;
    while (const struct dirent* entry = ::readdir(d)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        names.push_back(name);
        if (names.size() >= 64) break;   // a package directory, not a library
    }
    ::closedir(d);
    return names;
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

    // Put the failure on the panel and hold it, so a fatal startup error is
    // legible on the device rather than only over a serial cable. MVII shows a
    // guest's stderr in the Terminal view and only while it is attached, so
    // for an app launched from the dashboard this screen is the only report
    // the user will ever see.
    void hold_failure();

private:
    void find_bios(const std::string& dir);
    bool find_rom(int argc, char** argv);
    bool load_rom(const std::string& path);
    void present_frame();
    void pump_audio();
    void flush_save_if_settled(uint64_t now);
    void write_save();
    void report_stall(uint64_t now);

    // Say something once, to the serial console and to the failure screen.
    void note(const char* fmt, ...);

    GbaMvii* machine_ = nullptr;

    std::string rom_path_;
    // The real BIOS image, when the package ships one. Empty means HLE boot.
    std::string          bios_path_;
    std::vector<uint8_t> bios_;
    std::vector<std::string> notes_;   // the failure screen, one line each

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

    // Frame-time attribution, reported alongside the fps line. The whole point
    // is to answer "where did the frame go" in one run instead of by argument:
    // emulation, the framebuffer conversion+present, audio, and the pacing park
    // are the only four things this loop does. Three clock reads per frame on
    // top of the one the loop already takes, so it stays on in normal builds --
    // a number nobody can see is a number nobody trusts.
    uint64_t emu_us_       = 0;   // run_steps + the yields between them
    uint64_t present_us_   = 0;
    uint64_t audio_us_     = 0;
    uint64_t sleep_us_     = 0;
    uint64_t slices_       = 0;   // run_steps calls, i.e. yields, this window
    // The per-yield cost needs two clock reads *per yield* to measure, which is
    // ~60x the loop's own sampling rate and would distort what it measures. So
    // sample it on one frame in 64 and scale: enough to catch a yield that has
    // become expensive, cheap enough to leave in.
    uint64_t yield_ns_     = 0;
    uint64_t yield_count_  = 0;

    // Cleared the first time the frame clock fails to reach a deadline. From
    // then on the loop runs unpaced: a game running too fast is a bug, a window
    // that never updates again is not distinguishable from a crash.
    bool     pacing_ok_ = true;

    // Latches so the two faults that used to be silent report exactly once.
    // Once each, because both are wiring faults rather than conditions that
    // come and go, and stderr is not free on this box.
    bool     warned_pacing_ = false;
    bool     warned_no_framebuffer_ = false;

    uint64_t stall_report_us_ = 0;
    uint64_t stall_last_clock_ = 0;

    int16_t audio_buffer_[kAudioBufferSamples];
};

Runtime::~Runtime() {
    if (machine_) {
        gba_mvii_destroy(machine_);
        machine_ = nullptr;
    }
}

void Runtime::note(const char* fmt, ...) {
    char line[128];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0) return;
    notes_.push_back(std::string(line));
    logf("gba: %s\n", line);
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
    if (fd < 0) {
        // Expected for most candidates — this is the search, not the failure.
        logf("gba: no '%s' (errno %d)\n", path.c_str(), errno);
        return false;
    }

    // The file opened, so from here on every way out is a real failure and has
    // to say so on the panel. The first version of this returned false quietly
    // when the size probe misbehaved, and the screen then blamed the one thing
    // that was not wrong — "no cartridge found", about a cartridge that was
    // sitting right there. A guest that can see a file and cannot measure it is
    // reporting a broken seek, not a missing ROM.
    const std::string leaf = file_name_of(path);
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0 || ::lseek(fd, 0, SEEK_SET) != 0) {
        note("CANNOT SEEK %s (ERRNO %d)", leaf.c_str(), errno);
        ::close(fd);
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(end);
    if (size < 0xC0) {
        note("%s IS %u BYTES - TOO SMALL", leaf.c_str(),
             static_cast<unsigned>(size));
        ::close(fd);
        return false;
    }

    uint8_t* rom = gba_mvii_rom_alloc(size);
    if (!rom) {
        note("OUT OF MEMORY FOR A %u MB CARTRIDGE",
             static_cast<unsigned>(size >> 20));
        ::close(fd);
        return false;
    }

    // A 16 MB cartridge off eMMC is not instant, and a window that sits on one
    // colour for several seconds reads as a hang. Say what is happening.
    {
        char line[96];
        std::snprintf(line, sizeof(line), "LOADING %s (%u MB)",
                      leaf.c_str(), static_cast<unsigned>(size >> 20));
        const char* lines[] = {"GBA RUNTIME", "", line};
        video_.message(0x10, 0x10, 0x18, lines, 3);
    }

    std::size_t done = 0;
    while (done < size) {
        // A 16 MB ROM is ~256 reads off eMMC. Offer a turn between them so the
        // shell keeps compositing while the cartridge loads.
        const std::size_t want = size - done < 64u * 1024u ? size - done : 64u * 1024u;
        const ssize_t got = ::read(fd, rom + done, want);
        if (got < 0) {
            note("READ ERROR ON %s (ERRNO %d)", leaf.c_str(), errno);
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
    machine_ = bios_.empty()
        ? gba_mvii_create(rom, size)
        : gba_mvii_create_with_bios(rom, size, bios_.data(), bios_.size());
    if (!machine_) {
        note("THE CORE REFUSED %s", leaf.c_str());
        if (!bios_.empty()) {
            // The size was checked before we got here, so the only way the core
            // refuses a right-sized image is the SHA-1 gate: this file is not
            // the real BIOS. Say which file, because the alternative report —
            // "no cartridge found" — blames the one thing that is fine.
            note("%s IS NOT THE REAL BIOS",
                 file_name_of(bios_path_).c_str());
            note("REMOVE IT TO BOOT WITHOUT ONE");
        }
        return false;
    }
    logf("gba: loaded '%s' (%u bytes)\n", path.c_str(), static_cast<unsigned>(done));
    return true;
}

// Look for a real BIOS image beside the package.
//
// Optional by design: with no image the core boots HLE, which is a reduced but
// working machine. With one, the whole boot ROM runs — logo, chime, and the
// exact register and RAM state a game's first instruction is entitled to
// expect. So this searches rather than requires, and says which way it went.
//
// Only the size is checked here. The image's identity is the core's gate to
// keep (it hashes what it is given), and duplicating the SHA-1 on this side
// would mean two places that can disagree about what the real BIOS is.
void Runtime::find_bios(const std::string& dir) {
    const std::size_t want = gba_mvii_bios_size();

    // gba_bios.bin is what gbarecomp's own tooling names it, so a package
    // assembled from a recompile already has the right name. bios.bin is the
    // other name every GBA emulator has ever used.
    const std::string candidates[] = {
        dir + "gba_bios.bin",
        "gba_bios.bin",
        dir + "bios.bin",
        "bios.bin",
    };

    for (const std::string& path : candidates) {
        std::vector<uint8_t> bytes;
        if (!read_whole_file(path.c_str(), bytes)) continue;
        if (bytes.size() != want) {
            // Found and unusable is worth a line: it is almost always a
            // truncated copy, and silently HLE-booting past it would leave the
            // user wondering why their BIOS "did nothing".
            note("%s IS %u BYTES, NOT %u",
                 file_name_of(path).c_str(),
                 static_cast<unsigned>(bytes.size()),
                 static_cast<unsigned>(want));
            continue;
        }
        bios_ = std::move(bytes);
        bios_path_ = path;
        logf("gba: BIOS image '%s'\n", path.c_str());
        return;
    }
    logf("gba: no BIOS image — booting HLE\n");
}

// Find the cartridge and build the machine around it.
//
// This is deliberately more forgiving than "open the path the packager was
// told to write". MVII launches a package with argv[0] set to the .virtua's
// own path and the working directory set to the folder containing it
// (set_process_working_directory in mvii_arm_process_manager.cpp), so the
// staged layout — game.gba beside the executable — resolves two different
// ways, and both are tried. But a package assembled by hand keeps whatever
// name the ROM already had, and the first version of this runtime failed with
// nothing on screen but a flat red rectangle when it did. So after the exact
// names comes a scan of the package directory for anything that looks like a
// cartridge, and if that also comes up empty the directory listing goes on the
// failure screen, where the user can compare it against what they meant to
// copy.
bool Runtime::find_rom(int argc, char** argv) {
    // Two ways to name the package directory, because they can disagree.
    // argv[0] is whatever the launcher passed — the shell passes the .virtua's
    // path, but Dashboard.cpp lets a caller supply its own argv, in which case
    // argv[0] is that caller's idea of the program name. The working directory
    // is set from the executable path unconditionally, so it is the more
    // reliable of the two; it is also relative-path-resolvable, which is why
    // the bare "game.gba" candidate below is not redundant.
    std::string dir = directory_of(argc > 0 ? argv[0] : nullptr);

    char cwd[512] = {};
    if (::getcwd(cwd, sizeof(cwd)) && cwd[0]) {
        const std::size_t len = std::strlen(cwd);
        if (len + 1 < sizeof(cwd) && cwd[len - 1] != '/') {
            cwd[len] = '/';
            cwd[len + 1] = '\0';
        }
    } else {
        cwd[0] = '\0';
    }

    std::vector<std::string> candidates;
    auto consider = [&candidates](const std::string& path) {
        if (path.empty()) return;
        for (const std::string& seen : candidates) {
            if (seen == path) return;
        }
        candidates.push_back(path);
    };

    // An explicit argument wins: that is what the desktop tools pass.
    if (argc > 1 && argv[1] && argv[1][0] != '\0') consider(argv[1]);
    consider(dir + "game.gba");
    consider("game.gba");

    // Then whatever the directory actually holds. The listing is taken once
    // and kept, because it is also what the failure screen reports.
    std::vector<std::string> entries = list_directory(dir);
    if (entries.empty() && !dir.empty()) {
        // argv[0] did not name a directory we can read; fall back to the one
        // MVII actually put us in.
        dir = cwd;
        entries = list_directory(dir);
    }
    for (const std::string& name : entries) {
        if (looks_like_a_cartridge(name)) consider(dir + name);
    }

    // Before the first load_rom, because the BIOS is an argument to building
    // the machine rather than something that can be installed afterwards.
    find_bios(dir);

    for (const std::string& path : candidates) {
        if (load_rom(path)) {
            rom_path_ = path;
            return true;
        }
    }

    // The screen is 17 rows of 39 columns, and a path costs two of them, so
    // this is written to fit rather than to be complete — the serial log has
    // every candidate and its errno.
    note("NO CARTRIDGE FOUND");
    note("");
    note("IN %s", dir.empty() ? cwd : dir.c_str());
    if (entries.empty()) {
        note("  (EMPTY, OR NOT READABLE)");
    } else {
        constexpr std::size_t kMaxShown = 6;
        for (std::size_t i = 0; i < entries.size() && i < kMaxShown; ++i) {
            note("  %s", entries[i].c_str());
        }
        if (entries.size() > kMaxShown) {
            note("  ... AND %u MORE",
                 static_cast<unsigned>(entries.size() - kMaxShown));
        }
    }
    note("");
    note("COPY THE ROM HERE AS GAME.GBA");
    return false;
}

bool Runtime::load(int argc, char** argv) {
    if (gba_mvii_abi_version() != GBA_MVII_ABI_VERSION) {
        // A stale object file that links but disagrees about a signature fails
        // far more quietly than one that refuses to start.
        note("CORE ABI %u, RUNTIME EXPECTS %u",
             static_cast<unsigned>(gba_mvii_abi_version()),
             static_cast<unsigned>(GBA_MVII_ABI_VERSION));
        return false;
    }

    if (!find_rom(argc, argv)) return false;
    const std::string& rom_path = rom_path_;

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

    // Video::present() drops a null source on the floor, which is right for it
    // — a frame it cannot draw is not a reason to take the process down — but
    // it made a core that produces no pixels look exactly like a core producing
    // identical ones: frames counted, nothing on screen, nothing said. Ask
    // first, so the two are distinguishable from the log alone.
    const uint8_t* pixels = gba_mvii_framebuffer(machine_);
    if (pixels) {
        video_.present(pixels);
    } else if (!warned_no_framebuffer_) {
        warned_no_framebuffer_ = true;
        logf("gba: core signalled a frame but its framebuffer is null\n");
    }

    gba_mvii_frame_consume(machine_);
    if (!input_.poll()) running_ = false;
    gba_mvii_set_keys(machine_, input_.keyinput());
}

void Runtime::pump_audio() {
    // Drained even with no DAC: the core's queue is unbounded, and an emulator
    // that never empties it grows for as long as it runs.
    if (!audio_.enabled()) {
        (void)gba_mvii_drain_audio(machine_, nullptr, 0);
        return;
    }
    // One call, never a loop. The core empties its queue whether or not all of
    // it fit, and that is the behaviour we want: a backlog kept for the next
    // frame would drift audio behind video forever rather than glitch once.
    audio_.submit(audio_buffer_,
                  gba_mvii_drain_audio(machine_, audio_buffer_, kAudioBufferSamples));
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
        const char* lines[] = {"GBA RUNTIME", "", "LOOKING FOR THE CARTRIDGE..."};
        video_.message(0x10, 0x10, 0x18, lines, 3);
    }
}

void Runtime::hold_failure() {
    if (!video_.opened()) return;

    std::vector<const char*> lines;
    for (const std::string& n : notes_) lines.push_back(n.c_str());
    lines.push_back("");
    lines.push_back("PRESS ESC TO CLOSE");

    // Long enough to read a directory listing and copy a filename down, and it
    // ends the moment the user asks it to. Repainting every frame rather than
    // once is deliberate: the compositor owns the buffer pair, so a guest that
    // presents once and then sleeps can have its image exchanged out from
    // under it by an unrelated window.
    //
    // Counted in rounds as well as against the clock. Thirty seconds is a wait
    // the user is expected to sit through, so it is the one place where a clock
    // that does not advance would hold the screen — and the process — for good,
    // on the failure path, where the runtime is least able to say so.
    constexpr uint64_t kHoldUs        = 30000000ull;
    constexpr uint64_t kHoldStepUs    = 100000ull;
    constexpr unsigned kHoldMaxRounds = kHoldUs / kHoldStepUs;

    const uint64_t until = gbamvii::now_us() + kHoldUs;
    for (unsigned round = 0; round < kHoldMaxRounds; ++round) {
        if (gbamvii::now_us() >= until) break;
        video_.message(0x50, 0x0C, 0x0C, lines.data(), static_cast<int>(lines.size()));
        if (!input_.poll()) break;
        if (!gbamvii::sleep_until(gbamvii::now_us() + kHoldStepUs)) break;
    }
}

// Say what the machine is doing when it has gone a quarter of a million
// instructions without finishing a frame.
//
// A guest that emulates and never draws is indistinguishable from one that is
// wedged, from anywhere outside the process — the compositor only sees that
// nothing was presented. These lines are the difference between guessing and
// knowing: a clock that stands still, a clock that runs while the scanline
// counter does not, and a CPU halted on an interrupt nobody raises are three
// different bugs that look the same from the window.
//
// Rate-limited to one line every two seconds, and only ever reached from the
// stall branch, so a healthy run never pays for it: MVII puts stderr on the
// serial console a byte at a time, which is far too slow for the frame path.
void Runtime::report_stall(uint64_t now) {
    if (now < stall_report_us_) return;
    stall_report_us_ = now + kStallReportUs;

    uint32_t p[GBA_MVII_PROBE_WORDS] = {0};
    gba_mvii_probe(machine_, p);
    const uint64_t clock = (static_cast<uint64_t>(p[3]) << 32) | p[2];
    logf("gba: stalled pc=%08x cpsr=%08x clk=%llu (+%llu) frames=%u "
         "line=%u dispcnt=%04x ie=%04x if=%04x flags=%02x sp=%08x lr=%08x\n",
         static_cast<unsigned>(p[0]), static_cast<unsigned>(p[1]),
         static_cast<unsigned long long>(clock),
         static_cast<unsigned long long>(clock - stall_last_clock_),
         static_cast<unsigned>(p[4]), static_cast<unsigned>(p[6]),
         static_cast<unsigned>(p[5]), static_cast<unsigned>(p[8]),
         static_cast<unsigned>(p[9]), static_cast<unsigned>(p[7]),
         static_cast<unsigned>(p[10]), static_cast<unsigned>(p[11]));
    stall_last_clock_ = clock;
}

// One-shot host calibration, printed once at startup.
//
// Why this exists. Two independent parts of this runtime measured about an
// order of magnitude slower than they have any business being on a 1144 MHz
// Cortex-A7: the interpreter at ~610 host cycles per emulated guest
// instruction (from the slope of emu-vs-slices across a frame-rate readback),
// and the PPU at ~250 cycles per pixel (from the fixed per-frame term of the
// same fit). Those two share no code. When two unrelated things are both ~10x
// slow, the cause usually is not either of them, and optimising emulator
// internals is wasted effort until that is settled. Removing every redundant
// byte load from the fetch path — verified at the object level, 207 ldrb down
// to 81 in Machine::step — moved the frame rate by nothing, which is what a
// systemic cause predicts and an algorithmic one does not.
//
// The three numbers separate the candidates:
//
//   alu   A dependent integer chain: nothing but the core pipeline and
//         whatever now_us() measures. ~1-2 cycles/op is the expected answer.
//         Far above that means the CPU is not running at the speed cpufreq
//         reports, or now_us() does not measure what it says it does — and
//         then every "us" in every report on this device is scaled wrong,
//         including the frame rate.
//   seq   A linear sweep of a buffer well past L2, with the loads independent.
//         Measures whether the D-cache and the prefetcher are actually on.
//   rand  A pointer chase over the same buffer, each load's address depending
//         on the previous load's value, so the prefetcher cannot help. This is
//         the access pattern of fetching from a 16 MB ROM. If alu and seq look
//         sane and this is hundreds of ns, the emulator is memory-latency
//         bound, and no amount of instruction selection will ever fix it —
//         the answer would be a smaller working set or a recompiler.
//
// Costs one console line and well under a second, once, before the first frame.
// Deliberately not behind a build flag: it is three numbers that make every
// future performance claim about this runtime checkable instead of inferred.
void log_host_calibration()
{
    constexpr size_t   kWords    = 512u * 1024u;   // 2 MiB — comfortably past L2
    constexpr uint32_t kAluIters = 2000000u;
    constexpr uint32_t kChase    = 200000u;

    // ALU: two dependent ops per iteration, result consumed so it survives -O3.
    const uint64_t t0 = gbamvii::now_us();
    uint32_t a = 1u;
    for (uint32_t i = 0; i < kAluIters; ++i) {
        a = a * 1664525u + 1013904223u;
        a ^= a >> 15;
    }
    const uint64_t alu_us = gbamvii::now_us() - t0;

    std::vector<uint32_t> buf;
    buf.resize(kWords);

    // A single cycle through every slot: stride is odd, so gcd(stride, 2^n) = 1
    // and the chase visits all of them. ~64 KB per hop puts each one on its own
    // page as well as its own line.
    constexpr size_t kStride = 16385u;
    for (size_t i = 0; i < kWords; ++i) {
        buf[i] = static_cast<uint32_t>((i + kStride) & (kWords - 1u));
    }

    const uint64_t t1 = gbamvii::now_us();
    uint32_t sum = 0;
    for (size_t i = 0; i < kWords; ++i) sum += buf[i];
    const uint64_t seq_us = gbamvii::now_us() - t1;

    const uint64_t t2 = gbamvii::now_us();
    uint32_t idx = 0;
    for (uint32_t i = 0; i < kChase; ++i) idx = buf[idx];
    const uint64_t rand_us = gbamvii::now_us() - t2;

    // Consume both results; otherwise -O3 is entitled to delete the loops.
    volatile uint32_t sink = a + sum + idx;
    (void)sink;

    // Integer arithmetic only: this runtime is built -mfloat-abi=soft and a
    // division here is cheaper to read than a float formatting path.
    const uint64_t alu_ops  = static_cast<uint64_t>(kAluIters) * 2ull;
    const uint64_t alu_ps   = alu_us ? (alu_us * 1000000ull) / alu_ops : 0ull;
    const uint64_t seq_mbs  = seq_us ? (static_cast<uint64_t>(kWords) * 4ull) / seq_us : 0ull;
    const uint64_t rand_ns  = kChase ? (rand_us * 1000ull) / kChase : 0ull;

    logf("gba: calib alu %llu ps/op (%llu.%02llu cyc @1144MHz) seq %llu MB/s "
         "rand %llu ns/access ... alu %llu us seq %llu us rand %llu us\n",
         static_cast<unsigned long long>(alu_ps),
         static_cast<unsigned long long>((alu_ps * 1144ull) / 1000000ull),
         static_cast<unsigned long long>(((alu_ps * 1144ull) / 10000ull) % 100ull),
         static_cast<unsigned long long>(seq_mbs),
         static_cast<unsigned long long>(rand_ns),
         static_cast<unsigned long long>(alu_us),
         static_cast<unsigned long long>(seq_us),
         static_cast<unsigned long long>(rand_us));
}

int Runtime::run() {
    const uint32_t rate = gba_mvii_audio_rate();
    audio_.open(rate, 2);   // the core mixes interleaved stereo
    gba_mvii_set_keys(machine_, input_.keyinput());

    logf("gba: running (%ux%u, audio %u Hz %s%s, %s BIOS)\n",
         static_cast<unsigned>(gba_mvii_screen_width()),
         static_cast<unsigned>(gba_mvii_screen_height()),
         static_cast<unsigned>(rate), audio_.enabled() ? "stereo" : "off",
#if defined(GBA_MVII_NATIVE_AOT)
         " · statically recompiled",
#else
         " · interpreter",
#endif
         bios_.empty() ? "HLE" : "real");
#if defined(GBA_MVII_NATIVE_AOT)
    // Zero here means the .virtua carries no recompiled code at all, in which
    // case every dispatch will bridge and the run is an interpreter run wearing
    // the AOT build's name. Worth knowing in the first line of the log rather
    // than five seconds later from the frame rate.
    logf("gba: %u recompiled entries linked\n",
         static_cast<unsigned>(gba_mvii_native_block_count()));
#endif

    log_host_calibration();
    const uint32_t profiler_clock_ns = gba_mvii_prof_calibrate();
    logf("gba: frame profiler armed, clock %u ns/read\n",
         static_cast<unsigned>(profiler_clock_ns));

    uint64_t next_frame_us = gbamvii::now_us() + kFrameUs;
    uint64_t report_us     = gbamvii::now_us() + 5000000ull;
    uint64_t report_frames = 0;
    save_next_probe_us_    = gbamvii::now_us() + kSaveProbeUs;
    uint32_t stalled_slices = 0;

    uint64_t span_us = gbamvii::now_us();   // start of this frame's emulate span

    while (running_) {
        // Bounded work, then a turn for everything else. run_steps returns as
        // soon as the PPU finishes a frame, so this never overshoots one.
#if defined(GBA_MVII_NATIVE_AOT)
        const uint32_t run_status = gba_mvii_run_native_blocks(machine_, kBlocksPerYield);
        if (run_status == 2u) {
            // A guest PC the static corpus does not cover. The core already
            // bridged it through the reference interpreter and recorded it, so
            // the game is still running correctly — just not natively, at that
            // address. Name it and carry on: killing a running game over a
            // coverage gap that the next recompile closes helps nobody, and the
            // whole point of recording is that the address goes home.
            //
            // Logged, not note()d, and rate-limited by the distinct count so a
            // gap inside a per-frame routine cannot flood the console. The exit
            // banner reports the totals.
            const uint32_t misses = gba_mvii_native_miss_count();
            if (misses <= kMaxReportedMisses) {
                const uint32_t miss = gba_mvii_native_miss_key();
                logf("gba: coverage miss %u at %08X (%s) — interpreted\n",
                     static_cast<unsigned>(misses),
                     static_cast<unsigned>(miss & ~1u),
                     (miss & 1u) ? "thumb" : "arm");
                if (misses == kMaxReportedMisses) {
                    logf("gba: further coverage misses counted, not listed\n");
                }
            }
        }
        const uint32_t frame_ready = (run_status == 1u) ? 1u : 0u;
#else
        const uint32_t frame_ready = gba_mvii_run_steps(machine_, kStepsPerYield);
#endif
        ++slices_;
        if ((frames_ & 63u) == 0u) {
            const uint64_t y0 = gbamvii::now_us();
            gbamvii::yield_now();
            yield_ns_ += (gbamvii::now_us() - y0) * 1000ull;
            ++yield_count_;
        } else {
            gbamvii::yield_now();
        }
        if (!frame_ready) {
            // Input is polled on frame boundaries, which is the right cadence
            // while frames are arriving — nothing on screen changes faster. A
            // guest that stops producing them (a BIOS wait loop with the LCD
            // off, or an emulation bug) would otherwise become unquittable, so
            // this catches that case and only that case: 1024 slices is a
            // quarter of a million instructions, well past two frames' worth of
            // work, so it never fires while the emulator is making progress.
            if (++stalled_slices >= kStallPollSlices) {
                stalled_slices = 0;
                if (!input_.poll()) running_ = false;
                report_stall(gbamvii::now_us());
            }
            continue;
        }
        stalled_slices = 0;

        const uint64_t t_emu = gbamvii::now_us();
        present_frame();
        const uint64_t t_present = gbamvii::now_us();
        pump_audio();

        const uint64_t now = gbamvii::now_us();
        emu_us_     += t_emu - span_us;
        present_us_ += t_present - t_emu;
        audio_us_   += now - t_present;

        flush_save_if_settled(now);

        // Pace to the GBA's refresh when we are ahead of it, and simply carry on
        // when we are behind — which on this part is the case we should expect.
        // sleep_until() parks the process, so an early frame hands its slack to
        // the rest of the box rather than spinning it away.
        span_us = now;   // next frame's emulate span starts here unless we park
        if (pacing_ok_ && now < next_frame_us) {
            if (gbamvii::sleep_until(next_frame_us)) {
                next_frame_us += kFrameUs;
                span_us = gbamvii::now_us();
                sleep_us_ += span_us - now;
            } else {
                // The clock did not reach the deadline in the time we were
                // willing to wait for it, so it is not a clock we can pace
                // against. Stop trying: the previous code looped here until the
                // deadline passed, which for a clock that is not advancing is
                // never — one park, and the guest was gone. The emulator keeps
                // running and keeps drawing, unpaced, which is a fault that can
                // be seen and reported rather than one that looks like a hang.
                pacing_ok_ = false;
                if (!warned_pacing_) {
                    warned_pacing_ = true;
                    logf("gba: frame pacing off — clock at %lluus never reached %lluus\n",
                         static_cast<unsigned long long>(now),
                         static_cast<unsigned long long>(next_frame_us));
                }
            }
        } else {
            // Do not let the deadline drift into the past and turn every
            // subsequent frame into an instant one.
            next_frame_us = now + kFrameUs;
        }

        // Two reports, and they answer different questions. This one is gated
        // on the emulator's own frame count, so it survives the clock being the
        // thing at fault — the fps line below cannot, because a clock that
        // stops takes `now >= report_us` with it and the log goes quiet exactly
        // when there is something to say. Three lines total and then silent:
        // this is startup diagnosis, not telemetry.
        if (frames_ == 1 || frames_ == 60 || frames_ == 600) {
            logf("gba: frame %llu at %lluus (pacing %s)\n",
                 static_cast<unsigned long long>(frames_),
                 static_cast<unsigned long long>(now),
                 pacing_ok_ ? "on" : "off");
        }

        if (now >= report_us) {
            const uint64_t n = (frames_ - report_frames) ? (frames_ - report_frames) : 1;
            logf("gba: %u fps — emu %llu present %llu audio %llu sleep %llu us/frame, "
                 "%llu slices/frame, yield %llu ns\n",
                 static_cast<unsigned>((frames_ - report_frames) / 5),
                 static_cast<unsigned long long>(emu_us_ / n),
                 static_cast<unsigned long long>(present_us_ / n),
                 static_cast<unsigned long long>(audio_us_ / n),
                 static_cast<unsigned long long>(sleep_us_ / n),
                 static_cast<unsigned long long>(slices_ / n),
                 static_cast<unsigned long long>(yield_count_ ? yield_ns_ / yield_count_ : 0));

            // The breakdown above accounts for everything outside the core; this
            // one splits what is inside `emu`. Drained here rather than per
            // frame because reading clears, so the window matches exactly, and
            // because one call per five seconds costs nothing measurable.
            //
            // `rest` is what the PPU does not explain — guest code, DMA, timers,
            // the mixer — and the split between it and `ppu` says whether the
            // fixed per-frame cost or the variable cost is worth attacking.
            uint32_t prof[4] = {0, 0, 0, 0};
            gba_mvii_prof_take(prof);
            const uint64_t ppu_us  = prof[0];
            const uint64_t lines   = prof[1] ? prof[1] : 1;
            const uint64_t bridged = prof[3];
            logf("gba: emu split — ppu %llu us/frame (%llu lines, %llu ns/line) "
                 "audio %llu samples/frame rest %llu us/frame\n",
                 static_cast<unsigned long long>(ppu_us / n),
                 static_cast<unsigned long long>(prof[1] / n),
                 static_cast<unsigned long long>(ppu_us * 1000ull / lines),
                 static_cast<unsigned long long>(prof[2] / n),
                 static_cast<unsigned long long>(
                     emu_us_ > ppu_us ? (emu_us_ - ppu_us) / n : 0));

            // Reported separately, and only when it is not zero, because it is
            // not a cost breakdown — it is a correctness finding wearing a
            // performance number. Every one of these instructions is guest code
            // the recompiler did not translate, executed by the interpreter
            // instead. It belongs in `rest` above and it is the first thing to
            // look at when `rest` is large: a slow frame with a five-figure
            // bridge count is not a slow emulator, it is a coverage gap.
            if (bridged) {
                logf("gba: %llu bridged guest insns/frame — %u addresses missing "
                     "from the recompiled corpus\n",
                     static_cast<unsigned long long>(bridged / n),
                     static_cast<unsigned>(gbamvii::heal_miss_count()));
            }

            report_frames = frames_;
            report_us = now + 5000000ull;
            emu_us_ = present_us_ = audio_us_ = sleep_us_ = slices_ = 0;
            yield_ns_ = yield_count_ = 0;
        }
    }

    logf("gba: stopped after %u frames\n", static_cast<unsigned>(frames_));

    // The verdict on the run, printed unconditionally — including when it is
    // clean, because "FULLY STATIC" is the result the packaging pipeline exists
    // to produce and a report that only appears on failure cannot confirm it.
    // Printed before the save flush so it is on record even if writing the save
    // is what goes wrong.
    gbamvii::report_coverage();

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
