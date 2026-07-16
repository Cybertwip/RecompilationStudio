#include "config_loader.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

fs::path make_case(const fs::path& root, const char* name,
                   const std::string& controller_block,
                   bool include_runtime = true) {
    const fs::path dir = root / name;
    fs::create_directories(dir);
    std::ofstream out(dir / "game.toml", std::ios::binary);
    out << "[game]\n"
           "name = \"Controller config test\"\n"
           "id = \"TEST-00000\"\n"
           "exe = \"TEST_000.00\"\n"
           "load_address = \"0x80010000\"\n"
           "entry_pc = \"0x80010000\"\n"
           "text_size = \"0x1000\"\n"
           "stack_base = \"0x801FFFF0\"\n\n";
    if (include_runtime) {
        out << "[runtime]\n"
               "window_title = \"Controller config test\"\n\n";
    }
    out << "[recompiler]\n"
           "seeds = \"seeds.txt\"\n"
           "strict = true\n\n"
        << controller_block;
    out.close();
    return dir / "game.toml";
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("psxrecomp-controller-config-" + std::to_string(nonce));
    fs::create_directories(root);

    try {
        const fs::path configured = make_case(
            root, "configured",
            "[controller]\n"
            "p1_device = \"auto\"\n"
            "p2_device = \"gip:0e6f:02f1:001:2.3\"\n"
            "default_mode = \"digital\"\n");
        const auto config = PSXRecompV4::load_game_config(configured);
        check(config.runtime.has_p1_device, "p1_device presence");
        check(config.runtime.default_p1_device == "auto", "p1_device value");
        check(config.runtime.has_p2_device, "p2_device presence");
        check(config.runtime.default_p2_device == "gip:0e6f:02f1:001:2.3",
              "p2_device value");

        const fs::path defaults = make_case(root, "defaults", "");
        const auto default_config = PSXRecompV4::load_game_config(defaults);
        check(!default_config.runtime.has_p1_device, "absent p1_device remains unset");
        check(default_config.runtime.default_p1_device == "auto",
              "absent p1_device fallback");
        check(!default_config.runtime.has_p2_device, "absent p2_device remains unset");
        check(default_config.runtime.default_p2_device == "auto",
              "absent p2_device fallback");
        check(default_config.runtime.default_p1_mode ==
                  PSXRecompV4::PAD_MODE_ANALOG,
              "absent p1_mode defaults to analog DualShock");
        check(default_config.runtime.video_renderer == 1,
              "absent renderer defaults to OpenGL");

        const fs::path settings_path = root / "settings.toml";
        PSXRecompV4::UserSettings saved;
        saved.p1_device =
            "sdl:030000005e0400008e02000000000000:serial-0123456789abcdef";
        saved.p2_device =
            "sdl:030000004c050000e60c000000000000:path-fedcba9876543210";
        saved.has_p1_device = saved.has_p2_device = true;
        saved.p1_mode = PSXRecompV4::PAD_MODE_ANALOG;
        saved.p2_mode = PSXRecompV4::PAD_MODE_DIGITAL;
        saved.has_p1_mode = saved.has_p2_mode = true;
        saved.renderer = 1; saved.has_renderer = true;
        check(PSXRecompV4::save_user_settings(settings_path, saved),
              "controller settings save");
        const auto loaded = PSXRecompV4::load_user_settings(settings_path);
        check(loaded.has_p1_device && loaded.p1_device == saved.p1_device,
              "P1 persistent identity round trip");
        check(loaded.has_p2_device && loaded.p2_device == saved.p2_device,
              "P2 persistent identity round trip");
        check(loaded.has_p1_mode && loaded.p1_mode == PSXRecompV4::PAD_MODE_ANALOG,
              "analog mode round trip");
        check(loaded.has_renderer && loaded.renderer == 1,
              "OpenGL setting round trip");


        const fs::path no_runtime = make_case(
            root, "no-runtime", "[controller]\np1_device = \"auto\"\n", false);
        const auto no_runtime_config = PSXRecompV4::load_game_config(no_runtime);
        check(no_runtime_config.runtime.has_p1_device,
              "top-level controller loads without a runtime block");
        check(no_runtime_config.runtime.default_p1_device == "auto",
              "top-level controller value without a runtime block");

        bool rejected_empty = false;
        try {
            const fs::path empty = make_case(
                root, "empty", "[controller]\np1_device = \"\"\n");
            (void)PSXRecompV4::load_game_config(empty);
        } catch (const std::exception&) {
            rejected_empty = true;
        }
        check(rejected_empty, "empty packaged device is rejected");
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: unexpected exception: " << ex.what() << '\n';
        ++failures;
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return failures == 0 ? 0 : 1;
}
