#include "scanline_filter.h"

#include <array>
#include <cstdint>
#include <cstdio>

int main() {
    std::array<uint8_t, 4 * 4 * 3> frame{};
    frame.fill(200);
    gbarecomp::runtime::apply_scanline_filter(frame.data(), 4, 4, 128, 255);

    for (int y = 0; y < 4; ++y) {
        const uint8_t expected = (y & 1) ? 100 : 200;
        for (int x = 0; x < 4 * 3; ++x) {
            if (frame[static_cast<std::size_t>(y) * 12u + x] != expected) {
                std::printf("scanline_filter_test: row %d mismatch\n", y);
                return 1;
            }
        }
    }

    auto unchanged = frame;
    gbarecomp::runtime::apply_scanline_filter(unchanged.data(), 4, 4, 255, 255);
    if (unchanged != frame) return 2;

    std::printf("scanline_filter_test: ok\n");
    return 0;
}
