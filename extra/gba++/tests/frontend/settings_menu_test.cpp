#include "settings_menu.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace gbarecomp;
    using namespace gbarecomp::runtime;

    FrontendSettings settings;
    std::vector<HostControllerOption> controllers = {
        {"none", "None", true}, {"keyboard", "Keyboard", true},
        {"auto", "Automatic", true}, {"sdl:test", "Test Pad", true},
    };
    SettingsMenu menu;
    menu.open();
    menu.handle(SettingsMenuInput::Down, settings, controllers);
    auto result = menu.handle(SettingsMenuInput::Confirm, settings, controllers);
    if (settings.scanlines || !result.settings_changed) return 1;

    for (int i = 0; i < 5; ++i) menu.handle(SettingsMenuInput::Down, settings, controllers);
    result = menu.handle(SettingsMenuInput::Right, settings, controllers);
    if (settings.controller != "sdl:test" || !result.controller_changed) return 2;

    std::array<std::string, 10> labels = {
        "X", "Z", "Right Shift", "Return", "Right",
        "Left", "Up", "Down", "V", "C",
    };
    std::vector<uint8_t> frame(240u * 160u * 3u, 180u);
    menu.draw(frame.data(), 240, 160, settings, controllers, labels);
    std::size_t changed = 0;
    for (uint8_t value : frame) if (value != 180u) ++changed;
    if (changed < frame.size() / 2u) return 3;

    std::cout << "settings_menu_test: ok changed=" << changed << '\n';
    return 0;
}
