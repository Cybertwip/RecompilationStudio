// mvii_platform.h — the MVII device surface this runtime uses.
//
// MVII gives a Virtua guest exactly seven devices (see the _open dispatch in
// OS/MVII/Kernel/Machine64/Shared/Drivers/syscalls.cpp). This runtime uses
// three of them:
//
//   /dev/fb0     RGBA8 presentation, via FB_IOCTL_MAP_RGBA8 + FB_IOCTL_PRESENT
//                when the kernel will hand out its back buffer (zero-copy), and
//                FB_IOCTL_SWAP_RGBA8 otherwise.
//   /dev/input0  a ring of TouchEvent records; KEY_DOWN/KEY_UP carry Linux
//                keycodes, which map onto the ten GBA buttons.
//   /dev/dac0    signed-16 PCM, configured through AUDIO_IOCTL_SET_CONFIG.
//
// The surface is exactly 240x160 — the GBA's own resolution — and never
// scaled here. MVII's compositor scales a guest's presented surface to the
// window's content rect on its own (Dashboard.cpp, blitScaledVirtuaRGBA8),
// and it does so through the GPU when /dev/gpu0 takes the job. Scaling in the
// guest would cost a 4x-larger frame copy per present and then be rescaled
// again anyway, so the cheapest correct frame is the smallest one.
//
// There is deliberately no file I/O in here: MVII's eMMC writes are expensive
// enough to stall the whole OS, so nothing on the frame path may touch
// storage. main.cpp reads the ROM once at startup and writes the save file only
// when the cartridge's save chip has stopped changing — see the note over
// flush_save_if_settled(), which is the one deliberate exception.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gbamvii {

// ── framebuffer ────────────────────────────────────────────────────────────

class Video {
public:
    ~Video();

    // Open /dev/fb0 and negotiate a `width` x `height` surface. Returns false
    // if the device is unavailable, in which case the caller should keep
    // running headless rather than exit — a runtime that dies because the
    // compositor is busy is worse than one that shows nothing for a frame.
    bool open(int width, int height);

    // Blit one frame of exactly width() x height() pixels and present it.
    //
    // The source is the GBA's own format — BGR555, red in the low five bits —
    // because that is what the PPU writes and converting on the way out is one
    // pass instead of two. The 5-to-8-bit expansion is computed per pixel, not
    // looked up: a 32768-entry table is 128 KB against this Cortex-A7's 32 KB
    // L1D, so it missed on most pixels and evicted the interpreter's working
    // set once a frame on top of that. See rgba8_from_bgr555 in the .cpp.
    void present(const uint16_t* bgr555);

    // Present a flat colour. Writes straight into the surface, so it costs no
    // source buffer — worth having, because a 240x160 RGB24 scratch frame is
    // 115 KB and two of them as file-scope arrays put 230 KB of dead weight in
    // the packaged image.
    void fill(uint8_t r, uint8_t g, uint8_t b);

    // Present `count` lines of text on a flat background.
    //
    // This exists because MVII shows a guest's stderr only in the Terminal
    // view, and only while it is attached to that process — so an app launched
    // from the dashboard that fails at startup has no way to say why. It used
    // to paint a flat red and exit, which told the user precisely that
    // something went wrong and nothing about what. 39 columns of 5x7 glyphs is
    // enough for a path, and a path is usually the whole answer.
    //
    // Lines longer than the surface wrap onto the next row rather than being
    // clipped: the interesting end of a path is the right-hand end.
    void message(uint8_t r, uint8_t g, uint8_t b,
                 const char* const* lines, int count);

    // Characters that fit across the surface, for callers that want to lay out
    // their own text.
    int text_columns() const;

    int width()  const { return width_; }
    int height() const { return height_; }
    bool opened() const { return fd_ >= 0; }

private:
    // Re-acquire the mapped back buffer. The kernel exchanges the buffer pair
    // on every present, so the pointer from the previous frame is aimed at the
    // image currently on screen and must be re-mapped rather than reused.
    bool remap();

    // The buffer the next frame goes into, and its stride in pixels. Null when
    // there is nowhere to draw.
    uint8_t* surface(int& span_pixels);

    // Hand the surface to the compositor and get the next one ready.
    void flush();

    int       fd_      = -1;
    int       width_   = 0;
    int       height_  = 0;
    uint8_t*  mapped_  = nullptr;  // kernel back buffer, when MAP_RGBA8 works
    int       pitch_   = 0;        // mapped_ stride, in pixels
    uint8_t*  staging_ = nullptr;  // owned fallback buffer for SWAP_RGBA8
};

// ── input ──────────────────────────────────────────────────────────────────

// Drains /dev/input0 and maintains the GBA KEYINPUT word. Active-low: a set
// bit means released, which is what the hardware register reports.
class Input {
public:
    ~Input();

    bool open();

    // Drain every pending event. Returns false when the user asked to quit.
    bool poll();

    uint16_t keyinput() const { return keyinput_; }

private:
    int      fd_       = -1;
    uint16_t keyinput_ = 0x03FFu;
    bool     quit_     = false;
};

// ── audio ──────────────────────────────────────────────────────────────────

// The core mixes interleaved stereo signed-16 at 65536 Hz — 16777216 / 256,
// AUDIO_SAMPLE_CYCLES in gba-core's mem.rs. We hand the DAC that rate and let
// the kernel resample; if the device rejects it, audio is simply disabled — a
// silent game is far better than a dead one.
class Audio {
public:
    ~Audio();

    bool open(uint32_t sample_rate, uint8_t channels);
    bool enabled() const { return fd_ >= 0; }

    // Best-effort write of `sample_count` int16 samples (already interleaved).
    // Short writes are dropped rather than retried: blocking here would stall
    // emulation and, with it, everything else MVII cooperatively schedules.
    void submit(const int16_t* samples, std::size_t sample_count);

private:
    int fd_ = -1;
};

// ── time / scheduling ──────────────────────────────────────────────────────

// Microseconds from MVII's clock. Both CLOCK_MONOTONIC and gettimeofday land
// on the same kernel counter here, so the two are interchangeable; this is the
// one every deadline in the runtime is expressed in.
//
// Guaranteed to never go backwards and never to repeat a reading when the
// underlying clock refuses one. Every wait in this runtime is `while (now <
// deadline)`, so a reading that stands still turns a 16 ms frame pause into a
// permanent one — and a guest parked on a cooperative scheduler is never
// preempted back out of it. From the outside that is indistinguishable from a
// crash: healthy heartbeat, sane wake deadline, a window that never updates
// again. A clock that is slightly wrong is far cheaper than one that is stuck.
uint64_t now_us();

// Offer the CPU back to the MVII cooperative scheduler.
//
// Call this often and do NOT gate it on a clock of our own. MVII's yield is a
// clock read and a compare on the common path and returns immediately while the
// process still owns its slice; the round it is cut from is 120 Hz, so calling
// it every few hundred instructions is what actually delivers the guaranteed
// 120 turns a second to everything else on the box. The kernel says this in so
// many words over yieldUserProcessCooperatively(): a call site that second-
// guesses it with a private interval can hold the CPU for two full slices
// before it so much as asks, which is exactly the bug an 8.3 ms rasterizer
// interval caused there. Yielding on frame boundaries instead would tie the
// whole system's responsiveness to the emulated frame rate, which on a
// Cortex-A7 interpreting ARM7TDMI is the one number we cannot promise.
void yield_now();

// Sleep until `deadline_us`, yielding cooperatively; returns immediately if
// the deadline has already passed.
//
// Returns true when the deadline was actually reached and false when this gave
// up waiting for it. The wait is bounded, and by how much sleep it has *asked
// for* rather than by how much the clock says has elapsed — the clock being the
// one input a waiting function cannot use to check itself. Two things it
// refuses to do, both of which park a guest permanently:
//
//   * a deadline further out than any frame pause could be, which is what a
//     caller that mixed two clock domains hands over (the kernel clamps guest
//     sleeps to one second for the same reason — see minos_user_sleep_until_us);
//   * a deadline the clock is not advancing towards, where each round sleeps,
//     wakes, re-reads the same time and sleeps again.
//
// A false return is a fault report, not a timeout to retry: the caller should
// stop pacing against this clock rather than call again.
bool sleep_until(uint64_t deadline_us);

// Write a line to stderr, which MVII surfaces in the app log. Cheap, but not
// free: call it on startup and failure paths, never per frame.
void logf(const char* fmt, ...);

}  // namespace gbamvii
