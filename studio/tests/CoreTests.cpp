#include "CueSheet.h"
#include "DiscInspector.h"
#include "PipelineSupport.h"

#include "quazip.h"
#include "quazipfile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>

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
  const QString binPath = QDir(temp.path()).filePath(QStringLiteral("test.bin"));
  const QString cuePath = QDir(temp.path()).filePath(QStringLiteral("test.cue"));
  check(makeTestDisc(binPath, error), error);
  check(psxstudio::writeText(cuePath,
                  QStringLiteral("FILE \"test.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n"),
                  error), error);

  psxstudio::DiscDescription disc;
  check(psxstudio::CueSheet::parse(cuePath, {}, disc, error), error);
  check(disc.files.size() == 1, QStringLiteral("CUE file count"));
  check(disc.rewrittenCue.contains(QStringLiteral("\"test.bin\"")),
        QStringLiteral("CUE rewrite"));

  psxstudio::GameDescription game;
  check(psxstudio::DiscInspector::inspect(disc, game, error), error);
  check(game.bootFileName == QStringLiteral("TEST_000.00"), QStringLiteral("boot file detection"));
  check(game.serial == QStringLiteral("TEST-00000"), QStringLiteral("serial normalization"));
  check(game.entryPc == 0x80010000u, QStringLiteral("entry PC parsing"));
  check(game.textSize == 4u, QStringLiteral("text-size parsing"));

  check(psxstudio::sanitizedFileStem(QStringLiteral("Top Gun: Fire at Will!")) ==
          QStringLiteral("TopGunFireAtWill"),
        QStringLiteral("app-name sanitization"));
  check(psxstudio::sanitizedBundleIdentifier(QStringLiteral("SLUS-00032"), {}) ==
          QStringLiteral("org.psxrecomp.generated.slus-00032"),
        QStringLiteral("bundle-identifier sanitization"));
  const QString normalBoot = psxstudio::runtimeBootToml(false);
  check(normalBoot.contains(QStringLiteral("bios_hle_keep_intro = true")) &&
          normalBoot.contains(QStringLiteral("fast_boot = false")),
        QStringLiteral("normal BIOS boot TOML"));
  const QString directBoot = psxstudio::runtimeBootToml(true);
  check(directBoot.contains(QStringLiteral("bios_hle = false")) &&
          directBoot.contains(QStringLiteral("bios_hle_keep_intro = false")) &&
          directBoot.contains(QStringLiteral("fast_boot = true")),
        QStringLiteral("direct-to-game boot TOML"));

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
  check(gipRequest.targetPlatform == psxstudio::TargetPlatform::MacOS &&
          gipRequest.toJson().value(QStringLiteral("platform")).toString() ==
            QStringLiteral("macos"),
        QStringLiteral("Studio export defaults to macOS and serializes the platform"));
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
  check(QFileInfo(copiedFile).isExecutable(),
        QStringLiteral("Linux package executable permissions survive directory copy"));

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

  if (failures == 0) {
    qInfo() << "PSXRecomp Studio core tests passed";
  }
  return failures == 0 ? 0 : 1;
}
