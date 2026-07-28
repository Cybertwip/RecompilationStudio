set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv7)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "" CACHE STRING "" FORCE)
set(CMAKE_OSX_SYSROOT "" CACHE STRING "" FORCE)

set(VIRTUA_ARM_TARGET_TRIPLE "armv7a-none-eabi" CACHE STRING "Virtua ARM target triple")

# Where the LLVM cross-toolchain lives. Set it explicitly -- via
# -DVIRTUA_LLVM_ROOT=<root>, the VIRTUA_LLVM_ROOT environment variable, or the
# "LLVM toolchain" field in PSXRecomp Studio.
#
# This matters more than it looks. MVII targets a custom BSD-style syscall ABI
# and supplies its own llvm-libc/libc++ sysroot. A host or Linux clang will
# happily accept --target=armv7a-none-eabi and produce an object file, so the
# build succeeds -- but the resulting binary is linked against a libc whose
# syscall layer does not match MVII's, and the failure only shows up at runtime
# as syscalls that silently return errors (a clock that never advances, for
# instance). So resolve every tool from one known root, and refuse to guess.
if(NOT DEFINED VIRTUA_LLVM_ROOT OR VIRTUA_LLVM_ROOT STREQUAL "")
    if(NOT "$ENV{VIRTUA_LLVM_ROOT}" STREQUAL "")
        set(VIRTUA_LLVM_ROOT "$ENV{VIRTUA_LLVM_ROOT}")
    endif()
endif()

set(_virtua_find_extra)
if(DEFINED VIRTUA_LLVM_ROOT AND NOT VIRTUA_LLVM_ROOT STREQUAL "")
    if(NOT IS_DIRECTORY "${VIRTUA_LLVM_ROOT}/bin")
        message(FATAL_ERROR
            "VIRTUA_LLVM_ROOT is set to '${VIRTUA_LLVM_ROOT}' but '${VIRTUA_LLVM_ROOT}/bin' "
            "does not exist. Point it at an LLVM installation root (the directory "
            "containing bin/clang).")
    endif()
    # Pin to this root only. Falling back to PATH here would defeat the purpose
    # of naming a toolchain, and silently mixing a clang from one install with
    # an ld.lld from another is its own class of bug.
    set(_virtua_llvm_hints "${VIRTUA_LLVM_ROOT}/bin")
    set(_virtua_find_extra NO_DEFAULT_PATH)
else()
    set(_virtua_llvm_hints
        "/usr/local/opt/llvm/bin"
        "/opt/homebrew/opt/llvm/bin")
endif()

find_program(_virtua_clang   NAMES clang compiler  HINTS ${_virtua_llvm_hints} ${_virtua_find_extra} REQUIRED)
find_program(_virtua_clangxx NAMES clang++ compiler++ HINTS ${_virtua_llvm_hints} ${_virtua_find_extra} REQUIRED)
find_program(_virtua_ar      NAMES llvm-ar HINTS ${_virtua_llvm_hints} ${_virtua_find_extra} REQUIRED)
find_program(_virtua_ranlib  NAMES llvm-ranlib HINTS ${_virtua_llvm_hints} ${_virtua_find_extra} REQUIRED)
find_program(_virtua_objcopy NAMES llvm-objcopy HINTS ${_virtua_llvm_hints} ${_virtua_find_extra} REQUIRED)
find_program(_virtua_lld     NAMES ld.lld  HINTS ${_virtua_llvm_hints} ${_virtua_find_extra} REQUIRED)

# Verify the compiler we resolved is actually an LLVM cross-clang, once per
# cache. Apple clang is the usual accidental pick on macOS: it is first on PATH,
# it accepts the target triple, and it ships neither llvm-libc nor the baremetal
# runtime this target needs.
if(NOT DEFINED VIRTUA_LLVM_VERIFIED_COMPILER OR
   NOT VIRTUA_LLVM_VERIFIED_COMPILER STREQUAL "${_virtua_clang}")
    execute_process(COMMAND "${_virtua_clang}" --version
                    OUTPUT_VARIABLE _virtua_clang_version
                    ERROR_VARIABLE  _virtua_clang_version_err
                    RESULT_VARIABLE _virtua_clang_version_rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _virtua_clang_version_rc EQUAL 0)
        message(FATAL_ERROR
            "Could not run '${_virtua_clang} --version': ${_virtua_clang_version_err}")
    endif()
    if(_virtua_clang_version MATCHES "Apple clang")
        message(FATAL_ERROR
            "'${_virtua_clang}' is Apple clang, which cannot build the Virtua ARM "
            "baremetal target (${VIRTUA_ARM_TARGET_TRIPLE}).\n"
            "Install LLVM (brew install llvm) and pass -DVIRTUA_LLVM_ROOT=<llvm-root>, "
            "or set the \"LLVM toolchain\" path in PSXRecomp Studio.")
    endif()
    if(NOT DEFINED VIRTUA_LLVM_ROOT OR VIRTUA_LLVM_ROOT STREQUAL "")
        message(WARNING
            "VIRTUA_LLVM_ROOT is not set; using '${_virtua_clang}' found via the default "
            "search path. Set it explicitly to guarantee the toolchain matches MVII's "
            "libc and syscall ABI.")
    endif()
    set(VIRTUA_LLVM_VERIFIED_COMPILER "${_virtua_clang}"
        CACHE INTERNAL "Virtua ARM compiler that passed toolchain validation")
endif()

set(CMAKE_C_COMPILER "${_virtua_clang}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_virtua_clangxx}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_virtua_clang}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_TARGET "${VIRTUA_ARM_TARGET_TRIPLE}" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_TARGET "${VIRTUA_ARM_TARGET_TRIPLE}" CACHE STRING "" FORCE)
set(CMAKE_ASM_COMPILER_TARGET "${VIRTUA_ARM_TARGET_TRIPLE}" CACHE STRING "" FORCE)
set(CMAKE_AR "${_virtua_ar}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_virtua_ranlib}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${_virtua_objcopy}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${_virtua_lld}" CACHE FILEPATH "" FORCE)
