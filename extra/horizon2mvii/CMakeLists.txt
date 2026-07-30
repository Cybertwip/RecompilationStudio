# horizon2mvii — run 32-bit Switch software on MVII by executing it, not
# emulating it.
#
# The premise is the same one vita2mvii rests on: AArch32 Horizon is ARMv7-A and
# the J36 is an ARMv7-A Cortex-A7, so the guest's own instructions are the
# host's instructions. This target builds a loader for Horizon's containers and
# a reimplementation of Horizon's kernel and IPC, in the shape WINE reimplements
# Win32 on Linux. There is no CPU emulation and no static recompilation in it.
#
# ARMv7 only, and the guard below is load-bearing rather than defensive. An
# x86_64 build would compile every line here and then have nothing to branch to
# -- worse than not building, because it looks like it works.
#
# Nothing is vendored from PowerEngine. Dash, the POSIX shim, the linker script,
# the sysroot and the packager all come out of POWERENGINE_ROOT, resolved by
# extra/virtua/CMake/VirtuaPowerEngine.cmake exactly as every other Virtua
# target in this repo resolves them.
#
#   cmake -S extra/horizon2mvii -B build/horizon2mvii \
#         -DCMAKE_TOOLCHAIN_FILE=extra/virtua/CMake/VirtuaArmToolchain.cmake \
#         -DPOWERENGINE_ROOT=/path/to/PowerEngine \
#         -DVIRTUA_LLVM_ROOT=/path/to/PowerEngine/.../bundles/compiler \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/horizon2mvii --target horizon2mvii

cmake_minimum_required(VERSION 3.20)
project(horizon2mvii VERSION 0.5.0 LANGUAGES C CXX ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
    message(FATAL_ERROR
        "horizon2mvii is ARMv7-only: it loads Horizon ARM code and branches to "
        "it, so there is no meaningful x86_64 build. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=<psxrecomp>/extra/virtua/CMake/VirtuaArmToolchain.cmake")
endif()

get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(HORIZON2MVII_VIRTUA "${PSXRECOMP_ROOT}/extra/virtua")

include("${HORIZON2MVII_VIRTUA}/CMake/VirtuaPowerEngine.cmake")
include("${VIRTUA_POWERENGINE_VIRTUA_ROOT}/CMake/GoTooling.cmake")

set(HORIZON2MVII_SDK "${VIRTUA_ARM_SYSROOT}")
set(HORIZON2MVII_HERE "${CMAKE_CURRENT_LIST_DIR}")

if(NOT DEFINED HORIZON2MVII_OUTPUT_NAME OR HORIZON2MVII_OUTPUT_NAME STREQUAL "")
    set(HORIZON2MVII_OUTPUT_NAME "horizon2mvii")
endif()

# ── the shared guest-OS layer ──────────────────────────────────────────────
#
# The mapper, the ELF relocator and the missing-import registry are not
# Horizon-specific; vita2mvii stands on the same library. An NRO is not an ELF
# file, but its MOD0 header points at an ordinary .dynamic, so everything from
# that point on is shared rather than written twice.

if(NOT TARGET virtua-wine)
    include("${HORIZON2MVII_VIRTUA}/wine/CMakeLists.txt")
endif()

# ── sources ────────────────────────────────────────────────────────────────
#
# Listed rather than globbed. Four files, one layer each:
#
#   horizon_image.c   NRO0/NSO0 -> a mapped, relocated ARM32 image (incl. LZ4)
#   horizon_kernel.c  the SVCs, on MVII threads, memory and clock
#   horizon_svc.c     the `svc` trap handler and register marshalling
#   horizon_ipc.c     HIPC/CMIF and sm:
#
# Two files that were here are gone rather than ported: main.c drove
# /dev/native0 through MVII_NATIVE_IOCTL_LAUNCH and horizon_servctl_mvii.c drove
# /dev/horizon0 through ioctl 0x485A0100. Neither device nor either ioctl exists
# anywhere in PowerEngine -- they were transports to a kernel-side implementation
# nobody wrote -- so there was no working behaviour to preserve.

set(_horizon2mvii_sources
    "${HORIZON2MVII_HERE}/source/horizon_main.c"
    "${HORIZON2MVII_HERE}/source/horizon_image.c"
    "${HORIZON2MVII_HERE}/source/horizon_kernel.c"
    "${HORIZON2MVII_HERE}/source/horizon_svc.c"
    "${HORIZON2MVII_HERE}/source/horizon_ipc.c"
    # libc++/llvm-libc in the sysroot is linked against these: errno, the stdio
    # cookies, clock_gettime, sched_yield. Every Virtua app compiles it in.
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}/Dash/llvm_libc_stubs.cpp")

# ── R-Dash ─────────────────────────────────────────────────────────────────
#
# Virtua's C runtime: _start, minos_current_user_abi, the MVII syscall stubs,
# malloc. Whole-archived into the ELF below, so it has to carry the same ABI
# flags or the link silently mixes float conventions.

if(NOT TARGET R-Dash)
    set(DASH_FIXED_HEAP_BYTES 0)
    add_subdirectory("${VIRTUA_POWERENGINE_VIRTUA_ROOT}/Dash"
                     "${CMAKE_BINARY_DIR}/virtua-dash" EXCLUDE_FROM_ALL)
endif()

target_include_directories(R-Dash BEFORE PUBLIC
    "${VIRTUA_POWERENGINE_POSIX_INCLUDE}"
    "${HORIZON2MVII_SDK}/include"
    "${HORIZON2MVII_SDK}/include/c++/v1")
target_compile_definitions(R-Dash PUBLIC
    MINOS_PLATFORM_MACHINE64=1
    MINOS_PLATFORM_ARMV7=1
    MINOS_VIRTUA_USERLAND=1
    _LIBCPP_HAS_NO_THREADS
    _LIBCPP_DISABLE_AVAILABILITY
    _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE)
target_compile_options(R-Dash PRIVATE
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${HORIZON2MVII_SDK}"
    -mcpu=cortex-a7
    -marm
    -mfloat-abi=soft
    -munaligned-access
    -femulated-tls
    -ffreestanding
    -fpie
    -fno-stack-protector
    -fno-common
    -ffunction-sections
    -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${HORIZON2MVII_SDK}/include/c++/v1>)

# ── the front-end ELF ──────────────────────────────────────────────────────

add_executable(horizon2mvii-elf ${_horizon2mvii_sources})

# BEFORE, so Virtua's POSIX shim wins over anything the sysroot would otherwise
# supply for the same header names.
target_include_directories(horizon2mvii-elf BEFORE PRIVATE
    "${VIRTUA_POWERENGINE_POSIX_INCLUDE}"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}/Dash"
    "${HORIZON2MVII_SDK}/include"
    "${HORIZON2MVII_SDK}/include/c++/v1")
target_include_directories(horizon2mvii-elf PRIVATE
    "${HORIZON2MVII_HERE}/include")

target_compile_definitions(horizon2mvii-elf PRIVATE
    MINOS_PLATFORM_MACHINE64=1
    MINOS_PLATFORM_ARMV7=1
    MINOS_VIRTUA_USERLAND=1
    _LIBCPP_HAS_NO_THREADS
    _LIBCPP_DISABLE_AVAILABILITY
    _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE)

target_compile_options(horizon2mvii-elf PRIVATE
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${HORIZON2MVII_SDK}"
    -mcpu=cortex-a7
    # -marm, and this one matters more here than anywhere else in the repo: the
    # kernel's context save/restore and the TPIDRURO accessor are naked ARM
    # assembly, and Thumb would change the encoding under them.
    -marm
    # Soft-float, matching MVII's libc. The guest is hard-float; the boundary is
    # handled per-function with pcs("aapcs-vfp") (VWINE_GUEST_ABI) rather than by
    # building this side hard-float, which would set Tag_ABI_VFP_args on every
    # object and stop them linking against the sysroot at all.
    -mfloat-abi=soft
    -munaligned-access
    -femulated-tls
    -ffreestanding
    -fpie
    -fno-stack-protector
    -fno-common
    # No NEON reaches a Virtua guest, and the vectorizer will emit it given the
    # chance.
    -fno-vectorize
    -fno-slp-vectorize
    -ffunction-sections
    -fdata-sections
    -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${HORIZON2MVII_SDK}/include/c++/v1>)

set_target_properties(horizon2mvii-elf PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "${HORIZON2MVII_OUTPUT_NAME}"
    SUFFIX ".elf"
    LINK_DEPENDS "${VIRTUA_ARM_LINKER_SCRIPT}")

add_dependencies(horizon2mvii-elf R-Dash)

target_link_directories(horizon2mvii-elf PRIVATE
    "${HORIZON2MVII_SDK}/lib"
    "${HORIZON2MVII_SDK}/lib/${VIRTUA_ARM_TARGET_TRIPLE}")

target_link_options(horizon2mvii-elf PRIVATE
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${HORIZON2MVII_SDK}"
    -mcpu=cortex-a7
    -marm
    -mfloat-abi=soft
    -femulated-tls
    -fuse-ld=lld
    -nostdlib
    -nostartfiles
    -static
    # The virtua packager rewrites relocations as it lays the image out and
    # strips them itself afterwards, so they have to survive the link.
    -Wl,--emit-relocs
    -Wl,--gc-sections
    -Wl,-z,nostart-stop-gc
    -Wl,--build-id=none
    "-Wl,-T,${VIRTUA_ARM_LINKER_SCRIPT}"
    # Whole-archive: nothing references _start, and --gc-sections would drop the
    # entry point on the floor.
    -Wl,--whole-archive
    $<TARGET_FILE:R-Dash>
    -Wl,--no-whole-archive)

target_link_libraries(horizon2mvii-elf PRIVATE
    virtua-wine
    c++ c++abi unwind c m
    "${VIRTUA_ARM_COMPILER_RT}")

# ── packaging ──────────────────────────────────────────────────────────────

virtua_prepare_go_tool(HORIZON2MVII_VIRTUA_TOOL
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}/binary" "virtua")
add_custom_target(horizon2mvii-tool DEPENDS "${HORIZON2MVII_VIRTUA_TOOL}")

set(_horizon2mvii_virtua
    "${CMAKE_CURRENT_BINARY_DIR}/${HORIZON2MVII_OUTPUT_NAME}.virtua")
add_custom_command(
    OUTPUT "${_horizon2mvii_virtua}"
    # gui, because a Horizon guest presents through vi/nvnflinger and the shell
    # has to give it a window; cooperative, because every wait in the kernel is a
    # yield and MVII's scheduler is what turns those into progress. Note 1 in
    # horizon_kernel.h is the long version of why that is not an accident.
    COMMAND "${HORIZON2MVII_VIRTUA_TOOL}"
        -kind exec
        -app-mode gui
        -scheduler cooperative
        "$<TARGET_FILE:horizon2mvii-elf>"
        "${_horizon2mvii_virtua}"
    DEPENDS horizon2mvii-elf horizon2mvii-tool "${HORIZON2MVII_VIRTUA_TOOL}"
    COMMENT "Packaging ${HORIZON2MVII_OUTPUT_NAME}.virtua"
    VERBATIM)

add_custom_target(horizon2mvii ALL DEPENDS "${_horizon2mvii_virtua}")
add_dependencies(horizon2mvii horizon2mvii-elf horizon2mvii-tool)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(HORIZON2MVII_VIRTUA_FILE "${_horizon2mvii_virtua}" PARENT_SCOPE)
    set(HORIZON2MVII_ELF_FILE "$<TARGET_FILE:horizon2mvii-elf>" PARENT_SCOPE)
endif()

# ── optional staging ───────────────────────────────────────────────────────
#
# horizon_main.c takes the image path as argv[1]; a staged package is the
# runtime and the guest side by side. The payload is an NRO or NSO -- not an NSP
# or an NCA, which are encrypted archives this front-end does not open, and the
# old cache variable's "application.nsp" name promised otherwise.
#
# The staged image is named after HORIZON2MVII_OUTPUT_NAME rather than
# "horizon2mvii", because a package built by PSXRecomp Studio is named for its
# game and the runtime's own name never appears in it. HORIZON2MVII_STAGE_DIR
# lets the caller choose where that pair lands, the same knob gba-to-mvii exposes
# as GBA_MVII_STAGE_DIR.

set(HORIZON2MVII_PAYLOAD "" CACHE FILEPATH
    "Optional Horizon NRO/NSO staged beside the runtime as 'executable'")
if(NOT DEFINED HORIZON2MVII_STAGE_DIR)
    set(HORIZON2MVII_STAGE_DIR "")
endif()
if(HORIZON2MVII_PAYLOAD)
    if(NOT EXISTS "${HORIZON2MVII_PAYLOAD}")
        message(FATAL_ERROR "HORIZON2MVII_PAYLOAD does not exist: ${HORIZON2MVII_PAYLOAD}")
    endif()
    if(HORIZON2MVII_STAGE_DIR)
        set(_horizon2mvii_stage "${HORIZON2MVII_STAGE_DIR}")
    else()
        set(_horizon2mvii_stage
            "${CMAKE_BINARY_DIR}/steganos-package/horizon2mvii/$<CONFIG>")
    endif()
    set(_horizon2mvii_stamp "${CMAKE_CURRENT_BINARY_DIR}/horizon2mvii-stage.stamp")
    add_custom_command(
        OUTPUT "${_horizon2mvii_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_horizon2mvii_stage}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_horizon2mvii_virtua}"
                "${_horizon2mvii_stage}/${HORIZON2MVII_OUTPUT_NAME}.virtua"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HORIZON2MVII_PAYLOAD}" "${_horizon2mvii_stage}/executable"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HORIZON2MVII_HERE}/README.md" "${_horizon2mvii_stage}/README.md"
        COMMAND ${CMAKE_COMMAND} -E touch "${_horizon2mvii_stamp}"
        DEPENDS "${_horizon2mvii_virtua}" "${HORIZON2MVII_PAYLOAD}"
                "${HORIZON2MVII_HERE}/README.md"
        COMMENT "Staging horizon2mvii into ${_horizon2mvii_stage}"
        VERBATIM)
    add_custom_target(horizon2mvii-stage ALL DEPENDS "${_horizon2mvii_stamp}")
    add_dependencies(horizon2mvii-stage horizon2mvii)
endif()
