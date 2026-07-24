#include "frontend_settings.h"

#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "gbarecomp-frontend-settings-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return 1;

    const fs::path path = dir / "settings.toml";
    gbarecomp::runtime::FrontendSettings original;
    original.scanlines = false;
    original.linear_filter = true;
    original.screen = "backlit";
    original.fullscreen = 2;
    original.volume = 60;
    original.controller = "sdl:03000000abcdef";
    original.deadzone = 8192;

    std::string error;
    if (!gbarecomp::runtime::save_frontend_settings(path, original, &error)) {
        std::cerr << error << '\n';
        return 2;
    }

    gbarecomp::runtime::FrontendSettings loaded;
    if (!gbarecomp::runtime::load_frontend_settings(path, loaded, &error)) {
        std::cerr << error << '\n';
        return 3;
    }
    if (loaded.scanlines != original.scanlines ||
        loaded.linear_filter != original.linear_filter ||
        loaded.screen != original.screen ||
        loaded.fullscreen != original.fullscreen ||
        loaded.volume != original.volume ||
        loaded.controller != original.controller ||
        loaded.deadzone != original.deadzone) {
        return 4;
    }

    std::ofstream(path, std::ios::trunc)
        << "[video]\nscanlines = true\nfullscreen = 99\nscreen = \"invalid\"\n"
        << "[audio]\nvolume = -50\n[controller]\ndeadzone = 99999\n";
    loaded.screen = "classic";
    if (!gbarecomp::runtime::load_frontend_settings(path, loaded, &error)) return 5;
    if (!loaded.scanlines || loaded.fullscreen != 2 || loaded.volume != 0 ||
        loaded.deadzone != 32767 || loaded.screen != "classic") {
        return 6;
    }

    fs::remove_all(dir, ec);
    std::cout << "frontend_settings_test: ok\n";
    return 0;
}
