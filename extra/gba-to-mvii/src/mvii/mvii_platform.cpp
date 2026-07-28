#include "mvii_platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
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

// ── the smallest font that can print a path ────────────────────────────────
//
// ASCII 0x20..0x5F — space, punctuation, digits, uppercase. Lowercase is
// folded to uppercase on the way in, which costs nothing and halves the table.
// Five bytes a glyph, one per column, bit 0 at the top; the sixth column is
// the inter-character gap and is not stored. 320 bytes of .rodata, which the
// .virtua packager carries in the blob it already carries.
constexpr int kGlyphWidth   = 5;
constexpr int kGlyphHeight  = 7;
constexpr int kGlyphAdvance = kGlyphWidth + 1;
constexpr int kLineAdvance  = kGlyphHeight + 2;
constexpr int kTextMargin   = 2;

constexpr uint8_t kFont5x7[][kGlyphWidth] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00},  // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62},  // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50},  // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '\''
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14},  // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ','
    {0x08, 0x08, 0x08, 0x08, 0x08},  // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00},  // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02},  // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46},  // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39},  // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03},  // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36},  // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00},  // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00},  // ';'
    {0x00, 0x08, 0x14, 0x22, 0x41},  // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14},  // '='
    {0x41, 0x22, 0x14, 0x08, 0x00},  // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06},  // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E},  // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // 'E'
    {0x7F, 0x09, 0x09, 0x01, 0x01},  // 'F'
    {0x3E, 0x41, 0x41, 0x51, 0x32},  // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // 'L'
    {0x7F, 0x02, 0x04, 0x02, 0x7F},  // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31},  // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // 'V'
    {0x7F, 0x20, 0x18, 0x20, 0x7F},  // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63},  // 'X'
    {0x03, 0x04, 0x78, 0x04, 0x03},  // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43},  // 'Z'
    {0x00, 0x00, 0x7F, 0x41, 0x41},  // '['
    {0x02, 0x04, 0x08, 0x10, 0x20},  // '\\'
    {0x41, 0x41, 0x7F, 0x00, 0x00},  // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04},  // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40},  // '_'
};

constexpr int kFontFirst = 0x20;
constexpr int kFontCount = static_cast<int>(sizeof(kFont5x7) / sizeof(kFont5x7[0]));

const uint8_t* glyph_for(char ch) {
    int c = static_cast<unsigned char>(ch);
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (c < kFontFirst || c >= kFontFirst + kFontCount) c = '?';
    return kFont5x7[c - kFontFirst];
}

}  // namespace

// ── Video ──────────────────────────────────────────────────────────────────

Video::~Video() {
    delete[] staging_;
    staging_ = nullptr;
    delete[] lut_;
    lut_ = nullptr;
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

uint8_t* Video::surface(int& span_pixels) {
    if (fd_ < 0) return nullptr;
    span_pixels = mapped_ ? pitch_ : width_;
    return mapped_ ? mapped_ : staging_;
}

void Video::flush() {
    if (fd_ < 0) return;
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
    if (!staging_) return;

    fb_draw_rgba8 draw{};
    draw.x = 0;
    draw.y = 0;
    draw.w = static_cast<uint16_t>(width_);
    draw.h = static_cast<uint16_t>(height_);
    draw.data = staging_;
    (void)::ioctl(fd_, FB_IOCTL_SWAP_RGBA8, &draw);
}

// BGR555 -> RGBA8, computed rather than looked up.
//
// The obvious implementation of this is a 32768-entry table, and that is what
// it was. It is the wrong shape for this part: 128 KB indexed by pixel value
// against a Cortex-A7's 32 KB four-way L1D means most pixels are an L2 hit, and
// an L2 hit here costs more than the dozen single-cycle ALU ops it was there to
// avoid. Doing the arithmetic touches no memory at all, so it also stops the
// conversion evicting the emulator's own working set once per frame — which is
// the larger of the two effects, since the interpreter is what sets the frame
// rate and it runs 150k instructions between presents.
//
// The 5-to-8-bit expansion replicates the top bits (v << 3 | v >> 2) so that
// 0x1F reaches 0xFF rather than 0xF8, which is what keeps whites white.
// Little-endian: byte 0 is R, byte 1 G, byte 2 B, byte 3 A — the channel order
// the kernel's RGBA8 surface expects. Red is in the LOW five bits of the
// source, because that is the GBA's own layout, not the framebuffer's.
static inline uint32_t rgba8_from_bgr555(uint16_t v) {
    const uint32_t r5 = v & 0x1Fu;
    const uint32_t g5 = (v >> 5) & 0x1Fu;
    const uint32_t b5 = (v >> 10) & 0x1Fu;
    return ((r5 << 3) | (r5 >> 2)) |
           (((g5 << 3) | (g5 >> 2)) << 8) |
           (((b5 << 3) | (b5 >> 2)) << 16) |
           0xFF000000u;
}

void Video::present(const uint16_t* bgr555) {
    if (!bgr555) return;
    int span = 0;
    uint8_t* dst = surface(span);
    if (!dst) return;

    // One row at a time: the destination stride is the kernel's (1280 pixels
    // for the shared surface), not ours, so this cannot be a single linear
    // pass. Writing 32 bits at a time rather than four bytes matters here —
    // this loop runs 38400 times a frame.
    for (int y = 0; y < height_; ++y) {
        const uint16_t* src = bgr555 + static_cast<std::size_t>(y) * width_;
        uint32_t* out = reinterpret_cast<uint32_t*>(dst) +
                        static_cast<std::size_t>(y) * span;
        for (int x = 0; x < width_; ++x) {
            out[x] = rgba8_from_bgr555(src[x]);
        }
    }
    flush();
}

void Video::fill(uint8_t r, uint8_t g, uint8_t b) {
    int span = 0;
    uint8_t* dst = surface(span);
    if (!dst) return;

    const uint32_t packed = static_cast<uint32_t>(r) |
                            (static_cast<uint32_t>(g) << 8) |
                            (static_cast<uint32_t>(b) << 16) | 0xFF000000u;
    for (int y = 0; y < height_; ++y) {
        uint32_t* out = reinterpret_cast<uint32_t*>(dst) +
                        static_cast<std::size_t>(y) * span;
        for (int x = 0; x < width_; ++x) out[x] = packed;
    }
    flush();
}

int Video::text_columns() const {
    const int usable = width_ - 2 * kTextMargin;
    return usable > 0 ? usable / kGlyphAdvance : 0;
}

void Video::message(uint8_t r, uint8_t g, uint8_t b,
                    const char* const* lines, int count) {
    int span = 0;
    uint8_t* dst = surface(span);
    if (!dst) return;

    const uint32_t bg = static_cast<uint32_t>(r) |
                        (static_cast<uint32_t>(g) << 8) |
                        (static_cast<uint32_t>(b) << 16) | 0xFF000000u;
    for (int y = 0; y < height_; ++y) {
        uint32_t* out = reinterpret_cast<uint32_t*>(dst) +
                        static_cast<std::size_t>(y) * span;
        for (int x = 0; x < width_; ++x) out[x] = bg;
    }

    const int columns = text_columns();
    const int rows    = (height_ - 2 * kTextMargin) / kLineAdvance;
    if (columns <= 0 || rows <= 0) { flush(); return; }

    constexpr uint32_t kInk = 0xFFFFFFFFu;
    int row = 0;
    for (int i = 0; i < count && row < rows; ++i) {
        const char* text = lines[i] ? lines[i] : "";
        int consumed = 0;
        // An empty line is a blank row, not a skipped one: it is how the
        // caller separates the headline from the detail.
        do {
            const int top = kTextMargin + row * kLineAdvance;
            for (int col = 0; col < columns && text[consumed]; ++col, ++consumed) {
                const uint8_t* glyph = glyph_for(text[consumed]);
                const int left = kTextMargin + col * kGlyphAdvance;
                for (int gx = 0; gx < kGlyphWidth; ++gx) {
                    const uint8_t bits = glyph[gx];
                    if (!bits) continue;
                    for (int gy = 0; gy < kGlyphHeight; ++gy) {
                        if ((bits & (1u << gy)) == 0) continue;
                        uint32_t* out = reinterpret_cast<uint32_t*>(dst) +
                                        static_cast<std::size_t>(top + gy) * span +
                                        (left + gx);
                        *out = kInk;
                    }
                }
            }
            ++row;
        } while (text[consumed] && row < rows);
    }
    flush();
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
            keyinput_ &= static_cast<uint16_t>(~GBA_A);
            break;
        case TOUCH_MOVE:
            // Deliberately not a press. MVII's TouchEvent ABI has no hover
            // event, so the shell forwards a bare PointerMove as TOUCH_MOVE
            // too — and on the J36 the d-pad drives the pointer as well as the
            // direction keys (local_pointer_pump in mt6592_usb.c). Treating a
            // move as a begin therefore latched A down the instant the player
            // touched the d-pad, with no TOUCH_END ever coming to lift it. A
            // drag that started on the screen is already held by the
            // TOUCH_BEGIN above, so nothing is lost by ignoring this.
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
    // Returning 0 on a failed read — which is what this used to do — is the
    // worst possible answer: 0 is below every deadline in the runtime, so a
    // clock that stops answering reads as "the deadline is still ahead of us",
    // forever. Carry the last good reading instead and advance it, so the
    // failure costs pacing accuracy rather than the process. See the note in
    // the header for why standing still is the specific hazard here.
    //
    // A static local is fine: a .virtua image is one process with one thread,
    // and the toolchain builds with -fno-threadsafe-statics.
    static uint64_t last_us = 0;

    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        const uint64_t us = static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
                            static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
        // Clamped forward only. A clock that jumps backwards would otherwise
        // push every outstanding deadline out of reach by the size of the jump,
        // which is the same freeze by a different route — and MVII's clock can
        // change domain underneath a running guest, because sys_gettimeofday()
        // answers uptime until the kernel finds a wall clock and the date from
        // then on.
        if (us > last_us) last_us = us;
        return last_us;
    }
    return ++last_us;
}

void yield_now() { (void)sched_yield(); }

void sleepFor(std::chrono::nanoseconds duration)
{
    if (duration.count() <= 0) return;
    timespec ts {};
    ts.tv_sec = static_cast<time_t>(duration.count() / std::nano::den);
    ts.tv_nsec = static_cast<long>(duration.count() % std::nano::den);
    nanosleep(&ts, nullptr);
}

// Nothing here waits longer than this in one call, whatever it is asked for.
// One second is the kernel's own ceiling on a guest sleep, for the same reason:
// no real wait a guest has — frame pacing, a condvar poll, sleep(1) — is longer,
// so anything beyond it is a broken deadline rather than an intended one.
constexpr uint64_t kSleepMaxWaitUs = 1000000ull;

// Belt to the budget's braces. The budget below is denominated in microseconds
// asked for, which only grows on the sleeping branch; the yielding branch can
// spin without adding to it, so bound the rounds as well. 4096 yields is far
// more than any single frame pause needs and still returns promptly.
constexpr unsigned kSleepMaxRounds = 4096;

bool sleep_until(uint64_t deadline_us)
{
    const uint64_t start = now_us();
    if (start >= deadline_us) return true;

    // Refuse an implausible deadline outright rather than discovering it a
    // second at a time. This is the mixed-clock-domain case: an absolute
    // realtime deadline handed to a monotonic wait, or microseconds where
    // nanoseconds were meant.
    if (deadline_us - start > kSleepMaxWaitUs) return false;

    // How much sleep this call is willing to ask for before concluding the
    // clock is not moving. Twice the wait, plus a floor so that a sub-
    // millisecond deadline still gets a few rounds to land. Measured in
    // requested microseconds on purpose: elapsed time is what a stopped clock
    // misreports, so a budget denominated in it would never be spent.
    const uint64_t budget_us = (deadline_us - start) * 2ull + 2000ull;
    uint64_t asked_us = 0;

    for (unsigned round = 0; round < kSleepMaxRounds; ++round)
    {
        const uint64_t now = now_us();
        if (now >= deadline_us)
            return true;
        if (asked_us >= budget_us)
            return false;

        const uint64_t remaining = deadline_us - now;

        // usleep() lands on MVII's sleep_until_us syscall, which parks the
        // process and lets the scheduler run everything else — that is what
        // makes an idle frame cheap instead of a spin. Short waits are handed
        // to the yield instead: parking for a few hundred microseconds costs
        // more in scheduler round-trip than it saves.
        if (remaining > 1000ull)
        {
            // sleep a little less than the remaining time so we don't overshoot
            const uint64_t ask = remaining - 500ull;
            sleepFor(std::chrono::nanoseconds(ask * 1000ull));
            asked_us += ask;
        }
        else
        {
            (void)sched_yield();
        }
    }
    return false;
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

// ── hooks the Rust core calls ──────────────────────────────────────────────
//
// Declared in include/gba_mvii.h and, on the other side, in
// rust/gba-core/src/nostd.rs. They exist so that no MVII device access lives
// in Rust: the emulation core stays the reference implementation, and every
// syscall in this runtime is in this file.

extern "C" void gba_mvii_diag_write(const uint8_t* ptr, std::size_t len) {
    if (!ptr || len == 0) return;
    // Straight to stderr, which MVII copies to both the app log and the serial
    // console. Every caller is behind a trace flag this build leaves off — the
    // serial path is a byte at a time, and a per-frame trace would cost more
    // than the emulation.
    (void)::write(2, ptr, len);
}

extern "C" int64_t gba_mvii_host_epoch(void) {
    // MVII has one clock and no timezone database — localtime_r is gmtime_r
    // there (Dash/llvm_libc_stubs.cpp) — so this is already the "local" civil
    // time the cartridge RTC should report, with no conversion left to do.
    const time_t now = ::time(nullptr);
    if (now == static_cast<time_t>(-1)) return -1;
    return static_cast<int64_t>(now);
}
