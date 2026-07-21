#include "psx_scheduler.h"
#include <stdint.h>
#include <stdio.h>

static int expect(uint32_t stored, uint32_t expected)
{
    uint32_t actual = psx_scheduler_restore_status(stored);
    if (actual == expected) return 0;
    printf("TCB Status restore mismatch: stored=0x%08X expected=0x%08X actual=0x%08X\n",
           stored, expected, actual);
    return 1;
}

int main(void)
{
    int failures = 0;
    failures += expect(0x40000401u, 0x40000401u); /* fresh PsyQ live-form TCB */
    failures += expect(0x40000404u, 0x40000401u); /* exception-stacked IEp -> IEc */
    failures += expect(0x40000400u, 0x40000400u); /* stacked context with IE disabled */
    failures += expect(0x0000000Cu, 0x00000003u); /* stacked KU/IE pair */
    failures += expect(0x00000003u, 0x00000003u); /* fresh live KU/IE pair */
    return failures ? 1 : 0;
}
