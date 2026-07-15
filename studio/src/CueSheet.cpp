#include "CueSheet.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace psxstudio {

namespace {

QString canonicalOrAbsolute(const QString& path) {
  const QFileInfo info(path);
  const auto canonical = info.canonicalFilePath();
  return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QString lookupSelected(const QString& reference, const QStringList& selectedBinPaths) {
  const QString wanted = QFileInfo(reference).fileName();
  for (const auto& selected : selectedBinPaths) {
    if (QFileInfo(selected).fileName().compare(wanted, Qt::CaseInsensitive) == 0) {
      return selected;
    }
  }
  return {};
}

} // namespace

bool CueSheet::parse(const QString& cuePath,
                     const QStringList& selectedBinPaths,
                     DiscDescription& out,
                     QString& error) {
  out = {};
  const QFileInfo cueInfo(cuePath);
  if (!cueInfo.isFile() || cueInfo.suffix().compare(QStringLiteral("cue"), Qt::CaseInsensitive) != 0) {
    error = QStringLiteral("Select exactly one readable .cue file.");
    return false;
  }

  QFile cueFile(cuePath);
  if (!cueFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    error = QStringLiteral("Could not read the CUE file: %1").arg(cueFile.errorString());
    return false;
  }
  QString text = QString::fromUtf8(cueFile.readAll());
  cueFile.close();

  const QRegularExpression filePattern(
    QStringLiteral(R"cue((?im)^(\s*FILE\s+)(?:"([^"]+)"|(\S+))(\s+BINARY\s*)$)cue"));
  auto matches = filePattern.globalMatch(text);
  struct Replacement {
    qsizetype start;
    qsizetype length;
    QString text;
  };
  QList<Replacement> replacements;
  QSet<QString> packagedNames;
  QList<DiscFile> files;

  while (matches.hasNext()) {
    const auto match = matches.next();
    const QString reference = match.captured(2).isEmpty() ? match.captured(3) : match.captured(2);
    QString sourcePath = QDir(cueInfo.absolutePath()).filePath(reference);
    if (!QFileInfo::exists(sourcePath)) {
      sourcePath = lookupSelected(reference, selectedBinPaths);
    }
    if (sourcePath.isEmpty() || !QFileInfo(sourcePath).isFile()) {
      error = QStringLiteral("The CUE references a missing BIN file: %1").arg(reference);
      return false;
    }

    const QString packagedName = QFileInfo(reference).fileName();
    const QString foldedName = packagedName.toCaseFolded();
    if (packagedNames.contains(foldedName)) {
      error = QStringLiteral("The CUE contains duplicate BIN basenames: %1").arg(packagedName);
      return false;
    }
    packagedNames.insert(foldedName);

    files.append({ reference, canonicalOrAbsolute(sourcePath), packagedName });
    replacements.prepend({
      match.capturedStart(),
      match.capturedLength(),
      match.captured(1) + QStringLiteral("\"") + packagedName + QStringLiteral("\"") + match.captured(4),
    });
  }

  if (files.isEmpty()) {
    error = QStringLiteral("The selected CUE has no BINARY FILE entries.");
    return false;
  }

  for (const auto& replacement : replacements) {
    text.replace(replacement.start, replacement.length, replacement.text);
  }

  out.cuePath = canonicalOrAbsolute(cuePath);
  out.rewrittenCue = text;
  out.files = files;
  return true;
}

} // namespace psxstudio
