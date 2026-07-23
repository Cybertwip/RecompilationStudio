#include "starvation_ring.h"

#if STARVATION_RING_ENABLED != 0
#error "PSX_NO_DEBUG_TOOLS production builds must compile out the starvation watchdog"
#endif

int main(void)
{
    starvation_watchdog_heartbeat();
    starvation_watchdog_check();
    return 0;
}
