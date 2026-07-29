# gba-to-mvii — a real GBA runtime for MVII, not a wrapper.
#
# Two halves, and the split is the whole design:
#
#   rust/   the emulation core, vendored from extra/gba-rust (gba-core +
#           armv4t) and ported to no_std. cargo builds it into a staticlib.
#   src/    MVII, and nothing else: /dev/fb0, /dev/input0, /dev/dac0, the
#           cooperative yield, the ROM and save files.
#
# They meet at include/gba_mvii.h, a C ABI narrow enough that the C++ side never
# names a Rust type and the Rust side never makes a syscall. That keeps the
# emulation identical to the reference implementation, which is the only way to
# tell an emulator bug from a port bug.
#
# Configure it with the Virtua ARM toolchain — there is no host build, because
# there is no host target here:
#
#   cmake -S extra/gba-to-mvii -B build/gba-to-mvii \
#         -DCMAKE_TOOLCHAIN_FILE=extra/virtua/CMake/VirtuaArmToolchain.cmake \
#         -DPOWERENGINE_ROOT=/path/to/PowerEngine \
#         -DVIRTUA_LLVM_ROOT=/path/to/PowerEngine/compiler-bundle \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/gba-to-mvii --target gba-to-mvii
#
# Prerequisites beyond PowerEngine + its LLVM/runtime build: a Rust toolchain
# with the bare-metal ARM standard library, which is one command —
#
#   rustup target add armv7a-none-eabi
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

include("${CMAKE_CURRENT_LIST_DIR}/../virtua/CMake/VirtuaPowerEngine.cmake")
set(VIRTUA_ROOT "${VIRTUA_POWERENGINE_VIRTUA_ROOT}")
set(GBA_MVII_SDK "${VIRTUA_ARM_SYSROOT}")
set(GBA_MVII_SRC "${CMAKE_CURRENT_LIST_DIR}/src")
set(GBA_MVII_INC "${CMAKE_CURRENT_LIST_DIR}/include")
set(GBA_MVII_RUST "${CMAKE_CURRENT_LIST_DIR}/rust")

# Plain variables, not cache entries: -D on the command line still defines them,
# and an enclosing project can set() them before add_subdirectory() without
# having to FORCE its way past a cached default.
#
#   GBA_MVII_OUTPUT_NAME  base name of the emitted .virtua executable
#   GBA_MVII_ROM          optional ROM to stage beside it as game.gba
#   GBA_MVII_STAGE_DIR    optional directory to stage the pair into
if(NOT DEFINED GBA_MVII_OUTPUT_NAME OR GBA_MVII_OUTPUT_NAME STREQUAL "")
    set(GBA_MVII_OUTPUT_NAME "gba-runtime")
endif()
if(NOT DEFINED GBA_MVII_ROM)
    set(GBA_MVII_ROM "")
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

# ── the emulation core (Rust) ──────────────────────────────────────────────
#
# cargo is a second build system, so the contract with it is deliberately thin:
# CMake decides *when* to run it and where the archive lands, cargo decides
# everything else. The crate manifests under rust/ are the source list — that is
# what a Cargo.toml is for — so unlike the C++ half this really is a glob, and
# CONFIGURE_DEPENDS keeps it honest when a file appears.

find_program(GBA_MVII_CARGO NAMES cargo
    HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin"
    DOC "cargo, for the Rust emulation core under rust/")
if(NOT GBA_MVII_CARGO)
    message(FATAL_ERROR
        "cargo not found. The GBA emulation core is the Rust port under "
        "${GBA_MVII_RUST}; install a Rust toolchain and run "
        "'rustup target add ${VIRTUA_ARM_TARGET_TRIPLE}', or point "
        "-DGBA_MVII_CARGO=<path> at it.")
endif()

# Out of the source tree, so cleaning the build directory really cleans and two
# configurations do not fight over one target/ directory.
set(_gba_cargo_dir "${CMAKE_CURRENT_BINARY_DIR}/rust")
set(GBA_MVII_RUST_LIB
    "${_gba_cargo_dir}/${VIRTUA_ARM_TARGET_TRIPLE}/release/libgba_mvii.a")

file(GLOB_RECURSE _gba_rust_sources CONFIGURE_DEPENDS
    "${GBA_MVII_RUST}/*.rs"
    "${GBA_MVII_RUST}/*.toml")

set(_gba_cargo_feature_args)
if(GBA_MVII_NATIVE_AOT)
    list(APPEND _gba_cargo_feature_args --features native-aot)
endif()

add_custom_command(
    OUTPUT "${GBA_MVII_RUST_LIB}"
    # --locked and --offline together: the workspace has no external
    # dependencies at all, and a build that can reach the network is a build
    # that can change underneath you.
    COMMAND "${GBA_MVII_CARGO}" build
        --release
        --locked
        --offline
        --package gba-mvii
        ${_gba_cargo_feature_args}
        --target "${VIRTUA_ARM_TARGET_TRIPLE}"
        --target-dir "${_gba_cargo_dir}"
    # From rust/, so cargo picks up rust/.cargo/config.toml — which carries the
    # ABI flags that have to match the C++ half: static relocations (all the
    # virtua packager's relocation handler knows) and unaligned access (this
    # side is -munaligned-access; see the long note in that config.toml for why
    # both halves flipped away from strict alignment and what makes it safe).
    WORKING_DIRECTORY "${GBA_MVII_RUST}"
    DEPENDS ${_gba_rust_sources}
    COMMENT "Building the GBA emulation core (cargo, ${VIRTUA_ARM_TARGET_TRIPLE})"
    VERBATIM)

add_custom_target(gba-to-mvii-core DEPENDS "${GBA_MVII_RUST_LIB}")

# ── sources ────────────────────────────────────────────────────────────────
#
# Listed one by one rather than globbed. This tree exists precisely to answer
# "what does a GBA runtime actually need", and a glob would stop answering it
# the moment someone drops a file in.

set(_mvii_sources
    "${GBA_MVII_SRC}/mvii/main.cpp"
    "${GBA_MVII_SRC}/mvii/mvii_platform.cpp"
    ${GBA_MVII_NATIVE_SOURCES}
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
    "${VIRTUA_POWERENGINE_POSIX_INCLUDE}"
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
    # Not -mno-unaligned-access: that costs 4 ldrb + 3 orr per 32-bit load where
    # the core takes one ldr. SCTLR.A is 0 and the guest lives in Normal-WBWA
    # DRAM, so the hardware handles the unaligned case. Must stay in step with
    # rust/.cargo/config.toml's -strict-align, which documents the whole thing.
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
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${GBA_MVII_SDK}/include/c++/v1>)

# ── the runtime ELF ────────────────────────────────────────────────────────

add_executable(gba-to-mvii-elf ${_mvii_sources})

# BEFORE, so Virtua's shims win over anything the sources may otherwise find.
target_include_directories(gba-to-mvii-elf BEFORE PRIVATE
    "${VIRTUA_POWERENGINE_POSIX_INCLUDE}"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}"
    "${VIRTUA_POWERENGINE_VIRTUA_ROOT}/Dash"
    "${GBA_MVII_SDK}/include"
    "${GBA_MVII_SDK}/include/c++/v1")
target_include_directories(gba-to-mvii-elf PRIVATE
    "${GBA_MVII_INC}"
    "${GBA_MVII_SRC}/mvii")

target_compile_features(gba-to-mvii-elf PRIVATE cxx_std_20)

target_compile_definitions(gba-to-mvii-elf PRIVATE
    MINOS_PLATFORM_MACHINE64=1
    MINOS_PLATFORM_ARMV7=1
    MINOS_VIRTUA_USERLAND=1
    _LIBCPP_HAS_NO_THREADS
    _LIBCPP_DISABLE_AVAILABILITY
    _LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE)
if(GBA_MVII_NATIVE_AOT)
    target_compile_definitions(gba-to-mvii-elf PRIVATE GBA_MVII_NATIVE_AOT=1)
    # gba-rust deliberately shards very large translations and uses modest
    # optimization: full -O3 on hundreds of megabytes of generated C turns
    # packaging into an hours-long, memory-heavy compiler benchmark. The hot
    # operations are already explicit C; -O1 preserves the native speedup while
    # keeping per-unit compilation bounded.
    set_source_files_properties(${GBA_MVII_NATIVE_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-O1")
endif()

target_compile_options(gba-to-mvii-elf PRIVATE
    "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
    "--sysroot=${GBA_MVII_SDK}"
    -mcpu=cortex-a7
    -marm
    -mfloat-abi=soft
    # See the note on the same flag in the R-Dash options above.
    -munaligned-access
    -femulated-tls
    -ffreestanding
    -fpie
    -fno-stack-protector
    -fno-common
    # No NEON reaches a Virtua guest, and the auto-vectorizer will happily emit
    # it for the per-pixel present loop if allowed to.
    -fno-vectorize
    -fno-slp-vectorize
    -ffunction-sections
    -fdata-sections
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
    $<$<COMPILE_LANGUAGE:CXX>:-isystem${GBA_MVII_SDK}/include/c++/v1>
    $<$<COMPILE_LANGUAGE:CXX>:SHELL:-include virtua_cpp_thread_shim.h>)

add_dependencies(gba-to-mvii-elf R-Dash gba-to-mvii-core)

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
    "-Wl,-T,${VIRTUA_ARM_LINKER_SCRIPT}"
    # Whole-archive, because nothing in the runtime references _start and
    # --gc-sections would otherwise drop the entry point on the floor.
    -Wl,--whole-archive
    $<TARGET_FILE:R-Dash>
    -Wl,--no-whole-archive)

# Order is the contract with a static link: the emulation core comes first
# because it is what needs malloc, free, abort and libm out of the libraries
# after it. Naming it by full path also gives the ELF a file-level dependency on
# the archive, so a cargo rebuild relinks without anything else being told.
target_link_libraries(gba-to-mvii-elf PRIVATE
    "${GBA_MVII_RUST_LIB}"
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
add_dependencies(gba-to-mvii gba-to-mvii-core gba-to-mvii-elf gba-to-mvii-tool)

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
