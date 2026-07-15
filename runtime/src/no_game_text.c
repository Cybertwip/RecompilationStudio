#include <stdint.h>

/* A BIOS-only runtime has no configured game text image. This is the complete
 * answer to the shared dispatch/fntrace range query for that target. Game
 * targets provide their generated psx_game_address_in_text() instead. */
int psx_game_address_in_text(uint32_t addr)
{
    (void)addr;
    return 0;
}
