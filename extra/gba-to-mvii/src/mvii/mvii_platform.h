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
// storage. main.cpp reads the ROM once at startup and writes the save file
// only on exit.

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

    // Blit one RGB24 frame of exactly width() x height() pixels and present it.
    void present(const uint8_t* rgb24);

    // Present a flat colour. Writes straight into the surface, so it costs no
    // source buffer — worth having, because a 240x160 RGB24 scratch frame is
    // 115 KB and two of them as file-scope arrays put 230 KB of dead weight in
    // the packaged image.
    void fill(uint8_t r, uint8_t g, uint8_t b);

    int width()  const { return width_; }
    int height() const { return height_; }
    bool opened() const { return fd_ >= 0; }

private:
    // Re-acquire the mapped back buffer. The kernel exchanges the buffer pair
    // on every present, so the pointer from the previous frame is aimed at the
    // image currently on screen and must be re-mapped rather than reused.
    bool remap();

    int      fd_      = -1;
    int      width_   = 0;
    int      height_  = 0;
    uint8_t* mapped_  = nullptr;  // kernel back buffer, when MAP_RGBA8 works
    int      pitch_   = 0;        // mapped_ stride, in pixels
    uint8_t* staging_ = nullptr;  // owned fallback buffer for SWAP_RGBA8
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

// The GBA mixer produces interleaved stereo signed-16 at 32768 Hz (16777216 /
// 512). We hand the DAC that rate and let the kernel resample; if the device
// rejects it, audio is simply disabled — a silent game is far better than a
// dead one.
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
void sleep_until(uint64_t deadline_us);

// Write a line to stderr, which MVII surfaces in the app log. Cheap, but not
// free: call it on startup and failure paths, never per frame.
void logf(const char* fmt, ...);

}  // namespace gbamvii
