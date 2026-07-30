#pragma once

#include "PipelineTypes.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace psxstudio {

// Studio's half of the two direct-execution front-ends, extra/vita2hos
// (vita2mvii) and extra/horizon2mvii.
//
// Neither of them recompiles anything: the Vita is ARMv7-A Cortex-A9, 32-bit
// Horizon is ARMv7-A, and the J36 is an ARMv7-A Cortex-A7, so the guest's own
// instructions are the device's instructions. What each front-end supplies is
// the guest's operating system, in the shape WINE supplies Win32 on Linux.
//
// So a Studio export here is not a recompilation pipeline. It is: identify the
// container, refuse it by name if the front-end cannot open it, cross-compile
// the front-end for Cortex-A7, and stage the guest image beside the .virtua as
// `executable` — which is the path both front-ends read from argv[1].
//
// The identification is done here, at export time, rather than left to the
// device. Both front-ends already refuse a container they cannot open, but they
// do it after the package has been built, copied to the J36 and launched. The
// same refusal issued while the file is being chosen costs nothing and names
// the format instead of producing a package that cannot run.

enum class GuestContainer {
  Unknown,
  // Vita.
  VitaElf,   // ET_SCE_RELEXEC ARM ELF32 — what a .velf/eboot.elf is
  VitaSelf,  // SCE\0 wrapper, unencrypted, version 3 header type 1
  VitaVpk,   // ZIP carrying eboot.bin
  VitaPkg,   // PSN package: encrypted, needs a zRIF/work.bin licence
  // Horizon.
  HorizonNro,
  HorizonNso,
  HorizonNsp,  // PFS0 archive of NCAs: encrypted
  HorizonXci,  // cartridge dump: encrypted
};

enum class GuestArch {
  Unknown,
  Arm32,
  AArch64,
};

struct GuestAppDescription {
  SystemKind system{ SystemKind::Vita };
  GuestContainer container{ GuestContainer::Unknown };
  QString containerName;   // "SELF", "VPK", "NRO0", "PFS0 (NSP)", …
  QString suggestedTitle;  // from the file name; these containers carry no
                           // reliable display title outside their metadata
  qint64 size{ 0 };
  GuestArch arch{ GuestArch::Unknown };
  // Empty when the front-end can open this image. Otherwise the reason, naming
  // the format that was found and what the front-end does accept.
  QString refusal;
  // Evidence for the refusal where the container could be walked without
  // decrypting it — the PFS0 entry list, the VPK entry list.
  QStringList contents;

  bool loadable() const { return refusal.isEmpty(); }
};

struct GuestCatalogEntry {
  QString sourcePath;
  QString suggestedTitle;
  QString containerName;
};

QString guestArchName(GuestArch arch);

/* Reads only the container headers: nothing here decrypts, and nothing here
 * loads or relocates. `error` is set when the file cannot be read at all;
 * a file that reads fine but holds a container the front-end will not open
 * returns true with `description.refusal` populated. */
bool inspectGuestApp(SystemKind system,
                     const QString& path,
                     GuestAppDescription& description,
                     QString& error);

/* The file-dialog filter for the containers `system`'s front-end accepts. */
QString guestAppFileFilter(SystemKind system);

/* Recursive scan for batch exports. Images the front-end refuses are reported
 * in `warnings` and left out of `entries` rather than queued to fail later. */
bool scanGuestAppDirectory(SystemKind system,
                           const QString& directory,
                           QList<GuestCatalogEntry>& entries,
                           QStringList& warnings,
                           QString& error);

/* The generated package project. It adds the front-end as a subdirectory and
 * hands it the guest image, so the front-end's own CMake — including its
 * ARMv7-only guard — stays the single definition of how it is built. */
QString generatedGuestAppProjectCMake(const PipelineRequest& request,
                                      const GuestAppDescription& app,
                                      const QString& bundleName);

/* README for the source export and the delivered package. */
QString generatedGuestAppReadme(const PipelineRequest& request,
                                const GuestAppDescription& app,
                                const QString& bundleName);

} // namespace psxstudio
