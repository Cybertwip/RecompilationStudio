#include "host_input_policy.h"
#include "controller_identity.h"
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
    check(host_route_uses_keyboard(HOST_INPUT_SDL_CONTROLLER, false));
    check(!host_route_uses_keyboard(HOST_INPUT_SDL_CONTROLLER, false, 1));

    std::array<std::string, 2> routes = {"auto", "auto"};
    std::array<bool, 2> connected = {false, false};
    check(host_controller_assignment_slot(routes, connected, -1, "pad-a") == 0);
    routes[0] = "pad-a"; connected[0] = true;
    check(host_controller_assignment_slot(routes, connected, -1, "pad-b") == 1);
    routes[1] = "pad-b"; connected[1] = true;
    check(host_controller_assignment_slot(routes, connected, -1, "pad-c") == -1);
    connected[1] = false;
    check(host_controller_assignment_slot(routes, connected, 1, "pad-b") == 1);
    check(host_controller_assignment_slot(routes, connected, 1, "pad-c") == 1);
    connected = {false, false};
    check(host_controller_assignment_slot(routes, connected, 0, "pad-c") == 0);
    routes = {"keyboard", "none"};
    check(host_controller_assignment_slot(routes, connected, -1, "pad-c") == -1);

    const std::string guid = "030000005e0400008e02000000000000";
    const std::string serial_a = make_sdl_controller_id(guid, "SERIAL-A", "/dev/a");
    const std::string serial_a_again = make_sdl_controller_id(guid, "SERIAL-A", "/dev/b");
    const std::string serial_b = make_sdl_controller_id(guid, "SERIAL-B", "/dev/a");
    const std::string path_a = make_sdl_controller_id(guid, "", "/dev/a");
    const std::string path_b = make_sdl_controller_id(guid, "", "/dev/b");
    check(serial_a == serial_a_again);
    check(serial_a != serial_b);
    check(path_a != path_b);
    check(serial_a.rfind("sdl:" + guid + ":serial-", 0) == 0);
    SdlControllerIdentity live;
    live.persistent_id = serial_a;
    live.legacy_guid = guid;
    check(sdl_controller_id_matches(serial_a, live));
    check(sdl_controller_id_matches(guid, live));
    check(sdl_controller_id_matches("sdl:" + guid, live));
    check(!sdl_controller_id_matches(serial_b, live));

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
        "\"two_slot_controller_assignment\":true,"
        "\"persistent_controller_identity\":true,"
        "\"event_backed_keyboard\":true,"
        "\"psx_button_mapping\":true}\n",
        checks, failures);
    return failures == 0 ? 0 : 1;
}
