#include "GuestAppSupport.h"

#include "PipelineSupport.h"
#include "SwitchNsp.h"
#include "VitaPkg.h"

#include "quazip.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace psxstudio {

namespace {

// Vita.
constexpr quint16 kElfTypeSceRelexec = 0xFE04u;  // ET_SCE_RELEXEC, sce-elf.h
constexpr quint16 kElfMachineArm = 40u;          // EM_ARM
// Horizon.
constexpr quint32 kNsoMagic = 0x304F534Eu;  // 'NSO0' little-endian
constexpr quint32 kNroMagic = 0x304F524Eu;  // 'NRO0'
constexpr quint32 kPfs0Magic = 0x30534650u; // 'PFS0'

quint16 readLe16(const QByteArray& bytes, qsizetype offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint16>(p[0]) | static_cast<quint16>(p[1] << 8u);
}

quint32 readLe32(const QByteArray& bytes, qsizetype offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint32>(p[0]) |
         (static_cast<quint32>(p[1]) << 8u) |
         (static_cast<quint32>(p[2]) << 16u) |
         (static_cast<quint32>(p[3]) << 24u);
}

/* horizon_image.c's arch_from_entry_branch, applied to the same first word.
 * AArch64 is tested first for the reason stated there: the ARM32 test is a byte
 * compare an AArch64 branch could in principle satisfy. */
GuestArch archFromEntryBranch(quint32 word) {
  if ((word >> 26u) == 0x05u) return GuestArch::AArch64;  // AArch64 B
  if ((word >> 24u) == 0xEAu) return GuestArch::Arm32;    // ARM32 B, cond=AL
  return GuestArch::Unknown;
}

/* The first four decompressed bytes of an LZ4 block, when they can be had
 * without decoding it.
 *
 * horizon2mvii decodes NSO segments properly; duplicating its decoder here to
 * read one word would be two implementations of the same format, and the second
 * one is the one nothing tests. But an LZ4 block always opens with a literal
 * run — a block cannot begin with a match, there is nothing behind the cursor to
 * match against — so when that run is at least four bytes long, the first four
 * decompressed bytes are just the first four literals.
 *
 * When the run is shorter, this reports failure rather than guessing, and the
 * caller reports GuestArch::Unknown. Studio does not need to be sure here: the
 * front-end's own two-signal probe is what decides on the device. */
bool lz4FirstWord(const QByteArray& block, quint32& word) {
  if (block.isEmpty()) return false;
  const auto* bytes = reinterpret_cast<const uchar*>(block.constData());
  qsizetype cursor = 0;
  quint64 literals = static_cast<quint64>(bytes[cursor++] >> 4u);
  if (literals == 15u) {
    for (;;) {
      if (cursor >= block.size()) return false;
      const uchar add = bytes[cursor++];
      literals += add;
      if (add != 255u) break;
      if (literals > static_cast<quint64>(block.size())) return false;
    }
  }
  if (literals < 4u || cursor + 4 > block.size()) return false;
  word = readLe32(block, cursor);
  return true;
}

QString containerDisplayName(GuestContainer container) {
  switch (container) {
    case GuestContainer::VitaElf:     return QStringLiteral("ARM ELF32 (ET_SCE_RELEXEC)");
    case GuestContainer::VitaSelf:    return QStringLiteral("SELF");
    case GuestContainer::VitaVpk:     return QStringLiteral("VPK");
    case GuestContainer::VitaPkg:     return QStringLiteral("PKG");
    case GuestContainer::HorizonNro:  return QStringLiteral("NRO0");
    case GuestContainer::HorizonNso:  return QStringLiteral("NSO0");
    case GuestContainer::HorizonNsp:  return QStringLiteral("PFS0 (NSP)");
    case GuestContainer::HorizonXci:  return QStringLiteral("XCI");
    case GuestContainer::Unknown:     break;
  }
  return QStringLiteral("unrecognized");
}

/* The two module forms vita2mvii loads, tested on the module's own head —
 * whether that module was selected directly or read out of a package. Returns
 * false when the head is neither, leaving `description` untouched. */
bool classifyVitaModule(const QByteArray& head, GuestAppDescription& description) {
  // Order follows load_executable(): ELF magic, then SCE magic. A file matching
  // neither is what that function's final `return -1` refuses.
  if (head.size() >= 20 && head.startsWith(QByteArrayLiteral("\x7f" "ELF"))) {
    const quint16 type = readLe16(head, 16);
    const quint16 machine = readLe16(head, 18);
    description.container = GuestContainer::VitaElf;
    description.arch = GuestArch::Arm32;
    if (static_cast<uchar>(head.at(4)) != 1u) {  // ELFCLASS32
      description.arch = GuestArch::AArch64;
      description.refusal = QStringLiteral(
        "This is a 64-bit ELF. The Vita is a 32-bit ARMv7-A Cortex-A9 and so is "
        "the MVII device; there is no 64-bit Vita executable to run.");
      return true;
    }
    if (machine != kElfMachineArm) {
      description.arch = GuestArch::Unknown;
      description.refusal = QStringLiteral(
        "This ELF is not built for ARM (e_machine %1). vita2mvii branches into "
        "the guest's own instructions, so they have to be ARM instructions.")
          .arg(machine);
      return true;
    }
    if (type != kElfTypeSceRelexec) {
      description.refusal = QStringLiteral(
        "This is a plain ARM ELF (e_type 0x%1), not a Vita module. vita2mvii "
        "loads ET_SCE_RELEXEC (0xFE04) — a .velf or the eboot.elf a Vita "
        "toolchain emits — because that is the form that carries the SCE "
        "relocations and the import table it binds.")
          .arg(type, 4, 16, QLatin1Char('0'));
    }
    return true;
  }

  if (head.size() >= 4 && head.startsWith(QByteArrayLiteral("SCE\0"))) {
    description.container = GuestContainer::VitaSelf;
    description.arch = GuestArch::Arm32;
    if (head.size() < 0x20) {
      description.refusal = QStringLiteral("The SELF header is truncated.");
      return true;
    }
    const quint32 version = readLe32(head, 0x04);
    const quint16 headerType = readLe16(head, 0x0A);
    if (version != 3u) {
      description.refusal = QStringLiteral(
        "SELF header version %1 is not supported; vita2mvii loads version 3.")
          .arg(version);
      return true;
    }
    if (headerType != 1u) {
      // header_type 3 is the SELF-inside-a-PKG form.
      description.refusal = QStringLiteral(
        "SELF header type %1 is not a plain self (type 1). vita2mvii does not "
        "decrypt, so it loads only an unencrypted SELF.")
          .arg(headerType);
    }
    return true;
  }

  return false;
}

/* Opens the PSN package: the item table, param.sfo, and the package's own
 * record of what it keeps inside the PFS layer. The executable is then either
 * reachable — in which case its head is classified exactly as a directly
 * selected module's would be — or it is not, and the refusal says which layer
 * held it and what that layer's key comes from. */
void inspectVitaPackage(const QString& path, GuestAppDescription& description) {
  VitaPkgInfo package;
  QString packageError;
  if (!readVitaPkg(path, package, packageError)) {
    description.refusal = packageError;
    return;
  }

  for (const VitaPkgItem& item : package.items) {
    if (item.isDirectory()) {
      description.contents.append(QStringLiteral("%1/").arg(item.name));
      continue;
    }
    description.contents.append(
      QStringLiteral("%1 (%2 bytes)%3")
        .arg(item.name)
        .arg(item.size)
        .arg(package.isPfsProtected(item.name) ? QStringLiteral(" [PFS]") : QString()));
  }

  description.containerTitle = package.title;
  description.containerTitleId = package.titleId;
  description.notes.append(
    QStringLiteral("PKG opened with package key %1: %2 items, %3 bytes of data.")
      .arg(package.keyType).arg(package.items.size()).arg(package.dataSize));
  if (!package.contentId.isEmpty())
    description.notes.append(QStringLiteral("Content ID: %1").arg(package.contentId));
  if (!package.title.isEmpty() || !package.titleId.isEmpty()) {
    description.notes.append(
      QStringLiteral("param.sfo: %1 (%2)%3%4")
        .arg(package.title.isEmpty() ? QStringLiteral("untitled") : package.title)
        .arg(package.titleId.isEmpty() ? QStringLiteral("no title id") : package.titleId)
        .arg(package.category.isEmpty() ? QString()
                                        : QStringLiteral(", category %1").arg(package.category))
        .arg(package.appVersion.isEmpty() ? QString()
                                          : QStringLiteral(", version %1").arg(package.appVersion)));
  }
  description.notes.append(
    package.havePfsList
      ? QStringLiteral("sce_pfs/pflist lists %1 file(s) stored inside the PFS layer.")
          .arg(package.pfsProtectedFiles.size())
      : QStringLiteral("No sce_pfs/pflist: this package has no PFS layer."));

  if (package.executableName.isEmpty()) {
    description.refusal = QStringLiteral(
      "This package carries no eboot.bin, so there is no module for vita2mvii "
      "to load. Its content type is 0x%1%2 — a package that patches or adds to "
      "an application rather than being one.")
        .arg(package.contentType, 2, 16, QLatin1Char('0'))
        .arg(package.category.isEmpty()
               ? QString()
               : QStringLiteral(" and its param.sfo category is `%1`").arg(package.category));
    return;
  }

  if (package.executableIsPfsProtected) {
    description.refusal = QStringLiteral(
      "The package itself opened — %1 items, decrypted with the package key that "
      "is fixed in the format — but its %2 is inside the second layer, PFS. The "
      "package says so itself: sce_pfs/pflist lists that file without the `nenc` "
      "mark, which is how a package records what it stores PFS-encrypted. The PFS "
      "key comes from the licence (the zRIF / work.bin) issued to a console, and "
      "this package does not contain one — sce_sys/package holds a placeholder. "
      "No tool can read that module without the licence, so supply a VPK, a .velf "
      "or an unencrypted eboot.bin.")
        .arg(package.items.size()).arg(package.executableName);
    return;
  }

  // Reachable: read the module's head out of the package and classify it the
  // same way a directly selected one is.
  VitaPkgItem executable;
  for (const VitaPkgItem& item : package.items) {
    if (item.name == package.executableName) { executable = item; break; }
  }
  QByteArray moduleHead;
  QString itemError;
  if (!readVitaPkgItem(path, package, executable, 0x1000, moduleHead, itemError)) {
    description.refusal = QStringLiteral("The package's %1 could not be read: %2")
                            .arg(package.executableName, itemError);
    return;
  }

  GuestAppDescription probe;
  probe.system = SystemKind::Vita;
  if (!classifyVitaModule(moduleHead, probe)) {
    description.refusal = QStringLiteral(
      "The package's %1 is neither an ARM ELF nor a SELF (it begins %2). "
      "vita2mvii opens those two forms.")
        .arg(package.executableName,
             QString::fromLatin1(moduleHead.left(4).toHex(' ')));
    return;
  }
  description.arch = probe.arch;
  if (!probe.refusal.isEmpty()) {
    description.refusal = QStringLiteral("The package's %1: %2")
                            .arg(package.executableName, probe.refusal);
    return;
  }
  description.notes.append(QStringLiteral("%1 is a %2 and is not PFS-protected.")
                             .arg(package.executableName,
                                  containerDisplayName(probe.container)));
  description.requiresExtraction = true;
  description.executableName = package.executableName;
}

void inspectVita(const QByteArray& head, const QString& path,
                 GuestAppDescription& description) {
  if (classifyVitaModule(head, description)) return;

  if (head.size() >= 4 && head.startsWith(QByteArrayLiteral("\x7f" "PKG"))) {
    description.container = GuestContainer::VitaPkg;
    inspectVitaPackage(path, description);
    return;
  }

  if (head.size() >= 4 && head.startsWith(QByteArrayLiteral("PK\x03\x04"))) {
    description.container = GuestContainer::VitaVpk;
    description.arch = GuestArch::Arm32;
    QuaZip archive(path);
    if (!archive.open(QuaZip::mdUnzip)) {
      description.refusal = QStringLiteral(
        "This is a ZIP, but it could not be opened to confirm it is a VPK.");
      return;
    }
    bool hasEboot = false;
    for (bool more = archive.goToFirstFile(); more; more = archive.goToNextFile()) {
      const QString name = archive.getCurrentFileName();
      description.contents.append(name);
      if (name.compare(QStringLiteral("eboot.bin"), Qt::CaseSensitive) == 0) hasEboot = true;
    }
    archive.close();
    if (!hasEboot) {
      // load_vpk extracts by exact name, so a nested or differently-cased entry
      // is a miss there too.
      description.refusal = QStringLiteral(
        "This ZIP has no eboot.bin at its root. vita2mvii extracts that exact "
        "entry from a VPK and loads it as the module.");
    }
    return;
  }

  description.refusal = QStringLiteral(
    "This file is not an ARM ELF, a SELF or a VPK, which are the three "
    "containers vita2mvii opens.");
}

/* The architecture of an NSO0 module, read from the first instruction of its
 * .text — which is segment 0, and whose bytes are LZ4 blocks when flags bit 0
 * is set. Reports Unknown rather than guessing when the head is too short or
 * the LZ4 literal run is under a word long. */
GuestArch probeNsoArch(const QByteArray& module) {
  if (module.size() < 0x100) return GuestArch::Unknown;
  const quint32 flags = readLe32(module, 0x0C);
  const quint32 textOffset = readLe32(module, 0x10);
  const quint32 textCompressed = readLe32(module, 0x60);
  quint32 entryWord = 0;
  if ((flags & 1u) == 0u) {
    if (textOffset + 4u > static_cast<quint32>(module.size())) return GuestArch::Unknown;
    return archFromEntryBranch(readLe32(module, textOffset));
  }
  if (textOffset >= static_cast<quint32>(module.size())) return GuestArch::Unknown;
  const qsizetype available =
    std::min<qsizetype>(module.size() - textOffset, textCompressed);
  if (!lz4FirstWord(module.mid(textOffset, available), entryWord)) return GuestArch::Unknown;
  return archFromEntryBranch(entryWord);
}

/* Opens the submission package. The PFS0 table, the ticket and the `.cnmt.xml`
 * are read with no key at all; the NCA and its ExeFS need `prod.keys`, and when
 * that file is absent the refusal names the keys it wanted and the paths it
 * looked in rather than calling the whole package encrypted. */
void inspectHorizonPackage(const QString& path, GuestAppDescription& description) {
  SwitchNspInfo package;
  QString packageError;
  if (!readSwitchNsp(path, package, packageError)) {
    description.refusal = packageError;
    return;
  }

  for (const Pfs0Entry& entry : package.entries) {
    description.contents.append(
      QStringLiteral("%1 (%2 bytes)").arg(entry.name).arg(entry.size));
  }
  description.containerTitleId = package.titleId;
  if (!package.contentMetaType.isEmpty()) {
    description.notes.append(
      QStringLiteral("NSP: %1 entries, content meta type %2%3.")
        .arg(package.entries.size())
        .arg(package.contentMetaType)
        .arg(package.patchId.isEmpty() ? QString()
                                       : QStringLiteral(", patch id %1").arg(package.patchId)));
  } else {
    description.notes.append(
      QStringLiteral("NSP: %1 entries; the package ships no .cnmt.xml.")
        .arg(package.entries.size()));
  }
  for (const SwitchNspContent& content : package.contents) {
    description.notes.append(QStringLiteral("  %1 NCA %2 (%3 bytes)")
                               .arg(content.type, content.id).arg(content.size));
  }
  if (!package.rightsId.isEmpty()) {
    description.notes.append(
      QStringLiteral("%1: rights id %2, title key present (encrypted).")
        .arg(package.ticketName,
             QString::fromLatin1(package.rightsId.toHex())));
  }

  if (!package.programDecrypted) {
    const QString wanted =
      package.missingKeys.isEmpty()
        ? QStringLiteral("the console keys")
        : QStringLiteral("%1").arg(package.missingKeys.join(QStringLiteral(", ")));
    description.refusal = QStringLiteral(
      "The package opened — %1 entries, %2 — but its NCAs are encrypted under "
      "Nintendo's console keys, and this needs %3. Those keys are not in the "
      "package and cannot be derived from it; they are dumped from a console "
      "into a prod.keys file. Studio looked for one in: %4. Set "
      "PSXRECOMP_SWITCH_KEYS to a prod.keys, put one beside this package, or "
      "supply the NRO/NSO directly.")
        .arg(package.entries.size())
        .arg(package.titleId.isEmpty()
               ? QStringLiteral("no title id in the package")
               : QStringLiteral("title id %1").arg(package.titleId))
        .arg(wanted)
        .arg(package.keySource.isEmpty()
               ? package.keySearchPaths.join(QStringLiteral(", "))
               : package.keySource);
    return;
  }

  description.notes.append(
    QStringLiteral("NCA opened with the keys at %1: %2, master key revision %3.")
      .arg(package.keySource, package.ncaFormat).arg(package.masterKeyRevision));
  QStringList exeFs;
  for (const Pfs0Entry& entry : package.exeFsEntries)
    exeFs.append(QStringLiteral("%1 (%2)").arg(entry.name).arg(entry.size));
  description.notes.append(QStringLiteral("ExeFS: %1").arg(exeFs.join(QStringLiteral(", "))));

  if (package.mainModule.isEmpty()) {
    description.refusal = QStringLiteral(
      "The program NCA's ExeFS opened but carries no `main`, which is the NSO0 "
      "module horizon2mvii loads. It holds: %1.").arg(exeFs.join(QStringLiteral(", ")));
    return;
  }
  description.arch = probeNsoArch(package.mainModuleHead);
  description.requiresExtraction = true;
  description.executableName = package.mainModule;
}

void inspectHorizon(const QByteArray& head, const QString& path,
                    GuestAppDescription& description) {
  if (head.size() >= 4 && readLe32(head, 0) == kNsoMagic) {
    description.container = GuestContainer::HorizonNso;
    if (head.size() < 0x100) {
      description.refusal = QStringLiteral("The NSO0 header is truncated.");
      return;
    }
    description.arch = probeNsoArch(head);
  } else if (head.size() >= 0x14 && readLe32(head, 0x10) == kNroMagic) {
    description.container = GuestContainer::HorizonNro;
    description.arch = archFromEntryBranch(readLe32(head, 0x00));
  } else if (head.size() >= 4 && readLe32(head, 0) == kPfs0Magic) {
    description.container = GuestContainer::HorizonNsp;
    inspectHorizonPackage(path, description);
    // A package-level refusal stands as it is; only a package that produced a
    // module falls through to the architecture check below.
    if (!description.refusal.isEmpty()) return;
  } else if (head.size() >= 0x104 &&
             head.mid(0x100, 4) == QByteArrayLiteral("HEAD")) {
    description.container = GuestContainer::HorizonXci;
    description.refusal = QStringLiteral(
      "This is an XCI cartridge dump. Like an NSP it carries encrypted NCAs, "
      "so horizon2mvii has nothing to load. Supply the NRO or the NSO.");
    return;
  } else {
    description.refusal = QStringLiteral(
      "This file is neither an NRO0 nor an NSO0, which are the two module "
      "containers horizon2mvii decodes.");
    return;
  }

  // Both module containers land here, and the architecture is what decides.
  if (description.arch == GuestArch::AArch64) {
    description.refusal = QStringLiteral(
      "This module is AArch64. The MVII device is an ARMv7-A Cortex-A7, which "
      "cannot decode a single AArch64 instruction — so this is a different "
      "project rather than a harder case. horizon2mvii runs 32-bit (AArch32) "
      "Horizon modules.");
  } else if (description.arch == GuestArch::Unknown) {
    // Not a refusal. Studio applies one of the front-end's two signals; the
    // other is .dynamic, which is only reachable after the module is decoded.
    // horizon2mvii asks both and refuses if they disagree.
    description.refusal.clear();
  }
}

}  // namespace

QString guestArchName(GuestArch arch) {
  switch (arch) {
    case GuestArch::Arm32:   return QStringLiteral("ARM32 (AArch32)");
    case GuestArch::AArch64: return QStringLiteral("AArch64");
    case GuestArch::Unknown: break;
  }
  return QStringLiteral("indeterminate");
}

bool inspectGuestApp(SystemKind system,
                     const QString& path,
                     GuestAppDescription& description,
                     QString& error) {
  description = {};
  description.system = system;
  if (!systemRunsGuestNatively(system)) {
    error = QStringLiteral("%1 is not a direct-execution system.")
              .arg(systemKindDisplayName(system));
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the %1 application: %2")
              .arg(systemKindDisplayName(system), file.errorString());
    return false;
  }
  description.size = file.size();
  if (description.size < 0x40) {
    error = QStringLiteral("The selected file is too small to hold a %1 container header.")
              .arg(systemKindDisplayName(system));
    return false;
  }
  // Only the head: identification reads headers, and an NSP is hundreds of
  // megabytes of ciphertext there is no reason to pull into memory. 1 MiB
  // covers every header here plus a PFS0 string table.
  const QByteArray head = file.read(1024 * 1024);
  file.close();
  if (head.isEmpty()) {
    error = QStringLiteral("Could not read the %1 application header.")
              .arg(systemKindDisplayName(system));
    return false;
  }

  if (system == SystemKind::Vita) inspectVita(head, path, description);
  else inspectHorizon(head, path, description);

  description.containerName = containerDisplayName(description.container);
  // A package states its own title; a bare module does not, and the file name
  // is the only name there is.
  description.suggestedTitle = description.containerTitle.isEmpty()
                                 ? QFileInfo(path).completeBaseName()
                                 : description.containerTitle;
  return true;
}

bool stageGuestApp(const GuestAppDescription& description,
                   const QString& sourcePath,
                   const QString& destinationDirectory,
                   QString& stagedExecutable,
                   QStringList& log,
                   QString& error) {
  log.clear();
  stagedExecutable.clear();
  if (!description.loadable()) {
    error = QStringLiteral("This image was refused during identification: %1")
              .arg(description.refusal);
    return false;
  }
  if (!QDir().mkpath(destinationDirectory)) {
    error = QStringLiteral("Could not create %1.").arg(destinationDirectory);
    return false;
  }
  const QString executable = QDir(destinationDirectory).filePath(QStringLiteral("executable"));

  // A bare module — and a VPK, which the front-end opens itself — is staged as
  // the file that was selected. Nothing is rewritten: the guest's bytes are the
  // device's instructions and the loader is the thing that reads them.
  if (!description.requiresExtraction) {
    if (!copyFileReplacing(sourcePath, executable, error)) return false;
    log.append(QStringLiteral("executable <- %1 (%2, verbatim)")
                 .arg(QFileInfo(sourcePath).fileName(), description.containerName));
    stagedExecutable = executable;
    return true;
  }

  // A package: the module comes out of it, and what the package carried
  // alongside is kept next to the export so the extraction can be checked
  // against its source rather than taken on trust.
  const QString container = QDir(destinationDirectory).filePath(QStringLiteral("container"));
  if (!QDir().mkpath(container)) {
    error = QStringLiteral("Could not create %1.").arg(container);
    return false;
  }

  if (description.container == GuestContainer::VitaPkg) {
    VitaPkgInfo package;
    if (!readVitaPkg(sourcePath, package, error)) return false;
    if (package.executableName != description.executableName) {
      error = QStringLiteral(
        "The package now names its executable `%1`, but it was identified as "
        "`%2`. Re-select the file.")
          .arg(package.executableName, description.executableName);
      return false;
    }
    QStringList extracted;
    const QString wantedModule = description.executableName;
    if (!extractVitaPkg(sourcePath, package, container,
                        [&wantedModule](const VitaPkgItem& item) {
                          if (item.isDirectory()) return false;
                          if (item.name == wantedModule) return true;
                          // Everything else the package carries, up to a limit:
                          // the metadata is what makes the extraction checkable
                          // against its source, while data.vfs is gigabytes of
                          // asset archive the front-end never reads.
                          return item.size <= 4u * 1024u * 1024u;
                        },
                        extracted, error)) {
      return false;
    }
    for (const QString& relative : extracted)
      log.append(QStringLiteral("container/%1").arg(relative));

    const QString module = QDir(container).filePath(description.executableName);
    if (!QFile::exists(module)) {
      error = QStringLiteral("%1 was not written by the package extraction.")
                .arg(description.executableName);
      return false;
    }
    if (!copyFileReplacing(module, executable, error)) return false;
    log.append(QStringLiteral("executable <- %1 (out of the PKG)")
                 .arg(description.executableName));
    stagedExecutable = executable;
    return true;
  }

  if (description.container == GuestContainer::HorizonNsp) {
    SwitchNspInfo package;
    if (!readSwitchNsp(sourcePath, package, error)) return false;
    if (!package.programDecrypted || package.mainModule.isEmpty()) {
      error = QStringLiteral(
        "The package's program NCA cannot be opened now, though it could when "
        "the file was identified. Missing: %1.")
          .arg(package.missingKeys.isEmpty() ? QStringLiteral("(unstated)")
                                             : package.missingKeys.join(QStringLiteral(", ")));
      return false;
    }
    QStringList extracted;
    if (!extractSwitchNspPlaintext(sourcePath, package, container, extracted, error))
      return false;
    for (const QString& relative : extracted)
      log.append(QStringLiteral("container/%1").arg(relative));
    if (!extractSwitchNspMain(sourcePath, package, executable, error)) return false;
    log.append(QStringLiteral("executable <- ExeFS %1 of %2 (%3 bytes)")
                 .arg(package.mainModule, package.programNca).arg(package.mainSize));
    stagedExecutable = executable;
    return true;
  }

  error = QStringLiteral("%1 was marked for extraction but is not a container "
                         "this stages from.").arg(description.containerName);
  return false;
}

QString guestAppFileFilter(SystemKind system) {
  if (system == SystemKind::Vita) {
    return QStringLiteral(
      "Vita applications (*.self *.velf *.elf *.bin *.vpk *.pkg);;All files (*)");
  }
  return QStringLiteral("Horizon applications (*.nro *.nso *.nsp);;All files (*)");
}

bool scanGuestAppDirectory(SystemKind system,
                           const QString& directory,
                           QList<GuestCatalogEntry>& entries,
                           QStringList& warnings,
                           QString& error) {
  entries.clear();
  warnings.clear();
  if (!QFileInfo(directory).isDir()) {
    error = QStringLiteral("The selected %1 application directory does not exist.")
              .arg(systemKindDisplayName(system));
    return false;
  }
  const QStringList patterns = system == SystemKind::Vita
    ? QStringList{ QStringLiteral("*.self"), QStringLiteral("*.velf"),
                   QStringLiteral("*.vpk"), QStringLiteral("*.pkg"),
                   QStringLiteral("eboot.bin") }
    : QStringList{ QStringLiteral("*.nro"), QStringLiteral("*.nso"),
                   QStringLiteral("*.nsp") };
  QDirIterator iterator(directory, patterns, QDir::Files, QDirIterator::Subdirectories);
  QSet<QString> seen;
  while (iterator.hasNext()) {
    const QString path = iterator.next();
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString key =
      (canonical.isEmpty() ? QFileInfo(path).absoluteFilePath() : canonical).toLower();
    if (seen.contains(key)) continue;
    seen.insert(key);
    GuestAppDescription description;
    QString inspectError;
    if (!inspectGuestApp(system, path, description, inspectError)) {
      warnings.append(QStringLiteral("%1: %2").arg(path, inspectError));
      continue;
    }
    // An image the front-end will not open is left out rather than queued to
    // fail on the device: a batch export exists to produce packages that run.
    if (!description.loadable()) {
      warnings.append(QStringLiteral("%1: %2").arg(path, description.refusal));
      continue;
    }
    entries.append({ path, description.suggestedTitle, description.containerName });
  }
  std::sort(entries.begin(), entries.end(),
            [](const GuestCatalogEntry& a, const GuestCatalogEntry& b) {
              return a.suggestedTitle.compare(b.suggestedTitle, Qt::CaseInsensitive) < 0;
            });
  return true;
}

QString generatedGuestAppProjectCMake(const PipelineRequest& request,
                                      const GuestAppDescription& app,
                                      const QString& bundleName) {
  Q_UNUSED(app);
  const bool vita = request.system == SystemKind::Vita;
  const QString frontEnd = systemFrontEndName(request.system);
  const QString subdirectory = vita ? QStringLiteral("vita2hos")
                                    : QStringLiteral("horizon2mvii");
  const QString prefix = vita ? QStringLiteral("VITA2MVII")
                              : QStringLiteral("HORIZON2MVII");
  const QString guestOs = vita ? QStringLiteral("the Vita's kernel and its module imports")
                               : QStringLiteral("Horizon's kernel, SVC layer and HIPC/CMIF IPC");
  return QStringLiteral(R"CMAKE(cmake_minimum_required(VERSION 3.20)
project(GeneratedGuestVirtuaPackage C CXX ASM)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# There is no host form of this package, and that is the design rather than a
# packaging gap: the guest's ARMv7 instructions run on the device's ARMv7 CPU
# untranslated. An x86_64 build would compile every line of the loader and then
# have nothing it could branch to. framework/extra/%2 refuses a non-ARM
# CMAKE_SYSTEM_PROCESSOR itself; it is said here too, where the mistake is made.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
  message(FATAL_ERROR
    "This package targets MVII/Virtua. Configure with "
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/framework/extra/virtua/CMake/VirtuaArmToolchain.cmake "
    "-DPOWERENGINE_ROOT=<PowerEngine> -DVIRTUA_LLVM_ROOT=<compiler bundle>")
endif()

set(GUEST_APP_NAME %1)
set(_GUEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}")
set(_GUEST_STAGE "${CMAKE_BINARY_DIR}/steganos-package/guest-runtime/$<CONFIG>")

# The guest image itself. Nothing was recompiled during export: %4 is what
# this package supplies, and the guest's own code is loaded, relocated and
# branched to. Its absence is a hard error — an image-less package would build
# and then exit on argv[1].
set(GUEST_APP_IMAGE "${_GUEST_SRC}/package_inputs/executable")
if(NOT EXISTS "${GUEST_APP_IMAGE}")
  message(FATAL_ERROR
    "package_inputs/executable is missing. Re-export from PSXRecomp Studio; "
    "this package carries the guest image, it does not fetch one.")
endif()

# Plain variables, read by framework/extra/%2/CMakeLists.txt before it defines
# anything. %3_PAYLOAD is what makes it stage the image beside the runtime as
# `executable`, which is the path the front-end reads from argv[1].
set(%3_OUTPUT_NAME "${GUEST_APP_NAME}")
set(%3_PAYLOAD "${GUEST_APP_IMAGE}")
set(%3_STAGE_DIR "${_GUEST_STAGE}")
add_subdirectory(framework/extra/%2 "${CMAKE_BINARY_DIR}/%2")

# The rest of what a Steganos package carries.
add_custom_target(guest-runtime ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GUEST_SRC}/AppIcon.png" "${_GUEST_STAGE}/AppIcon.png"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GUEST_SRC}/game.manifest.json" "${_GUEST_STAGE}/game.manifest.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_GUEST_SRC}/PSXRecomp-Proof.zip" "${_GUEST_STAGE}/PSXRecomp-Proof.zip"
  DEPENDS "${_GUEST_SRC}/AppIcon.png"
          "${_GUEST_SRC}/game.manifest.json"
          "${_GUEST_SRC}/PSXRecomp-Proof.zip"
  COMMENT "Staging the ${GUEST_APP_NAME} Virtua package"
  VERBATIM)
add_dependencies(guest-runtime %5-stage)
)CMAKE")
    .arg(cmakeQuoted(bundleName), subdirectory, prefix, guestOs, frontEnd);
}

QString generatedGuestAppReadme(const PipelineRequest& request,
                                const GuestAppDescription& app,
                                const QString& bundleName) {
  const bool vita = request.system == SystemKind::Vita;
  const QString frontEnd = systemFrontEndName(request.system);
  const QString subdirectory = vita ? QStringLiteral("vita2hos")
                                    : QStringLiteral("horizon2mvii");
  const QString guestCpu = vita ? QStringLiteral("ARMv7-A Cortex-A9")
                                : QStringLiteral("ARMv7-A (AArch32)");
  const QString guestOs = vita
    ? QStringLiteral(
        "the Vita's kernel API. Every import the module names is bound to a "
        "host implementation, and an import with no implementation is a hard "
        "failure reported by name — never a stub that returns 0 and misbehaves "
        "somewhere else.")
    : QStringLiteral(
        "Horizon's kernel, SVC layer and HIPC/CMIF IPC. An unimplemented SVC "
        "prints its register frame and stops rather than returning a Result "
        "for a service that did not run.");
  // Where the staged image came from. A package was opened during the export
  // and one entry taken out of it, and the README says which entry rather than
  // presenting the result as the file that was selected.
  const QString imageProvenance = app.requiresExtraction
    ? QStringLiteral(
        "- `package_inputs/executable` — the guest image: the `%1` entry of the "
        "%2 that was selected, taken out of it at export time. "
        "`package_inputs/container/` holds what that package carried alongside "
        "it, so the extraction can be checked against its source rather than "
        "taken on trust.")
        .arg(app.executableName, app.containerName)
    : QStringLiteral(
        "- `package_inputs/executable` — the guest image, exactly as selected: "
        "a %1 of %2 bytes.")
        .arg(app.containerName, QString::number(app.size));
  return QStringLiteral(
    "# %1 — %2 Studio export\n\n"
    "Generated by PSXRecomp Studio for **Virtua ARM**.\n\n"
    "## Nothing here was recompiled\n\n"
    "The %3 is %4 and the MVII device is an ARMv7-A Cortex-A7. Same instruction "
    "set — so the guest's own code runs on the CPU untranslated, and there is no "
    "translation step to perform, inspect or verify. What is missing on the "
    "device is not a CPU, it is the guest's operating system, and that is what "
    "`framework/extra/%5` supplies: %6\n\n"
    "This is the WINE model, not emulation, and it is why this package exists "
    "for Virtua ARM alone. A macOS, Windows or Linux build would be an x86_64 "
    "loader with nothing it could branch to.\n\n"
    "## Layout\n\n"
    "%7 It is staged beside the runtime as `executable`, which is the path "
    "`%2` reads from `argv[1]`.\n"
    "- `framework/extra/%5/` — the front-end: the loader, the relocator and the "
    "guest-OS reimplementation.\n"
    "- `framework/extra/virtua/` — the shared guest-OS layer (`virtua-wine`: the "
    "ARM ELF32 mapper, the guest memory mapper and the missing-import registry) "
    "and the CMake that resolves PowerEngine. Nothing from PowerEngine is "
    "vendored; the toolchain, sysroot, R-Dash, linker script and packager all "
    "come out of `POWERENGINE_ROOT`.\n"
    "- `proof/` — the container identification and the packaged-image verification.\n\n"
    "## Build\n\n"
    "```sh\ncmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \\\n"
    "  -DCMAKE_TOOLCHAIN_FILE=framework/extra/virtua/CMake/VirtuaArmToolchain.cmake \\\n"
    "  -DPOWERENGINE_ROOT=/path/to/PowerEngine \\\n"
    "  -DVIRTUA_LLVM_ROOT=/path/to/PowerEngine/compiler-bundle\n"
    "cmake --build build --target guest-runtime --parallel\n```\n\n"
    "The package is emitted under `build/steganos-package/guest-runtime/Release/`: "
    "`%8.virtua` with `executable` beside it.\n")
      .arg(request.windowTitle, frontEnd, systemKindDisplayName(request.system),
           guestCpu, subdirectory, guestOs, imageProvenance, bundleName);
}

} // namespace psxstudio
