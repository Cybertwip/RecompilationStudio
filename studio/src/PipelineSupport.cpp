#include "PipelineSupport.h"

#include "quazip.h"
#include "quazipfile.h"
#include "quazipfileinfo.h"
#include "quazipnewinfo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSvgRenderer>

#include <algorithm>

namespace psxstudio {

QString sha256File(const QString& path, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not hash %1: %2").arg(path, file.errorString());
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) {
    error = QStringLiteral("Could not read %1 while hashing it.").arg(path);
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

QString batchEntrySettingsId(const QString& sourcePath) {
  const QFileInfo info(sourcePath);
  QString normalized = info.canonicalFilePath();
  if (normalized.isEmpty()) {
    normalized = info.absoluteFilePath();
  }
  normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalized));
#if defined(Q_OS_WIN)
  normalized = normalized.toCaseFolded();
#endif
  return QString::fromLatin1(
    QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex());
}

int javaMajorVersion(const QString& versionOutput) {
  const QRegularExpression pattern(QStringLiteral(
    R"((?im)^\s*(?:openjdk|java)(?:\s+version)?\s+"?(\d+)(?:\.(\d+))?)"));
  const auto match = pattern.match(versionOutput);
  if (!match.hasMatch()) {
    return -1;
  }
  bool ok = false;
  int major = match.captured(1).toInt(&ok);
  if (!ok) {
    return -1;
  }
  if (major == 1 && !match.captured(2).isEmpty()) {
    major = match.captured(2).toInt(&ok);
  }
  return ok ? major : -1;
}

QString findJava21Home() {
  QStringList candidates{ qEnvironmentVariable("JAVA_HOME") };
#if defined(Q_OS_MACOS)
  QProcess javaHomeProcess;
  javaHomeProcess.start(QStringLiteral("/usr/libexec/java_home"),
                        { QStringLiteral("-v"), QStringLiteral("21") });
  if (javaHomeProcess.waitForStarted(3000) &&
      javaHomeProcess.waitForFinished(5000) &&
      javaHomeProcess.exitStatus() == QProcess::NormalExit &&
      javaHomeProcess.exitCode() == 0) {
    candidates << QString::fromUtf8(
      javaHomeProcess.readAllStandardOutput()).trimmed();
  }

  /* Versioned Homebrew JDKs are keg-only and need not appear in java_home. */
  candidates << QStringLiteral("/usr/local/opt/openjdk@21")
             << QStringLiteral("/opt/homebrew/opt/openjdk@21")
             << QStringLiteral("/opt/local/libexec/openjdk21");
  for (const QString& root : {
         QStringLiteral("/Library/Java/JavaVirtualMachines"),
         QDir::home().filePath(QStringLiteral("Library/Java/JavaVirtualMachines")),
         QDir::home().filePath(QStringLiteral(".sdkman/candidates/java")),
         QDir::home().filePath(QStringLiteral(".asdf/installs/java")),
       }) {
    QDir directory(root);
    for (const auto& name : directory.entryList(
           QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
      const QString path = directory.filePath(name);
      candidates << path
                 << QDir(path).filePath(QStringLiteral("Contents/Home"))
                 << QDir(path).filePath(
                      QStringLiteral("libexec/openjdk.jdk/Contents/Home"));
    }
  }
#endif
#if defined(Q_OS_WIN)
  for (const auto& vendor : { QStringLiteral("Microsoft"),
                              QStringLiteral("Eclipse Adoptium") }) {
    QDir vendorDirectory(QDir(qEnvironmentVariable("ProgramFiles")).filePath(vendor));
    for (const auto& directory : vendorDirectory.entryList(
           { QStringLiteral("jdk-21*") }, QDir::Dirs | QDir::NoDotAndDotDot,
           QDir::Name | QDir::Reversed)) {
      candidates << vendorDirectory.filePath(directory);
    }
  }
#endif

  const QString pathJava = QStandardPaths::findExecutable(
#if defined(Q_OS_WIN)
    QStringLiteral("java.exe")
#else
    QStringLiteral("java")
#endif
  );
  if (!pathJava.isEmpty()) {
    const QString canonicalJava = QFileInfo(pathJava).canonicalFilePath();
    candidates << QDir(QFileInfo(canonicalJava).absolutePath()).absoluteFilePath(
      QStringLiteral(".."));
  }

  candidates.removeAll(QString());
  candidates.removeDuplicates();
  for (const auto& candidate : candidates) {
#if defined(Q_OS_WIN)
    const QString java = QDir(candidate).filePath(QStringLiteral("bin/java.exe"));
#else
    const QString java = QDir(candidate).filePath(QStringLiteral("bin/java"));
#endif
    if (!QFileInfo(java).isExecutable()) continue;
    QProcess process;
    process.start(java, { QStringLiteral("--version") });
    if (!process.waitForStarted(3000) || !process.waitForFinished(5000) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
      continue;
    }
    const QString version = QString::fromUtf8(
      process.readAllStandardOutput() + process.readAllStandardError());
    if (javaMajorVersion(version) == 21) return candidate;
  }
  return {};
}

QString sanitizedFileStem(const QString& value) {
  QString result;
  bool capitalize = true;
  for (const auto c : value.trimmed()) {
    if (c.isLetterOrNumber()) {
      result.append(capitalize ? c.toUpper() : c);
      capitalize = false;
    } else {
      capitalize = true;
    }
  }
  if (result.isEmpty()) {
    result = QStringLiteral("PSXRecompiledGame");
  }
  if (result.front().isDigit()) {
    result.prepend(QStringLiteral("Game"));
  }
  return result.left(80);
}

QString sanitizedBundleIdentifier(const QString& serial, const QString& title) {
  QString suffix = serial.isEmpty() ? sanitizedFileStem(title) : serial;
  suffix = suffix.toLower();
  suffix.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
  suffix.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
  if (suffix.isEmpty()) {
    suffix = QStringLiteral("game");
  }
  return QStringLiteral("org.psxrecomp.generated.%1").arg(suffix);
}

QString cleanBundleName(const QString& title) {
  QString name = title.trimmed();
  // Path separators → readable separators (matches prior Studio behavior).
  name.replace('/', QStringLiteral(" - "));
  name.replace('\\', QStringLiteral(" - "));
  name.replace(':', QStringLiteral(" -"));
  // Drop control chars and characters that break shell quoting when CMake/Ninja
  // embeds OUTPUT_NAME into a /bin/sh -c rule. Apostrophe is the known case:
  // "The King of Fighters '98" → unescaped ' in POST_BUILD paths →
  // "unexpected EOF while looking for matching `''".
  // Also strip Windows-illegal filename chars and other shell metacharacters.
  name.remove(QRegularExpression(QStringLiteral(
    "[\\x00-\\x1f'\"`$;&|<>*?#]")));
  // Unicode apostrophe / quotes that users paste from web listings.
  name.remove(QChar(0x2018)); // ‘
  name.remove(QChar(0x2019)); // ’
  name.remove(QChar(0x201C)); // “
  name.remove(QChar(0x201D)); // ”
  name = name.simplified();
  while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
    name.chop(1);
  }
  name = name.trimmed();
  return name.left(80);
}

QString exportOutputName(const PipelineRequest& request) {
  const QString bundleName = cleanBundleName(request.windowTitle);
  const QString platformName = request.targetPlatform == TargetPlatform::MacOS
    ? QStringLiteral("macOS") : targetPlatformDisplayName(request.targetPlatform);
  if (request.exportMode == ExportMode::Source) {
    const QString sourceName = QStringLiteral("%1-%2-Source")
      .arg(bundleName, platformName);
    return request.exportAsZip ? sourceName + QStringLiteral(".zip") : sourceName;
  }
  if (request.exportAsZip) {
    switch (request.targetPlatform) {
      case TargetPlatform::MacOS:
        return bundleName + QStringLiteral("-macOS.zip");
      case TargetPlatform::Windows:
        return bundleName + QStringLiteral("-Windows.zip");
      case TargetPlatform::Linux:
        return bundleName + QStringLiteral("-Linux.zip");
      case TargetPlatform::All:
        return {};
    }
  }
  switch (request.targetPlatform) {
    case TargetPlatform::MacOS:
      return bundleName + QStringLiteral(".app");
    case TargetPlatform::Windows:
      return bundleName + QStringLiteral("-Windows");
    case TargetPlatform::Linux:
      return bundleName + QStringLiteral("-Linux");
    case TargetPlatform::All:
      return {};
  }
  return {};
}

QJsonObject gameManifestForRequest(const PipelineRequest& request) {
  const QString bundleName = cleanBundleName(request.windowTitle);
  QString executable;
  switch (request.targetPlatform) {
    case TargetPlatform::MacOS:
      executable = bundleName + QStringLiteral(".app");
      break;
    case TargetPlatform::Windows:
      executable = bundleName + QStringLiteral(".exe");
      break;
    case TargetPlatform::Linux:
      executable = bundleName;
      break;
    case TargetPlatform::All:
      break;
  }
  return {
    { QStringLiteral("executable"), executable },
    { QStringLiteral("name"), request.windowTitle },
    { QStringLiteral("platform"), targetPlatformDisplayName(request.targetPlatform) },
  };
}

QString createMacosInspectionAlias(const QString& sourcePath,
                                   const QString& aliasDirectory,
                                   QString& error) {
  if (!QFileInfo(sourcePath).isFile()) {
    error = QStringLiteral("Cannot create an inspection alias for a missing file: %1")
              .arg(sourcePath);
    return {};
  }
  if (!QDir().mkpath(aliasDirectory)) {
    error = QStringLiteral("Could not create the macOS inspection directory: %1")
              .arg(aliasDirectory);
    return {};
  }
  const QString aliasPath =
    QDir(aliasDirectory).filePath(QStringLiteral("mach-o-executable"));
  if (QFileInfo::exists(aliasPath) || QFileInfo(aliasPath).isSymLink()) {
    error = QStringLiteral("The macOS inspection alias already exists: %1")
              .arg(aliasPath);
    return {};
  }
  if (!QFile::link(QFileInfo(sourcePath).absoluteFilePath(), aliasPath) ||
      !QFileInfo(aliasPath).isSymLink() || !QFileInfo(aliasPath).isFile()) {
    error = QStringLiteral("Could not create the macOS inspection alias: %1")
              .arg(aliasPath);
    return {};
  }
  return aliasPath;
}

QString cmakeQuoted(const QString& value) {
  QString escaped = value;
  escaped.replace('\\', QStringLiteral("\\\\"));
  escaped.replace('"', QStringLiteral("\\\""));
  escaped.replace(';', QStringLiteral("\\;"));
  return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

QString tomlQuoted(const QString& value) {
  QString escaped = value;
  escaped.replace('\\', QStringLiteral("\\\\"));
  escaped.replace('"', QStringLiteral("\\\""));
  escaped.replace('\n', QStringLiteral("\\n"));
  escaped.replace('\r', QStringLiteral("\\r"));
  return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

QString hex32(quint32 value) {
  return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

QString runtimeBootToml(bool skipBiosBoot) {
  QString text;
  text += QStringLiteral("bios_hle = false\n");
  text += QStringLiteral("bios_hle_keep_intro = %1\n")
            .arg(skipBiosBoot ? QStringLiteral("false") : QStringLiteral("true"));
  text += QStringLiteral("fast_boot = %1\n")
            .arg(skipBiosBoot ? QStringLiteral("true") : QStringLiteral("false"));
  return text;
}

QString defaultControllerToml() {
  /* One package can contain any title, particularly in Batch mode. The runtime
   * therefore owns controller selection: it starts with a plain D-Pad and only
   * enables Hybrid after observing a rejected SIO configuration probe. Locking
   * the policy prevents stale per-user settings from bypassing negotiation. */
  return QStringLiteral(
           "[controller]\n"
           "p1_device = \"auto\"\n"
           "p2_device = \"none\"\n"
           "default_mode = \"auto\"\n"
           "deadzone = 12000\n"
           "allow_hybrid = true\n"
           "lock_mode = true\n\n");
}

QString macosGipCmakeOption(bool enabled) {
  return QStringLiteral("set(PSX_MACOS_GIP_GAMEPAD %1 CACHE BOOL \"\" FORCE)\n")
    .arg(enabled ? QStringLiteral("ON") : QStringLiteral("OFF"));
}

bool nmOutputHasMacosGipBackend(const QByteArray& output) {
  return output.contains("_psx_gip_gamepad_open") &&
         output.contains("_psx_gip_gamepad_get_state") &&
         output.contains("_psx_gip_gamepad_close");
}

bool writeBytes(const QString& path, const QByteArray& bytes, QString& error) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    error = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
    return false;
  }
  if (file.write(bytes) != bytes.size()) {
    error = QStringLiteral("Could not completely write %1: %2").arg(path, file.errorString());
    return false;
  }
  return true;
}

bool writeText(const QString& path, const QString& text, QString& error) {
  return writeBytes(path, text.toUtf8(), error);
}

bool writeJson(const QString& path, const QJsonObject& object, QString& error) {
  return writeBytes(path, QJsonDocument(object).toJson(QJsonDocument::Indented), error);
}

bool copyFileReplacing(const QString& source, const QString& destination, QString& error) {
  QDir().mkpath(QFileInfo(destination).absolutePath());
  if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
    error = QStringLiteral("Could not replace %1.").arg(destination);
    return false;
  }
  if (!QFile::copy(source, destination)) {
    error = QStringLiteral("Could not copy %1 to %2.").arg(source, destination);
    return false;
  }
  return true;
}

bool copyDirectoryTree(const QString& sourceDirectory,
                       const QString& destinationDirectory,
                       QString& error) {
  const QDir source(sourceDirectory);
  if (!source.exists()) {
    error = QStringLiteral("Source directory does not exist: %1").arg(sourceDirectory);
    return false;
  }
  if (!QDir().mkpath(destinationDirectory)) {
    error = QStringLiteral("Could not create directory: %1").arg(destinationDirectory);
    return false;
  }

  QDirIterator entries(sourceDirectory,
                       QDir::AllEntries | QDir::Hidden | QDir::System |
                         QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
  while (entries.hasNext()) {
    const QString sourcePath = entries.next();
    const QString relativePath = source.relativeFilePath(sourcePath);
    const QString destinationPath = QDir(destinationDirectory).filePath(relativePath);
    const QFileInfo info(sourcePath);
    if (info.isSymLink()) {
      if (QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).isSymLink()) {
        QFile::remove(destinationPath);
      }
      if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath()) ||
          !QFile::link(info.symLinkTarget(), destinationPath)) {
        error = QStringLiteral("Could not replicate symbolic link: %1").arg(relativePath);
        return false;
      }
    } else if (info.isDir()) {
      if (!QDir().mkpath(destinationPath)) {
        error = QStringLiteral("Could not create directory: %1").arg(destinationPath);
        return false;
      }
    } else if (!copyFileReplacing(sourcePath, destinationPath, error)) {
      return false;
    } else {
      QFile::setPermissions(destinationPath, info.permissions());
    }
  }
  return true;
}

namespace {

bool renderIcon(const QString& path, int size, QImage& output, QString& error) {
  output = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
  output.fill(Qt::transparent);
  if (QFileInfo(path).suffix().compare(QStringLiteral("svg"), Qt::CaseInsensitive) == 0) {
    QSvgRenderer renderer(path);
    if (!renderer.isValid()) {
      error = QStringLiteral("The selected SVG icon is invalid.");
      return false;
    }
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return true;
  }

  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QImage source = reader.read();
  if (source.isNull()) {
    error = QStringLiteral("Could not decode the selected icon: %1").arg(reader.errorString());
    return false;
  }
  const auto scaled = source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QPainter painter(&output);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
  return true;
}

bool runIconutil(const QString& iconsetPath, const QString& outputPath, QString& error) {
  QProcess process;
  process.start(QStringLiteral("/usr/bin/iconutil"),
                { QStringLiteral("-c"), QStringLiteral("icns"), iconsetPath,
                  QStringLiteral("-o"), outputPath });
  if (!process.waitForStarted(5000) || !process.waitForFinished(60000) || process.exitCode() != 0) {
    error = QStringLiteral("iconutil failed: %1")
              .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
    return false;
  }
  return QFileInfo::exists(outputPath);
}

void appendBe32(QByteArray& bytes, quint32 value) {
  bytes.append(static_cast<char>((value >> 24u) & 0xffu));
  bytes.append(static_cast<char>((value >> 16u) & 0xffu));
  bytes.append(static_cast<char>((value >> 8u) & 0xffu));
  bytes.append(static_cast<char>(value & 0xffu));
}

bool writePngBackedIcns(const QString& iconsetPath,
                        const QString& outputPath,
                        QString& error) {
  struct Element { const char* type; const char* file; };
  static constexpr Element elements[]{
    { "icp4", "icon_16x16.png" },
    { "icp5", "icon_32x32.png" },
    { "icp6", "icon_32x32@2x.png" },
    { "ic07", "icon_128x128.png" },
    { "ic08", "icon_256x256.png" },
    { "ic09", "icon_512x512.png" },
    { "ic10", "icon_512x512@2x.png" },
  };
  QByteArray payload;
  for (const auto& element : elements) {
    QFile png(QDir(iconsetPath).filePath(QString::fromLatin1(element.file)));
    if (!png.open(QIODevice::ReadOnly)) {
      error = QStringLiteral("Could not read ICNS source %1: %2")
                .arg(png.fileName(), png.errorString());
      return false;
    }
    const QByteArray image = png.readAll();
    if (!image.startsWith("\x89PNG\r\n\x1a\n")) {
      error = QStringLiteral("ICNS source is not a PNG: %1").arg(png.fileName());
      return false;
    }
    payload.append(element.type, 4);
    appendBe32(payload, static_cast<quint32>(image.size() + 8));
    payload.append(image);
  }
  QByteArray icns("icns", 4);
  appendBe32(icns, static_cast<quint32>(payload.size() + 8));
  icns.append(payload);
  return writeBytes(outputPath, icns, error);
}

} // namespace

bool createMacosIconset(const QString& sourceIcon,
                        const QString& iconsetPath,
                        QString& error) {
  QDir iconset(iconsetPath);
  if (iconset.exists() && !iconset.removeRecursively()) {
    error = QStringLiteral("Could not reset the macOS iconset directory.");
    return false;
  }
  if (!QDir().mkpath(iconsetPath)) {
    error = QStringLiteral("Could not create the macOS iconset directory.");
    return false;
  }

  const QList<int> logicalSizes{ 16, 32, 128, 256, 512 };
  for (const int logicalSize : logicalSizes) {
    for (const int scale : { 1, 2 }) {
      const int pixelSize = logicalSize * scale;
      QImage image;
      if (!renderIcon(sourceIcon, pixelSize, image, error)) {
        return false;
      }
      const QString suffix = scale == 2 ? QStringLiteral("@2x") : QString();
      const QString name = QStringLiteral("icon_%1x%1%2.png").arg(logicalSize).arg(suffix);
      if (!image.save(iconset.filePath(name), "PNG")) {
        error = QStringLiteral("Could not write iconset image %1.").arg(name);
        return false;
      }
    }
  }
  return true;
}

bool createIcns(const QString& sourceIcon,
                const QString& workingDirectory,
                const QString& outputPath,
                QString& error) {
  if (QFileInfo(sourceIcon).suffix().compare(QStringLiteral("icns"), Qt::CaseInsensitive) == 0) {
    return copyFileReplacing(sourceIcon, outputPath, error);
  }

  const QString iconsetPath = QDir(workingDirectory).filePath(QStringLiteral("AppIcon.iconset"));
  if (!createMacosIconset(sourceIcon, iconsetPath, error)) {
    return false;
  }
  QDir().mkpath(QFileInfo(outputPath).absolutePath());
  QString iconutilError;
  if (runIconutil(iconsetPath, outputPath, iconutilError)) return true;
  if (writePngBackedIcns(iconsetPath, outputPath, error)) {
    error.clear();
    return true;
  }
  if (error.isEmpty()) error = iconutilError;
  return false;
}

bool createIco(const QString& sourceIcon,
               const QString& outputPath,
               QString& error) {
  QImage image;
  if (!renderIcon(sourceIcon, 256, image, error)) {
    return false;
  }
  QDir().mkpath(QFileInfo(outputPath).absolutePath());
  if (!image.save(outputPath, "ICO")) {
    error = QStringLiteral("Could not encode the selected icon as a Windows ICO file.");
    return false;
  }
  return true;
}

bool createPngIcon(const QString& sourceIcon,
                   const QString& outputPath,
                   QString& error) {
  QImage image;
  if (!renderIcon(sourceIcon, 512, image, error)) {
    return false;
  }
  QDir().mkpath(QFileInfo(outputPath).absolutePath());
  if (!image.save(outputPath, "PNG")) {
    error = QStringLiteral("Could not encode the selected icon as a Linux PNG file.");
    return false;
  }
  return true;
}

namespace {

struct PackageArchiveExpectation {
  bool directory{ false };
  bool symbolicLink{ false };
  quint64 size{ 0 };
  QFile::Permissions permissions;
  QByteArray sha256;
};

constexpr qsizetype kArchiveBufferSize = 1024 * 1024;

QFile::Permissions portableArchivePermissions(QFile::Permissions permissions) {
  constexpr QFile::Permissions portableMask =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
    QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
    QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
  return permissions & portableMask;
}

bool archiveCancelled(const std::function<bool()>& cancellationRequested) {
  return cancellationRequested && cancellationRequested();
}

} // namespace

bool createPackageArchive(
    const QString& sourceDirectory,
    const QString& rootDirectoryName,
    const QString& outputPath,
    QString& error,
    const QMap<QString, QByteArray>& rootFiles,
    const std::function<bool()>& cancellationRequested) {
  error.clear();
  const QFileInfo sourceInfo(sourceDirectory);
  if (!sourceInfo.isDir()) {
    error = QStringLiteral("Cannot archive a missing package directory: %1")
              .arg(sourceDirectory);
    return false;
  }
  if (rootDirectoryName == QStringLiteral(".") ||
      rootDirectoryName == QStringLiteral("..") ||
      rootDirectoryName.contains('/') || rootDirectoryName.contains('\\')) {
    error = QStringLiteral("The ZIP root directory name is invalid: %1")
              .arg(rootDirectoryName);
    return false;
  }

  QString normalizedSource = QDir::cleanPath(sourceInfo.absoluteFilePath());
  QString normalizedOutput = QDir::cleanPath(QFileInfo(outputPath).absoluteFilePath());
#if defined(Q_OS_WIN)
  normalizedSource = normalizedSource.toCaseFolded();
  normalizedOutput = normalizedOutput.toCaseFolded();
#endif
  if (normalizedOutput.startsWith(normalizedSource + QLatin1Char('/'))) {
    error = QStringLiteral("The package ZIP cannot be created inside the package being archived.");
    return false;
  }
  if (!QDir().mkpath(QFileInfo(outputPath).absolutePath())) {
    error = QStringLiteral("Could not create the package ZIP output directory.");
    return false;
  }
  if ((QFileInfo::exists(outputPath) || QFileInfo(outputPath).isSymLink()) &&
      !QFile::remove(outputPath)) {
    error = QStringLiteral("Could not replace the package ZIP: %1").arg(outputPath);
    return false;
  }

  QStringList sourcePaths;
  QDirIterator iterator(
    sourceDirectory,
    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
    QDirIterator::Subdirectories);
  while (iterator.hasNext()) sourcePaths.append(iterator.next());
  const QDir sourceRoot(sourceDirectory);
  std::sort(sourcePaths.begin(), sourcePaths.end(), [&](const QString& left,
                                                        const QString& right) {
    return sourceRoot.relativeFilePath(left) < sourceRoot.relativeFilePath(right);
  });

  QuaZip archive(outputPath);
  archive.setZip64Enabled(true);
  if (!archive.open(QuaZip::mdCreate)) {
    error = QStringLiteral("Could not create package ZIP %1.").arg(outputPath);
    return false;
  }

  QHash<QString, PackageArchiveExpectation> expected;
  auto addEntry = [&](const QString& sourcePath, QString archiveName) -> bool {
    if (archiveCancelled(cancellationRequested)) {
      error = QStringLiteral("ZIP export was cancelled.");
      return false;
    }
    const QFileInfo info(sourcePath);
    const bool symbolicLink = info.isSymLink();
    const bool directory = !symbolicLink && info.isDir();
    if (!directory && !symbolicLink && !info.isFile()) {
      error = QStringLiteral("The package contains an unsupported filesystem entry: %1")
                .arg(sourcePath);
      return false;
    }
    archiveName = QDir::fromNativeSeparators(archiveName);
    if (directory && !archiveName.endsWith('/')) archiveName.append('/');
    if (archiveName.isEmpty() || archiveName.startsWith('/') ||
        archiveName == QStringLiteral("../") || archiveName.startsWith(QStringLiteral("../")) ||
        archiveName.contains(QStringLiteral("/../")) || expected.contains(archiveName)) {
      error = QStringLiteral("The package produced an invalid or duplicate ZIP entry: %1")
                .arg(archiveName);
      return false;
    }

    QuaZipNewInfo zipInfo(archiveName, sourcePath);
    QuaZipFile output(&archive);
    if (!output.open(QIODevice::WriteOnly, zipInfo, nullptr, 0,
                     directory ? 0 : Z_DEFLATED,
                     directory ? 0 : Z_DEFAULT_COMPRESSION)) {
      error = QStringLiteral("Could not create ZIP entry %1.").arg(archiveName);
      return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    quint64 bytesWritten = 0;
    bool writeSucceeded = true;
    if (symbolicLink) {
      const QString target = info.symLinkTarget();
      if (target.isEmpty()) {
        error = QStringLiteral("Could not resolve package symlink %1.").arg(sourcePath);
        writeSucceeded = false;
      } else {
        const QByteArray data = QFile::encodeName(info.dir().relativeFilePath(target));
        hash.addData(data);
        bytesWritten = static_cast<quint64>(data.size());
        writeSucceeded = output.write(data) == data.size();
      }
    } else if (!directory) {
      QFile input(sourcePath);
      if (!input.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Could not read package file %1: %2")
                  .arg(sourcePath, input.errorString());
        writeSucceeded = false;
      }
      while (writeSucceeded && input.isOpen() && !input.atEnd()) {
        if (archiveCancelled(cancellationRequested)) {
          error = QStringLiteral("ZIP export was cancelled.");
          writeSucceeded = false;
          break;
        }
        const QByteArray data = input.read(kArchiveBufferSize);
        if (data.isEmpty()) {
          if (input.atEnd()) break;
          error = QStringLiteral("Could not completely read package file %1: %2")
                    .arg(sourcePath, input.errorString());
          writeSucceeded = false;
          break;
        }
        hash.addData(data);
        bytesWritten += static_cast<quint64>(data.size());
        if (output.write(data) != data.size()) {
          error = QStringLiteral("Could not completely write ZIP entry %1.")
                    .arg(archiveName);
          writeSucceeded = false;
        }
      }
    }
    output.close();
    if (!writeSucceeded || output.getZipError() != UNZ_OK) {
      if (error.isEmpty()) {
        error = QStringLiteral("QuaZip could not finalize ZIP entry %1.").arg(archiveName);
      }
      return false;
    }
    expected.insert(archiveName, {
      directory,
      symbolicLink,
      bytesWritten,
      portableArchivePermissions(info.permissions()),
      directory ? QByteArray() : hash.result(),
    });
    return true;
  };
  auto addRootFile = [&](QString archiveName, const QByteArray& data) -> bool {
    if (archiveCancelled(cancellationRequested)) {
      error = QStringLiteral("ZIP export was cancelled.");
      return false;
    }
    archiveName = QDir::fromNativeSeparators(archiveName);
    if (archiveName.isEmpty() || archiveName.startsWith('/') ||
        archiveName.contains('/') || archiveName == QStringLiteral(".") ||
        archiveName == QStringLiteral("..") || expected.contains(archiveName)) {
      error = QStringLiteral("The package produced an invalid or duplicate ZIP root file: %1")
                .arg(archiveName);
      return false;
    }
    QuaZipNewInfo zipInfo(archiveName);
    const QFile::Permissions permissions =
      QFileDevice::ReadOwner | QFileDevice::WriteOwner |
      QFileDevice::ReadGroup | QFileDevice::ReadOther;
    zipInfo.setPermissions(permissions);
    QuaZipFile output(&archive);
    if (!output.open(QIODevice::WriteOnly, zipInfo) ||
        output.write(data) != data.size()) {
      output.close();
      error = QStringLiteral("Could not create ZIP root file %1.").arg(archiveName);
      return false;
    }
    output.close();
    if (output.getZipError() != UNZ_OK) {
      error = QStringLiteral("QuaZip could not finalize ZIP root file %1.")
                .arg(archiveName);
      return false;
    }
    expected.insert(archiveName, {
      false,
      false,
      static_cast<quint64>(data.size()),
      portableArchivePermissions(permissions),
      QCryptographicHash::hash(data, QCryptographicHash::Sha256),
    });
    return true;
  };

  if (!rootDirectoryName.isEmpty() &&
      !addEntry(sourceDirectory, rootDirectoryName + QLatin1Char('/'))) {
    archive.close();
    QFile::remove(outputPath);
    return false;
  }
  for (auto rootFile = rootFiles.cbegin(); rootFile != rootFiles.cend(); ++rootFile) {
    if (!addRootFile(rootFile.key(), rootFile.value())) {
      archive.close();
      QFile::remove(outputPath);
      return false;
    }
  }
  for (const QString& sourcePath : sourcePaths) {
    QString archiveName = QDir::fromNativeSeparators(
      sourceRoot.relativeFilePath(sourcePath));
    if (!rootDirectoryName.isEmpty()) {
      archiveName.prepend(rootDirectoryName + QLatin1Char('/'));
    }
    if (!addEntry(sourcePath, archiveName)) {
      archive.close();
      QFile::remove(outputPath);
      return false;
    }
  }
  archive.close();
  if (archive.getZipError() != UNZ_OK) {
    error = QStringLiteral("QuaZip reported error %1 while closing package ZIP %2.")
              .arg(archive.getZipError()).arg(outputPath);
    QFile::remove(outputPath);
    return false;
  }

  QuaZip verification(outputPath);
  if (!verification.open(QuaZip::mdUnzip)) {
    error = QStringLiteral("The package ZIP could not be reopened for verification: %1")
              .arg(outputPath);
    QFile::remove(outputPath);
    return false;
  }
  QSet<QString> observed;
  QString verificationError;
  bool hasEntry = verification.goToFirstFile();
  while (hasEntry) {
    if (archiveCancelled(cancellationRequested)) {
      verificationError = QStringLiteral("ZIP export was cancelled.");
      break;
    }
    QuaZipFileInfo64 info;
    if (!verification.getCurrentFileInfo(&info)) {
      verificationError = QStringLiteral("Could not read package ZIP entry metadata.");
      break;
    }
    const auto expectationIt = expected.constFind(info.name);
    if (expectationIt == expected.cend() || observed.contains(info.name)) {
      verificationError = QStringLiteral("The package ZIP contains an unexpected or duplicate entry: %1")
                            .arg(info.name);
      break;
    }
    const auto& expectation = expectationIt.value();
    const bool directory = info.name.endsWith('/');
    if (directory != expectation.directory ||
        info.isSymbolicLink() != expectation.symbolicLink ||
        info.uncompressedSize != expectation.size ||
        portableArchivePermissions(info.getPermissions()) != expectation.permissions) {
      verificationError = QStringLiteral("The package ZIP metadata does not match its source: %1")
                            .arg(info.name);
      break;
    }
    if (!directory) {
      QuaZipFile input(&verification);
      if (!input.open(QIODevice::ReadOnly)) {
        verificationError = QStringLiteral("Could not read package ZIP entry %1.")
                              .arg(info.name);
        break;
      }
      QCryptographicHash hash(QCryptographicHash::Sha256);
      quint64 bytesRead = 0;
      while (!input.atEnd()) {
        if (archiveCancelled(cancellationRequested)) {
          verificationError = QStringLiteral("ZIP export was cancelled.");
          break;
        }
        const QByteArray data = input.read(kArchiveBufferSize);
        if (data.isEmpty()) {
          if (input.atEnd()) break;
          verificationError = QStringLiteral("Package ZIP entry %1 failed CRC verification.")
                                .arg(info.name);
          break;
        }
        hash.addData(data);
        bytesRead += static_cast<quint64>(data.size());
      }
      input.close();
      if (verificationError.isEmpty() &&
          (input.getZipError() != UNZ_OK || bytesRead != expectation.size ||
           hash.result() != expectation.sha256)) {
        verificationError = QStringLiteral("Package ZIP entry %1 does not match its source bytes.")
                              .arg(info.name);
      }
      if (!verificationError.isEmpty()) break;
    }
    observed.insert(info.name);
    hasEntry = verification.goToNextFile();
  }
  verification.close();
  if (verificationError.isEmpty() && observed.size() != expected.size()) {
    verificationError = QStringLiteral("The package ZIP is missing one or more source entries.");
  }
  if (!verificationError.isEmpty()) {
    error = verificationError;
    QFile::remove(outputPath);
    return false;
  }
  return true;
}

bool createProofArchive(const QString& proofDirectory,
                        const QString& outputPath,
                        QString& error) {
  QDir().mkpath(QFileInfo(outputPath).absolutePath());
  if (QFileInfo::exists(outputPath) && !QFile::remove(outputPath)) {
    error = QStringLiteral("Could not replace the proof archive.");
    return false;
  }

  QuaZip archive(outputPath);
  if (!archive.open(QuaZip::mdCreate)) {
    error = QStringLiteral("Could not create %1.").arg(outputPath);
    return false;
  }

  QDir base(proofDirectory);
  QDirIterator it(proofDirectory, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString sourcePath = it.next();
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
      archive.close();
      error = QStringLiteral("Could not read proof artifact %1.").arg(sourcePath);
      return false;
    }
    const QString relative = base.relativeFilePath(sourcePath);
    QuaZipFile output(&archive);
    QuaZipNewInfo info(relative, sourcePath);
    if (!output.open(QIODevice::WriteOnly, info)) {
      archive.close();
      error = QStringLiteral("Could not add %1 to the proof archive.").arg(relative);
      return false;
    }
    const QByteArray data = source.readAll();
    if (output.write(data) != data.size()) {
      output.close();
      archive.close();
      error = QStringLiteral("Could not completely archive %1.").arg(relative);
      return false;
    }
    output.close();
  }
  archive.close();
  if (archive.getZipError() != UNZ_OK) {
    error = QStringLiteral("QuaZip reported error %1 while closing the proof archive.")
              .arg(archive.getZipError());
    return false;
  }
  return true;
}

QList<QPair<quint32, quint32>> parseCodeRanges(const QString& path, QString& error) {
  QList<QPair<quint32, quint32>> ranges;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    error = QStringLiteral("Could not read the generated range manifest: %1").arg(file.errorString());
    return {};
  }
  while (!file.atEnd()) {
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    if (!line.startsWith(QStringLiteral("R "))) {
      continue;
    }
    const auto parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() != 3) {
      error = QStringLiteral("Malformed generated range line: %1").arg(line);
      return {};
    }
    bool okStart = false;
    bool okLength = false;
    const quint32 start = parts.at(1).toUInt(&okStart, 16);
    const quint32 length = parts.at(2).toUInt(&okLength, 16);
    if (!okStart || !okLength || length == 0 || start + length < start) {
      error = QStringLiteral("Invalid generated range line: %1").arg(line);
      return {};
    }
    ranges.append({ start, start + length });
  }
  std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });
  return ranges;
}


QSet<quint32> parseFunctionEntries(const QString& path, QString& error) {
  QSet<quint32> entries;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    error = QStringLiteral("Could not read the generated range manifest: %1").arg(file.errorString());
    return {};
  }
  while (!file.atEnd()) {
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    if (!line.startsWith(QStringLiteral("F "))) {
      continue;
    }
    const auto parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() != 2) {
      error = QStringLiteral("Malformed generated function-entry line: %1").arg(line);
      return {};
    }
    bool ok = false;
    const quint32 entry = parts.at(1).toUInt(&ok, 16);
    if (!ok) {
      error = QStringLiteral("Invalid generated function-entry line: %1").arg(line);
      return {};
    }
    entries.insert(entry);
  }
  return entries;
}

bool addressInRanges(quint32 address, const QList<QPair<quint32, quint32>>& ranges) {
  for (const auto& range : ranges) {
    if (address < range.first) {
      return false;
    }
    if (address >= range.first && address < range.second) {
      return true;
    }
  }
  return false;
}

QString findExecutable(const QString& name) {
  const QString found = QStandardPaths::findExecutable(name);
  if (!found.isEmpty()) {
    return found;
  }
  QStringList prefixes;
  const QString cargoHome = qEnvironmentVariable("CARGO_HOME");
  if (!cargoHome.isEmpty()) prefixes << QDir(cargoHome).filePath(QStringLiteral("bin"));
  if (!QDir::homePath().isEmpty())
    prefixes << QDir(QDir::homePath()).filePath(QStringLiteral(".cargo/bin"));
  prefixes << QStringLiteral("/usr/local/bin") << QStringLiteral("/opt/homebrew/bin")
           << QStringLiteral("/usr/bin") << QStringLiteral("/bin");
  for (const auto& prefix : prefixes) {
    const QString path = QDir(prefix).filePath(name);
    if (QFileInfo(path).isExecutable()) {
      return path;
    }
  }
  return {};
}

QString ghidraAnalyzeHeadlessPath(const QString& ghidraHome) {
  if (!QFileInfo(QDir(ghidraHome).filePath(
        QStringLiteral("Ghidra/application.properties"))).isFile()) {
    return {};
  }
  const QDir supportDir(QDir(ghidraHome).filePath(QStringLiteral("support")));
#if defined(Q_OS_WIN)
  const QString launcher = supportDir.filePath(QStringLiteral("analyzeHeadless.bat"));
  return QFileInfo(launcher).isFile() ? launcher : QString();
#else
  const QString launcher = supportDir.filePath(QStringLiteral("analyzeHeadless"));
  return QFileInfo(launcher).isExecutable() ? launcher : QString();
#endif
}

} // namespace psxstudio
