cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)

include(CTest)

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

if(BUILD_TESTING)
    add_executable(pad_negotiation_test
        tests/pad_negotiation_test.c
        src/pad_negotiation.c
    )
    target_include_directories(pad_negotiation_test PRIVATE include)
    add_test(NAME pad_negotiation COMMAND pad_negotiation_test)

    add_executable(host_input_policy_test
        tests/host_input_policy_test.cpp
        src/psx_keybinds.c
        src/controller_identity.cpp
    )
    target_include_directories(host_input_policy_test PRIVATE
        include
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(host_input_policy_test PRIVATE ${SDL2_LIBRARIES})
    add_test(NAME host_input_policy COMMAND host_input_policy_test)

    if(PSX_SETTINGS_MENU)
        psxrecomp_ensure_launcher_libs()
        add_executable(frontend_menu_smoke_test
            tests/frontend_menu_smoke_test.cpp
            launcher/launcher.cpp
            launcher/stb_image_impl.cpp
            ${PSXRECOMP_ROOT}/lib/RmlUi/Backends/RmlUi_Platform_SDL.cpp
            ${PSXRECOMP_ROOT}/lib/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
            src/controller_identity.cpp
            src/disc_identity.cpp
            src/crc32.c
            src/memcard.c
            src/psx_keybinds.c
            ${PSXRECOMP_ROOT}/recompiler/src/config_loader.cpp
            ${PSXRECOMP_ROOT}/recompiler/src/ps1_exe_parser.cpp
        )
        target_include_directories(frontend_menu_smoke_test PRIVATE
            include
            launcher
            ${PSXRECOMP_ROOT}/recompiler/src
            ${PSXRECOMP_ROOT}/recompiler/include
            ${PSXRECOMP_ROOT}/recompiler/lib/fmt/include
            ${PSXRECOMP_ROOT}/recompiler/lib/toml11
            ${PSXRECOMP_ROOT}/lib/RmlUi/Include
            ${PSXRECOMP_ROOT}/lib/RmlUi/Backends
            ${SDL2_INCLUDE_DIRS}
        )
        target_compile_definitions(frontend_menu_smoke_test PRIVATE
            FMT_HEADER_ONLY=1
            PSX_TEST_ASSETS_DIR="${PSXRECOMP_ROOT}/runtime/launcher/assets"
        )
        if(NOT MSVC)
            set_source_files_properties(
                ${PSXRECOMP_ROOT}/lib/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
                PROPERTIES COMPILE_OPTIONS "-include;algorithm")
        endif()
        target_link_libraries(frontend_menu_smoke_test PRIVATE
            RmlUi::Core ${SDL2_LIBRARIES})
        if(SDL2_LIBRARY_DIRS)
            target_link_directories(frontend_menu_smoke_test PRIVATE ${SDL2_LIBRARY_DIRS})
        endif()
        if(WIN32 OR MINGW)
            target_link_libraries(frontend_menu_smoke_test PRIVATE comdlg32 opengl32)
        elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_package(OpenGL)
            if(OpenGL_FOUND)
                target_link_libraries(frontend_menu_smoke_test PRIVATE OpenGL::GL)
            endif()
        endif()
        # Deliberately not registered with CTest: it needs a real WindowServer/X
        # display and is a manual visual-proof target for developer machines.
    endif()

    add_executable(text_xlate_policy_test
        tests/text_xlate_policy_test.cpp
    )
    target_include_directories(text_xlate_policy_test PRIVATE include)
    add_test(NAME text_xlate_policy COMMAND text_xlate_policy_test)

    add_executable(tcb_status_restore_test
        tests/tcb_status_restore_test.c
    )
    target_include_directories(tcb_status_restore_test PRIVATE include)
    add_test(NAME tcb_status_restore COMMAND tcb_status_restore_test)

    add_executable(main_ram_mirror_test
        tests/main_ram_mirror_test.c
    )
    target_include_directories(main_ram_mirror_test PRIVATE include)
    add_test(NAME main_ram_mirror COMMAND main_ram_mirror_test)

    add_executable(starvation_release_policy_test
        tests/starvation_release_policy_test.c
        src/starvation_ring.c
    )
    target_include_directories(starvation_release_policy_test PRIVATE include)
    target_compile_definitions(starvation_release_policy_test PRIVATE PSX_NO_DEBUG_TOOLS=1)
    add_test(NAME starvation_release_policy COMMAND starvation_release_policy_test)

    add_executable(gte_trace_policy_test
        tests/gte_trace_policy_test.cpp
    )
    target_include_directories(gte_trace_policy_test PRIVATE include)
    add_test(NAME gte_trace_policy COMMAND gte_trace_policy_test)

    add_executable(gte_register_file_test
        tests/gte_register_file_test.cpp
        src/gte.cpp
    )
    target_include_directories(gte_register_file_test PRIVATE include)
    add_test(NAME gte_register_file COMMAND gte_register_file_test)
endif()
