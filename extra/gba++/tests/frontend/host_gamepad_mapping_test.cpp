#include "host_gamepad.h"

#include <cstdint>
#include <iostream>

namespace {
bool pressed(uint16_t keyinput, int bit) {
    return (keyinput & (1u << bit)) == 0;
}
}

int main() {
    gbarecomp::HostGamepadState state;
    state.buttons = gbarecomp::HOST_PAD_FACE_EAST |
                    gbarecomp::HOST_PAD_FACE_SOUTH |
                    gbarecomp::HOST_PAD_BACK |
                    gbarecomp::HOST_PAD_START |
                    gbarecomp::HOST_PAD_LEFT_SHOULDER;
    state.left_x = 20000;
    state.left_y = -20000;
    state.right_trigger = 10000;

    const uint16_t keys = gbarecomp::gba_keyinput_from_gamepad(state, 12000);
    for (int bit : {0, 1, 2, 3, 4, 6, 8, 9}) {
        if (!pressed(keys, bit)) return 1;
    }
    if (pressed(keys, 5) || pressed(keys, 7)) return 2;

    state = {};
    state.left_x = 1000;
    state.left_y = -1000;
    if (gbarecomp::gba_keyinput_from_gamepad(state, 12000) != 0x03FFu) return 3;

    std::cout << "host_gamepad_mapping_test: ok\n";
    return 0;
}
