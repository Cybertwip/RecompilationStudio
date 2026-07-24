#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gbarecomp {

enum HostGamepadButton : uint32_t {
    HOST_PAD_FACE_SOUTH    = 1u << 0,
    HOST_PAD_FACE_EAST     = 1u << 1,
    HOST_PAD_FACE_WEST     = 1u << 2,
    HOST_PAD_FACE_NORTH    = 1u << 3,
    HOST_PAD_BACK          = 1u << 4,
    HOST_PAD_START         = 1u << 5,
    HOST_PAD_GUIDE         = 1u << 6,
    HOST_PAD_DPAD_UP       = 1u << 7,
    HOST_PAD_DPAD_DOWN     = 1u << 8,
    HOST_PAD_DPAD_LEFT     = 1u << 9,
    HOST_PAD_DPAD_RIGHT    = 1u << 10,
    HOST_PAD_LEFT_SHOULDER = 1u << 11,
    HOST_PAD_RIGHT_SHOULDER= 1u << 12,
};

struct HostGamepadState {
    uint32_t buttons = 0;
    int16_t left_x = 0;
    int16_t left_y = 0;
    int16_t right_x = 0;
    int16_t right_y = 0;
    uint16_t left_trigger = 0;   // normalized 0..32767
    uint16_t right_trigger = 0;  // normalized 0..32767
};

struct HostControllerOption {
    std::string selector;  // none | keyboard | auto | sdl:* | gip:*
    std::string label;
    bool available = true;
};

// Map a normalized physical controller onto GBA KEYINPUT (active-low). The
// face-button layout follows the established gba-recomp defaults: east=A,
// south=B, with north/west accepted as ergonomic alternates.
uint16_t gba_keyinput_from_gamepad(const HostGamepadState& state,
                                   int deadzone);

class HostGamepad {
public:
    HostGamepad();
    ~HostGamepad();

    HostGamepad(const HostGamepad&) = delete;
    HostGamepad& operator=(const HostGamepad&) = delete;

    static bool is_available();

    void load_mapping_database(const char* path);
    void set_route(const std::string& selector);
    const std::string& route() const;
    void set_deadzone(int deadzone);
    int deadzone() const;

    // Refresh hot-plug state and copy the latest coherent physical state.
    void update();
    HostGamepadState state() const;
    bool connected() const;
    std::string connected_label() const;

    // Enumerate built-ins and live controllers. A selected offline identity is
    // retained as an unavailable row so settings survive unplug/reconnect.
    std::vector<HostControllerOption> options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gbarecomp
