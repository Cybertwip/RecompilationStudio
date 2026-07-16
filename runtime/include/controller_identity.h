#ifndef PSXRECOMP_CONTROLLER_IDENTITY_H
#define PSXRECOMP_CONTROLLER_IDENTITY_H

#include <SDL.h>

#include <string>

namespace PSXRecompV4 {

/* A persistent host-controller identity. SDL instance ids only live for one
 * connection, while a bare joystick GUID identifies a controller model and is
 * therefore ambiguous when two identical pads are attached. Prefer a serial
 * number, then the platform device path, and retain the GUID for compatibility
 * with settings.toml files written by older builds. */
struct SdlControllerIdentity {
    int device_index = -1;
    SDL_JoystickID instance_id = -1;
    std::string persistent_id;
    std::string legacy_guid;
    std::string name;
};

/* Pure constructor used by tests and by the live SDL descriptor. Serial/path
 * values are fingerprinted so the settings string is TOML-safe and does not
 * expose a platform device path. */
std::string make_sdl_controller_id(const std::string& guid,
                                   const std::string& serial,
                                   const std::string& path);

/* Describe one SDL game-controller device index. An empty persistent_id means
 * the index is not a usable SDL game controller. */
SdlControllerIdentity describe_sdl_controller(int device_index);

/* Match both the new persistent format and legacy bare-GUID settings. */
bool sdl_controller_id_matches(const std::string& saved,
                               const SdlControllerIdentity& live);

} // namespace PSXRecompV4

#endif
