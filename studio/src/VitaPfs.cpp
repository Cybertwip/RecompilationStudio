#include "VitaPfs.h"

#include "GuestCrypto.h"
#include "VitaPkg.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>

#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// psvpfstools, compiled in from extra/vita2hos (see studio/CMakeLists.txt).
// These are the only headers Studio reaches into; everything below the
// PfsFilesystem entry point is the reference implementation's own.
#include <zRIF/licdec.h>
#include <zRIF/rif.h>

#include "F00DKeyEncryptorFactory.h"
#include "FilesDbParser.h"
#include "FlagOperations.h"
#include "ICryptoOperations.h"
#include "PfsFilesystem.h"
#include "SecretGenerator.h"

namespace psxstudio {

namespace {

constexpr int kMaxTitleEntries = 4096;

// ── the ten primitives psvpfsparser asks for ───────────────────────────────
//
// The reference implementation ships an OpenSSL-backed version of this. Studio
// already has AES and Qt already has the digests, so this supplies the same
// interface from those and keeps OpenSSL out of the build. The contract is
// copied from OpenSSLCryptoOperations, including the parts that are easy to get
// wrong: `key_size` is in *bits* for the ciphers and the CMAC but in *bytes*
// for the HMACs, CBC leaves the last ciphertext block in `iv` so consecutive
// calls chain, and CTR advances `iv` by the number of blocks it consumed.
class StudioCryptoOperations final : public ICryptoOperations {
 public:
  int aes_cbc_encrypt(const unsigned char* src, unsigned char* dst, int size,
                      const unsigned char* key, int key_size,
                      unsigned char* iv) const override {
    if (size == 0) return 0;
    const QByteArray k = cipherKey(key, key_size);
    if (k.isEmpty()) return -1;
    return crypto::aesCbcEncrypt(k, src, dst, size, iv) ? 0 : -1;
  }

  int aes_cbc_decrypt(const unsigned char* src, unsigned char* dst, int size,
                      const unsigned char* key, int key_size,
                      unsigned char* iv) const override {
    if (size == 0) return 0;
    const QByteArray k = cipherKey(key, key_size);
    if (k.isEmpty()) return -1;
    return crypto::aesCbcDecrypt(k, src, dst, size, iv) ? 0 : -1;
  }

  int aes_ctr_encrypt(const unsigned char* src, unsigned char* dst, int size,
                      const unsigned char* key, int key_size,
                      unsigned char* iv) override {
    return ctr(src, dst, size, key, key_size, iv);
  }

  int aes_ctr_decrypt(const unsigned char* src, unsigned char* dst, int size,
                      const unsigned char* key, int key_size,
                      unsigned char* iv) override {
    return ctr(src, dst, size, key, key_size, iv);
  }

  int aes_ecb_encrypt(const unsigned char* src, unsigned char* dst, int size,
                      const unsigned char* key, int key_size) const override {
    return ecb(src, dst, size, key, key_size, true);
  }

  int aes_ecb_decrypt(const unsigned char* src, unsigned char* dst, int size,
                      const unsigned char* key, int key_size) const override {
    return ecb(src, dst, size, key, key_size, false);
  }

  int aes_cmac(const unsigned char* src, unsigned char* dst, int size,
               const unsigned char* key, int key_size) const override {
    const QByteArray k = cipherKey(key, key_size);
    if (k.isEmpty()) return -1;
    const QByteArray mac =
      crypto::aesCmac(k, reinterpret_cast<const char*>(src), size);
    if (mac.size() != 16) return -1;
    std::memcpy(dst, mac.constData(), 16);
    return 0;
  }

  int sha1(const unsigned char* src, unsigned char* dst, int size) const override {
    return digest(QCryptographicHash::Sha1, src, dst, size, 20);
  }

  int sha256(const unsigned char* src, unsigned char* dst, int size) const override {
    return digest(QCryptographicHash::Sha256, src, dst, size, 32);
  }

  int hmac_sha1(const unsigned char* src, unsigned char* dst, int size,
                const unsigned char* key, int key_size) const override {
    return hmac(QCryptographicHash::Sha1, src, dst, size, key, key_size, 20);
  }

  int hmac_sha256(const unsigned char* src, unsigned char* dst, int size,
                  const unsigned char* key, int key_size) const override {
    return hmac(QCryptographicHash::Sha256, src, dst, size, key, key_size, 32);
  }

 private:
  static QByteArray cipherKey(const unsigned char* key, int keySizeBits) {
    if (keySizeBits != 128 && keySizeBits != 256) return {};
    return QByteArray(reinterpret_cast<const char*>(key), keySizeBits / 8);
  }

  static int ecb(const unsigned char* src, unsigned char* dst, int size,
                 const unsigned char* key, int keySizeBits, bool encrypt) {
    const QByteArray k = cipherKey(key, keySizeBits);
    if (k.isEmpty() || size < 0 || size % 16 != 0) return -1;
    const crypto::Aes aes(k);
    if (!aes.isValid()) return -1;
    for (int at = 0; at < size; at += 16) {
      if (encrypt)
        aes.encryptBlock(src + at, dst + at);
      else
        aes.decryptBlock(src + at, dst + at);
    }
    return 0;
  }

  static int ctr(const unsigned char* src, unsigned char* dst, int size,
                 const unsigned char* key, int keySizeBits, unsigned char* iv) {
    const QByteArray k = cipherKey(key, keySizeBits);
    if (k.isEmpty() || size < 0) return -1;
    crypto::CtrStream stream(
      k, QByteArray(reinterpret_cast<const char*>(iv), 16));
    if (!stream.isValid()) return -1;
    if (src != dst) std::memcpy(dst, src, static_cast<size_t>(size));
    stream.apply(reinterpret_cast<char*>(dst), size);

    // The caller's counter advances by the blocks consumed, as a 128-bit
    // big-endian integer. A partial trailing block still costs a whole one.
    quint64 blocks = static_cast<quint64>(size) / 16u;
    for (int i = 15; i >= 0; --i) {
      blocks += iv[i];
      iv[i] = static_cast<unsigned char>(blocks & 0xFFu);
      blocks >>= 8u;
    }
    return 0;
  }

  static int digest(QCryptographicHash::Algorithm algorithm,
                    const unsigned char* src, unsigned char* dst, int size,
                    int length) {
    if (size < 0) return -1;
    QCryptographicHash hash(algorithm);
    hash.addData(QByteArrayView(src, size));
    const QByteArray result = hash.result();
    if (result.size() != length) return -1;
    std::memcpy(dst, result.constData(), static_cast<size_t>(length));
    return 0;
  }

  static int hmac(QCryptographicHash::Algorithm algorithm,
                  const unsigned char* src, unsigned char* dst, int size,
                  const unsigned char* key, int keySizeBytes, int length) {
    if (size < 0 || keySizeBytes < 0) return -1;
    QMessageAuthenticationCode code(
      algorithm, QByteArray(reinterpret_cast<const char*>(key), keySizeBytes));
    code.addData(QByteArrayView(src, size));
    const QByteArray result = code.result();
    if (result.size() != length) return -1;
    std::memcpy(dst, result.constData(), static_cast<size_t>(length));
    return 0;
  }
};

std::shared_ptr<ICryptoOperations> makeCryptoOperations() {
  return std::make_shared<StudioCryptoOperations>();
}

std::filesystem::path toStdPath(const QString& path) {
  return std::filesystem::path(path.toStdString());
}

/* psvpfsparser reports to a std::ostream, and its key-file helpers report to
 * std::cout directly. Both are captured here and handed back as log lines: a
 * GUI has no console, and rule 3 of this project is that nothing prints. */
class CapturedOutput {
 public:
  CapturedOutput() : previous_(std::cout.rdbuf(stream_.rdbuf())) {}
  ~CapturedOutput() { std::cout.rdbuf(previous_); }

  CapturedOutput(const CapturedOutput&) = delete;
  CapturedOutput& operator=(const CapturedOutput&) = delete;

  std::ostream& stream() { return stream_; }

  QStringList lines() const {
    QStringList out;
    const QString text = QString::fromStdString(stream_.str());
    for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
      out.append(line.trimmed());
    return out;
  }

 private:
  std::ostringstream stream_;
  std::streambuf* previous_;
};

/* The last few lines of psvpfsparser's own output, which is where it says what
 * it refused. Used to turn a `-1` into a sentence instead of a number. */
QString tailOf(const QStringList& lines, int count = 3) {
  if (lines.isEmpty()) return {};
  return lines.mid(qMax(0, lines.size() - count)).join(QStringLiteral("; "));
}

void licenseFromRif(const SceNpDrmLicense& rif, const QString& source,
                    VitaLicense& license) {
  license.klicensee = QByteArray(reinterpret_cast<const char*>(rif.key), 0x10);
  license.source = source;
  license.contentId =
    QString::fromLatin1(rif.content_id, qstrnlen(rif.content_id, sizeof(rif.content_id)));
  license.accountId = rif.aid;
  license.licenseType = rif.type;
  license.fakeAccount = rif.aid == static_cast<quint64>(FAKE_AID);
}

}  // namespace

bool inspectVitaTitleDirectory(const QString& directory,
                               VitaTitleDirectory& info,
                               QString& error) {
  info = VitaTitleDirectory{};
  error.clear();

  const QDir root(directory);
  if (!root.exists()) {
    error = QStringLiteral("%1 is not a directory.").arg(directory);
    return false;
  }
  info.path = root.absolutePath();

  // param.sfo is what makes a directory a title rather than a directory that
  // happens to hold an eboot.bin.
  const QString sfoPath = root.filePath(QStringLiteral("sce_sys/param.sfo"));
  QFile sfo(sfoPath);
  if (!sfo.open(QIODevice::ReadOnly)) {
    error = QStringLiteral(
      "%1 has no sce_sys/param.sfo, so it is not an unpacked PSVita title.")
        .arg(directory);
    return false;
  }
  QHash<QString, QString> values;
  if (!parseSonyPsf(sfo.readAll(), values)) {
    error = QStringLiteral("%1 could not be read as a param.sfo.").arg(sfoPath);
    return false;
  }
  info.titleId = values.value(QStringLiteral("TITLE_ID"));
  info.title = values.value(QStringLiteral("TITLE"));
  info.category = values.value(QStringLiteral("CATEGORY"));
  info.appVersion = values.value(QStringLiteral("APP_VER"));

  // The PFS metadata. files.db is the file tree and its signature; the hash
  // database beside it is unicv.db for an application and an icv.db directory
  // for savedata.
  const bool filesDb = QFileInfo(root.filePath(QStringLiteral("sce_pfs/files.db"))).isFile();
  info.haveUnicv = QFileInfo(root.filePath(QStringLiteral("sce_pfs/unicv.db"))).isFile();
  const bool icvDb = QFileInfo(root.filePath(QStringLiteral("sce_pfs/icv.db"))).isDir();
  info.havePfs = filesDb && (info.haveUnicv || icvDb);
  if (filesDb && !info.havePfs) {
    info.notes.append(QStringLiteral(
      "sce_pfs/files.db is present but neither unicv.db nor an icv.db "
      "directory is, so the hash database this title is checked against is "
      "missing."));
  }

  // The licence, in the forms a dumped title carries it. Most specific first:
  // work.bin is the one that belongs to this title by construction.
  const QStringList candidates = {
    QStringLiteral("sce_sys/package/work.bin"),
    QStringLiteral("sce_sys/package/temp.bin"),
  };
  for (const QString& relative : candidates) {
    if (QFileInfo(root.filePath(relative)).isFile())
      info.licenseCandidates.append(relative);
  }
  const QStringList rifs =
    root.entryList({ QStringLiteral("*.rif") }, QDir::Files, QDir::Name);
  for (const QString& name : rifs) info.licenseCandidates.append(name);

  // And the contents, bounded: this is for a description, not an index.
  QDirIterator iterator(info.path, QDir::Files | QDir::Hidden,
                        QDirIterator::Subdirectories);
  int seen = 0;
  while (iterator.hasNext()) {
    const QString absolute = iterator.next();
    const QString relative = root.relativeFilePath(absolute);
    if (relative.startsWith(QLatin1Char('.'))) continue;
    info.totalBytes += static_cast<quint64>(QFileInfo(absolute).size());
    if (++seen <= kMaxTitleEntries) info.files.append(relative);
  }
  info.files.sort();
  if (seen > kMaxTitleEntries) {
    info.notes.append(QStringLiteral("%1 further file(s) are present and were "
                                     "counted but not listed.")
                        .arg(seen - kMaxTitleEntries));
  }

  if (QFileInfo(root.filePath(QStringLiteral("eboot.bin"))).isFile())
    info.executableName = QStringLiteral("eboot.bin");

  if (info.executableName.isEmpty()) {
    error = QStringLiteral("%1 has a param.sfo but no eboot.bin.").arg(directory);
    return false;
  }
  return true;
}

bool readVitaLicenseFile(const QString& path, VitaLicense& license, QString& error) {
  license = VitaLicense{};
  error.clear();

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("%1 could not be opened.").arg(path);
    return false;
  }
  return readVitaLicenseBlob(file.read(SceNpDrmLicenseSize), path, license, error);
}

bool readVitaLicenseBlob(const QByteArray& blob,
                         const QString& source,
                         VitaLicense& license,
                         QString& error) {
  license = VitaLicense{};
  error.clear();

  if (blob.size() != static_cast<qsizetype>(SceNpDrmLicenseSize)) {
    error = QStringLiteral(
      "%1 is %2 bytes; a SceNpDrmLicense is %3.")
        .arg(source).arg(blob.size()).arg(SceNpDrmLicenseSize);
    return false;
  }

  SceNpDrmLicense rif{};
  std::memcpy(&rif, blob.constData(), sizeof(rif));

  // A package's own `temp.bin` is a placeholder rather than a licence: it is
  // written with no account and a version of 0xFFFF, and its key field is
  // zero. Refusing it by name beats deriving a secret from sixteen zeroes and
  // reporting a header mismatch three steps later.
  if (rif.version == 0xFFFF && rif.aid == 0) {
    error = QStringLiteral(
      "%1 is a licence placeholder, not a licence: it carries no account and "
      "no key. A dumped title's real licence is sce_sys/package/work.bin, a "
      ".rif, or a zRIF string.").arg(source);
    return false;
  }

  licenseFromRif(rif, source, license);
  if (license.klicensee.count('\0') == license.klicensee.size()) {
    error = QStringLiteral("%1 carries an all-zero key, which is not a licence.")
              .arg(source);
    license = VitaLicense{};
    return false;
  }
  return true;
}

bool readVitaZrifLicense(const QString& zrif, VitaLicense& license, QString& error) {
  license = VitaLicense{};
  error.clear();

  const QString trimmed = zrif.trimmed();
  if (trimmed.isEmpty()) {
    error = QStringLiteral("No zRIF string was given.");
    return false;
  }

  std::shared_ptr<SceNpDrmLicense> decoded;
  CapturedOutput output;
  try {
    decoded = decode_license_np(trimmed.toStdString());
  } catch (const std::exception& failure) {
    error = QStringLiteral("The zRIF string could not be decoded: %1")
              .arg(QString::fromUtf8(failure.what()));
    return false;
  }
  if (!decoded) {
    const QString detail = tailOf(output.lines());
    error = detail.isEmpty()
      ? QStringLiteral("The zRIF string could not be decoded.")
      : QStringLiteral("The zRIF string could not be decoded: %1").arg(detail);
    return false;
  }

  licenseFromRif(*decoded, QStringLiteral("zRIF string"), license);
  return true;
}

bool findVitaLicense(const VitaTitleDirectory& info, VitaLicense& license, QString& error) {
  license = VitaLicense{};
  error.clear();

  if (info.licenseCandidates.isEmpty()) {
    error = QStringLiteral(
      "%1 carries no licence. A PSVita title's files are encrypted under a "
      "key that is unique to the title and ships only with its licence "
      "(sce_sys/package/work.bin, a .rif, or a zRIF string); it is not in the "
      "title and cannot be derived from it.").arg(info.path);
    return false;
  }

  QStringList refusals;
  for (const QString& relative : info.licenseCandidates) {
    QString why;
    if (readVitaLicenseFile(QDir(info.path).filePath(relative), license, why)) {
      license.source = relative;
      return true;
    }
    refusals.append(why);
  }
  error = QStringLiteral("None of the licence files in %1 could be read: %2")
            .arg(info.path, refusals.join(QStringLiteral(" ")));
  return false;
}

bool verifyVitaLicense(const QString& titleDirectory,
                       const VitaLicense& license,
                       QString& error) {
  error.clear();

  const QString filesDbPath =
    QDir(titleDirectory).filePath(QStringLiteral("sce_pfs/files.db"));
  QFile filesDb(filesDbPath);
  if (!filesDb.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("%1 could not be opened.").arg(filesDbPath);
    return false;
  }
  const QByteArray head = filesDb.read(sizeof(sce_ng_pfs_header_t));
  if (head.size() != static_cast<qsizetype>(sizeof(sce_ng_pfs_header_t))) {
    error = QStringLiteral("%1 is too short to be a files.db.").arg(filesDbPath);
    return false;
  }
  return verifyVitaLicenseHeader(head, license, error);
}

bool verifyVitaLicenseHeader(const QByteArray& filesDbHeader,
                             const VitaLicense& license,
                             QString& error) {
  error.clear();
  if (!license.isValid()) {
    error = QStringLiteral("No licence was supplied.");
    return false;
  }
  const QByteArray head = filesDbHeader;
  if (head.size() < static_cast<qsizetype>(sizeof(sce_ng_pfs_header_t))) {
    error = QStringLiteral(
      "The files.db header is %1 bytes; it is %2.")
        .arg(head.size()).arg(sizeof(sce_ng_pfs_header_t));
    return false;
  }

  sce_ng_pfs_header_t header{};
  std::memcpy(&header, head.constData(), sizeof(header));
  if (std::memcmp(header.magic, MAGIC_WORD, 8) != 0) {
    error = QStringLiteral("This is not a files.db: its magic is %1, not %2.")
              .arg(QString::fromLatin1(head.left(8).toHex(' ')),
                   QStringLiteral(MAGIC_WORD));
    return false;
  }

  // The secret the header is signed with, derived from the klicensee and the
  // title's own salt — which is why a licence for another title fails here.
  auto cryptops = makeCryptoOperations();
  auto f00d = F00DKeyEncryptorFactory::create(F00DEncryptorTypes::native, cryptops);
  unsigned char secret[0x14] = {};
  CapturedOutput output;
  try {
    if (scePfsUtilGetSecret(
          cryptops, f00d, secret,
          reinterpret_cast<const unsigned char*>(license.klicensee.constData()),
          header.files_salt, img_spec_to_crypto_engine_flag(header.image_spec),
          0, 0) < 0) {
      error = QStringLiteral("The licence's secret could not be derived. %1")
                .arg(tailOf(output.lines()));
      return false;
    }
  } catch (const std::exception& failure) {
    error = QStringLiteral("The licence's secret could not be derived: %1")
              .arg(QString::fromUtf8(failure.what()));
    return false;
  }

  // files.db signs the first 0x160 bytes of its own header with the signature
  // fields zeroed. Recompute over a copy so the file is never modified.
  QByteArray signed_ = head.left(0x160);
  std::memset(signed_.data() + offsetof(sce_ng_pfs_header_t, header_icv), 0, 0x14);
  std::memset(signed_.data() + offsetof(sce_ng_pfs_header_t, rsa_sig0), 0,
              0x160 - offsetof(sce_ng_pfs_header_t, rsa_sig0));

  QMessageAuthenticationCode code(
    QCryptographicHash::Sha1,
    QByteArray(reinterpret_cast<const char*>(secret), 0x14));
  code.addData(signed_);
  const QByteArray computed = code.result();
  const QByteArray stated(reinterpret_cast<const char*>(header.header_icv), 0x14);
  if (computed != stated) {
    error = QStringLiteral(
      "The licence does not belong to this title. files.db signs its header "
      "as %1; this licence derives %2.")
        .arg(QString::fromLatin1(stated.toHex()),
             QString::fromLatin1(computed.toHex()));
    return false;
  }
  return true;
}

bool decryptVitaTitle(const QString& titleDirectory,
                      const VitaLicense& license,
                      const QString& destination,
                      QStringList& log,
                      QString& error) {
  log.clear();
  error.clear();

  if (!license.isValid()) {
    error = QStringLiteral("No licence was supplied.");
    return false;
  }
  if (!verifyVitaLicense(titleDirectory, license, error)) return false;
  if (!QDir().mkpath(destination)) {
    error = QStringLiteral("Could not create %1.").arg(destination);
    return false;
  }

  // PfsFilesystem keeps a reference to the source path rather than a copy, so
  // this has to outlive it.
  const std::filesystem::path source = toStdPath(QDir(titleDirectory).absolutePath());
  const std::filesystem::path target = toStdPath(QDir(destination).absolutePath());

  CapturedOutput output;
  int mounted = -1;
  int decrypted = -1;
  try {
    auto cryptops = makeCryptoOperations();
    auto f00d = F00DKeyEncryptorFactory::create(F00DEncryptorTypes::native, cryptops);
    PfsFilesystem pfs(
      cryptops, f00d, output.stream(),
      reinterpret_cast<const unsigned char*>(license.klicensee.constData()),
      source);
    mounted = pfs.mount();
    if (mounted >= 0) decrypted = pfs.decrypt_files(target);
  } catch (const std::exception& failure) {
    log = output.lines();
    error = QStringLiteral("The PFS layer of %1 could not be opened: %2")
              .arg(titleDirectory, QString::fromUtf8(failure.what()));
    return false;
  }

  log = output.lines();
  if (mounted < 0) {
    error = QStringLiteral("The PFS layer of %1 could not be mounted. %2")
              .arg(titleDirectory, tailOf(log));
    return false;
  }
  if (decrypted < 0) {
    error = QStringLiteral("The PFS layer of %1 mounted but did not decrypt. %2")
              .arg(titleDirectory, tailOf(log));
    return false;
  }
  return true;
}

}  // namespace psxstudio
