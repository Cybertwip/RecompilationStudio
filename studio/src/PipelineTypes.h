#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace psxstudio {

enum class TargetPlatform {
  MacOS,
  Windows,
};

inline QString targetPlatformKey(TargetPlatform platform) {
  return platform == TargetPlatform::Windows ? QStringLiteral("windows")
                                             : QStringLiteral("macos");
}

inline QString targetPlatformDisplayName(TargetPlatform platform) {
  return platform == TargetPlatform::Windows ? QStringLiteral("Windows")
                                             : QStringLiteral("macOS");
}

inline TargetPlatform targetPlatformFromKey(const QString& key) {
  return key.compare(QStringLiteral("windows"), Qt::CaseInsensitive) == 0
    ? TargetPlatform::Windows
    : TargetPlatform::MacOS;
}

struct PipelineRequest {
  TargetPlatform targetPlatform{ TargetPlatform::MacOS };
  QString cuePath;
  QStringList selectedBinPaths;
  QString biosPath;
  QString iconPath;
  QString windowTitle;
  QString outputDirectory;
  QString certificatePath;
  QString certificatePassword;
  QString ghidraHome;
  QString frameworkRoot;
  bool patchBiosBranding{ false };
  QString biosInitialSplashPath;
  QString biosHandoffImagePath;
  bool biosMuteBootAudio{ true };
  bool biosRemoveStockPsGlyph{ true };
  bool skipBiosBoot{ false };
  bool macosGipGamepad{ true };
  bool overwriteOutput{ false };

  QJsonObject toJson(bool includeSecret = false) const {
    QJsonArray bins;
    for (const auto& path : selectedBinPaths) {
      bins.append(path);
    }
    QJsonObject object{
      { QStringLiteral("platform"), targetPlatformKey(targetPlatform) },
      { QStringLiteral("cue_path"), cuePath },
      { QStringLiteral("bin_paths"), bins },
      { QStringLiteral("bios_path"), biosPath },
      { QStringLiteral("icon_path"), iconPath },
      { QStringLiteral("window_title"), windowTitle },
      { QStringLiteral("output_directory"), outputDirectory },
      { QStringLiteral("certificate_path"), certificatePath },
      { QStringLiteral("ghidra_home"), ghidraHome },
      { QStringLiteral("framework_root"), frameworkRoot },
      { QStringLiteral("patch_bios_branding"), patchBiosBranding },
      { QStringLiteral("bios_initial_splash_path"), biosInitialSplashPath },
      { QStringLiteral("bios_handoff_image_path"), biosHandoffImagePath },
      { QStringLiteral("bios_mute_boot_audio"), biosMuteBootAudio },
      { QStringLiteral("bios_remove_stock_ps_glyph"), biosRemoveStockPsGlyph },
      { QStringLiteral("skip_bios_boot"), skipBiosBoot },
      { QStringLiteral("macos_gip_gamepad"), macosGipGamepad },
      { QStringLiteral("overwrite_output"), overwriteOutput },
    };
    if (includeSecret) {
      object.insert(QStringLiteral("certificate_password"), certificatePassword);
    }
    return object;
  }
};

struct DiscFile {
  QString cueReference;
  QString sourcePath;
  QString packagedName;
};

struct DiscDescription {
  QString cuePath;
  QString rewrittenCue;
  QList<DiscFile> files;
};

struct GameDescription {
  QString bootPath;
  QString bootFileName;
  QString serial;
  QString volumeId;
  QByteArray executableBytes;
  QByteArray codeBytes;
  quint32 loadAddress{ 0 };
  quint32 entryPc{ 0 };
  quint32 textSize{ 0 };
  quint32 stackBase{ 0 };
};

} // namespace psxstudio

Q_DECLARE_METATYPE(psxstudio::PipelineRequest)
Q_DECLARE_METATYPE(psxstudio::TargetPlatform)
