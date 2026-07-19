#ifndef PSXRECOMP_PAD_NEGOTIATION_H
#define PSXRECOMP_PAD_NEGOTIATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Automatic controller selection is deliberately driven by completed SIO pad
 * commands, never by a title database or host-time timeout. Before game entry
 * the BIOS may poll the controller, so those commands are ignored. At game
 * entry the port starts as a real D-Pad. A 0x42 poll resets the rejection
 * sequence; two 0x43 configuration probes without an intervening 0x42 select
 * Hybrid. The first 0x43 remains unanswered, giving D-Pad-aware drivers the
 * opportunity to fall back to their normal 0x42 path. */
typedef enum PsxPadNegotiationState {
    PSX_PAD_NEGOTIATION_OFF = 0,
    PSX_PAD_NEGOTIATION_WAITING_FOR_GAME = 1,
    PSX_PAD_NEGOTIATION_DPAD = 2,
    PSX_PAD_NEGOTIATION_DPAD_PROBE = 3,
    PSX_PAD_NEGOTIATION_HYBRID = 4
} PsxPadNegotiationState;

typedef struct PsxPadNegotiation {
    uint8_t enabled;
    uint8_t state;
    uint8_t config_probe_streak;
} PsxPadNegotiation;

void psx_pad_negotiation_reset(PsxPadNegotiation* policy, int enabled);
void psx_pad_negotiation_begin_game(PsxPadNegotiation* policy);

/* Observe a pad command byte at the PAD_WAIT_ACCESS boundary. Returns 1 only
 * on the command that promotes the port from D-Pad to Hybrid. */
int psx_pad_negotiation_observe_command(PsxPadNegotiation* policy,
                                        uint8_t command);

int psx_pad_negotiation_is_enabled(const PsxPadNegotiation* policy);
int psx_pad_negotiation_uses_dpad(const PsxPadNegotiation* policy);
int psx_pad_negotiation_uses_hybrid(const PsxPadNegotiation* policy);
const char* psx_pad_negotiation_state_name(const PsxPadNegotiation* policy);

#ifdef __cplusplus
}
#endif

#endif
