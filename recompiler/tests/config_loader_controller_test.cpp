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
        check(default_config.runtime.default_p1_device == "keyboard",
              "absent p1_device fallback");
        check(!default_config.runtime.has_p2_device, "absent p2_device remains unset");
        check(default_config.runtime.default_p2_device == "none",
              "absent p2_device fallback");


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
