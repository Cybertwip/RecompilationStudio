cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED VIRTUA_ROOT OR VIRTUA_ROOT STREQUAL "")
    get_filename_component(VIRTUA_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_dash_sources
    "${CMAKE_CURRENT_LIST_DIR}/libDash.c"
    "${CMAKE_CURRENT_LIST_DIR}/stub.c"
)

set(_dash_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(_dash_arch MATCHES "^(x86_64|amd64)$")
    list(APPEND _dash_sources
        "${CMAKE_CURRENT_LIST_DIR}/x86_64/crt.s"
        "${CMAKE_CURRENT_LIST_DIR}/x86_64/minos_user_abi.cpp")
elseif(_dash_arch MATCHES "^riscv64")
    list(APPEND _dash_sources
        "${CMAKE_CURRENT_LIST_DIR}/riscv64/crt.s"
        "${CMAKE_CURRENT_LIST_DIR}/usys.S"
    )
elseif(_dash_arch MATCHES "^(arm|armv7)")
    list(APPEND _dash_sources
        "${CMAKE_CURRENT_LIST_DIR}/armv7/crt.s"
        "${CMAKE_CURRENT_LIST_DIR}/x86_64/minos_user_abi.cpp")
else()
    message(FATAL_ERROR
        "External/Virtua/Dash: unsupported CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'.")
endif()

add_library(R-Dash ${_dash_sources})
target_include_directories(R-Dash PUBLIC "${VIRTUA_ROOT}" "${CMAKE_CURRENT_LIST_DIR}")

if(DEFINED DASH_FIXED_HEAP_BYTES)
    target_compile_definitions(R-Dash PUBLIC DASH_FIXED_HEAP_BYTES=${DASH_FIXED_HEAP_BYTES})
endif()

add_library(Virtua::Dash ALIAS R-Dash)
