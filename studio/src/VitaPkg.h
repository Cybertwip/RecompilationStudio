#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace psxstudio {

// PSN package (PKG) reader.
//
// A PKG is a directory of items behind one AES-128-CTR stream whose key is
// derived from the package's own `pkg_data_riv` and one of four keys that are
// public and fixed in the format — nothing here needs a licence. That layer is
// what this file opens: the item table, the names, the file data, param.sfo.
//
// PSVita packages then put the *application's* files behind a second layer,
// PFS, whose key does come from a licence (the zRIF / work.bin) that a package
// does not contain. The package says so itself, in `sce_pfs/pflist`: every file
// it stores encrypted is listed there without the `nenc` mark. That list is
// read rather than inferred, so a package whose executable is reachable is
// opened and one whose executable is not is refused by name — instead of both
// being refused because the container is "encrypted".

struct VitaPkgItem {
  QString name;
  quint64 offset{ 0 };  // within the package's encrypted data area
  quint64 size{ 0 };
  quint32 flags{ 0 };

  quint32 kind() const { return flags & 0xFFu; }
  bool isDirectory() const { return kind() == 4u || kind() == 18u; }
};

struct VitaPkgInfo {
  quint16 packageType{ 0 };   // header 0x06: 1 = PS3/PSP, 2 = PSVita
  quint32 contentType{ 0 };   // metadata 2: 0x15 game, 0x16 DLC, 0x1F PSM, …
  int keyType{ 0 };           // extended header 0xE4 & 7
  QString contentId;
  // sce_sys/param.sfo, when the package carries one.
  QString titleId, title, category, appVersion;
  QList<VitaPkgItem> items;

  // sce_pfs/pflist: the package's own record of what it stores inside the PFS
  // layer. Empty (with havePfsList false) when there is no pflist, which is
  // what a package with no PFS layer looks like.
  bool havePfsList{ false };
  QStringList pfsProtectedFiles;

  QString executableName;                // "eboot.bin" when present
  bool executableIsPfsProtected{ false };

  // Enough to decrypt the data area again without re-deriving it.
  quint64 dataOffset{ 0 }, dataSize{ 0 };
  QByteArray itemKey;      // AES-128-CTR key
  QByteArray itemCounter;  // the counter at data-area offset 0 (pkg_data_riv)

  bool isPfsProtected(const QString& name) const {
    return pfsProtectedFiles.contains(name);
  }
};

/* Reads the header, the item table and the small metadata items (param.sfo,
 * pflist). Nothing large is read and nothing is written. */
bool readVitaPkg(const QString& path, VitaPkgInfo& info, QString& error);

/* Decrypts up to `maxSize` bytes of one item. */
bool readVitaPkgItem(const QString& path,
                     const VitaPkgInfo& info,
                     const VitaPkgItem& item,
                     qint64 maxSize,
                     QByteArray& out,
                     QString& error);

/* Decrypts the package into `destination`, recreating its directory layout.
 * `wanted` selects the items to write; a default-constructed one writes them
 * all. `extracted` receives the relative paths written, in package order.
 *
 * Files the package stores inside the PFS layer are written out as they are —
 * still PFS ciphertext — because that is what the package holds; `readVitaPkg`
 * is what says which those are. */
bool extractVitaPkg(const QString& path,
                    const VitaPkgInfo& info,
                    const QString& destination,
                    const std::function<bool(const VitaPkgItem&)>& wanted,
                    QStringList& extracted,
                    QString& error);

/* Sony's PSF key/value blob (param.sfo). Values are returned as text for the
 * UTF-8 formats and as decimal for the integer format. */
bool parseSonyPsf(const QByteArray& blob, QHash<QString, QString>& values);

}  // namespace psxstudio
