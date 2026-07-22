#include "cpu_state.h"
#include <stdint.h>
#include <stdio.h>

static int expect(uint32_t address, uint32_t expected)
{
    uint32_t actual = psx_main_ram_mirror_phys(address);
    if (actual == expected) return 0;
    printf("main-RAM mirror mismatch: addr=0x%08X expected=0x%08X actual=0x%08X\n",
           address, expected, actual);
    return 1;
}

int main(void)
{
    int failures = 0;
    failures += expect(0x80000000u, 0x00000000u);
    failures += expect(0x801FFFFCu, 0x001FFFFCu);
    failures += expect(0x80200000u, 0x00000000u);
    failures += expect(0x80336CD8u, 0x00136CD8u);
    failures += expect(0x807FFFFCu, 0x001FFFFCu);
    failures += expect(0xA03356C8u, 0x001356C8u);
    failures += expect(0x80800000u, 0x00800000u);
    failures += expect(0x1F801000u, 0x1F801000u);
    return failures ? 1 : 0;
}
