#pragma once

#include <cstdint>

namespace gbarecomp::runtime::bitmap_font {

constexpr int kWidth = 8;
constexpr int kHeight = 8;

// Printable ASCII 0x20..0x7e, MSB-left, with '?' fallback.
const uint8_t* glyph(char c);

} // namespace gbarecomp::runtime::bitmap_font
