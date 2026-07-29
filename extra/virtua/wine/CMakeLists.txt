# virtua-wine — the in-process guest-OS layer shared by the Vita and Horizon
# front-ends.
#
# ARMv7 only, and not by omission. This layer's entire premise is that the guest
# is native code: the loader maps ARM instructions and branches to them. On
# x86_64 there is nothing to branch to, so an x86_64 build of this library would
# be a library that cannot do the one thing it exists for. The front-ends are
# gated the same way -- see the guard in each of their CMakeLists.
#
# Nothing from PowerEngine is vendored here. The include paths point into
# POWERENGINE_ROOT, exactly as VirtuaPowerEngine.cmake resolves it for every
# other Virtua target in this repo.

include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
    message(FATAL_ERROR
        "virtua-wine is ARMv7-only: it executes guest ARM code natively. "
        "Configure with -DCMAKE_TOOLCHAIN_FILE=<psxrecomp>/extra/virtua/CMake/VirtuaArmToolchain.cmake")
endif()

get_filename_component(_vwine_virtua_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${_vwine_virtua_root}/CMake/VirtuaPowerEngine.cmake")

add_library(virtua-wine STATIC
    "${CMAKE_CURRENT_LIST_DIR}/source/vwine_mem.c"
    "${CMAKE_CURRENT_LIST_DIR}/source/vwine_elf.c"
    "${CMAKE_CURRENT_LIST_DIR}/source/vwine_registry.c"
    "${CMAKE_CURRENT_LIST_DIR}/source/vwine_log.c")

target_include_directories(virtua-wine PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}/Dash"
    "${VIRTUA_POWERENGINE_POSIX_INCLUDE}"
    "${VIRTUA_ARM_SYSROOT}/include")

target_compile_options(virtua-wine PRIVATE
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${VIRTUA_ARM_SYSROOT}"
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
    -Wall -Wextra
    -Os)

set_target_properties(virtua-wine PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
