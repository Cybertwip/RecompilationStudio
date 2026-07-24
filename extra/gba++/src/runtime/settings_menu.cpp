#include "settings_menu.h"

#include "bitmap_font.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace gbarecomp::runtime {
namespace {

constexpr std::array<const char*, 10> kMainLabels = {
    "RESUME", "SCANLINES", "FILTERING", "SCREEN", "FULLSCREEN",
    "VOLUME", "CONTROLLER", "DEADZONE", "KEYBOARD", "QUIT",
};
constexpr std::array<const char*, 10> kBindingLabels = {
    "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L",
};
constexpr std::array<const char*, 5> kScreens = {
    "raw", "unlit", "frontlit", "backlit", "classic",
};

constexpr uint8_t kPanelBg[3] = {16, 18, 34};
constexpr uint8_t kPanelEdge[3] = {111, 102, 221};
constexpr uint8_t kTitleFg[3] = {238, 241, 248};
constexpr uint8_t kRowFg[3] = {188, 198, 222};
constexpr uint8_t kValueFg[3] = {125, 232, 255};
constexpr uint8_t kSelectFg[3] = {255, 240, 150};
constexpr uint8_t kSelectBar[3] = {44, 50, 96};

int row_count(SettingsMenu::Page page) {
    return page == SettingsMenu::Page::Main ? 10 : 12;
}

void put(uint8_t* rgb, int width, int height, int x, int y,
         const uint8_t color[3]) {
    if (!rgb || x < 0 || y < 0 || x >= width || y >= height) return;
    uint8_t* pixel = rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
    pixel[0] = color[0];
    pixel[1] = color[1];
    pixel[2] = color[2];
}

void fill_rect(uint8_t* rgb, int width, int height,
               int x, int y, int w, int h, const uint8_t color[3]) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(width, x + w);
    const int y1 = std::min(height, y + h);
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) put(rgb, width, height, px, py, color);
}

void frame_rect(uint8_t* rgb, int width, int height,
                int x, int y, int w, int h, const uint8_t color[3]) {
    fill_rect(rgb, width, height, x, y, w, 1, color);
    fill_rect(rgb, width, height, x, y + h - 1, w, 1, color);
    fill_rect(rgb, width, height, x, y, 1, h, color);
    fill_rect(rgb, width, height, x + w - 1, y, 1, h, color);
}

void draw_text(uint8_t* rgb, int width, int height, int x, int y,
               const std::string& text, const uint8_t color[3]) {
    int cursor = x;
    for (char c : text) {
        const uint8_t* glyph = bitmap_font::glyph(c);
        for (int gy = 0; gy < bitmap_font::kHeight; ++gy) {
            for (int gx = 0; gx < bitmap_font::kWidth; ++gx) {
                if ((glyph[gy] & (0x80u >> gx)) != 0)
                    put(rgb, width, height, cursor + gx, y + gy, color);
            }
        }
        cursor += bitmap_font::kWidth + 1;
    }
}

std::string upper_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return value;
}

int controller_index(const FrontendSettings& settings,
                     const std::vector<HostControllerOption>& controllers) {
    for (std::size_t i = 0; i < controllers.size(); ++i)
        if (controllers[i].selector == settings.controller) return static_cast<int>(i);
    return controllers.empty() ? -1 : 0;
}

std::string controller_label(const FrontendSettings& settings,
                             const std::vector<HostControllerOption>& controllers) {
    const int index = controller_index(settings, controllers);
    if (index >= 0) return upper_ascii(controllers[static_cast<std::size_t>(index)].label);
    return upper_ascii(settings.controller);
}

std::string main_value(int row, const FrontendSettings& settings,
                       const std::vector<HostControllerOption>& controllers) {
    switch (row) {
        case 1: return settings.scanlines ? "ON" : "OFF";
        case 2: return settings.linear_filter ? "LINEAR" : "NEAREST";
        case 3: return upper_ascii(settings.screen);
        case 4:
            return settings.fullscreen == 2 ? "EXCLUSIVE"
                 : settings.fullscreen == 1 ? "BORDERLESS" : "WINDOWED";
        case 5: return std::to_string(settings.volume) + "%";
        case 6: return controller_label(settings, controllers);
        case 7: return std::to_string(settings.deadzone * 100 / 32767) + "%";
        case 8: return "EDIT";
        default: return {};
    }
}

void cycle_screen(FrontendSettings& settings, bool forward) {
    int index = 0;
    for (std::size_t i = 0; i < kScreens.size(); ++i)
        if (settings.screen == kScreens[i]) index = static_cast<int>(i);
    index = forward ? (index + 1) % static_cast<int>(kScreens.size())
                    : (index + static_cast<int>(kScreens.size()) - 1) %
                          static_cast<int>(kScreens.size());
    settings.screen = kScreens[static_cast<std::size_t>(index)];
}

void cycle_controller(FrontendSettings& settings,
                      const std::vector<HostControllerOption>& controllers,
                      bool forward) {
    if (controllers.empty()) {
        settings.controller = "auto";
        return;
    }
    int index = controller_index(settings, controllers);
    if (index < 0) index = 0;
    const int count = static_cast<int>(controllers.size());
    index = forward ? (index + 1) % count : (index + count - 1) % count;
    settings.controller = controllers[static_cast<std::size_t>(index)].selector;
}

} // namespace

void SettingsMenu::open() {
    open_ = true;
    page_ = Page::Main;
    selected_ = 0;
    capture_binding_ = -1;
}

void SettingsMenu::finish_binding_capture() { capture_binding_ = -1; }

SettingsMenuResult SettingsMenu::handle(
    SettingsMenuInput input,
    FrontendSettings& settings,
    const std::vector<HostControllerOption>& controllers) {
    SettingsMenuResult result;
    if (!open_) return result;

    if (capture_binding_ >= 0) {
        if (input == SettingsMenuInput::Cancel) capture_binding_ = -1;
        return result;
    }

    const int count = row_count(page_);
    if (input == SettingsMenuInput::Up) {
        selected_ = (selected_ + count - 1) % count;
        return result;
    }
    if (input == SettingsMenuInput::Down) {
        selected_ = (selected_ + 1) % count;
        return result;
    }
    if (input == SettingsMenuInput::Cancel) {
        if (page_ == Page::Keyboard) {
            page_ = Page::Main;
            selected_ = 8;
        } else {
            open_ = false;
            result.close = true;
        }
        return result;
    }

    const bool forward = input != SettingsMenuInput::Left;
    const bool change = input == SettingsMenuInput::Left ||
                        input == SettingsMenuInput::Right ||
                        input == SettingsMenuInput::Confirm;
    if (!change) return result;

    if (page_ == Page::Keyboard) {
        if (selected_ == 0) {
            page_ = Page::Main;
            selected_ = 8;
        } else if (selected_ >= 1 && selected_ <= 10) {
            capture_binding_ = selected_ - 1;
            result.capture_binding = capture_binding_;
        } else if (selected_ == 11) {
            result.reset_bindings = true;
        }
        return result;
    }

    switch (selected_) {
        case 0:
            if (input == SettingsMenuInput::Confirm) {
                open_ = false;
                result.close = true;
            }
            break;
        case 1:
            settings.scanlines = !settings.scanlines;
            result.settings_changed = true;
            break;
        case 2:
            settings.linear_filter = !settings.linear_filter;
            result.settings_changed = true;
            break;
        case 3:
            cycle_screen(settings, forward);
            result.settings_changed = true;
            break;
        case 4:
            settings.fullscreen = forward ? (settings.fullscreen + 1) % 3
                : (settings.fullscreen + 2) % 3;
            result.settings_changed = true;
            break;
        case 5:
            settings.volume = forward ? settings.volume + 10 : settings.volume - 10;
            if (settings.volume > 100) settings.volume = 0;
            if (settings.volume < 0) settings.volume = 100;
            result.settings_changed = true;
            break;
        case 6:
            cycle_controller(settings, controllers, forward);
            result.settings_changed = true;
            result.controller_changed = true;
            break;
        case 7: {
            int pct = settings.deadzone * 100 / 32767;
            pct = ((pct + 2) / 5) * 5;
            pct = forward ? pct + 5 : pct - 5;
            if (pct > 50) pct = 0;
            if (pct < 0) pct = 50;
            settings.deadzone = pct * 32767 / 100;
            result.settings_changed = true;
            result.controller_changed = true;
            break;
        }
        case 8:
            if (input == SettingsMenuInput::Confirm) {
                page_ = Page::Keyboard;
                selected_ = 0;
            }
            break;
        case 9:
            if (input == SettingsMenuInput::Confirm) {
                open_ = false;
                result.close = true;
                result.quit = true;
            }
            break;
    }
    return result;
}

void SettingsMenu::draw(
    uint8_t* rgb, int width, int height,
    const FrontendSettings& settings,
    const std::vector<HostControllerOption>& controllers,
    const std::array<std::string, 10>& key_labels) const {
    if (!open_ || !rgb || width <= 0 || height <= 0) return;

    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < pixels; ++i) {
        uint8_t* pixel = rgb + i * 3u;
        pixel[0] = static_cast<uint8_t>(pixel[0] * 5u / 16u);
        pixel[1] = static_cast<uint8_t>(pixel[1] * 5u / 16u);
        pixel[2] = static_cast<uint8_t>(pixel[2] * 5u / 16u);
    }

    constexpr int pad = 4;
    constexpr int line_height = 10;
    const int panel_x = 4;
    const int panel_y = 4;
    const int panel_w = std::min(width - 8, 232);
    const int count = row_count(page_);
    const int panel_h = std::min(height - 8, pad * 2 + 12 + count * line_height);
    fill_rect(rgb, width, height, panel_x, panel_y, panel_w, panel_h, kPanelBg);
    frame_rect(rgb, width, height, panel_x, panel_y, panel_w, panel_h, kPanelEdge);

    const std::string title = page_ == Page::Main ? "PAUSED - SETTINGS"
                                                  : "KEYBOARD CONTROLS";
    draw_text(rgb, width, height, panel_x + pad, panel_y + pad,
              title, kTitleFg);
    fill_rect(rgb, width, height, panel_x + pad, panel_y + pad + 9,
              panel_w - pad * 2, 1, kPanelEdge);

    int y = panel_y + pad + 12;
    for (int row = 0; row < count; ++row, y += line_height) {
        const bool selected = row == selected_;
        if (selected) {
            fill_rect(rgb, width, height, panel_x + 2, y - 1,
                      panel_w - 4, line_height, kSelectBar);
        }
        const uint8_t* fg = selected ? kSelectFg : kRowFg;
        draw_text(rgb, width, height, panel_x + pad, y,
                  selected ? ">" : " ", fg);

        std::string label;
        std::string value;
        if (page_ == Page::Main) {
            label = kMainLabels[static_cast<std::size_t>(row)];
            value = main_value(row, settings, controllers);
        } else if (row == 0) {
            label = "BACK";
        } else if (row >= 1 && row <= 10) {
            const int binding = row - 1;
            label = kBindingLabels[static_cast<std::size_t>(binding)];
            value = capture_binding_ == binding ? "PRESS KEY"
                                                : upper_ascii(key_labels[static_cast<std::size_t>(binding)]);
        } else {
            label = "RESET DEFAULTS";
        }

        const int glyph_step = bitmap_font::kWidth + 1;
        const int label_x = panel_x + pad + glyph_step;
        const int right = panel_x + panel_w - pad;
        int value_x = right - static_cast<int>(value.size()) * glyph_step;
        const int max_label_chars = value.empty()
            ? std::max(0, (right - label_x) / glyph_step)
            : std::max(0, (value_x - label_x - glyph_step) / glyph_step);
        if (static_cast<int>(label.size()) > max_label_chars)
            label.resize(static_cast<std::size_t>(max_label_chars));
        const int max_value_chars = std::max(0, (right - label_x) / glyph_step);
        if (static_cast<int>(value.size()) > max_value_chars) {
            value = value.substr(value.size() - static_cast<std::size_t>(max_value_chars));
            value_x = right - static_cast<int>(value.size()) * glyph_step;
        }
        draw_text(rgb, width, height, label_x, y, label, fg);
        if (!value.empty()) {
            draw_text(rgb, width, height, value_x, y, value,
                      selected ? kSelectFg : kValueFg);
        }
    }
}

} // namespace gbarecomp::runtime
