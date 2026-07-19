#include "pad_negotiation.h"

void psx_pad_negotiation_reset(PsxPadNegotiation* policy, int enabled) {
    if (!policy) return;
    policy->enabled = enabled ? 1u : 0u;
    policy->state = enabled ? PSX_PAD_NEGOTIATION_WAITING_FOR_GAME
                            : PSX_PAD_NEGOTIATION_OFF;
    policy->config_probe_streak = 0;
}

void psx_pad_negotiation_begin_game(PsxPadNegotiation* policy) {
    if (!policy || !policy->enabled) return;
    policy->state = PSX_PAD_NEGOTIATION_DPAD;
    policy->config_probe_streak = 0;
}

int psx_pad_negotiation_observe_command(PsxPadNegotiation* policy,
                                        uint8_t command) {
    if (!policy || !policy->enabled) return 0;
    if (policy->state != PSX_PAD_NEGOTIATION_DPAD &&
        policy->state != PSX_PAD_NEGOTIATION_DPAD_PROBE) {
        return 0;
    }

    if (command == 0x42u) {
        policy->state = PSX_PAD_NEGOTIATION_DPAD;
        policy->config_probe_streak = 0;
        return 0;
    }
    if (command != 0x43u) return 0;

    if (policy->config_probe_streak == 0) {
        policy->config_probe_streak = 1;
        policy->state = PSX_PAD_NEGOTIATION_DPAD_PROBE;
        return 0;
    }

    policy->config_probe_streak = 2;
    policy->state = PSX_PAD_NEGOTIATION_HYBRID;
    return 1;
}

int psx_pad_negotiation_is_enabled(const PsxPadNegotiation* policy) {
    return policy && policy->enabled;
}

int psx_pad_negotiation_uses_dpad(const PsxPadNegotiation* policy) {
    return policy && policy->enabled &&
           policy->state != PSX_PAD_NEGOTIATION_HYBRID;
}

int psx_pad_negotiation_uses_hybrid(const PsxPadNegotiation* policy) {
    return policy && policy->enabled &&
           policy->state == PSX_PAD_NEGOTIATION_HYBRID;
}

const char* psx_pad_negotiation_state_name(const PsxPadNegotiation* policy) {
    if (!policy) return "off";
    switch ((PsxPadNegotiationState)policy->state) {
    case PSX_PAD_NEGOTIATION_WAITING_FOR_GAME: return "waiting_for_game";
    case PSX_PAD_NEGOTIATION_DPAD:             return "dpad";
    case PSX_PAD_NEGOTIATION_DPAD_PROBE:       return "dpad_probe";
    case PSX_PAD_NEGOTIATION_HYBRID:           return "hybrid";
    case PSX_PAD_NEGOTIATION_OFF:
    default:                                   return "off";
    }
}
