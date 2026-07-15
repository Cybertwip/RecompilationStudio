#include "BiosBrandingPatcher.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  psxstudio::BiosBrandingPatchOptions options;
  const QStringList args = app.arguments();
  for (int i = 1; i < args.size(); ++i) {
    const QString arg = args[i];
    auto take = [&](QString& value) -> bool {
      if (i + 1 >= args.size()) return false;
      value = args[++i]; return true;
    };
    if (arg == QStringLiteral("--bios")) { if (!take(options.inputBiosPath)) return 2; }
    else if (arg == QStringLiteral("--output")) { if (!take(options.outputBiosPath)) return 2; }
    else if (arg == QStringLiteral("--initial-image")) { if (!take(options.initialSplashImagePath)) return 2; }
    else if (arg == QStringLiteral("--handoff-image")) { if (!take(options.handoffImagePath)) return 2; }
    else if (arg == QStringLiteral("--manifest")) { if (!take(options.manifestPath)) return 2; }
    else if (arg == QStringLiteral("--keep-audio")) options.muteBootAudio = false;
    else if (arg == QStringLiteral("--keep-ps-glyph")) options.removeStockPsGlyph = false;
    else if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
      QTextStream(stdout) << "Usage: PSXBiosBrandingPatch --bios SCPH1001.BIN --output patched.BIN "
                             "[--initial-image image] [--handoff-image image] [--manifest proof.json] "
                             "[--keep-audio] [--keep-ps-glyph]\n";
      return 0;
    } else {
      QTextStream(stderr) << "Unknown argument: " << arg << "\n";
      return 2;
    }
  }
  if (options.inputBiosPath.isEmpty() || options.outputBiosPath.isEmpty()) {
    QTextStream(stderr) << "--bios and --output are required.\n";
    return 2;
  }
  QString error;
  if (!psxstudio::BiosBrandingPatcher::patch(options, error)) {
    QTextStream(stderr) << error << "\n";
    return 1;
  }
  QTextStream(stdout) << "Patched BIOS written to " << options.outputBiosPath << "\n";
  return 0;
}
