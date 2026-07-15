cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ---- SDL2 detection ----

if(MSVC)
    file(GLOB SDL2_MSVC_DIR "${CMAKE_SOURCE_DIR}/../sdl2-msvc/SDL2-*")
    if(SDL2_MSVC_DIR)
        set(SDL2_INCLUDE_DIRS "${SDL2_MSVC_DIR}/include")
        set(SDL2_LIBRARIES "${SDL2_MSVC_DIR}/lib/x64/SDL2.lib")
        message(STATUS "SDL2 MSVC: ${SDL2_MSVC_DIR}")
    else()
        message(FATAL_ERROR "SDL2 MSVC dev package not found. Run: bash tools/build_oracle.sh")
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

# ---- Native/oracle runtime targets ----

set(PSXRECOMP_ROOT "${CMAKE_SOURCE_DIR}/..")
include("${CMAKE_SOURCE_DIR}/runtime.cmake")

set(PSXRECOMP_DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN")
if(NOT EXISTS "${PSXRECOMP_DEFAULT_BIOS_PATH}" AND
    EXISTS "${PSXRECOMP_ROOT}/branding/SCPH1001.BIN")
    set(PSXRECOMP_DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/branding/SCPH1001.BIN")
endif()
psxrecomp_add_runtime_target(psx-runtime
    DEBUG_PORT 4370
    WINDOW_TITLE "psxrecomp - BIOS"
    DEFAULT_BIOS_PATH "${PSXRECOMP_DEFAULT_BIOS_PATH}"
)

if(NOT MSVC)
    psxrecomp_add_runtime_target(psx-oracle
        ORACLE
        DEBUG_PORT 4371
        WINDOW_TITLE "psxrecomp - Oracle"
        DEFAULT_BIOS_PATH "${PSXRECOMP_DEFAULT_BIOS_PATH}"
    )
endif()

# Pure protocol conformance plus optional explicit hardware probe. The default
# CTest path never claims a physical controller; pass a proof path and
# --hardware manually to exercise the full USB handshake when a device is attached.
if(PSXRECOMP_GIP_GAMEPAD_AVAILABLE)
    add_executable(gip_gamepad_protocol_test
        tests/gip_gamepad_protocol_test.cpp
        src/gip_gamepad.cpp
    )
    target_include_directories(gip_gamepad_protocol_test PRIVATE include)
    psxrecomp_configure_gip_gamepad(gip_gamepad_protocol_test)
    enable_testing()
    add_test(NAME gip_gamepad_protocol COMMAND gip_gamepad_protocol_test)
endif()
