#include "GbaSupport.h"

#include "PipelineSupport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace psxstudio {

namespace {

constexpr qint64 kGbaMaxRomSize = 32ll * 1024ll * 1024ll;
constexpr auto kSdl2SourceUrl =
  "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz";
constexpr auto kSdl2SourceSha256 =
  "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165";
constexpr auto kLinuxSdl2WheelUrl =
  "https://files.pythonhosted.org/packages/2b/1c/91fb667c3968b420de5dd76e840259b44b876b6fc84c5889151633fcc06e/"
  "pysdl2_dll-2.32.10-py2.py3-none-manylinux_2_28_x86_64.whl";
constexpr auto kLinuxSdl2WheelSha256 =
  "1e6139e3ce832d95d8376b77f4c9288bef34d6b131f844f2a5fcc8873ce8e6da";

quint32 readLe32(const QByteArray& bytes, qsizetype offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint32>(p[0]) |
         (static_cast<quint32>(p[1]) << 8u) |
         (static_cast<quint32>(p[2]) << 16u) |
         (static_cast<quint32>(p[3]) << 24u);
}

QString trimmedAscii(const QByteArray& bytes, qsizetype offset, qsizetype size) {
  QByteArray text = bytes.mid(offset, size);
  while (!text.isEmpty() && (text.endsWith('\0') || text.endsWith(' '))) text.chop(1);
  for (char& ch : text) {
    const uchar c = static_cast<uchar>(ch);
    if (c < 0x20u || c > 0x7eu) ch = '?';
  }
  return QString::fromLatin1(text);
}

QString detectSaveType(const QByteArray& bytes) {
  struct Signature { const char* value; const char* name; };
  static constexpr Signature signatures[]{
    { "EEPROM_V", "eeprom" },
    { "SRAM_V", "sram" },
    { "FLASH1M_V", "flash1m" },
    { "FLASH512_V", "flash512" },
    { "FLASH_V", "flash512" },
  };
  for (qsizetype offset = 0; offset + 16 < bytes.size(); offset += 4) {
    for (const auto& signature : signatures) {
      const QByteArrayView needle(signature.value, static_cast<qsizetype>(qstrlen(signature.value)));
      if (QByteArrayView(bytes).sliced(offset).startsWith(needle)) {
        return QString::fromLatin1(signature.name);
      }
    }
  }
  return QStringLiteral("unknown");
}

QString cppQuoted(QString value) {
  value.replace('\\', QStringLiteral("\\\\"));
  value.replace('"', QStringLiteral("\\\""));
  value.replace('\n', QStringLiteral("\\n"));
  value.replace('\r', QStringLiteral("\\r"));
  return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString gbaHex32(quint32 value) {
  return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

} // namespace

bool inspectGbaRom(const QString& path, GbaDescription& description, QString& error) {
  description = {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the GBA ROM: %1").arg(file.errorString());
    return false;
  }
  const qint64 size = file.size();
  if (size < 0xC0) {
    error = QStringLiteral("The selected file is smaller than the 192-byte GBA cartridge header.");
    return false;
  }
  if (size > kGbaMaxRomSize) {
    error = QStringLiteral("The selected file exceeds the 32 MiB GBA cartridge address space.");
    return false;
  }
  const QByteArray bytes = file.readAll();
  if (bytes.size() != size) {
    error = QStringLiteral("Could not read the complete GBA ROM: %1").arg(file.errorString());
    return false;
  }

  description.romSize = size;
  description.entryWord = readLe32(bytes, 0);
  const quint32 condition = description.entryWord >> 28u;
  const quint32 opcode = (description.entryWord >> 25u) & 7u;
  description.headerBranchValid = condition == 0xEu && opcode == 0x5u;
  if (description.headerBranchValid) {
    qint32 displacement = static_cast<qint32>(description.entryWord & 0x00ffffffu);
    if ((displacement & 0x00800000) != 0) displacement |= static_cast<qint32>(0xff000000u);
    const qint64 target = static_cast<qint64>(0x08000008u) +
                          static_cast<qint64>(displacement) * 4ll;
    description.entryTarget = static_cast<quint32>(target);
  }
  description.title = trimmedAscii(bytes, 0xA0, 12);
  description.gameCode = trimmedAscii(bytes, 0xAC, 4);
  description.makerCode = trimmedAscii(bytes, 0xB0, 2);
  quint32 sum = 0x19u;
  for (qsizetype offset = 0xA0; offset <= 0xBC; ++offset)
    sum += static_cast<uchar>(bytes.at(offset));
  const quint8 expected = static_cast<quint8>((-static_cast<qint32>(sum)) & 0xff);
  description.complementValid = expected == static_cast<quint8>(bytes.at(0xBD));
  description.saveType = detectSaveType(bytes);

  QStringList warnings;
  if (!description.headerBranchValid)
    warnings.append(QStringLiteral("header entry is not an unconditional ARM branch"));
  if (static_cast<uchar>(bytes.at(0xB2)) != 0x96u)
    warnings.append(QStringLiteral("fixed header byte 0xB2 is not 0x96"));
  if (static_cast<uchar>(bytes.at(0xB3)) != 0u)
    warnings.append(QStringLiteral("main unit code is not GBA (0)"));
  if (!description.complementValid)
    warnings.append(QStringLiteral("header complement byte does not validate"));
  if (!warnings.isEmpty()) {
    description.warning = QStringLiteral("The ROM is within the GBA cartridge size range, but %1.")
                            .arg(warnings.join(QStringLiteral(", ")));
  }
  if (description.title.isEmpty()) description.title = QFileInfo(path).completeBaseName();
  return true;
}

bool scanGbaDirectory(const QString& directory,
                      QList<GbaCatalogEntry>& entries,
                      QStringList& warnings,
                      QString& error) {
  entries.clear();
  warnings.clear();
  if (!QFileInfo(directory).isDir()) {
    error = QStringLiteral("The selected GBA game directory does not exist.");
    return false;
  }
  QDirIterator iterator(directory, { QStringLiteral("*.gba"), QStringLiteral("*.GBA") },
                        QDir::Files, QDirIterator::Subdirectories);
  QSet<QString> seen;
  while (iterator.hasNext()) {
    const QString path = iterator.next();
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString key = (canonical.isEmpty() ? QFileInfo(path).absoluteFilePath() : canonical).toLower();
    if (seen.contains(key)) continue;
    seen.insert(key);
    GbaDescription description;
    QString inspectError;
    if (!inspectGbaRom(path, description, inspectError)) {
      warnings.append(QStringLiteral("%1: %2").arg(path, inspectError));
      continue;
    }
    if (!description.warning.isEmpty())
      warnings.append(QStringLiteral("%1: %2").arg(path, description.warning));
    entries.append({ path, description.title + QStringLiteral(" Recompiled"), description.gameCode });
  }
  std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.sourcePath.compare(rhs.sourcePath, Qt::CaseInsensitive) < 0;
  });
  if (entries.isEmpty()) {
    error = QStringLiteral("No readable .gba cartridge images were found.");
    return false;
  }
  return true;
}

QString sha1File(const QString& path, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not checksum %1: %2").arg(path, file.errorString());
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha1);
  if (!hash.addData(&file)) {
    error = QStringLiteral("Could not checksum %1: %2").arg(path, file.errorString());
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

QString generatedGbaMainCpp(const QString& bundleName,
                            const QString& romSha1,
                            quint32 romCrc32) {
  return QStringLiteral(R"CPP(#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "runtime.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path executable_path() {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) return fs::current_path();
  buffer.resize(size);
  return fs::path(buffer);
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) return fs::current_path();
  return fs::weakly_canonical(fs::path(buffer.c_str()));
#else
  std::string buffer(PATH_MAX, '\0');
  const ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (size <= 0) return fs::current_path();
  buffer.resize(static_cast<std::size_t>(size));
  return fs::path(buffer);
#endif
}

fs::path resource_dir(const fs::path& executable) {
#if defined(__APPLE__)
  return executable.parent_path().parent_path() / "Resources";
#else
  return executable.parent_path();
#endif
}

fs::path user_data_dir() {
#if defined(_WIN32)
  if (const wchar_t* appdata = _wgetenv(L"APPDATA"))
    return fs::path(appdata) / "PSXRecomp" / "GBA" / %1;
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / "Library" / "Application Support" / "PSXRecomp" / "GBA" / %1;
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME"))
    return fs::path(xdg) / "PSXRecomp" / "GBA" / %1;
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / ".local" / "share" / "PSXRecomp" / "GBA" / %1;
#endif
  return fs::temp_directory_path() / "PSXRecomp" / "GBA" / %1;
}

bool has_option(const std::vector<std::string>& args, const char* name) {
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == name) return true;
  }
  return false;
}

int run(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc) + 6);
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");

  const fs::path executable = executable_path();
  const fs::path resources = resource_dir(executable);
  const fs::path data = user_data_dir();
  std::error_code ec;
  fs::create_directories(data, ec);
  if (!ec) fs::current_path(data, ec);

  if (!has_option(args, "--config")) {
    args.emplace_back("--config");
    args.emplace_back((resources / "game.toml").string());
  }
  if (!has_option(args, "--save") && !has_option(args, "--save-path")) {
    args.emplace_back("--save-path");
    args.emplace_back((data / "game.sav").string());
  }

  std::vector<char*> raw;
  raw.reserve(args.size());
  for (auto& value : args) raw.push_back(value.data());

  gbarecomp::RunOptions options;
  options.builtin_game_name = %2;
  options.builtin_rom_sha1 = %3;
  options.builtin_rom_crc32 = %4u;
  return gbarecomp::run_game(static_cast<int>(raw.size()), raw.data(), options);
}

} // namespace

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!wide) return 1;
  std::vector<std::string> utf8;
  utf8.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    std::string value(bytes > 0 ? static_cast<std::size_t>(bytes) : 0, '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), bytes, nullptr, nullptr);
    if (!value.empty() && value.back() == '\0') value.pop_back();
    utf8.push_back(std::move(value));
  }
  LocalFree(wide);
  std::vector<char*> raw;
  raw.reserve(utf8.size());
  for (auto& value : utf8) raw.push_back(value.data());
  return run(static_cast<int>(raw.size()), raw.data());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
)CPP")
    .arg(cppQuoted(sanitizedFileStem(bundleName)), cppQuoted(bundleName),
         cppQuoted(romSha1), QString::number(romCrc32));
}

QString generatedGbaGameToml(const PipelineRequest& request,
                             const GbaDescription& game,
                             const QString& romSha1,
                             quint32 romCrc32,
                             const QString& biosSha1,
                             quint32 biosCrc32,
                             bool biosHle) {
  QString text;
  text += QStringLiteral("[game]\n");
  text += QStringLiteral("name = %1\n").arg(tomlQuoted(request.windowTitle));
  text += QStringLiteral("short_name = %1\n").arg(tomlQuoted(sanitizedFileStem(request.windowTitle)));
  text += QStringLiteral("game_code = %1\n\n").arg(tomlQuoted(game.gameCode));
  text += QStringLiteral("[bios]\npath = \"bios/gba_bios.bin\"\n");
  text += QStringLiteral("sha1 = %1\ncrc32 = %2\nhle = %3\nhle_keep_intro = false\n\n")
            .arg(tomlQuoted(biosSha1), tomlQuoted(gbaHex32(biosCrc32)),
                 biosHle ? QStringLiteral("true") : QStringLiteral("false"));
  text += QStringLiteral("[rom]\npath = \"game/game.gba\"\n");
  text += QStringLiteral("sha1 = %1\ncrc32 = %2\n\n")
            .arg(tomlQuoted(romSha1), tomlQuoted(gbaHex32(romCrc32)));
  text += QStringLiteral("[recompiler]\nentry_point = %1\nout_dir = \"generated\"\nstrict = false\n\n")
            .arg(tomlQuoted(gbaHex32(game.entryTarget)));
  text += QStringLiteral("[save]\ntype = %1\n\n").arg(tomlQuoted(game.saveType));
  text += QStringLiteral("[runtime]\nwindow_title = %1\ndebug_port = 4371\n\n")
            .arg(tomlQuoted(request.windowTitle));
  text += QStringLiteral("[video]\nscreen = \"frontlit\"\nresize_view = false\n\n");
  text += QStringLiteral("[audio]\nshadow = false\n");
  return text;
}

QString generatedGbaInfoPlist(const QString& bundleName, const QString& bundleId) {
  return QStringLiteral(R"PLIST(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>%1</string>
  <key>CFBundleDisplayName</key><string>%1</string>
  <key>CFBundleExecutable</key><string>%1</string>
  <key>CFBundleIdentifier</key><string>%2</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleIconFile</key><string>AppIcon.icns</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
)PLIST").arg(bundleName.toHtmlEscaped(), bundleId.toHtmlEscaped());
}

QString generatedGbaProjectCMake(const PipelineRequest& request,
                                 const GbaDescription& game,
                                 const QString& bundleName,
                                 const QString& bundleId) {
  Q_UNUSED(game);
  const bool windows = request.targetPlatform == TargetPlatform::Windows;
  const bool linux = request.targetPlatform == TargetPlatform::Linux;
  const bool macos = request.targetPlatform == TargetPlatform::MacOS;
  QString cmake;
  cmake += QStringLiteral("cmake_minimum_required(VERSION 3.20)\n");
  cmake += windows ? QStringLiteral("project(GeneratedGbaApp C CXX RC)\n")
                   : QStringLiteral("project(GeneratedGbaApp C CXX)\n");
  cmake += QStringLiteral("set(CMAKE_C_STANDARD 11)\nset(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n");
  cmake += QStringLiteral("set(PSXRECOMP_DEPENDENCY_CACHE \"${CMAKE_BINARY_DIR}/dependencies\" CACHE PATH \"Shared dependency cache\")\n");
  cmake += QStringLiteral("include(FetchContent)\nset(FETCHCONTENT_QUIET OFF)\nset(FETCHCONTENT_BASE_DIR \"${PSXRECOMP_DEPENDENCY_CACHE}\" CACHE PATH \"\" FORCE)\n");
  if (windows) {
    cmake += QStringLiteral("if(MSVC)\n");
    cmake += QStringLiteral("  FetchContent_Declare(SDL2Source URL %1 URL_HASH SHA256=%2 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)\n")
               .arg(cmakeQuoted(QString::fromLatin1(kSdl2SourceUrl)), QString::fromLatin1(kSdl2SourceSha256));
    cmake += QStringLiteral("  set(SDL_SHARED OFF CACHE BOOL \"\" FORCE)\n  set(SDL_STATIC ON CACHE BOOL \"\" FORCE)\n  set(SDL_TEST OFF CACHE BOOL \"\" FORCE)\n  set(SDL2_DISABLE_INSTALL ON CACHE BOOL \"\" FORCE)\n");
    cmake += QStringLiteral("  set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>\")\n  FetchContent_MakeAvailable(SDL2Source)\n");
    cmake += QStringLiteral("  set(_GBA_SDL_INCLUDE \"${sdl2source_SOURCE_DIR}/include\")\n  set(_GBA_SDL_LIB SDL2::SDL2-static)\n");
    cmake += QStringLiteral("else()\n");
    cmake += QStringLiteral("  FetchContent_Declare(SDL2Mingw URL \"https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz\" URL_HASH SHA256=83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)\n");
    cmake += QStringLiteral("  FetchContent_GetProperties(SDL2Mingw)\n  if(NOT sdl2mingw_POPULATED)\n    FetchContent_Populate(SDL2Mingw)\n  endif()\n");
    cmake += QStringLiteral("  set(_GBA_SDL_ROOT \"${sdl2mingw_SOURCE_DIR}/x86_64-w64-mingw32\")\n  find_package(SDL2 CONFIG REQUIRED PATHS \"${_GBA_SDL_ROOT}/lib/cmake/SDL2\" NO_DEFAULT_PATH)\n");
    cmake += QStringLiteral("  set(_GBA_SDL_INCLUDE \"${_GBA_SDL_ROOT}/include;${_GBA_SDL_ROOT}/include/SDL2\")\n  set(_GBA_SDL_LIB SDL2::SDL2-static)\nendif()\n");
  } else if (linux) {
    cmake += QStringLiteral("FetchContent_Declare(SDL2Source URL %1 URL_HASH SHA256=%2 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)\n")
               .arg(cmakeQuoted(QString::fromLatin1(kSdl2SourceUrl)), QString::fromLatin1(kSdl2SourceSha256));
    cmake += QStringLiteral("FetchContent_GetProperties(SDL2Source)\nif(NOT sdl2source_POPULATED)\n  FetchContent_Populate(SDL2Source)\nendif()\n");
    cmake += QStringLiteral("FetchContent_Declare(SDL2Linux URL %1 URL_HASH SHA256=%2 DOWNLOAD_NAME \"pysdl2_dll-2.32.10-manylinux_2_28_x86_64.zip\" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)\n")
               .arg(cmakeQuoted(QString::fromLatin1(kLinuxSdl2WheelUrl)), QString::fromLatin1(kLinuxSdl2WheelSha256));
    cmake += QStringLiteral("FetchContent_GetProperties(SDL2Linux)\nif(NOT sdl2linux_POPULATED)\n  FetchContent_Populate(SDL2Linux)\nendif()\n");
    cmake += QStringLiteral("set(_GBA_SDL_LIBRARY \"${sdl2linux_SOURCE_DIR}/sdl2dll/dll/libSDL2-2.0.so\")\nif(NOT EXISTS \"${_GBA_SDL_LIBRARY}\")\n  message(FATAL_ERROR \"Pinned Linux SDL2 library missing\")\nendif()\n");
    cmake += QStringLiteral("add_library(GbaStudioSDL2 SHARED IMPORTED GLOBAL)\nset_target_properties(GbaStudioSDL2 PROPERTIES IMPORTED_LOCATION \"${_GBA_SDL_LIBRARY}\" INTERFACE_INCLUDE_DIRECTORIES \"${sdl2source_SOURCE_DIR}/include\")\n");
    cmake += QStringLiteral("set(_GBA_SDL_INCLUDE \"${sdl2source_SOURCE_DIR}/include\")\nset(_GBA_SDL_LIB GbaStudioSDL2)\nset(CMAKE_BUILD_WITH_INSTALL_RPATH ON)\nset(CMAKE_INSTALL_RPATH \"$ORIGIN\")\n");
  } else {
    cmake += QStringLiteral("find_package(PkgConfig REQUIRED)\npkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)\nset(_GBA_SDL_INCLUDE \"${SDL2_INCLUDE_DIRS}\")\nset(_GBA_SDL_LIB PkgConfig::SDL2)\n");
    cmake += QStringLiteral("set(CMAKE_OSX_DEPLOYMENT_TARGET \"14.0\" CACHE STRING \"\" FORCE)\nset(CMAKE_MACOSX_RPATH ON)\nset(CMAKE_BUILD_WITH_INSTALL_RPATH ON)\nset(CMAKE_INSTALL_RPATH \"@executable_path/../Frameworks\")\n");
  }
  cmake += QStringLiteral("set(GBARECOMP_COMPILER_CACHE OFF CACHE STRING \"\" FORCE)\nset(GBARECOMP_BUILD_ORACLE OFF CACHE BOOL \"\" FORCE)\nset(GBARECOMP_SDL2_INCLUDE_DIRS \"${_GBA_SDL_INCLUDE}\" CACHE STRING \"\" FORCE)\nset(GBARECOMP_SDL2_LIBRARIES \"${_GBA_SDL_LIB}\" CACHE STRING \"\" FORCE)\nadd_subdirectory(gba++ gbarecomp_build EXCLUDE_FROM_ALL)\n");
  cmake += QStringLiteral("file(GLOB _GBA_SHARDS CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled_[0-9][0-9][0-9].cpp\")\nlist(LENGTH _GBA_SHARDS _GBA_SHARD_COUNT)\nif(_GBA_SHARD_COUNT LESS 2)\n  message(FATAL_ERROR \"Expected at least two generated GBA recompilation shards\")\nendif()\n");
  cmake += windows
    ? QStringLiteral("add_executable(psx-runtime WIN32 src/main.cpp ${_GBA_SHARDS} generated/dispatch_table.cpp)\n")
    : macos
      ? QStringLiteral("add_executable(psx-runtime MACOSX_BUNDLE src/main.cpp ${_GBA_SHARDS} generated/dispatch_table.cpp)\n")
      : QStringLiteral("add_executable(psx-runtime src/main.cpp ${_GBA_SHARDS} generated/dispatch_table.cpp)\n");
  cmake += QStringLiteral("if(EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp\")\n  target_sources(psx-runtime PRIVATE generated/symbol_map.cpp)\nendif()\n");
  cmake += QStringLiteral("target_include_directories(psx-runtime PRIVATE gba++/src/armv4t gba++/src/gba gba++/src/runtime gba++/src/debug generated)\n");
  cmake += QStringLiteral("target_compile_definitions(psx-runtime PRIVATE GBARECOMP_WINDOW_TITLE=%1 GBARECOMP_DEFAULT_DEBUG_PORT=4371)\n")
             .arg(cmakeQuoted(bundleName));
  cmake += QStringLiteral("if(MINGW)\n  target_link_libraries(psx-runtime PRIVATE -Wl,--start-group gbarecomp_runtime gbarecomp_debug gbarecomp_gba gbarecomp_armv4t gbarecomp_recompile_core gbarecomp_heal_gate -Wl,--end-group)\nelse()\n  target_link_libraries(psx-runtime PRIVATE gbarecomp_runtime gbarecomp_debug gbarecomp_gba gbarecomp_armv4t gbarecomp_recompile_core gbarecomp_heal_gate)\nendif()\n");
  cmake += QStringLiteral("set(_GBA_RESOURCES \"${CMAKE_CURRENT_SOURCE_DIR}/package_resources\")\nset(_GBA_STEGANOS_PACKAGE_DIR \"${CMAKE_BINARY_DIR}/steganos-package/psx-runtime/$<CONFIG>\")\n");
  if (windows) {
    cmake += QStringLiteral("target_sources(psx-runtime PRIVATE app.rc)\nset_target_properties(psx-runtime PROPERTIES OUTPUT_NAME %1)\n").arg(cmakeQuoted(bundleName));
    cmake += QStringLiteral("if(MINGW)\n  target_link_options(psx-runtime PRIVATE -static-libgcc -static-libstdc++ -Wl,--stack,16777216)\nelseif(MSVC)\n  target_link_options(psx-runtime PRIVATE /STACK:16777216)\nendif()\n");
    cmake += QStringLiteral("install(TARGETS psx-runtime RUNTIME DESTINATION .)\ninstall(DIRECTORY \"${_GBA_RESOURCES}/\" DESTINATION .)\ninstall(FILES game.manifest.json DESTINATION .)\nadd_custom_command(TARGET psx-runtime POST_BUILD COMMAND ${CMAKE_COMMAND} -E rm -rf \"${_GBA_STEGANOS_PACKAGE_DIR}\" COMMAND ${CMAKE_COMMAND} -E make_directory \"${_GBA_STEGANOS_PACKAGE_DIR}\" COMMAND ${CMAKE_COMMAND} -E copy_if_different \"$<TARGET_FILE:psx-runtime>\" \"${_GBA_STEGANOS_PACKAGE_DIR}/%1.exe\" COMMAND ${CMAKE_COMMAND} -E copy_directory \"${_GBA_RESOURCES}\" \"${_GBA_STEGANOS_PACKAGE_DIR}\" COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/game.manifest.json\" \"${_GBA_STEGANOS_PACKAGE_DIR}/game.manifest.json\" VERBATIM)\n").arg(bundleName);
  } else if (linux) {
    cmake += QStringLiteral("set_target_properties(psx-runtime PROPERTIES OUTPUT_NAME %1 BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH \"$ORIGIN\")\n").arg(cmakeQuoted(bundleName));
    cmake += QStringLiteral("target_link_options(psx-runtime PRIVATE -static-libgcc -static-libstdc++ -Wl,-z,origin)\ninstall(TARGETS psx-runtime RUNTIME DESTINATION .)\ninstall(FILES \"${_GBA_SDL_LIBRARY}\" DESTINATION . RENAME libSDL2-2.0.so.0)\ninstall(DIRECTORY \"${_GBA_RESOURCES}/\" DESTINATION .)\ninstall(FILES AppIcon.png game.manifest.json DESTINATION .)\nadd_custom_command(TARGET psx-runtime POST_BUILD COMMAND ${CMAKE_COMMAND} -E rm -rf \"${_GBA_STEGANOS_PACKAGE_DIR}\" COMMAND ${CMAKE_COMMAND} -E make_directory \"${_GBA_STEGANOS_PACKAGE_DIR}\" COMMAND ${CMAKE_COMMAND} -E copy_if_different \"$<TARGET_FILE:psx-runtime>\" \"${_GBA_STEGANOS_PACKAGE_DIR}/%1\" COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${_GBA_SDL_LIBRARY}\" \"${_GBA_STEGANOS_PACKAGE_DIR}/libSDL2-2.0.so.0\" COMMAND ${CMAKE_COMMAND} -E copy_directory \"${_GBA_RESOURCES}\" \"${_GBA_STEGANOS_PACKAGE_DIR}\" COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png\" \"${_GBA_STEGANOS_PACKAGE_DIR}/AppIcon.png\" COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/game.manifest.json\" \"${_GBA_STEGANOS_PACKAGE_DIR}/game.manifest.json\" VERBATIM)\n").arg(bundleName);
  } else {
    cmake += QStringLiteral("set_source_files_properties(AppIcon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)\ntarget_sources(psx-runtime PRIVATE AppIcon.icns)\n");
    cmake += QStringLiteral("set_target_properties(psx-runtime PROPERTIES OUTPUT_NAME %1 MACOSX_BUNDLE TRUE MACOSX_BUNDLE_INFO_PLIST \"${CMAKE_CURRENT_SOURCE_DIR}/Info.plist\" MACOSX_BUNDLE_GUI_IDENTIFIER %2 MACOSX_BUNDLE_BUNDLE_NAME %1 MACOSX_BUNDLE_ICON_FILE AppIcon.icns XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED OFF XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY \"\")\n")
               .arg(cmakeQuoted(bundleName), cmakeQuoted(bundleId));
    cmake += QStringLiteral("add_custom_command(TARGET psx-runtime POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory \"${_GBA_RESOURCES}\" \"$<TARGET_BUNDLE_CONTENT_DIR:psx-runtime>/Resources\" COMMAND ${CMAKE_COMMAND} -E rm -rf \"${_GBA_STEGANOS_PACKAGE_DIR}\" COMMAND ${CMAKE_COMMAND} -E make_directory \"${_GBA_STEGANOS_PACKAGE_DIR}\" COMMAND ${CMAKE_COMMAND} -E copy_directory \"$<TARGET_BUNDLE_DIR:psx-runtime>\" \"${_GBA_STEGANOS_PACKAGE_DIR}/%1.app\" COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/game.manifest.json\" \"${_GBA_STEGANOS_PACKAGE_DIR}/game.manifest.json\" VERBATIM)\ninstall(TARGETS psx-runtime BUNDLE DESTINATION .)\ninstall(FILES game.manifest.json DESTINATION .)\n").arg(bundleName);
  }
  return cmake;
}

} // namespace psxstudio
