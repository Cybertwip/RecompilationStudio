#pragma once

#include <cstdint>

namespace gbarecomp::runtime {

// Present-time only. The emulated framebuffer and all verification hashes stay
// untouched; this darkens alternating output rows in a caller-owned copy.
void apply_scanline_filter(uint8_t* rgb888, int width, int height,
                           uint8_t dark_numerator = 208,
                           uint8_t dark_denominator = 255);

} // namespace gbarecomp::runtime
