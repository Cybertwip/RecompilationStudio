include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/GoTooling.cmake")

function(virtua_add_armv7_app target)
    set(options GUI COOPERATIVE)
    set(oneValueArgs OUTPUT_NAME)
    set(multiValueArgs SOURCES INCLUDE_DIRECTORIES)
    cmake_parse_arguments(VAPP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT VAPP_SOURCES)
        message(FATAL_ERROR "virtua_add_armv7_app(${target}) requires SOURCES")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
        message(FATAL_ERROR "virtua_add_armv7_app requires the Virtua ARM toolchain")
    endif()
    if(NOT VAPP_OUTPUT_NAME)
        set(VAPP_OUTPUT_NAME "${target}")
    endif()

    get_filename_component(_virtua_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    set(_elf_target "${target}_elf")
    add_executable(${_elf_target}
        ${VAPP_SOURCES}
        "${_virtua_root}/native/minimal_arm_runtime.c"
        "${_virtua_root}/Dash/armv7/crt.s")
    target_include_directories(${_elf_target} PRIVATE
        ${VAPP_INCLUDE_DIRECTORIES}
        "${_virtua_root}/Dash"
        "${_virtua_root}/include")
    target_compile_options(${_elf_target} PRIVATE
        "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
        -mcpu=cortex-a7
        -marm
        -mfloat-abi=soft
        -mno-unaligned-access
        -ffreestanding
        -fpie
        -fno-stack-protector
        -fno-common
        -ffunction-sections
        -fdata-sections
        -Os)
    target_link_options(${_elf_target} PRIVATE
        "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
        -mcpu=cortex-a7
        -marm
        -mfloat-abi=soft
        -fuse-ld=lld
        -nostdlib
        -nostartfiles
        -static
        -Wl,--emit-relocs
        -Wl,--gc-sections
        -Wl,--build-id=none
        "-Wl,-T,${_virtua_root}/CMake/armv7_linker.ld")
    set_target_properties(${_elf_target} PROPERTIES
        OUTPUT_NAME "${VAPP_OUTPUT_NAME}"
        SUFFIX ".elf"
        LINK_DEPENDS "${_virtua_root}/CMake/armv7_linker.ld")

    virtua_prepare_go_tool(_virtua_tool "${_virtua_root}"
        "${_virtua_root}/binary" "virtua")
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/${VAPP_OUTPUT_NAME}.virtua")
    set(_app_mode console)
    if(VAPP_GUI)
        set(_app_mode gui)
    endif()
    set(_scheduler legacy)
    if(VAPP_COOPERATIVE)
        set(_scheduler cooperative)
    endif()
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND "${_virtua_tool}"
            -kind exec
            -app-mode "${_app_mode}"
            -scheduler "${_scheduler}"
            "$<TARGET_FILE:${_elf_target}>"
            "${_output}"
        DEPENDS ${_elf_target} "${_virtua_tool}"
        COMMENT "Packaging ${VAPP_OUTPUT_NAME}.virtua"
        VERBATIM)
    add_custom_target(${target} ALL DEPENDS "${_output}")
    set(${target}_VIRTUA_FILE "${_output}" PARENT_SCOPE)
    set(${target}_ELF_FILE "$<TARGET_FILE:${_elf_target}>" PARENT_SCOPE)
endfunction()
