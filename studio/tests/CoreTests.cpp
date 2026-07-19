#include "CueSheet.h"
#include "DiscCatalog.h"
#include "DiscInspector.h"
#include "PipelineSupport.h"

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
  QTemporaryDir temp;
  check(temp.isValid(), QStringLiteral("temporary directory creation"));
  if (!temp.isValid()) {
    return 1;
  }

  QString error;
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
        QStringLiteral("batch catalog reports an incomplete CUE"));
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
  gipRequest.exportAsZip = true;
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

  if (failures == 0) {
    qInfo() << "PSXRecomp Studio core tests passed";
  }
  return failures == 0 ? 0 : 1;
}
