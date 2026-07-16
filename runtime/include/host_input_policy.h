#ifndef PSXRECOMP_HOST_INPUT_POLICY_H
#define PSXRECOMP_HOST_INPUT_POLICY_H

#include <SDL.h>

#include <array>
#include <cstddef>

namespace PSXRecompV4 {

enum HostInputRoute {
    HOST_INPUT_NONE = 0,
    HOST_INPUT_KEYBOARD = 1,
    HOST_INPUT_SDL_CONTROLLER = 2,
    HOST_INPUT_DIRECT_USB = 3,
    HOST_INPUT_AUTOMATIC = 4,
};

inline bool host_route_uses_keyboard(int route, bool physical_input_attached) {
    return route == HOST_INPUT_KEYBOARD ||
           (route == HOST_INPUT_AUTOMATIC && !physical_input_attached);
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
