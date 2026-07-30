#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace psxstudio {

// PSVita PFS layer: the title's own files, and the per-title licence that opens
// them.
//
// A PSN package's outer AES-CTR layer (VitaPkg.h) is keyed by the format and
// opens with no licence at all. Everything the *application* consists of —
// eboot.bin, the suprx modules, the asset archives — sits behind a second
// layer, PFS, whose key is derived from a 16-byte klicensee that ships with the
// title's licence and with nothing else. There is one klicensee per title: it
// is not a global key, cannot be recovered from the package, and a title
// without its licence stays closed.
//
// Studio takes the licence from any of the three forms it is distributed in:
//
//   * `sce_sys/package/work.bin` — the NoNpDrm licence a dumped title carries
//     beside itself. This is the form in branding/vita/standard/PCSE00317.
//   * a `.rif` file — the same 0x200-byte SceNpDrmLicense structure.
//   * a zRIF string — that structure deflated and base64'd, which is how
//     licences are passed around as text.
//
// The klicensee is then checked against the title before anything is decrypted:
// files.db signs its own header with an HMAC-SHA1 under a secret derived from
// the klicensee, so a licence that belongs to a different title fails at the
// header and is reported as the wrong licence — rather than producing 26 MB of
// plausible-looking garbage that only fails on the device.
//
// The format itself is not reimplemented here. psvpfstools is the reference
// implementation and it is already in this repository (a nested submodule of
// the vita2hos front-end); Studio compiles it in and supplies the ten crypto
// primitives it asks for from src/GuestCrypto.h, so no OpenSSL is pulled in.

struct VitaLicense {
  QByteArray klicensee;  // 16 bytes; empty means no licence was found
  QString source;        // the file or "zRIF string" it came from
  QString contentId;     // as the licence states it, e.g. UP4040-PCSE00317_00-…
  quint64 accountId{ 0 };
  quint16 licenseType{ 0 };
  bool fakeAccount{ false };  // the NoNpDrm marker account, not a PSN one

  bool isValid() const { return klicensee.size() == 16; }
};

// What an unpacked title directory holds, before anything is decrypted. This is
// the `PCSE00317/` form: a dumped application, still PFS-encrypted, with its
// licence and its sce_pfs metadata beside it.
struct VitaTitleDirectory {
  QString path;
  QString titleId, title, category, appVersion;  // from sce_sys/param.sfo
  QStringList files;                             // relative, sorted, bounded
  quint64 totalBytes{ 0 };

  bool havePfs{ false };       // sce_pfs/files.db and unicv.db or icv.db
  bool haveUnicv{ false };     // unicv.db (a game) rather than icv.db (savedata)
  QString executableName;      // "eboot.bin" when the directory carries one
  QStringList licenseCandidates;  // licence files found, most specific first
  QStringList notes;
};

/* Reads a directory as an unpacked PSVita title. False means it is not one —
 * `error` says what was missing. */
bool inspectVitaTitleDirectory(const QString& directory,
                               VitaTitleDirectory& info,
                               QString& error);

/* Reads a 0x200-byte SceNpDrmLicense: work.bin, or a .rif. */
bool readVitaLicenseFile(const QString& path, VitaLicense& license, QString& error);

/* Decodes a zRIF string (base64 of a deflated SceNpDrmLicense). */
bool readVitaZrifLicense(const QString& zrif, VitaLicense& license, QString& error);

/* Takes the first licence in `info.licenseCandidates` that reads. False when
 * the title carries none, which is a title that cannot be opened rather than a
 * malformed one. */
bool findVitaLicense(const VitaTitleDirectory& info, VitaLicense& license, QString& error);

/* Checks a klicensee against the title itself, by recomputing the HMAC-SHA1
 * files.db signs its own header with. Cheap — it reads 0x400 bytes and derives
 * one key — and it is the difference between "this licence opens this title"
 * and "this licence is 16 bytes long". */
bool verifyVitaLicense(const QString& titleDirectory,
                       const VitaLicense& license,
                       QString& error);

/* Mounts the PFS layer and writes the decrypted title into `destination`,
 * recreating its directory layout. `log` receives psvpfsparser's own account of
 * what it mounted and decrypted, one line per entry. */
bool decryptVitaTitle(const QString& titleDirectory,
                      const VitaLicense& license,
                      const QString& destination,
                      QStringList& log,
                      QString& error);

}  // namespace psxstudio
