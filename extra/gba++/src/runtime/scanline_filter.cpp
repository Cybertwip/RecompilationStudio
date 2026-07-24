#include "scanline_filter.h"

#include <cstddef>

namespace gbarecomp::runtime {

void apply_scanline_filter(uint8_t* rgb888, int width, int height,
                           uint8_t dark_numerator,
                           uint8_t dark_denominator) {
    if (!rgb888 || width <= 0 || height <= 1 || dark_denominator == 0 ||
        dark_numerator >= dark_denominator) {
        return;
    }

    const unsigned numerator = dark_numerator;
    const unsigned denominator = dark_denominator;
    const std::size_t pitch = static_cast<std::size_t>(width) * 3u;
    for (int y = 1; y < height; y += 2) {
        uint8_t* row = rgb888 + static_cast<std::size_t>(y) * pitch;
        for (std::size_t x = 0; x < pitch; ++x) {
            row[x] = static_cast<uint8_t>(
                (static_cast<unsigned>(row[x]) * numerator + denominator / 2u) /
                denominator);
        }
    }
}

} // namespace gbarecomp::runtime
