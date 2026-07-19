#include "DiscCatalog.h"

#include "CueSheet.h"
#include "DiscInspector.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace psxstudio {

namespace {

QString canonicalOrAbsolute(const QString& path) {
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QString suggestedTitle(const QString& sourcePath, const GameDescription& game) {
  QString title = QFileInfo(sourcePath).completeBaseName().trimmed();
  if (title.isEmpty()) {
    title = game.volumeId.trimmed();
  }
  if (title.isEmpty()) {
    title = game.serial.trimmed();
  }
  return title.isEmpty() ? QStringLiteral("PlayStation Game") : title;
}

void recordCueOwnedBins(const QString& cuePath,
                        const QStringList& binPaths,
                        QSet<QString>& ownedBins) {
  QFile cue(cuePath);
  if (!cue.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return;
  }
  const QString text = QString::fromUtf8(cue.readAll());
  const QRegularExpression filePattern(
    QStringLiteral(R"cue((?im)^\s*FILE\s+(?:"([^"]+)"|(\S+))\s+BINARY\s*$)cue"));
  auto matches = filePattern.globalMatch(text);
  while (matches.hasNext()) {
    const auto match = matches.next();
    const QString reference = match.captured(1).isEmpty()
      ? match.captured(2) : match.captured(1);
    const QString relativePath = QDir(QFileInfo(cuePath).absolutePath()).filePath(reference);
    if (QFileInfo(relativePath).isFile()) {
      ownedBins.insert(canonicalOrAbsolute(relativePath).toCaseFolded());
      continue;
    }
    const QString referencedName = QFileInfo(reference).fileName();
    for (const auto& binPath : binPaths) {
      if (QFileInfo(binPath).fileName().compare(referencedName,
                                                Qt::CaseInsensitive) == 0) {
        ownedBins.insert(canonicalOrAbsolute(binPath).toCaseFolded());
      }
    }
  }
}

} // namespace

bool DiscCatalog::scanDirectory(const QString& directory,
                                QList<DiscCatalogEntry>& entries,
                                QStringList& warnings,
                                QString& error) {
  entries.clear();
  warnings.clear();
  error.clear();

  const QFileInfo rootInfo(directory);
  if (!rootInfo.isDir()) {
    error = QStringLiteral("Select a readable directory containing PlayStation BIN/CUE files.");
    return false;
  }

  QStringList cuePaths;
  QStringList binPaths;
  QDirIterator iterator(directory, QDir::Files, QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString path = canonicalOrAbsolute(iterator.next());
    const QString suffix = QFileInfo(path).suffix();
    if (suffix.compare(QStringLiteral("cue"), Qt::CaseInsensitive) == 0) {
      cuePaths.append(path);
    } else if (suffix.compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0) {
      binPaths.append(path);
    }
  }
  cuePaths.removeDuplicates();
  binPaths.removeDuplicates();
  cuePaths.sort(Qt::CaseInsensitive);
  binPaths.sort(Qt::CaseInsensitive);

  QSet<QString> cueOwnedBins;
  for (const QString& cuePath : cuePaths) {
    /* A broken multi-track CUE still owns any tracks it names. Otherwise an
     * incomplete disc could be silently reclassified as a standalone BIN. */
    recordCueOwnedBins(cuePath, binPaths, cueOwnedBins);
    DiscDescription disc;
    QString discError;
    if (!CueSheet::parse(cuePath, binPaths, disc, discError)) {
      warnings.append(QStringLiteral("%1 — %2")
                        .arg(QDir(directory).relativeFilePath(cuePath), discError));
      continue;
    }
    for (const auto& file : disc.files) {
      cueOwnedBins.insert(canonicalOrAbsolute(file.sourcePath).toCaseFolded());
    }

    GameDescription game;
    if (!DiscInspector::inspect(disc, game, discError)) {
      warnings.append(QStringLiteral("%1 — %2")
                        .arg(QDir(directory).relativeFilePath(cuePath), discError));
      continue;
    }
    QStringList selectedBins;
    for (const auto& file : disc.files) {
      selectedBins.append(file.sourcePath);
    }
    entries.append({ cuePath, selectedBins, suggestedTitle(cuePath, game),
                     game.serial, game.volumeId });
  }

  for (const QString& binPath : binPaths) {
    if (cueOwnedBins.contains(canonicalOrAbsolute(binPath).toCaseFolded())) {
      continue;
    }
    DiscDescription disc;
    GameDescription game;
    QString discError;
    if (!CueSheet::parse(binPath, {}, disc, discError) ||
        !DiscInspector::inspect(disc, game, discError)) {
      warnings.append(QStringLiteral("%1 — %2")
                        .arg(QDir(directory).relativeFilePath(binPath), discError));
      continue;
    }
    entries.append({ binPath, {}, suggestedTitle(binPath, game),
                     game.serial, game.volumeId });
  }

  std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
    const int titleOrder = lhs.suggestedTitle.compare(rhs.suggestedTitle, Qt::CaseInsensitive);
    return titleOrder == 0
      ? lhs.sourcePath.compare(rhs.sourcePath, Qt::CaseInsensitive) < 0
      : titleOrder < 0;
  });

  if (entries.isEmpty()) {
    error = cuePaths.isEmpty() && binPaths.isEmpty()
      ? QStringLiteral("The selected directory contains no .cue or .bin files.")
      : QStringLiteral("No valid PlayStation discs were found in the selected directory.");
    if (!warnings.isEmpty()) {
      error += QStringLiteral("\n\n") + warnings.join(QStringLiteral("\n"));
    }
    return false;
  }
  return true;
}

} // namespace psxstudio
