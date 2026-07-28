#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace psxstudio {

enum class SystemKind {
  PlayStation,
  GameBoyAdvance,
};

inline QString systemKindKey(SystemKind system) {
  return system == SystemKind::GameBoyAdvance ? QStringLiteral("gba")
                                               : QStringLiteral("playstation");
}

inline QString systemKindDisplayName(SystemKind system) {
  return system == SystemKind::GameBoyAdvance
    ? QStringLiteral("Game Boy Advance") : QStringLiteral("PlayStation");
}

inline SystemKind systemKindFromKey(const QString& key) {
  return key.compare(QStringLiteral("gba"), Qt::CaseInsensitive) == 0 ||
         key.compare(QStringLiteral("game-boy-advance"), Qt::CaseInsensitive) == 0
    ? SystemKind::GameBoyAdvance : SystemKind::PlayStation;
}

enum class TargetPlatform {
  All,
  MacOS,
  Windows,
  Linux,
  VirtuaArm,
};

enum class ExportMode {
  Source,
  Build,
};

enum class BuildBackend {
  Local,
  RemoteCi,
};


inline QString exportModeKey(ExportMode mode) {
  return mode == ExportMode::Source ? QStringLiteral("source")
                                    : QStringLiteral("build");
}

inline QString exportModeDisplayName(ExportMode mode) {
  return mode == ExportMode::Source ? QStringLiteral("Source")
                                    : QStringLiteral("Build");
}

inline ExportMode exportModeFromKey(const QString& key) {
  return key.compare(QStringLiteral("source"), Qt::CaseInsensitive) == 0
    ? ExportMode::Source : ExportMode::Build;
}

inline QString buildBackendKey(BuildBackend backend) {
  return backend == BuildBackend::RemoteCi ? QStringLiteral("ci")
                                            : QStringLiteral("local");
}

inline TargetPlatform hostTargetPlatform() {
#if defined(Q_OS_WIN)
  return TargetPlatform::Windows;
#elif defined(Q_OS_LINUX)
  return TargetPlatform::Linux;
#else
  return TargetPlatform::MacOS;
#endif
}

inline bool hostCanSelectTargetPlatform() {
#if defined(Q_OS_MACOS)
  return true;
#else
  return false;
#endif
}

inline bool targetPlatformSupportedOnHost(TargetPlatform platform) {
  if (platform == TargetPlatform::All) {
    return hostCanSelectTargetPlatform();
  }
  return hostCanSelectTargetPlatform() || platform == hostTargetPlatform();
}

inline QList<TargetPlatform> concreteTargetPlatforms(TargetPlatform platform) {
  if (platform == TargetPlatform::All) {
    return { TargetPlatform::MacOS, TargetPlatform::Windows, TargetPlatform::Linux };
  }
  return { platform };
}

inline QString targetPlatformKey(TargetPlatform platform) {
  switch (platform) {
    case TargetPlatform::All:
      return QStringLiteral("all");
    case TargetPlatform::Windows:
      return QStringLiteral("windows");
    case TargetPlatform::Linux:
      return QStringLiteral("linux");
    case TargetPlatform::MacOS:
      return QStringLiteral("macos");
    case TargetPlatform::VirtuaArm:
      return QStringLiteral("virtua-arm");
  }
  return QStringLiteral("macos");
}

inline QString targetPlatformDisplayName(TargetPlatform platform) {
  switch (platform) {
    case TargetPlatform::All:
      return QStringLiteral("All");
    case TargetPlatform::Windows:
      return QStringLiteral("Windows");
    case TargetPlatform::Linux:
      return QStringLiteral("Linux");
    case TargetPlatform::MacOS:
      return QStringLiteral("macOS");
    case TargetPlatform::VirtuaArm:
      return QStringLiteral("Virtua ARM");
  }
  return QStringLiteral("macOS");
}

inline TargetPlatform targetPlatformFromKey(const QString& key) {
  if (key.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0) {
    return TargetPlatform::All;
  }
  if (key.compare(QStringLiteral("windows"), Qt::CaseInsensitive) == 0) {
    return TargetPlatform::Windows;
  }
  if (key.compare(QStringLiteral("linux"), Qt::CaseInsensitive) == 0) {
    return TargetPlatform::Linux;
  }
  if (key.compare(QStringLiteral("virtua-arm"), Qt::CaseInsensitive) == 0 ||
      key.compare(QStringLiteral("virtua"), Qt::CaseInsensitive) == 0) {
    return TargetPlatform::VirtuaArm;
  }
  return TargetPlatform::MacOS;
}

struct PipelineRequest {
  SystemKind system{ SystemKind::PlayStation };
  TargetPlatform targetPlatform{ hostTargetPlatform() };
  ExportMode exportMode{ ExportMode::Build };
  BuildBackend buildBackend{ BuildBackend::Local };
  QString cuePath;
  QString romPath;
  QStringList selectedBinPaths;
  QString biosPath;
  QString iconPath;
  QString windowTitle;
  QString outputDirectory;
  QString certificatePath;
  QString certificatePassword;
  QString ghidraHome;
  // Root of the LLVM/compiler bundle used for Virtua ARM. The toolchain also
  // resolves the matching PowerEngine ARM llvm-libc/libc++ and compiler-rt
  // outputs from this build tree; PSXRecomp does not vendor those artifacts.
  QString llvmRoot;
  // PowerEngine source checkout providing External/Virtua (Dash, packager,
  // ABI headers) and OS/MVII/Kernel/Shared/posix-shim.
  QString powerEngineRoot;
  QString frameworkRoot;
  bool patchBiosBranding{ false };
  QString biosInitialSplashPath;
  QString biosHandoffImagePath;
  bool biosMuteBootAudio{ true };
  bool biosRemoveStockPsGlyph{ true };
  bool skipBiosBoot{ false };
  bool macosGipGamepad{ true };
  bool nativeExecution{ false };
  bool exportAsZip{ true };
  bool useCi{ false };
  QString ciBuilderId;
  QString ciBuilderName;
  QString ciBuilderEndpoint;
  QString ciArchitecture;
  bool overwriteOutput{ false };

  QJsonObject toJson(bool includeSecret = false) const {
    QJsonArray bins;
    for (const auto& path : selectedBinPaths) {
      bins.append(path);
    }
    QJsonObject object{
      { QStringLiteral("system"), systemKindKey(system) },
      { QStringLiteral("platform"), targetPlatformKey(targetPlatform) },
      { QStringLiteral("export_mode"), exportModeKey(exportMode) },
      { QStringLiteral("build_backend"), buildBackendKey(buildBackend) },
      { QStringLiteral("cue_path"), cuePath },
      { QStringLiteral("rom_path"), romPath },
      { QStringLiteral("bin_paths"), bins },
      { QStringLiteral("bios_path"), biosPath },
      { QStringLiteral("icon_path"), iconPath },
      { QStringLiteral("window_title"), windowTitle },
      { QStringLiteral("output_directory"), outputDirectory },
      { QStringLiteral("certificate_path"), certificatePath },
      { QStringLiteral("ghidra_home"), ghidraHome },
      { QStringLiteral("llvm_root"), llvmRoot },
      { QStringLiteral("powerengine_root"), powerEngineRoot },
      { QStringLiteral("framework_root"), frameworkRoot },
      { QStringLiteral("patch_bios_branding"), patchBiosBranding },
      { QStringLiteral("bios_initial_splash_path"), biosInitialSplashPath },
      { QStringLiteral("bios_handoff_image_path"), biosHandoffImagePath },
      { QStringLiteral("bios_mute_boot_audio"), biosMuteBootAudio },
      { QStringLiteral("bios_remove_stock_ps_glyph"), biosRemoveStockPsGlyph },
      { QStringLiteral("skip_bios_boot"), skipBiosBoot },
      { QStringLiteral("macos_gip_gamepad"), macosGipGamepad },
      { QStringLiteral("native"), nativeExecution },
      { QStringLiteral("export_as_zip"), exportAsZip },
      { QStringLiteral("use_ci"), useCi },
      { QStringLiteral("ci_builder_id"), ciBuilderId },
      { QStringLiteral("ci_builder_name"), ciBuilderName },
      { QStringLiteral("ci_architecture"), ciArchitecture },
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
Q_DECLARE_METATYPE(psxstudio::SystemKind)
Q_DECLARE_METATYPE(psxstudio::TargetPlatform)
Q_DECLARE_METATYPE(psxstudio::ExportMode)
Q_DECLARE_METATYPE(psxstudio::BuildBackend)
