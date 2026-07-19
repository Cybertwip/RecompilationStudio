cmake_minimum_required(VERSION 3.21)
project(PSXRecompStudio VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(APPLE AND (NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "14.0"))
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Core5Compat Gui Widgets Svg Network)

add_subdirectory(../recompiler/lib/fmt ${CMAKE_CURRENT_BINARY_DIR}/fmt EXCLUDE_FROM_ALL)
if(NOT MSVC)
  target_compile_definitions(fmt PUBLIC FMT_CONSTEVAL=)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(QLEMENTINE_SANDBOX OFF CACHE BOOL "" FORCE)
set(QLEMENTINE_SHOWCASE OFF CACHE BOOL "" FORCE)
set(QLEMENTINE_ICONS_SANDBOX OFF CACHE BOOL "" FORCE)
set(QTAPPINSTANCEMANAGER_TESTS OFF CACHE BOOL "" FORCE)
set(QTAPPINSTANCEMANAGER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QUAZIP_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(QUAZIP_INSTALL OFF CACHE BOOL "" FORCE)
set(QUAZIP_BZIP2 OFF CACHE BOOL "" FORCE)


# Force static zlib (matches QuaZip's BUILD_SHARED_LIBS=OFF and avoids
# shipping a zlib DLL on Windows).  Also disable its install rules.
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON  CACHE BOOL "" FORCE)
set(ZLIB_INSTALL      OFF CACHE BOOL "" FORCE)

add_subdirectory(3rdparty/zlib)

# Make FindZLIB (used by QuaZip) happy with the in-tree build instead of
# searching for a system zlib.
set(ZLIB_INCLUDE_DIR
    "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}"
    CACHE PATH "zlib include directory" FORCE)

if(TARGET zlibstatic)
  if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
  endif()
  set(ZLIB_LIBRARY ZLIB::ZLIB CACHE FILEPATH "zlib library" FORCE)
elseif(TARGET zlib)
  if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib)
  endif()
  set(ZLIB_LIBRARY ZLIB::ZLIB CACHE FILEPATH "zlib library" FORCE)
else()
  message(FATAL_ERROR "3rdparty/zlib did not create zlib or zlibstatic")
endif()

add_subdirectory(3rdparty/quazip)
add_subdirectory(3rdparty/oclero/qlementine)
add_subdirectory(3rdparty/oclero/qlementine-icons)
add_subdirectory(3rdparty/oclero/qtappinstancemanager)

set(STUDIO_CORE_SOURCES
  src/BiosBrandingPatcher.cpp
  src/BiosBrandingPatcher.h
  src/CueSheet.cpp
  src/CueSheet.h
  src/DiscInspector.cpp
  src/DiscInspector.h
  src/PipelineSupport.cpp
  src/PipelineSupport.h
  src/PipelineTypes.h
  src/PipelineWorker.cpp
  src/PipelineWorker.h
  ../runtime/src/iso_reader.cpp
  ../recompiler/src/ps1_exe_parser.cpp
)

qt_add_executable(PSXRecompStudio MACOSX_BUNDLE
  src/main.cpp
  src/MainWindow.cpp
  src/MainWindow.h
  ${STUDIO_CORE_SOURCES}
)

qt_add_resources(PSXRecompStudio studio_resources
  PREFIX "/psxrecomp/studio"
  FILES
    resources/psxrecomp-studio.svg
    resources/themes/dark.json
    resources/themes/light.json
)

target_include_directories(PSXRecompStudio PRIVATE
  src
  ../runtime/include
  ../recompiler/include
)

target_link_libraries(PSXRecompStudio PRIVATE
  Qt6::Core
  Qt6::Core5Compat
  Qt6::Gui
  Qt6::Widgets
  Qt6::Svg
  Qt6::Network
  qlementine
  qlementine-icons
  oclero::QtAppInstanceManager
  QuaZip
  fmt
)

target_compile_definitions(PSXRecompStudio PRIVATE
  PSXRECOMP_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
  PSX_STUDIO_VERSION="${PROJECT_VERSION}"
)

set_target_properties(PSXRecompStudio PROPERTIES
  OUTPUT_NAME "PSXRecomp Studio"
  MACOSX_BUNDLE_BUNDLE_NAME "PSXRecomp Studio"
  MACOSX_BUNDLE_GUI_IDENTIFIER "org.psxrecomp.studio"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
)

qt_add_executable(PSXBiosBrandingPatch
  src/bios_patch_main.cpp
  src/BiosBrandingPatcher.cpp
  src/BiosBrandingPatcher.h
)
target_include_directories(PSXBiosBrandingPatch PRIVATE src)
target_link_libraries(PSXBiosBrandingPatch PRIVATE Qt6::Core Qt6::Gui)

include(CTest)
if(BUILD_TESTING)
  qt_add_executable(PSXRecompStudioTests
    tests/CoreTests.cpp
    src/BiosBrandingPatcher.cpp
    src/BiosBrandingPatcher.h
    src/CueSheet.cpp
    src/CueSheet.h
    src/DiscInspector.cpp
    src/DiscInspector.h
    src/PipelineSupport.cpp
    src/PipelineSupport.h
    src/PipelineTypes.h
    ../runtime/src/iso_reader.cpp
    ../recompiler/src/ps1_exe_parser.cpp
  )
  target_include_directories(PSXRecompStudioTests PRIVATE
    src
    ../runtime/include
    ../recompiler/include
  )
  target_link_libraries(PSXRecompStudioTests PRIVATE
    Qt6::Core Qt6::Core5Compat Qt6::Gui Qt6::Svg QuaZip fmt
  )
  add_test(NAME PSXRecompStudioTests COMMAND PSXRecompStudioTests)

  set(PSX_BRANDING_TEST_BIOS "${CMAKE_CURRENT_SOURCE_DIR}/../branding/SCPH1001.BIN")
  if(EXISTS "${PSX_BRANDING_TEST_BIOS}")
    qt_add_executable(PSXBiosBrandingPatcherTests
      tests/BiosBrandingPatcherTests.cpp
      src/BiosBrandingPatcher.cpp
      src/BiosBrandingPatcher.h
    )
    target_include_directories(PSXBiosBrandingPatcherTests PRIVATE src)
    target_link_libraries(PSXBiosBrandingPatcherTests PRIVATE Qt6::Core Qt6::Gui)
    target_compile_definitions(PSXBiosBrandingPatcherTests PRIVATE
      PSX_BRANDING_TEST_BIOS="${PSX_BRANDING_TEST_BIOS}")
    add_test(NAME PSXBiosBrandingPatcherTests COMMAND PSXBiosBrandingPatcherTests)
  endif()
endif()

if(APPLE)
  install(TARGETS PSXRecompStudio BUNDLE DESTINATION . COMPONENT Studio)
  find_program(PSX_STUDIO_MACDEPLOYQT NAMES macdeployqt REQUIRED)
  find_program(PSX_STUDIO_QTPATHS NAMES qtpaths6 qtpaths REQUIRED)
  execute_process(
    COMMAND ${PSX_STUDIO_QTPATHS} --query QT_INSTALL_PLUGINS
    OUTPUT_VARIABLE PSX_STUDIO_QT_PLUGINS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployStudio.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/DeployStudio.cmake
    @ONLY
  )
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/DeployStudio.cmake COMPONENT Studio)
endif()
