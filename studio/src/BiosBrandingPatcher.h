#pragma once

#include <QString>

namespace psxstudio {

struct BiosBrandingPatchOptions {
  QString inputBiosPath;
  QString outputBiosPath;
  QString initialSplashImagePath;
  QString handoffImagePath;
  QString manifestPath;
  bool muteBootAudio{ true };
  bool removeStockPsGlyph{ true };
};

class BiosBrandingPatcher final {
public:
  static bool patch(const BiosBrandingPatchOptions& options, QString& error);
};

} // namespace psxstudio
