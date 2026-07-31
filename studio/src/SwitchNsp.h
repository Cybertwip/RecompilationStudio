#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace psxstudio {

// Switch submission package (NSP) reader.
//
// An NSP is a PFS0 archive: a plain table of files, walked here without any
// key. Most of those files are NCAs, and an NCA's header and body are
// encrypted under Nintendo's console keys, which are not in the package and
// cannot be derived from it. So the reader has two tiers, and it is explicit
// about which one it is in:
//
//   * Always — the PFS0 table, the ticket's rights ID and encrypted title key,
//     and the `.cnmt.xml` the package ships in the clear: title ID, content
//     meta type, and which NCA is the program.
//   * With a key file — the NCA header (AES-XTS under `header_key`), the
//     section table, and then both of the program's sections: the ExeFS
//     partition, which holds `main` and the rest of the modules, and the RomFS,
//     which holds the game's data. Both decrypt with AES-CTR under the title
//     key from the ticket, or the NCA's own key area.
//
// The RomFS is the bulk of a package and it is not optional. UNDERTALE's
// program NCA is 180 MB, of which `main` is 2.7 MB and the RomFS is 150 MB:
// `game.win` and the soundtrack. A package staged without it is a loader with
// no game behind it, so the RomFS is located, measured and written out like
// everything else.
//
// Without keys the second tier is reported as unavailable and names the keys
// it wanted. It is never approximated: a wrong key produces a wrong module,
// and a wrong module is a package that fails on the device for a reason no
// one can see.

struct Pfs0Entry {
  QString name;
  quint64 offset{ 0 };  // absolute, in whatever the entry was read from
  quint64 size{ 0 };
};

struct SwitchKeySet {
  QHash<QString, QByteArray> keys;
  QStringList searchedPaths;
  QString loadedFrom;

  bool isEmpty() const { return keys.isEmpty(); }
  QByteArray value(const QString& name) const { return keys.value(name.toLower()); }
  bool has(const QString& name) const { return keys.contains(name.toLower()); }
};

/* Loads `prod.keys` from $PSXRECOMP_SWITCH_KEYS, then from beside `besidePath`,
 * then from ~/.switch. The paths tried are recorded in `searchedPaths` so a
 * miss can say where it looked. */
SwitchKeySet loadSwitchKeys(const QString& besidePath);

struct SwitchNspContent {
  QString type;  // Program, Meta, Control, LegalInformation, …
  QString id;
  quint64 size{ 0 };
};

struct SwitchNspInfo {
  QList<Pfs0Entry> entries;

  // From the .cnmt.xml the package ships unencrypted.
  QString titleId, contentMetaType, patchId;
  QList<SwitchNspContent> contents;
  QString programNca;  // the file name of the Program NCA
  QString controlNca;  // and of the Control NCA, which carries the NACP

  // From the .tik.
  QString ticketName;
  QByteArray rightsId;
  QByteArray encryptedTitleKey;

  // Second tier.
  bool keysLoaded{ false };
  QString keySource;         // the key file that was used, when one was
  QStringList keySearchPaths;  // where one was looked for, so a miss can say
  QStringList missingKeys;   // the key names that were wanted and absent
  QString ncaFormat;         // "NCA3", "NCA2", "NCA0"
  int masterKeyRevision{ 0 };
  bool programDecrypted{ false };
  QList<Pfs0Entry> exeFsEntries;  // offsets are relative to the NCA
  QString mainModule;             // "main", when the ExeFS carries one
  QByteArray mainModuleHead;      // its first bytes, for the architecture probe

  // From the ExeFS `main.npdm`, the process manifest Horizon's own loader reads
  // to decide how to create the process. Its flags byte is the authority on the
  // module's instruction set — an NSO can begin with a word that is not a branch
  // in either encoding, and Undertale's does, so probing the entry instruction
  // cannot always answer and this can.
  bool npdmPresent{ false };
  bool npdmIs64Bit{ false };
  int npdmAddressSpace{ -1 };  // 0=32-bit, 1=64-bit (legacy), 2=32-bit no-reserved, 3=64-bit
  QString npdmName;            // the process name the manifest declares

  // What the extraction functions need to read the sections out again.
  quint64 programNcaOffset{ 0 };  // the NCA's offset in the NSP file
  quint64 mainOffset{ 0 };        // `main`'s offset within the NCA
  quint64 mainSize{ 0 };
  QByteArray sectionKey;      // AES-128-CTR key for both program sections
  QByteArray sectionCounter;  // the ExeFS counter at NCA offset 0; empty when
                              // that section is stored unencrypted

  // The program's RomFS — the game's data. `romFsOffset` is NCA-relative and
  // already past the IVFC hash levels, so it addresses the RomFS image itself:
  // the thing a `romfs:` mount reads, staged verbatim rather than unpacked into
  // a host directory tree that would re-encode every name.
  bool romFsPresent{ false };
  quint64 romFsOffset{ 0 };
  quint64 romFsSize{ 0 };
  QByteArray romFsCounter;      // empty when the section is unencrypted
  quint64 romFsFileCount{ 0 };  // from its own file metadata table
  quint64 romFsFileBytes{ 0 };
  QStringList romFsRootEntries;  // what sits directly under the root, bounded

  // From the Control NCA's RomFS, when the package carries one. This is where a
  // Switch title's real name lives; the NSP file name is only ever a guess at
  // it.
  QString displayTitle;
  QString publisher;
  QString iconEntryName;
  QByteArray icon;  // the bytes of icon_<language>.dat, which is a JPEG
};

/* Walks the package. Returns false only when the file cannot be read as a
 * PFS0 at all; a package that reads fine but cannot be decrypted returns true
 * with `programDecrypted` false and `missingKeys` populated. */
bool readSwitchNsp(const QString& path, SwitchNspInfo& info, QString& error);

/* Writes the ExeFS `main` module to `destinationFile`. Requires a prior
 * `readSwitchNsp` that reached the second tier. */
bool extractSwitchNspMain(const QString& path,
                          const SwitchNspInfo& info,
                          const QString& destinationFile,
                          QString& error);

/* Writes every ExeFS entry into `destination` — `main`, `main.npdm`, `rtld`,
 * `sdk` and the subsdks. `main` is the module that is branched to, but the
 * others are the ones it resolves its imports against and the NPDM is the
 * process's own manifest, so a package that carries only `main` is missing the
 * program. Names are taken from the partition and are flat, so nothing is
 * created outside `destination`.
 *
 * `extracted` receives one entry name per file written. */
bool extractSwitchNspExeFs(const QString& path,
                           const SwitchNspInfo& info,
                           const QString& destination,
                           QStringList& extracted,
                           QString& error);

/* Writes the program's RomFS image to `destinationFile`, decrypted and
 * otherwise verbatim — the IVFC hash levels above it are left behind, so the
 * file begins at the RomFS header and is exactly what a `romfs:` mount reads.
 *
 * It is not unpacked into a directory tree on purpose: RomFS names are UTF-8
 * and case-sensitive, the host filesystem here is neither, and an unpack would
 * make the staged data a re-encoding of the game's rather than a copy of it. */
bool extractSwitchNspRomFs(const QString& path,
                           const SwitchNspInfo& info,
                           const QString& destinationFile,
                           QString& error);

/* Copies the package's plaintext entries — the ticket, the certificate and the
 * `.cnmt.xml` — into `destination`. Used to keep the export's evidence next to
 * the module it produced. */
bool extractSwitchNspPlaintext(const QString& path,
                               const SwitchNspInfo& info,
                               const QString& destination,
                               QStringList& extracted,
                               QString& error);

}  // namespace psxstudio
