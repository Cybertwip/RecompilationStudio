cmake_minimum_required(VERSION 3.20)
project(horizon2mvii VERSION 0.1.0 LANGUAGES C ASM)

get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PSX_VIRTUA_SUPPORT_ROOT "${PSXRECOMP_ROOT}/extra/virtua")
include("${PSX_VIRTUA_SUPPORT_ROOT}/CMake/VirtuaArmApp.cmake")

add_library(horizon-mvii-transport STATIC source/horizon_servctl_mvii.c)
target_include_directories(horizon-mvii-transport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}/Dash"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}"
    "${PSX_VIRTUA_SUPPORT_ROOT}/include")
target_compile_options(horizon-mvii-transport PRIVATE
    --target=armv7a-none-eabi -mcpu=cortex-a7 -marm -mfloat-abi=soft
    -ffreestanding -fpie -fno-stack-protector -fno-common -Os)

virtua_add_armv7_app(horizon2mvii_app
    GUI
    COOPERATIVE
    OUTPUT_NAME "horizon2mvii"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/source/main.c")

set(HORIZON2MVII_PAYLOAD "" CACHE FILEPATH
    "Optional NRO/NSO/NCA/NSP/deconstructed image staged as application.nsp")
set(_STAGE "${CMAKE_BINARY_DIR}/steganos-package/horizon2mvii/$<CONFIG>")
set(_STAMP "${CMAKE_BINARY_DIR}/horizon2mvii-package.stamp")
set(_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${horizon2mvii_app_VIRTUA_FILE}" "${_STAGE}/horizon2mvii.virtua"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${_STAGE}/README.md")
set(_DEPENDS horizon2mvii_app horizon-mvii-transport "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
if(HORIZON2MVII_PAYLOAD)
    if(NOT EXISTS "${HORIZON2MVII_PAYLOAD}")
        message(FATAL_ERROR "HORIZON2MVII_PAYLOAD does not exist: ${HORIZON2MVII_PAYLOAD}")
    endif()
    list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${HORIZON2MVII_PAYLOAD}" "${_STAGE}/application.nsp")
    list(APPEND _DEPENDS "${HORIZON2MVII_PAYLOAD}")
endif()
list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E touch "${_STAMP}")
add_custom_command(OUTPUT "${_STAMP}" ${_COMMANDS} DEPENDS ${_DEPENDS} VERBATIM)
add_custom_target(horizon2mvii ALL DEPENDS "${_STAMP}")
