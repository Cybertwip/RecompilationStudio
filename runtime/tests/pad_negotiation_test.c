#include "pad_negotiation.h"

#include <string.h>

static int failures;

#define CHECK(condition, message)                                                \
    do {                                                                         \
        if (!(condition)) {                                                      \
            (void)(message);                                                      \
            failures++;                                                         \
        }                                                                        \
    } while (0)

int main(void) {
    PsxPadNegotiation policy;

    psx_pad_negotiation_reset(&policy, 0);
    CHECK(!psx_pad_negotiation_is_enabled(&policy), "manual mode is disabled");
    CHECK(!psx_pad_negotiation_observe_command(&policy, 0x43),
          "manual mode ignores configuration probes");

    psx_pad_negotiation_reset(&policy, 1);
    CHECK(strcmp(psx_pad_negotiation_state_name(&policy), "waiting_for_game") == 0,
          "automatic mode waits for the game handoff");
    psx_pad_negotiation_observe_command(&policy, 0x42);
    psx_pad_negotiation_observe_command(&policy, 0x43);
    CHECK(strcmp(psx_pad_negotiation_state_name(&policy), "waiting_for_game") == 0,
          "BIOS pad traffic cannot decide a game's controller");

    psx_pad_negotiation_begin_game(&policy);
    CHECK(psx_pad_negotiation_uses_dpad(&policy), "game starts with a D-Pad");
    for (int frame = 0; frame < 120; ++frame) {
        CHECK(!psx_pad_negotiation_observe_command(&policy, 0x43),
              "first DualShock probe remains unanswered");
        CHECK(!psx_pad_negotiation_observe_command(&policy, 0x42),
              "normal D-Pad poll is accepted");
    }
    CHECK(psx_pad_negotiation_uses_dpad(&policy),
          "0x43 then 0x42 remains D-Pad across frames");

    psx_pad_negotiation_begin_game(&policy);
    CHECK(!psx_pad_negotiation_observe_command(&policy, 0x43),
          "first consecutive configuration probe tests the D-Pad");
    CHECK(psx_pad_negotiation_observe_command(&policy, 0x43),
          "second consecutive configuration probe selects Hybrid");
    CHECK(psx_pad_negotiation_uses_hybrid(&policy),
          "Hybrid selection is observable");
    CHECK(!psx_pad_negotiation_observe_command(&policy, 0x42),
          "Hybrid selection is sticky for the running game");
    CHECK(psx_pad_negotiation_uses_hybrid(&policy),
          "later polls do not demote Hybrid");

    psx_pad_negotiation_begin_game(&policy);
    CHECK(psx_pad_negotiation_uses_dpad(&policy),
          "a new game negotiation starts from D-Pad again");

    return failures == 0 ? 0 : 1;
}
