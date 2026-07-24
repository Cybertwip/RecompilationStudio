#pragma once

#include "frontend_settings.h"
#include "host_gamepad.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp::runtime {

enum class SettingsMenuInput {
    Up,
    Down,
    Left,
    Right,
    Confirm,
    Cancel,
};

struct SettingsMenuResult {
    bool close = false;
    bool quit = false;
    bool settings_changed = false;
    bool controller_changed = false;
    bool reset_bindings = false;
    int capture_binding = -1;
};

class SettingsMenu {
public:
    enum class Page { Main, Keyboard };

    void open();
    bool is_open() const { return open_; }
    bool capturing_binding() const { return capture_binding_ >= 0; }
    int capture_binding() const { return capture_binding_; }
    void finish_binding_capture();

    SettingsMenuResult handle(
        SettingsMenuInput input,
        FrontendSettings& settings,
        const std::vector<HostControllerOption>& controllers);

    // Composite the menu into a host-owned RGB888 copy of the last game frame.
    void draw(uint8_t* rgb888, int width, int height,
              const FrontendSettings& settings,
              const std::vector<HostControllerOption>& controllers,
              const std::array<std::string, 10>& key_labels) const;

private:
    bool open_ = false;
    Page page_ = Page::Main;
    int selected_ = 0;
    int capture_binding_ = -1;
};

} // namespace gbarecomp::runtime
