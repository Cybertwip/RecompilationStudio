#include "PipelineSupport.h"

#include "quazip.h"
#include "quazipfile.h"
#include "quazipnewinfo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSvgRenderer>

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
                       QDir::AllEntries | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
  while (entries.hasNext()) {
    const QString sourcePath = entries.next();
    const QString relativePath = source.relativeFilePath(sourcePath);
    const QString destinationPath = QDir(destinationDirectory).filePath(relativePath);
    const QFileInfo info(sourcePath);
    if (info.isDir()) {
      if (!QDir().mkpath(destinationPath)) {
        error = QStringLiteral("Could not create directory: %1").arg(destinationPath);
        return false;
      }
    } else if (!copyFileReplacing(sourcePath, destinationPath, error)) {
      return false;
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

} // namespace

bool createIcns(const QString& sourceIcon,
                const QString& workingDirectory,
                const QString& outputPath,
                QString& error) {
  if (QFileInfo(sourceIcon).suffix().compare(QStringLiteral("icns"), Qt::CaseInsensitive) == 0) {
    return copyFileReplacing(sourceIcon, outputPath, error);
  }

  const QString iconsetPath = QDir(workingDirectory).filePath(QStringLiteral("AppIcon.iconset"));
  QDir iconset(iconsetPath);
  if (iconset.exists() && !iconset.removeRecursively()) {
    error = QStringLiteral("Could not reset the temporary iconset directory.");
    return false;
  }
  if (!QDir().mkpath(iconsetPath)) {
    error = QStringLiteral("Could not create the temporary iconset directory.");
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
  QDir().mkpath(QFileInfo(outputPath).absolutePath());
  return runIconutil(iconsetPath, outputPath, error);
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
  for (const auto& prefix : { QStringLiteral("/usr/local/bin"), QStringLiteral("/opt/homebrew/bin"),
                              QStringLiteral("/usr/bin"), QStringLiteral("/bin") }) {
    const QString path = QDir(prefix).filePath(name);
    if (QFileInfo(path).isExecutable()) {
      return path;
    }
  }
  return {};
}

} // namespace psxstudio
