#pragma once

#include "PipelineTypes.h"

#include <QList>
#include <QString>

namespace psxstudio {

struct GbaDescription {
  QString title;
  QString gameCode;
  QString makerCode;
  QString saveType;
  quint32 entryWord{ 0 };
  quint32 entryTarget{ 0x08000000u };
  qint64 romSize{ 0 };
  bool headerBranchValid{ false };
  bool complementValid{ false };
  QString warning;
};

struct GbaCatalogEntry {
  QString sourcePath;
  QString suggestedTitle;
  QString gameCode;
};

bool inspectGbaRom(const QString& path, GbaDescription& description, QString& error);
bool scanGbaDirectory(const QString& directory,
                      QList<GbaCatalogEntry>& entries,
                      QStringList& warnings,
                      QString& error);
QString sha1File(const QString& path, QString& error);
QString generatedGbaMainCpp(const QString& bundleName,
                            const QString& romSha1,
                            quint32 romCrc32);
QString generatedGbaGameToml(const PipelineRequest& request,
                             const GbaDescription& game,
                             const QString& romSha1,
                             quint32 romCrc32,
                             const QString& biosSha1,
                             quint32 biosCrc32,
                             bool biosHle);
QString generatedGbaProjectCMake(const PipelineRequest& request,
                                 const GbaDescription& game,
                                 const QString& bundleName,
                                 const QString& bundleId);
QString generatedGbaNativeProjectCMake(const QString& bundleName);

QString generatedGbaPackToml(const PipelineRequest& request,
                             const QString& bundleName,
                             const QString& romSha256,
                             const QString& biosSha256);
QString generatedGbaInfoPlist(const QString& bundleName,
                              const QString& bundleId);

} // namespace psxstudio
