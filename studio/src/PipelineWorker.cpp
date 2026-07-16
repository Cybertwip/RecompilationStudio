#include "PipelineWorker.h"

#include "BiosBrandingPatcher.h"
#include "CueSheet.h"
#include "DiscInspector.h"
#include "PipelineSupport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <algorithm>

namespace psxstudio {

namespace {

constexpr auto kScph1001Sha256 = "71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3";
constexpr qint64 kScph1001Size = 512 * 1024;

QString cleanBundleName(QString title) {
  title = title.trimmed();
  title.replace('/', QStringLiteral(" - "));
  title.replace(':', QStringLiteral(" -"));
  title.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")));
  title = title.simplified();
  return title.left(80);
}

QString escapedComment(QString text) {
  text.replace('\n', ' ');
  text.replace('\r', ' ');
  return text;
}

QJsonDocument readJson(const QString& path, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read %1: %2").arg(path, file.errorString());
    return {};
  }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || document.isNull()) {
    error = QStringLiteral("Invalid JSON in %1: %2").arg(path, parseError.errorString());
    return {};
  }
  return document;
}

QSet<quint32> readDynamicTargets(const QString& path, QString& error) {
  QSet<quint32> targets;
  const auto document = readJson(path, error);
  if (document.isNull()) {
    return {};
  }
  for (const auto& value : document.object().value(QStringLiteral("targets")).toArray()) {
    bool ok = false;
    const quint32 target = value.toObject().value(QStringLiteral("target")).toString().toUInt(&ok, 16);
    if (!ok) {
      error = QStringLiteral("Invalid runtime-installed target in %1.").arg(path);
      return {};
    }
    targets.insert(target);
  }
  return targets;
}

QList<QPair<quint32, quint32>> readDataRanges(const QString& path, QString& error) {
  QList<QPair<quint32, quint32>> ranges;
  const auto document = readJson(path, error);
  if (document.isNull()) {
    return {};
  }
  const auto rows = document.object().value(QStringLiteral("ranges")).toArray();
  for (const auto& value : rows) {
    const auto row = value.toObject();
    bool okStart = false;
    bool okEnd = false;
    const quint32 start = row.value(QStringLiteral("start")).toString().toUInt(&okStart, 16);
    const quint32 end = row.value(QStringLiteral("end")).toString().toUInt(&okEnd, 16);
    if (!okStart || !okEnd || end <= start) {
      error = QStringLiteral("Invalid data range in %1.").arg(path);
      return {};
    }
    ranges.append({ start, end });
  }
  std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });
  return ranges;
}

bool ghidraFunctionHasStaticExit(const GameDescription& game, const QJsonObject& function) {
  bool okStart = false;
  bool okEnd = false;
  const quint32 start = function.value(QStringLiteral("body_min")).toString().toUInt(&okStart, 16);
  const quint32 endInclusive = function.value(QStringLiteral("body_max")).toString().toUInt(&okEnd, 16);
  if (!okStart || !okEnd || endInclusive < start || start < game.loadAddress) {
    return true;
  }
  const quint32 span = endInclusive - start + 1u;
  if (span > 0x400u || (span & 3u) != 0u) {
    return true;
  }
  for (quint32 address = start; address <= endInclusive; address += 4u) {
    const quint32 offset = address - game.loadAddress;
    if (offset + 4u > static_cast<quint32>(game.codeBytes.size())) {
      return true;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(game.codeBytes.constData() + offset);
    const quint32 word = static_cast<quint32>(bytes[0]) |
                         (static_cast<quint32>(bytes[1]) << 8u) |
                         (static_cast<quint32>(bytes[2]) << 16u) |
                         (static_cast<quint32>(bytes[3]) << 24u);
    const quint32 opcode = word >> 26u;
    if (opcode == 0x02u) {
      return true;
    }
    if (opcode == 0u) {
      const quint32 funct = word & 0x3fu;
      if (funct == 0x08u || funct == 0x09u) {
        return true;
      }
    }
  }
  return false;
}

quint32 crc32File(const QString& path, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not checksum %1: %2").arg(path, file.errorString());
    return 0;
  }
  static quint32 table[256];
  static bool initialized = false;
  if (!initialized) {
    for (quint32 i = 0; i < 256; ++i) {
      quint32 value = i;
      for (int bit = 0; bit < 8; ++bit)
        value = (value & 1u) ? (value >> 1u) ^ 0xEDB88320u : value >> 1u;
      table[i] = value;
    }
    initialized = true;
  }
  quint32 crc = 0xFFFFFFFFu;
  while (!file.atEnd()) {
    const QByteArray chunk = file.read(1 << 20);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
      error = QStringLiteral("Could not checksum %1: %2").arg(path, file.errorString());
      return 0;
    }
    for (uchar byte : chunk) crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8u);
  }
  return crc ^ 0xFFFFFFFFu;
}

QString makeGameToml(const PipelineRequest& request,
                     const GameDescription& game,
                     const QString& exePath,
                     const QString& discPath,
                     const QString& seedsPath,
                     const QString& outDir) {
  const quint32 romEnd = 0x800u + game.textSize;
  QString text;
  text += QStringLiteral("[game]\n");
  text += QStringLiteral("name = %1\n").arg(tomlQuoted(request.windowTitle));
  text += QStringLiteral("id = %1\n").arg(tomlQuoted(game.serial));
  text += QStringLiteral("exe = %1\n").arg(tomlQuoted(exePath));
  text += QStringLiteral("disc = %1\n").arg(tomlQuoted(discPath));
  text += QStringLiteral("load_address = %1\n").arg(tomlQuoted(hex32(game.loadAddress)));
  text += QStringLiteral("entry_pc = %1\n").arg(tomlQuoted(hex32(game.entryPc)));
  text += QStringLiteral("text_size = %1\n").arg(tomlQuoted(hex32(game.textSize)));
  text += QStringLiteral("stack_base = %1\n\n").arg(tomlQuoted(hex32(game.stackBase)));
  text += QStringLiteral("[recompiler]\n");
  text += QStringLiteral("seeds = %1\n").arg(tomlQuoted(seedsPath));
  text += QStringLiteral("out_dir = %1\n").arg(tomlQuoted(outDir));
  text += QStringLiteral("strict = true\n\n");
  text += QStringLiteral("[runtime]\n");
  text += QStringLiteral("window_title = %1\n").arg(tomlQuoted(request.windowTitle));
  text += QStringLiteral("disc_speed = \"1x\"\n");
  text += runtimeBootToml(request.skipBiosBoot);
  text += QStringLiteral("overlay_cache = false\n");
  text += QStringLiteral("turbo_loads = false\n\n");
  text += QStringLiteral("[video]\n");
  text += QStringLiteral("renderer = \"opengl\"\n");
  text += QStringLiteral("supersampling = 1\n");
  text += QStringLiteral("antialiasing = false\n");
  text += QStringLiteral("texture_filtering = \"nearest\"\n");
  text += QStringLiteral("aspect_ratio = \"4:3\"\n\n");
  text += QStringLiteral("[controller]\n");
  text += QStringLiteral("p1_device = \"auto\"\n");
  text += QStringLiteral("p2_device = \"none\"\n");
  text += QStringLiteral("default_mode = \"digital\"\n");
  text += QStringLiteral("deadzone = 12000\n");
  text += QStringLiteral("allow_hybrid = false\n");
  text += QStringLiteral("lock_mode = false\n\n");
  text += QStringLiteral("[audit]\n\n");
  text += QStringLiteral("[[audit.regions]]\n");
  text += QStringLiteral("name = \"Text\"\n");
  text += QStringLiteral("rom_start = \"0x800\"\n");
  text += QStringLiteral("rom_end = %1\n").arg(tomlQuoted(hex32(romEnd)));
  text += QStringLiteral("vaddr_base = %1\n\n").arg(tomlQuoted(hex32(game.loadAddress)));
  text += QStringLiteral("[audit.normalize]\n");
  text += QStringLiteral("kseg_mask = \"0x1FFFFFFF\"\n");
  return text;
}

QString makeInfoPlist(const QString& bundleId,
                      const QString& bundleName,
                      const QString& executableName) {
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleDisplayName</key><string>%1</string>
  <key>CFBundleExecutable</key><string>%2</string>
  <key>CFBundleIconFile</key><string>AppIcon.icns</string>
  <key>CFBundleIdentifier</key><string>%3</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>%1</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
)").arg(bundleName.toHtmlEscaped(), executableName.toHtmlEscaped(), bundleId.toHtmlEscaped());
}

QString makeWindowsResource(QString iconPath) {
  iconPath.replace('\\', QStringLiteral("\\\\"));
  iconPath.replace('"', QStringLiteral("\\\""));
  return QStringLiteral("IDI_APP_ICON ICON \"%1\"\n").arg(iconPath);
}

QString makeMingwToolchain(const QString& gcc,
                           const QString& gxx,
                           const QString& windres,
                           const QString& ar,
                           const QString& ranlib,
                           const QString& strip) {
  QString cmake;
  cmake += QStringLiteral("set(CMAKE_SYSTEM_NAME Windows)\n");
  cmake += QStringLiteral("set(CMAKE_SYSTEM_PROCESSOR x86_64)\n");
  cmake += QStringLiteral("set(CMAKE_C_COMPILER %1)\n").arg(cmakeQuoted(gcc));
  cmake += QStringLiteral("set(CMAKE_CXX_COMPILER %1)\n").arg(cmakeQuoted(gxx));
  cmake += QStringLiteral("set(CMAKE_RC_COMPILER %1)\n").arg(cmakeQuoted(windres));
  cmake += QStringLiteral("set(CMAKE_AR %1)\n").arg(cmakeQuoted(ar));
  cmake += QStringLiteral("set(CMAKE_RANLIB %1)\n").arg(cmakeQuoted(ranlib));
  cmake += QStringLiteral("set(CMAKE_STRIP %1)\n").arg(cmakeQuoted(strip));
  cmake += QStringLiteral("set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n");
  cmake += QStringLiteral("set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n");
  cmake += QStringLiteral("set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n");
  cmake += QStringLiteral("set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)\n");
  return cmake;
}

QString makeProjectCMake(const PipelineRequest& request,
                         const GameDescription& game,
                         const QString& bundleName,
                         const QString& bundleId,
                         const QString& platformMetadataPath,
                         const QString& iconPath,
                         const QString& dependencyCachePath,
                         const QString& biosFull,
                         const QString& biosDispatch,
                         const QString& gameFull,
                         const QString& gameDispatch,
                         quint32 expectedBiosCrc) {
  const QString appSupportName = QStringLiteral("PSXRecomp/%1").arg(game.serial.isEmpty()
    ? sanitizedFileStem(request.windowTitle)
    : game.serial);
  const bool windowsTarget = request.targetPlatform == TargetPlatform::Windows;
  QString cmake;
  cmake += QStringLiteral("cmake_minimum_required(VERSION 3.20)\n");
  cmake += windowsTarget ? QStringLiteral("project(GeneratedPSXApp C CXX RC)\n")
                         : QStringLiteral("project(GeneratedPSXApp C CXX)\n");
  cmake += QStringLiteral("set(CMAKE_C_STANDARD 99)\nset(CMAKE_CXX_STANDARD 17)\n");
  if (windowsTarget) {
    cmake += QStringLiteral("include(FetchContent)\n");
    cmake += QStringLiteral("set(FETCHCONTENT_QUIET OFF)\n");
    cmake += QStringLiteral("set(FETCHCONTENT_BASE_DIR %1 CACHE PATH \"\" FORCE)\n")
               .arg(cmakeQuoted(dependencyCachePath));
    cmake += QStringLiteral("FetchContent_Declare(SDL2Mingw\n");
    cmake += QStringLiteral("  URL \"https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz\"\n");
    cmake += QStringLiteral("  URL_HASH SHA256=83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2\n");
    cmake += QStringLiteral("  DOWNLOAD_EXTRACT_TIMESTAMP TRUE\n)\n");
    cmake += QStringLiteral("FetchContent_GetProperties(SDL2Mingw)\n");
    cmake += QStringLiteral("if(NOT sdl2mingw_POPULATED)\n");
    cmake += QStringLiteral("  FetchContent_Populate(SDL2Mingw)\nendif()\n");
    cmake += QStringLiteral("set(_PSX_SDL2_ROOT \"${sdl2mingw_SOURCE_DIR}/x86_64-w64-mingw32\")\n");
    cmake += QStringLiteral("find_package(SDL2 CONFIG REQUIRED PATHS \"${_PSX_SDL2_ROOT}/lib/cmake/SDL2\" NO_DEFAULT_PATH)\n");
    cmake += QStringLiteral("set(SDL2_INCLUDE_DIRS \"${_PSX_SDL2_ROOT}/include;${_PSX_SDL2_ROOT}/include/SDL2\")\n");
    cmake += QStringLiteral("set(SDL2_LIBRARIES SDL2::SDL2main SDL2::SDL2-static)\n");
    cmake += QStringLiteral("set(SDL2_STATIC_LDFLAGS ${SDL2_LIBRARIES})\n");
    cmake += QStringLiteral("set(PSX_STATIC_RUNTIME ON CACHE BOOL \"\" FORCE)\n");
  } else {
    cmake += QStringLiteral("set(CMAKE_OSX_DEPLOYMENT_TARGET \"14.0\" CACHE STRING \"\" FORCE)\n");
    cmake += QStringLiteral("set(CMAKE_MACOSX_RPATH ON)\nset(CMAKE_BUILD_WITH_INSTALL_RPATH ON)\n");
    cmake += QStringLiteral("set(CMAKE_INSTALL_RPATH \"@executable_path/../Frameworks\")\n");
  }
  cmake += QStringLiteral("set(PSXRECOMP_ROOT %1 CACHE PATH \"\" FORCE)\n")
             .arg(cmakeQuoted(request.frameworkRoot));
  cmake += QStringLiteral("set(PSXRECOMP_SKIP_BIOS_STALE_CHECK ON CACHE BOOL \"\" FORCE)\n");
  cmake += QStringLiteral("set(PSX_LAUNCHER OFF CACHE BOOL \"\" FORCE)\n");
  cmake += QStringLiteral("set(PSX_DEBUG_TOOLS OFF CACHE BOOL \"\" FORCE)\n");
  cmake += macosGipCmakeOption(!windowsTarget && request.macosGipGamepad);
  cmake += QStringLiteral("include(${PSXRECOMP_ROOT}/runtime/runtime.cmake)\n");
  cmake += QStringLiteral("psxrecomp_add_runtime_target(psx-runtime\n");
  cmake += QStringLiteral("  BIOS_GENERATED_FULL_C %1\n").arg(cmakeQuoted(biosFull));
  cmake += QStringLiteral("  BIOS_GENERATED_DISPATCH_C %1\n").arg(cmakeQuoted(biosDispatch));
  cmake += QStringLiteral("  GAME_GENERATED_FULL_C %1\n").arg(cmakeQuoted(gameFull));
  cmake += QStringLiteral("  GAME_GENERATED_DISPATCH_C %1\n").arg(cmakeQuoted(gameDispatch));
  cmake += QStringLiteral("  WINDOW_TITLE %1\n").arg(cmakeQuoted(request.windowTitle));
  cmake += QStringLiteral("  EXE_NAME %1\n").arg(cmakeQuoted(bundleName));
  cmake += windowsTarget
    ? QStringLiteral("  DEFAULT_BIOS_PATH \"bios/SCPH1001.BIN\"\n")
    : QStringLiteral("  DEFAULT_BIOS_PATH \"../Resources/bios/SCPH1001.BIN\"\n");
  cmake += windowsTarget
    ? QStringLiteral("  DEFAULT_GAME_CONFIG_PATH \"game.toml\"\n")
    : QStringLiteral("  DEFAULT_GAME_CONFIG_PATH \"../Resources/game.toml\"\n");
  cmake += QStringLiteral("  APP_SUPPORT_DIR_NAME %1\n)\n").arg(cmakeQuoted(appSupportName));
  cmake += QStringLiteral("target_compile_definitions(psx-runtime PRIVATE PSX_EXPECTED_BIOS_CRC32=0x%1u)\n")
             .arg(expectedBiosCrc, 8, 16, QLatin1Char('0'));
  if (windowsTarget) {
    cmake += QStringLiteral("target_sources(psx-runtime PRIVATE %1)\n")
               .arg(cmakeQuoted(platformMetadataPath));
    cmake += QStringLiteral("set_target_properties(psx-runtime PROPERTIES OUTPUT_NAME %1)\n")
               .arg(cmakeQuoted(bundleName));
    cmake += QStringLiteral("install(TARGETS psx-runtime RUNTIME DESTINATION .)\n");
  } else {
    cmake += QStringLiteral("set_source_files_properties(%1 PROPERTIES MACOSX_PACKAGE_LOCATION \"Resources\")\n")
               .arg(cmakeQuoted(iconPath));
    cmake += QStringLiteral("target_sources(psx-runtime PRIVATE %1)\n").arg(cmakeQuoted(iconPath));
    cmake += QStringLiteral("set_target_properties(psx-runtime PROPERTIES\n");
    cmake += QStringLiteral("  MACOSX_BUNDLE TRUE\n");
    cmake += QStringLiteral("  OUTPUT_NAME %1\n").arg(cmakeQuoted(bundleName));
    cmake += QStringLiteral("  MACOSX_BUNDLE_INFO_PLIST %1\n").arg(cmakeQuoted(platformMetadataPath));
    cmake += QStringLiteral("  MACOSX_BUNDLE_GUI_IDENTIFIER %1\n").arg(cmakeQuoted(bundleId));
    cmake += QStringLiteral("  MACOSX_BUNDLE_BUNDLE_NAME %1\n").arg(cmakeQuoted(bundleName));
    cmake += QStringLiteral("  MACOSX_BUNDLE_ICON_FILE \"AppIcon.icns\"\n");
    cmake += QStringLiteral("  XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED OFF\n");
    cmake += QStringLiteral("  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY \"\"\n)\n");
    cmake += QStringLiteral("install(TARGETS psx-runtime BUNDLE DESTINATION .)\n");
  }
  return cmake;
}

QString findOpenSsl3() {
  QStringList candidates;
  const QString configured = qEnvironmentVariable("PSXRECOMP_OPENSSL");
  if (!configured.isEmpty()) candidates << configured;
  candidates << QStringLiteral("/usr/local/opt/openssl@3/bin/openssl")
             << QStringLiteral("/opt/homebrew/opt/openssl@3/bin/openssl")
             << QStringLiteral("/usr/local/bin/openssl")
             << QStringLiteral("/opt/homebrew/bin/openssl")
             << QStringLiteral("/usr/local/Caskroom/miniconda/base/bin/openssl");
  const QString pathCandidate = QStandardPaths::findExecutable(QStringLiteral("openssl"));
  if (!pathCandidate.isEmpty()) candidates << pathCandidate;
  candidates.removeDuplicates();
  for (const QString& candidate : candidates) {
    if (!QFileInfo(candidate).isExecutable()) continue;
    QProcess version;
    version.start(candidate, { QStringLiteral("version") });
    if (!version.waitForStarted(3000) || !version.waitForFinished(5000) || version.exitCode() != 0)
      continue;
    const QString output = QString::fromUtf8(version.readAllStandardOutput() + version.readAllStandardError());
    if (!output.startsWith(QStringLiteral("OpenSSL 3."))) continue;
    QProcess help;
    help.start(candidate, { QStringLiteral("pkcs12"), QStringLiteral("-help") });
    if (!help.waitForStarted(3000) || !help.waitForFinished(5000)) continue;
    const QString helpText = QString::fromUtf8(help.readAllStandardOutput() + help.readAllStandardError());
    if (helpText.contains(QStringLiteral("-legacy"))) return candidate;
  }
  return {};
}

QString versionText(const QString& program, const QStringList& args) {
  QProcess process;
  process.start(program, args);
  if (!process.waitForStarted(5000) || !process.waitForFinished(15000)) {
    return {};
  }
  QByteArray output = process.readAllStandardOutput();
  output += process.readAllStandardError();
  return QString::fromUtf8(output).trimmed();
}

} // namespace

PipelineWorker::PipelineWorker(QObject* parent)
: QObject(parent) {
}

void PipelineWorker::requestCancel() {
  cancelRequested_.store(true, std::memory_order_relaxed);
}

bool PipelineWorker::cancellationRequested() const {
  return cancelRequested_.load(std::memory_order_relaxed);
}

void PipelineWorker::emitProcessOutput(const QByteArray& bytes, QByteArray& pending) {
  pending += bytes;
  for (;;) {
    const auto newline = pending.indexOf('\n');
    if (newline < 0) {
      break;
    }
    QByteArray line = pending.left(newline);
    pending.remove(0, newline + 1);
    if (line.endsWith('\r')) {
      line.chop(1);
    }
    emit logLine(QString::fromUtf8(line));
  }
}

bool PipelineWorker::runCommand(const QString& program,
                                const QStringList& arguments,
                                const QString& workingDirectory,
                                const QString& displayCommand,
                                int timeoutMs,
                                QByteArray* capturedOutput,
                                const QProcessEnvironment* environmentOverride) {
  if (cancellationRequested()) {
    return false;
  }
  emit logLine(QStringLiteral("> %1").arg(displayCommand));
  QProcess process;
  process.setWorkingDirectory(workingDirectory);
  process.setProcessChannelMode(QProcess::MergedChannels);
  auto environment = environmentOverride
    ? *environmentOverride
    : QProcessEnvironment::systemEnvironment();
  QString path = environment.value(QStringLiteral("PATH"));
  const QString additions = QStringLiteral("/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin");
  if (!path.startsWith(additions)) {
    path = additions + QStringLiteral(":") + path;
  }
  environment.insert(QStringLiteral("PATH"), path);
  process.setProcessEnvironment(environment);
  process.start(program, arguments);
  if (!process.waitForStarted(15000)) {
    emit logLine(QStringLiteral("Could not start %1: %2").arg(program, process.errorString()));
    return false;
  }

  QElapsedTimer timer;
  timer.start();
  QByteArray pending;
  QByteArray captured;
  while (process.state() != QProcess::NotRunning) {
    process.waitForReadyRead(200);
    const QByteArray chunk = process.readAll();
    if (!chunk.isEmpty()) {
      emitProcessOutput(chunk, pending);
      if (captured.size() < 16 * 1024 * 1024) {
        captured += chunk.left(16 * 1024 * 1024 - captured.size());
      }
    }
    if (cancellationRequested()) {
      process.terminate();
      if (!process.waitForFinished(3000)) {
        process.kill();
        process.waitForFinished(3000);
      }
      return false;
    }
    if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
      process.kill();
      process.waitForFinished(3000);
      emit logLine(QStringLiteral("Command timed out after %1 seconds.").arg(timeoutMs / 1000));
      return false;
    }
  }
  const QByteArray tail = process.readAll();
  if (!tail.isEmpty()) {
    emitProcessOutput(tail, pending);
    if (captured.size() < 16 * 1024 * 1024) {
      captured += tail.left(16 * 1024 * 1024 - captured.size());
    }
  }
  if (!pending.isEmpty()) {
    emit logLine(QString::fromUtf8(pending));
  }
  if (capturedOutput) {
    *capturedOutput = captured;
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    emit logLine(QStringLiteral("Command exited with status %1.").arg(process.exitCode()));
    return false;
  }
  return true;
}

void PipelineWorker::cleanupKeychain() {
  if (temporaryKeychainPath_.isEmpty()) {
    return;
  }
  QProcess process;
  process.start(QStringLiteral("/usr/bin/security"),
                { QStringLiteral("delete-keychain"), temporaryKeychainPath_ });
  process.waitForFinished(15000);
  temporaryKeychainPath_.clear();
  temporaryKeychainPassword_.clear();
}

void PipelineWorker::run(PipelineRequest request) {
  cancelRequested_.store(false, std::memory_order_relaxed);
  cleanupKeychain();
  const bool windowsTarget = request.targetPlatform == TargetPlatform::Windows;

  constexpr int totalStages = 9;
  QString sensitivePemPath;
  QString normalizedCertificatePath;
  QString normalizedCertificatePassword;
  int stage = 0;
  auto nextStage = [&](const QString& name) {
    ++stage;
    emit stageChanged(name, stage, totalStages);
    emit logLine(QStringLiteral("\n=== %1 ===").arg(name));
  };
  auto fail = [&](const QString& message, const QString& workspace) {
    if (!sensitivePemPath.isEmpty()) QFile::remove(sensitivePemPath);
    if (!normalizedCertificatePath.isEmpty()) QFile::remove(normalizedCertificatePath);
    normalizedCertificatePassword.clear();
    cleanupKeychain();
    emit logLine(QStringLiteral("ERROR: %1").arg(message));
    emit failed(message, workspace);
  };

  nextStage(QStringLiteral("Validate inputs"));
  QString error;
  DiscDescription disc;
  if (!CueSheet::parse(request.cuePath, request.selectedBinPaths, disc, error)) {
    fail(error, {});
    return;
  }
  GameDescription game;
  if (!DiscInspector::inspect(disc, game, error)) {
    fail(error, {});
    return;
  }
  if (QFileInfo(request.biosPath).fileName() != QStringLiteral("SCPH1001.BIN")) {
    fail(QStringLiteral("The BIOS file must be named exactly SCPH1001.BIN."), {});
    return;
  }
  if (QFileInfo(request.biosPath).size() != kScph1001Size) {
    fail(QStringLiteral("SCPH1001.BIN must be exactly 524,288 bytes."), {});
    return;
  }
  const QString biosHash = sha256File(request.biosPath, error);
  if (biosHash.isEmpty()) {
    fail(error, {});
    return;
  }
  if (biosHash.compare(QString::fromLatin1(kScph1001Sha256), Qt::CaseInsensitive) != 0) {
    fail(QStringLiteral("The BIOS is not the supported SCPH1001.BIN revision (SHA-256 mismatch)."), {});
    return;
  }
  if (!QFileInfo(request.iconPath).isFile()) {
    fail(QStringLiteral("Select a readable app icon."), {});
    return;
  }
  if (request.patchBiosBranding &&
      (!QFileInfo(request.biosInitialSplashPath).isFile() ||
       !QFileInfo(request.biosHandoffImagePath).isFile())) {
    fail(QStringLiteral("BIOS branding is enabled, but both splash images were not selected."), {});
    return;
  }
  if (request.windowTitle.trimmed().isEmpty()) {
    fail(QStringLiteral("Window title is required."), {});
    return;
  }
  const QString bundleName = cleanBundleName(request.windowTitle);
  if (bundleName.isEmpty()) {
    fail(QStringLiteral("The window title cannot be converted to a valid app bundle name."), {});
    return;
  }
  if (!QFileInfo(request.outputDirectory).isDir() || !QFileInfo(request.outputDirectory).isWritable()) {
    fail(QStringLiteral("Select a writable output directory."), {});
    return;
  }
  if (!windowsTarget &&
      (!QFileInfo(request.certificatePath).isFile() || request.certificatePassword.isEmpty())) {
    fail(QStringLiteral("A password-protected .pfx signing certificate and its password are required."), {});
    return;
  }
  if (!QFileInfo(request.frameworkRoot).isDir() ||
      !QFileInfo(QDir(request.frameworkRoot).filePath(QStringLiteral("runtime/runtime.cmake"))).isFile()) {
    fail(QStringLiteral("The PSXRecomp framework root is invalid."), {});
    return;
  }
  const QString analyzeHeadless = QDir(request.ghidraHome).filePath(QStringLiteral("support/analyzeHeadless"));
  if (!QFileInfo(analyzeHeadless).isExecutable()) {
    fail(QStringLiteral("The selected Ghidra installation does not provide analyzeHeadless."), {});
    return;
  }
  const QString cmake = findExecutable(QStringLiteral("cmake"));
  const QString ninja = findExecutable(QStringLiteral("ninja"));
  const QString python = findExecutable(QStringLiteral("python3"));
  const QString pkgConfig = windowsTarget ? QString() : findExecutable(QStringLiteral("pkg-config"));
  const QString nm = windowsTarget ? QString() : QStringLiteral("/usr/bin/nm");
  const QString openssl = windowsTarget ? QString() : findOpenSsl3();
  const QString mingwGcc = windowsTarget
    ? findExecutable(QStringLiteral("x86_64-w64-mingw32-gcc")) : QString();
  const QString mingwGxx = windowsTarget
    ? findExecutable(QStringLiteral("x86_64-w64-mingw32-g++")) : QString();
  const QString mingwWindres = windowsTarget
    ? findExecutable(QStringLiteral("x86_64-w64-mingw32-windres")) : QString();
  const QString mingwAr = windowsTarget
    ? findExecutable(QStringLiteral("x86_64-w64-mingw32-ar")) : QString();
  const QString mingwRanlib = windowsTarget
    ? findExecutable(QStringLiteral("x86_64-w64-mingw32-ranlib")) : QString();
  const QString mingwStrip = windowsTarget
    ? findExecutable(QStringLiteral("x86_64-w64-mingw32-strip")) : QString();
  const QString mingwObjdump = windowsTarget
    ? findExecutable(QStringLiteral("x86_64-w64-mingw32-objdump")) : QString();
  if (!windowsTarget && openssl.isEmpty()) {
    fail(QStringLiteral("OpenSSL 3 with PKCS#12 legacy-provider support is required. Install openssl@3 with Homebrew or set PSXRECOMP_OPENSSL."), {});
    return;
  }
  QStringList requiredTools{ cmake, ninja, python };
  if (windowsTarget) {
    requiredTools << mingwGcc << mingwGxx << mingwWindres << mingwAr
                  << mingwRanlib << mingwStrip << mingwObjdump;
  } else {
    requiredTools << pkgConfig << openssl
                  << QStringLiteral("/usr/bin/iconutil") << QStringLiteral("/usr/bin/security")
                  << QStringLiteral("/usr/bin/codesign") << QStringLiteral("/usr/bin/ditto")
                  << QStringLiteral("/usr/bin/otool") << nm
                  << QStringLiteral("/usr/bin/install_name_tool");
  }
  for (const auto& tool : requiredTools) {
    if (tool.isEmpty() || !QFileInfo(tool).isExecutable()) {
      fail(QStringLiteral("A required build tool is unavailable: %1").arg(tool.isEmpty() ? QStringLiteral("unknown") : tool), {});
      return;
    }
  }
  emit logLine(QStringLiteral("Target platform: %1")
                 .arg(targetPlatformDisplayName(request.targetPlatform)));
  if (windowsTarget) {
    emit logLine(QStringLiteral("MinGW C compiler: %1").arg(mingwGcc));
    emit logLine(QStringLiteral("MinGW C++ compiler: %1").arg(mingwGxx));
  }
  QString libusbVersion;
  QString libusbStaticArchive;
  if (!windowsTarget && request.macosGipGamepad) {
    QByteArray versionOutput;
    QByteArray libdirOutput;
    if (!runCommand(pkgConfig,
                    { QStringLiteral("--modversion"), QStringLiteral("libusb-1.0") },
                    request.frameworkRoot,
                    QStringLiteral("pkg-config --modversion libusb-1.0"),
                    15000, &versionOutput) ||
        !runCommand(pkgConfig,
                    { QStringLiteral("--variable=libdir"), QStringLiteral("libusb-1.0") },
                    request.frameworkRoot,
                    QStringLiteral("pkg-config --variable=libdir libusb-1.0"),
                    15000, &libdirOutput)) {
      fail(QStringLiteral("Wired Xbox/PDP controller support is enabled, but libusb-1.0 is unavailable. Install it with Homebrew and rebuild."), {});
      return;
    }
    libusbVersion = QString::fromUtf8(versionOutput).trimmed();
    const QString libusbDirectory = QString::fromUtf8(libdirOutput).trimmed();
    libusbStaticArchive = QDir(libusbDirectory).filePath(QStringLiteral("libusb-1.0.a"));
    if (!QFileInfo(libusbStaticArchive).isFile()) {
      fail(QStringLiteral("Wired Xbox/PDP controller support requires the static libusb archive for a self-contained export: %1")
             .arg(libusbStaticArchive), {});
      return;
    }
    emit logLine(QStringLiteral("macOS wired Xbox/PDP input: enabled (libusb %1, static archive %2)")
                   .arg(libusbVersion, libusbStaticArchive));
  } else if (!windowsTarget) {
    emit logLine(QStringLiteral("macOS wired Xbox/PDP input: disabled by export settings"));
  }

  emit logLine(QStringLiteral("Disc: %1 (%2)").arg(game.volumeId, game.serial));
  emit logLine(QStringLiteral("Boot EXE: %1, entry %2, load %3, text %4 bytes")
                 .arg(game.bootPath, hex32(game.entryPc), hex32(game.loadAddress))
                 .arg(game.textSize));
  emit logLine(QStringLiteral("BIOS: canonical SCPH1001.BIN (%1)").arg(biosHash));
  emit logLine(request.skipBiosBoot
                 ? QStringLiteral("Boot mode: direct to game (BIOS intro skipped)")
                 : QStringLiteral("Boot mode: real recompiled BIOS intro"));

  nextStage(QStringLiteral("Prepare reproducible workspace"));
  QTemporaryDir temporary(QDir::tempPath() + QStringLiteral("/psxrecomp-studio-XXXXXX"));
  temporary.setAutoRemove(false);
  if (!temporary.isValid()) {
    fail(QStringLiteral("Could not create the temporary build workspace."), {});
    return;
  }
  const QString workspace = temporary.path();
  const QString projectDir = QDir(workspace).filePath(QStringLiteral("project"));
  const QString proofDir = QDir(workspace).filePath(QStringLiteral("proof"));
  const QString gameDir = QDir(projectDir).filePath(QStringLiteral("game"));
  const QString biosDir = QDir(projectDir).filePath(QStringLiteral("bios"));
  const QString seedsDir = QDir(projectDir).filePath(QStringLiteral("seeds"));
  const QString generatedDir = QDir(projectDir).filePath(QStringLiteral("generated"));
  const QString biosGeneratedDir = QDir(generatedDir).filePath(QStringLiteral("bios"));
  const QString ghidraDir = QDir(workspace).filePath(QStringLiteral("ghidra"));
  const QString buildDir = QDir(workspace).filePath(QStringLiteral("build"));
  const QString stageDir = QDir(workspace).filePath(QStringLiteral("stage"));
  for (const auto& path : { projectDir, proofDir, gameDir, biosDir, seedsDir,
                            generatedDir, biosGeneratedDir, ghidraDir, buildDir, stageDir }) {
    if (!QDir().mkpath(path)) {
      fail(QStringLiteral("Could not create workspace directory %1.").arg(path), workspace);
      return;
    }
  }
  if (!writeText(QDir(projectDir).filePath(QStringLiteral(".gitignore")),
                 QStringLiteral("# Temporary PSXRecomp Studio project\n*\n"), error)) {
    fail(error, workspace);
    return;
  }
  if (!windowsTarget) {
    sensitivePemPath = QDir(workspace).filePath(QStringLiteral("signing-source.pem"));
    normalizedCertificatePath = QDir(workspace).filePath(QStringLiteral("signing-normalized.p12"));
    QProcessEnvironment certificateEnvironment = QProcessEnvironment::systemEnvironment();
    normalizedCertificatePassword =
      QUuid::createUuid().toString(QUuid::WithoutBraces) +
      QUuid::createUuid().toString(QUuid::WithoutBraces);
    certificateEnvironment.insert(QStringLiteral("PSXRECOMP_PFX_PASSWORD"),
                                  request.certificatePassword);
    certificateEnvironment.insert(QStringLiteral("PSXRECOMP_NORMALIZED_PFX_PASSWORD"),
                                  normalizedCertificatePassword);
    const QStringList extractArgs{
      QStringLiteral("pkcs12"), QStringLiteral("-in"), request.certificatePath,
      QStringLiteral("-nodes"), QStringLiteral("-out"), sensitivePemPath,
      QStringLiteral("-passin"), QStringLiteral("env:PSXRECOMP_PFX_PASSWORD")
    };
    bool certificateExtracted = runCommand(
      openssl, extractArgs, workspace,
      QStringLiteral("openssl pkcs12 -in <certificate.pfx> -nodes -out <temporary PEM> -passin <redacted>"),
      60000, nullptr, &certificateEnvironment);
    if (!certificateExtracted) {
      QStringList legacyArgs = extractArgs;
      legacyArgs.insert(1, QStringLiteral("-legacy"));
      certificateExtracted = runCommand(
        openssl, legacyArgs, workspace,
        QStringLiteral("openssl pkcs12 -legacy -in <certificate.pfx> -nodes -out <temporary PEM> -passin <redacted>"),
        60000, nullptr, &certificateEnvironment);
    }
    if (!certificateExtracted) {
      fail(QStringLiteral("The PFX password was rejected while validating the certificate."), workspace);
      return;
    }
    QFile::setPermissions(sensitivePemPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!runCommand(
          openssl,
          { QStringLiteral("pkcs12"), QStringLiteral("-legacy"),
            QStringLiteral("-export"), QStringLiteral("-in"), sensitivePemPath,
            QStringLiteral("-out"), normalizedCertificatePath,
            QStringLiteral("-passout"), QStringLiteral("env:PSXRECOMP_NORMALIZED_PFX_PASSWORD"),
            QStringLiteral("-name"), QStringLiteral("PSXRecomp Studio Signing") },
          workspace,
          QStringLiteral("openssl pkcs12 -legacy -export -in <temporary PEM> -out <normalized P12> -passout <generated>"),
          60000, nullptr, &certificateEnvironment)) {
      fail(QStringLiteral("The signing certificate could not be normalized for macOS Security."), workspace);
      return;
    }
    QFile::remove(sensitivePemPath);
    sensitivePemPath.clear();
    emit logLine(QStringLiteral("Signing certificate password validated; PKCS#12 normalized for macOS."));
  } else {
    emit logLine(QStringLiteral("Windows package signing: not requested; the MinGW export is unsigned."));
  }
  emit logLine(QStringLiteral("Workspace: %1").arg(workspace));

  const QString exePath = QDir(gameDir).filePath(game.bootFileName);
  const QString rawName = game.bootFileName + QStringLiteral("_no_header.bin");
  const QString rawPath = QDir(gameDir).filePath(rawName);
  const QString biosPath = QDir(biosDir).filePath(QStringLiteral("SCPH1001.BIN"));
  if (!writeBytes(exePath, game.executableBytes, error) ||
      !writeBytes(rawPath, game.codeBytes, error)) {
    fail(error, workspace);
    return;
  }
  if (request.patchBiosBranding) {
    BiosBrandingPatchOptions patchOptions;
    patchOptions.inputBiosPath = request.biosPath;
    patchOptions.outputBiosPath = biosPath;
    patchOptions.initialSplashImagePath = request.biosInitialSplashPath;
    patchOptions.handoffImagePath = request.biosHandoffImagePath;
    patchOptions.manifestPath = QDir(proofDir).filePath(QStringLiteral("bios_branding_patch.json"));
    patchOptions.muteBootAudio = request.biosMuteBootAudio;
    patchOptions.removeStockPsGlyph = request.biosRemoveStockPsGlyph;
    if (!BiosBrandingPatcher::patch(patchOptions, error)) {
      fail(error, workspace);
      return;
    }
    emit logLine(QStringLiteral("Bundled BIOS branding/audio patch generated."));
  } else if (!copyFileReplacing(request.biosPath, biosPath, error)) {
    fail(error, workspace);
    return;
  }
  const QString effectiveBiosHash = sha256File(biosPath, error);
  const quint32 effectiveBiosCrc = crc32File(biosPath, error);
  if (!error.isEmpty() || effectiveBiosHash.isEmpty()) {
    fail(error, workspace);
    return;
  }

  QJsonArray discFiles;
  const QString cueHash = sha256File(disc.cuePath, error);
  if (cueHash.isEmpty()) {
    fail(error, workspace);
    return;
  }
  for (const auto& file : disc.files) {
    if (cancellationRequested()) {
      QDir(workspace).removeRecursively();
      emit cancelled();
      return;
    }
    emit logLine(QStringLiteral("Hashing disc file %1").arg(file.packagedName));
    const QString hash = sha256File(file.sourcePath, error);
    if (hash.isEmpty()) {
      fail(error, workspace);
      return;
    }
    discFiles.append(QJsonObject{
      { QStringLiteral("cue_reference"), file.cueReference },
      { QStringLiteral("packaged_name"), file.packagedName },
      { QStringLiteral("size"), static_cast<double>(QFileInfo(file.sourcePath).size()) },
      { QStringLiteral("sha256"), hash },
    });
  }
  const QJsonObject discManifest{
    { QStringLiteral("schema"), 1 },
    { QStringLiteral("cue_name"), QFileInfo(disc.cuePath).fileName() },
    { QStringLiteral("cue_sha256"), cueHash },
    { QStringLiteral("volume_id"), game.volumeId },
    { QStringLiteral("serial"), game.serial },
    { QStringLiteral("boot_path"), game.bootPath },
    { QStringLiteral("files"), discFiles },
  };
  if (!writeJson(QDir(proofDir).filePath(QStringLiteral("disc_manifest.json")), discManifest, error) ||
      !writeJson(QDir(proofDir).filePath(QStringLiteral("request.json")), request.toJson(false), error)) {
    fail(error, workspace);
    return;
  }
  const QJsonObject exeProof{
    { QStringLiteral("schema"), 1 },
    { QStringLiteral("boot_file"), game.bootFileName },
    { QStringLiteral("serial"), game.serial },
    { QStringLiteral("load_address"), hex32(game.loadAddress) },
    { QStringLiteral("entry_pc"), hex32(game.entryPc) },
    { QStringLiteral("text_size"), hex32(game.textSize) },
    { QStringLiteral("stack_base"), hex32(game.stackBase) },
    { QStringLiteral("exe_sha256"), sha256File(exePath, error) },
  };
  if (!error.isEmpty() || !writeJson(QDir(proofDir).filePath(QStringLiteral("exe_header_proof.json")), exeProof, error)) {
    fail(error, workspace);
    return;
  }

  nextStage(QStringLiteral("Analyze with Ghidra"));
  const QString projectName = QStringLiteral("PSX_%1").arg(
    game.serial.isEmpty() ? sanitizedFileStem(request.windowTitle) : sanitizedFileStem(game.serial));
  const QString ghidraAnalysisPath = QDir(proofDir).filePath(QStringLiteral("ghidra_analysis.json"));
  const QString scriptsPath = QDir(request.frameworkRoot).filePath(QStringLiteral("tools/ghidra"));
  const QStringList ghidraArgs{
    ghidraDir,
    projectName,
    QStringLiteral("-import"), rawPath,
    QStringLiteral("-overwrite"),
    QStringLiteral("-processor"), QStringLiteral("MIPS:LE:32:default"),
    QStringLiteral("-loader"), QStringLiteral("BinaryLoader"),
    QStringLiteral("-loader-baseAddr"), hex32(game.loadAddress),
    QStringLiteral("-loader-blockName"), QStringLiteral(".text"),
    QStringLiteral("-scriptPath"), scriptsPath,
    QStringLiteral("-prescript"), QStringLiteral("SeedPsxEntry.java"), hex32(game.entryPc),
    QStringLiteral("-postscript"), QStringLiteral("ExportGameAnalysis.java"),
      ghidraAnalysisPath, hex32(game.entryPc), hex32(game.loadAddress),
      hex32(game.loadAddress + game.textSize),
    QStringLiteral("-analysisTimeoutPerFile"), QStringLiteral("1800"),
    QStringLiteral("-max-cpu"), QString::number(std::max(1, QThread::idealThreadCount())),
  };
  if (!runCommand(analyzeHeadless, ghidraArgs, workspace,
                  QStringLiteral("analyzeHeadless <workspace> %1 -import %2 [MIPS analysis]")
                    .arg(projectName, rawName),
                  35 * 60 * 1000)) {
    if (cancellationRequested()) {
      QDir(workspace).removeRecursively();
      emit cancelled();
    } else {
      fail(QStringLiteral("Ghidra headless analysis failed."), workspace);
    }
    return;
  }
  if (!QFileInfo(ghidraAnalysisPath).isFile()) {
    fail(QStringLiteral("Ghidra did not produce its analysis manifest."), workspace);
    return;
  }

  const auto ghidraDocument = readJson(ghidraAnalysisPath, error);
  if (ghidraDocument.isNull()) {
    fail(error, workspace);
    return;
  }
  const auto ghidraObject = ghidraDocument.object();
  QString observedEntry = ghidraObject.value(QStringLiteral("entry")).toString().toLower();
  observedEntry.remove(QStringLiteral("0x"));
  const QString expectedEntry = QStringLiteral("%1").arg(game.entryPc, 8, 16, QLatin1Char('0')).toLower();
  const QString observedLanguage = ghidraObject.value(QStringLiteral("language")).toString();
  const QString observedEntryFunction = ghidraObject.value(QStringLiteral("entry_function")).toString();
  bool memoryBlockMatches = false;
  const quint32 expectedEnd = game.loadAddress + game.textSize - 1u;
  for (const auto& blockValue : ghidraObject.value(QStringLiteral("memory_blocks")).toArray()) {
    const auto block = blockValue.toObject();
    bool okStart = false;
    bool okEnd = false;
    const quint32 startAddress = block.value(QStringLiteral("start")).toString().toUInt(&okStart, 16);
    const quint32 endAddress = block.value(QStringLiteral("end")).toString().toUInt(&okEnd, 16);
    if (okStart && okEnd && startAddress == game.loadAddress && endAddress == expectedEnd) {
      memoryBlockMatches = true;
      break;
    }
  }
  const auto entryInstructions = ghidraObject.value(QStringLiteral("entry_instructions")).toArray();
  bool entryWordMatches = false;
  QString observedWord;
  QString expectedWord;
  if (!entryInstructions.isEmpty() && game.entryPc >= game.loadAddress) {
    const auto firstInstruction = entryInstructions.first().toObject();
    observedWord = firstInstruction.value(QStringLiteral("word_little_endian")).toString().toUpper();
    const quint32 offset = game.entryPc - game.loadAddress;
    if (offset + 4u <= static_cast<quint32>(game.codeBytes.size())) {
      const auto* bytes = reinterpret_cast<const unsigned char*>(game.codeBytes.constData() + offset);
      const quint32 word = static_cast<quint32>(bytes[0]) |
                           (static_cast<quint32>(bytes[1]) << 8u) |
                           (static_cast<quint32>(bytes[2]) << 16u) |
                           (static_cast<quint32>(bytes[3]) << 24u);
      expectedWord = hex32(word);
      entryWordMatches = observedWord == expectedWord;
    }
  }
  const bool ghidraVerified =
    observedEntry == expectedEntry &&
    observedLanguage == QStringLiteral("MIPS:LE:32:default") &&
    observedEntryFunction == QStringLiteral("entry") &&
    memoryBlockMatches && entryWordMatches &&
    ghidraObject.value(QStringLiteral("function_count")).toInt() > 0;
  const QJsonObject ghidraVerification{
    { QStringLiteral("schema"), 1 },
    { QStringLiteral("status"), ghidraVerified ? QStringLiteral("CLEAN") : QStringLiteral("CONFLICT") },
    { QStringLiteral("expected_entry"), hex32(game.entryPc) },
    { QStringLiteral("observed_entry"), ghidraObject.value(QStringLiteral("entry")) },
    { QStringLiteral("entry_function"), observedEntryFunction },
    { QStringLiteral("language"), observedLanguage },
    { QStringLiteral("memory_block_matches_exe"), memoryBlockMatches },
    { QStringLiteral("expected_entry_word"), expectedWord },
    { QStringLiteral("observed_entry_word"), observedWord },
    { QStringLiteral("entry_word_matches_exe"), entryWordMatches },
    { QStringLiteral("function_count"), ghidraObject.value(QStringLiteral("function_count")) },
    { QStringLiteral("verification_source"), QStringLiteral("Ghidra headless post-analysis manifest and raw EXE bytes") },
  };
  if (!writeJson(QDir(proofDir).filePath(QStringLiteral("ghidra_verification.json")),
                 ghidraVerification, error)) {
    fail(error, workspace);
    return;
  }
  if (!ghidraVerified) {
    fail(QStringLiteral("Ghidra's analyzed entry does not match the extracted PS-X EXE; the build stopped without guessing."), workspace);
    return;
  }
  emit logLine(QStringLiteral("Headless Ghidra proof verified entry %1 and its raw instruction word. Existing Ghidra sessions were left untouched.")
                 .arg(hex32(game.entryPc)));

  nextStage(QStringLiteral("Generate native sources"));
  const QString cacheRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
                              .filePath(QStringLiteral("org.psxrecomp.studio"));
  const QString recompilerBuild = QDir(cacheRoot).filePath(QStringLiteral("recompiler-build"));
  QDir().mkpath(recompilerBuild);
  const QString recompilerSource = QDir(request.frameworkRoot).filePath(QStringLiteral("recompiler"));
  if (!runCommand(cmake,
                  { QStringLiteral("-S"), recompilerSource, QStringLiteral("-B"), recompilerBuild,
                    QStringLiteral("-G"), QStringLiteral("Ninja"),
                    QStringLiteral("-DCMAKE_BUILD_TYPE=Release") },
                  request.frameworkRoot,
                  QStringLiteral("cmake -S recompiler -B <tool-cache> -G Ninja -DCMAKE_BUILD_TYPE=Release"),
                  10 * 60 * 1000) ||
      !runCommand(cmake,
                  { QStringLiteral("--build"), recompilerBuild, QStringLiteral("--target"),
                    QStringLiteral("psxrecomp-bios"), QStringLiteral("psxrecomp-game"),
                    QStringLiteral("--parallel"), QString::number(std::max(1, QThread::idealThreadCount())) },
                  request.frameworkRoot,
                  QStringLiteral("cmake --build <tool-cache> --target psxrecomp-bios psxrecomp-game"),
                  45 * 60 * 1000)) {
    if (cancellationRequested()) {
      QDir(workspace).removeRecursively();
      emit cancelled();
    } else {
      fail(QStringLiteral("The PSXRecomp source-generation tools could not be built."), workspace);
    }
    return;
  }
  const QString gameTool = QDir(recompilerBuild).filePath(QStringLiteral("psxrecomp-game"));
  const QString biosTool = QDir(recompilerBuild).filePath(QStringLiteral("psxrecomp-bios"));
  if (!QFileInfo(gameTool).isExecutable() || !QFileInfo(biosTool).isExecutable()) {
    fail(QStringLiteral("The recompiler build completed without producing its required tools."), workspace);
    return;
  }

  const QString seedsPath = QDir(seedsDir).filePath(QStringLiteral("ghidra_funcs.txt"));
  QString seedText = QStringLiteral("# Evidence-backed function entries for %1.\n").arg(game.serial);
  seedText += QStringLiteral("%1  # PS-X EXE header entry; Ghidra function entry.\n").arg(hex32(game.entryPc));
  if (!writeText(seedsPath, seedText, error)) {
    fail(error, workspace);
    return;
  }
  const QString codegenToml = QDir(projectDir).filePath(QStringLiteral("codegen.toml"));
  const QString codegenTomlText = makeGameToml(
    request, game,
    QStringLiteral("game/%1").arg(game.bootFileName),
    disc.cuePath,
    QStringLiteral("seeds/ghidra_funcs.txt"),
    QStringLiteral("generated"));
  if (!writeText(codegenToml, codegenTomlText, error)) {
    fail(error, workspace);
    return;
  }

  const QString outStem = game.bootFileName;
  const QString rangesPath = QDir(generatedDir).filePath(outStem + QStringLiteral("_full.ranges"));
  const QString dataRangesPath = QDir(generatedDir).filePath(outStem + QStringLiteral("_data_ranges.json"));
  const QString dynamicTargetsPath = QDir(generatedDir).filePath(outStem + QStringLiteral("_dynamic_targets.json"));
  const auto analysisDocument = readJson(ghidraAnalysisPath, error);
  if (analysisDocument.isNull()) {
    fail(error, workspace);
    return;
  }
  const QJsonArray ghidraFunctions = analysisDocument.object().value(QStringLiteral("functions")).toArray();
  QSet<quint32> seeded{ game.entryPc };
  QJsonArray iterationProof;
  bool coverageComplete = false;
  for (int iteration = 1; iteration <= 4; ++iteration) {
    if (!runCommand(gameTool,
                    { QStringLiteral("--config"), codegenToml },
                    projectDir,
                    QStringLiteral("psxrecomp-game --config codegen.toml (pass %1)").arg(iteration),
                    45 * 60 * 1000)) {
      if (cancellationRequested()) {
        QDir(workspace).removeRecursively();
        emit cancelled();
      } else {
        fail(QStringLiteral("Game source generation failed."), workspace);
      }
      return;
    }

    QString parseError;
    const auto generatedEntries = parseFunctionEntries(rangesPath, parseError);
    if (!parseError.isEmpty()) {
      fail(parseError, workspace);
      return;
    }
    const auto dataRanges = readDataRanges(dataRangesPath, parseError);
    if (!parseError.isEmpty()) {
      fail(parseError, workspace);
      return;
    }
    const auto dynamicTargets = readDynamicTargets(dynamicTargetsPath, parseError);
    if (!parseError.isEmpty()) {
      fail(parseError, workspace);
      return;
    }

    QList<QJsonObject> missing;
    QJsonArray conflicts;
    QJsonArray runtimeInstalled;
    for (const auto& functionValue : ghidraFunctions) {
      const auto function = functionValue.toObject();
      bool ok = false;
      const quint32 address = function.value(QStringLiteral("entry")).toString().toUInt(&ok, 16);
      if (!ok) {
        fail(QStringLiteral("Ghidra produced an invalid function address."), workspace);
        return;
      }
      if (addressInRanges(address, dataRanges)) {
        if (dynamicTargets.contains(address) &&
            !ghidraFunctionHasStaticExit(game, function)) {
          runtimeInstalled.append(function);
        } else {
          conflicts.append(function);
        }
      } else if (!generatedEntries.contains(address) && !seeded.contains(address)) {
        missing.append(function);
      }
    }
    QJsonArray missingJson;
    for (const auto& function : missing) {
      missingJson.append(function);
    }
    iterationProof.append(QJsonObject{
      { QStringLiteral("iteration"), iteration },
      { QStringLiteral("generated_entry_count"), generatedEntries.size() },
      { QStringLiteral("new_ghidra_seed_count"), missing.size() },
      { QStringLiteral("data_conflicts"), conflicts },
      { QStringLiteral("runtime_installed_targets"), runtimeInstalled },
      { QStringLiteral("new_ghidra_seeds"), missingJson },
    });
    if (!conflicts.isEmpty()) {
      QJsonObject conflictProof{
        { QStringLiteral("schema"), 1 },
        { QStringLiteral("status"), QStringLiteral("CONFLICT") },
        { QStringLiteral("iterations"), iterationProof },
      };
      writeJson(QDir(proofDir).filePath(QStringLiteral("ghidra_coverage_proof.json")),
                conflictProof, error);
      fail(QStringLiteral("Ghidra and the recompiler disagree about code versus data; the build stopped without guessing."), workspace);
      return;
    }
    if (missing.isEmpty()) {
      coverageComplete = true;
      break;
    }

    QFile seedFile(seedsPath);
    if (!seedFile.open(QIODevice::Append | QIODevice::Text)) {
      fail(QStringLiteral("Could not extend the Ghidra seed manifest."), workspace);
      return;
    }
    for (const auto& function : missing) {
      const quint32 address = function.value(QStringLiteral("entry")).toString().toUInt(nullptr, 16);
      seedFile.write(QStringLiteral("%1  # Ghidra %2, body %3-%4.\n")
                       .arg(hex32(address),
                            escapedComment(function.value(QStringLiteral("name")).toString()),
                            function.value(QStringLiteral("body_min")).toString(),
                            function.value(QStringLiteral("body_max")).toString())
                       .toUtf8());
      seeded.insert(address);
    }
    seedFile.close();
    emit logLine(QStringLiteral("Added %1 uncovered Ghidra function entries to the seed manifest.")
                   .arg(missing.size()));
  }
  if (!coverageComplete) {
    fail(QStringLiteral("Ghidra-backed function coverage did not converge after four source-generation passes."), workspace);
    return;
  }
  const QJsonObject coverageProof{
    { QStringLiteral("schema"), 1 },
    { QStringLiteral("status"), QStringLiteral("CLEAN") },
    { QStringLiteral("ghidra_function_count"), ghidraFunctions.size() },
    { QStringLiteral("final_seed_count"), seeded.size() },
    { QStringLiteral("iterations"), iterationProof },
  };
  if (!writeJson(QDir(proofDir).filePath(QStringLiteral("ghidra_coverage_proof.json")),
                 coverageProof, error)) {
    fail(error, workspace);
    return;
  }

  if (!copyFileReplacing(dynamicTargetsPath,
                         QDir(proofDir).filePath(QStringLiteral("dynamic_targets.json")),
                         error)) {
    fail(error, workspace);
    return;
  }

  const QString biosSeeds = QDir(request.frameworkRoot)
                              .filePath(QStringLiteral("recompiler/seeds/phase2_ghidra_seeds.json"));
  if (!runCommand(biosTool,
                  { biosPath, biosGeneratedDir, QStringLiteral("--emit-full"), biosSeeds },
                  projectDir,
                  QStringLiteral("psxrecomp-bios SCPH1001.BIN <generated> --emit-full <verified-seeds>"),
                  45 * 60 * 1000)) {
    if (cancellationRequested()) {
      QDir(workspace).removeRecursively();
      emit cancelled();
    } else {
      fail(QStringLiteral("BIOS source generation failed."), workspace);
    }
    return;
  }

  const QString auditScript = QDir(request.frameworkRoot).filePath(QStringLiteral("tools/codegen_audit.py"));
  const QString auditProof = QDir(proofDir).filePath(QStringLiteral("codegen_audit.json"));
  if (!runCommand(python,
                  { auditScript, QStringLiteral("--config"), codegenToml,
                    QStringLiteral("--json-out"), auditProof },
                  projectDir,
                  QStringLiteral("python3 codegen_audit.py --config codegen.toml --json-out <proof>"),
                  20 * 60 * 1000)) {
    fail(QStringLiteral("Generated-code verification found an unresolved issue."), workspace);
    return;
  }

  nextStage(windowsTarget ? QStringLiteral("Create Windows app project")
                          : QStringLiteral("Create macOS app project"));
  const QString bundleId = sanitizedBundleIdentifier(game.serial, request.windowTitle);
  const QString iconPath = QDir(projectDir).filePath(
    windowsTarget ? QStringLiteral("AppIcon.ico") : QStringLiteral("AppIcon.icns"));
  const QString platformMetadataPath = QDir(projectDir).filePath(
    windowsTarget ? QStringLiteral("AppIcon.rc") : QStringLiteral("Info.plist"));
  const QString dependencyCachePath = QDir(
    QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
      .filePath(QStringLiteral("dependencies"));
  if (windowsTarget && !QDir().mkpath(dependencyCachePath)) {
    fail(QStringLiteral("Could not create the Studio dependency cache: %1")
           .arg(dependencyCachePath), workspace);
    return;
  }
  if (windowsTarget) {
    if (!createIco(request.iconPath, iconPath, error) ||
        !writeText(platformMetadataPath, makeWindowsResource(iconPath), error)) {
      fail(error, workspace);
      return;
    }
  } else if (!createIcns(request.iconPath, workspace, iconPath, error) ||
             !writeText(platformMetadataPath,
                        makeInfoPlist(bundleId, bundleName, bundleName), error)) {
    fail(error, workspace);
    return;
  }
  const QString gameFull = QDir(generatedDir).filePath(outStem + QStringLiteral("_full.c"));
  const QString gameDispatch = QDir(generatedDir).filePath(outStem + QStringLiteral("_dispatch.c"));
  const QString biosFull = QDir(biosGeneratedDir).filePath(QStringLiteral("SCPH1001_full.c"));
  const QString biosDispatch = QDir(biosGeneratedDir).filePath(QStringLiteral("SCPH1001_dispatch.c"));
  for (const auto& path : { gameFull, gameDispatch, biosFull, biosDispatch }) {
    if (!QFileInfo(path).isFile()) {
      fail(QStringLiteral("A required generated source file is missing: %1").arg(path), workspace);
      return;
    }
  }
  const QString cmakeListsPath = QDir(projectDir).filePath(QStringLiteral("CMakeLists.txt"));
  if (!writeText(cmakeListsPath,
                 makeProjectCMake(request, game, bundleName, bundleId, platformMetadataPath,
                                  iconPath, dependencyCachePath,
                                  biosFull, biosDispatch, gameFull, gameDispatch,
                                  effectiveBiosCrc),
                 error)) {
    fail(error, workspace);
    return;
  }
  const QString mingwToolchainPath = QDir(projectDir).filePath(QStringLiteral("mingw-toolchain.cmake"));
  if (windowsTarget &&
      !writeText(mingwToolchainPath,
                 makeMingwToolchain(mingwGcc, mingwGxx, mingwWindres,
                                    mingwAr, mingwRanlib, mingwStrip), error)) {
    fail(error, workspace);
    return;
  }

  nextStage(windowsTarget ? QStringLiteral("Cross-compile Windows app")
                          : QStringLiteral("Compile native app"));
  QStringList configureArguments{
    QStringLiteral("-S"), projectDir, QStringLiteral("-B"), buildDir,
    QStringLiteral("-G"), QStringLiteral("Ninja"),
    QStringLiteral("-DCMAKE_BUILD_TYPE=Release"),
    QStringLiteral("-DPSX_LAUNCHER=OFF"),
    QStringLiteral("-DPSX_DEBUG_TOOLS=OFF")
  };
  if (windowsTarget) {
    configureArguments << QStringLiteral("-DCMAKE_TOOLCHAIN_FILE=%1").arg(mingwToolchainPath)
                       << QStringLiteral("-DPSX_STATIC_RUNTIME=ON")
                       << QStringLiteral("-DPSX_MACOS_GIP_GAMEPAD=OFF");
  } else {
    configureArguments << QStringLiteral("-DPSX_MACOS_GIP_GAMEPAD=%1")
                            .arg(request.macosGipGamepad ? QStringLiteral("ON")
                                                       : QStringLiteral("OFF"));
  }
  if (!runCommand(cmake,
                  configureArguments,
                  workspace,
                  windowsTarget
                    ? QStringLiteral("cmake -S <project> -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=<mingw-toolchain> -DCMAKE_BUILD_TYPE=Release")
                    : QStringLiteral("cmake -S <project> -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release"),
                  15 * 60 * 1000) ||
      !runCommand(cmake,
                  { QStringLiteral("--build"), buildDir, QStringLiteral("--target"),
                    QStringLiteral("psx-runtime"), QStringLiteral("--parallel"),
                    QString::number(std::max(1, QThread::idealThreadCount())) },
                  workspace,
                  QStringLiteral("cmake --build <build> --target psx-runtime"),
                  2 * 60 * 60 * 1000) ||
      !runCommand(cmake,
                  { QStringLiteral("--install"), buildDir, QStringLiteral("--prefix"), stageDir },
                  workspace,
                  QStringLiteral("cmake --install <build> --prefix <stage>"),
                  20 * 60 * 1000)) {
    if (cancellationRequested()) {
      QDir(workspace).removeRecursively();
      emit cancelled();
    } else {
      fail(windowsTarget
             ? QStringLiteral("The Windows app could not be cross-compiled and staged with MinGW.")
             : QStringLiteral("The native macOS app could not be compiled and staged."),
           workspace);
    }
    return;
  }
  const QString stagedApp = windowsTarget
    ? stageDir
    : QDir(stageDir).filePath(bundleName + QStringLiteral(".app"));
  if ((!windowsTarget && !QFileInfo(stagedApp).isDir()) ||
      (windowsTarget && !QFileInfo(stageDir).isDir())) {
    fail(QStringLiteral("CMake did not stage the expected %1 output: %2")
           .arg(targetPlatformDisplayName(request.targetPlatform), stagedApp), workspace);
    return;
  }
  const QString mainExecutable = windowsTarget
    ? QDir(stageDir).filePath(bundleName + QStringLiteral(".exe"))
    : QDir(stagedApp).filePath(QStringLiteral("Contents/MacOS/") + bundleName);
  if (!QFileInfo(mainExecutable).isFile()) {
    fail(QStringLiteral("CMake did not stage the expected executable: %1").arg(mainExecutable), workspace);
    return;
  }
  bool gipBackendCompiled = false;
  if (!windowsTarget) {
    QByteArray controllerSymbols;
    if (!runCommand(nm, { QStringLiteral("-gU"), mainExecutable }, workspace,
                    QStringLiteral("nm -gU <app executable>"), 30000,
                    &controllerSymbols)) {
      fail(QStringLiteral("The exported controller backend symbols could not be inspected."), workspace);
      return;
    }
    gipBackendCompiled = nmOutputHasMacosGipBackend(controllerSymbols);
    if (gipBackendCompiled != request.macosGipGamepad) {
      fail(request.macosGipGamepad
             ? QStringLiteral("The Studio export requested wired Xbox/PDP support, but the native GIP backend is absent from the app executable.")
             : QStringLiteral("The Studio export disabled wired Xbox/PDP support, but GIP backend symbols are still present."),
           workspace);
      return;
    }
    emit logLine(request.macosGipGamepad
                   ? QStringLiteral("Verified native Xbox GIP controller symbols in exported executable.")
                   : QStringLiteral("Verified Xbox GIP controller backend is absent from exported executable."));
    const QJsonObject controllerBackendProof{
      { QStringLiteral("schema"), 1 },
      { QStringLiteral("requested"), request.macosGipGamepad },
      { QStringLiteral("compiled"), gipBackendCompiled },
      { QStringLiteral("transport"), request.macosGipGamepad
          ? QStringLiteral("libusb-1.0 static") : QStringLiteral("disabled") },
      { QStringLiteral("libusb_version"), libusbVersion },
      { QStringLiteral("libusb_static_archive"), libusbStaticArchive },
      { QStringLiteral("player1_device"), QStringLiteral("auto") },
      { QStringLiteral("symbol_verification"), QStringLiteral("nm -gU") },
    };
    if (!writeJson(QDir(proofDir).filePath(QStringLiteral("macos_gip_controller.json")),
                   controllerBackendProof, error)) {
      fail(error, workspace);
      return;
    }
  }
  nextStage(QStringLiteral("Embed disc, BIOS, and proof"));
  const QString resourcesDir = windowsTarget
    ? stagedApp
    : QDir(stagedApp).filePath(QStringLiteral("Contents/Resources"));
  const QString packagedBiosDir = QDir(resourcesDir).filePath(QStringLiteral("bios"));
  const QString packagedGameDir = QDir(resourcesDir).filePath(QStringLiteral("game"));
  const QString packagedDiscDir = QDir(resourcesDir).filePath(QStringLiteral("disc"));
  const QString packagedSeedsDir = QDir(resourcesDir).filePath(QStringLiteral("seeds"));
  for (const auto& path : { resourcesDir, packagedBiosDir, packagedGameDir,
                            packagedDiscDir, packagedSeedsDir }) {
    if (!QDir().mkpath(path)) {
      fail(QStringLiteral("Could not create app resource directory %1.").arg(path), workspace);
      return;
    }
  }
  if (!copyFileReplacing(biosPath, QDir(packagedBiosDir).filePath(QStringLiteral("SCPH1001.BIN")), error) ||
      !copyFileReplacing(exePath, QDir(packagedGameDir).filePath(game.bootFileName), error) ||
      !copyFileReplacing(seedsPath, QDir(packagedSeedsDir).filePath(QStringLiteral("ghidra_funcs.txt")), error) ||
      !copyFileReplacing(seedsPath, QDir(proofDir).filePath(QStringLiteral("ghidra_funcs.txt")), error)) {
    fail(error, workspace);
    return;
  }
  for (const auto& file : disc.files) {
    emit logLine(QStringLiteral("Embedding %1").arg(file.packagedName));
    if (!copyFileReplacing(file.sourcePath, QDir(packagedDiscDir).filePath(file.packagedName), error)) {
      fail(error, workspace);
      return;
    }
  }
  const QString packagedCueName = QFileInfo(disc.cuePath).fileName();
  if (!writeText(QDir(packagedDiscDir).filePath(packagedCueName), disc.rewrittenCue, error)) {
    fail(error, workspace);
    return;
  }
  const QString finalToml = makeGameToml(
    request, game,
    QStringLiteral("game/%1").arg(game.bootFileName),
    QStringLiteral("disc/%1").arg(packagedCueName),
    QStringLiteral("seeds/ghidra_funcs.txt"),
    QStringLiteral("generated"));
  if (!writeText(QDir(resourcesDir).filePath(QStringLiteral("game.toml")), finalToml, error)) {
    fail(error, workspace);
    return;
  }

  QByteArray gitOutput;
  runCommand(QStringLiteral("/usr/bin/git"),
             { QStringLiteral("-C"), request.frameworkRoot, QStringLiteral("rev-parse"), QStringLiteral("HEAD") },
             request.frameworkRoot, QStringLiteral("git -C <framework> rev-parse HEAD"), 15000, &gitOutput);
  const QJsonObject buildManifest{
    { QStringLiteral("schema"), 1 },
    { QStringLiteral("created_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
    { QStringLiteral("platform"), targetPlatformKey(request.targetPlatform) },
    { QStringLiteral("target_architecture"), windowsTarget
        ? QStringLiteral("x86_64-w64-mingw32") : QSysInfo::currentCpuArchitecture() },
    { QStringLiteral("bundle_name"), bundleName },
    { QStringLiteral("bundle_identifier"), bundleId },
    { QStringLiteral("window_title"), request.windowTitle },
    { QStringLiteral("serial"), game.serial },
    { QStringLiteral("source_bios_sha256"), biosHash },
    { QStringLiteral("effective_bios_sha256"), effectiveBiosHash },
    { QStringLiteral("effective_bios_crc32"), QStringLiteral("0x%1").arg(effectiveBiosCrc, 8, 16, QLatin1Char('0')).toUpper() },
    { QStringLiteral("bios_branding_patched"), request.patchBiosBranding },
    { QStringLiteral("skip_bios_boot"), request.skipBiosBoot },
    { QStringLiteral("macos_gip_gamepad_requested"), !windowsTarget && request.macosGipGamepad },
    { QStringLiteral("macos_gip_gamepad_compiled"), gipBackendCompiled },
    { QStringLiteral("libusb_version"), libusbVersion },
    { QStringLiteral("framework_commit"), QString::fromUtf8(gitOutput).trimmed() },
    { QStringLiteral("cmake"), versionText(cmake, { QStringLiteral("--version") }).section('\n', 0, 0) },
    { QStringLiteral("ninja"), versionText(ninja, { QStringLiteral("--version") }) },
    { QStringLiteral("ghidra_home"), request.ghidraHome },
    { QStringLiteral("host_architecture"), QSysInfo::currentCpuArchitecture() },
    { QStringLiteral("compiler"), windowsTarget
        ? versionText(mingwGcc, { QStringLiteral("--version") }).section('\n', 0, 0)
        : versionText(QStringLiteral("/usr/bin/clang"), { QStringLiteral("--version") }).section('\n', 0, 0) },
    { QStringLiteral("sdl2_source"), windowsTarget
        ? QStringLiteral("SDL2-devel-2.32.10-mingw.tar.gz") : QStringLiteral("host pkg-config") },
    { QStringLiteral("sdl2_sha256"), windowsTarget
        ? QStringLiteral("83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2")
        : QString() },
  };
  if (!writeJson(QDir(proofDir).filePath(QStringLiteral("build_manifest.json")), buildManifest, error)) {
    fail(error, workspace);
    return;
  }
  const QString proofArchive = QDir(resourcesDir).filePath(QStringLiteral("PSXRecomp-Proof.zip"));
  if (!createProofArchive(proofDir, proofArchive, error)) {
    fail(error, workspace);
    return;
  }

  if (windowsTarget) {
    nextStage(QStringLiteral("Verify Windows package"));
    QByteArray peOutput;
    if (!runCommand(mingwObjdump,
                    { QStringLiteral("-p"), mainExecutable }, workspace,
                    QStringLiteral("x86_64-w64-mingw32-objdump -p <Windows executable>"),
                    30000, &peOutput)) {
      fail(QStringLiteral("The staged Windows executable could not be inspected."), workspace);
      return;
    }
    const QString peText = QString::fromUtf8(peOutput);
    if (!peText.contains(QStringLiteral("file format pei-x86-64")) ||
        !peText.contains(QRegularExpression(QStringLiteral(
          R"((?m)^Subsystem\s+00000002\s+\(Windows GUI\))")))) {
      fail(QStringLiteral("The staged executable is not an x86-64 Windows GUI PE binary."), workspace);
      return;
    }

    QJsonArray importedDlls;
    const QRegularExpression dllPattern(QStringLiteral(R"((?m)^\s*DLL Name:\s*(\S+)\s*$)"));
    auto matches = dllPattern.globalMatch(peText);
    QStringList forbiddenImports;
    while (matches.hasNext()) {
      const QString dll = matches.next().captured(1);
      importedDlls.append(dll);
      if (dll.compare(QStringLiteral("SDL2.dll"), Qt::CaseInsensitive) == 0 ||
          dll.startsWith(QStringLiteral("libgcc_"), Qt::CaseInsensitive) ||
          dll.startsWith(QStringLiteral("libstdc++"), Qt::CaseInsensitive) ||
          dll.startsWith(QStringLiteral("libwinpthread"), Qt::CaseInsensitive)) {
        forbiddenImports.append(dll);
      }
    }
    if (!forbiddenImports.isEmpty()) {
      fail(QStringLiteral("The Windows executable is not self-contained; non-system DLL imports remain: %1")
             .arg(forbiddenImports.join(QStringLiteral(", "))), workspace);
      return;
    }
    const QString stagedExeHash = sha256File(mainExecutable, error);
    const QJsonObject windowsProof{
      { QStringLiteral("schema"), 1 },
      { QStringLiteral("platform"), QStringLiteral("windows") },
      { QStringLiteral("architecture"), QStringLiteral("x86_64") },
      { QStringLiteral("toolchain"), QStringLiteral("x86_64-w64-mingw32") },
      { QStringLiteral("compiler"), versionText(mingwGcc, { QStringLiteral("--version") }).section('\n', 0, 0) },
      { QStringLiteral("executable"), QFileInfo(mainExecutable).fileName() },
      { QStringLiteral("executable_sha256"), stagedExeHash },
      { QStringLiteral("subsystem"), QStringLiteral("Windows GUI") },
      { QStringLiteral("imported_dlls"), importedDlls },
      { QStringLiteral("self_contained_runtime"), true },
      { QStringLiteral("signed"), false },
      { QStringLiteral("verification"), QStringLiteral("MinGW objdump PE header and import-table inspection") },
    };
    if (stagedExeHash.isEmpty() ||
        !writeJson(QDir(proofDir).filePath(QStringLiteral("windows_binary_verification.json")),
                   windowsProof, error) ||
        !createProofArchive(proofDir, proofArchive, error)) {
      fail(error, workspace);
      return;
    }

    nextStage(QStringLiteral("Deliver Windows package"));
    const QString outputPackage = QDir(request.outputDirectory).filePath(
      bundleName + QStringLiteral("-Windows"));
    const QString deliveryToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString deliveryPackage = QDir(request.outputDirectory).filePath(
      QStringLiteral(".%1.psxrecomp-new-%2-Windows").arg(bundleName, deliveryToken));
    const QString backupPackage = QDir(request.outputDirectory).filePath(
      QStringLiteral(".%1.psxrecomp-old-%2-Windows").arg(bundleName, deliveryToken));
    QDir(deliveryPackage).removeRecursively();
    QDir(backupPackage).removeRecursively();
    if (!copyDirectoryTree(stageDir, deliveryPackage, error)) {
      QDir(deliveryPackage).removeRecursively();
      fail(error, workspace);
      return;
    }
    const QString deliveredStagingExe = QDir(deliveryPackage).filePath(
      bundleName + QStringLiteral(".exe"));
    if (sha256File(deliveredStagingExe, error) != stagedExeHash) {
      QDir(deliveryPackage).removeRecursively();
      fail(QStringLiteral("The Windows executable changed while staging the package for delivery."), workspace);
      return;
    }

    const bool hadExistingOutput = QFileInfo::exists(outputPackage);
    if (hadExistingOutput && !request.overwriteOutput) {
      QDir(deliveryPackage).removeRecursively();
      fail(QStringLiteral("The Windows output already exists and overwrite was not approved: %1")
             .arg(outputPackage), workspace);
      return;
    }
    if (hadExistingOutput && !QDir().rename(outputPackage, backupPackage)) {
      QDir(deliveryPackage).removeRecursively();
      fail(QStringLiteral("Could not preserve the existing Windows package before replacement: %1")
             .arg(outputPackage), workspace);
      return;
    }
    if (!QDir().rename(deliveryPackage, outputPackage)) {
      if (hadExistingOutput) QDir().rename(backupPackage, outputPackage);
      QDir(deliveryPackage).removeRecursively();
      fail(QStringLiteral("Could not move the verified Windows package into its final output path."), workspace);
      return;
    }
    const QString outputExe = QDir(outputPackage).filePath(bundleName + QStringLiteral(".exe"));
    const bool deliveryVerified = sha256File(outputExe, error) == stagedExeHash &&
      QFileInfo(QDir(outputPackage).filePath(QStringLiteral("PSXRecomp-Proof.zip"))).isFile();
    if (!deliveryVerified) {
      const QString failedPackage = outputPackage + QStringLiteral(".failed-") + deliveryToken;
      QDir().rename(outputPackage, failedPackage);
      if (hadExistingOutput) QDir().rename(backupPackage, outputPackage);
      fail(QStringLiteral("The delivered Windows package failed final verification; the failed copy remains at %1")
             .arg(failedPackage), workspace);
      return;
    }
    if (hadExistingOutput && !QDir(backupPackage).removeRecursively()) {
      emit logLine(QStringLiteral("Warning: previous Windows package backup remains at %1")
                     .arg(backupPackage));
    }
    emit logLine(QStringLiteral("Windows app package created: %1").arg(outputPackage));
    QDir(workspace).removeRecursively();
    emit completed(outputPackage);
    return;
  }

  QByteArray dependencyOutput;
  if (!runCommand(QStringLiteral("/usr/bin/otool"),
                  { QStringLiteral("-L"), mainExecutable }, workspace,
                  QStringLiteral("otool -L <app executable>"), 30000, &dependencyOutput)) {
    fail(QStringLiteral("The staged app dependencies could not be inspected."), workspace);
    return;
  }
  const QString dependencyText = QString::fromUtf8(dependencyOutput);
  if (request.macosGipGamepad &&
      dependencyText.contains(QStringLiteral("libusb"), Qt::CaseInsensitive)) {
    fail(QStringLiteral("The exported app links libusb dynamically; Studio requires the verified static backend for a self-contained signed app."), workspace);
    return;
  }
  const QRegularExpression sdlPattern(
    QStringLiteral(R"sdl((?m)^\s+([^\s]+/libSDL2-2\.0\.0\.dylib)\s)sdl"));
  const auto sdlMatch = sdlPattern.match(dependencyText);
  if (sdlMatch.hasMatch()) {
    const QString originalSdlPath = sdlMatch.captured(1);
    const QString canonicalSdlPath = QFileInfo(originalSdlPath).canonicalFilePath();
    if (canonicalSdlPath.isEmpty()) {
      fail(QStringLiteral("The linked SDL2 library could not be resolved: %1").arg(originalSdlPath), workspace);
      return;
    }
    const QString frameworksDir = QDir(stagedApp).filePath(QStringLiteral("Contents/Frameworks"));
    if (!QDir().mkpath(frameworksDir)) {
      fail(QStringLiteral("Could not create the app Frameworks directory."), workspace);
      return;
    }
    const QString bundledSdlPath = QDir(frameworksDir).filePath(QStringLiteral("libSDL2-2.0.0.dylib"));
    if (!copyFileReplacing(canonicalSdlPath, bundledSdlPath, error)) {
      fail(error, workspace);
      return;
    }
    QFile::setPermissions(bundledSdlPath,
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
      QFileDevice::ReadGroup | QFileDevice::ExeGroup |
      QFileDevice::ReadOther | QFileDevice::ExeOther);
    if (!runCommand(QStringLiteral("/usr/bin/install_name_tool"),
                    { QStringLiteral("-id"), QStringLiteral("@rpath/libSDL2-2.0.0.dylib"),
                      bundledSdlPath }, workspace,
                    QStringLiteral("install_name_tool -id @rpath/libSDL2-2.0.0.dylib <bundled SDL2>"),
                    30000) ||
        !runCommand(QStringLiteral("/usr/bin/install_name_tool"),
                    { QStringLiteral("-change"), originalSdlPath,
                      QStringLiteral("@rpath/libSDL2-2.0.0.dylib"), mainExecutable },
                    workspace,
                    QStringLiteral("install_name_tool -change <Homebrew SDL2> @rpath/libSDL2-2.0.0.dylib <app>"),
                    30000)) {
      fail(QStringLiteral("The SDL2 dependency could not be embedded in the app bundle."), workspace);
      return;
    }
    QByteArray fixedDependencies;
    if (!runCommand(QStringLiteral("/usr/bin/otool"),
                    { QStringLiteral("-L"), mainExecutable }, workspace,
                    QStringLiteral("otool -L <fixed app executable>"), 30000, &fixedDependencies) ||
        !QString::fromUtf8(fixedDependencies).contains(QStringLiteral("@rpath/libSDL2-2.0.0.dylib"))) {
      fail(QStringLiteral("The bundled executable still references an external SDL2 library."), workspace);
      return;
    }
    emit logLine(QStringLiteral("Bundled SDL2: %1").arg(canonicalSdlPath));

    /* Homebrew's current `sdl2` formula is SDL2-compat, whose constructor
     * dlopens SDL3 from @loader_path/libSDL3.dylib instead of recording it as
     * a Mach-O dependency. `otool -L` therefore cannot reveal it. Detect the
     * compat keg from the resolved source path and bundle the matching SDL3
     * runtime beside SDL2, which is exactly the first path SDL2-compat probes. */
    if (canonicalSdlPath.contains(QStringLiteral("sdl2-compat"), Qt::CaseInsensitive)) {
      QString sdl3Source;
      const QStringList candidates{
        QStringLiteral("/usr/local/opt/sdl3/lib/libSDL3.dylib"),
        QStringLiteral("/opt/homebrew/opt/sdl3/lib/libSDL3.dylib"),
        QStringLiteral("/usr/local/lib/libSDL3.dylib"),
        QStringLiteral("/opt/homebrew/lib/libSDL3.dylib")
      };
      for (const QString& candidate : candidates) {
        const QString canonical = QFileInfo(candidate).canonicalFilePath();
        if (!canonical.isEmpty()) { sdl3Source = canonical; break; }
      }
      if (sdl3Source.isEmpty()) {
        fail(QStringLiteral("The linked SDL2 library is SDL2-compat, but its required SDL3 runtime was not found."), workspace);
        return;
      }
      const QString bundledSdl3Path = QDir(frameworksDir).filePath(QStringLiteral("libSDL3.dylib"));
      if (!copyFileReplacing(sdl3Source, bundledSdl3Path, error)) {
        fail(error, workspace);
        return;
      }
      QFile::setPermissions(bundledSdl3Path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::ExeOther);
      if (!runCommand(QStringLiteral("/usr/bin/install_name_tool"),
                      { QStringLiteral("-id"), QStringLiteral("@loader_path/libSDL3.dylib"),
                        bundledSdl3Path }, workspace,
                      QStringLiteral("install_name_tool -id @loader_path/libSDL3.dylib <bundled SDL3>"),
                      30000)) {
        fail(QStringLiteral("The SDL3 compatibility runtime could not be embedded."), workspace);
        return;
      }
      emit logLine(QStringLiteral("Bundled SDL3 for SDL2-compat: %1").arg(sdl3Source));
    }
  } else {
    emit logLine(QStringLiteral("No dynamic SDL2 dependency was present; the runtime is already self-contained."));
  }


  const QString appEntitlementsPath =
    QDir(workspace).filePath(QStringLiteral("App.entitlements"));
  if (!writeText(appEntitlementsPath,
                 QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                "<plist version=\"1.0\"><dict>\n"
                                "<key>com.apple.security.cs.disable-library-validation</key><true/>\n"
                                "</dict></plist>\n"), error)) {
    fail(error, workspace);
    return;
  }

  nextStage(QStringLiteral("Sign and verify app"));
  temporaryKeychainPath_ = QDir(workspace).filePath(QStringLiteral("signing.keychain-db"));
  temporaryKeychainPassword_ = QUuid::createUuid().toString(QUuid::WithoutBraces)
                                   + QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!runCommand(QStringLiteral("/usr/bin/security"),
                  { QStringLiteral("create-keychain"), QStringLiteral("-p"), temporaryKeychainPassword_,
                    temporaryKeychainPath_ },
                  workspace, QStringLiteral("security create-keychain <temporary>"), 30000) ||
      !runCommand(QStringLiteral("/usr/bin/security"),
                  { QStringLiteral("set-keychain-settings"), QStringLiteral("-lut"), QStringLiteral("21600"),
                    temporaryKeychainPath_ },
                  workspace, QStringLiteral("security set-keychain-settings <temporary>"), 30000) ||
      !runCommand(QStringLiteral("/usr/bin/security"),
                  { QStringLiteral("unlock-keychain"), QStringLiteral("-p"), temporaryKeychainPassword_,
                    temporaryKeychainPath_ },
                  workspace, QStringLiteral("security unlock-keychain <temporary>"), 30000)) {
    fail(QStringLiteral("The temporary signing keychain could not be created."), workspace);
    return;
  }
  bool certificateImported = runCommand(
    QStringLiteral("/usr/bin/security"),
    { QStringLiteral("import"), normalizedCertificatePath,
      QStringLiteral("-k"), temporaryKeychainPath_,
      QStringLiteral("-P"), normalizedCertificatePassword,
      QStringLiteral("-T"), QStringLiteral("/usr/bin/codesign"),
      QStringLiteral("-T"), QStringLiteral("/usr/bin/security") },
    workspace,
    QStringLiteral("security import <normalized certificate> -k <temporary> -P <redacted>"),
    60000);
  if (!certificateImported ||
      !runCommand(QStringLiteral("/usr/bin/security"),
                  { QStringLiteral("set-key-partition-list"), QStringLiteral("-S"),
                    QStringLiteral("apple-tool:,apple:"), QStringLiteral("-s"),
                    QStringLiteral("-k"), temporaryKeychainPassword_, temporaryKeychainPath_ },
                  workspace, QStringLiteral("security set-key-partition-list <temporary>"), 60000)) {
    fail(QStringLiteral("The normalized certificate could not be imported into the temporary signing keychain."), workspace);
    return;
  }
  QFile::remove(normalizedCertificatePath);
  normalizedCertificatePath.clear();
  normalizedCertificatePassword.clear();
  QByteArray identityOutput;
  if (!runCommand(QStringLiteral("/usr/bin/security"),
                  { QStringLiteral("find-identity"), QStringLiteral("-v"), QStringLiteral("-p"),
                    QStringLiteral("codesigning"), temporaryKeychainPath_ },
                  workspace, QStringLiteral("security find-identity -v -p codesigning <temporary>"),
                  30000, &identityOutput)) {
    fail(QStringLiteral("The imported PFX contains no usable code-signing identity."), workspace);
    return;
  }
  const QString identityText = QString::fromUtf8(identityOutput);
  const QRegularExpression identityPattern(
    QStringLiteral(R"identity(([0-9A-Fa-f]{40})\s+"([^"]+)")identity"));
  const auto identityMatch = identityPattern.match(identityText);
  if (!identityMatch.hasMatch()) {
    fail(QStringLiteral("The imported PFX contains no usable code-signing identity."), workspace);
    return;
  }
  const QString identityHash = identityMatch.captured(1);
  const QString identityName = identityMatch.captured(2);
  emit logLine(QStringLiteral("Signing identity: %1").arg(identityName));

  auto signPath = [&](const QString& path, const QString& label, bool appCode) -> bool {
    runCommand(QStringLiteral("/usr/bin/codesign"),
               { QStringLiteral("--remove-signature"), path }, workspace,
               QStringLiteral("codesign --remove-signature %1").arg(label), 30000);
    QStringList arguments{
      QStringLiteral("--force"), QStringLiteral("--timestamp"),
      QStringLiteral("--keychain"), temporaryKeychainPath_,
      QStringLiteral("--sign"), identityHash
    };
    if (appCode) {
      arguments << QStringLiteral("--options") << QStringLiteral("runtime")
                << QStringLiteral("--entitlements") << appEntitlementsPath;
    }
    arguments << path;
    return runCommand(QStringLiteral("/usr/bin/codesign"), arguments, workspace,
                      QStringLiteral("codesign %1").arg(label), 5 * 60 * 1000);
  };
  QDirIterator binaries(stagedApp, QDir::Files, QDirIterator::Subdirectories);
  QStringList nestedCode;
  while (binaries.hasNext()) {
    const QString path = binaries.next();
    const QFileInfo info(path);
    if (info.suffix().compare(QStringLiteral("dylib"), Qt::CaseInsensitive) == 0 ||
        path.contains(QStringLiteral(".framework/Versions/"))) {
      nestedCode.append(path);
    }
  }
  std::sort(nestedCode.begin(), nestedCode.end(), [](const QString& lhs, const QString& rhs) {
    return lhs.count('/') > rhs.count('/');
  });
  for (const auto& path : nestedCode) {
    if (!signPath(path, QFileInfo(path).fileName(), false)) {
      fail(QStringLiteral("A nested library could not be code-signed."), workspace);
      return;
    }
  }
  if (!QFileInfo(mainExecutable).isExecutable() || !signPath(mainExecutable, QStringLiteral("main executable"), true) ||
      !signPath(stagedApp, QStringLiteral("app bundle (pass 1)"), true) ||
      !runCommand(QStringLiteral("/usr/bin/codesign"),
                  { QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"),
                    QStringLiteral("--verbose=4"), stagedApp },
                  workspace, QStringLiteral("codesign --verify --deep --strict <app>"), 60000)) {
    fail(QStringLiteral("The staged app did not pass code-signature verification."), workspace);
    return;
  }

  const QJsonObject signingProof{
    { QStringLiteral("schema"), 1 },
    { QStringLiteral("identity_sha1"), identityHash },
    { QStringLiteral("identity_name"), identityName },
    { QStringLiteral("hardened_runtime"), true },
    { QStringLiteral("disable_library_validation"), true },
    { QStringLiteral("secure_timestamp"), true },
    { QStringLiteral("keychain_independent_verification"), true },
    { QStringLiteral("verification"), QStringLiteral("codesign --verify --deep --strict passed before final seal and after temporary-keychain removal") },
  };
  if (!writeJson(QDir(proofDir).filePath(QStringLiteral("signature_verification.json")),
                 signingProof, error) ||
      !createProofArchive(proofDir, proofArchive, error) ||
      !signPath(stagedApp, QStringLiteral("app bundle (final seal)"), true) ||
      !runCommand(QStringLiteral("/usr/bin/codesign"),
                  { QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"),
                    QStringLiteral("--verbose=4"), stagedApp },
                  workspace, QStringLiteral("codesign --verify --deep --strict <final app>"), 60000)) {
    fail(error.isEmpty() ? QStringLiteral("The final app seal could not be verified.") : error, workspace);
    return;
  }

  nextStage(QStringLiteral("Deliver signed app"));
  const QString outputApp = QDir(request.outputDirectory).filePath(bundleName + QStringLiteral(".app"));
  const QString deliveryToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString deliveryApp = QDir(request.outputDirectory).filePath(
    QStringLiteral(".%1.psxrecomp-new-%2.app").arg(bundleName, deliveryToken));
  const QString backupContents = QDir(request.outputDirectory).filePath(
    QStringLiteral(".%1.psxrecomp-old-%2.contents").arg(bundleName, deliveryToken));
  const QString failedContents = QDir(request.outputDirectory).filePath(
    QStringLiteral(".%1.psxrecomp-failed-%2.contents").arg(bundleName, deliveryToken));
  auto removeTree = [&](const QString& path, bool required) -> bool {
    if (!QFileInfo::exists(path)) return true;
    runCommand(QStringLiteral("/bin/chmod"),
               { QStringLiteral("-R"), QStringLiteral("u+rwX"), path }, workspace,
               QStringLiteral("chmod -R u+rwX <old app>"), 60000);
    const bool removed = runCommand(QStringLiteral("/bin/rm"),
                                    { QStringLiteral("-rf"), path }, workspace,
                                    QStringLiteral("rm -rf <old app>"), 10 * 60 * 1000);
    if (!removed && required)
      emit logLine(QStringLiteral("Could not remove app tree: %1").arg(path));
    return removed;
  };
  auto moveTree = [&](const QString& source, const QString& destination,
                      const QString& label) -> bool {
    return runCommand(QStringLiteral("/bin/mv"), { source, destination }, workspace,
                      label, 10 * 60 * 1000);
  };

  removeTree(deliveryApp, false);
  removeTree(backupContents, false);
  removeTree(failedContents, false);
  if (!runCommand(QStringLiteral("/usr/bin/ditto"),
                  { stagedApp, deliveryApp }, workspace,
                  QStringLiteral("ditto <signed app> <delivery staging app>"),
                  60 * 60 * 1000) ||
      !runCommand(QStringLiteral("/usr/bin/codesign"),
                  { QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"),
                    QStringLiteral("--verbose=4"), deliveryApp },
                  workspace, QStringLiteral("codesign --verify --deep --strict <delivery staging app>"),
                  60000)) {
    removeTree(deliveryApp, false);
    fail(QStringLiteral("The signed app could not be staged and verified in the output directory."), workspace);
    return;
  }

  bool hadExistingOutput = QFileInfo::exists(outputApp);
  if (hadExistingOutput && !request.overwriteOutput) {
    removeTree(deliveryApp, false);
    fail(QStringLiteral("The output app already exists and overwrite was not approved: %1").arg(outputApp), workspace);
    return;
  }
  if (!hadExistingOutput) {
    if (!moveTree(deliveryApp, outputApp, QStringLiteral("mv <verified staging app> <final app>"))) {
      removeTree(deliveryApp, false);
      fail(QStringLiteral("Could not move the verified app into its final output path."), workspace);
      return;
    }
  } else {
    /* The macOS Desktop may carry a deny-delete ACL which prevents renaming or
     * deleting an existing .app root even for its owner. Preserve that root and
     * swap only Contents. Rollback and diagnostic directories MUST remain next
     * to the app: any extra directory in the bundle root invalidates its seal. */
    const QString activeContents =
      QDir(outputApp).filePath(QStringLiteral("Contents"));
    const QString stagedContents =
      QDir(deliveryApp).filePath(QStringLiteral("Contents"));
    if (!moveTree(activeContents, backupContents,
                  QStringLiteral("mv <old Contents> <sibling rollback Contents>")) ||
        !moveTree(stagedContents, activeContents,
                  QStringLiteral("mv <new Contents> <active Contents>"))) {
      if (!QFileInfo::exists(activeContents) && QFileInfo::exists(backupContents)) {
        moveTree(backupContents, activeContents,
                 QStringLiteral("restore <sibling rollback Contents>"));
      }
      removeTree(deliveryApp, false);
      removeTree(failedContents, false);
      fail(QStringLiteral("Could not atomically replace the existing app contents: %1").arg(outputApp), workspace);
      return;
    }
    if (!runCommand(QStringLiteral("/usr/bin/codesign"),
                    { QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"),
                      QStringLiteral("--verbose=4"), outputApp },
                    workspace, QStringLiteral("codesign --verify --deep --strict <contents-swapped app>"),
                    60000)) {
      const bool quarantined = moveTree(
        activeContents, failedContents,
        QStringLiteral("mv <failed Contents> <sibling diagnostic Contents>"));
      const bool restored = quarantined && moveTree(
        backupContents, activeContents,
        QStringLiteral("restore <sibling rollback Contents>"));
      if (restored) {
        removeTree(failedContents, false);
      }
      removeTree(deliveryApp, false);
      if (!restored) {
        fail(QStringLiteral("The replacement app failed verification and automatic rollback also failed. "
                            "The previous Contents remain at: %1").arg(backupContents), workspace);
      } else {
        fail(QStringLiteral("The replacement app failed verification; the previous Contents were restored."), workspace);
      }
      return;
    }
    if (!removeTree(backupContents, false))
      emit logLine(QStringLiteral("Warning: previous Contents backup remains at %1").arg(backupContents));
    if (!removeTree(deliveryApp, false))
      emit logLine(QStringLiteral("Warning: delivery staging app remains at %1").arg(deliveryApp));
    removeTree(failedContents, false);
  }
  cleanupKeychain();
  emit logLine(QStringLiteral("Temporary signing keychain removed; verifying delivered app independently."));
  if (!runCommand(QStringLiteral("/usr/bin/codesign"),
                  { QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"),
                    QStringLiteral("--verbose=4"), outputApp },
                  workspace, QStringLiteral("codesign --verify --deep --strict <delivered app after keychain removal>"), 60000)) {
    fail(QStringLiteral("The delivered app signature depends on the temporary keychain or is otherwise invalid."), workspace);
    return;
  }
  emit logLine(QStringLiteral("Signed app created: %1").arg(outputApp));
  QDir(workspace).removeRecursively();
  emit completed(outputApp);
}

} // namespace psxstudio
