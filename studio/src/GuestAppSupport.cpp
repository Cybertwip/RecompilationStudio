#include "GuestAppSupport.h"

#include "PipelineSupport.h"
#include "SwitchNsp.h"
#include "VitaPfs.h"
#include "VitaPkg.h"

#include "quazip.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
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
    case GuestContainer::VitaAppDir:  return QStringLiteral("unpacked title");
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

// ── the per-title licence ──────────────────────────────────────────────────
//
// A Vita title's PFS layer is keyed by a 16-byte klicensee that belongs to that
// one title and ships only with its licence. It cannot be derived from the
// title, so the whole of what Studio can do is look where a licence is kept and
// check each one it finds against the title itself. That check — recomputing
// the HMAC-SHA1 files.db signs its own header with — is what makes the search
// order a matter of speed rather than of correctness: a licence for another
// title fails the header and is passed over, and one that passes the header is
// the licence for this title.

struct VitaLicenseSearch {
  QList<VitaLicense> candidates;
  QStringList tried;  // the places swept, in order — what to report on a miss
  QStringList seen;   // absolute paths already read, so each file is read once
};

/* `tried` is what the refusal quotes, so it names places rather than files: a
 * list of the files that happened to exist is exactly the wrong thing to print
 * when the complaint is that none did. */
void noteVitaSearchLocation(VitaLicenseSearch& search, const QString& where) {
  if (!search.tried.contains(where)) search.tried.append(where);
}

void appendVitaLicenseFile(const QString& path, VitaLicenseSearch& search) {
  const QFileInfo info(path);
  if (!info.isFile()) return;
  const QString absolute = info.absoluteFilePath();
  if (search.seen.contains(absolute)) return;
  search.seen.append(absolute);

  VitaLicense license;
  QString error;
  const QString suffix = info.suffix().toLower();
  if (suffix == QStringLiteral("zrif") || suffix == QStringLiteral("txt")) {
    // A licence passed around as text: base64 of a deflated SceNpDrmLicense.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QString text = QString::fromLatin1(file.read(64 * 1024)).trimmed();
    if (readVitaZrifLicense(text, license, error)) {
      license.source = absolute;
      search.candidates.append(license);
    }
    return;
  }
  if (readVitaLicenseFile(path, license, error)) search.candidates.append(license);
}

/* The named forms first, then anything else in the directory that is shaped
 * like a licence. Bounded to one directory: this is a look-beside, not a scan. */
void sweepVitaLicenseDirectory(const QDir& directory,
                               const QString& baseName,
                               const QString& titleId,
                               VitaLicenseSearch& search) {
  if (!directory.exists()) return;
  noteVitaSearchLocation(
    search,
    QStringLiteral("%1 (work.bin, license.rif, %2*.rif, *.zrif)")
      .arg(directory.absolutePath(),
           titleId.isEmpty() ? QString()
                             : QStringLiteral("%1.rif, %1.zrif, ").arg(titleId)));
  QStringList named;
  if (!baseName.isEmpty()) {
    named << baseName + QStringLiteral(".rif")
          << baseName + QStringLiteral(".zrif")
          << baseName + QStringLiteral(".work.bin");
  }
  if (!titleId.isEmpty()) {
    named << titleId + QStringLiteral(".rif")
          << titleId + QStringLiteral(".zrif")
          << titleId + QStringLiteral(".bin");
  }
  named << QStringLiteral("work.bin") << QStringLiteral("license.rif");
  for (const QString& name : named)
    appendVitaLicenseFile(directory.filePath(name), search);

  const QStringList rest =
    directory.entryList({ QStringLiteral("*.rif"), QStringLiteral("*.zrif") },
                        QDir::Files, QDir::Name);
  for (const QString& name : rest)
    appendVitaLicenseFile(directory.filePath(name), search);
}

/* A title dumped beside the package it came from. `<TITLEID>/sce_sys/package/
 * work.bin` is the fixed shape of that, so it is looked for by name at one and
 * two levels down rather than found by walking the tree. */
void sweepVitaDumpedTitles(const QDir& directory,
                           const QString& titleId,
                           VitaLicenseSearch& search) {
  if (titleId.isEmpty() || !directory.exists()) return;
  const QString relative =
    titleId + QStringLiteral("/sce_sys/package/work.bin");
  noteVitaSearchLocation(search, QStringLiteral("%1/[*/]%2")
                                   .arg(directory.absolutePath(), relative));
  appendVitaLicenseFile(directory.filePath(relative), search);
  const QStringList children =
    directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QString& child : children)
    appendVitaLicenseFile(directory.filePath(child + QLatin1Char('/') + relative), search);
}

/* `containerPath` is the .pkg or the title directory that was selected;
 * `titleDirectory` is set only when the title is already unpacked. */
VitaLicenseSearch findVitaLicenseCandidates(const QString& containerPath,
                                            const QString& titleId,
                                            const QString& titleDirectory) {
  VitaLicenseSearch search;
  const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

  const QString zrif =
    environment.value(QStringLiteral("PSXRECOMP_VITA_ZRIF")).trimmed();
  if (!zrif.isEmpty()) {
    noteVitaSearchLocation(search, QStringLiteral("PSXRECOMP_VITA_ZRIF"));
    VitaLicense license;
    QString error;
    if (readVitaZrifLicense(zrif, license, error)) search.candidates.append(license);
  }
  const QString fromEnvironment =
    environment.value(QStringLiteral("PSXRECOMP_VITA_LICENSE")).trimmed();
  if (!fromEnvironment.isEmpty()) {
    if (QFileInfo(fromEnvironment).isDir()) {
      sweepVitaLicenseDirectory(QDir(fromEnvironment), QString(), titleId, search);
      sweepVitaDumpedTitles(QDir(fromEnvironment), titleId, search);
    } else {
      noteVitaSearchLocation(search, QStringLiteral("PSXRECOMP_VITA_LICENSE=%1")
                                       .arg(fromEnvironment));
      appendVitaLicenseFile(fromEnvironment, search);
    }
  }

  // The title's own licence, when it was dumped with one. This is the form in
  // branding/vita/standard/PCSE00317 and it is the one that belongs to the
  // title by construction, so it is tried before anything found by looking
  // around.
  if (!titleDirectory.isEmpty()) {
    const QDir title(titleDirectory);
    noteVitaSearchLocation(
      search, QStringLiteral("%1/sce_sys/package/ (work.bin, temp.bin)")
                .arg(QDir(titleDirectory).absolutePath()));
    appendVitaLicenseFile(title.filePath(QStringLiteral("sce_sys/package/work.bin")), search);
    appendVitaLicenseFile(title.filePath(QStringLiteral("sce_sys/package/temp.bin")), search);
    sweepVitaLicenseDirectory(title, QString(), titleId, search);
  }

  const QFileInfo container(containerPath);
  const QDir beside = container.absoluteDir();
  sweepVitaLicenseDirectory(beside, container.completeBaseName(), titleId, search);
  sweepVitaDumpedTitles(beside, titleId, search);
  QDir above = beside;
  if (above.cdUp()) {
    sweepVitaLicenseDirectory(above, container.completeBaseName(), titleId, search);
    sweepVitaDumpedTitles(above, titleId, search);
  }
  return search;
}

/* Takes the candidate that opens this title. Everything rejected is said, with
 * the reason, because "no licence" and "three licences, none of them this
 * title's" are different problems for whoever is looking at the export. */
bool chooseVitaLicense(const VitaLicenseSearch& search,
                       const QString& titleId,
                       const QByteArray& filesDbHeader,
                       VitaLicense& license,
                       QStringList& notes,
                       QString& refusal) {
  for (const VitaLicense& candidate : search.candidates) {
    QString error;
    if (verifyVitaLicenseHeader(filesDbHeader, candidate, error)) {
      license = candidate;
      notes.append(
        QStringLiteral("Licence: %1 — content id %2, %3. It signs this title's "
                       "files.db header, so it is this title's licence.")
          .arg(candidate.source,
               candidate.contentId.isEmpty() ? QStringLiteral("(none stated)")
                                             : candidate.contentId,
               candidate.fakeAccount ? QStringLiteral("NoNpDrm (no account)")
                                     : QStringLiteral("account %1")
                                         .arg(candidate.accountId, 16, 16, QLatin1Char('0'))));
      return true;
    }
    notes.append(QStringLiteral("Licence %1 (content id %2) rejected: %3")
                   .arg(candidate.source,
                        candidate.contentId.isEmpty() ? QStringLiteral("(none stated)")
                                                      : candidate.contentId,
                        error));
  }

  const QString looked = search.tried.isEmpty()
    ? QStringLiteral("(nowhere — no candidate paths existed)")
    : search.tried.join(QStringLiteral("; "));
  const QString subject = titleId.isEmpty()
    ? QStringLiteral("this title")
    : QStringLiteral("%1").arg(titleId);
  refusal = search.candidates.isEmpty()
    ? QStringLiteral(
        "%1's files are inside its PFS layer, and that layer is keyed by a "
        "klicensee that belongs to that one title — a licence for the same game "
        "in another region does not open it. The key is not in the container "
        "and cannot be derived from it: it ships with the title's licence. "
        "Studio looked in: %2. Supply the licence as a .rif or a work.bin "
        "beside the file, as a zRIF in PSXRECOMP_VITA_ZRIF, or point "
        "PSXRECOMP_VITA_LICENSE at it.").arg(subject, looked)
    : QStringLiteral(
        "%1 licence(s) were found but none of them opens %2 — each one derives "
        "a different signature for that title's files.db header, which is what "
        "a licence for a different title does. The rejected content ids are in "
        "the notes above; a licence has to match the title id exactly. Studio "
        "looked in: %3.")
        .arg(search.candidates.size()).arg(subject, looked);
  return false;
}

/* The licence that opens `filesDbHeader`, resolved from scratch. Identification
 * and staging both call this rather than passing a key between them: a key held
 * in a description would go stale, and re-deriving it costs one HMAC. */
bool vitaLicenseForTitle(const QString& containerPath,
                         const QString& titleId,
                         const QString& titleDirectory,
                         const QByteArray& filesDbHeader,
                         VitaLicense& license,
                         QStringList& notes,
                         QString& error) {
  const VitaLicenseSearch search =
    findVitaLicenseCandidates(containerPath, titleId, titleDirectory);
  return chooseVitaLicense(search, titleId, filesDbHeader, license, notes, error);
}

/* Copies a tree, recording every file written relative to `to`. */
bool copyTree(const QString& from, const QString& to,
              QStringList& written, QString& error) {
  const QDir source(from);
  QDirIterator iterator(from, QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString absolute = iterator.next();
    const QString relative = source.relativeFilePath(absolute);
    const QString destination = QDir(to).filePath(relative);
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
      error = QStringLiteral("Could not create %1.")
                .arg(QFileInfo(destination).absolutePath());
      return false;
    }
    if (!copyFileReplacing(absolute, destination, error)) return false;
    written.append(relative);
  }
  written.sort();
  return true;
}

/* The files a title carries that its own files.db does not cover — the PFS
 * metadata and the licence. They are not part of what runs, so they are kept
 * beside the export rather than in it, the same way an NSP's ticket is. */
bool copyVitaMetadata(const QString& from, const QString& to,
                      QStringList& written, QString& error) {
  const QDir source(from);
  const QStringList directories = { QStringLiteral("sce_pfs"),
                                    QStringLiteral("sce_sys/package") };
  for (const QString& relative : directories) {
    const QDir directory(source.filePath(relative));
    if (!directory.exists()) continue;
    const QStringList names = directory.entryList(QDir::Files, QDir::Name);
    for (const QString& name : names) {
      const QString destination =
        QDir(to).filePath(relative + QLatin1Char('/') + name);
      if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        error = QStringLiteral("Could not create %1.")
                  .arg(QFileInfo(destination).absolutePath());
        return false;
      }
      if (!copyFileReplacing(directory.filePath(name), destination, error)) return false;
      written.append(relative + QLatin1Char('/') + name);
    }
  }
  return true;
}

/* Reads a title directory as a guest application: what it is, whether its files
 * are behind the PFS layer, and — when they are — which licence opens it. */
void inspectVitaDirectory(const QString& directory, GuestAppDescription& description) {
  description.container = GuestContainer::VitaAppDir;
  description.arch = GuestArch::Arm32;

  VitaTitleDirectory title;
  QString titleError;
  if (!inspectVitaTitleDirectory(directory, title, titleError)) {
    description.refusal = titleError;
    return;
  }
  description.contents = title.files;
  description.containerTitle = title.title;
  description.containerTitleId = title.titleId;
  description.notes.append(
    QStringLiteral("Unpacked title at %1: %2 file(s), %3 bytes.")
      .arg(title.path).arg(title.files.size()).arg(title.totalBytes));
  if (!title.title.isEmpty() || !title.titleId.isEmpty()) {
    description.notes.append(
      QStringLiteral("param.sfo: %1 (%2)%3%4")
        .arg(title.title.isEmpty() ? QStringLiteral("untitled") : title.title)
        .arg(title.titleId.isEmpty() ? QStringLiteral("no title id") : title.titleId)
        .arg(title.category.isEmpty() ? QString()
                                      : QStringLiteral(", category %1").arg(title.category))
        .arg(title.appVersion.isEmpty() ? QString()
                                        : QStringLiteral(", version %1").arg(title.appVersion)));
  }
  description.notes.append(title.notes);
  description.requiresExtraction = true;
  description.executableName = title.executableName;

  // A title with no sce_pfs was decrypted before it got here; its eboot.bin is
  // a module and is classified as one.
  if (!title.havePfs) {
    QFile eboot(QDir(title.path).filePath(title.executableName));
    if (!eboot.open(QIODevice::ReadOnly)) {
      description.refusal = QStringLiteral("%1 could not be read.").arg(title.executableName);
      return;
    }
    const QByteArray head = eboot.read(0x1000);
    GuestAppDescription probe;
    probe.system = SystemKind::Vita;
    if (!classifyVitaModule(head, probe)) {
      description.refusal = QStringLiteral(
        "This directory has no sce_pfs, so its files are not PFS-encrypted — "
        "but its %1 is neither an ARM ELF nor a SELF (it begins %2).")
          .arg(title.executableName,
               QString::fromLatin1(head.left(4).toHex(' ')));
      return;
    }
    description.arch = probe.arch;
    description.refusal = probe.refusal;
    description.notes.append(
      QStringLiteral("No sce_pfs: this title is already decrypted, and its %1 is a %2.")
        .arg(title.executableName, containerDisplayName(probe.container)));
    return;
  }

  QFile filesDb(QDir(title.path).filePath(QStringLiteral("sce_pfs/files.db")));
  if (!filesDb.open(QIODevice::ReadOnly)) {
    description.refusal = QStringLiteral(
      "sce_pfs/files.db could not be read, so the PFS layer cannot be opened.");
    return;
  }
  const QByteArray header = filesDb.read(0x400);
  VitaLicense license;
  if (!vitaLicenseForTitle(title.path, title.titleId, title.path, header, license,
                           description.notes, description.refusal)) {
    return;
  }
  description.notes.append(
    QStringLiteral("PFS: files.db and %1 are present; the export will decrypt the "
                   "title with this licence.")
      .arg(title.haveUnicv ? QStringLiteral("unicv.db") : QStringLiteral("icv.db")));
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
    // The second layer. Its key is the title's licence, and the package's own
    // files.db — read here, 0x400 bytes of it — is what says whether a given
    // licence is that title's.
    const auto entry =
      std::find_if(package.items.cbegin(), package.items.cend(),
                   [](const VitaPkgItem& item) {
                     return item.name == QStringLiteral("sce_pfs/files.db");
                   });
    if (entry == package.items.cend()) {
      description.refusal = QStringLiteral(
        "The package's %1 is inside the PFS layer — sce_pfs/pflist lists it "
        "without the `nenc` mark — but the package carries no sce_pfs/files.db, "
        "which is the file that layer is described and keyed by. This package "
        "cannot be opened by anything.").arg(package.executableName);
      return;
    }
    QByteArray filesDbHeader;
    QString headerError;
    if (!readVitaPkgItem(path, package, *entry, 0x400, filesDbHeader, headerError)) {
      description.refusal =
        QStringLiteral("The package's sce_pfs/files.db could not be read: %1")
          .arg(headerError);
      return;
    }

    // A licence the package itself carries, before looking around it. Usually a
    // placeholder — which readVitaLicenseBlob refuses by name — but a package
    // repacked with its licence has the real one here.
    VitaLicenseSearch search =
      findVitaLicenseCandidates(path, package.titleId, QString());
    for (const QString& name : { QStringLiteral("sce_sys/package/work.bin"),
                                 QStringLiteral("sce_sys/package/temp.bin") }) {
      const auto held =
        std::find_if(package.items.cbegin(), package.items.cend(),
                     [&name](const VitaPkgItem& item) { return item.name == name; });
      if (held == package.items.cend()) continue;
      noteVitaSearchLocation(search,
                             QStringLiteral("%1 (inside the package)").arg(name));
      QByteArray blob;
      QString itemError;
      if (!readVitaPkgItem(path, package, *held, 0x200, blob, itemError)) continue;
      VitaLicense license;
      if (readVitaLicenseBlob(blob, QStringLiteral("%1 (inside the package)").arg(name),
                              license, itemError)) {
        search.candidates.prepend(license);
      }
    }

    VitaLicense license;
    if (!chooseVitaLicense(search, package.titleId, filesDbHeader, license,
                           description.notes, description.refusal)) {
      description.refusal = QStringLiteral(
        "The package itself opened — %1 items, decrypted with the package key "
        "that is fixed in the format — but its %2 is inside the second layer, "
        "PFS. %3")
          .arg(package.items.size())
          .arg(package.executableName, description.refusal);
      return;
    }
    description.notes.append(
      QStringLiteral("PFS: the export will extract the package and decrypt its "
                     "%1 file(s) with this licence.")
        .arg(package.pfsProtectedFiles.size()));
    description.requiresExtraction = true;
    description.executableName = package.executableName;
    description.arch = GuestArch::Arm32;
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

/* The directory a title was dumped to, when what was selected is a file inside
 * one. `<TITLEID>/eboot.bin` is a module, but the *title* is the directory: its
 * data.vfs and its sce_module suprx files are what the game reads at runtime,
 * and its sce_pfs/ is what the whole tree is encrypted under. Selecting the
 * eboot and getting only the eboot is the same mistake the Switch export
 * made. */
QString vitaTitleDirectoryOf(const QString& path) {
  const QFileInfo info(path);
  const QDir directory = info.isDir() ? QDir(path) : info.absoluteDir();
  if (!QFileInfo(directory.filePath(QStringLiteral("sce_sys/param.sfo"))).isFile())
    return QString();
  if (!QFileInfo(directory.filePath(QStringLiteral("eboot.bin"))).isFile())
    return QString();
  return directory.absolutePath();
}

void inspectVita(const QByteArray& head, const QString& path,
                 GuestAppDescription& description) {
  if (head.size() >= 4 && head.startsWith(QByteArrayLiteral("\x7f" "PKG"))) {
    description.container = GuestContainer::VitaPkg;
    inspectVitaPackage(path, description);
    return;
  }

  // Before the module check: a title directory holds a module, but the module
  // alone is not the title.
  const QString titleDirectory = vitaTitleDirectoryOf(path);
  if (!titleDirectory.isEmpty()) {
    inspectVitaDirectory(titleDirectory, description);
    return;
  }

  if (classifyVitaModule(head, description)) return;

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
    "This file is not an ARM ELF, a SELF, a VPK or a PKG, and the directory it "
    "is in is not an unpacked title (no sce_sys/param.sfo beside an eboot.bin) "
    "— which are the forms a Vita application arrives in.");
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

  // The RomFS is where a title's bulk lives, so it is stated with the same
  // detail as the ExeFS rather than summarised as "data".
  if (package.romFsPresent) {
    description.notes.append(
      QStringLiteral("RomFS: %1 bytes at NCA offset 0x%2, %3 file(s) totalling %4 bytes.")
        .arg(package.romFsSize)
        .arg(package.romFsOffset, 0, 16)
        .arg(package.romFsFileCount)
        .arg(package.romFsFileBytes));
    if (!package.romFsRootEntries.isEmpty()) {
      description.notes.append(
        QStringLiteral("  root: %1%2")
          .arg(package.romFsRootEntries.join(QStringLiteral(", ")),
               package.romFsRootEntries.size() >= 64 ? QStringLiteral(", …")
                                                     : QString()));
    }
  } else {
    // Said, not passed over. Almost every application NCA has one, so its
    // absence is a fact about this package worth carrying into the export.
    description.notes.append(
      QStringLiteral("RomFS: none in the program NCA — this package carries no "
                     "data section."));
  }
  for (const QString& entry : package.romFsRootEntries)
    description.contents.append(QStringLiteral("romfs:/%1").arg(entry));

  // The title's own name, out of the Control NCA's NACP. Until now the export
  // was named after the NSP file.
  if (!package.displayTitle.isEmpty()) {
    description.containerTitle = package.displayTitle;
    description.notes.append(
      QStringLiteral("control.nacp: %1%2")
        .arg(package.displayTitle,
             package.publisher.isEmpty()
               ? QString()
               : QStringLiteral(" — %1").arg(package.publisher)));
  }
  if (!package.icon.isEmpty()) {
    description.notes.append(QStringLiteral("Control NCA icon: %1 (%2 bytes).")
                               .arg(package.iconEntryName).arg(package.icon.size()));
  }

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
  // A Vita title can be a directory rather than a file — the shape it is dumped
  // to. It is identified from its contents, so there is no header to read.
  if (system == SystemKind::Vita && QFileInfo(path).isDir()) {
    inspectVitaDirectory(QDir(path).absolutePath(), description);
    description.containerName = containerDisplayName(description.container);
    description.suggestedTitle = description.containerTitle.isEmpty()
                                   ? QDir(path).dirName()
                                   : description.containerTitle;
    for (const QString& relative : description.contents) {
      description.size +=
        QFileInfo(QDir(path).filePath(relative)).size();
    }
    return true;
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

  // A Vita title, whichever of its two shapes it arrived in. Both end the same
  // way: the whole title under `data/`, decrypted if it was PFS-encrypted, with
  // its eboot.bin also copied to `executable` — which is what vita2mvii is
  // handed as argv[1]. Staging the module alone left the game without its
  // data.vfs and its sce_module/*.suprx, which is most of what it is.
  if (description.container == GuestContainer::VitaPkg ||
      description.container == GuestContainer::VitaAppDir) {
    const QString data = QDir(destinationDirectory).filePath(QStringLiteral("data"));
    QString title;   // the unpacked title tree, wherever it ends up
    QString scratch; // set when the package had to be unpacked to reach it

    // The scratch is tens of megabytes and every failure below returns early,
    // so its removal is tied to leaving this block rather than written out at
    // each exit.
    struct ScratchGuard {
      const QString& path;
      ~ScratchGuard() { if (!path.isEmpty()) QDir(path).removeRecursively(); }
    } scratchGuard{ scratch };

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
      // The whole package. The PFS-encrypted files come out as ciphertext,
      // which is what the package holds; decrypting them is the next step.
      scratch = QDir(destinationDirectory).filePath(QStringLiteral("package_source"));
      QDir(scratch).removeRecursively();
      QStringList extracted;
      if (!extractVitaPkg(sourcePath, package, scratch,
                          [](const VitaPkgItem& item) { return !item.isDirectory(); },
                          extracted, error)) {
        return false;
      }
      log.append(QStringLiteral("PKG: %1 item(s) extracted, %2 bytes")
                   .arg(extracted.size()).arg(package.dataSize));
      title = scratch;
    } else {
      title = QFileInfo(sourcePath).isDir() ? QDir(sourcePath).absolutePath()
                                            : QFileInfo(sourcePath).absolutePath();
    }

    // The PFS metadata and the licence: kept beside the export so the
    // decryption can be checked against its source rather than taken on trust.
    QStringList metadata;
    if (!copyVitaMetadata(title, container, metadata, error)) return false;
    for (const QString& relative : metadata)
      log.append(QStringLiteral("container/%1").arg(relative));

    const bool encrypted =
      QFileInfo(QDir(title).filePath(QStringLiteral("sce_pfs/files.db"))).isFile();
    if (encrypted) {
      QFile filesDb(QDir(title).filePath(QStringLiteral("sce_pfs/files.db")));
      if (!filesDb.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("%1 could not be read.").arg(filesDb.fileName());
        return false;
      }
      const QByteArray header = filesDb.read(0x400);
      filesDb.close();

      VitaLicense license;
      QStringList notes;
      if (!vitaLicenseForTitle(sourcePath, description.containerTitleId,
                               description.container == GuestContainer::VitaAppDir
                                 ? title : QString(),
                               header, license, notes, error)) {
        return false;
      }
      log.append(notes);

      QStringList pfsLog;
      if (!decryptVitaTitle(title, license, data, pfsLog, error)) return false;
      // psvpfsparser's own account: what it verified and what it wrote. Kept
      // because it is the proof the hash tree checked out, not decoration.
      log.append(pfsLog);
    } else {
      QStringList copied;
      if (!copyTree(title, data, copied, error)) return false;
      for (const QString& relative : copied)
        log.append(QStringLiteral("data/%1").arg(relative));
    }

    const QString module = QDir(data).filePath(description.executableName);
    QFile staged(module);
    if (!staged.open(QIODevice::ReadOnly)) {
      error = QStringLiteral(
        "%1 is not in the staged title. The title's own files.db decides what "
        "is written, and it did not name it.").arg(description.executableName);
      return false;
    }
    const QByteArray moduleHead = staged.read(64);
    staged.close();

    // Identification could not read this module when it was still behind PFS,
    // so the ELF/SELF check happens here instead — on the bytes that were
    // actually written, not on the assumption that a Vita title decrypts to a
    // Vita module. A wrong-but-verifying licence is not possible, but a
    // truncated or mis-assembled decryption is, and it shows up right here.
    GuestAppDescription probe;
    if (!classifyVitaModule(moduleHead, probe)) {
      error = QStringLiteral(
        "The decrypted %1 is neither an ARM ELF nor a SELF (it begins %2). "
        "The PFS layer opened, so this is the title's own contents rather than "
        "a key problem.")
          .arg(description.executableName,
               QString::fromLatin1(moduleHead.left(4).toHex(' ')));
      return false;
    }
    if (!probe.refusal.isEmpty()) {
      error = QStringLiteral("The decrypted %1: %2")
                .arg(description.executableName, probe.refusal);
      return false;
    }
    log.append(QStringLiteral("data/%1 is a %2, %3")
                 .arg(description.executableName,
                      containerDisplayName(probe.container),
                      guestArchName(probe.arch)));

    if (!copyFileReplacing(module, executable, error)) return false;
    log.append(QStringLiteral("executable <- data/%1 (%2 bytes)")
                 .arg(description.executableName).arg(QFileInfo(module).size()));
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

    // The program, whole. `main` is branched to, but `rtld` is what relocates
    // it and `sdk`/`subsdk*` are the libraries it resolves its imports against;
    // `main.npdm` is the process manifest. Staging `main` alone produced a
    // package that was 2.7 MB of a 180 MB title.
    const QString data = QDir(destinationDirectory).filePath(QStringLiteral("data"));
    QStringList exeFs;
    if (!extractSwitchNspExeFs(sourcePath, package,
                               QDir(data).filePath(QStringLiteral("exefs")),
                               exeFs, error)) {
      return false;
    }
    for (const QString& name : exeFs) {
      const auto entry =
        std::find_if(package.exeFsEntries.cbegin(), package.exeFsEntries.cend(),
                     [&name](const Pfs0Entry& e) { return e.name == name; });
      log.append(QStringLiteral("data/exefs/%1 (%2 bytes)")
                   .arg(name)
                   .arg(entry == package.exeFsEntries.cend() ? 0u : entry->size));
    }

    // And the data. Verbatim: the RomFS image is self-describing and is what a
    // `romfs:` mount reads, so it is copied rather than unpacked into a host
    // tree whose names would be a re-encoding of the game's.
    if (package.romFsPresent) {
      const QString romFs = QDir(data).filePath(QStringLiteral("romfs.bin"));
      if (!extractSwitchNspRomFs(sourcePath, package, romFs, error)) return false;
      log.append(QStringLiteral("data/romfs.bin (%1 bytes, %2 file(s) totalling %3)")
                   .arg(package.romFsSize)
                   .arg(package.romFsFileCount)
                   .arg(package.romFsFileBytes));
    }
    if (!package.icon.isEmpty()) {
      const QString icon = QDir(data).filePath(QStringLiteral("control/") +
                                               package.iconEntryName);
      if (!writeBytes(icon, package.icon, error)) return false;
      log.append(QStringLiteral("data/control/%1 (%2 bytes)")
                   .arg(package.iconEntryName).arg(package.icon.size()));
    }

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
    // `eboot.bin` is listed in its own right: selecting the eboot inside an
    // unpacked title selects the title, which is the shape a dumped game is in.
    return QStringLiteral(
      "Vita applications (*.self *.velf *.elf *.bin *.vpk *.pkg eboot.bin);;"
      "Unpacked title (eboot.bin);;"
      "All files (*)");
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

# The guest's data, staged as `data/` beside the runtime.
#
# `executable` above is only the module that is branched to, and on both systems
# it is a small fraction of the title. For a Switch package it is the ExeFS
# `main`; the rest of the ExeFS (`rtld`, which relocates it, `sdk` and the
# subsdks, which its imports resolve against, and `main.npdm`) and the RomFS
# image are here. For a Vita title it is `eboot.bin`; the decrypted `sce_module`
# libraries and the asset archives are here. A package staged without them is a
# loader with nothing behind it, so Studio re-hashes every one of these files in
# the built package and fails the export if any is missing.
#
# The file list is globbed at configure time and the copy hangs off a stamp, so
# a rebuild that changed nothing does not re-copy a RomFS image that is hundreds
# of megabytes. Re-export or re-configure when the data changes.
set(_GUEST_DATA "${_GUEST_SRC}/package_inputs/data")
file(GLOB_RECURSE _GUEST_DATA_FILES LIST_DIRECTORIES false "${_GUEST_DATA}/*")
if(_GUEST_DATA_FILES)
  set(_GUEST_DATA_STAMP "${CMAKE_CURRENT_BINARY_DIR}/guest-data.stamp")
  add_custom_command(
    OUTPUT "${_GUEST_DATA_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_GUEST_DATA}" "${_GUEST_STAGE}/data"
    COMMAND ${CMAKE_COMMAND} -E touch "${_GUEST_DATA_STAMP}"
    DEPENDS ${_GUEST_DATA_FILES}
    COMMENT "Staging the ${GUEST_APP_NAME} guest data"
    VERBATIM)
  add_custom_target(guest-data DEPENDS "${_GUEST_DATA_STAMP}")
endif()

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
if(TARGET guest-data)
  add_dependencies(guest-runtime guest-data)
endif()
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
  // What `data/` holds differs by system, and saying only the Switch case read
  // as though a Vita export shipped its module alone.
  const QString dataProvenance = vita
    ? QStringLiteral(
        "the whole title, decrypted. A Vita application's files sit behind PFS, "
        "keyed by a klicensee that belongs to that one title and ships with its "
        "licence; the export resolved that licence, checked it against the "
        "title's own `files.db` header, and wrote the decrypted tree here — "
        "`eboot.bin`, the `sce_module` suprx libraries the module's imports "
        "resolve against, `sce_sys/`, and the asset archives, which are most of "
        "what the game is.")
    : QStringLiteral(
        "`exefs/` — the whole ExeFS, because `main` alone is the module that is "
        "branched to and not the program: `rtld` relocates it, `sdk` and the "
        "subsdks are what its imports resolve against, and `main.npdm` is the "
        "process manifest — and `romfs.bin`, the RomFS image, decrypted and "
        "otherwise verbatim. The RomFS is not unpacked into a directory tree: "
        "its names are UTF-8 and case-sensitive, the exporting host's "
        "filesystem is not, and the image is what a `romfs:` mount reads "
        "anyway.");
  const QString imageProvenance = app.requiresExtraction
    ? QStringLiteral(
        "- `package_inputs/executable` — the guest image: the `%1` entry of the "
        "%2 that was selected, taken out of it at export time. "
        "`package_inputs/container/` holds the metadata that package carried "
        "alongside it — tickets, the PFS databases, the content manifest — so "
        "the extraction can be checked against its source rather than taken on "
        "trust. The container itself is not copied; it would double the size of "
        "the export, and `proof/` records its SHA-256 instead.\n"
        "- `package_inputs/data/` — the rest of what came out of that "
        "container, staged as `data/` beside the runtime: %3\n"
        "  Every file here is hashed at export time and re-hashed in the built "
        "package; a build that does not carry them all is a failure, not a "
        "smaller package.")
        .arg(app.executableName, app.containerName, dataProvenance)
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
    "`%8.virtua`, `executable`, and `data/` when the container carried any.\n")
      .arg(request.windowTitle, frontEnd, systemKindDisplayName(request.system),
           guestCpu, subdirectory, guestOs, imageProvenance, bundleName);
}

} // namespace psxstudio
