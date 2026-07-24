#include "host_gamepad.h"

#include <algorithm>
#include <utility>

#if defined(GBARECOMP_HAVE_SDL2)
#include <SDL.h>
#include "recomp_gamepad/controller_identity.h"
#if defined(PSX_HAVE_GIP_GAMEPAD)
#include "recomp_gamepad/gip_gamepad.h"
#endif
#endif

namespace gbarecomp {

uint16_t gba_keyinput_from_gamepad(const HostGamepadState& state,
                                   int deadzone) {
    deadzone = std::clamp(deadzone, 0, 32767);
    uint16_t keys = 0x03FFu;
    auto press = [&](int bit) {
        keys &= static_cast<uint16_t>(~(1u << bit));
    };
    const uint32_t b = state.buttons;
    if (b & (HOST_PAD_FACE_EAST | HOST_PAD_FACE_NORTH)) press(0); // GBA A
    if (b & (HOST_PAD_FACE_SOUTH | HOST_PAD_FACE_WEST)) press(1); // GBA B
    if (b & HOST_PAD_BACK) press(2);
    if (b & HOST_PAD_START) press(3);
    if ((b & HOST_PAD_DPAD_RIGHT) || state.left_x > deadzone) press(4);
    if ((b & HOST_PAD_DPAD_LEFT)  || state.left_x < -deadzone) press(5);
    if ((b & HOST_PAD_DPAD_UP)    || state.left_y < -deadzone) press(6);
    if ((b & HOST_PAD_DPAD_DOWN)  || state.left_y > deadzone) press(7);
    if ((b & HOST_PAD_RIGHT_SHOULDER) || state.right_trigger > 8191u) press(8);
    if ((b & HOST_PAD_LEFT_SHOULDER)  || state.left_trigger > 8191u) press(9);
    return keys;
}

#if defined(GBARECOMP_HAVE_SDL2)

namespace {

uint16_t normalized_trigger(Sint16 value) {
    return value <= 0 ? 0u : static_cast<uint16_t>(value);
}

std::vector<recomp_gamepad::SdlControllerIdentity> live_sdl_controllers() {
    std::vector<recomp_gamepad::SdlControllerIdentity> result;
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        auto identity = recomp_gamepad::describe_sdl_controller(i);
        if (!identity.persistent_id.empty()) result.push_back(std::move(identity));
    }
    return result;
}

HostGamepadState sample_sdl(SDL_GameController* controller) {
    HostGamepadState state;
    if (!controller || !SDL_GameControllerGetAttached(controller)) return state;
    auto button = [&](SDL_GameControllerButton id) {
        return SDL_GameControllerGetButton(controller, id) != 0;
    };
    if (button(SDL_CONTROLLER_BUTTON_A)) state.buttons |= HOST_PAD_FACE_SOUTH;
    if (button(SDL_CONTROLLER_BUTTON_B)) state.buttons |= HOST_PAD_FACE_EAST;
    if (button(SDL_CONTROLLER_BUTTON_X)) state.buttons |= HOST_PAD_FACE_WEST;
    if (button(SDL_CONTROLLER_BUTTON_Y)) state.buttons |= HOST_PAD_FACE_NORTH;
    if (button(SDL_CONTROLLER_BUTTON_BACK)) state.buttons |= HOST_PAD_BACK;
    if (button(SDL_CONTROLLER_BUTTON_START)) state.buttons |= HOST_PAD_START;
    if (button(SDL_CONTROLLER_BUTTON_GUIDE)) state.buttons |= HOST_PAD_GUIDE;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_UP)) state.buttons |= HOST_PAD_DPAD_UP;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) state.buttons |= HOST_PAD_DPAD_DOWN;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) state.buttons |= HOST_PAD_DPAD_LEFT;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) state.buttons |= HOST_PAD_DPAD_RIGHT;
    if (button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
        state.buttons |= HOST_PAD_LEFT_SHOULDER;
    if (button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
        state.buttons |= HOST_PAD_RIGHT_SHOULDER;
    state.left_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    state.left_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    state.right_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
    state.right_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
    state.left_trigger = normalized_trigger(
        SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    state.right_trigger = normalized_trigger(
        SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    return state;
}

#if defined(PSX_HAVE_GIP_GAMEPAD)
HostGamepadState sample_gip(PsxGipGamepad* controller) {
    HostGamepadState state;
    PsxGipGamepadState input{};
    if (!controller || !psx_gip_gamepad_get_state(controller, &input)) return state;
    if (input.buttons & PSX_GIP_BUTTON_A) state.buttons |= HOST_PAD_FACE_SOUTH;
    if (input.buttons & PSX_GIP_BUTTON_B) state.buttons |= HOST_PAD_FACE_EAST;
    if (input.buttons & PSX_GIP_BUTTON_X) state.buttons |= HOST_PAD_FACE_WEST;
    if (input.buttons & PSX_GIP_BUTTON_Y) state.buttons |= HOST_PAD_FACE_NORTH;
    if (input.buttons & PSX_GIP_BUTTON_VIEW) state.buttons |= HOST_PAD_BACK;
    if (input.buttons & PSX_GIP_BUTTON_MENU) state.buttons |= HOST_PAD_START;
    if (input.guide) state.buttons |= HOST_PAD_GUIDE;
    if (input.buttons & PSX_GIP_BUTTON_DPAD_UP) state.buttons |= HOST_PAD_DPAD_UP;
    if (input.buttons & PSX_GIP_BUTTON_DPAD_DOWN) state.buttons |= HOST_PAD_DPAD_DOWN;
    if (input.buttons & PSX_GIP_BUTTON_DPAD_LEFT) state.buttons |= HOST_PAD_DPAD_LEFT;
    if (input.buttons & PSX_GIP_BUTTON_DPAD_RIGHT) state.buttons |= HOST_PAD_DPAD_RIGHT;
    if (input.buttons & PSX_GIP_BUTTON_LEFT_BUMPER)
        state.buttons |= HOST_PAD_LEFT_SHOULDER;
    if (input.buttons & PSX_GIP_BUTTON_RIGHT_BUMPER)
        state.buttons |= HOST_PAD_RIGHT_SHOULDER;
    state.left_x = input.left_x;
    state.left_y = input.left_y;
    state.right_x = input.right_x;
    state.right_y = input.right_y;
    state.left_trigger = static_cast<uint16_t>(
        std::min<unsigned>(32767u, static_cast<unsigned>(input.left_trigger) * 32u));
    state.right_trigger = static_cast<uint16_t>(
        std::min<unsigned>(32767u, static_cast<unsigned>(input.right_trigger) * 32u));
    return state;
}
#endif

} // namespace

struct HostGamepad::Impl {
    std::string route = "auto";
    int deadzone = 12000;
    SDL_GameController* sdl = nullptr;
    SDL_JoystickID sdl_instance = -1;
    std::string sdl_selector;
    std::string label;
    HostGamepadState state{};
    Uint32 next_reconnect_tick = 0;
#if defined(PSX_HAVE_GIP_GAMEPAD)
    PsxGipGamepad* gip = nullptr;
    std::string gip_selector;
#endif

    ~Impl() { close_all(); }

    void close_sdl() {
        if (sdl) SDL_GameControllerClose(sdl);
        sdl = nullptr;
        sdl_instance = -1;
        sdl_selector.clear();
    }

#if defined(PSX_HAVE_GIP_GAMEPAD)
    void close_gip() {
        if (gip) psx_gip_gamepad_close(gip);
        gip = nullptr;
        gip_selector.clear();
    }
#endif

    void close_all() {
        close_sdl();
#if defined(PSX_HAVE_GIP_GAMEPAD)
        close_gip();
#endif
        label.clear();
        state = {};
    }

    bool open_sdl(const recomp_gamepad::SdlControllerIdentity& identity) {
        SDL_GameController* handle = SDL_GameControllerOpen(identity.device_index);
        if (!handle) return false;
        close_all();
        sdl = handle;
        sdl_instance = identity.instance_id;
        sdl_selector = identity.persistent_id;
        label = identity.name;
        return true;
    }

#if defined(PSX_HAVE_GIP_GAMEPAD)
    bool open_gip(const std::string& selector, const std::string& name) {
        PsxGipGamepad* handle = psx_gip_gamepad_open(selector.c_str());
        if (!handle) return false;
        close_all();
        gip = handle;
        gip_selector = selector;
        label = name;
        return true;
    }
#endif

    bool has_live_sdl() const {
        return sdl && SDL_GameControllerGetAttached(sdl);
    }

    bool has_live_gip() const {
#if defined(PSX_HAVE_GIP_GAMEPAD)
        return gip && psx_gip_gamepad_connection(gip) == PSX_GIP_CONNECTION_CONNECTED;
#else
        return false;
#endif
    }

    void reconnect(bool force) {
        if (route == "none" || route == "keyboard") {
            close_all();
            return;
        }
        if (has_live_sdl() || has_live_gip()) return;

        const Uint32 now = SDL_GetTicks();
        if (!force && now < next_reconnect_tick) return;
        next_reconnect_tick = now + 1000u;

        if (sdl && !SDL_GameControllerGetAttached(sdl)) close_sdl();
        const auto live = live_sdl_controllers();
        if (route == "auto") {
            if (!live.empty() && open_sdl(live.front())) return;
        } else if (route.rfind("sdl:", 0) == 0 || route.size() == 32) {
            for (const auto& identity : live) {
                if (recomp_gamepad::sdl_controller_id_matches(route, identity) &&
                    open_sdl(identity)) {
                    return;
                }
            }
        }

#if defined(PSX_HAVE_GIP_GAMEPAD)
        if (route.rfind("gip:", 0) == 0 && route != "gip:auto") {
            if (!gip || gip_selector != route) {
                open_gip(route, "Direct USB controller");
            }
            return;
        }
        if (route == "auto" || route == "gip:auto") {
            PsxGipGamepadInfo info[16]{};
            const std::size_t total = psx_gip_gamepad_enumerate(info, 16);
            const std::size_t count = std::min<std::size_t>(total, 16);
            if (count > 0) open_gip(info[0].selector, info[0].name);
        }
#endif
    }
};

HostGamepad::HostGamepad() : impl_(std::make_unique<Impl>()) {}
HostGamepad::~HostGamepad() = default;

bool HostGamepad::is_available() { return true; }

void HostGamepad::load_mapping_database(const char* path) {
    if (path && path[0]) SDL_GameControllerAddMappingsFromFile(path);
}

void HostGamepad::set_route(const std::string& selector) {
    const std::string route = selector.empty() ? "auto" : selector;
    if (impl_->route == route) return;
    impl_->close_all();
    impl_->route = route;
    impl_->next_reconnect_tick = 0;
    impl_->reconnect(true);
}

const std::string& HostGamepad::route() const { return impl_->route; }

void HostGamepad::set_deadzone(int deadzone) {
    impl_->deadzone = std::clamp(deadzone, 0, 32767);
}

int HostGamepad::deadzone() const { return impl_->deadzone; }

void HostGamepad::update() {
    SDL_GameControllerUpdate();
    impl_->reconnect(false);
    if (impl_->has_live_sdl()) impl_->state = sample_sdl(impl_->sdl);
#if defined(PSX_HAVE_GIP_GAMEPAD)
    else if (impl_->gip) impl_->state = sample_gip(impl_->gip);
#endif
    else impl_->state = {};
}

HostGamepadState HostGamepad::state() const { return impl_->state; }

bool HostGamepad::connected() const {
    return impl_->has_live_sdl() || impl_->has_live_gip();
}

std::string HostGamepad::connected_label() const {
    return connected() ? impl_->label : std::string();
}

std::vector<HostControllerOption> HostGamepad::options() const {
    std::vector<HostControllerOption> result = {
        {"none", "None", true},
        {"keyboard", "Keyboard", true},
        {"auto", "Automatic", true},
    };
    const auto live = live_sdl_controllers();
    for (const auto& identity : live) {
        result.push_back({identity.persistent_id, identity.name, true});
    }
#if defined(PSX_HAVE_GIP_GAMEPAD)
    PsxGipGamepadInfo info[16]{};
    const std::size_t total = psx_gip_gamepad_enumerate(info, 16);
    const std::size_t count = std::min<std::size_t>(total, 16);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back({info[i].selector,
                          std::string(info[i].name) + " (direct USB)", true});
    }
#endif
    const bool found = std::any_of(result.begin(), result.end(), [&](const auto& option) {
        if (option.selector == impl_->route) return true;
        if (impl_->route.rfind("sdl:", 0) == 0 &&
            option.selector.rfind("sdl:", 0) == 0) {
            for (const auto& identity : live) {
                if (identity.persistent_id == option.selector &&
                    recomp_gamepad::sdl_controller_id_matches(impl_->route, identity)) {
                    return true;
                }
            }
        }
        return false;
    });
    if (!found && impl_->route != "none" && impl_->route != "keyboard" &&
        impl_->route != "auto") {
        result.push_back({impl_->route, "Saved controller (offline)", false});
    }
    return result;
}

#else

struct HostGamepad::Impl {
    std::string route = "auto";
    int deadzone = 12000;
};

HostGamepad::HostGamepad() : impl_(std::make_unique<Impl>()) {}
HostGamepad::~HostGamepad() = default;
bool HostGamepad::is_available() { return false; }
void HostGamepad::load_mapping_database(const char*) {}
void HostGamepad::set_route(const std::string& selector) {
    impl_->route = selector.empty() ? "auto" : selector;
}
const std::string& HostGamepad::route() const { return impl_->route; }
void HostGamepad::set_deadzone(int deadzone) {
    impl_->deadzone = std::clamp(deadzone, 0, 32767);
}
int HostGamepad::deadzone() const { return impl_->deadzone; }
void HostGamepad::update() {}
HostGamepadState HostGamepad::state() const { return {}; }
bool HostGamepad::connected() const { return false; }
std::string HostGamepad::connected_label() const { return {}; }
std::vector<HostControllerOption> HostGamepad::options() const {
    return {{"none", "None", true}, {"keyboard", "Keyboard", true},
            {"auto", "Automatic", true}};
}

#endif

} // namespace gbarecomp
