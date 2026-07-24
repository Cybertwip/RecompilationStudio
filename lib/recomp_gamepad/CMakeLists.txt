cmake_minimum_required(VERSION 3.20)

# Reusable host-controller pieces shared by PSX and GBA frontends. Parent
# projects provide their pinned SDL/libusb targets through cache variables so
# this component never performs an unpinned dependency download itself.

set(RECOMP_GAMEPAD_SDL_INCLUDE_DIRS "" CACHE STRING
    "SDL2 include directories supplied by the parent project")
set(RECOMP_GAMEPAD_SDL_LIBRARIES "" CACHE STRING
    "SDL2 libraries or targets supplied by the parent project")
set(RECOMP_GAMEPAD_GIP_INCLUDE_DIRS "" CACHE STRING
    "libusb include directories supplied by the parent project")
set(RECOMP_GAMEPAD_GIP_LIBRARIES "" CACHE STRING
    "libusb and platform libraries supplied by the parent project")
option(RECOMP_GAMEPAD_BUILD_GIP
    "Build the direct-USB Xbox GIP transport" OFF)

add_library(recomp_gamepad_identity STATIC
    src/controller_identity.cpp)
add_library(recomp_gamepad::identity ALIAS recomp_gamepad_identity)
target_compile_features(recomp_gamepad_identity PUBLIC cxx_std_17)
target_include_directories(recomp_gamepad_identity
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${RECOMP_GAMEPAD_SDL_INCLUDE_DIRS})
if(RECOMP_GAMEPAD_SDL_LIBRARIES)
    target_link_libraries(recomp_gamepad_identity PUBLIC
        ${RECOMP_GAMEPAD_SDL_LIBRARIES})
endif()
set_property(TARGET recomp_gamepad_identity PROPERTY POSITION_INDEPENDENT_CODE ON)

if(RECOMP_GAMEPAD_BUILD_GIP)
    if(NOT RECOMP_GAMEPAD_GIP_INCLUDE_DIRS OR NOT RECOMP_GAMEPAD_GIP_LIBRARIES)
        message(FATAL_ERROR
            "RECOMP_GAMEPAD_BUILD_GIP=ON requires libusb include and link inputs")
    endif()
    add_library(recomp_gamepad_gip STATIC src/gip_gamepad.cpp)
    add_library(recomp_gamepad::gip ALIAS recomp_gamepad_gip)
    target_compile_features(recomp_gamepad_gip PUBLIC cxx_std_17)
    target_compile_definitions(recomp_gamepad_gip PUBLIC PSX_HAVE_GIP_GAMEPAD=1)
    target_include_directories(recomp_gamepad_gip
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${RECOMP_GAMEPAD_GIP_INCLUDE_DIRS})
    target_link_libraries(recomp_gamepad_gip PUBLIC
        ${RECOMP_GAMEPAD_GIP_LIBRARIES})
    set_property(TARGET recomp_gamepad_gip PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()
