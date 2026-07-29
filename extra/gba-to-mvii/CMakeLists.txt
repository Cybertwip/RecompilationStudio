# gba-to-mvii — a real GBA runtime for MVII, not a wrapper.
#
# Two halves, and the split is the whole design:
#
#   extra/gbarecomp   the emulation core: a static ARM7TDMI-to-C++ recompiler
#                     and the runtime its output calls into. A curated subset of
#                     its sources is compiled for Cortex-A7 below, into the
#                     static library `gbarecomp-mvii`.
#   src/              MVII, and nothing else: /dev/fb0, /dev/input0, /dev/dac0,
#                     the cooperative yield, the ROM and save files. Plus
#                     src/core/, which implements include/gba_mvii.h on top of
#                     gbarecomp.
#
# They meet at include/gba_mvii.h, a C ABI narrow enough that the MVII side never
# names a gbarecomp type and gbarecomp never makes a syscall. That keeps the
# emulation identical to the reference implementation, which is the only way to
# tell an emulator bug from a port bug.
#
# Configure it with the Virtua ARM toolchain — there is no host build, because
# there is no host target here (gbarecomp itself has one; use that tree for
# desktop work):
#
#   cmake -S extra/gba-to-mvii -B build/gba-to-mvii \
#         -DCMAKE_TOOLCHAIN_FILE=extra/virtua/CMake/VirtuaArmToolchain.cmake \
#         -DPOWERENGINE_ROOT=/path/to/PowerEngine \
#         -DVIRTUA_LLVM_ROOT=/path/to/PowerEngine/compiler-bundle \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/gba-to-mvii --target gba-to-mvii
#
# Nothing is vendored: PowerEngine's Dash, POSIX shim, linker script, sysroot and
# packager all come out of POWERENGINE_ROOT, resolved by
# extra/virtua/CMake/VirtuaPowerEngine.cmake exactly as every other Virtua target
# in this repo resolves them. gbarecomp is used from its own directory.
#
# Or pull it into a generated package project with add_subdirectory(), in which
# case GBA_TO_MVII_VIRTUA_FILE and GBA_TO_MVII_ELF_FILE come back in the parent
# scope and `gba-to-mvii` is the target to depend on. That is what Studio's GBA
# pipeline does, handing GBA_MVII_NATIVE_SOURCES the recompiled cartridge.

cmake_minimum_required(VERSION 3.20)
project(GbaToMvii C CXX ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # An 845 MHz Cortex-A7 has no headroom to spare, and the fallback path when
    # the static corpus misses is an ARM7TDMI interpreter; an accidental -O0
    # build is not a slower runtime, it is an unusable one.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
    message(FATAL_ERROR
        "gba-to-mvii targets MVII/Virtua only. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=<psxrecomp>/extra/virtua/CMake/VirtuaArmToolchain.cmake")
endif()

# ── locations ──────────────────────────────────────────────────────────────

include("${CMAKE_CURRENT_LIST_DIR}/../virtua/CMake/VirtuaPowerEngine.cmake")
set(VIRTUA_ROOT "${VIRTUA_POWERENGINE_VIRTUA_ROOT}")
set(GBA_MVII_SDK "${VIRTUA_ARM_SYSROOT}")
set(GBA_MVII_SRC "${CMAKE_CURRENT_LIST_DIR}/src")
set(GBA_MVII_INC "${CMAKE_CURRENT_LIST_DIR}/include")

# The emulation core. A sibling directory by default; overridable so a generated
# package project can point at the copy it snapshotted alongside the recompiled
# cartridge (Studio's pipeline does exactly that, so a package rebuilds years
# later against the recompiler it was generated with).
if(NOT DEFINED GBARECOMP_ROOT OR GBARECOMP_ROOT STREQUAL "")
    get_filename_component(GBARECOMP_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../gbarecomp" ABSOLUTE)
endif()
if(NOT EXISTS "${GBARECOMP_ROOT}/src/armv4t/runtime_arm.cpp")
    message(FATAL_ERROR
        "GBARECOMP_ROOT does not look like the gbarecomp tree: ${GBARECOMP_ROOT}\n"
        "It is the emulation core; point -DGBARECOMP_ROOT at a checkout of it.")
endif()

# Plain variables, not cache entries: -D on the command line still defines them,
# and an enclosing project can set() them before add_subdirectory() without
# having to FORCE its way past a cached default.
#
#   GBA_MVII_OUTPUT_NAME    base name of the emitted .virtua executable
#   GBA_MVII_ROM            optional ROM to stage beside it as game.gba
#   GBA_MVII_BIOS           optional 16 KB BIOS image staged as gba_bios.bin
#   GBA_MVII_STAGE_DIR      optional directory to stage them into
#   GBA_MVII_NATIVE_SOURCES the recompiled cartridge: recompiled_*.cpp plus the
#                           dispatch_table.cpp that indexes them
if(NOT DEFINED GBA_MVII_OUTPUT_NAME OR GBA_MVII_OUTPUT_NAME STREQUAL "")
    set(GBA_MVII_OUTPUT_NAME "gba-runtime")
endif()
if(NOT DEFINED GBA_MVII_ROM)
    set(GBA_MVII_ROM "")
endif()
if(NOT DEFINED GBA_MVII_BIOS)
    set(GBA_MVII_BIOS "")
endif()
if(NOT DEFINED GBA_MVII_STAGE_DIR)
    set(GBA_MVII_STAGE_DIR "")
endif()
if(NOT DEFINED GBA_MVII_NATIVE_SOURCES)
    set(GBA_MVII_NATIVE_SOURCES "")
endif()
if(GBA_MVII_NATIVE_SOURCES)
    set(GBA_MVII_NATIVE_AOT ON)
else()
    set(GBA_MVII_NATIVE_AOT OFF)
endif()

include("${VIRTUA_POWERENGINE_VIRTUA_ROOT}/CMake/GoTooling.cmake")

# ── the ABI every ARM object here is built with ────────────────────────────
#
# Collected once because R-Dash, the core library and the ELF must agree on all
# of it. A mismatch in the float ABI or the TLS model does not fail the compile;
# it fails at link, or worse, at run.

set(_gba_arm_defs
    MINOS_PLATFORM_MACHINE64=1
    MINOS_PLATFORM_ARMV7=1
    MINOS_VIRTUA_USERLAND=1
    _LIBCPP_HAS_NO_THREADS
    _LIBCPP_DISABLE_AVAILABILITY
    _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE)

set(_gba_arm_flags
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${GBA_MVII_SDK}"
    -mcpu=cortex-a7
    -marm
    -mfloat-abi=soft
    # Not -mno-unaligned-access: that costs 4 ldrb + 3 orr per 32-bit load where
    # the core takes one ldr, and the core's whole job is 32-bit guest memory
    # accesses. SCTLR.A is 0 and the guest lives in Normal-WBWA DRAM, so the
    # hardware handles the unaligned case.
    -munaligned-access
    -femulated-tls
    -ffreestanding
    -fpie
    -fno-stack-protector
    -fno-common
    -ffunction-sections
    -fdata-sections)

set(_gba_arm_cxx_flags
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${GBA_MVII_SDK}/include/c++/v1>
    # The POSIX shim's std::mutex / std::thread / std::this_thread, force-included
    # so gbarecomp's sources get them without being modified: several of them
    # include <mutex> or <atomic> and expect the real names to be there.
    $<$<COMPILE_LANGUAGE:CXX>:SHELL:-include virtua_cpp_thread_shim.h>)

set(_gba_arm_includes
    # BEFORE everything: Virtua's POSIX shim wins over anything the sources
    # would otherwise find.
    "${VIRTUA_POWERENGINE_POSIX_INCLUDE}"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}"
    # Dash's <thread>, which the shim above defers to for std::thread itself.
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}/Dash"
    "${GBA_MVII_SDK}/include"
    "${GBA_MVII_SDK}/include/c++/v1")

# ── R-Dash ─────────────────────────────────────────────────────────────────
#
# Virtua's C runtime: _start, minos_current_user_abi, the MVII syscall stubs,
# malloc. Whole-archived into the ELF below, so it has to be configured with the
# same ABI flags or the link silently mixes float conventions.

if(NOT TARGET R-Dash)
    set(DASH_FIXED_HEAP_BYTES 0)
    add_subdirectory("${VIRTUA_ROOT}/Dash" "${CMAKE_BINARY_DIR}/virtua-dash" EXCLUDE_FROM_ALL)
endif()

target_include_directories(R-Dash BEFORE PUBLIC
    "${VIRTUA_POWERENGINE_POSIX_INCLUDE}"
    "${GBA_MVII_SDK}/include"
    "${GBA_MVII_SDK}/include/c++/v1")
target_compile_definitions(R-Dash PUBLIC ${_gba_arm_defs})
target_compile_options(R-Dash PRIVATE
    ${_gba_arm_flags}
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${GBA_MVII_SDK}/include/c++/v1>)

# ── the emulation core ─────────────────────────────────────────────────────
#
# A curated subset of gbarecomp, compiled here rather than pulled in with
# add_subdirectory(). That is not a shortcut around its CMake — it is the only
# correct thing to do, for two reasons.
#
# Its build describes a desktop program: it probes for ccache and SDL2, sets
# global -Wall -Wextra on everything, and links comdlg32/user32/winmm or
# ${CMAKE_DL_LIBS}. None of that survives a freestanding cross-compile.
#
# More importantly, gbarecomp_runtime is a *host* library. Alongside the parts a
# GBA needs it carries runtime.cpp (a CLI/TOML/filesystem monolith with the
# window and the audio device in it), the Stage-2 overlay recompiler (which
# emits C++, shells out to a compiler and dlopen's the result), a TCP debug
# server on BSD sockets, and native file dialogs. On MVII those are not merely
# unnecessary, they are unimplementable — there is no compiler, no dlopen and no
# socket layer on the device.
#
# So the list below is explicit, and each omission is a decision. This matches
# how the rest of this file lists its sources, and for the same reason: the
# interesting question here is what a GBA runtime actually needs, and a glob
# stops answering it.

set(_gba_core_sources
    # armv4t — the ARM7TDMI itself: decoders, the IR the recompiler emits from,
    # the dispatch/exception/tick engine the generated code calls into, and the
    # reference interpreter that bridges a coverage miss.
    # arm_codegen.cpp is absent: it is the recompiler's C++ *emitter*, needed to
    # produce generated code, never to run it.
    "${GBARECOMP_ROOT}/src/armv4t/arm_decode.cpp"
    "${GBARECOMP_ROOT}/src/armv4t/thumb_decode.cpp"
    "${GBARECOMP_ROOT}/src/armv4t/arm_ir.cpp"
    "${GBARECOMP_ROOT}/src/armv4t/runtime_arm.cpp"
    "${GBARECOMP_ROOT}/src/armv4t/symbol_lookup.cpp"
    "${GBARECOMP_ROOT}/src/armv4t/interpreter.cpp"

    # gba — the machine. All of it: every one of these is a device the hardware
    # has, and a GBA missing any of them is a different machine.
    "${GBARECOMP_ROOT}/src/gba/gba_memory.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_bus.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_io.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_irq.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_dma.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_timers.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_ppu.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_audio.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_m4a.cpp"
    "${GBARECOMP_ROOT}/src/gba/audio_shadow.cpp"
    "${GBARECOMP_ROOT}/src/gba/mp2k_shadow.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_input.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_save.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_gpio.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_gyro.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_rtc.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_bios.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_scheduler.cpp"
    "${GBARECOMP_ROOT}/src/gba/gba_rom_header.cpp"
    "${GBARECOMP_ROOT}/src/gba/sha1.cpp"
    "${GBARECOMP_ROOT}/src/gba/crc32.cpp"

    # runtime — only the parts that are the machine rather than the host.
    #   runtime_bus_bridge   binds the bus/PPU to generated code; owns the
    #                        device tick loop and the VBlank-start counter
    #   arm_cpu_bridge       the CPU-state view the interpreter bridge shares
    #   bios_hle             the SWI HLE layer and the boot-skip handoff
    #   color_lut            the PPU's 15-bit-to-RGB888 palette expansion
    #   save_config          save-type resolution
    #   runtime_arm_default_aborts
    #                        runtime_dispatch_miss and the interpreter bridge.
    #                        gbarecomp_armv4t deliberately ships no default for
    #                        it, so every consumer must supply one; compiling
    #                        gbarecomp's own is how this port inherits the exact
    #                        stop-address and IRQ-continuation contract instead
    #                        of reimplementing it slightly differently.
    #   self_heal_cluster    groups misses into proposals for the next recompile
    # Absent: runtime.cpp, overlay_{loader,compile,emit}.cpp, host_platform.cpp,
    # host_window.cpp, asset_picker.cpp, mod_runtime.cpp, generated_dispatch.cpp
    # (a stderr stub with no callers), and all of src/debug.
    "${GBARECOMP_ROOT}/src/runtime/runtime_bus_bridge.cpp"
    "${GBARECOMP_ROOT}/src/runtime/arm_cpu_bridge.cpp"
    "${GBARECOMP_ROOT}/src/runtime/bios_hle.cpp"
    "${GBARECOMP_ROOT}/src/runtime/color_lut.cpp"
    "${GBARECOMP_ROOT}/src/runtime/save_config.cpp"
    "${GBARECOMP_ROOT}/src/runtime/runtime_arm_default_aborts.cpp"
    "${GBARECOMP_ROOT}/src/runtime/self_heal_cluster.cpp")

# The recompiled BIOS, selected exactly as gbarecomp's own CMake selects it: the
# table and the bodies are one unit, and a clean checkout has neither, so it
# falls back to a tracked empty table. Without it a real BIOS image still runs —
# every BIOS PC misses and bridges through the interpreter, which is correct but
# slow, and the coverage report says so.
if(EXISTS "${GBARECOMP_ROOT}/src/runtime/generated_bios/bios_recompiled.cpp")
    list(APPEND _gba_core_sources
        "${GBARECOMP_ROOT}/src/runtime/generated_bios/bios_dispatch_table.cpp"
        "${GBARECOMP_ROOT}/src/runtime/generated_bios/bios_recompiled.cpp")
    set(_gba_have_bios_recomp ON)
    message(STATUS "gba-to-mvii: recompiled BIOS present — linking it")
else()
    list(APPEND _gba_core_sources
        "${GBARECOMP_ROOT}/src/runtime/generated_bios/bios_dispatch_stub.cpp")
    set(_gba_have_bios_recomp OFF)
    message(STATUS "gba-to-mvii: no recompiled BIOS — BIOS PCs will bridge "
                   "through the interpreter. Run "
                   "`gba_recompile --bios bios/gba_bios.bin` in ${GBARECOMP_ROOT} "
                   "to populate it.")
endif()

add_library(gbarecomp-mvii STATIC ${_gba_core_sources})

target_include_directories(gbarecomp-mvii BEFORE PRIVATE ${_gba_arm_includes})
target_include_directories(gbarecomp-mvii PUBLIC
    "${GBARECOMP_ROOT}/src"
    "${GBARECOMP_ROOT}/src/armv4t"
    "${GBARECOMP_ROOT}/src/gba"
    "${GBARECOMP_ROOT}/src/runtime"
    # snapshot.h only, and no snapshot.cpp. Every gba/ subsystem includes it for
    # SnapshotWriter/SnapshotReader, which are header-only precisely so that
    # gbarecomp_gba need not link gbarecomp_debug. The orchestrator in
    # snapshot.cpp is a different matter — it is <fstream> over a host path, so
    # it stays out, and this image has no save-state entry point in the ABI.
    "${GBARECOMP_ROOT}/src/debug")

target_compile_features(gbarecomp-mvii PRIVATE cxx_std_20)
target_compile_definitions(gbarecomp-mvii PRIVATE ${_gba_arm_defs})
if(_gba_have_bios_recomp)
    target_compile_definitions(gbarecomp-mvii PUBLIC GBARECOMP_HAVE_BIOS_RECOMP=1)
endif()
target_compile_options(gbarecomp-mvii PRIVATE
    ${_gba_arm_flags}
    ${_gba_arm_cxx_flags}
    # No NEON reaches a Virtua guest, and the auto-vectorizer will emit it given
    # any pixel or sample loop.
    -fno-vectorize
    -fno-slp-vectorize
    # Upstream warnings are not this port's to fix, and leaving them on buries
    # the ones that are. gbarecomp's own build has -Wall -Wextra; that is where
    # they belong.
    -w)

# ── sources ────────────────────────────────────────────────────────────────
#
# Listed one by one rather than globbed. This tree exists precisely to answer
# "what does a GBA runtime actually need", and a glob would stop answering it
# the moment someone drops a file in.

set(_mvii_sources
    "${GBA_MVII_SRC}/mvii/main.cpp"
    "${GBA_MVII_SRC}/mvii/mvii_platform.cpp"
    # include/gba_mvii.h implemented on gbarecomp, plus the device's half of the
    # self-healing loop (record a coverage miss; there is no compiler here to
    # close it with).
    "${GBA_MVII_SRC}/core/gba_mvii_core.cpp"
    "${GBA_MVII_SRC}/core/gba_mvii_heal.cpp"
    ${GBA_MVII_NATIVE_SOURCES}
    # Not part of R-Dash's own source list, but libc++/llvm-libc in the sysroot
    # is linked against it: errno, the stdio cookies, clock_gettime, usleep,
    # sched_yield, getenv. Every Virtua C++ app has to compile this in.
    "${VIRTUA_ROOT}/Dash/llvm_libc_stubs.cpp")

if(NOT GBA_MVII_NATIVE_AOT)
    # kDispatchTable / kDispatchTableLen are normally the recompiled cartridge's
    # dispatch_table.cpp. Without one, runtime_arm.cpp still references them, so
    # supply an empty table: every dispatch misses and bridges through the
    # interpreter. That is a working runtime and an honest one — the coverage
    # report reads NOT STATIC, which is exactly what a build with no recompiled
    # code is.
    list(APPEND _mvii_sources "${GBA_MVII_SRC}/core/gba_mvii_dispatch_stub.cpp")
endif()

# ── the runtime ELF ────────────────────────────────────────────────────────

add_executable(gba-to-mvii-elf ${_mvii_sources})

target_include_directories(gba-to-mvii-elf BEFORE PRIVATE ${_gba_arm_includes})
target_include_directories(gba-to-mvii-elf PRIVATE
    "${GBA_MVII_INC}"
    "${GBA_MVII_SRC}/mvii"
    "${GBA_MVII_SRC}/core")

target_compile_features(gba-to-mvii-elf PRIVATE cxx_std_20)

target_compile_definitions(gba-to-mvii-elf PRIVATE ${_gba_arm_defs})
if(GBA_MVII_NATIVE_AOT)
    target_compile_definitions(gba-to-mvii-elf PRIVATE GBA_MVII_NATIVE_AOT=1)
    # The recompiler shards very large translations, and full -O3 across
    # hundreds of megabytes of generated C++ turns packaging into an hours-long,
    # memory-heavy compiler benchmark. The hot operations are already explicit;
    # -O1 keeps the native speedup with per-unit compilation bounded.
    #
    # -w for the same reason as the core library: generated code is not written
    # to be warning-clean and its diagnostics would bury the real ones.
    set_source_files_properties(${GBA_MVII_NATIVE_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-O1;-w")
endif()

target_compile_options(gba-to-mvii-elf PRIVATE
    ${_gba_arm_flags}
    ${_gba_arm_cxx_flags}
    # No NEON reaches a Virtua guest, and the auto-vectorizer will happily emit
    # it for the per-pixel present loop if allowed to.
    -fno-vectorize
    -fno-slp-vectorize
    -Wall -Wextra)

add_dependencies(gba-to-mvii-elf R-Dash)

target_link_directories(gba-to-mvii-elf PRIVATE
    "${GBA_MVII_SDK}/lib"
    "${GBA_MVII_SDK}/lib/${VIRTUA_ARM_TARGET_TRIPLE}")

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
    "-Wl,-T,${VIRTUA_ARM_LINKER_SCRIPT}"
    # Whole-archive, because nothing in the runtime references _start and
    # --gc-sections would otherwise drop the entry point on the floor.
    -Wl,--whole-archive
    $<TARGET_FILE:R-Dash>
    -Wl,--no-whole-archive)

# Order is the contract with a static link: the emulation core comes first
# because it is what needs malloc, free, abort and libm out of the libraries
# after it.
target_link_libraries(gba-to-mvii-elf PRIVATE
    gbarecomp-mvii
    c++ c++abi unwind c m
    "${VIRTUA_ARM_COMPILER_RT}")

set_target_properties(gba-to-mvii-elf PROPERTIES
    OUTPUT_NAME "${GBA_MVII_OUTPUT_NAME}"
    SUFFIX ".elf"
    LINK_DEPENDS "${VIRTUA_ARM_LINKER_SCRIPT}")

# ── packaging ──────────────────────────────────────────────────────────────

virtua_prepare_go_tool(GBA_MVII_VIRTUA_TOOL
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}/binary" "virtua")
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
# main.cpp resolves the ROM as dirname(argv[0])/game.gba and the BIOS as
# dirname(argv[0])/gba_bios.bin, so a staged package is just the files side by
# side.

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
    if(GBA_MVII_BIOS)
        list(APPEND _gba_stage_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GBA_MVII_BIOS}" "${GBA_MVII_STAGE_DIR}/gba_bios.bin")
        list(APPEND _gba_stage_depends "${GBA_MVII_BIOS}")
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
