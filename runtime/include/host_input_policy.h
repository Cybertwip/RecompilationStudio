#ifndef PSXRECOMP_HOST_INPUT_POLICY_H
#define PSXRECOMP_HOST_INPUT_POLICY_H

#include <SDL.h>

#include <array>
#include <cstddef>
#include <string>

namespace PSXRecompV4 {

enum HostInputRoute {
    HOST_INPUT_NONE = 0,
    HOST_INPUT_KEYBOARD = 1,
    HOST_INPUT_SDL_CONTROLLER = 2,
    HOST_INPUT_DIRECT_USB = 3,
    HOST_INPUT_AUTOMATIC = 4,
};

inline bool host_route_uses_keyboard(int route, bool physical_input_attached,
                                     int player_index = 0) {
    if (route == HOST_INPUT_KEYBOARD) return true;
    if (player_index != 0 || physical_input_attached) return false;
    return route == HOST_INPUT_SDL_CONTROLLER ||
           route == HOST_INPUT_DIRECT_USB ||
           route == HOST_INPUT_AUTOMATIC;
}

inline bool host_route_accepts_controller(const std::string& route) {
    return route == "auto" ||
           (!route.empty() && route != "none" && route != "keyboard");
}

/* Choose the exclusive P1/P2 slot for a newly observed controller.
 *
 * - A remembered identity always reclaims its old free slot.
 * - Otherwise the most recently unplugged eligible slot is replaced.
 * - Fresh automatic slots fill P1 then P2.
 * - With two live pads, additional controllers remain unassigned.
 *
 * This is deliberately pure so the hot-plug policy has a proof test independent
 * of physical hardware and SDL event timing. */
inline int host_controller_assignment_slot(
    const std::array<std::string, 2>& routes,
    const std::array<bool, 2>& connected,
    int last_unplugged_slot,
    const std::string& incoming_id) {
    for (int slot = 0; slot < 2; ++slot) {
        if (!connected[slot] && routes[slot] == incoming_id) return slot;
    }
    if (connected[0] && connected[1]) return -1;
    if (last_unplugged_slot >= 0 && last_unplugged_slot < 2 &&
        !connected[last_unplugged_slot] &&
        host_route_accepts_controller(routes[last_unplugged_slot])) {
        return last_unplugged_slot;
    }
    for (int slot = 0; slot < 2; ++slot) {
        if (!connected[slot] && routes[slot] == "auto") return slot;
    }
    for (int slot = 0; slot < 2; ++slot) {
        if (!connected[slot] && host_route_accepts_controller(routes[slot]))
            return slot;
    }
    return -1;
}

class HostKeyboardState {
public:
    static SDL_Scancode event_scancode(const SDL_KeyboardEvent& event) {
        SDL_Scancode scancode = event.keysym.scancode;
        if (scancode == SDL_SCANCODE_UNKNOWN && event.keysym.sym != SDLK_UNKNOWN)
            scancode = SDL_GetScancodeFromKey(event.keysym.sym);
        return scancode;
    }

    void update(const SDL_KeyboardEvent& event, bool pressed) {
        const SDL_Scancode scancode = event_scancode(event);
        if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_NUM_SCANCODES)
            state_[(std::size_t)scancode] = pressed ? 1 : 0;
    }

    void reset() { state_.fill(0); }
    const Uint8* data() const { return state_.data(); }

private:
    std::array<Uint8, SDL_NUM_SCANCODES> state_{};
};

} // namespace PSXRecompV4

#endif
