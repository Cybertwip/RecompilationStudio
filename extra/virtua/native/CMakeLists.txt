cmake_minimum_required(VERSION 3.20)
project(VirtuaNativeLaunchers C ASM)

include("${CMAKE_CURRENT_LIST_DIR}/../CMake/VirtuaArmApp.cmake")
virtua_add_armv7_app(gba-native
    GUI
    COOPERATIVE
    OUTPUT_NAME "GbaNative"
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/gba_native_main.c")
