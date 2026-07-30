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
//     section table, and the ExeFS partition (AES-CTR under the title key from
//     the ticket, or the NCA's own key area), from which `main` — the NSO0
//     module horizon2mvii loads — is read out.
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

  // What `extractSwitchNspMain` needs to read the module out again.
  quint64 programNcaOffset{ 0 };  // the NCA's offset in the NSP file
  quint64 mainOffset{ 0 };        // `main`'s offset within the NCA
  quint64 mainSize{ 0 };
  QByteArray sectionKey;      // AES-128-CTR key for the ExeFS section
  QByteArray sectionCounter;  // its 16-byte counter at NCA offset 0
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

/* Copies the package's plaintext entries — the ticket, the certificate and the
 * `.cnmt.xml` — into `destination`. Used to keep the export's evidence next to
 * the module it produced. */
bool extractSwitchNspPlaintext(const QString& path,
                               const SwitchNspInfo& info,
                               const QString& destination,
                               QStringList& extracted,
                               QString& error);

}  // namespace psxstudio
