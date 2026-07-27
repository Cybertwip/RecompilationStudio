# gba-to-mvii — a real GBA runtime for MVII, not a wrapper.
#
# This builds the extracted ARM7TDMI interpreter and GBA hardware model against
# MVII's Virtua userland and emits a packaged `.virtua` executable.
#
# Configure it with the Virtua ARM toolchain — there is no host build, because
# there is no host target here:
#
#   cmake -S extra/gba-to-mvii -B build/gba-to-mvii \
#         -DCMAKE_TOOLCHAIN_FILE=extra/virtua/CMake/VirtuaArmToolchain.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/gba-to-mvii --target gba-to-mvii
#
# Or pull it into a generated package project with add_subdirectory(), in which
# case GBA_TO_MVII_VIRTUA_FILE and GBA_TO_MVII_ELF_FILE come back in the parent
# scope and `gba-to-mvii` is the target to depend on.

cmake_minimum_required(VERSION 3.20)
project(GbaToMvii C CXX ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # An ARM7TDMI interpreter on an 845 MHz Cortex-A7 has no headroom to spare;
    # an accidental -O0 build is not a slower runtime, it is an unusable one.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
    message(FATAL_ERROR
        "gba-to-mvii targets MVII/Virtua only. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=<psxrecomp>/extra/virtua/CMake/VirtuaArmToolchain.cmake")
endif()

# ── locations ──────────────────────────────────────────────────────────────

if(NOT DEFINED VIRTUA_ROOT OR VIRTUA_ROOT STREQUAL "")
    get_filename_component(VIRTUA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../virtua" ABSOLUTE)
endif()
if(NOT EXISTS "${VIRTUA_ROOT}/Dash/CMakeLists.txt")
    message(FATAL_ERROR "VIRTUA_ROOT='${VIRTUA_ROOT}' does not contain Dash/CMakeLists.txt")
endif()

set(GBA_MVII_SDK "${VIRTUA_ROOT}/sdk/arm-eabi")
set(GBA_MVII_SRC "${CMAKE_CURRENT_LIST_DIR}/src")

set(GBA_MVII_OUTPUT_NAME "gba-runtime" CACHE STRING
    "Base name of the emitted .virtua executable")
set(GBA_MVII_ROM "" CACHE FILEPATH
    "Optional ROM to stage next to the runtime as game.gba")
set(GBA_MVII_STAGE_DIR "" CACHE PATH
    "Optional directory to stage the runtime and ROM into")

include("${VIRTUA_ROOT}/CMake/GoTooling.cmake")

# ── sources ────────────────────────────────────────────────────────────────
#
# Listed one by one rather than globbed. This tree exists precisely to answer
# "what does a GBA runtime actually need", and a glob would stop answering it
# the moment someone drops a file in.

set(_armv4t_sources
    "${GBA_MVII_SRC}/armv4t/arm_decode.cpp"
    "${GBA_MVII_SRC}/armv4t/arm_ir.cpp"
    "${GBA_MVII_SRC}/armv4t/interpreter.cpp"
    "${GBA_MVII_SRC}/armv4t/thumb_decode.cpp")

set(_gba_sources
    "${GBA_MVII_SRC}/gba/audio_shadow.cpp"
    "${GBA_MVII_SRC}/gba/crc32.cpp"
    "${GBA_MVII_SRC}/gba/gba_audio.cpp"
    "${GBA_MVII_SRC}/gba/gba_bios.cpp"
    "${GBA_MVII_SRC}/gba/gba_bus.cpp"
    "${GBA_MVII_SRC}/gba/gba_dma.cpp"
    "${GBA_MVII_SRC}/gba/gba_input.cpp"
    "${GBA_MVII_SRC}/gba/gba_io.cpp"
    "${GBA_MVII_SRC}/gba/gba_irq.cpp"
    "${GBA_MVII_SRC}/gba/gba_m4a.cpp"
    "${GBA_MVII_SRC}/gba/gba_memory.cpp"
    "${GBA_MVII_SRC}/gba/gba_ppu.cpp"
    "${GBA_MVII_SRC}/gba/gba_rom_header.cpp"
    "${GBA_MVII_SRC}/gba/gba_rtc.cpp"
    "${GBA_MVII_SRC}/gba/gba_save.cpp"
    "${GBA_MVII_SRC}/gba/gba_scheduler.cpp"
    "${GBA_MVII_SRC}/gba/gba_timers.cpp"
    "${GBA_MVII_SRC}/gba/mp2k_shadow.cpp"
    "${GBA_MVII_SRC}/gba/sha1.cpp")

set(_mvii_sources
    "${GBA_MVII_SRC}/mvii/bios_hle.cpp"
    "${GBA_MVII_SRC}/mvii/main.cpp"
    "${GBA_MVII_SRC}/mvii/mvii_platform.cpp"
    # Not part of R-Dash's own source list, but libc++/llvm-libc in the sysroot
    # is linked against it: errno, the stdio cookies, clock_gettime, usleep,
    # sched_yield, getenv. Every Virtua C++ app has to compile this in.
    "${VIRTUA_ROOT}/Dash/llvm_libc_stubs.cpp")

# ── R-Dash ─────────────────────────────────────────────────────────────────
#
# Virtua's C runtime: _start, the MVII syscall stubs, malloc. Whole-archived
# into the ELF below, so it has to be configured with the same ABI flags or the
# link silently mixes float ABIs.

if(NOT TARGET R-Dash)
    set(DASH_FIXED_HEAP_BYTES 0)
    add_subdirectory("${VIRTUA_ROOT}/Dash" "${CMAKE_BINARY_DIR}/virtua-dash" EXCLUDE_FROM_ALL)
endif()

target_include_directories(R-Dash BEFORE PUBLIC
    "${VIRTUA_ROOT}/posix-shim/include"
    "${GBA_MVII_SDK}/include"
    "${GBA_MVII_SDK}/include/c++/v1")
target_compile_definitions(R-Dash PUBLIC
    MINOS_PLATFORM_MACHINE64=1
    MINOS_PLATFORM_ARMV7=1
    MINOS_VIRTUA_USERLAND=1
    _LIBCPP_HAS_NO_THREADS
    _LIBCPP_DISABLE_AVAILABILITY
    _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE)
target_compile_options(R-Dash PRIVATE
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${GBA_MVII_SDK}"
    -mcpu=cortex-a7
    -marm
    -mfloat-abi=soft
    -mno-unaligned-access
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
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${GBA_MVII_SDK}/include/c++/v1>)

# ── the runtime ELF ────────────────────────────────────────────────────────

add_executable(gba-to-mvii-elf
    ${_armv4t_sources}
    ${_gba_sources}
    ${_mvii_sources})

# BEFORE, so Virtua's shims win over anything the extracted sources may find.
target_include_directories(gba-to-mvii-elf BEFORE PRIVATE
    "${VIRTUA_ROOT}/posix-shim/include"
    "${VIRTUA_ROOT}/include"
    "${VIRTUA_ROOT}/Dash"
    "${GBA_MVII_SDK}/include"
    "${GBA_MVII_SDK}/include/c++/v1")
target_include_directories(gba-to-mvii-elf PRIVATE
    "${GBA_MVII_SRC}"
    "${GBA_MVII_SRC}/armv4t"
    "${GBA_MVII_SRC}/gba"
    "${GBA_MVII_SRC}/debug"
    "${GBA_MVII_SRC}/mvii")

target_compile_features(gba-to-mvii-elf PRIVATE cxx_std_20)

target_compile_definitions(gba-to-mvii-elf PRIVATE
    MINOS_PLATFORM_MACHINE64=1
    MINOS_PLATFORM_ARMV7=1
    MINOS_VIRTUA_USERLAND=1
    GBA_MVII_RUNTIME=1
    _LIBCPP_HAS_NO_THREADS
    _LIBCPP_DISABLE_AVAILABILITY
    _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE)

target_compile_options(gba-to-mvii-elf PRIVATE
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${GBA_MVII_SDK}"
    -mcpu=cortex-a7
    -marm
    -mfloat-abi=soft
    -mno-unaligned-access
    -femulated-tls
    -ffreestanding
    -fpie
    -fno-stack-protector
    -fno-common
    # No NEON reaches a Virtua guest, and the auto-vectorizer will happily emit
    # it for the PPU's per-pixel loops if allowed to.
    -fno-vectorize
    -fno-slp-vectorize
    -ffunction-sections
    -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${GBA_MVII_SDK}/include/c++/v1>
    $<$<COMPILE_LANGUAGE:CXX>:SHELL:-include virtua_cpp_thread_shim.h>)

add_dependencies(gba-to-mvii-elf R-Dash)

target_link_directories(gba-to-mvii-elf PRIVATE
    "${GBA_MVII_SDK}/lib"
    "${GBA_MVII_SDK}/lib/armv7a-none-eabi")

target_link_options(gba-to-mvii-elf PRIVATE
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${GBA_MVII_SDK}"
    -mcpu=cortex-a7
    -marm
    -mfloat-abi=soft
    -femulated-tls
    -fuse-ld=lld
    -nostdlib
    -nostartfiles
    -static
    # --emit-relocs is not optional: the virtua packager rewrites relocations
    # when it lays the image out, and strips them itself afterwards.
    -Wl,--emit-relocs
    -Wl,--gc-sections
    -Wl,--build-id=none
    "-Wl,-T,${VIRTUA_ROOT}/CMake/armv7_linker.ld"
    # Whole-archive, because nothing in the runtime references _start and
    # --gc-sections would otherwise drop the entry point on the floor.
    -Wl,--whole-archive
    $<TARGET_FILE:R-Dash>
    -Wl,--no-whole-archive)

target_link_libraries(gba-to-mvii-elf PRIVATE
    c++ c++abi unwind c m
    "${GBA_MVII_SDK}/lib/clang-runtime/libclang_rt.builtins-armv7.a")

set_target_properties(gba-to-mvii-elf PROPERTIES
    OUTPUT_NAME "${GBA_MVII_OUTPUT_NAME}"
    SUFFIX ".elf"
    LINK_DEPENDS "${VIRTUA_ROOT}/CMake/armv7_linker.ld")

# ── packaging ──────────────────────────────────────────────────────────────

virtua_prepare_go_tool(GBA_MVII_VIRTUA_TOOL
    "${VIRTUA_ROOT}" "${VIRTUA_ROOT}/binary" "virtua")
add_custom_target(gba-to-mvii-tool DEPENDS "${GBA_MVII_VIRTUA_TOOL}")

set(_gba_virtua "${CMAKE_CURRENT_BINARY_DIR}/${GBA_MVII_OUTPUT_NAME}.virtua")
add_custom_command(
    OUTPUT "${_gba_virtua}"
    # gui, because it presents to /dev/fb0 and the shell must give it a window;
    # cooperative, because the run loop yields to MVII's 120 Hz round rather
    # than being preempted mid-frame.
    COMMAND "${GBA_MVII_VIRTUA_TOOL}"
        -kind exec
        -app-mode gui
        -scheduler cooperative
        "$<TARGET_FILE:gba-to-mvii-elf>"
        "${_gba_virtua}"
    DEPENDS gba-to-mvii-elf gba-to-mvii-tool "${GBA_MVII_VIRTUA_TOOL}"
    COMMENT "Packaging ${GBA_MVII_OUTPUT_NAME}.virtua"
    VERBATIM)

add_custom_target(gba-to-mvii ALL DEPENDS "${_gba_virtua}")
add_dependencies(gba-to-mvii gba-to-mvii-elf gba-to-mvii-tool)

# ── optional staging ───────────────────────────────────────────────────────
#
# main.cpp resolves the ROM as dirname(argv[0])/game.gba, so a staged package is
# just the two files side by side.

if(GBA_MVII_STAGE_DIR)
    set(_gba_stage_stamp "${CMAKE_CURRENT_BINARY_DIR}/gba-to-mvii-stage.stamp")
    set(_gba_stage_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GBA_MVII_STAGE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_gba_virtua}" "${GBA_MVII_STAGE_DIR}/${GBA_MVII_OUTPUT_NAME}.virtua")
    set(_gba_stage_depends "${_gba_virtua}")
    if(GBA_MVII_ROM)
        list(APPEND _gba_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GBA_MVII_ROM}" "${GBA_MVII_STAGE_DIR}/game.gba")
        list(APPEND _gba_stage_depends "${GBA_MVII_ROM}")
    endif()
    add_custom_command(
        OUTPUT "${_gba_stage_stamp}"
        ${_gba_stage_commands}
        COMMAND ${CMAKE_COMMAND} -E touch "${_gba_stage_stamp}"
        DEPENDS ${_gba_stage_depends}
        COMMENT "Staging gba-to-mvii into ${GBA_MVII_STAGE_DIR}"
        VERBATIM)
    add_custom_target(gba-to-mvii-stage ALL DEPENDS "${_gba_stage_stamp}")
    add_dependencies(gba-to-mvii-stage gba-to-mvii)
endif()

# ── outputs for an enclosing project ───────────────────────────────────────

if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(GBA_TO_MVII_VIRTUA_FILE "${_gba_virtua}" PARENT_SCOPE)
    set(GBA_TO_MVII_ELF_FILE "$<TARGET_FILE:gba-to-mvii-elf>" PARENT_SCOPE)
endif()
