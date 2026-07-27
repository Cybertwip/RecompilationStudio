set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv7)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_OSX_ARCHITECTURES "" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "" CACHE STRING "" FORCE)
set(CMAKE_OSX_SYSROOT "" CACHE STRING "" FORCE)

set(VIRTUA_ARM_TARGET_TRIPLE "armv7a-none-eabi" CACHE STRING "Virtua ARM target triple")

set(_virtua_llvm_hints)
if(DEFINED VIRTUA_LLVM_ROOT AND NOT VIRTUA_LLVM_ROOT STREQUAL "")
    list(APPEND _virtua_llvm_hints "${VIRTUA_LLVM_ROOT}/bin")
endif()
list(APPEND _virtua_llvm_hints
    "/usr/local/opt/llvm/bin"
    "/opt/homebrew/opt/llvm/bin")

find_program(_virtua_clang NAMES clang compiler HINTS ${_virtua_llvm_hints} REQUIRED)
find_program(_virtua_clangxx NAMES clang++ compiler++ HINTS ${_virtua_llvm_hints} REQUIRED)
find_program(_virtua_ar NAMES llvm-ar ar HINTS ${_virtua_llvm_hints} REQUIRED)
find_program(_virtua_ranlib NAMES llvm-ranlib ranlib HINTS ${_virtua_llvm_hints} REQUIRED)
find_program(_virtua_objcopy NAMES llvm-objcopy objcopy HINTS ${_virtua_llvm_hints} REQUIRED)
find_program(_virtua_lld NAMES ld.lld HINTS ${_virtua_llvm_hints} REQUIRED)

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
