#include "controller_identity.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>

namespace PSXRecompV4 {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

uint64_t fnv1a64(const std::string& value) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char c : value) {
        hash ^= c;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string fingerprint(const char* kind, const std::string& value) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(value)));
    return std::string(kind) + "-" + hex;
}

} // namespace

std::string make_sdl_controller_id(const std::string& guid,
                                   const std::string& serial,
                                   const std::string& path) {
    const std::string normalized_guid = lower_ascii(guid);
    if (normalized_guid.empty()) return {};

    std::string id = "sdl:" + normalized_guid;
    if (!serial.empty()) return id + ":" + fingerprint("serial", serial);
    if (!path.empty())   return id + ":" + fingerprint("path", path);
    return id;
}

SdlControllerIdentity describe_sdl_controller(int device_index) {
    SdlControllerIdentity out;
    out.device_index = device_index;
    if (device_index < 0 || !SDL_IsGameController(device_index)) return out;

    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(device_index);
    char guid_buf[40] = {};
    SDL_JoystickGetGUIDString(guid, guid_buf, sizeof(guid_buf));
    out.legacy_guid = lower_ascii(guid_buf);
    out.instance_id = SDL_JoystickGetDeviceInstanceID(device_index);

    const char* controller_name = SDL_GameControllerNameForIndex(device_index);
    out.name = controller_name && controller_name[0] ? controller_name : "Controller";

    std::string path;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    if (const char* value = SDL_GameControllerPathForIndex(device_index))
        path = value;
#endif

    std::string serial;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SDL_GameController* handle = nullptr;
    bool opened_here = false;
    if (out.instance_id >= 0)
        handle = SDL_GameControllerFromInstanceID(out.instance_id);
    if (!handle) {
        handle = SDL_GameControllerOpen(device_index);
        opened_here = handle != nullptr;
    }
    if (handle) {
        if (const char* value = SDL_GameControllerGetSerial(handle))
            serial = value;
        if (opened_here) SDL_GameControllerClose(handle);
    }
#endif

    out.persistent_id = make_sdl_controller_id(out.legacy_guid, serial, path);
    return out;
}

bool sdl_controller_id_matches(const std::string& saved,
                               const SdlControllerIdentity& live) {
    if (saved.empty() || live.persistent_id.empty()) return false;
    const std::string normalized = lower_ascii(saved);
    if (normalized == lower_ascii(live.persistent_id)) return true;
    if (normalized == live.legacy_guid) return true;
    return normalized == "sdl:" + live.legacy_guid;
}

} // namespace PSXRecompV4
