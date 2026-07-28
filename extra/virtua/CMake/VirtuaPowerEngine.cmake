include_guard(GLOBAL)

set(VIRTUA_ARM_TARGET_TRIPLE "armv7a-none-eabi" CACHE STRING
    "Virtua ARM target triple")

# PowerEngine owns the Virtua userspace ABI. PSXRecomp deliberately does not
# vendor Dash, the POSIX shim, the packager, or an LLVM runtime sysroot.
if(NOT DEFINED POWERENGINE_ROOT OR POWERENGINE_ROOT STREQUAL "")
    if(NOT "$ENV{POWERENGINE_ROOT}" STREQUAL "")
        set(POWERENGINE_ROOT "$ENV{POWERENGINE_ROOT}")
    elseif(NOT "$ENV{POWER_ENGINE_ROOT}" STREQUAL "")
        set(POWERENGINE_ROOT "$ENV{POWER_ENGINE_ROOT}")
    endif()
endif()
if(NOT DEFINED POWERENGINE_ROOT OR POWERENGINE_ROOT STREQUAL "")
    message(FATAL_ERROR
        "Virtua ARM requires POWERENGINE_ROOT. Point it at the PowerEngine source "
        "directory containing External/Virtua and OS/MVII.")
endif()
get_filename_component(_virtua_powerengine_root "${POWERENGINE_ROOT}" ABSOLUTE)
set(POWERENGINE_ROOT "${_virtua_powerengine_root}" CACHE PATH
    "PowerEngine source directory used by Virtua ARM builds" FORCE)

set(_virtua_powerengine_virtua_root "${POWERENGINE_ROOT}/External/Virtua")
set(_virtua_powerengine_posix_include
    "${POWERENGINE_ROOT}/OS/MVII/Kernel/Shared/posix-shim/include")
set(_virtua_powerengine_arm_linker
    "${POWERENGINE_ROOT}/OS/MVII/Examples/Pong/linker_armv7.ld")
foreach(_virtua_required IN ITEMS
        "${_virtua_powerengine_virtua_root}/CMake/GoTooling.cmake"
        "${_virtua_powerengine_virtua_root}/binary/virtua.go"
        "${_virtua_powerengine_virtua_root}/go.mod"
        "${_virtua_powerengine_virtua_root}/Dash/CMakeLists.txt"
        "${_virtua_powerengine_virtua_root}/Dash/llvm_libc_stubs.cpp"
        "${_virtua_powerengine_virtua_root}/Dash/armv7/crt.s"
        "${_virtua_powerengine_posix_include}/pthread.h"
        "${_virtua_powerengine_posix_include}/virtua_cpp_thread_shim.h"
        "${_virtua_powerengine_arm_linker}")
    if(NOT EXISTS "${_virtua_required}")
        message(FATAL_ERROR
            "POWERENGINE_ROOT='${POWERENGINE_ROOT}' is not a usable PowerEngine "
            "checkout; required Virtua/MVII file is missing: ${_virtua_required}")
    endif()
endforeach()

set(VIRTUA_POWERENGINE_VIRTUA_ROOT "${_virtua_powerengine_virtua_root}"
    CACHE INTERNAL "PowerEngine External/Virtua directory")
set(VIRTUA_POWERENGINE_POSIX_INCLUDE "${_virtua_powerengine_posix_include}"
    CACHE INTERNAL "PowerEngine MVII POSIX shim include directory")
set(VIRTUA_ARM_LINKER_SCRIPT "${_virtua_powerengine_arm_linker}"
    CACHE INTERNAL "PowerEngine ARMv7 Virtua linker script")

# The compiler root is supplied separately because Studio can point at a
# packaged PowerEngine compiler bundle. Every compiler/binutils executable is
# selected from this one root; falling back to PATH would silently mix ABIs.
if(NOT DEFINED VIRTUA_LLVM_ROOT OR VIRTUA_LLVM_ROOT STREQUAL "")
    if(NOT "$ENV{VIRTUA_LLVM_ROOT}" STREQUAL "")
        set(VIRTUA_LLVM_ROOT "$ENV{VIRTUA_LLVM_ROOT}")
    endif()
endif()
if(NOT DEFINED VIRTUA_LLVM_ROOT OR VIRTUA_LLVM_ROOT STREQUAL "")
    message(FATAL_ERROR
        "Virtua ARM requires VIRTUA_LLVM_ROOT. Point it at the LLVM/compiler "
        "bundle selected in PSXRecomp Studio.")
endif()
get_filename_component(_virtua_llvm_root "${VIRTUA_LLVM_ROOT}" ABSOLUTE)
set(VIRTUA_LLVM_ROOT "${_virtua_llvm_root}" CACHE PATH
    "LLVM compiler bundle used by Virtua ARM builds" FORCE)
if(NOT IS_DIRECTORY "${VIRTUA_LLVM_ROOT}/bin")
    message(FATAL_ERROR
        "VIRTUA_LLVM_ROOT='${VIRTUA_LLVM_ROOT}' has no bin directory.")
endif()

function(_virtua_pick_llvm_tool out_var description)
    foreach(_virtua_name IN LISTS ARGN)
        if(EXISTS "${VIRTUA_LLVM_ROOT}/bin/${_virtua_name}")
            set(${out_var} "${VIRTUA_LLVM_ROOT}/bin/${_virtua_name}"
                CACHE FILEPATH "${description}" FORCE)
            return()
        endif()
    endforeach()
    string(JOIN ", " _virtua_names ${ARGN})
    message(FATAL_ERROR
        "VIRTUA_LLVM_ROOT='${VIRTUA_LLVM_ROOT}' does not provide ${description} "
        "(${_virtua_names}).")
endfunction()

_virtua_pick_llvm_tool(VIRTUA_CLANG_EXECUTABLE
    "the Virtua ARM C compiler" compiler clang)
_virtua_pick_llvm_tool(VIRTUA_CLANGXX_EXECUTABLE
    "the Virtua ARM C++ compiler" compiler++ clang++)
_virtua_pick_llvm_tool(VIRTUA_AR_EXECUTABLE
    "llvm-ar" llvm-ar)
_virtua_pick_llvm_tool(VIRTUA_RANLIB_EXECUTABLE
    "llvm-ranlib" llvm-ranlib)
_virtua_pick_llvm_tool(VIRTUA_OBJCOPY_EXECUTABLE
    "llvm-objcopy" llvm-objcopy)
_virtua_pick_llvm_tool(VIRTUA_LLD_EXECUTABLE
    "ld.lld" ld.lld)

execute_process(
    COMMAND "${VIRTUA_CLANG_EXECUTABLE}" --version
    OUTPUT_VARIABLE _virtua_clang_version
    ERROR_VARIABLE _virtua_clang_version_error
    RESULT_VARIABLE _virtua_clang_version_result
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _virtua_clang_version_result EQUAL 0)
    message(FATAL_ERROR
        "Could not execute '${VIRTUA_CLANG_EXECUTABLE} --version': "
        "${_virtua_clang_version_error}")
endif()
if(_virtua_clang_version MATCHES "Apple clang")
    message(FATAL_ERROR
        "VIRTUA_LLVM_ROOT resolved Apple clang. Virtua ARM requires the "
        "PowerEngine LLVM/compiler bundle and its bare-metal ARM runtimes.")
endif()

function(_virtua_arm_sysroot_is_usable candidate out_var)
    if(IS_DIRECTORY "${candidate}"
       AND EXISTS "${candidate}/include/stdio.h"
       AND EXISTS "${candidate}/include/c++/v1/vector"
       AND EXISTS "${candidate}/lib/libc++.a"
       AND EXISTS "${candidate}/lib/libc++abi.a"
       AND EXISTS "${candidate}/lib/libunwind.a"
       AND EXISTS "${candidate}/lib/${VIRTUA_ARM_TARGET_TRIPLE}/libc.a"
       AND EXISTS "${candidate}/lib/${VIRTUA_ARM_TARGET_TRIPLE}/libm.a")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

if((NOT DEFINED VIRTUA_ARM_SYSROOT OR VIRTUA_ARM_SYSROOT STREQUAL "")
   AND NOT "$ENV{VIRTUA_ARM_SYSROOT}" STREQUAL "")
    set(VIRTUA_ARM_SYSROOT "$ENV{VIRTUA_ARM_SYSROOT}")
endif()
if((NOT DEFINED VIRTUA_ARM_SYSROOT OR VIRTUA_ARM_SYSROOT STREQUAL "")
   AND NOT "$ENV{MVII_ARM_SYSROOT}" STREQUAL "")
    set(VIRTUA_ARM_SYSROOT "$ENV{MVII_ARM_SYSROOT}")
endif()

set(_virtua_arm_sysroot_candidates)
if(DEFINED VIRTUA_ARM_SYSROOT AND NOT VIRTUA_ARM_SYSROOT STREQUAL "")
    get_filename_component(_virtua_explicit_sysroot "${VIRTUA_ARM_SYSROOT}" ABSOLUTE)
    list(APPEND _virtua_arm_sysroot_candidates "${_virtua_explicit_sysroot}")
else()
    # Standard LLVM runtime layouts first. A complete compiler bundle may carry
    # the bare-metal runtime directly under one of these directories.
    list(APPEND _virtua_arm_sysroot_candidates
        "${VIRTUA_LLVM_ROOT}/lib/clang-runtimes/${VIRTUA_ARM_TARGET_TRIPLE}"
        "${VIRTUA_LLVM_ROOT}/lib/clang-runtimes/arm-eabi"
        "${VIRTUA_LLVM_ROOT}/runtimes/arm-eabi"
        "${VIRTUA_LLVM_ROOT}/sysroot/arm-eabi"
        "${VIRTUA_LLVM_ROOT}/sysroot/${VIRTUA_ARM_TARGET_TRIPLE}"
        "${VIRTUA_LLVM_ROOT}/arm-eabi")

    # PowerEngine's packaged compiler and its MVII llvm-runtimes are siblings
    # under the same build tree. Walk ancestors of the selected LLVM root so a
    # Studio-selected bundle resolves its matching ARM sysroot without copying
    # that sysroot into PSXRecomp.
    set(_virtua_cursor "${VIRTUA_LLVM_ROOT}")
    foreach(_virtua_depth RANGE 0 10)
        list(APPEND _virtua_arm_sysroot_candidates
            "${_virtua_cursor}/MVII/toolchains/llvm-runtimes/install/arm-eabi"
            "${_virtua_cursor}/stage/MVII/toolchains/llvm-runtimes/install/arm-eabi")
        get_filename_component(_virtua_parent "${_virtua_cursor}" DIRECTORY)
        if(_virtua_parent STREQUAL _virtua_cursor)
            break()
        endif()
        set(_virtua_cursor "${_virtua_parent}")
    endforeach()

    list(APPEND _virtua_arm_sysroot_candidates
        "${POWERENGINE_ROOT}/build/Release/stage/MVII/toolchains/llvm-runtimes/install/arm-eabi"
        "${POWERENGINE_ROOT}/build/Debug/stage/MVII/toolchains/llvm-runtimes/install/arm-eabi")
    file(GLOB _virtua_powerengine_sysroots LIST_DIRECTORIES TRUE
        "${POWERENGINE_ROOT}/build/*/stage/MVII/toolchains/llvm-runtimes/install/arm-eabi")
    list(APPEND _virtua_arm_sysroot_candidates ${_virtua_powerengine_sysroots})
endif()
list(REMOVE_DUPLICATES _virtua_arm_sysroot_candidates)

set(_virtua_resolved_arm_sysroot "")
foreach(_virtua_candidate IN LISTS _virtua_arm_sysroot_candidates)
    _virtua_arm_sysroot_is_usable("${_virtua_candidate}" _virtua_candidate_usable)
    if(_virtua_candidate_usable)
        get_filename_component(_virtua_resolved_arm_sysroot
            "${_virtua_candidate}" ABSOLUTE)
        break()
    endif()
endforeach()
if(NOT _virtua_resolved_arm_sysroot)
    string(JOIN "\n  " _virtua_sysroot_report ${_virtua_arm_sysroot_candidates})
    message(FATAL_ERROR
        "Could not resolve the PowerEngine ARM llvm-libc/libc++ sysroot from "
        "VIRTUA_LLVM_ROOT='${VIRTUA_LLVM_ROOT}'. Checked:\n  "
        "${_virtua_sysroot_report}\nBuild PowerEngine's mvii-llvm-runtimes-arm-eabi "
        "target or pass -DVIRTUA_ARM_SYSROOT=<path>.")
endif()
set(VIRTUA_ARM_SYSROOT "${_virtua_resolved_arm_sysroot}" CACHE PATH
    "PowerEngine ARM llvm-libc/libc++ sysroot" FORCE)
set(VIRTUA_ARM_CXX_INCLUDE "${VIRTUA_ARM_SYSROOT}/include/c++/v1"
    CACHE INTERNAL "PowerEngine ARM libc++ headers")

if((NOT DEFINED VIRTUA_ARM_COMPILER_RT OR VIRTUA_ARM_COMPILER_RT STREQUAL "")
   AND NOT "$ENV{VIRTUA_ARM_COMPILER_RT}" STREQUAL "")
    set(VIRTUA_ARM_COMPILER_RT "$ENV{VIRTUA_ARM_COMPILER_RT}")
endif()

set(_virtua_compiler_rt_candidates)
if(DEFINED VIRTUA_ARM_COMPILER_RT AND NOT VIRTUA_ARM_COMPILER_RT STREQUAL "")
    get_filename_component(_virtua_explicit_compiler_rt
        "${VIRTUA_ARM_COMPILER_RT}" ABSOLUTE)
    list(APPEND _virtua_compiler_rt_candidates "${_virtua_explicit_compiler_rt}")
else()
    execute_process(
        COMMAND "${VIRTUA_CLANG_EXECUTABLE}"
                "--target=${VIRTUA_ARM_TARGET_TRIPLE}"
                --print-libgcc-file-name
        OUTPUT_VARIABLE _virtua_printed_compiler_rt
        ERROR_QUIET
        RESULT_VARIABLE _virtua_printed_compiler_rt_result
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_virtua_printed_compiler_rt_result EQUAL 0
       AND NOT _virtua_printed_compiler_rt STREQUAL "")
        list(APPEND _virtua_compiler_rt_candidates "${_virtua_printed_compiler_rt}")
    endif()

    execute_process(
        COMMAND "${VIRTUA_CLANG_EXECUTABLE}" --print-resource-dir
        OUTPUT_VARIABLE _virtua_resource_dir
        ERROR_QUIET
        RESULT_VARIABLE _virtua_resource_dir_result
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_virtua_resource_dir_result EQUAL 0 AND IS_DIRECTORY "${_virtua_resource_dir}")
        list(APPEND _virtua_compiler_rt_candidates
            "${_virtua_resource_dir}/lib/armv7a-unknown-none-eabi/libclang_rt.builtins.a"
            "${_virtua_resource_dir}/lib/${VIRTUA_ARM_TARGET_TRIPLE}/libclang_rt.builtins.a"
            "${_virtua_resource_dir}/lib/baremetal/libclang_rt.builtins-arm.a")
        file(GLOB _virtua_resource_builtins
            "${_virtua_resource_dir}/lib/baremetal/libclang_rt.builtins-arm*.a")
        list(APPEND _virtua_compiler_rt_candidates ${_virtua_resource_builtins})
    endif()

    list(APPEND _virtua_compiler_rt_candidates
        "${VIRTUA_ARM_SYSROOT}/lib/clang-runtime/libclang_rt.builtins-armv7.a")
    set(_virtua_cursor "${VIRTUA_LLVM_ROOT}")
    foreach(_virtua_depth RANGE 0 10)
        list(APPEND _virtua_compiler_rt_candidates
            "${_virtua_cursor}/MVII/architecture/armv7/mediatek-j36-ultra/compiler-rt-builtins-armv7/compiler-rt/lib/generic/libclang_rt.builtins-armv7.a"
            "${_virtua_cursor}/stage/MVII/architecture/armv7/mediatek-j36-ultra/compiler-rt-builtins-armv7/compiler-rt/lib/generic/libclang_rt.builtins-armv7.a")
        get_filename_component(_virtua_parent "${_virtua_cursor}" DIRECTORY)
        if(_virtua_parent STREQUAL _virtua_cursor)
            break()
        endif()
        set(_virtua_cursor "${_virtua_parent}")
    endforeach()
    list(APPEND _virtua_compiler_rt_candidates
        "${POWERENGINE_ROOT}/build/Release/stage/MVII/architecture/armv7/mediatek-j36-ultra/compiler-rt-builtins-armv7/compiler-rt/lib/generic/libclang_rt.builtins-armv7.a"
        "${POWERENGINE_ROOT}/build/Debug/stage/MVII/architecture/armv7/mediatek-j36-ultra/compiler-rt-builtins-armv7/compiler-rt/lib/generic/libclang_rt.builtins-armv7.a")
    file(GLOB _virtua_powerengine_builtins
        "${POWERENGINE_ROOT}/build/*/stage/MVII/architecture/armv7/mediatek-j36-ultra/compiler-rt-builtins-armv7/compiler-rt/lib/generic/libclang_rt.builtins-armv7.a")
    list(APPEND _virtua_compiler_rt_candidates ${_virtua_powerengine_builtins})
endif()
list(REMOVE_DUPLICATES _virtua_compiler_rt_candidates)

set(_virtua_resolved_compiler_rt "")
foreach(_virtua_candidate IN LISTS _virtua_compiler_rt_candidates)
    if(EXISTS "${_virtua_candidate}")
        get_filename_component(_virtua_resolved_compiler_rt
            "${_virtua_candidate}" ABSOLUTE)
        break()
    endif()
endforeach()
if(NOT _virtua_resolved_compiler_rt)
    string(JOIN "\n  " _virtua_compiler_rt_report ${_virtua_compiler_rt_candidates})
    message(FATAL_ERROR
        "Could not resolve ARMv7 compiler-rt builtins for the selected LLVM root. "
        "Checked:\n  ${_virtua_compiler_rt_report}\nBuild PowerEngine's "
        "mvii-armv7-compiler-rt-builtins target or pass "
        "-DVIRTUA_ARM_COMPILER_RT=<archive>.")
endif()
set(VIRTUA_ARM_COMPILER_RT "${_virtua_resolved_compiler_rt}" CACHE FILEPATH
    "PowerEngine ARMv7 compiler-rt builtins archive" FORCE)
