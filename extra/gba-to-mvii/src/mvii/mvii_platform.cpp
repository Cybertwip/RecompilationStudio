#include "mvii_platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "media.h"  // MVII device ABI: FB_IOCTL_*, pcm_config, TouchEvent

namespace gbamvii {
namespace {

// The kernel's user framebuffer is a fixed 1280-pixel-wide store whatever
// visible size a guest asks for (kUserFbMaxWidth in syscalls.cpp), and
// FB_IOCTL_MAP_RGBA8 reports that in pitch_pixels. Nothing here assumes the
// value — it is read back from the map — but a sane default matters for the
// staging path, which owns its own tightly-packed buffer instead.
constexpr uint16_t kMapFlagDirectSurface = 1u;

// Linux input keycodes, as MVII forwards them in TouchEvent.x. The J36's
// physical buttons arrive already translated by mt6592_usb.c's k_local_keys[]
// table, so these are the codes a handheld actually produces — not an
// abstraction over them.
enum : uint16_t {
    KC_ESC       = 0x001,
    KC_BACKSPACE = 0x00e,
    KC_TAB       = 0x00f,
    KC_ENTER     = 0x01c,
    KC_A         = 0x01e,
    KC_S         = 0x01f,
    KC_Z         = 0x02c,
    KC_X         = 0x02d,
    KC_SPACE     = 0x039,
    KC_UP        = 0x067,
    KC_PAGEUP    = 0x068,
    KC_LEFT      = 0x069,
    KC_RIGHT     = 0x06a,
    KC_DOWN      = 0x06c,
    KC_PAGEDOWN  = 0x06d,
};

// GBA KEYINPUT (0x04000130) bit assignments. Active low.
enum : uint16_t {
    GBA_A      = 1u << 0,
    GBA_B      = 1u << 1,
    GBA_SELECT = 1u << 2,
    GBA_START  = 1u << 3,
    GBA_RIGHT  = 1u << 4,
    GBA_LEFT   = 1u << 5,
    GBA_UP     = 1u << 6,
    GBA_DOWN   = 1u << 7,
    GBA_R      = 1u << 8,
    GBA_L      = 1u << 9,
};

// One keycode can drive more than one control and two keycodes can drive the
// same one, so this is a list rather than a switch: the J36's X/Y buttons and a
// USB keyboard's Z/X both need to reach GBA A and B.
//
// The device's own A and B buttons are absent on purpose. mt6592_usb.c maps
// them to BTN_LEFT/BTN_RIGHT — they are the pointer buttons, and the shell
// turns them into touches and a context menu before a guest ever sees them.
// The touch that A produces is folded into GBA A below, which is why tapping
// the screen works too.
struct KeyBinding {
    uint16_t code;
    uint16_t gba_bits;
};

constexpr KeyBinding kBindings[] = {
    {KC_UP,        GBA_UP},
    {KC_DOWN,      GBA_DOWN},
    {KC_LEFT,      GBA_LEFT},
    {KC_RIGHT,     GBA_RIGHT},
    {KC_SPACE,     GBA_A},        // J36 X
    {KC_Z,         GBA_A},        // keyboard
    {KC_TAB,       GBA_B},        // J36 Y
    {KC_X,         GBA_B},        // keyboard
    {KC_PAGEUP,    GBA_L},        // J36 L1
    {KC_A,         GBA_L},        // keyboard
    {KC_PAGEDOWN,  GBA_R},        // J36 R1
    {KC_S,         GBA_R},        // keyboard
    {KC_ENTER,     GBA_START},    // J36 START
    {KC_BACKSPACE, GBA_SELECT},   // J36 SELECT
};

uint16_t bits_for_keycode(uint16_t code) {
    uint16_t bits = 0;
    for (const KeyBinding& b : kBindings) {
        if (b.code == code) bits |= b.gba_bits;
    }
    return bits;
}

}  // namespace

// ── Video ──────────────────────────────────────────────────────────────────

Video::~Video() {
    delete[] staging_;
    staging_ = nullptr;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Video::open(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    width_  = width;
    height_ = height;

    fd_ = ::open("/dev/fb0", O_RDWR);
    if (fd_ < 0) {
        logf("gba: /dev/fb0 unavailable (errno %d); running without video\n", errno);
        return false;
    }

    if (!remap()) {
        // No shared surface: fall back to staging + FB_IOCTL_SWAP_RGBA8, which
        // costs one extra full-frame copy through the kernel. At 240x160 that
        // is 150 KB a frame, so it is a real but survivable loss.
        staging_ = new (std::nothrow) uint8_t[static_cast<std::size_t>(width_) * height_ * 4];
        if (!staging_) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        logf("gba: framebuffer mapped surface unavailable; using SWAP_RGBA8\n");
    }
    return true;
}

bool Video::remap() {
    if (fd_ < 0) return false;
    fb_map_rgba8 map{};
    map.width  = static_cast<uint16_t>(width_);
    map.height = static_cast<uint16_t>(height_);
    if (::ioctl(fd_, FB_IOCTL_MAP_RGBA8, &map) != 0) return false;
    if (!map.data || (map.flags & kMapFlagDirectSurface) == 0) return false;
    if (map.pitch_pixels < width_) return false;
    mapped_ = map.data;
    pitch_  = map.pitch_pixels;
    return true;
}

void Video::present(const uint8_t* rgb24) {
    if (fd_ < 0 || !rgb24) return;

    uint8_t* dst   = mapped_ ? mapped_ : staging_;
    const int span = mapped_ ? pitch_ : width_;
    if (!dst) return;

    // RGB24 -> RGBA8, one row at a time. The destination row stride is the
    // kernel's, not ours, so this cannot be a single linear pass.
    for (int y = 0; y < height_; ++y) {
        const uint8_t* src = rgb24 + static_cast<std::size_t>(y) * width_ * 3;
        uint8_t* out = dst + static_cast<std::size_t>(y) * span * 4;
        for (int x = 0; x < width_; ++x) {
            out[0] = src[0];
            out[1] = src[1];
            out[2] = src[2];
            out[3] = 0xFF;
            src += 3;
            out += 4;
        }
    }

    if (mapped_) {
        (void)::ioctl(fd_, FB_IOCTL_PRESENT, nullptr);
        // The pair was just exchanged, so the pointer we held is now the image
        // on screen. Re-map, and if the kernel has stopped handing out the
        // surface, drop to the staging path rather than drawing into a frame
        // the compositor is reading.
        if (!remap()) {
            mapped_ = nullptr;
            if (!staging_) {
                staging_ = new (std::nothrow)
                    uint8_t[static_cast<std::size_t>(width_) * height_ * 4];
            }
        }
        return;
    }

    fb_draw_rgba8 draw{};
    draw.x = 0;
    draw.y = 0;
    draw.w = static_cast<uint16_t>(width_);
    draw.h = static_cast<uint16_t>(height_);
    draw.data = staging_;
    (void)::ioctl(fd_, FB_IOCTL_SWAP_RGBA8, &draw);
}

void Video::fill(uint8_t r, uint8_t g, uint8_t b) {
    if (fd_ < 0) return;
    uint8_t* dst   = mapped_ ? mapped_ : staging_;
    const int span = mapped_ ? pitch_ : width_;
    if (!dst) return;

    for (int y = 0; y < height_; ++y) {
        uint8_t* out = dst + static_cast<std::size_t>(y) * span * 4;
        for (int x = 0; x < width_; ++x) {
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = 0xFF;
            out += 4;
        }
    }

    if (mapped_) {
        (void)::ioctl(fd_, FB_IOCTL_PRESENT, nullptr);
        if (!remap()) {
            mapped_ = nullptr;
            if (!staging_) {
                staging_ = new (std::nothrow)
                    uint8_t[static_cast<std::size_t>(width_) * height_ * 4];
            }
        }
        return;
    }

    fb_draw_rgba8 draw{};
    draw.x = 0;
    draw.y = 0;
    draw.w = static_cast<uint16_t>(width_);
    draw.h = static_cast<uint16_t>(height_);
    draw.data = staging_;
    (void)::ioctl(fd_, FB_IOCTL_SWAP_RGBA8, &draw);
}

// ── Input ──────────────────────────────────────────────────────────────────

Input::~Input() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Input::open() {
    fd_ = ::open("/dev/input0", O_RDONLY | O_NONBLOCK);
    if (fd_ < 0) {
        logf("gba: /dev/input0 unavailable (errno %d); running without input\n", errno);
        return false;
    }
    return true;
}

bool Input::poll() {
    if (fd_ < 0) return !quit_;

    // Drain the whole ring. A bounded loop, because a held auto-repeating key
    // refills it while we read and an unbounded drain would never come back.
    for (int guard = 0; guard < 256; ++guard) {
        TouchEvent ev{};
        const ssize_t got = ::read(fd_, &ev, sizeof(ev));
        if (got != static_cast<ssize_t>(sizeof(ev))) break;

        switch (ev.type) {
        case KEY_DOWN: {
            if (ev.x == KC_ESC) { quit_ = true; break; }
            keyinput_ &= static_cast<uint16_t>(~bits_for_keycode(ev.x));
            break;
        }
        case KEY_UP:
            keyinput_ |= bits_for_keycode(ev.x);
            break;
        case TOUCH_BEGIN:
        case TOUCH_MOVE:
            keyinput_ &= static_cast<uint16_t>(~GBA_A);
            break;
        case TOUCH_END:
            keyinput_ |= GBA_A;
            break;
        default:
            break;
        }
    }
    return !quit_;
}

// ── Audio ──────────────────────────────────────────────────────────────────

Audio::~Audio() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Audio::open(uint32_t sample_rate, uint8_t channels) {
    fd_ = ::open("/dev/dac0", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        logf("gba: /dev/dac0 unavailable (errno %d); running silent\n", errno);
        return false;
    }
    pcm_config config{};
    config.sample_rate     = sample_rate;
    config.channels        = channels;
    config.bits_per_sample = 16;
    if (::ioctl(fd_, AUDIO_IOCTL_SET_CONFIG, &config) != 0) {
        logf("gba: DAC rejected %u Hz / %u ch (errno %d); running silent\n",
             static_cast<unsigned>(sample_rate), static_cast<unsigned>(channels), errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

void Audio::submit(const int16_t* samples, std::size_t sample_count) {
    if (fd_ < 0 || !samples || sample_count == 0) return;
    const std::size_t bytes = sample_count * sizeof(int16_t);
    const ssize_t wrote = ::write(fd_, samples, bytes);
    // A short or refused write means the DAC ring is full: the emulator is
    // ahead of playback, and the right response is to drop the excess. Retrying
    // would park the whole runtime on an audio device.
    (void)wrote;
}

// ── time / scheduling ──────────────────────────────────────────────────────

uint64_t now_us() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

void yield_now() { (void)sched_yield(); }

void sleep_until(uint64_t deadline_us) {
    for (;;) {
        const uint64_t now = now_us();
        if (now >= deadline_us) return;
        const uint64_t remaining = deadline_us - now;
        // usleep() lands on MVII's sleep_until_us syscall, which parks the
        // process and lets the scheduler run everything else — that is what
        // makes an idle frame cheap instead of a spin. Short waits are handed
        // to the yield instead: parking for a few hundred microseconds costs
        // more in scheduler round-trip than it saves.
        if (remaining > 1000ull) {
            ::usleep(static_cast<useconds_t>(remaining - 500ull));
        } else {
            (void)sched_yield();
        }
    }
}

void logf(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n <= 0) return;
    const std::size_t len =
        static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n)
                                                   : sizeof(line) - 1;
    (void)::write(2, line, len);
}

}  // namespace gbamvii
