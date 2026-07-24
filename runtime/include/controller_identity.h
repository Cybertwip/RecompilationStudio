#ifndef PSXRECOMP_CONTROLLER_IDENTITY_H
#define PSXRECOMP_CONTROLLER_IDENTITY_H

#include "../../lib/recomp_gamepad/include/recomp_gamepad/controller_identity.h"

namespace PSXRecompV4 {

using SdlControllerIdentity = recomp_gamepad::SdlControllerIdentity;

inline std::string make_sdl_controller_id(const std::string& guid,
                                          const std::string& serial,
                                          const std::string& path) {
    return recomp_gamepad::make_sdl_controller_id(guid, serial, path);
}

inline SdlControllerIdentity describe_sdl_controller(int device_index) {
    return recomp_gamepad::describe_sdl_controller(device_index);
}

inline bool sdl_controller_id_matches(const std::string& saved,
                                      const SdlControllerIdentity& live) {
    return recomp_gamepad::sdl_controller_id_matches(saved, live);
}

} // namespace PSXRecompV4

#endif
