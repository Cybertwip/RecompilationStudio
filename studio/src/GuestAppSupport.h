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
// container, take the guest image out of it, cross-compile the front-end for
// Cortex-A7, and stage that image beside the .virtua as `executable` — which is
// the path both front-ends read from argv[1].
//
// The distribution containers are opened here rather than refused. A PSN
// package (`.pkg`) and a Switch submission package (`.nsp`) are archives, and
// Studio reads them: see VitaPkg.h, VitaPfs.h and SwitchNsp.h for what each
// layer needs. Where a layer genuinely cannot be opened — a Vita title with no
// licence, an NSP with no key file — the refusal names that layer and what it
// wanted, having read everything above it, instead of dismissing the whole file
// as "encrypted".
//
// A Vita title's second layer, PFS, is keyed per title: its licence is the only
// thing that opens it, and Studio takes that licence from the title's own
// `sce_sys/package/work.bin`, from a `.rif` or zRIF beside the container, or
// from PSXRECOMP_VITA_LICENSE / PSXRECOMP_VITA_ZRIF — and checks it against the
// title before using it. So both shapes a Vita title arrives in are accepted:
// the `.pkg` it was distributed as, and the unpacked directory it was dumped
// to (`<TITLEID>/` with param.sfo, eboot.bin and sce_pfs/ inside).
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
  VitaPkg,   // PSN package: opened here; its PFS layer needs a licence
  VitaAppDir,  // an unpacked title directory — param.sfo, eboot.bin, sce_pfs/
  // Horizon.
  HorizonNro,
  HorizonNso,
  HorizonNsp,  // PFS0 archive of NCAs: opened here; the NCAs need a key file
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
  QString suggestedTitle;  // the container's own title where it carries one,
                           // otherwise the file name
  qint64 size{ 0 };        // of the selected file
  GuestArch arch{ GuestArch::Unknown };
  // Empty when the front-end can open this image. Otherwise the reason, naming
  // the format that was found and what the front-end does accept.
  QString refusal;
  // What the container holds — the package item list, the PFS0 entry list, the
  // VPK entry list. Present whether or not the image was loadable.
  QStringList contents;

  // Metadata the container states about itself: param.sfo for a Vita package,
  // the .cnmt.xml for an NSP. Empty for a bare module, which carries none.
  QString containerTitle;
  QString containerTitleId;
  // What was read and what was not — the key file that was used, the layer that
  // was open. Informational; a refusal is a refusal and lives above.
  QStringList notes;

  // Set when the guest image is inside the container and `stageGuestApp` has to
  // take it out: `executableName` is the entry it will extract.
  bool requiresExtraction{ false };
  QString executableName;

  bool loadable() const { return refusal.isEmpty(); }
};

struct GuestCatalogEntry {
  QString sourcePath;
  QString suggestedTitle;
  QString containerName;
};

QString guestArchName(GuestArch arch);

/* Identifies the container and, for a distribution package, opens it: the item
 * table, the metadata, and enough of the guest image to say whether the
 * front-end can load it. Nothing is written and nothing is relocated.
 *
 * `error` is set when the file cannot be read at all; a file that reads fine
 * but holds an image the front-end will not open returns true with
 * `description.refusal` populated. */
bool inspectGuestApp(SystemKind system,
                     const QString& path,
                     GuestAppDescription& description,
                     QString& error);

/* Produces the image the front-end loads, at `<destinationDirectory>/executable`.
 * For a bare module that is a copy; for a package it is the extraction, and the
 * files the package carried alongside the module are written to
 * `<destinationDirectory>/container/` so the export keeps what it read.
 *
 * `log` receives one line per file written. Only valid for a description whose
 * `loadable()` is true. */
bool stageGuestApp(const GuestAppDescription& description,
                   const QString& sourcePath,
                   const QString& destinationDirectory,
                   QString& stagedExecutable,
                   QStringList& log,
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
