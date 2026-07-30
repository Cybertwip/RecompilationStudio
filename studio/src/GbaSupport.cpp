#include "GbaSupport.h"

#include "PipelineSupport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

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

// TOML's hex-integer prefix is lowercase `0x`; toml++ rejects `0X`, and
// gba_recompile aborts on a configuration it cannot parse. gbaHex32 uppercases
// the prefix along with the digits, so it is only safe inside a quoted string.
QString tomlHexLiteral(quint32 value) {
  return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper()
    .replace(0, 2, QStringLiteral("0x"));
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
#if defined(_WIN32)
  _putenv_s("GBARECOMP_ASSET_CACHE_DIR", data.string().c_str());
#else
  setenv("GBARECOMP_ASSET_CACHE_DIR", data.string().c_str(), 1);
#endif

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
  // Only keys the gbarecomp runtime actually reads are emitted. It ignores
  // unknown keys silently, so anything inert here would read as configuration
  // that does something. [rom].path and [bios].path resolve against this
  // file's own directory.
  QString text;
  text += QStringLiteral("# %1 (%2) — generated by PSXRecomp Studio.\n")
            .arg(request.windowTitle, game.gameCode);
  text += QStringLiteral("# The cartridge and BIOS are statically recompiled; the images below are\n"
                         "# the identity the runtime hash-verifies them against before booting.\n\n");
  text += QStringLiteral("[game]\n");
  text += QStringLiteral("short_name = %1\n\n")
            .arg(tomlQuoted(sanitizedFileStem(request.windowTitle)));
  text += QStringLiteral("[bios]\npath = \"bios/gba_bios.bin\"\n");
  text += QStringLiteral("sha1 = %1\ncrc32 = %2\nhle = %3\nhle_keep_intro = false\n\n")
            .arg(tomlQuoted(biosSha1), tomlQuoted(gbaHex32(biosCrc32)),
                 biosHle ? QStringLiteral("true") : QStringLiteral("false"));
  text += QStringLiteral("[rom]\npath = \"game/game.gba\"\n");
  text += QStringLiteral("sha1 = %1\ncrc32 = %2\n\n")
            .arg(tomlQuoted(romSha1), tomlQuoted(gbaHex32(romCrc32)));
  text += QStringLiteral("[save]\ntype = %1\n\n").arg(tomlQuoted(game.saveType));
  text += QStringLiteral("[video]\nscreen = \"frontlit\"\nresize_view = false\n\n");
  text += QStringLiteral("[audio]\nshadow = false\n");
  return text;
}

QString generatedGbaRecompilerToml(const PipelineRequest& request,
                                   const GbaDescription& game,
                                   const QString& romSha1) {
  // [identity].sha1 makes gba_recompile abort outright if it is ever pointed at
  // a different image than the one this configuration was derived from.
  QString text;
  text += QStringLiteral("# gba_recompile configuration for %1.\n")
            .arg(request.windowTitle);
  text += QStringLiteral("# Function discovery is seeded from Ghidra through the separate\n"
                         "# seeds/ghidra_funcs.tsv symbol table, so this file stays stable\n"
                         "# across recompilation passes.\n\n");
  text += QStringLiteral("[program]\n");
  text += QStringLiteral("name = %1\n").arg(tomlQuoted(request.windowTitle));
  text += QStringLiteral("id = %1\n").arg(tomlQuoted(game.gameCode));
  text += QStringLiteral("load_address = %1\n").arg(tomlHexLiteral(0x08000000u));
  text += QStringLiteral("size = %1\n").arg(game.romSize);
  text += QStringLiteral("entry_pc = %1\n").arg(tomlHexLiteral(game.entryTarget));
  text += QStringLiteral("codegen_shards = 0  # 0 selects the tool's automatic sharding\n\n");
  text += QStringLiteral("[identity]\n");
  text += QStringLiteral("sha1 = %1\n").arg(tomlQuoted(romSha1));
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

namespace {

/* The Virtua ARM form of the package.
 *
 * The desktop project below builds gbarecomp's own host runtime — a window, an
 * audio device, a TOML loader, a Stage-2 overlay recompiler that shells out to
 * a compiler. None of that exists on an 845 MHz Cortex-A7 with seven character
 * devices, so this project does not try to cross-compile it. It hands the
 * recompiled cartridge to extra/gba-to-mvii, which compiles the *emulation*
 * half of gbarecomp — the same sources, not a port — against MVII's own
 * framebuffer, input and DAC.
 *
 * The guest is ARMv4T and the host is ARMv7-A, so this is still static
 * recompilation: the generated C++ is compiled for Cortex-A7 and runs natively.
 * Nothing here interprets that a desktop build would not, and a coverage miss
 * bridges through the same reference interpreter and is reported the same way. */
QString generatedGbaVirtuaProjectCMake(const QString& bundleName) {
  return QStringLiteral(R"CMAKE(cmake_minimum_required(VERSION 3.20)
project(GeneratedGbaVirtuaPackage CXX C ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Configure with the Virtua ARM toolchain; there is no host form of this
# package. gba-to-mvii refuses a non-ARM CMAKE_SYSTEM_PROCESSOR itself, but it
# is worth saying here, where the mistake is actually made.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
  message(FATAL_ERROR
    "This package targets MVII/Virtua. Configure with "
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/framework/extra/virtua/CMake/VirtuaArmToolchain.cmake "
    "-DPOWERENGINE_ROOT=<PowerEngine> -DVIRTUA_LLVM_ROOT=<compiler bundle>")
endif()

set(GBA_APP_NAME %1)

# gba_recompile emitted these during export. Their presence IS the proof that
# the cartridge was translated; an empty generated/ is a hard error rather than
# a silent fall back to interpretation.
file(GLOB GBA_RECOMPILED_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled_*.cpp")
if(NOT GBA_RECOMPILED_SOURCES)
  message(FATAL_ERROR
    "generated/ contains no recompiled translation units. "
    "Re-export from PSXRecomp Studio; this project never recompiles at build time.")
endif()
foreach(_required dispatch_table.cpp recompiled.h)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/${_required}")
    message(FATAL_ERROR "generated/${_required} is missing; the recompilation is incomplete.")
  endif()
endforeach()
list(APPEND GBA_RECOMPILED_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/generated/dispatch_table.cpp")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
  list(APPEND GBA_RECOMPILED_SOURCES
       "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
endif()
list(LENGTH GBA_RECOMPILED_SOURCES _gba_unit_count)
message(STATUS "gbarecomp: linking ${_gba_unit_count} recompiled cartridge units for Cortex-A7")

# gba-to-mvii links the recompiled BIOS out of the gbarecomp copy this export
# carries. The export wrote it there; refuse the placeholder dispatch stub,
# which would mean the BIOS was never translated.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp/src/runtime/generated_bios/bios_recompiled.cpp")
  message(FATAL_ERROR
    "The recompiled GBA BIOS is missing. gba-to-mvii would fall back to its "
    "placeholder dispatch stub, and the BIOS must never be stubbed.")
endif()

# Plain variables, read by framework/extra/gba-to-mvii/CMakeLists.txt before it
# defines anything. The cartridge goes in as GBA_MVII_NATIVE_SOURCES, which is
# also what switches that project into its AOT configuration.
set(_GBA_SRC "${CMAKE_CURRENT_SOURCE_DIR}")
set(_GBA_STAGE "${CMAKE_BINARY_DIR}/steganos-package/gba-runtime/$<CONFIG>")
set(GBARECOMP_ROOT "${_GBA_SRC}/gbarecomp")
set(GBA_MVII_NATIVE_SOURCES ${GBA_RECOMPILED_SOURCES})
set(GBA_MVII_OUTPUT_NAME "${GBA_APP_NAME}")
set(GBA_MVII_ROM "${_GBA_SRC}/package_inputs/game.gba")
set(GBA_MVII_BIOS "${_GBA_SRC}/package_inputs/gba_bios.bin")
set(GBA_MVII_STAGE_DIR "${_GBA_STAGE}")
add_subdirectory(framework/extra/gba-to-mvii "${CMAKE_BINARY_DIR}/gba-to-mvii")

# gba-to-mvii stages the .virtua, game.gba and gba_bios.bin side by side — the
# runtime resolves both assets as dirname(argv[0])/<name>, so there is no
# resources directory and no game.toml in the package. The rest of what a
# Steganos package carries is added here.
add_custom_target(gba-runtime ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GBA_SRC}/AppIcon.png" "${_GBA_STAGE}/AppIcon.png"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GBA_SRC}/game.manifest.json" "${_GBA_STAGE}/game.manifest.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GBA_SRC}/PSXRecomp-Proof.zip" "${_GBA_STAGE}/PSXRecomp-Proof.zip"
  DEPENDS "${_GBA_SRC}/AppIcon.png"
          "${_GBA_SRC}/game.manifest.json"
          "${_GBA_SRC}/PSXRecomp-Proof.zip"
  COMMENT "Staging the statically recompiled GBA Virtua package"
  VERBATIM)
add_dependencies(gba-runtime gba-to-mvii-stage)
)CMAKE")
    .arg(cmakeQuoted(bundleName));
}

}  // namespace

QString generatedGbaProjectCMake(const PipelineRequest& request,
                                 const GbaDescription& game,
                                 const QString& bundleName,
                                 const QString& bundleId) {
  Q_UNUSED(game);
  Q_UNUSED(bundleId);
  if (request.targetPlatform == TargetPlatform::VirtuaArm)
    return generatedGbaVirtuaProjectCMake(bundleName);
  // The cartridge and the BIOS were statically recompiled during export, so
  // this project compiles checked-in C++ and never needs the ROM to build.
  return QStringLiteral(R"CMAKE(cmake_minimum_required(VERSION 3.20)
project(GeneratedGbaRecompPackage CXX C ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# A title is a display name, not an identifier: it may hold spaces and
# punctuation that CMP0037 rejects in a target name. So the executable target
# carries a fixed id and the title rides on OUTPUT_NAME and the bundle path,
# which is also what the psx-runtime package does.
set(GBA_APP_NAME %1)
set(GBA_APP_TARGET gba-runtime-app)

# gba_recompile emitted these during export. Their presence IS the proof that
# the cartridge was translated; an empty generated/ is a hard error rather than
# a silent fall back to interpretation.
file(GLOB GBA_RECOMPILED_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/generated/recompiled_*.cpp")
if(NOT GBA_RECOMPILED_SOURCES)
  message(FATAL_ERROR
    "generated/ contains no recompiled translation units. "
    "Re-export from PSXRecomp Studio; this project never recompiles at build time.")
endif()
foreach(_required dispatch_table.cpp recompiled.h)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/${_required}")
    message(FATAL_ERROR "generated/${_required} is missing; the recompilation is incomplete.")
  endif()
endforeach()
list(APPEND GBA_RECOMPILED_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/generated/dispatch_table.cpp")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
  list(APPEND GBA_RECOMPILED_SOURCES
       "${CMAKE_CURRENT_SOURCE_DIR}/generated/symbol_map.cpp")
endif()
list(LENGTH GBA_RECOMPILED_SOURCES _gba_unit_count)
message(STATUS "gbarecomp: linking ${_gba_unit_count} recompiled cartridge units")

# gbarecomp links its own recompiled BIOS from
# gbarecomp/src/runtime/generated_bios/. The export wrote it there; refuse the
# placeholder dispatch stub, which would mean the BIOS was never translated.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp/src/runtime/generated_bios/bios_recompiled.cpp")
  message(FATAL_ERROR
    "The recompiled GBA BIOS is missing. gbarecomp would fall back to its "
    "placeholder dispatch stub, and the BIOS must never be stubbed.")
endif()
add_subdirectory(gbarecomp "${CMAKE_BINARY_DIR}/gbarecomp")

add_executable(${GBA_APP_TARGET}
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
  ${GBA_RECOMPILED_SOURCES})
set_target_properties(${GBA_APP_TARGET} PROPERTIES OUTPUT_NAME "${GBA_APP_NAME}")
target_include_directories(${GBA_APP_TARGET} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/generated"
  "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp/src/armv4t")
target_link_gbarecomp_runtime_stack(${GBA_APP_TARGET})
if(WIN32)
  set_target_properties(${GBA_APP_TARGET} PROPERTIES WIN32_EXECUTABLE ON)
endif()

# Assemble the layout the runtime expects. game.toml resolves [rom].path and
# [bios].path against its own directory, so the assets travel beside it: next
# to the executable everywhere, and in Contents/Resources on macOS, which is
# what the generated main.cpp probes.
set(_GBA_STAGE "${CMAKE_BINARY_DIR}/steganos-package/gba-runtime/$<CONFIG>")
set(_GBA_SRC "${CMAKE_CURRENT_SOURCE_DIR}")
if(APPLE)
  set(_GBA_BUNDLE "${_GBA_STAGE}/${GBA_APP_NAME}.app")
  set(_GBA_EXE_DIR "${_GBA_BUNDLE}/Contents/MacOS")
  set(_GBA_RES_DIR "${_GBA_BUNDLE}/Contents/Resources")
  set(_GBA_ICON "${_GBA_SRC}/AppIcon.icns")
  set(_GBA_EXTRA_STAGE
    COMMAND ${CMAKE_COMMAND} -E copy "${_GBA_SRC}/Info.plist" "${_GBA_BUNDLE}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy "${_GBA_ICON}" "${_GBA_RES_DIR}/AppIcon.icns")
  set(_GBA_EXTRA_DEPENDS "${_GBA_SRC}/Info.plist" "${_GBA_ICON}")
else()
  set(_GBA_EXE_DIR "${_GBA_STAGE}")
  set(_GBA_RES_DIR "${_GBA_STAGE}")
  set(_GBA_ICON "${_GBA_SRC}/AppIcon.png")
  set(_GBA_EXTRA_STAGE
    COMMAND ${CMAKE_COMMAND} -E copy "${_GBA_ICON}" "${_GBA_RES_DIR}/AppIcon.png")
  set(_GBA_EXTRA_DEPENDS "${_GBA_ICON}")
endif()

set(_GBA_STAMP "${CMAKE_BINARY_DIR}/gba-package.stamp")
add_custom_command(OUTPUT "${_GBA_STAMP}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${_GBA_STAGE}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_GBA_EXE_DIR}" "${_GBA_RES_DIR}"
          "${_GBA_RES_DIR}/game" "${_GBA_RES_DIR}/bios"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${GBA_APP_TARGET}>" "${_GBA_EXE_DIR}/"
  COMMAND ${CMAKE_COMMAND} -E copy "${_GBA_SRC}/resources/game.toml" "${_GBA_RES_DIR}/game.toml"
  COMMAND ${CMAKE_COMMAND} -E copy "${_GBA_SRC}/package_inputs/game.gba" "${_GBA_RES_DIR}/game/game.gba"
  COMMAND ${CMAKE_COMMAND} -E copy "${_GBA_SRC}/package_inputs/gba_bios.bin" "${_GBA_RES_DIR}/bios/gba_bios.bin"
  COMMAND ${CMAKE_COMMAND} -E copy "${_GBA_SRC}/game.manifest.json" "${_GBA_RES_DIR}/game.manifest.json"
  COMMAND ${CMAKE_COMMAND} -E copy "${_GBA_SRC}/PSXRecomp-Proof.zip" "${_GBA_RES_DIR}/PSXRecomp-Proof.zip"
  ${_GBA_EXTRA_STAGE}
  COMMAND ${CMAKE_COMMAND} -E touch "${_GBA_STAMP}"
  DEPENDS ${GBA_APP_TARGET}
          "${_GBA_SRC}/resources/game.toml"
          "${_GBA_SRC}/package_inputs/game.gba"
          "${_GBA_SRC}/package_inputs/gba_bios.bin"
          "${_GBA_SRC}/game.manifest.json"
          "${_GBA_SRC}/PSXRecomp-Proof.zip"
          ${_GBA_EXTRA_DEPENDS}
  COMMENT "Staging the statically recompiled GBA package"
  VERBATIM)
add_custom_target(gba-runtime ALL DEPENDS "${_GBA_STAMP}")
)CMAKE")
    .arg(cmakeQuoted(bundleName));
}


namespace {

/* The text gba_recompile printed before the first space, i.e. the mnemonic. */
QString gbaMnemonic(const QString& disassembly) {
  const int space = disassembly.indexOf(QLatin1Char(' '));
  return space < 0 ? disassembly : disassembly.left(space);
}

}  // namespace

/* Reads both descriptions of the translation out of one pass over the shards.
 * `wantedSources` bounds the instruction map to the addresses Ghidra actually
 * cites: a fully covered cartridge emits well over a hundred megabytes of C++,
 * and keeping every instruction would be paying to remember addresses no rule
 * can ever ask about. A null `wantedSources` skips the instruction scan
 * outright, which is what a pass needs before Ghidra has run on it: the extents
 * are the input to that run, and nothing has cited an address yet. */
bool readGbaTranslation(const QString& generatedDir,
                        const QSet<quint32>* wantedSources,
                        QList<GbaTranslatedSpan>& spans,
                        GbaInstructionMap& instructions,
                        QString& error) {
  spans.clear();
  instructions.clear();
  const QStringList shards = QDir(generatedDir).entryList(
    { QStringLiteral("recompiled_*.cpp"), QStringLiteral("recompiled.cpp") },
    QDir::Files, QDir::Name);
  if (shards.isEmpty()) {
    error = QStringLiteral("gbarecomp produced no recompiled shard in %1.").arg(generatedDir);
    return false;
  }
  static const QRegularExpression headerPattern(QStringLiteral(
    R"(^/\* 0x([0-9A-Fa-f]{8})  mode=(arm|thumb)  end=0x([0-9A-Fa-f]{8}))"),
    QRegularExpression::MultilineOption);
  static const QRegularExpression instructionPattern(QStringLiteral(
    R"(^    /\* ([0-9A-Fa-f]{8})  [0-9a-f]{8} [AT] (.*) \*/$)"),
    QRegularExpression::MultilineOption);
  for (const QString& shard : shards) {
    QFile file(QDir(generatedDir).filePath(shard));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      error = QStringLiteral("Could not read the gbarecomp shard %1: %2")
                .arg(shard, file.errorString());
      return false;
    }
    const QString text = QString::fromUtf8(file.readAll());
    auto headers = headerPattern.globalMatch(text);
    while (headers.hasNext()) {
      const auto match = headers.next();
      GbaTranslatedSpan span;
      span.start = match.captured(1).toUInt(nullptr, 16);
      span.thumb = match.captured(2) == QStringLiteral("thumb");
      span.end = match.captured(3).toUInt(nullptr, 16);
      if (span.end <= span.start) continue;
      spans.append(span);
    }
    if (wantedSources == nullptr) continue;
    auto lowered = instructionPattern.globalMatch(text);
    while (lowered.hasNext()) {
      const auto match = lowered.next();
      const quint32 address = match.captured(1).toUInt(nullptr, 16);
      if (wantedSources->contains(address)) instructions.insert(address, match.captured(2));
    }
  }
  if (spans.isEmpty()) {
    error = QStringLiteral("No gbarecomp shard in %1 declares a translated function extent.")
              .arg(generatedDir);
    return false;
  }
  std::sort(spans.begin(), spans.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.start < rhs.start;
  });
  quint32 reach = 0;
  for (auto& span : spans) {
    reach = std::max(reach, span.end);
    span.reach = reach;
  }
  return true;
}

/* The span covering `address`, or nullptr. Extents can overlap — an alias entry
 * re-enters a function partway through — so this is an interval stab, not a
 * plain lower-bound: walk back from the last span that starts at or before the
 * address and stop as soon as no earlier span can still reach it. */
const GbaTranslatedSpan* gbaSpanContaining(const QList<GbaTranslatedSpan>& spans,
                                           quint32 address) {
  auto upper = std::upper_bound(spans.begin(), spans.end(), address,
                                [](quint32 value, const GbaTranslatedSpan& span) {
                                  return value < span.start;
                                });
  for (auto it = upper; it != spans.begin();) {
    --it;
    if (it->reach <= address) break;
    if (address >= it->start && address < it->end) return &*it;
  }
  return nullptr;
}

/* The extents file SeedGbaEntry.java reads. Every span is listed rather than
 * merged: the prescript needs each one as a disassembly start, because a
 * function reached only through a BX is inside its predecessor's merged range
 * and would otherwise never be started at. Identical triples are dropped —
 * gba_recompile emits one per alias entry into the same function — since the
 * prescript would skip the duplicate anyway and the file is read by a script,
 * not a stream parser. */
QString gbaExtentsTable(const QList<GbaTranslatedSpan>& spans) {
  QString text = QStringLiteral(
    "# Function extents gba_recompile proved reachable by recursive descent.\n"
    "# Format: 0xSTART<TAB>0xEND_EXCLUSIVE<TAB>arm|thumb\n");
  QSet<QString> emitted;
  emitted.reserve(spans.size());
  for (const auto& span : spans) {
    const QString row = QStringLiteral("0x%1\t0x%2\t%3\n")
                          .arg(span.start, 8, 16, QLatin1Char('0'))
                          .arg(span.end, 8, 16, QLatin1Char('0'))
                          .arg(span.thumb ? QStringLiteral("thumb") : QStringLiteral("arm"));
    if (emitted.contains(row)) continue;
    emitted.insert(row);
    text += row;
  }
  return text;
}

/* The number of distinct bytes the spans cover, counting an overlap once. */
quint32 gbaTranslatedBytes(const QList<GbaTranslatedSpan>& spans) {
  quint32 total = 0;
  quint32 covered = 0;  // exclusive end of everything already counted
  for (const auto& span : spans) {
    const quint32 from = std::max(span.start, covered);
    if (span.end > from) {
      total += span.end - from;
      covered = span.end;
    }
  }
  return total;
}

/* True when the instruction's destination is register-borne — a BX, or anything
 * that writes PC directly. gbarecomp knows control leaves here and cannot say
 * where, which is precisely the gap a Ghidra candidate is able to fill. A BX
 * carries the destination instruction set in bit 0, so the two sides may
 * legitimately disagree about mode at a site like this. */
bool gbaTransfersThroughRegister(const QString& disassembly) {
  const QString mnemonic = gbaMnemonic(disassembly);
  if (mnemonic == QStringLiteral("bx") || mnemonic == QStringLiteral("blx")) return true;
  static const QRegularExpression writesPc(QStringLiteral(R"(^[a-z.]+[a-z0-9]*\s+pc\b)"));
  return writesPc.match(disassembly).hasMatch();
}

/* The destination of a direct branch at `source`, or false when gbarecomp did
 * not resolve one there.
 *
 * `b` and its conditional forms print their target, so those are read straight
 * back. A THUMB BL does not: the destination only exists once both halfwords
 * are combined, so gba_recompile renders the pair as `bl.hi`/`bl.lo` and prints
 * 0x00000000 on the second half. Recovering it means decoding the pair out of
 * the ROM — and, critically, requiring that the `bl.hi` was decoded too.
 *
 * An orphan `bl.lo` is a BL second halfword with no first halfword in front of
 * it. That happens wherever data is being decoded as code, and its destination
 * is unknowable from the halfword alone. Ghidra fills the blank by propagating
 * LR, which manufactures a plausible address out of nothing; on a 16 MB
 * cartridge it produced 113,516 such COMPUTED_CALL references against 22 real
 * recovered switch targets. Refusing to treat an unresolved branch as evidence
 * about where control goes is what keeps compressed art out of the dispatch
 * table. */
bool gbaDirectBranchTarget(const GbaInstructionMap& instructions, quint32 source,
                           const QByteArray& rom, quint32 romBase, quint32& target) {
  const auto found = instructions.constFind(source);
  if (found == instructions.constEnd()) return false;
  const QString& disassembly = found.value();
  const QString mnemonic = gbaMnemonic(disassembly);
  if (mnemonic == QStringLiteral("bl.lo")) {
    const auto upper = instructions.constFind(source - 2);
    if (upper == instructions.constEnd() ||
        gbaMnemonic(upper.value()) != QStringLiteral("bl.hi")) {
      return false;  // orphan: the BL's first halfword was never decoded
    }
    const qint64 offset = static_cast<qint64>(source) - romBase - 2;
    if (offset < 0 || offset + 4 > rom.size()) return false;
    const auto* bytes = reinterpret_cast<const uchar*>(rom.constData()) + offset;
    const quint32 high = qFromLittleEndian<quint16>(bytes);
    const quint32 low = qFromLittleEndian<quint16>(bytes + 2);
    if ((high & 0xF800u) != 0xF000u || (low & 0xF800u) != 0xF800u) return false;
    qint32 displacement =
      static_cast<qint32>(((high & 0x7FFu) << 12) | ((low & 0x7FFu) << 1));
    if (displacement & 0x400000) displacement -= 0x800000;
    target = static_cast<quint32>(source + 2 + displacement);
    return true;
  }
  static const QSet<QString> directBranches{
    QStringLiteral("b"),   QStringLiteral("beq"), QStringLiteral("bne"),
    QStringLiteral("bcs"), QStringLiteral("bcc"), QStringLiteral("bmi"),
    QStringLiteral("bpl"), QStringLiteral("bvs"), QStringLiteral("bvc"),
    QStringLiteral("bhi"), QStringLiteral("bls"), QStringLiteral("bge"),
    QStringLiteral("blt"), QStringLiteral("bgt"), QStringLiteral("ble"),
    QStringLiteral("bal"),
  };
  if (!directBranches.contains(mnemonic)) return false;
  static const QRegularExpression targetPattern(QStringLiteral(R"(\b0x([0-9A-Fa-f]{8})\b)"));
  const auto match = targetPattern.match(disassembly);
  if (!match.hasMatch()) return false;
  target = match.captured(1).toUInt(nullptr, 16);
  return true;
}

/* Is Ghidra's claim about this function backed by a control transfer that
 * gbarecomp itself decoded and that can actually reach the candidate?
 *
 * A raw cartridge carries no section table, so Ghidra disassembles compressed
 * art and sample data as freely as it disassembles code and cannot tell the
 * difference. Its function list is therefore a seed *source*, never a coverage
 * oracle — and neither is its reference class, which is derived from that same
 * decoding. gbarecomp's translation is the opposite: it comes from recursive
 * descent along real control flow out of the entry point, so it never walks
 * into a texture.
 *
 * Every rule below therefore asks gbarecomp, not Ghidra, what the cited source
 * address contains. Being inside a translated extent is necessary but not
 * sufficient: a span that legitimately contains a run of code can still have a
 * Ghidra reference hanging off a byte offset that is not an instruction
 * boundary at all, and a decoded instruction whose target gbarecomp could not
 * resolve says nothing about where control goes. Ghidra's contribution is
 * narrowed to what it alone can supply — a destination for a transfer gbarecomp
 * proved happens but could not follow.
 *
 * Returns true and fills `rule`/`evidence` on admission, setting `modeCrossed`
 * when the admitting evidence sits in the other instruction set; on rejection
 * `reason` is set only when the candidate was contradicted rather than merely
 * unsupported. */
bool admitGbaSeed(const GbaSeedCandidate& candidate,
                  const QList<GbaTranslatedSpan>& spans,
                  const GbaInstructionMap& instructions,
                  const QByteArray& rom, quint32 romBase,
                  QString& rule, quint32& evidence, bool& modeCrossed,
                  QString& reason) {
  const QString candidateMode =
    candidate.thumb ? QStringLiteral("thumb") : QStringLiteral("arm");
  // Ghidra sorts its references by how it classified them, but that
  // classification is exactly what cannot be trusted here, so both lists are
  // weighed under both rules and the instruction itself decides which applies.
  QVector<quint32> branchSources = candidate.directSources;
  branchSources += candidate.computedSources;

  // 1. A direct branch gbarecomp decoded whose resolved destination *is* this
  //    entry. ARMv4T has no BLX, so a direct branch cannot change instruction
  //    set and the site's mode pins the callee's; when the two disagree, one of
  //    them decoded data as code.
  for (const quint32 source : branchSources) {
    const GbaTranslatedSpan* span = gbaSpanContaining(spans, source);
    if (!span) continue;
    quint32 target = 0;
    if (!gbaDirectBranchTarget(instructions, source, rom, romBase, target)) {
      if (reason.isEmpty() && instructions.contains(source) &&
          gbaMnemonic(instructions.value(source)) == QStringLiteral("bl.lo")) {
        reason = QStringLiteral("the only branch cited at %1 is an orphan bl.lo — a BL second halfword whose first halfword was never decoded, so its destination is unknown")
                   .arg(hex32(source));
      }
      continue;
    }
    if (target != candidate.entry) continue;
    if (span->thumb != candidate.thumb) {
      if (reason.isEmpty()) {
        reason = QStringLiteral("the %1 branch at %2 reaches %3, but ARMv4T has no BLX so it cannot enter it as %4")
                   .arg(span->thumb ? QStringLiteral("thumb") : QStringLiteral("arm"),
                        hex32(source), hex32(target), candidateMode);
      }
      continue;
    }
    rule = QStringLiteral("direct-branch");
    evidence = source;
    return true;
  }

  // 2. A register-indirect transfer inside translated code: gbarecomp proved
  //    control leaves here and could not resolve the destination, which is the
  //    one thing Ghidra can usefully add. No mode constraint applies — a BX
  //    legitimately switches instruction set, and refusing a mode change would
  //    reject the ARM interworking veneers a THUMB-compiled title reaches
  //    exactly this way — so the crossing is flagged for the proof instead.
  for (const quint32 source : branchSources) {
    const GbaTranslatedSpan* span = gbaSpanContaining(spans, source);
    if (!span) continue;
    const auto found = instructions.constFind(source);
    if (found == instructions.constEnd()) continue;
    if (!gbaTransfersThroughRegister(found.value())) continue;
    rule = QStringLiteral("register-indirect");
    evidence = source;
    modeCrossed = span->thumb != candidate.thumb;
    return true;
  }

  // 3. A pointer word inside translated code — a literal pool slot or an inline
  //    jump-table entry. Bit 0 of a BX target selects the instruction set, so
  //    the stored word must be `entry | 1` for THUMB and `entry` for ARM. That
  //    is an exact test on the word's value: one failing it is not a pointer to
  //    this candidate, whatever Ghidra recorded.
  const quint32 expectedWord = candidate.entry | (candidate.thumb ? 1u : 0u);
  for (const quint32 source : candidate.pointerSources) {
    if ((source & 3u) != 0 || source < romBase) continue;
    if (!gbaSpanContaining(spans, source)) continue;
    const qint64 offset = static_cast<qint64>(source) - romBase;
    if (offset + 4 > rom.size()) continue;
    const quint32 stored = qFromLittleEndian<quint32>(
      reinterpret_cast<const uchar*>(rom.constData()) + offset);
    if (stored != expectedWord) {
      if (reason.isEmpty()) {
        reason = QStringLiteral("the pointer word at %1 holds %2, not the %3 target %4")
                   .arg(hex32(source), hex32(stored), candidateMode, hex32(expectedWord));
      }
      continue;
    }
    rule = QStringLiteral("pointer-word");
    evidence = source;
    return true;
  }
  return false;
}

} // namespace psxstudio
