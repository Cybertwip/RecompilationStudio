#include "frontend_settings.h"

#include "color_lut.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string_view>

namespace gbarecomp::runtime {
namespace {

std::string trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() &&
           std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

std::string strip_comment(std::string_view text) {
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted && c == '\\' && !escaped) {
            escaped = true;
            continue;
        }
        if (c == '"' && !escaped) quoted = !quoted;
        if (c == '#' && !quoted) return std::string(text.substr(0, i));
        escaped = false;
    }
    return std::string(text);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parse_bool(const std::string& value, bool& out) {
    const std::string v = lower_ascii(trim(value));
    if (v == "true" || v == "1" || v == "on") {
        out = true;
        return true;
    }
    if (v == "false" || v == "0" || v == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_int(const std::string& value, int& out) {
    const std::string v = trim(value);
    if (v.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(v.c_str(), &end, 0);
    if (end == v.c_str() || *end != '\0') return false;
    out = static_cast<int>(parsed);
    return true;
}

std::string parse_string(std::string value) {
    value = trim(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return value;
    std::string out;
    out.reserve(value.size() - 2);
    bool escaped = false;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        const char c = value[i];
        if (escaped) {
            if (c == 'n') out.push_back('\n');
            else if (c == 'r') out.push_back('\r');
            else if (c == 't') out.push_back('\t');
            else out.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else {
            out.push_back(c);
        }
    }
    if (escaped) out.push_back('\\');
    return out;
}

std::string quote(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool valid_screen(const std::string& value) {
    ScreenKind kind;
    return screen_kind_from_name(value, kind);
}

void normalize(FrontendSettings& settings) {
    if (!valid_screen(settings.screen)) settings.screen = "frontlit";
    settings.fullscreen = std::clamp(settings.fullscreen, 0, 2);
    settings.volume = std::clamp(settings.volume, 0, 100);
    settings.deadzone = std::clamp(settings.deadzone, 0, 32767);
    if (settings.controller.empty()) settings.controller = "auto";
}

} // namespace

bool load_frontend_settings(const std::filesystem::path& path,
                            FrontendSettings& settings,
                            std::string* error) {
    if (path.empty()) return true;
    std::ifstream input(path);
    if (!input) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return true;
        if (error) *error = "could not open user settings " + path.string();
        return false;
    }

    std::string section;
    std::string raw;
    while (std::getline(input, raw)) {
        const std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower_ascii(trim(
                std::string_view(line).substr(1, line.size() - 2)));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower_ascii(trim(
            std::string_view(line).substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));

        if (section == "video" && key == "scanlines") {
            bool parsed = false;
            if (parse_bool(value, parsed)) settings.scanlines = parsed;
        } else if (section == "video" &&
                   (key == "linear_filter" || key == "linear_filtering")) {
            bool parsed = false;
            if (parse_bool(value, parsed)) settings.linear_filter = parsed;
        } else if (section == "video" && key == "screen") {
            const std::string parsed = lower_ascii(parse_string(value));
            if (valid_screen(parsed)) settings.screen = parsed;
        } else if (section == "video" && key == "fullscreen") {
            bool boolean = false;
            int integer = 0;
            if (parse_bool(value, boolean)) settings.fullscreen = boolean ? 1 : 0;
            else if (parse_int(value, integer)) settings.fullscreen = integer;
        } else if (section == "audio" && key == "volume") {
            int parsed = 0;
            if (parse_int(value, parsed)) settings.volume = parsed;
        } else if (section == "controller" && key == "device") {
            const std::string parsed = parse_string(value);
            if (!parsed.empty()) settings.controller = parsed;
        } else if (section == "controller" && key == "deadzone") {
            int parsed = 0;
            if (parse_int(value, parsed)) settings.deadzone = parsed;
        }
    }
    if (!input.eof() && input.fail()) {
        if (error) *error = "could not read user settings " + path.string();
        return false;
    }
    normalize(settings);
    return true;
}

bool save_frontend_settings(const std::filesystem::path& path,
                            const FrontendSettings& settings_in,
                            std::string* error) {
    if (path.empty()) return true;
    FrontendSettings settings = settings_in;
    normalize(settings);

    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error) *error = "could not create user settings directory " +
                                path.parent_path().string();
            return false;
        }
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            if (error) *error = "could not create user settings " + temporary.string();
            return false;
        }
        output << "# GBARecomp host settings. Guest execution is unaffected.\n\n";
        output << "[video]\n";
        output << "scanlines = " << (settings.scanlines ? "true" : "false") << "\n";
        output << "linear_filter = "
               << (settings.linear_filter ? "true" : "false") << "\n";
        output << "screen = " << quote(settings.screen) << "\n";
        output << "fullscreen = " << settings.fullscreen << "\n\n";
        output << "[audio]\n";
        output << "volume = " << settings.volume << "\n\n";
        output << "[controller]\n";
        output << "device = " << quote(settings.controller) << "\n";
        output << "deadzone = " << settings.deadzone << "\n";
        if (!output.good()) {
            if (error) *error = "could not write user settings " + temporary.string();
            output.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) {
        if (error) *error = "could not replace user settings " + path.string();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

} // namespace gbarecomp::runtime
