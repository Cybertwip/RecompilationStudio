#pragma once

#include "PipelineTypes.h"

#include <QString>

namespace psxstudio {

class DiscInspector final {
public:
  static bool inspect(const DiscDescription& disc, GameDescription& out, QString& error);
};

} // namespace psxstudio
