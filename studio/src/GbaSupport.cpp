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
  text += QStringLiteral("[runtime]\nwindow_title = %1\n\n")
            .arg(tomlQuoted(request.windowTitle));
  text += QStringLiteral("[video]\nscreen = \"frontlit\"\nresize_view = false\n\n");
  text += QStringLiteral("[audio]\nshadow = false\n");
  return text;
}

QString generatedGbaPackToml(const PipelineRequest& request,
                             const QString& bundleName,
                             const QString& romSha256,
                             const QString& biosSha256) {
  const QString platform = request.targetPlatform == TargetPlatform::Windows
    ? QStringLiteral("windows")
    : request.targetPlatform == TargetPlatform::Linux
      ? QStringLiteral("linux") : QStringLiteral("macos");
  QString text;
  text += QStringLiteral("[package]\n");
  text += QStringLiteral("name = %1\n").arg(tomlQuoted(bundleName));
  text += QStringLiteral("version = \"1.0.0\"\n");
  text += QStringLiteral("platforms = [%1]\n").arg(tomlQuoted(platform));
  text += QStringLiteral("icon = \"AppIcon.png\"\n\n");
  text += QStringLiteral("[image]\n");
  text += QStringLiteral("rom-sha256 = %1\n").arg(tomlQuoted(romSha256));
  if (!biosSha256.isEmpty())
    text += QStringLiteral("bios-sha256 = %1\n").arg(tomlQuoted(biosSha256));
  text += QStringLiteral("\n");
  text += QStringLiteral("[runtime]\n");
  text += QStringLiteral("menu = true\n");
  text += QStringLiteral("enhanced-audio = true\n");
  text += QStringLiteral("screen-sim = true\n");
  text += QStringLiteral("engine-hle = \"auto\"\n");
  text += QStringLiteral("skip-bios = true\n");
  text += QStringLiteral("interpreter = true\n\n");
  text += QStringLiteral("[output]\n");
  text += QStringLiteral("binary = true\n");
  text += QStringLiteral("embed-inputs = true\n");
  text += QStringLiteral("c-source = false\n");
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
  Q_UNUSED(bundleId);
  const bool requireGip = request.targetPlatform == TargetPlatform::MacOS &&
                          request.macosGipGamepad;
  QString cmake;
  cmake += QStringLiteral("cmake_minimum_required(VERSION 3.20)\n");
  cmake += QStringLiteral("project(GeneratedGbaRustPackage NONE)\n");
  cmake += QStringLiteral("find_program(GBA_CARGO NAMES cargo HINTS \"$ENV{CARGO_HOME}/bin\" \"$ENV{HOME}/.cargo/bin\" /usr/local/bin /opt/homebrew/bin /usr/bin /bin)\n");
  cmake += QStringLiteral("if(NOT GBA_CARGO)\n  message(FATAL_ERROR \"Cargo was not found. Install Rust with rustup or set CARGO_HOME.\")\nendif()\n");
  cmake += QStringLiteral("set(_GBA_RUST_ROOT \"${CMAKE_CURRENT_SOURCE_DIR}/gba-rust\")\n");
  cmake += QStringLiteral("set(_GBA_GAMEPAD_ROOT \"${CMAKE_CURRENT_SOURCE_DIR}/recomp_gamepad\")\n");
  cmake += QStringLiteral("set(GBA_CARGO_TARGET_DIR \"${CMAKE_BINARY_DIR}/cargo-target\" CACHE PATH \"Reusable Cargo target cache\")\n");
  cmake += QStringLiteral("option(GBA_CLEAN_CARGO_TARGET \"Remove Cargo intermediates after packaging\" ON)\n");
  cmake += QStringLiteral("set(_GBA_CARGO_TARGET \"${GBA_CARGO_TARGET_DIR}\")\n");
  cmake += QStringLiteral("set(_GBA_PACK_OUT \"${CMAKE_BINARY_DIR}/pack-out\")\n");
  cmake += QStringLiteral("set(_GBA_PACKAGE_NAME %1)\n").arg(cmakeQuoted(bundleName));
  cmake += QStringLiteral("if(WIN32)\n  set(_GBA_EXE_SUFFIX \".exe\")\nelse()\n  set(_GBA_EXE_SUFFIX \"\")\nendif()\n");
  cmake += QStringLiteral("if(GBA_CLEAN_CARGO_TARGET)\n  set(_GBA_CARGO_CLEAN_COMMAND ${CMAKE_COMMAND} -E rm -rf \"${_GBA_CARGO_TARGET}\")\nelse()\n  set(_GBA_CARGO_CLEAN_COMMAND ${CMAKE_COMMAND} -E true)\nendif()\n");
  cmake += QStringLiteral("set(_GBA_ENV CARGO_TARGET_DIR=${_GBA_CARGO_TARGET} RECOMP_GAMEPAD_ROOT=${_GBA_GAMEPAD_ROOT} CARGO_INCREMENTAL=0 CARGO_PROFILE_DIST_LTO=thin CARGO_PROFILE_DIST_CODEGEN_UNITS=8)\n");
  cmake += requireGip
    ? QStringLiteral("list(APPEND _GBA_ENV RECOMP_REQUIRE_GIP=1 RECOMP_EXPORT_GIP_SYMBOLS=1 CARGO_PROFILE_DIST_STRIP=none)\n")
    : QStringLiteral("list(APPEND _GBA_ENV RECOMP_DISABLE_GIP=1)\n");
  cmake += QStringLiteral("file(GLOB_RECURSE _GBA_RUST_SOURCES \"${_GBA_RUST_ROOT}/*.rs\" \"${_GBA_RUST_ROOT}/*.toml\" \"${_GBA_RUST_ROOT}/Cargo.lock\")\n");
  cmake += QStringLiteral("file(GLOB_RECURSE _GBA_GAMEPAD_SOURCES \"${_GBA_GAMEPAD_ROOT}/*\")\n");
  cmake += QStringLiteral("set(_GBA_STAMP \"${CMAKE_BINARY_DIR}/gba-rust-package.stamp\")\n");
  cmake += QStringLiteral("set(_GBA_STEGANOS_PACKAGE_DIR \"${CMAKE_BINARY_DIR}/steganos-package/gba-runtime/$<CONFIG>\")\n");
  cmake += QStringLiteral("add_custom_command(OUTPUT \"${_GBA_STAMP}\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E echo \"[1/2] Building locked Rust runtime and packager\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E rm -rf \"${_GBA_PACK_OUT}\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E env ${_GBA_ENV} ${GBA_CARGO} build --manifest-path \"${_GBA_RUST_ROOT}/Cargo.toml\" --profile dist --locked -p recomp -p gba-pack\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E echo \"[2/2] Assembling thin Rust GBA package (translation deferred)\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E env ${_GBA_ENV} \"${_GBA_CARGO_TARGET}/dist/gba-pack${_GBA_EXE_SUFFIX}\" build \"${CMAKE_CURRENT_SOURCE_DIR}/pack.toml\" --rom \"${CMAKE_CURRENT_SOURCE_DIR}/package_inputs/game.gba\" --bios \"${CMAKE_CURRENT_SOURCE_DIR}/package_inputs/gba_bios.bin\" --out \"${_GBA_PACK_OUT}\" --recomp \"${_GBA_CARGO_TARGET}/dist/recomp${_GBA_EXE_SUFFIX}\" --gamedb \"${_GBA_RUST_ROOT}/gamedb.sqlite\" --defer-translation --no-soak\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E rm -rf \"${_GBA_STEGANOS_PACKAGE_DIR}\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E make_directory \"${_GBA_STEGANOS_PACKAGE_DIR}\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E copy_directory \"${_GBA_PACK_OUT}/${_GBA_PACKAGE_NAME}\" \"${_GBA_STEGANOS_PACKAGE_DIR}\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/game.manifest.json\" \"${_GBA_STEGANOS_PACKAGE_DIR}/game.manifest.json\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E copy_if_different \"${CMAKE_CURRENT_SOURCE_DIR}/PSXRecomp-Proof.zip\" \"${_GBA_STEGANOS_PACKAGE_DIR}/PSXRecomp-Proof.zip\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E echo \"Cleaning one-shot translation intermediates\"\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E rm -rf \"${_GBA_PACK_OUT}\"\n");
  cmake += QStringLiteral("  COMMAND ${_GBA_CARGO_CLEAN_COMMAND}\n");
  cmake += QStringLiteral("  COMMAND ${CMAKE_COMMAND} -E touch \"${_GBA_STAMP}\"\n");
  cmake += QStringLiteral("  DEPENDS ${_GBA_RUST_SOURCES} ${_GBA_GAMEPAD_SOURCES} \"${CMAKE_CURRENT_SOURCE_DIR}/pack.toml\" \"${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png\" \"${CMAKE_CURRENT_SOURCE_DIR}/package_inputs/game.gba\" \"${CMAKE_CURRENT_SOURCE_DIR}/package_inputs/gba_bios.bin\" \"${CMAKE_CURRENT_SOURCE_DIR}/game.manifest.json\" \"${CMAKE_CURRENT_SOURCE_DIR}/PSXRecomp-Proof.zip\"\n");
  cmake += QStringLiteral("  WORKING_DIRECTORY \"${CMAKE_CURRENT_SOURCE_DIR}\"\n  COMMENT \"Building and packing Rust GBA runtime\"\n  USES_TERMINAL\n  VERBATIM)\n");
  cmake += QStringLiteral("add_custom_target(gba-runtime ALL DEPENDS \"${_GBA_STAMP}\")\n");
  return cmake;
}

} // namespace psxstudio
