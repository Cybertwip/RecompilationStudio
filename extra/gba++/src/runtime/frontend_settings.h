#pragma once

#include <filesystem>
#include <string>

namespace gbarecomp::runtime {

struct FrontendSettings {
    bool scanlines = true;
    bool linear_filter = false;
    std::string screen = "frontlit";
    int fullscreen = 0;          // 0 windowed, 1 borderless, 2 exclusive
    int volume = 100;            // 0..100
    std::string controller = "auto";
    int deadzone = 12000;        // SDL axis units, 0..32767
};

// Load a tolerant user override layer. Missing files are success and leave the
// caller-provided seed unchanged. Recognized values are clamped/validated;
// unrelated keys remain forward-compatible and are ignored.
bool load_frontend_settings(const std::filesystem::path& path,
                            FrontendSettings& settings,
                            std::string* error = nullptr);

// Atomically replace the user settings file with the complete host-facing
// settings model. This file never contains guest/recompiler configuration.
bool save_frontend_settings(const std::filesystem::path& path,
                            const FrontendSettings& settings,
                            std::string* error = nullptr);

} // namespace gbarecomp::runtime
