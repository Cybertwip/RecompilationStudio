#include "host_input_policy.h"
#include "psx_keybinds.h"

#include <SDL.h>

#include <array>
#include <cstdio>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition) {
    ++checks;
    if (!condition) ++failures;
}

} // namespace

int main() {
    using namespace PSXRecompV4;

    check(host_route_uses_keyboard(HOST_INPUT_KEYBOARD, false));
    check(host_route_uses_keyboard(HOST_INPUT_AUTOMATIC, false));
    check(!host_route_uses_keyboard(HOST_INPUT_AUTOMATIC, true));
    check(!host_route_uses_keyboard(HOST_INPUT_SDL_CONTROLLER, false));

    HostKeyboardState state;
    SDL_KeyboardEvent event{};
    event.keysym.scancode = SDL_SCANCODE_X;
    event.keysym.sym = SDLK_x;
    state.update(event, true);
    check(state.data()[SDL_SCANCODE_X] != 0);
    check((psx_keybinds_pad_word(state.data(), 1) & (1u << 14)) == 0);
    check(psx_keybinds_button_mask_for_scancode(SDL_SCANCODE_X, 1) ==
          (1u << 14));

    state.update(event, false);
    check(state.data()[SDL_SCANCODE_X] == 0);
    check(psx_keybinds_pad_word(state.data(), 1) == 0xFFFFu);

    event.keysym.scancode = SDL_SCANCODE_LEFT;
    event.keysym.sym = SDLK_LEFT;
    state.update(event, true);
    check((psx_keybinds_pad_word(state.data(), 1) & (1u << 7)) == 0);
    state.reset();
    check(psx_keybinds_pad_word(state.data(), 1) == 0xFFFFu);

    std::printf(
        "{\"schema\":1,\"artifact\":\"host_input_policy\","
        "\"checks\":%d,\"failures\":%d,"
        "\"automatic_keyboard_fallback\":true,"
        "\"event_backed_keyboard\":true,"
        "\"psx_button_mapping\":true}\n",
        checks, failures);
    return failures == 0 ? 0 : 1;
}
