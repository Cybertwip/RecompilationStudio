// bios_hle.cpp — see bios_hle.h.
//
// Ported from gba++ src/runtime/bios_hle.cpp and the standalone IRQ pair in
// src/armv4t/runtime_arm.cpp. The SWI bodies are unchanged apart from the
// register/bus accessors; keep them that way, so a divergence against the
// upstream oracle is always a porting bug in this file's top forty lines and
// never a rewritten algorithm.

#include "bios_hle.h"

#include <cmath>
#include <cstdint>

#include "bus.h"
#include "cpu_state.h"

namespace gbamvii {
namespace {

armv4t::CPUState* g_cpu = nullptr;
armv4t::Bus*      g_bus = nullptr;

bool g_intr_wait_armed = false;

// Per-nesting-level record of what was pending when an IRQ was vectored, so
// the epilogue can complete the dispatcher contract. Upstream sizes this at
// 32; a GBA that nests IRQs 32 deep has already lost, and the modulo keeps a
// runaway from walking off the array.
constexpr uint32_t kHleIrqMaxDepth = 32u;
uint32_t g_irq_src_stack[kHleIrqMaxDepth] = {};
uint16_t g_irq_ie_stack[kHleIrqMaxDepth] = {};
uint32_t g_irq_nest_depth = 0;

// ── register / memory shorthands ───────────────────────────────────────────
// The upstream originals; only their bodies differ.
inline uint32_t  R(int i)               { return g_cpu->R[i]; }
inline void      setR(int i, uint32_t v){ g_cpu->R[i] = v; }
inline uint32_t  rd32(uint32_t a)       { return g_bus->read32(a); }
inline uint16_t  rd16(uint32_t a)       { return g_bus->read16(a); }
inline uint8_t   rd8 (uint32_t a)       { return g_bus->read8 (a); }
inline void      wr32(uint32_t a, uint32_t v){ g_bus->write32(a, v); }
inline void      wr16(uint32_t a, uint16_t v){ g_bus->write16(a, v); }
inline void      wr8 (uint32_t a, uint8_t  v){ g_bus->write8 (a, v); }

inline int clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }

constexpr float kPi = 3.14159265358979323846f;

// ── CPSR (de)serialization ─────────────────────────────────────────────────
// The interpreter keeps CPSR as a bitfield but stores SPSR as the packed
// 32-bit architectural value; these are the same pack/unpack the interpreter's
// own enter_irq / exception-return paths use.
uint32_t pack_cpsr(const armv4t::CPUState& c) {
    uint32_t v = 0;
    if (c.cpsr.n) v |= 1u << 31;
    if (c.cpsr.z) v |= 1u << 30;
    if (c.cpsr.c) v |= 1u << 29;
    if (c.cpsr.v) v |= 1u << 28;
    if (c.cpsr.i) v |= 1u << 7;
    if (c.cpsr.f) v |= 1u << 6;
    if (c.cpsr.t) v |= 1u << 5;
    return v | (c.cpsr.mode & 0x1Fu);
}

void unpack_cpsr(armv4t::CPUState& c, uint32_t v) {
    c.cpsr.n = (v >> 31) & 1u;
    c.cpsr.z = (v >> 30) & 1u;
    c.cpsr.c = (v >> 29) & 1u;
    c.cpsr.v = (v >> 28) & 1u;
    c.cpsr.i = (v >> 7)  & 1u;
    c.cpsr.f = (v >> 6)  & 1u;
    c.cpsr.t = (v >> 5)  & 1u;
    c.cpsr.mode = static_cast<uint8_t>(v & 0x1Fu);
    c.thumb = c.cpsr.t;
}

armv4t::BankedSlot mode_to_bank(uint8_t m) {
    switch (static_cast<armv4t::Mode>(m)) {
        case armv4t::Mode::FIQ:        return armv4t::Bank_FIQ;
        case armv4t::Mode::IRQ:        return armv4t::Bank_IRQ;
        case armv4t::Mode::Supervisor: return armv4t::Bank_Supervisor;
        case armv4t::Mode::Abort:      return armv4t::Bank_Abort;
        case armv4t::Mode::Undefined:  return armv4t::Bank_Undefined;
        default:                       return armv4t::Bank_User;
    }
}

// GBA SWI numbers (GBATEK).
enum : uint32_t {
    SWI_SOFT_RESET          = 0x00,
    SWI_REGISTER_RAM_RESET  = 0x01,
    SWI_HALT                = 0x02,
    SWI_STOP                = 0x03,
    SWI_INTR_WAIT           = 0x04,
    SWI_VBLANK_INTR_WAIT    = 0x05,
    SWI_DIV                 = 0x06,
    SWI_DIV_ARM             = 0x07,
    SWI_SQRT                = 0x08,
    SWI_ARCTAN              = 0x09,
    SWI_ARCTAN2             = 0x0A,
    SWI_CPU_SET             = 0x0B,
    SWI_CPU_FAST_SET        = 0x0C,
    SWI_GET_BIOS_CHECKSUM   = 0x0D,
    SWI_BG_AFFINE_SET       = 0x0E,
    SWI_OBJ_AFFINE_SET      = 0x0F,
    SWI_BIT_UNPACK          = 0x10,
    SWI_LZ77_UNCOMP_WRAM    = 0x11,
    SWI_LZ77_UNCOMP_VRAM    = 0x12,
    SWI_HUFFMAN_UNCOMP      = 0x13,
    SWI_RL_UNCOMP_WRAM      = 0x14,
    SWI_RL_UNCOMP_VRAM      = 0x15,
    SWI_DIFF_8BIT_UNFILTER_WRAM = 0x16,
    SWI_DIFF_8BIT_UNFILTER_VRAM = 0x17,
    SWI_DIFF_16BIT_UNFILTER = 0x18,
    SWI_SOUND_BIAS          = 0x19,
    SWI_MIDI_KEY_2_FREQ     = 0x1F,
};

constexpr uint32_t GBA_BIOS_CHECKSUM = 0xBAAE187Fu;
constexpr uint32_t SIZE_BIOS         = 0x4000u;
constexpr uint32_t REG_SOUNDBIAS     = 0x04000088u;

// ── boot / wait SWIs ───────────────────────────────────────────────────────
void do_register_ram_reset() {
    const uint32_t flags = R(0);
    if (flags & 0x01u) {
        for (uint32_t a = 0x02000000u; a < 0x02040000u; a += 4u) wr32(a, 0);
    }
    if (flags & 0x02u) {
        // Preserve the BIOS work area / IRQ pointer in the final 0x200 bytes.
        for (uint32_t a = 0x03000000u; a < 0x03007E00u; a += 4u) wr32(a, 0);
    }
    if (flags & 0x04u) {
        for (uint32_t a = 0x05000000u; a < 0x05000400u; a += 4u) wr32(a, 0);
    }
    if (flags & 0x08u) {
        for (uint32_t a = 0x06000000u; a < 0x06018000u; a += 4u) wr32(a, 0);
    }
    if (flags & 0x10u) {
        for (uint32_t a = 0x07000000u; a < 0x07000400u; a += 4u) wr32(a, 0);
    }
    if (flags & 0x80u) wr16(0x04000000u, 0x0080u);  // forced blank
}

void do_halt(bool stop) {
    wr8(0x04000301u, stop ? 0x80u : 0x00u);
}

void do_intr_wait(bool vblank) {
    const bool discard = vblank || R(0) != 0;
    const uint16_t mask = vblank ? 1u : static_cast<uint16_t>(R(1));
    if (!g_intr_wait_armed) {
        if (discard) {
            const uint16_t flags = rd16(0x03007FF8u);
            wr16(0x03007FF8u, static_cast<uint16_t>(flags & ~mask));
        }
        g_intr_wait_armed = true;
    }
    const uint16_t flags = rd16(0x03007FF8u);
    if ((flags & mask) != 0) {
        wr16(0x03007FF8u, static_cast<uint16_t>(flags & ~mask));
        g_intr_wait_armed = false;
        return;
    }
    // The BIOS wait loop forces IME on, halts, then re-checks this SWI after
    // the IRQ handler has ORed its source into 0x03007FF8.
    wr16(0x04000208u, 1u);
    do_halt(false);
    g_cpu->R[15] -= g_cpu->cpsr.t ? 2u : 4u;
}

// ── arithmetic SWIs ─────────────────────────────────────────────────────────

int _mulWait(int32_t r) {
    if ((r & 0xFFFFFF00) == 0xFFFFFF00 || !(r & 0xFFFFFF00)) return 1;
    if ((r & 0xFFFF0000) == 0xFFFF0000 || !(r & 0xFFFF0000)) return 2;
    if ((r & 0xFF000000) == 0xFF000000 || !(r & 0xFF000000)) return 3;
    return 4;
}

// Div (num,denom) → r0=quot, r1=rem, r3=abs(quot). Returns the cycle stall.
uint32_t do_div(int32_t num, int32_t denom) {
    if (denom == 0) {
        setR(0, (num < 0) ? 0xFFFFFFFFu : 1u);
        setR(1, static_cast<uint32_t>(num));
        setR(3, 1u);
    } else if (denom == -1 && num == INT32_MIN) {
        setR(0, static_cast<uint32_t>(INT32_MIN));
        setR(1, 0u);
        setR(3, static_cast<uint32_t>(INT32_MIN));
    } else {
        // C++ integer division truncates toward zero, which is what div()
        // guarantees; upstream used div() only for the paired quot/rem.
        const int32_t quot = num / denom;
        const int32_t rem  = num % denom;
        setR(0, static_cast<uint32_t>(quot));
        setR(1, static_cast<uint32_t>(rem));
        setR(3, static_cast<uint32_t>(quot < 0 ? -quot : quot));
    }
    int loops = clz32(static_cast<uint32_t>(denom)) - clz32(static_cast<uint32_t>(num));
    if (loops < 1) loops = 1;
    return 4u + 13u * static_cast<uint32_t>(loops) + 7u;
}

int16_t _ArcTan(int32_t i, int32_t* r1, int32_t* r3, uint32_t* cycles) {
    uint32_t c = 37;
    c += _mulWait(i * i);
    int32_t a = -((i * i) >> 14);
    c += _mulWait(0xA9 * a);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0x91C;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0xFB6;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0x16AA;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0x2081;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0x3651;
    c += _mulWait(b * a); b = ((b * a) >> 14) + 0xA2F9;
    if (r1) *r1 = a;
    if (r3) *r3 = b;
    *cycles = c;
    return static_cast<int16_t>((i * b) >> 16);
}

int16_t _ArcTan2(int32_t x, int32_t y, int32_t* r1, uint32_t* cycles) {
    if (!y) { *cycles = 11; return x >= 0 ? 0 : static_cast<int16_t>(0x8000); }
    if (!x) { *cycles = 11; return y >= 0 ? 0x4000 : static_cast<int16_t>(0xC000); }
    if (y >= 0) {
        if (x >= 0) {
            if (x >= y) return _ArcTan((y << 14) / x, r1, nullptr, cycles);
        } else if (-x >= y) {
            return static_cast<int16_t>(_ArcTan((y << 14) / x, r1, nullptr, cycles) + 0x8000);
        }
        return static_cast<int16_t>(0x4000 - _ArcTan((x << 14) / y, r1, nullptr, cycles));
    } else {
        if (x <= 0) {
            if (-x > -y) return static_cast<int16_t>(_ArcTan((y << 14) / x, r1, nullptr, cycles) + 0x8000);
        } else if (x >= -y) {
            return static_cast<int16_t>(_ArcTan((y << 14) / x, r1, nullptr, cycles) + 0x10000);
        }
        return static_cast<int16_t>(0xC000 - _ArcTan((x << 14) / y, r1, nullptr, cycles));
    }
}

int32_t _Sqrt(uint32_t x, uint32_t* cycles) {
    if (!x) { *cycles = 53; return 0; }
    int32_t c = 15;
    uint32_t upper = x, bound = 1, lower;
    while (bound < upper) { upper >>= 1; bound <<= 1; c += 6; }
    while (true) {
        c += 6;
        upper = x;
        uint32_t accum = 0;
        lower = bound;
        while (true) {
            c += 5;
            uint32_t oldLower = lower;
            if (lower <= upper >> 1) lower <<= 1;
            if (oldLower >= upper >> 1) break;
        }
        while (true) {
            c += 8;
            accum <<= 1;
            if (upper >= lower) { ++accum; upper -= lower; }
            if (lower == bound) break;
            lower >>= 1;
        }
        uint32_t oldBound = bound;
        bound += accum;
        bound >>= 1;
        if (bound >= oldBound) { bound = oldBound; break; }
    }
    *cycles = static_cast<uint32_t>(c);
    return static_cast<int32_t>(bound);
}

// ── memory-copy SWIs ────────────────────────────────────────────────────────

void do_cpu_set() {
    uint32_t src = R(0), dst = R(1), ctrl = R(2);
    uint32_t count = ctrl & 0x1FFFFFu;
    bool fixed = (ctrl >> 24) & 1u;
    bool w32   = (ctrl >> 26) & 1u;
    if (w32) {
        for (uint32_t i = 0; i < count; ++i) {
            wr32(dst, rd32(src));
            dst += 4; if (!fixed) src += 4;
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            wr16(dst, rd16(src));
            dst += 2; if (!fixed) src += 2;
        }
    }
}

void do_cpu_fast_set() {
    uint32_t src = R(0), dst = R(1), ctrl = R(2);
    uint32_t count = ctrl & 0x1FFFFFu;      // word count (games pass multiples of 8)
    bool fixed = (ctrl >> 24) & 1u;
    for (uint32_t i = 0; i < count; ++i) {
        wr32(dst, rd32(src));
        dst += 4; if (!fixed) src += 4;
    }
}

// ── affine SWIs (float, matches mGBA) ───────────────────────────────────────

void do_bg_affine_set() {
    uint32_t off = R(0), dest = R(1);
    int32_t i = static_cast<int32_t>(R(2));
    while (i-- > 0) {
        float ox = static_cast<int32_t>(rd32(off))      / 256.f;
        float oy = static_cast<int32_t>(rd32(off + 4))  / 256.f;
        float cx = static_cast<int16_t>(rd16(off + 8));
        float cy = static_cast<int16_t>(rd16(off + 10));
        float sx = static_cast<int16_t>(rd16(off + 12)) / 256.f;
        float sy = static_cast<int16_t>(rd16(off + 14)) / 256.f;
        float theta = (rd16(off + 16) >> 8) / 128.f * kPi;
        off += 20;
        float a, b, c, d;
        a = d = cosf(theta);
        b = c = sinf(theta);
        a *= sx; b *= -sx; c *= sy; d *= sy;
        float rx = ox - (a * cx + b * cy);
        float ry = oy - (c * cx + d * cy);
        wr16(dest,      static_cast<uint16_t>(static_cast<int32_t>(a * 256.f)));
        wr16(dest + 2,  static_cast<uint16_t>(static_cast<int32_t>(b * 256.f)));
        wr16(dest + 4,  static_cast<uint16_t>(static_cast<int32_t>(c * 256.f)));
        wr16(dest + 6,  static_cast<uint16_t>(static_cast<int32_t>(d * 256.f)));
        wr32(dest + 8,  static_cast<uint32_t>(static_cast<int32_t>(rx * 256.f)));
        wr32(dest + 12, static_cast<uint32_t>(static_cast<int32_t>(ry * 256.f)));
        dest += 16;
    }
}

void do_obj_affine_set() {
    uint32_t off = R(0), dest = R(1);
    int32_t i = static_cast<int32_t>(R(2));
    int32_t diff = static_cast<int32_t>(R(3));
    while (i-- > 0) {
        float sx = static_cast<int16_t>(rd16(off))     / 256.f;
        float sy = static_cast<int16_t>(rd16(off + 2)) / 256.f;
        float theta = (rd16(off + 4) >> 8) / 128.f * kPi;
        off += 8;
        float a, b, c, d;
        a = d = cosf(theta);
        b = c = sinf(theta);
        a *= sx; b *= -sx; c *= sy; d *= sy;
        wr16(dest,            static_cast<uint16_t>(static_cast<int32_t>(a * 256.f)));
        wr16(dest + diff,     static_cast<uint16_t>(static_cast<int32_t>(b * 256.f)));
        wr16(dest + diff * 2, static_cast<uint16_t>(static_cast<int32_t>(c * 256.f)));
        wr16(dest + diff * 3, static_cast<uint16_t>(static_cast<int32_t>(d * 256.f)));
        dest += diff * 4;
    }
}

// ── decompression SWIs ──────────────────────────────────────────────────────

uint32_t do_unLz77(int width) {
    uint32_t source = R(0), dest = R(1);
    uint32_t cycles = 20;
    int remaining = (rd32(source) & 0xFFFFFF00u) >> 8;
    int blockheader = 0;
    source += 4;
    int blocksRemaining = 0;
    uint32_t disp; int bytes, byte; int halfword = 0;
    while (remaining > 0) {
        cycles += 14;
        if (blocksRemaining) {
            cycles += 18;
            if (blockheader & 0x80) {
                int block = rd8(source + 1) | (rd8(source) << 8);
                source += 2;
                disp = dest - (block & 0x0FFF) - 1;
                bytes = (block >> 12) + 3;
                while (bytes--) {
                    cycles += 10;
                    if (remaining) --remaining;  // else improperly-compressed (overrun on HW)
                    if (width == 2) {
                        byte = static_cast<int16_t>(rd16(disp & ~1u));
                        byte >>= (disp & 1) * 8;
                        if (dest & 1) { halfword |= byte << 8; wr16(dest ^ 1, static_cast<uint16_t>(halfword)); }
                        else          { halfword = byte & 0xFF; }
                        cycles += 4;
                    } else {
                        byte = rd8(disp);
                        wr8(dest, static_cast<uint8_t>(byte));
                    }
                    ++disp; ++dest;
                }
            } else {
                byte = rd8(source); ++source;
                if (width == 2) {
                    if (dest & 1) { halfword |= byte << 8; wr16(dest ^ 1, static_cast<uint16_t>(halfword)); }
                    else          { halfword = byte; }
                } else {
                    wr8(dest, static_cast<uint8_t>(byte));
                }
                ++dest; --remaining;
            }
            blockheader <<= 1;
            --blocksRemaining;
        } else {
            blockheader = rd8(source); ++source;
            blocksRemaining = 8;
        }
    }
    setR(0, source); setR(1, dest); setR(3, 0);
    return cycles;
}

void do_unHuffman() {
    uint32_t source = R(0) & 0xFFFFFFFCu, dest = R(1);
    uint32_t header = rd32(source);
    int remaining = header >> 8;
    unsigned bits = header & 0xF;
    if (bits == 0) bits = 8;
    if ((32 % bits) || bits == 1) return;  // unimplemented unaligned Huffman
    int treesize = (rd8(source + 4) << 1) + 1;
    int block = 0;
    uint32_t treeBase = source + 5;
    source += 5 + treesize;
    uint32_t nPointer = treeBase;
    uint8_t node; int bitsSeen = 0;
    node = rd8(nPointer);
    while (remaining > 0) {
        uint32_t bitstream = rd32(source);
        source += 4;
        for (int bitsRemaining = 32; bitsRemaining > 0 && remaining > 0;
             --bitsRemaining, bitstream <<= 1) {
            uint32_t next = (nPointer & ~1u) + (node & 0x3F) * 2 + 2;  // Offset = bits[5:0]
            int readBits;
            if (bitstream & 0x80000000u) {
                if (node & 0x40) { readBits = rd8(next + 1); }        // RTerm = bit 6
                else { nPointer = next + 1; node = rd8(nPointer); continue; }
            } else {
                if (node & 0x80) { readBits = rd8(next); }            // LTerm = bit 7
                else { nPointer = next; node = rd8(nPointer); continue; }
            }
            block |= (readBits & ((1 << bits) - 1)) << bitsSeen;
            bitsSeen += bits;
            nPointer = treeBase; node = rd8(nPointer);
            if (bitsSeen == 32) {
                bitsSeen = 0; wr32(dest, block); dest += 4; remaining -= 4; block = 0;
            }
        }
    }
    setR(0, source); setR(1, dest);
}

void do_unRl(int width) {
    uint32_t source = R(0);
    int remaining = (rd32(source & 0xFFFFFFFCu) & 0xFFFFFF00u) >> 8;
    int padding = (4 - remaining) & 0x3;
    source += 4;
    uint32_t dest = R(1);
    int halfword = 0;
    while (remaining > 0) {
        int blockheader = rd8(source); ++source;
        if (blockheader & 0x80) {
            blockheader &= 0x7F; blockheader += 3;
            int block = rd8(source); ++source;
            while (blockheader-- && remaining) {
                --remaining;
                if (width == 2) {
                    if (dest & 1) { halfword |= block << 8; wr16(dest ^ 1, static_cast<uint16_t>(halfword)); }
                    else          { halfword = block; }
                } else { wr8(dest, static_cast<uint8_t>(block)); }
                ++dest;
            }
        } else {
            blockheader++;
            while (blockheader-- && remaining) {
                --remaining;
                int byte = rd8(source); ++source;
                if (width == 2) {
                    if (dest & 1) { halfword |= byte << 8; wr16(dest ^ 1, static_cast<uint16_t>(halfword)); }
                    else          { halfword = byte; }
                } else { wr8(dest, static_cast<uint8_t>(byte)); }
                ++dest;
            }
        }
    }
    if (width == 2) {
        if (dest & 1) { --padding; ++dest; }
        for (; padding > 0; padding -= 2, dest += 2) wr16(dest, 0);
    } else {
        while (padding--) { wr8(dest, 0); ++dest; }
    }
    setR(0, source); setR(1, dest);
}

void do_unFilter(int inwidth, int outwidth) {
    uint32_t source = R(0) & 0xFFFFFFFCu, dest = R(1);
    uint32_t header = rd32(source);
    int remaining = header >> 8;
    uint16_t halfword = 0, old = 0;
    source += 4;
    while (remaining > 0) {
        uint16_t nw = (inwidth == 1) ? rd8(source) : rd16(source);
        nw += old;
        if (outwidth > inwidth) {
            halfword >>= 8; halfword |= (nw << 8);
            if (source & 1) { wr16(dest, halfword); dest += outwidth; remaining -= outwidth; }
        } else if (outwidth == 1) {
            wr8(dest, static_cast<uint8_t>(nw)); dest += outwidth; remaining -= outwidth;
        } else {
            wr16(dest, nw); dest += outwidth; remaining -= outwidth;
        }
        old = nw;
        source += inwidth;
    }
    setR(0, source); setR(1, dest);
}

void do_unBitPack() {
    uint32_t source = R(0), dest = R(1), info = R(2);
    unsigned sourceLen   = rd16(info);
    unsigned sourceWidth = rd8(info + 2);
    unsigned destWidth   = rd8(info + 3);
    switch (sourceWidth) { case 1: case 2: case 4: case 8: break; default: return; }
    switch (destWidth)   { case 1: case 2: case 4: case 8: case 16: case 32: break; default: return; }
    uint32_t bias = rd32(info + 4);
    uint8_t in = 0; uint32_t out = 0;
    int bitsRemaining = 0, bitsEaten = 0;
    while (sourceLen > 0 || bitsRemaining) {
        if (!bitsRemaining) { in = rd8(source); bitsRemaining = 8; ++source; --sourceLen; }
        unsigned scaled = in & ((1u << sourceWidth) - 1);
        in >>= sourceWidth;
        if (scaled || (bias & 0x80000000u)) scaled += bias & 0x7FFFFFFFu;
        bitsRemaining -= sourceWidth;
        out |= scaled << bitsEaten;
        bitsEaten += destWidth;
        if (bitsEaten == 32) { wr32(dest, out); bitsEaten = 0; out = 0; dest += 4; }
    }
    setR(0, source); setR(1, dest);
}

void do_midi_key_2_freq() {
    uint32_t key = rd32(R(0) + 4);
    setR(0, static_cast<uint32_t>(
        key / exp2f((180.f - R(1) - R(2) / 256.f) / 12.f)));
}

}  // namespace

// ── public surface ──────────────────────────────────────────────────────────

void bios_hle_bind(armv4t::CPUState* cpu, armv4t::Bus* bus) {
    g_cpu = cpu;
    g_bus = bus;
    g_intr_wait_armed = false;
}

void bios_hle_boot_skip(uint32_t cart_entry) {
    // Synthesize the state the real BIOS leaves when it hands off to the cart
    // (GBATEK "GBA Reset"; the same state NanoBoyAdvance / mGBA HLE reproduce),
    // then jump straight to the cart entry.
    for (int i = 0; i < 13; ++i) g_cpu->R[i] = 0;
    g_cpu->banked_sp[armv4t::Bank_Supervisor] = 0x03007FE0u;
    g_cpu->banked_sp[armv4t::Bank_IRQ]        = 0x03007FA0u;
    g_cpu->banked_sp[armv4t::Bank_User]       = 0x03007F00u;
    g_cpu->banked_lr[armv4t::Bank_Supervisor] = 0;
    g_cpu->banked_lr[armv4t::Bank_IRQ]        = 0;
    g_cpu->R[13] = 0x03007F00u;   // active SP = System/User bank
    g_cpu->R[14] = 0;
    unpack_cpsr(*g_cpu, 0x0000001Fu);  // System mode, ARM state, IRQ/FIQ enabled
    g_cpu->R[15] = cart_entry;
    // User IRQ handler pointer the BIOS IRQ dispatcher reads; the game installs
    // its own during init. Zero = none yet. (IME defaults to 0, so no IRQ can
    // vector before the game programs IE/IME anyway.)
    wr32(0x03007FFCu, 0);
    g_intr_wait_armed = false;
    // SWI_SOFT_RESET can reach here from inside an IRQ handler that will never
    // return, so the nesting depth has to be dropped with the rest of the state
    // or the epilogue would fire on a stale frame.
    g_irq_nest_depth = 0;
}

uint32_t bios_hle_swi(uint32_t swi) {
    uint32_t cost = 45;  // nominal SWI overhead for the non-stall cases
    switch (swi) {
    case SWI_SOFT_RESET:
        bios_hle_boot_skip(rd8(0x03007FFAu) ? 0x02000000u : 0x08000000u);
        break;
    case SWI_REGISTER_RAM_RESET:
        do_register_ram_reset();
        break;
    case SWI_HALT:
        do_halt(false);
        break;
    case SWI_STOP:
        do_halt(true);
        break;
    case SWI_INTR_WAIT:
        do_intr_wait(false);
        break;
    case SWI_VBLANK_INTR_WAIT:
        do_intr_wait(true);
        break;
    case SWI_DIV:              cost = do_div(static_cast<int32_t>(R(0)), static_cast<int32_t>(R(1))); break;
    case SWI_DIV_ARM:          cost = do_div(static_cast<int32_t>(R(1)), static_cast<int32_t>(R(0))); break;
    case SWI_SQRT: {
        uint32_t c; setR(0, static_cast<uint32_t>(_Sqrt(R(0), &c))); cost = c; break;
    }
    case SWI_ARCTAN: {
        uint32_t c; int32_t r1, r3;
        int16_t v = _ArcTan(static_cast<int32_t>(R(0)), &r1, &r3, &c);
        setR(0, static_cast<uint32_t>(static_cast<int32_t>(v)));
        setR(1, static_cast<uint32_t>(r1)); setR(3, static_cast<uint32_t>(r3)); cost = c; break;
    }
    case SWI_ARCTAN2: {
        uint32_t c; int32_t r1 = static_cast<int32_t>(R(1));
        int16_t v = _ArcTan2(static_cast<int32_t>(R(0)), static_cast<int32_t>(R(1)), &r1, &c);
        setR(0, static_cast<uint16_t>(v)); setR(1, static_cast<uint32_t>(r1));
        setR(3, 0x170u); cost = c; break;
    }
    case SWI_GET_BIOS_CHECKSUM:
        setR(0, GBA_BIOS_CHECKSUM); setR(1, 1u); setR(3, SIZE_BIOS); break;
    case SWI_CPU_SET:          do_cpu_set(); cost = 10 + (R(2) & 0x1FFFFF) * 2; break;
    case SWI_CPU_FAST_SET:     do_cpu_fast_set(); cost = 10 + (R(2) & 0x1FFFFF) * 2; break;
    case SWI_BG_AFFINE_SET:    do_bg_affine_set(); cost = 10 + R(2) * 60; break;
    case SWI_OBJ_AFFINE_SET:   do_obj_affine_set(); cost = 10 + R(2) * 40; break;
    case SWI_BIT_UNPACK:       do_unBitPack(); break;
    case SWI_LZ77_UNCOMP_WRAM: cost = do_unLz77(1); break;
    case SWI_LZ77_UNCOMP_VRAM: cost = do_unLz77(2); break;
    case SWI_HUFFMAN_UNCOMP:   do_unHuffman(); break;
    case SWI_RL_UNCOMP_WRAM:   do_unRl(1); break;
    case SWI_RL_UNCOMP_VRAM:   do_unRl(2); break;
    case SWI_DIFF_8BIT_UNFILTER_WRAM: do_unFilter(1, 1); break;
    case SWI_DIFF_8BIT_UNFILTER_VRAM: do_unFilter(1, 2); break;
    case SWI_DIFF_16BIT_UNFILTER:     do_unFilter(2, 2); break;
    case SWI_SOUND_BIAS:       wr16(REG_SOUNDBIAS, static_cast<uint16_t>(R(0) ? 0x200 : 0)); break;
    case SWI_MIDI_KEY_2_FREQ:  do_midi_key_2_freq(); break;
    default:
        // Standalone: an unimplemented call must never vector into the zeroed
        // BIOS region. Charge a token cost and resume the caller.
        cost = 3u;
        break;
    }
    return cost;
}

bool bios_hle_irq_enter(uint32_t return_address) {
    const uint32_t handler = rd32(0x03007FFCu) & ~3u;
    if (handler == 0u) return false;

    const uint32_t slot = g_irq_nest_depth % kHleIrqMaxDepth;
    g_irq_src_stack[slot] = rd16(0x04000200u) & rd16(0x04000202u);
    g_irq_ie_stack[slot]  = rd16(0x04000200u);
    ++g_irq_nest_depth;

    const uint32_t saved_cpsr = pack_cpsr(*g_cpu);
    const armv4t::BankedSlot old_bank = mode_to_bank(g_cpu->cpsr.mode);

    // Bank out the interrupted mode and switch to IRQ, ARM state, IRQ masked.
    g_cpu->banked_sp[old_bank] = g_cpu->R[13];
    g_cpu->banked_lr[old_bank] = g_cpu->R[14];
    g_cpu->banked_spsr[armv4t::Bank_IRQ] = saved_cpsr;
    g_cpu->cpsr.mode = static_cast<uint8_t>(armv4t::Mode::IRQ);
    g_cpu->cpsr.t = false;
    g_cpu->cpsr.i = true;
    g_cpu->thumb  = false;
    g_cpu->R[13]  = g_cpu->banked_sp[armv4t::Bank_IRQ];

    // The BIOS dispatcher's own prologue: push {r0-r3,r12,lr}, r0 = 0x04000000
    // (the IO base the standard intr_main expects), lr = the return sentinel.
    const uint32_t return_lr = return_address + 4u;
    const uint32_t sp = g_cpu->R[13] - 24u;
    g_cpu->R[13] = sp;
    const uint32_t saved_regs[6] = {
        g_cpu->R[0], g_cpu->R[1], g_cpu->R[2], g_cpu->R[3], g_cpu->R[12], return_lr
    };
    for (unsigned i = 0; i < 6; ++i) wr32(sp + i * 4u, saved_regs[i]);
    g_cpu->R[0]  = 0x04000000u;
    g_cpu->R[14] = kHleIrqReturn;
    g_cpu->R[15] = handler;
    return true;
}

bool bios_hle_irq_epilogue() {
    if ((g_cpu->R[15] & ~1u) != kHleIrqReturn || g_irq_nest_depth == 0u ||
        g_cpu->cpsr.mode != static_cast<uint8_t>(armv4t::Mode::IRQ)) {
        return false;
    }
    const uint32_t slot = (g_irq_nest_depth - 1u) % kHleIrqMaxDepth;
    const uint32_t irq_src = g_irq_src_stack[slot];
    const uint16_t saved_ie = g_irq_ie_stack[slot];

    const uint32_t sp = g_cpu->R[13];
    uint32_t restored[6]{};
    for (unsigned i = 0; i < 6; ++i) restored[i] = rd32(sp + i * 4u);
    g_cpu->R[13] = sp + 24u;
    g_cpu->R[0] = restored[0]; g_cpu->R[1] = restored[1];
    g_cpu->R[2] = restored[2]; g_cpu->R[3] = restored[3];
    g_cpu->R[12] = restored[4]; g_cpu->R[14] = restored[5];

    // Complete the dispatcher contract the standalone reference defines: the
    // serviced source is ORed into the BIOS IntrWait flag word, IF is acked,
    // and IE is put back to its value at vector time.
    //
    // The flag-word OR is the load-bearing one. SWI_INTR_WAIT spins on
    // 0x03007FF8 and only the interrupt path can ever set it; a game whose
    // handler does not write it itself would otherwise wait forever. The OR is
    // idempotent, so doing it for handlers that DO write it costs nothing.
    //
    // Restoring IE is the questionable half, and it is kept only because it is
    // what the tested upstream standalone path does: a handler that
    // deliberately enables a new interrupt source in IE (say, arming HBlank
    // from inside the VBlank handler) has that write silently undone here. If a
    // game ever hangs waiting on a source it enabled from inside an IRQ, this
    // line is the first suspect.
    const uint16_t flags = rd16(0x03007FF8u);
    wr16(0x03007FF8u, static_cast<uint16_t>(flags | irq_src));
    wr16(0x04000202u, static_cast<uint16_t>(irq_src));
    wr16(0x04000200u, saved_ie);

    // Bank back into the interrupted mode and restore CPSR from SPSR_irq.
    const uint32_t saved_cpsr = g_cpu->banked_spsr[armv4t::Bank_IRQ];
    const armv4t::BankedSlot old_bank = mode_to_bank(static_cast<uint8_t>(saved_cpsr & 0x1Fu));
    if (old_bank != armv4t::Bank_IRQ) {
        g_cpu->banked_sp[armv4t::Bank_IRQ] = g_cpu->R[13];
        g_cpu->banked_lr[armv4t::Bank_IRQ] = g_cpu->R[14];
        g_cpu->R[13] = g_cpu->banked_sp[old_bank];
        g_cpu->R[14] = g_cpu->banked_lr[old_bank];
    }
    unpack_cpsr(*g_cpu, saved_cpsr);
    g_cpu->R[15] = restored[5] - 4u;
    --g_irq_nest_depth;
    return true;
}

}  // namespace gbamvii
