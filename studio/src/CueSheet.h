#pragma once

#include "PipelineTypes.h"

#include <QString>
#include <QStringList>

namespace psxstudio {

class CueSheet final {
public:
  static bool parse(const QString& cuePath,
                    const QStringList& selectedBinPaths,
                    DiscDescription& out,
                    QString& error);
};

} // namespace psxstudio
