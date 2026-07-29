#include "CueSheet.h"
#include "DiscCatalog.h"
#include "DiscInspector.h"
#include "GbaSupport.h"
#include "PipelineSupport.h"
#include "PipelineWorker.h"

#include "quazip.h"
#include "quazipfile.h"
#include "quazipfileinfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const QString& message) {
  if (!condition) {
    ++failures;
    qCritical().noquote() << "FAIL:" << message;
  }
}

void write733(char* dst, quint32 value) {
  qToLittleEndian(value, reinterpret_cast<uchar*>(dst));
  qToBigEndian(value, reinterpret_cast<uchar*>(dst + 4));
}

QByteArray directoryRecord(const QByteArray& name, quint32 lba, quint32 size, bool directory) {
  const int padding = (name.size() % 2 == 0) ? 1 : 0;
  QByteArray record(33 + name.size() + padding, 0);
  record[0] = static_cast<char>(record.size());
  write733(record.data() + 2, lba);
  write733(record.data() + 10, size);
  record[25] = directory ? 2 : 0;
  record[28] = 1;
  record[31] = 1;
  record[32] = static_cast<char>(name.size());
  std::memcpy(record.data() + 33, name.constData(), static_cast<size_t>(name.size()));
  return record;
}

bool makeTestGba(const QString& path, QString& error) {
  QByteArray rom(1 << 20, static_cast<char>(0xff));
  qToLittleEndian<quint32>(0xEA00002Eu,
                           reinterpret_cast<uchar*>(rom.data()));
  for (int i = 4; i < 0xA0; ++i) rom[i] = static_cast<char>((i * 37 + 11) & 0xff);
  std::memcpy(rom.data() + 0xA0, "STUDIO TEST ", 12);
  std::memcpy(rom.data() + 0xAC, "TSTE", 4);
  std::memcpy(rom.data() + 0xB0, "01", 2);
  rom[0xB2] = static_cast<char>(0x96);
  rom[0xB3] = 0;
  rom[0xBC] = 0;
  quint32 sum = 0x19;
  for (int offset = 0xA0; offset <= 0xBC; ++offset)
    sum += static_cast<uchar>(rom.at(offset));
  rom[0xBD] = static_cast<char>((-static_cast<qint32>(sum)) & 0xff);
  std::memcpy(rom.data() + 0x1000, "FLASH1M_V123", 12);
  return psxstudio::writeBytes(path, rom, error);
}

bool makeTestDisc(const QString& path, QString& error) {
  constexpr int sectorSize = 2048;
  QByteArray image(24 * sectorSize, 0);
  char* pvd = image.data() + 16 * sectorSize;
  pvd[0] = 1;
  std::memcpy(pvd + 1, "CD001", 5);
  pvd[6] = 1;
  std::memcpy(pvd + 40, "PSXRECOMP STUDIO TEST", 21);
  const auto rootRecord = directoryRecord(QByteArray(1, 0), 20, sectorSize, true);
  std::memcpy(pvd + 156, rootRecord.constData(), static_cast<size_t>(rootRecord.size()));

  QByteArray directory;
  directory += directoryRecord(QByteArray(1, 0), 20, sectorSize, true);
  directory += directoryRecord(QByteArray(1, 1), 20, sectorSize, true);
  const QByteArray systemCnf("BOOT = cdrom:\\TEST_000.00;1\r\n");
  directory += directoryRecord("SYSTEM.CNF;1", 21, systemCnf.size(), false);
  directory += directoryRecord("TEST_000.00;1", 22, 2052, false);
  std::memcpy(image.data() + 20 * sectorSize, directory.constData(), static_cast<size_t>(directory.size()));
  std::memcpy(image.data() + 21 * sectorSize, systemCnf.constData(), static_cast<size_t>(systemCnf.size()));

  QByteArray executable(2052, 0);
  std::memcpy(executable.data(), "PS-X EXE", 8);
  qToLittleEndian<quint32>(0x80010000u, reinterpret_cast<uchar*>(executable.data() + 0x10));
  qToLittleEndian<quint32>(0x80010000u, reinterpret_cast<uchar*>(executable.data() + 0x18));
  qToLittleEndian<quint32>(4u, reinterpret_cast<uchar*>(executable.data() + 0x1c));
  qToLittleEndian<quint32>(0x801ffff0u, reinterpret_cast<uchar*>(executable.data() + 0x30));
  std::memcpy(image.data() + 22 * sectorSize, executable.constData(), static_cast<size_t>(executable.size()));
  return psxstudio::writeBytes(path, image, error);
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc >= 6 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--gba-pipeline-smoke")) {
    psxstudio::PipelineRequest request;
    request.system = psxstudio::SystemKind::GameBoyAdvance;
    request.targetPlatform = psxstudio::hostTargetPlatform();
    if (argc >= 7) {
      request.targetPlatform = psxstudio::targetPlatformFromKey(QString::fromLocal8Bit(argv[6]));
    }
    const QString smokeMode = QString::fromLocal8Bit(argv[5]);
    request.exportMode = smokeMode.startsWith(QStringLiteral("source"))
      ? psxstudio::ExportMode::Source : psxstudio::ExportMode::Build;
    request.romPath = QString::fromLocal8Bit(argv[2]);
    request.biosPath = QString::fromLocal8Bit(argv[3]);
    request.outputDirectory = QString::fromLocal8Bit(argv[4]);
    request.frameworkRoot = QDir::cleanPath(
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../..")));
    request.powerEngineRoot = qEnvironmentVariable("POWERENGINE_ROOT");
    request.llvmRoot = qEnvironmentVariable("VIRTUA_LLVM_ROOT");
    request.ghidraHome = qEnvironmentVariable("GHIDRA_HOME");
    request.windowTitle = argc >= 8 ? QString::fromLocal8Bit(argv[7])
                                    : QStringLiteral("GBA Studio Smoke");
    request.exportAsZip = QString::fromLocal8Bit(argv[5]).endsWith(QStringLiteral("zip"));
    request.overwriteOutput = true;
    psxstudio::PipelineWorker worker;
    bool ok = false;
    QString failure;
    QObject::connect(&worker, &psxstudio::PipelineWorker::logLine,
                     [](const QString& line) { qInfo().noquote() << line; });
    QObject::connect(&worker, &psxstudio::PipelineWorker::completed,
                     [&](const QString& path) { ok = true; qInfo().noquote() << path; });
    QObject::connect(&worker, &psxstudio::PipelineWorker::failed,
                     [&](const QString& message, const QString& workspace) {
                       failure = message + QStringLiteral("\n") + workspace;
                     });
    worker.run(request);
    if (!ok) qCritical().noquote() << failure;
    return ok ? 0 : 1;
  }
  QTemporaryDir temp;
  check(temp.isValid(), QStringLiteral("temporary directory creation"));
  if (!temp.isValid()) {
    return 1;
  }

  QString error;
  const QString fallbackIcns = QDir(temp.path()).filePath(QStringLiteral("fallback.icns"));
  check(psxstudio::createIcns(
          QStringLiteral(":/psxrecomp/studio/resources/psxrecomp-studio.svg"),
          temp.path(), fallbackIcns, error), error);
  QFile fallbackIcnsFile(fallbackIcns);
  check(fallbackIcnsFile.open(QIODevice::ReadOnly) &&
          fallbackIcnsFile.read(4) == QByteArray("icns", 4) &&
          fallbackIcnsFile.size() > 8,
        QStringLiteral("PNG-backed ICNS fallback repairs iconutil failures"));
  fallbackIcnsFile.close();

  const QString gbaPath = QDir(temp.path()).filePath(QStringLiteral("studio-test.gba"));
  check(makeTestGba(gbaPath, error), error);
  psxstudio::GbaDescription gbaDescription;
  check(psxstudio::inspectGbaRom(gbaPath, gbaDescription, error) &&
          gbaDescription.title == QStringLiteral("STUDIO TEST") &&
          gbaDescription.gameCode == QStringLiteral("TSTE") &&
          gbaDescription.entryTarget == 0x080000C0u &&
          gbaDescription.headerBranchValid && gbaDescription.complementValid &&
          gbaDescription.saveType == QStringLiteral("flash1m") &&
          gbaDescription.warning.isEmpty(),
        error.isEmpty() ? QStringLiteral("GBA ROM header inspection") : error);
  QList<psxstudio::GbaCatalogEntry> gbaCatalog;
  QStringList gbaWarnings;
  check(psxstudio::scanGbaDirectory(temp.path(), gbaCatalog, gbaWarnings, error) &&
          gbaCatalog.size() == 1 &&
          gbaCatalog.constFirst().gameCode == QStringLiteral("TSTE"),
        error.isEmpty() ? QStringLiteral("GBA batch directory scan") : error);
  psxstudio::PipelineRequest gbaRequest;
  gbaRequest.system = psxstudio::SystemKind::GameBoyAdvance;
  gbaRequest.targetPlatform = psxstudio::TargetPlatform::MacOS;
  gbaRequest.romPath = gbaPath;
  gbaRequest.windowTitle = QStringLiteral("Studio Test Advance");
  check(psxstudio::systemKindFromKey(QStringLiteral("GBA")) ==
          psxstudio::SystemKind::GameBoyAdvance &&
          psxstudio::systemKindKey(gbaRequest.system) == QStringLiteral("gba") &&
          gbaRequest.toJson().value(QStringLiteral("system")).toString() ==
            QStringLiteral("gba") &&
          gbaRequest.toJson().value(QStringLiteral("rom_path")).toString() == gbaPath,
        QStringLiteral("GBA request system and ROM serialization"));
  const QString gbaMain = psxstudio::generatedGbaMainCpp(
    QStringLiteral("Studio Test Advance"), QString(40, QLatin1Char('a')), 0x12345678u);
  check(gbaMain.contains(QStringLiteral("parent_path().parent_path() / \"Resources\"")) &&
          gbaMain.contains(QStringLiteral("--save-path")) &&
          gbaMain.contains(QStringLiteral("GBARECOMP_ASSET_CACHE_DIR")) &&
          gbaMain.contains(QStringLiteral("builtin_rom_sha1")),
        QStringLiteral("generated GBA entry point resolves packaged resources and writable saves"));
  const QString gbaToml = psxstudio::generatedGbaGameToml(
    gbaRequest, gbaDescription, QString(40, QLatin1Char('a')), 0x12345678u,
    QString(40, QLatin1Char('b')), 0x87654321u, false);
  check(gbaToml.contains(QStringLiteral("path = \"bios/gba_bios.bin\"")) &&
          gbaToml.contains(QStringLiteral("path = \"game/game.gba\"")) &&
          gbaToml.contains(QStringLiteral("short_name = \"StudioTestAdvance\"")) &&
          gbaToml.contains(QStringLiteral("hle = false")) &&
          gbaToml.contains(QStringLiteral("shadow = false")) &&
          gbaToml.contains(QStringLiteral("type = \"flash1m\"")),
        QStringLiteral("generated GBA runtime configuration pins packaged inputs"));
  // The runtime silently ignores keys it does not know, so an inert section
  // would read as configuration that does something. Only keys gbarecomp
  // actually dispatches on may appear.
  for (const auto& inert : { QStringLiteral("[recompiler]"), QStringLiteral("[runtime]"),
                             QStringLiteral("window_title"), QStringLiteral("entry_point"),
                             QStringLiteral("debug_port"), QStringLiteral("interpreter") }) {
    check(!gbaToml.contains(inert),
          QStringLiteral("generated GBA game.toml omits the inert key %1").arg(inert));
  }
  const QString gbaHleToml = psxstudio::generatedGbaGameToml(
    gbaRequest, gbaDescription, QString(40, QLatin1Char('a')), 0x12345678u,
    QString(40, QLatin1Char('b')), 0x87654321u, true);
  check(gbaHleToml.contains(QStringLiteral("hle = true")) &&
          gbaHleToml.contains(QStringLiteral("hle_keep_intro = false")),
        QStringLiteral("GBA boot skip selects the runtime HLE boot path"));
  const QString gbaRecompilerToml = psxstudio::generatedGbaRecompilerToml(
    gbaRequest, gbaDescription, QString(40, QLatin1Char('a')));
  check(gbaRecompilerToml.contains(QStringLiteral("[program]")) &&
          gbaRecompilerToml.contains(QStringLiteral("load_address = 0x08000000")) &&
          gbaRecompilerToml.contains(QStringLiteral("entry_pc = 0x080000C0")) &&
          gbaRecompilerToml.contains(
            QStringLiteral("size = %1").arg(gbaDescription.romSize)) &&
          gbaRecompilerToml.contains(QStringLiteral("[identity]")) &&
          gbaRecompilerToml.contains(
            QStringLiteral("sha1 = \"%1\"").arg(QString(40, QLatin1Char('a')))),
        QStringLiteral("generated gba_recompile configuration pins the image it was derived from"));
  const QString gbaCmake = psxstudio::generatedGbaProjectCMake(
    gbaRequest, gbaDescription, QStringLiteral("Studio Test Advance"),
    QStringLiteral("org.psxrecomp.gba.test"));
  check(gbaCmake.contains(QStringLiteral("project(GeneratedGbaRecompPackage")) &&
          gbaCmake.contains(QStringLiteral("generated/recompiled_*.cpp")) &&
          gbaCmake.contains(QStringLiteral("generated/dispatch_table.cpp")) &&
          gbaCmake.contains(QStringLiteral("generated_bios/bios_recompiled.cpp")) &&
          gbaCmake.contains(QStringLiteral("add_subdirectory(gbarecomp")) &&
          gbaCmake.contains(QStringLiteral("target_link_gbarecomp_runtime_stack")) &&
          gbaCmake.contains(QStringLiteral("package_inputs/gba_bios.bin")) &&
          gbaCmake.contains(QStringLiteral("add_custom_target(gba-runtime")) &&
          gbaCmake.contains(QStringLiteral("steganos-package/gba-runtime")) &&
          !gbaCmake.contains(QStringLiteral("GBA_CARGO")) &&
          !gbaCmake.contains(QStringLiteral("add_executable(psx-runtime")),
        QStringLiteral("generated GBA CMake compiles the recompiled cartridge and BIOS"));
  // An export that produced no translation must stop the build rather than
  // quietly ship an executable with nothing recompiled in it.
  // A separate root: CMake's compiler probe drops *.bin files into its build
  // tree, and the disc catalog scan below recurses. Sharing `temp` would make
  // those probe artifacts look like PlayStation tracks.
  QTemporaryDir gbaCmakeTemp;
  check(gbaCmakeTemp.isValid(), QStringLiteral("gbarecomp CMake fixture root"));
  const QString gbaProject =
    QDir(gbaCmakeTemp.path()).filePath(QStringLiteral("generated-gbarecomp-project"));
  const QString gbaBuild =
    QDir(gbaCmakeTemp.path()).filePath(QStringLiteral("generated-gbarecomp-build"));
  check(psxstudio::writeText(QDir(gbaProject).filePath(QStringLiteral("CMakeLists.txt")),
                             gbaCmake, error) &&
        QDir().mkpath(QDir(gbaProject).filePath(QStringLiteral("generated"))) &&
        QDir().mkpath(QDir(gbaProject).filePath(QStringLiteral("package_inputs"))),
        error.isEmpty() ? QStringLiteral("generated gbarecomp CMake fixture") : error);
  QProcess gbaConfigure;
  gbaConfigure.start(QStringLiteral("cmake"),
    { QStringLiteral("-S"), gbaProject, QStringLiteral("-B"), gbaBuild,
      QStringLiteral("-G"), QStringLiteral("Ninja"),
      QStringLiteral("-DCMAKE_BUILD_TYPE=Release") });
  const bool gbaConfigureRan = gbaConfigure.waitForStarted(5000) &&
                               gbaConfigure.waitForFinished(30000) &&
                               gbaConfigure.exitStatus() == QProcess::NormalExit;
  const QString gbaConfigureError = QString::fromUtf8(gbaConfigure.readAllStandardError());
  check(gbaConfigureRan && gbaConfigure.exitCode() != 0 &&
          gbaConfigureError.contains(QStringLiteral("no recompiled translation units")),
        QStringLiteral("generated gbarecomp CMake refuses an empty translation: %1")
          .arg(gbaConfigureError));

  const QString fakeGhidraHome = QDir(temp.path()).filePath(QStringLiteral("ghidra"));
  const QString fakeGhidraProperties =
    QDir(fakeGhidraHome).filePath(QStringLiteral("Ghidra/application.properties"));
#if defined(Q_OS_WIN)
  const QString fakeGhidraLauncher =
    QDir(fakeGhidraHome).filePath(QStringLiteral("support/analyzeHeadless.bat"));
#else
  const QString fakeGhidraLauncher =
    QDir(fakeGhidraHome).filePath(QStringLiteral("support/analyzeHeadless"));
#endif
  check(psxstudio::writeText(fakeGhidraProperties, QStringLiteral("application.name=Ghidra"), error),
        error);
  check(psxstudio::writeText(fakeGhidraLauncher, QStringLiteral("launcher"), error), error);
#if !defined(Q_OS_WIN)
  check(QFile::setPermissions(fakeGhidraLauncher,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner),
        QStringLiteral("fake Ghidra launcher permissions"));
#endif
  check(psxstudio::ghidraAnalyzeHeadlessPath(fakeGhidraHome) == fakeGhidraLauncher,
        QStringLiteral("host Ghidra analyzeHeadless launcher resolution"));

  const QString repositoryFixture = QDir(temp.path()).filePath(QStringLiteral("source-repository"));
  const QString repositoryCopy = QDir(temp.path()).filePath(QStringLiteral("source-repository-copy"));
  check(psxstudio::writeText(
          QDir(repositoryFixture).filePath(QStringLiteral(".git/HEAD")),
          QStringLiteral("ref: refs/heads/main\n"), error) &&
        psxstudio::writeText(
          QDir(repositoryFixture).filePath(QStringLiteral("CMakeLists.txt")),
          QStringLiteral("cmake_minimum_required(VERSION 3.20)\n"), error), error);
  check(psxstudio::copyDirectoryTree(repositoryFixture, repositoryCopy, error) &&
          QFileInfo(QDir(repositoryCopy).filePath(QStringLiteral(".git/HEAD"))).isFile(),
        error.isEmpty() ? QStringLiteral("source delivery preserves initialized Git metadata") : error);

  check(psxstudio::javaMajorVersion(
          QStringLiteral("openjdk 21.0.11 2026-04-21\nOpenJDK Runtime Environment")) == 21,
        QStringLiteral("OpenJDK 21 version parsing"));
  check(psxstudio::javaMajorVersion(
          QStringLiteral("java version \"21.0.8\" 2025-07-15 LTS")) == 21,
        QStringLiteral("Oracle JDK 21 legacy version parsing"));
  check(psxstudio::javaMajorVersion(
          QStringLiteral("java 21.0.8 2025-07-15 LTS")) == 21,
        QStringLiteral("Oracle JDK 21 modern version parsing"));
  check(psxstudio::javaMajorVersion(
          QStringLiteral("openjdk 24.0.2 2025-07-15")) == 24,
        QStringLiteral("non-21 Java version rejection input"));
#if defined(Q_OS_MACOS)
  if (QFileInfo(QStringLiteral("/usr/local/opt/openjdk@21/bin/java")).isExecutable() ||
      QFileInfo(QStringLiteral("/opt/homebrew/opt/openjdk@21/bin/java")).isExecutable()) {
    const QString detectedJavaHome = psxstudio::findJava21Home();
    check(!detectedJavaHome.isEmpty(),
          QStringLiteral("keg-only Homebrew JDK 21 discovery"));
    QProcess java;
    java.start(QDir(detectedJavaHome).filePath(QStringLiteral("bin/java")),
               { QStringLiteral("--version") });
    check(java.waitForStarted(3000) && java.waitForFinished(5000) &&
            java.exitStatus() == QProcess::NormalExit && java.exitCode() == 0 &&
            psxstudio::javaMajorVersion(QString::fromUtf8(
              java.readAllStandardOutput() + java.readAllStandardError())) == 21,
          QStringLiteral("discovered macOS Java home executes JDK 21"));
  }
#endif

  const QString binPath = QDir(temp.path()).filePath(QStringLiteral("test.bin"));
  const QString cuePath = QDir(temp.path()).filePath(QStringLiteral("test.cue"));
  check(makeTestDisc(binPath, error), error);
  check(psxstudio::writeText(cuePath,
                  QStringLiteral("FILE \"test.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n"),
                  error), error);
  const QString batchId = psxstudio::batchEntrySettingsId(cuePath);
  check(batchId.size() == 64 &&
          batchId == psxstudio::batchEntrySettingsId(QFileInfo(cuePath).absoluteFilePath()) &&
          batchId != psxstudio::batchEntrySettingsId(binPath),
        QStringLiteral("stable distinct batch settings identity"));

  psxstudio::DiscDescription disc;
  check(psxstudio::CueSheet::parse(cuePath, {}, disc, error), error);
  check(disc.files.size() == 1, QStringLiteral("CUE file count"));
  check(disc.rewrittenCue.contains(QStringLiteral("\"test.bin\"")),
        QStringLiteral("CUE rewrite"));

  psxstudio::DiscDescription standaloneDisc;
  check(psxstudio::CueSheet::parse(binPath, {}, standaloneDisc, error), error);
  check(standaloneDisc.files.size() == 1 && standaloneDisc.rewrittenCue.isEmpty() &&
          standaloneDisc.files.constFirst().sourcePath == QFileInfo(binPath).canonicalFilePath(),
        QStringLiteral("standalone BIN parsing"));

  psxstudio::GameDescription game;
  check(psxstudio::DiscInspector::inspect(disc, game, error), error);
  check(game.bootFileName == QStringLiteral("TEST_000.00"), QStringLiteral("boot file detection"));
  check(game.serial == QStringLiteral("TEST-00000"), QStringLiteral("serial normalization"));
  check(game.entryPc == 0x80010000u, QStringLiteral("entry PC parsing"));
  check(game.textSize == 4u, QStringLiteral("text-size parsing"));
  psxstudio::GameDescription standaloneGame;
  check(psxstudio::DiscInspector::inspect(standaloneDisc, standaloneGame, error), error);
  check(standaloneGame.serial == game.serial && standaloneGame.entryPc == game.entryPc,
        QStringLiteral("standalone BIN inspection"));

  const QString standaloneBinPath =
    QDir(temp.path()).filePath(QStringLiteral("standalone.BiN"));
  check(QFile::copy(binPath, standaloneBinPath),
        QStringLiteral("standalone catalog fixture copy"));
  const QString incompleteBinPath =
    QDir(temp.path()).filePath(QStringLiteral("incomplete.bin"));
  const QString brokenCuePath =
    QDir(temp.path()).filePath(QStringLiteral("broken.cue"));
  check(QFile::copy(binPath, incompleteBinPath),
        QStringLiteral("incomplete CUE fixture copy"));
  check(psxstudio::writeText(
          brokenCuePath,
          QStringLiteral("FILE \"incomplete.bin\" BINARY\n"
                         "  TRACK 01 MODE1/2048\n"
                         "    INDEX 01 00:00:00\n"
                         "FILE \"missing-track.bin\" BINARY\n"
                         "  TRACK 02 AUDIO\n"
                         "    INDEX 01 00:00:00\n"), error), error);
  QList<psxstudio::DiscCatalogEntry> catalog;
  QStringList catalogWarnings;
  check(psxstudio::DiscCatalog::scanDirectory(
          temp.path(), catalog, catalogWarnings, error), error);
  check(catalog.size() == 2,
        QStringLiteral("batch catalog groups a CUE set and an unowned standalone BIN"));
  check(catalogWarnings.size() == 1 &&
          catalogWarnings.constFirst().contains(QStringLiteral("missing-track.bin")),
        QStringLiteral("batch catalog reports an incomplete CUE (got: %1)")
          .arg(catalogWarnings.isEmpty() ? QStringLiteral("<none>")
                                         : catalogWarnings.join(QStringLiteral(" | "))));
  const auto catalogHasSource = [&](const QString& path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return std::any_of(catalog.cbegin(), catalog.cend(), [&](const auto& entry) {
      return entry.sourcePath == canonical;
    });
  };
  check(catalogHasSource(cuePath) && catalogHasSource(standaloneBinPath) &&
          !catalogHasSource(binPath) && !catalogHasSource(incompleteBinPath),
        QStringLiteral("batch catalog never reclassifies CUE-owned tracks as standalone discs"));

  check(psxstudio::sanitizedFileStem(QStringLiteral("Top Gun: Fire at Will!")) ==
          QStringLiteral("TopGunFireAtWill"),
        QStringLiteral("app-name sanitization"));
  check(psxstudio::sanitizedBundleIdentifier(QStringLiteral("SLUS-00032"), {}) ==
          QStringLiteral("org.psxrecomp.generated.slus-00032"),
        QStringLiteral("bundle-identifier sanitization"));
  // Apostrophes must not appear in package / .app names — they break Ninja
  // shell rules on macOS (see cleanBundleName).
  check(psxstudio::cleanBundleName(QStringLiteral("The King of Fighters '98")) ==
          QStringLiteral("The King of Fighters 98"),
        QStringLiteral("bundle-name strips ASCII apostrophe"));
  check(psxstudio::cleanBundleName(QStringLiteral("The King of Fighters \u201998")) ==
          QStringLiteral("The King of Fighters 98"),
        QStringLiteral("bundle-name strips curly apostrophe"));
  check(psxstudio::cleanBundleName(QStringLiteral("Top Gun: Fire at Will!")) ==
          QStringLiteral("Top Gun - Fire at Will!"),
        QStringLiteral("bundle-name colon → dash keeps readable title"));
  check(psxstudio::cleanBundleName(QStringLiteral("Evil Zone (Europe)")) ==
          QStringLiteral("Evil Zone (Europe)"),
        QStringLiteral("parenthesized region remains in the user-visible bundle name"));
  check(psxstudio::cleanBundleName(QStringLiteral("Foo/Bar:Baz")).isEmpty() == false &&
          psxstudio::cleanBundleName(QStringLiteral("Foo/Bar:Baz")) ==
            QStringLiteral("Foo - Bar -Baz"),
        QStringLiteral("bundle-name path separators"));
#if defined(Q_OS_MACOS)
  const QString parenthesizedMachO =
    QDir(temp.path()).filePath(QStringLiteral("Evil Zone (Europe)"));
  check(QFile::link(QCoreApplication::applicationFilePath(), parenthesizedMachO),
        QStringLiteral("parenthesized Mach-O regression fixture"));
  const QString inspectionAlias = psxstudio::createMacosInspectionAlias(
    parenthesizedMachO,
    QDir(temp.path()).filePath(QStringLiteral("macos-inspection")),
    error);
  check(!inspectionAlias.isEmpty() &&
          QFileInfo(inspectionAlias).isSymLink() &&
          QFileInfo(inspectionAlias).canonicalFilePath() ==
            QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath(),
        error.isEmpty() ? QStringLiteral("parenthesis-free macOS inspection alias") : error);
  QProcess otool;
  otool.start(QStringLiteral("/usr/bin/otool"),
              { QStringLiteral("-L"), inspectionAlias });
  check(otool.waitForStarted(3000) && otool.waitForFinished(10000) &&
          otool.exitStatus() == QProcess::NormalExit && otool.exitCode() == 0,
        QStringLiteral("otool inspects Evil Zone (Europe) through its safe alias"));
#endif
  const QString normalBoot = psxstudio::runtimeBootToml(false);
  check(normalBoot.contains(QStringLiteral("bios_hle_keep_intro = true")) &&
          normalBoot.contains(QStringLiteral("fast_boot = false")),
        QStringLiteral("normal BIOS boot TOML"));
  const QString directBoot = psxstudio::runtimeBootToml(true);
  check(directBoot.contains(QStringLiteral("bios_hle = false")) &&
          directBoot.contains(QStringLiteral("bios_hle_keep_intro = false")) &&
          directBoot.contains(QStringLiteral("fast_boot = true")),
        QStringLiteral("direct-to-game boot TOML"));

  const QString controllerDefaults = psxstudio::defaultControllerToml();
  check(controllerDefaults.contains(QStringLiteral("default_mode = \"auto\"")) &&
          controllerDefaults.contains(QStringLiteral("allow_hybrid = true")) &&
          controllerDefaults.contains(QStringLiteral("lock_mode = true")) &&
          !controllerDefaults.contains(QStringLiteral("default_mode = \"digital\"")) &&
          !controllerDefaults.contains(QStringLiteral("default_mode = \"analog\"")),
        QStringLiteral("Studio locks automatic D-Pad-to-Hybrid negotiation"));

  check(psxstudio::macosGipCmakeOption(true).contains(
          QStringLiteral("PSX_MACOS_GIP_GAMEPAD ON")),
        QStringLiteral("Studio enables macOS GIP CMake option"));
  check(psxstudio::macosGipCmakeOption(false).contains(
          QStringLiteral("PSX_MACOS_GIP_GAMEPAD OFF")),
        QStringLiteral("Studio disables macOS GIP CMake option"));
  const QByteArray gipSymbols(
    "0000000100010000 T _psx_gip_gamepad_close\n"
    "0000000100010100 T _psx_gip_gamepad_get_state\n"
    "0000000100010200 T _psx_gip_gamepad_open\n");
  check(psxstudio::nmOutputHasMacosGipBackend(gipSymbols),
        QStringLiteral("GIP symbol proof detection"));
  check(!psxstudio::nmOutputHasMacosGipBackend(
          QByteArray("0000000100010000 T _main\n")),
        QStringLiteral("missing GIP symbols are rejected"));
  psxstudio::PipelineRequest gipRequest;
  check(gipRequest.macosGipGamepad &&
          gipRequest.toJson().value(QStringLiteral("macos_gip_gamepad")).toBool(),
        QStringLiteral("GIP export defaults enabled and is serialized"));
  check(gipRequest.exportAsZip &&
          gipRequest.toJson().value(QStringLiteral("export_as_zip")).toBool(),
        QStringLiteral("ZIP export defaults enabled and is serialized"));
  check(gipRequest.exportMode == psxstudio::ExportMode::Build &&
          psxstudio::exportModeFromKey(QStringLiteral("SOURCE")) ==
            psxstudio::ExportMode::Source &&
          gipRequest.toJson().value(QStringLiteral("export_mode")).toString() ==
            QStringLiteral("build"),
        QStringLiteral("Source/Build export mode defaults and serialization"));
  gipRequest.windowTitle = QStringLiteral("Evil Zone (Europe)");
  gipRequest.targetPlatform = psxstudio::TargetPlatform::MacOS;
  check(psxstudio::exportOutputName(gipRequest) ==
          QStringLiteral("Evil Zone (Europe)-macOS.zip"),
        QStringLiteral("macOS ZIP output name"));
  const QJsonObject macosGameManifest =
    psxstudio::gameManifestForRequest(gipRequest);
  check(macosGameManifest.size() == 3 &&
          macosGameManifest.value(QStringLiteral("executable")).toString() ==
            QStringLiteral("Evil Zone (Europe).app") &&
          macosGameManifest.value(QStringLiteral("name")).toString() ==
            QStringLiteral("Evil Zone (Europe)") &&
          macosGameManifest.value(QStringLiteral("platform")).toString() ==
            QStringLiteral("macOS"),
        QStringLiteral("macOS game manifest names the root app"));
  gipRequest.targetPlatform = psxstudio::TargetPlatform::Windows;
  check(psxstudio::exportOutputName(gipRequest) ==
          QStringLiteral("Evil Zone (Europe)-Windows.zip"),
        QStringLiteral("Windows ZIP output name"));
  check(psxstudio::gameManifestForRequest(gipRequest)
          .value(QStringLiteral("executable")).toString() ==
            QStringLiteral("Evil Zone (Europe).exe") &&
          psxstudio::gameManifestForRequest(gipRequest)
            .value(QStringLiteral("platform")).toString() ==
              QStringLiteral("Windows"),
        QStringLiteral("Windows game manifest names the root executable"));
  gipRequest.targetPlatform = psxstudio::TargetPlatform::Linux;
  check(psxstudio::exportOutputName(gipRequest) ==
          QStringLiteral("Evil Zone (Europe)-Linux.zip"),
        QStringLiteral("Linux ZIP output name"));
  check(psxstudio::gameManifestForRequest(gipRequest)
          .value(QStringLiteral("executable")).toString() ==
            QStringLiteral("Evil Zone (Europe)") &&
          psxstudio::gameManifestForRequest(gipRequest)
            .value(QStringLiteral("platform")).toString() ==
              QStringLiteral("Linux"),
        QStringLiteral("Linux game manifest names the root executable"));
  gipRequest.exportAsZip = false;
  check(psxstudio::exportOutputName(gipRequest) ==
          QStringLiteral("Evil Zone (Europe)-Linux"),
        QStringLiteral("unpacked export output name remains available"));
  gipRequest.exportMode = psxstudio::ExportMode::Source;
  check(psxstudio::exportOutputName(gipRequest) ==
          QStringLiteral("Evil Zone (Europe)-Linux-Source"),
        QStringLiteral("unpacked source repository output name"));
  gipRequest.exportAsZip = true;
  check(psxstudio::exportOutputName(gipRequest) ==
          QStringLiteral("Evil Zone (Europe)-Linux-Source.zip"),
        QStringLiteral("source repository ZIP output name"));
  gipRequest.exportMode = psxstudio::ExportMode::Build;
  gipRequest.targetPlatform = psxstudio::hostTargetPlatform();
  check(gipRequest.targetPlatform == psxstudio::hostTargetPlatform() &&
          gipRequest.toJson().value(QStringLiteral("platform")).toString() ==
            psxstudio::targetPlatformKey(psxstudio::hostTargetPlatform()),
        QStringLiteral("Studio export defaults to its host and serializes the platform"));
  check(psxstudio::targetPlatformSupportedOnHost(psxstudio::hostTargetPlatform()),
        QStringLiteral("Studio always supports its native host target"));
  check(psxstudio::hostCanSelectTargetPlatform() ||
          (!psxstudio::targetPlatformSupportedOnHost(psxstudio::TargetPlatform::MacOS) &&
           !psxstudio::targetPlatformSupportedOnHost(
             psxstudio::hostTargetPlatform() == psxstudio::TargetPlatform::Windows
               ? psxstudio::TargetPlatform::Linux
               : psxstudio::TargetPlatform::Windows)),
        QStringLiteral("Only macOS Studio builds support cross-platform targets"));
  const auto allTargets =
    psxstudio::concreteTargetPlatforms(psxstudio::TargetPlatform::All);
  check(psxstudio::targetPlatformKey(psxstudio::TargetPlatform::All) ==
          QStringLiteral("all") &&
          psxstudio::targetPlatformDisplayName(psxstudio::TargetPlatform::All) ==
            QStringLiteral("All") &&
          psxstudio::targetPlatformFromKey(QStringLiteral("ALL")) ==
            psxstudio::TargetPlatform::All,
        QStringLiteral("All platform selection round-trips"));
  check(allTargets == QList<psxstudio::TargetPlatform>{
          psxstudio::TargetPlatform::MacOS,
          psxstudio::TargetPlatform::Windows,
          psxstudio::TargetPlatform::Linux },
        QStringLiteral("All expands to each concrete platform exactly once"));
  check(psxstudio::targetPlatformSupportedOnHost(psxstudio::TargetPlatform::All) ==
          psxstudio::hostCanSelectTargetPlatform(),
        QStringLiteral("All is available only on macOS Studio builds"));
  gipRequest.targetPlatform = psxstudio::TargetPlatform::Windows;
  check(psxstudio::targetPlatformKey(gipRequest.targetPlatform) == QStringLiteral("windows") &&
          psxstudio::targetPlatformFromKey(QStringLiteral("WINDOWS")) ==
            psxstudio::TargetPlatform::Windows &&
          gipRequest.toJson().value(QStringLiteral("platform")).toString() ==
            QStringLiteral("windows"),
        QStringLiteral("Windows platform selection round-trips through the request"));
  gipRequest.targetPlatform = psxstudio::TargetPlatform::Linux;
  check(psxstudio::targetPlatformKey(gipRequest.targetPlatform) == QStringLiteral("linux") &&
          psxstudio::targetPlatformDisplayName(gipRequest.targetPlatform) ==
            QStringLiteral("Linux") &&
          psxstudio::targetPlatformFromKey(QStringLiteral("LINUX")) ==
            psxstudio::TargetPlatform::Linux &&
          gipRequest.toJson().value(QStringLiteral("platform")).toString() ==
            QStringLiteral("linux"),
        QStringLiteral("Linux platform selection round-trips through the request"));


  gipRequest.targetPlatform = psxstudio::TargetPlatform::VirtuaArm;
  gipRequest.exportMode = psxstudio::ExportMode::Build;
  gipRequest.exportAsZip = true;
  gipRequest.powerEngineRoot = QStringLiteral("/host-only/PowerEngine");
  gipRequest.llvmRoot = QStringLiteral("/host-only/PowerEngine/compiler");
  check(psxstudio::targetPlatformFromKey(QStringLiteral("VIRTUA-ARM")) ==
          psxstudio::TargetPlatform::VirtuaArm &&
          psxstudio::targetPlatformDisplayName(gipRequest.targetPlatform) ==
            QStringLiteral("Virtua ARM") &&
          psxstudio::exportOutputName(gipRequest) ==
            QStringLiteral("Evil Zone (Europe)-Virtua-ARM.zip") &&
          psxstudio::gameManifestForRequest(gipRequest)
            .value(QStringLiteral("executable")).toString() ==
              QStringLiteral("Evil Zone (Europe).virtua") &&
          psxstudio::gameManifestForRequest(gipRequest)
            .value(QStringLiteral("icon")).toString() == QStringLiteral("AppIcon.png") &&
          gipRequest.toJson().value(QStringLiteral("powerengine_root")).toString() ==
            gipRequest.powerEngineRoot &&
          gipRequest.toJson().value(QStringLiteral("llvm_root")).toString() ==
            gipRequest.llvmRoot,
        QStringLiteral("Virtua ARM platform, output, and manifest round-trip"));

  psxstudio::GameDescription generatedGame;
  generatedGame.bootFileName = QStringLiteral("SLUS_000.00");
  generatedGame.serial = QStringLiteral("SLUS-00000");
  generatedGame.textSize = 0x1000;
  psxstudio::PipelineRequest generatedRequest;
  generatedRequest.windowTitle = QStringLiteral("Portable Test Game");
  generatedRequest.macosGipGamepad = false;
  generatedRequest.frameworkRoot = QStringLiteral("/host-only/framework-root");
  generatedRequest.powerEngineRoot = QStringLiteral("/host-only/PowerEngine");
  generatedRequest.llvmRoot = QStringLiteral("/host-only/PowerEngine/compiler");
  for (const auto platform : { psxstudio::TargetPlatform::MacOS,
                               psxstudio::TargetPlatform::Windows,
                               psxstudio::TargetPlatform::Linux,
                               psxstudio::TargetPlatform::VirtuaArm }) {
    generatedRequest.targetPlatform = platform;
    const QString generatedCmake = psxstudio::generatedProjectCMake(
      generatedRequest, generatedGame, QStringLiteral("Portable Test Game"),
      QStringLiteral("org.psxrecomp.generated.test"), 0x12345678u);
    check(generatedCmake.contains(QStringLiteral(
            "${CMAKE_BINARY_DIR}/steganos-package/psx-runtime/$<CONFIG>")) &&
          generatedCmake.contains(QStringLiteral(
            "${CMAKE_CURRENT_SOURCE_DIR}/game.manifest.json")) &&
          generatedCmake.contains(QStringLiteral(
            "copy_if_different \"${_PSX_GAME_MANIFEST}\"")) &&
          generatedCmake.contains(QStringLiteral(
            "${CMAKE_CURRENT_SOURCE_DIR}/framework")) &&
          !generatedCmake.contains(generatedRequest.frameworkRoot),
          QStringLiteral("generated CMake is portable and stages the game manifest during the target build"));
    if (platform == psxstudio::TargetPlatform::Linux) {
      check(generatedCmake.contains(QStringLiteral(
              "INTERFACE_INCLUDE_DIRECTORIES \"${sdl2source_SOURCE_DIR}/include\")\nset(SDL2_INCLUDE_DIRS")),
            QStringLiteral("generated Linux CMake closes imported SDL target properties"));
    } else if (platform == psxstudio::TargetPlatform::VirtuaArm) {
      check(generatedCmake.contains(QStringLiteral("set(PSX_VIRTUA ON")) &&
              generatedCmake.contains(QStringLiteral("${POWERENGINE_ROOT}/External/Virtua")) &&
              !generatedCmake.contains(QStringLiteral("framework/extra/virtua/binary")) &&
              !generatedCmake.contains(generatedRequest.powerEngineRoot) &&
              !generatedCmake.contains(generatedRequest.llvmRoot) &&
              generatedCmake.contains(QStringLiteral("-scheduler cooperative")) &&
              generatedCmake.contains(QStringLiteral("Portable Test Game.virtua")),
            QStringLiteral("generated PlayStation CMake consumes external PowerEngine bindings"));
    }
  }

#if defined(Q_OS_MACOS)
  generatedRequest.targetPlatform = psxstudio::TargetPlatform::MacOS;
  const QString macProject = QDir(temp.path()).filePath(QStringLiteral("generated-cmake-project"));
  const QString macBuild = QDir(temp.path()).filePath(QStringLiteral("generated-cmake-build"));
  check(QDir().mkpath(QDir(macProject).filePath(QStringLiteral("framework/runtime"))) &&
          QDir().mkpath(QDir(macProject).filePath(QStringLiteral("package-resources"))),
        QStringLiteral("generated CMake fixture directories"));
  const QString runtimeStub = QStringLiteral(
    "function(psxrecomp_add_runtime_target target)\n"
    "  cmake_parse_arguments(PSXRT \"MACOSX_BUNDLE\" \"\" \"\" ${ARGN})\n"
    "  file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/dummy.c\" \"int main(void){return 0;}\\n\")\n"
    "  add_executable(${target} ${CMAKE_CURRENT_BINARY_DIR}/dummy.c)\n"
    "  if(PSXRT_MACOSX_BUNDLE)\n"
    "    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE TRUE)\n"
    "  endif()\n"
    "endfunction()\n");
  check(psxstudio::writeText(
          QDir(macProject).filePath(QStringLiteral("framework/runtime/runtime.cmake")),
          runtimeStub, error), error);
  check(psxstudio::writeText(
          QDir(macProject).filePath(QStringLiteral("CMakeLists.txt")),
          psxstudio::generatedProjectCMake(
            generatedRequest, generatedGame, QStringLiteral("Portable Test Game"),
            QStringLiteral("org.psxrecomp.generated.test"), 0x12345678u), error), error);
  check(psxstudio::writeBytes(
          QDir(macProject).filePath(QStringLiteral("AppIcon.icns")), QByteArray("icon"), error) &&
        psxstudio::writeText(
          QDir(macProject).filePath(QStringLiteral("Info.plist")), QStringLiteral("<plist/>"), error) &&
        psxstudio::writeText(
          QDir(macProject).filePath(QStringLiteral("game.manifest.json")), QStringLiteral("{}"), error), error);
  QProcess generatedConfigure;
  generatedConfigure.start(QStringLiteral("cmake"),
    { QStringLiteral("-S"), macProject, QStringLiteral("-B"), macBuild,
      QStringLiteral("-G"), QStringLiteral("Ninja"),
      QStringLiteral("-DCMAKE_BUILD_TYPE=Release") });
  check(generatedConfigure.waitForStarted(5000) &&
          generatedConfigure.waitForFinished(30000) &&
          generatedConfigure.exitStatus() == QProcess::NormalExit &&
          generatedConfigure.exitCode() == 0,
        QStringLiteral("generated macOS CMake configures with its build-time package pipeline: %1")
          .arg(QString::fromUtf8(generatedConfigure.readAllStandardError())));
  QProcess generatedBuild;
  generatedBuild.start(QStringLiteral("cmake"),
    { QStringLiteral("--build"), macBuild, QStringLiteral("--target"),
      QStringLiteral("psx-runtime"), QStringLiteral("--config"),
      QStringLiteral("Release") });
  const QString generatedPackage = QDir(macBuild).filePath(
    QStringLiteral("steganos-package/psx-runtime/Release"));
  check(generatedBuild.waitForStarted(5000) &&
          generatedBuild.waitForFinished(30000) &&
          generatedBuild.exitStatus() == QProcess::NormalExit &&
          generatedBuild.exitCode() == 0 &&
          QFileInfo(QDir(generatedPackage).filePath(
            QStringLiteral("game.manifest.json"))).isFile() &&
          QFileInfo(QDir(generatedPackage).filePath(
            QStringLiteral("Portable Test Game.app"))).isDir(),
        QStringLiteral("generated target builds a Steganos package directory with its root game manifest: %1")
          .arg(QString::fromUtf8(generatedBuild.readAllStandardError())));
#endif

  const QString sourceTree = QDir(temp.path()).filePath(QStringLiteral("source-tree"));
  const QString copiedTree = QDir(temp.path()).filePath(QStringLiteral("copied-tree"));
  const QString sourceTreeFile = QDir(sourceTree).filePath(QStringLiteral("nested/file.txt"));
  check(psxstudio::writeText(sourceTreeFile,
                             QStringLiteral("windows-package"), error), error);
  check(QFile::setPermissions(sourceTreeFile,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                              QFileDevice::ExeGroup | QFileDevice::ReadOther |
                              QFileDevice::ExeOther),
        QStringLiteral("directory-package source executable permissions"));
  check(psxstudio::copyDirectoryTree(sourceTree, copiedTree, error), error);
  QFile copiedFile(QDir(copiedTree).filePath(QStringLiteral("nested/file.txt")));
  check(copiedFile.open(QIODevice::ReadOnly) && copiedFile.readAll() == "windows-package",
        QStringLiteral("directory-package file copy"));
#if !defined(Q_OS_WIN)
  check(QFileInfo(copiedFile).isExecutable(),
        QStringLiteral("Linux package executable permissions survive directory copy"));
#endif

  QImage sourceIcon(64, 64, QImage::Format_ARGB32);
  sourceIcon.fill(QColor(32, 96, 192));
  const QString sourceIconPath = QDir(temp.path()).filePath(QStringLiteral("source-icon.png"));
  const QString windowsIconPath = QDir(temp.path()).filePath(QStringLiteral("AppIcon.ico"));
  check(sourceIcon.save(sourceIconPath, "PNG"), QStringLiteral("Windows icon source PNG"));
  check(psxstudio::createIco(sourceIconPath, windowsIconPath, error), error);
  check(QFileInfo(windowsIconPath).isFile() && QFileInfo(windowsIconPath).size() > 0,
        QStringLiteral("Windows ICO generation"));
  const QString linuxIconPath = QDir(temp.path()).filePath(QStringLiteral("AppIcon.png"));
  check(psxstudio::createPngIcon(sourceIconPath, linuxIconPath, error), error);
  QImage linuxIcon(linuxIconPath);
  check(!linuxIcon.isNull() && linuxIcon.size() == QSize(512, 512),
        QStringLiteral("Linux PNG icon generation"));
  const QString defaultIconPath =
    QDir(temp.path()).filePath(QStringLiteral("DefaultAppIcon.png"));
  check(psxstudio::createPngIcon(
          QStringLiteral(":/psxrecomp/studio/resources/psxrecomp-studio.svg"),
          defaultIconPath, error), error);
  const QImage defaultIcon(defaultIconPath);
  check(!defaultIcon.isNull() && defaultIcon.size() == QSize(512, 512),
        QStringLiteral("optional icon falls back to the built-in Studio artwork"));

  const QString proofDir = QDir(temp.path()).filePath(QStringLiteral("proof"));
  QDir().mkpath(proofDir);
  check(psxstudio::writeJson(QDir(proofDir).filePath(QStringLiteral("manifest.json")),
                  QJsonObject{ { QStringLiteral("status"), QStringLiteral("clean") } }, error), error);
  const QString archivePath = QDir(temp.path()).filePath(QStringLiteral("proof.zip"));
  check(psxstudio::createProofArchive(proofDir, archivePath, error), error);
  QuaZip archive(archivePath);
  check(archive.open(QuaZip::mdUnzip), QStringLiteral("proof ZIP open"));
  check(archive.setCurrentFile(QStringLiteral("manifest.json")), QStringLiteral("proof ZIP entry"));
  QuaZipFile archivedFile(&archive);
  check(archivedFile.open(QIODevice::ReadOnly), QStringLiteral("proof ZIP read"));
  check(archivedFile.readAll().contains("clean"), QStringLiteral("proof ZIP contents"));
  archivedFile.close();
  archive.close();

  const QString packageSource =
    QDir(temp.path()).filePath(QStringLiteral("package-source"));
  const QString packageExecutable =
    QDir(packageSource).filePath(QStringLiteral("Evil Zone (Europe).exe"));
  const QString packageProof =
    QDir(packageSource).filePath(QStringLiteral("proof/manifest.json"));
  const QString hiddenMetadata =
    QDir(packageSource).filePath(QStringLiteral(".package-metadata"));
  check(psxstudio::writeBytes(packageExecutable, QByteArray("native-binary"), error), error);
  check(psxstudio::writeBytes(packageProof, QByteArray("verified-proof"), error), error);
  check(psxstudio::writeBytes(hiddenMetadata, QByteArray("hidden"), error), error);
  check(QDir().mkpath(QDir(packageSource).filePath(QStringLiteral("empty"))),
        QStringLiteral("package ZIP empty-directory fixture"));
  const QFile::Permissions executablePermissions =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
    QFileDevice::ReadGroup | QFileDevice::ExeGroup |
    QFileDevice::ReadOther | QFileDevice::ExeOther;
  check(QFile::setPermissions(packageExecutable, executablePermissions),
        QStringLiteral("package ZIP executable permission fixture"));
#if !defined(Q_OS_WIN)
  const QString packageLink =
    QDir(packageSource).filePath(QStringLiteral("runtime-link"));
  check(QFile::link(packageExecutable, packageLink),
        QStringLiteral("package ZIP symlink fixture"));
#endif

  const QString rootContentsZip =
    QDir(temp.path()).filePath(QStringLiteral("Evil Zone-Windows.zip"));
  psxstudio::PipelineRequest manifestRequest;
  manifestRequest.windowTitle = QStringLiteral("Evil Zone (Europe)");
  manifestRequest.targetPlatform = psxstudio::TargetPlatform::Windows;
  const QMap<QString, QByteArray> windowsRootFiles{
    { QStringLiteral("game.manifest.json"),
      QJsonDocument(psxstudio::gameManifestForRequest(manifestRequest))
        .toJson(QJsonDocument::Indented) },
  };
  check(psxstudio::createPackageArchive(
          packageSource, {}, rootContentsZip, error, windowsRootFiles), error);
  QuaZip rootContentsArchive(rootContentsZip);
  check(rootContentsArchive.open(QuaZip::mdUnzip),
        QStringLiteral("root-contents package ZIP opens"));
  const QStringList rootContentsEntries = rootContentsArchive.getFileNameList();
  check(rootContentsEntries.contains(QStringLiteral("Evil Zone (Europe).exe")) &&
          rootContentsEntries.contains(QStringLiteral("game.manifest.json")) &&
          rootContentsEntries.contains(QStringLiteral("proof/manifest.json")) &&
          rootContentsEntries.contains(QStringLiteral("empty/")) &&
          rootContentsEntries.contains(QStringLiteral(".package-metadata")) &&
          std::none_of(rootContentsEntries.cbegin(), rootContentsEntries.cend(),
                       [](const QString& name) {
                         return name.startsWith(QStringLiteral("package-source/"));
                       }),
        QStringLiteral("Windows/Linux package contents sit directly at ZIP root"));
  check(rootContentsArchive.setCurrentFile(QStringLiteral("game.manifest.json")),
        QStringLiteral("root ZIP game manifest entry"));
  QuaZipFile gameManifestFile(&rootContentsArchive);
  check(gameManifestFile.open(QIODevice::ReadOnly),
        QStringLiteral("root ZIP game manifest opens"));
  const QJsonObject archivedGameManifest =
    QJsonDocument::fromJson(gameManifestFile.readAll()).object();
  gameManifestFile.close();
  check(archivedGameManifest.size() == 3 &&
          archivedGameManifest.value(QStringLiteral("executable")).toString() ==
            QStringLiteral("Evil Zone (Europe).exe") &&
          archivedGameManifest.value(QStringLiteral("name")).toString() ==
            QStringLiteral("Evil Zone (Europe)") &&
          archivedGameManifest.value(QStringLiteral("platform")).toString() ==
            QStringLiteral("Windows"),
        QStringLiteral("root ZIP game manifest contents"));
  check(rootContentsArchive.setCurrentFile(QStringLiteral("Evil Zone (Europe).exe")),
        QStringLiteral("root ZIP executable entry"));
  QuaZipFileInfo64 executableInfo;
  check(rootContentsArchive.getCurrentFileInfo(&executableInfo) &&
          executableInfo.getPermissions().testFlag(QFileDevice::ExeOwner),
        QStringLiteral("package ZIP preserves executable permissions"));
#if !defined(Q_OS_WIN)
  check(rootContentsArchive.setCurrentFile(QStringLiteral("runtime-link")),
        QStringLiteral("root ZIP symlink entry"));
  QuaZipFileInfo64 linkInfo;
  check(rootContentsArchive.getCurrentFileInfo(&linkInfo) && linkInfo.isSymbolicLink(),
        QStringLiteral("package ZIP preserves symbolic links"));
#endif
  rootContentsArchive.close();

  const QString macosZip =
    QDir(temp.path()).filePath(QStringLiteral("Evil Zone-macOS.zip"));
  manifestRequest.targetPlatform = psxstudio::TargetPlatform::MacOS;
  const QMap<QString, QByteArray> macosRootFiles{
    { QStringLiteral("game.manifest.json"),
      QJsonDocument(psxstudio::gameManifestForRequest(manifestRequest))
        .toJson(QJsonDocument::Indented) },
  };
  check(psxstudio::createPackageArchive(
          packageSource, QStringLiteral("Evil Zone (Europe).app"), macosZip, error,
          macosRootFiles),
        error);
  QuaZip macosArchive(macosZip);
  check(macosArchive.open(QuaZip::mdUnzip),
        QStringLiteral("macOS package ZIP opens"));
  const QStringList macosEntries = macosArchive.getFileNameList();
  check(macosEntries.contains(QStringLiteral("Evil Zone (Europe).app/")) &&
          macosEntries.contains(QStringLiteral("game.manifest.json")) &&
          macosEntries.contains(
            QStringLiteral("Evil Zone (Europe).app/Evil Zone (Europe).exe")) &&
          std::all_of(macosEntries.cbegin(), macosEntries.cend(),
                      [](const QString& name) {
                        return name == QStringLiteral("game.manifest.json") ||
                          name.startsWith(QStringLiteral("Evil Zone (Europe).app/"));
                      }),
        QStringLiteral("macOS ZIP root contains only its game manifest and .app"));
  macosArchive.close();
#if defined(Q_OS_MACOS)
  const QString extractedMacosZip =
    QDir(temp.path()).filePath(QStringLiteral("extracted-macos-zip"));
  QProcess ditto;
  ditto.start(QStringLiteral("/usr/bin/ditto"),
              { QStringLiteral("-x"), QStringLiteral("-k"), macosZip,
                extractedMacosZip });
  check(ditto.waitForStarted(3000) && ditto.waitForFinished(10000) &&
          ditto.exitStatus() == QProcess::NormalExit && ditto.exitCode() == 0,
        QStringLiteral("macOS package ZIP extracts with Apple ditto"));
  const QString extractedExecutable = QDir(extractedMacosZip).filePath(
    QStringLiteral("Evil Zone (Europe).app/Evil Zone (Europe).exe"));
  check(QFileInfo(extractedExecutable).isExecutable(),
        QStringLiteral("Apple ZIP extraction retains app executable permissions"));
  const QString extractedLink = QDir(extractedMacosZip).filePath(
    QStringLiteral("Evil Zone (Europe).app/runtime-link"));
  check(QFileInfo(extractedLink).isSymLink() &&
          QFileInfo(extractedLink).canonicalFilePath() ==
            QFileInfo(extractedExecutable).canonicalFilePath(),
        QStringLiteral("Apple ZIP extraction retains app symbolic links"));
#endif

  const QString cancelledZip =
    QDir(temp.path()).filePath(QStringLiteral("cancelled.zip"));
  check(!psxstudio::createPackageArchive(
          packageSource, {}, cancelledZip, error, {}, []() { return true; }) &&
          !QFileInfo::exists(cancelledZip),
        QStringLiteral("cancelled package ZIP is not delivered"));

  // ── Ghidra seed admission ──────────────────────────────────────────────────
  //
  // These guard the rule that keeps compressed art out of the dispatch table.
  // Seeding on Ghidra's reference class instead admits data: on Final Fantasy
  // VI Advance it offers 113,516 COMPUTED_CALL references against 22 real
  // COMPUTED_JUMP, and because every admitted seed widens the translated extent
  // that the next pass anchors to, the run cascades — 834,559 dispatch rows and
  // 1.5 GB of generated C++ by pass 11, against 16,129 rows at the fixed point.
  {
    constexpr quint32 kRomBase = 0x08000000u;
    const QString generatedDir = QDir(temp.path()).filePath(QStringLiteral("gba-generated"));
    check(QDir().mkpath(generatedDir), QStringLiteral("seed-admission fixture directory"));

    // A THUMB BL is two halfwords and gba_recompile lowers it as a `bl.hi`/
    // `bl.lo` pair, so 0x08000102 is a complete call and 0x08000108 — sitting
    // behind a `bx`, not a `bl.hi` — is an orphan second halfword. Ghidra turns
    // exactly that shape into a call through LR with an invented destination.
    QByteArray rom(0x400, '\0');
    const auto pokeHalf = [&rom](quint32 address, quint16 value) {
      qToLittleEndian(value, reinterpret_cast<uchar*>(rom.data()) + (address - kRomBase));
    };
    const auto pokeWord = [&rom](quint32 address, quint32 value) {
      qToLittleEndian(value, reinterpret_cast<uchar*>(rom.data()) + (address - kRomBase));
    };
    pokeHalf(0x08000100u, 0xF000u);  // BL, upper half
    pokeHalf(0x08000102u, 0xF87Eu);  // BL, lower half -> 0x08000200
    pokeWord(0x08000130u, 0x08000601u);  // a THUMB function pointer

    QString shard = QStringLiteral(
      "/* 0x08000100  mode=thumb  end=0x08000140  branches=3 */\n"
      "void gf_tfunc_08000100(void) {\n"
      "    /* 08000100  08000100 T bl.hi 0x08000104 */\n"
      "    /* 08000102  08000102 T bl.lo 0x00000000 */\n"
      "    /* 08000104  08000104 T b 0x08000120 */\n"
      "    /* 08000106  08000106 T bx r3 */\n"
      "    /* 08000108  08000108 T bl.lo 0x00000000 */\n"
      "    /* 0800010A  0800010a T movs r0,r0 */\n"
      "}\n");
    check(psxstudio::writeText(
            QDir(generatedDir).filePath(QStringLiteral("recompiled_000.cpp")), shard, error),
          error);

    QSet<quint32> wanted;
    for (quint32 address = 0x080000FEu; address <= 0x08000140u; address += 1) {
      wanted.insert(address);
    }
    QList<psxstudio::GbaTranslatedSpan> spans;
    psxstudio::GbaInstructionMap instructions;
    check(psxstudio::readGbaTranslation(generatedDir, wanted, spans, instructions, error) &&
            spans.size() == 1 && instructions.size() == 6 &&
            psxstudio::gbaTranslatedBytes(spans) == 0x40u &&
            psxstudio::gbaSpanContaining(spans, 0x08000102u) != nullptr &&
            psxstudio::gbaSpanContaining(spans, 0x08000140u) == nullptr,
          error.isEmpty() ? QStringLiteral("gbarecomp shard spans and instructions parse")
                          : error);

    const auto candidate = [](quint32 entry, bool thumb, const QVector<quint32>& computed,
                              const QVector<quint32>& pointers = {}) {
      psxstudio::GbaSeedCandidate value;
      value.entry = entry;
      value.thumb = thumb;
      value.name = QStringLiteral("fixture");
      value.computedSources = computed;
      value.pointerSources = pointers;
      return value;
    };
    const auto admit = [&](const psxstudio::GbaSeedCandidate& value, QString& rule,
                           bool& modeCrossed, QString& reason) {
      quint32 evidence = 0;
      rule.clear();
      reason.clear();
      modeCrossed = false;
      return psxstudio::admitGbaSeed(value, spans, instructions, rom, kRomBase,
                                     rule, evidence, modeCrossed, reason);
    };

    QString rule;
    QString reason;
    bool modeCrossed = false;
    check(admit(candidate(0x08000200u, true, { 0x08000102u }), rule, modeCrossed, reason) &&
            rule == QStringLiteral("direct-branch") && !modeCrossed,
          QStringLiteral("a complete BL pair resolving to the entry is admitted"));
    check(admit(candidate(0x08000120u, true, { 0x08000104u }), rule, modeCrossed, reason) &&
            rule == QStringLiteral("direct-branch"),
          QStringLiteral("a direct B printing the entry as its target is admitted"));
    check(admit(candidate(0x08000400u, false, { 0x08000106u }), rule, modeCrossed, reason) &&
            rule == QStringLiteral("register-indirect") && modeCrossed,
          QStringLiteral("a BX admits either instruction set and reports the crossing"));
    check(admit(candidate(0x08000600u, true, {}, { 0x08000130u }), rule, modeCrossed, reason) &&
            rule == QStringLiteral("pointer-word"),
          QStringLiteral("a ROM word holding entry|1 is admitted as a THUMB pointer"));

    check(!admit(candidate(0x08000300u, true, { 0x08000108u }), rule, modeCrossed, reason) &&
            reason.contains(QStringLiteral("orphan bl.lo")),
          QStringLiteral("an orphan bl.lo is rejected: its destination is unknown"));
    check(!admit(candidate(0x08000500u, true, { 0x08000109u }), rule, modeCrossed, reason),
          QStringLiteral("an address inside a span but off the instruction boundary is rejected"));
    check(!admit(candidate(0x08000200u, false, { 0x08000102u }), rule, modeCrossed, reason) &&
            reason.contains(QStringLiteral("no BLX")),
          QStringLiteral("a direct branch cannot enter its target in the other instruction set"));
    check(!admit(candidate(0x08000700u, true, { 0x08000104u }), rule, modeCrossed, reason),
          QStringLiteral("a direct branch resolving elsewhere does not seed this entry"));
    check(!admit(candidate(0x08000800u, true, { 0x08009000u }), rule, modeCrossed, reason) &&
            reason.isEmpty(),
          QStringLiteral("a source outside every translated extent is unsupported, not contradicted"));
    check(!admit(candidate(0x08000900u, true, {}, { 0x08000130u }), rule, modeCrossed, reason) &&
            reason.contains(QStringLiteral("holds")),
          QStringLiteral("a pointer word holding some other address is rejected"));
  }

  if (failures == 0) {
    qInfo() << "PSXRecomp Studio core tests passed";
  }
  return failures == 0 ? 0 : 1;
}
