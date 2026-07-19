#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace psxstudio {

struct DiscCatalogEntry {
  QString sourcePath;
  QStringList selectedBinPaths;
  QString suggestedTitle;
  QString serial;
  QString volumeId;
};

class DiscCatalog final {
public:
  static bool scanDirectory(const QString& directory,
                            QList<DiscCatalogEntry>& entries,
                            QStringList& warnings,
                            QString& error);
};

} // namespace psxstudio
