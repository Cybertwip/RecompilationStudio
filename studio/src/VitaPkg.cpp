#include "VitaPkg.h"

#include "GuestCrypto.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace psxstudio {

namespace {

constexpr qint64 kHeaderSize = 0x100;
constexpr quint64 kMaxItems = 65536u;
constexpr qint64 kChunk = 1 << 20;

/* The four package keys. They are part of the format, not a secret: every PKG
 * reader carries the same constants, and the licence — the thing a package
 * genuinely does not include — is a separate layer above this one. Which key a
 * package uses is stated in its extended header at 0xE4. */
QByteArray packageKey(int keyType) {
  switch (keyType) {
    case 1: return QByteArray::fromHex("07f2c68290b50d2c33818d709b60e62b");
    case 2: return QByteArray::fromHex("e31a70c9ce1dd72bf3c0622963f2eccb");
    case 3: return QByteArray::fromHex("423aca3a2bd5649f9686abad6fd8801f");
    case 4: return QByteArray::fromHex("af07fd59652527baf13389668b17d9ea");
    default: break;
  }
  return {};
}

quint16 be16(const QByteArray& bytes, qsizetype at) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + at);
  return static_cast<quint16>((p[0] << 8u) | p[1]);
}

quint32 be32(const QByteArray& bytes, qsizetype at) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + at);
  return (static_cast<quint32>(p[0]) << 24u) | (static_cast<quint32>(p[1]) << 16u) |
         (static_cast<quint32>(p[2]) << 8u) | static_cast<quint32>(p[3]);
}

quint64 be64(const QByteArray& bytes, qsizetype at) {
  return (static_cast<quint64>(be32(bytes, at)) << 32u) | be32(bytes, at + 4);
}

quint16 le16(const QByteArray& bytes, qsizetype at) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + at);
  return static_cast<quint16>(p[0] | (p[1] << 8u));
}

quint32 le32(const QByteArray& bytes, qsizetype at) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + at);
  return static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8u) |
         (static_cast<quint32>(p[2]) << 16u) | (static_cast<quint32>(p[3]) << 24u);
}

/* One decrypted range out of the data area. Offsets are the ones the item table
 * uses: relative to the start of the encrypted data, which is where the CTR
 * counter is the package's pkg_data_riv. */
bool readDecrypted(QFile& file, const VitaPkgInfo& info, quint64 offset,
                   qint64 size, QByteArray& out) {
  out.clear();
  if (size < 0) return false;
  if (offset > info.dataSize || static_cast<quint64>(size) > info.dataSize - offset) {
    return false;
  }
  crypto::CtrStream stream(info.itemKey, info.itemCounter);
  if (!stream.isValid()) return false;
  stream.seek(offset);
  if (!file.seek(static_cast<qint64>(info.dataOffset + offset))) return false;
  out = file.read(size);
  if (out.size() != size) {
    out.clear();
    return false;
  }
  stream.apply(out.data(), out.size());
  return true;
}

/* The item table plus its name region, decrypted with `keyType`'s key, or an
 * empty list if that key does not produce a self-consistent table. This is the
 * check that says which key a package is under: a wrong key gives offsets that
 * leave the data area and names that are not names. */
bool decodeItems(QFile& file, VitaPkgInfo& info, quint32 itemCount,
                 quint64 tableOffset, QList<VitaPkgItem>& items) {
  items.clear();
  if (itemCount == 0u || itemCount > kMaxItems) return false;
  QByteArray table;
  if (!readDecrypted(file, info, tableOffset,
                     static_cast<qint64>(itemCount) * 32, table)) {
    return false;
  }
  QList<VitaPkgItem> decoded;
  decoded.reserve(static_cast<qsizetype>(itemCount));
  for (quint32 index = 0; index < itemCount; ++index) {
    const qsizetype at = static_cast<qsizetype>(index) * 32;
    const quint32 nameOffset = be32(table, at);
    const quint32 nameSize = be32(table, at + 4);
    VitaPkgItem item;
    item.offset = be64(table, at + 8);
    item.size = be64(table, at + 16);
    item.flags = be32(table, at + 24);
    if (nameSize == 0u || nameSize > 1024u) return false;
    if (nameOffset + nameSize > info.dataSize) return false;
    if (item.offset > info.dataSize || item.size > info.dataSize - item.offset) return false;
    QByteArray name;
    if (!readDecrypted(file, info, nameOffset, nameSize, name)) return false;
    for (const char byte : name) {
      const auto value = static_cast<uchar>(byte);
      if (value < 0x20u || value > 0x7Eu) return false;  // not a path
    }
    item.name = QString::fromLatin1(name);
    decoded.append(item);
  }
  items = decoded;
  return true;
}

/* sce_pfs/pflist, the package's own list of what it stores inside the PFS
 * layer. Tab-separated: path, ownership, mark, index, size, digest — where the
 * mark is `dir` for a directory, `nenc` for a file stored in the clear, and
 * empty for a file stored encrypted. */
void parsePfsList(const QByteArray& blob, QStringList& protectedFiles) {
  protectedFiles.clear();
  const QList<QByteArray> lines = blob.split('\n');
  for (const QByteArray& raw : lines) {
    const QByteArray line = raw.trimmed();
    if (line.isEmpty() || line.startsWith('#')) continue;
    const QList<QByteArray> columns = line.split('\t');
    if (columns.size() < 3) continue;
    const QByteArray mark = columns.at(2).trimmed();
    if (mark == "dir" || mark == "nenc") continue;
    if (!mark.isEmpty()) continue;  // an unfamiliar mark is not read as "encrypted"
    protectedFiles.append(QString::fromLatin1(columns.at(0)));
  }
}

}  // namespace

bool parseSonyPsf(const QByteArray& blob, QHash<QString, QString>& values) {
  values.clear();
  if (blob.size() < 20 || !blob.startsWith(QByteArrayLiteral("\0PSF"))) return false;
  const quint32 keyTable = le32(blob, 8);
  const quint32 dataTable = le32(blob, 12);
  const quint32 count = le32(blob, 16);
  if (count > 4096u) return false;
  const qsizetype entries = 20 + static_cast<qsizetype>(count) * 16;
  if (entries > blob.size() || keyTable > blob.size() || dataTable > blob.size()) {
    return false;
  }
  for (quint32 index = 0; index < count; ++index) {
    const qsizetype at = 20 + static_cast<qsizetype>(index) * 16;
    const quint32 keyOffset = le16(blob, at);
    const quint16 format = le16(blob, at + 2);
    const quint32 length = le32(blob, at + 4);
    const quint32 dataOffset = le32(blob, at + 12);
    const qsizetype keyAt = static_cast<qsizetype>(keyTable) + keyOffset;
    const qsizetype dataAt = static_cast<qsizetype>(dataTable) + dataOffset;
    if (keyAt >= blob.size() ||
        dataAt + static_cast<qsizetype>(length) > blob.size()) {
      continue;
    }
    const qsizetype end = blob.indexOf('\0', keyAt);
    if (end < 0) continue;
    const QString key = QString::fromLatin1(blob.mid(keyAt, end - keyAt));
    const QByteArray raw = blob.mid(dataAt, static_cast<qsizetype>(length));
    switch (format >> 8u) {
      case 0x02:  // UTF-8, NUL-terminated
        values.insert(key, QString::fromUtf8(raw).remove(QChar(u'\0')));
        break;
      case 0x04:  // u32
        if (raw.size() >= 4) values.insert(key, QString::number(le32(raw, 0)));
        break;
      default:  // 0x00: raw bytes; reported as hex rather than dropped
        values.insert(key, QString::fromLatin1(raw.toHex()));
        break;
    }
  }
  return true;
}

bool readVitaPkg(const QString& path, VitaPkgInfo& info, QString& error) {
  info = {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the package: %1").arg(file.errorString());
    return false;
  }
  const QByteArray header = file.read(kHeaderSize);
  if (header.size() < kHeaderSize ||
      !header.startsWith(QByteArrayLiteral("\x7f" "PKG"))) {
    error = QStringLiteral("This file does not begin with a PKG header.");
    return false;
  }
  info.packageType = be16(header, 0x06);
  const quint32 metaOffset = be32(header, 0x08);
  const quint32 metaCount = be32(header, 0x0C);
  const quint32 metaSize = be32(header, 0x10);
  const quint32 itemCount = be32(header, 0x14);
  info.dataOffset = be64(header, 0x20);
  info.dataSize = be64(header, 0x28);
  QByteArray contentId = header.mid(0x30, 0x30);
  const qsizetype contentIdEnd = contentId.indexOf('\0');
  if (contentIdEnd >= 0) contentId.truncate(contentIdEnd);
  info.contentId = QString::fromLatin1(contentId);
  info.itemCounter = header.mid(0x70, 0x10);
  info.keyType = static_cast<int>(be32(header, 0xE4) & 7u);

  const qint64 fileSize = file.size();
  if (info.dataOffset > static_cast<quint64>(fileSize) ||
      info.dataSize > static_cast<quint64>(fileSize) - info.dataOffset) {
    error = QStringLiteral(
      "The package header describes %1 bytes of data at 0x%2, which is past the "
      "end of a %3-byte file — it is truncated.")
        .arg(info.dataSize).arg(info.dataOffset, 0, 16).arg(fileSize);
    return false;
  }

  // Metadata sits in the clear between the header and the data area.
  quint64 tableOffset = 0;
  bool haveTableOffset = false;
  if (metaCount > 0u && metaCount < 256u && metaSize > 0u &&
      metaOffset + metaSize <= static_cast<quint64>(fileSize)) {
    if (!file.seek(metaOffset)) {
      error = QStringLiteral("The package metadata could not be read.");
      return false;
    }
    const QByteArray meta = file.read(std::min<qint64>(kChunk, metaSize));
    qsizetype at = 0;
    for (quint32 index = 0; index < metaCount && at + 8 <= meta.size(); ++index) {
      const quint32 type = be32(meta, at);
      const quint32 size = be32(meta, at + 4);
      if (at + 8 + static_cast<qsizetype>(size) > meta.size()) break;
      if (type == 2u && size >= 4u) info.contentType = be32(meta, at + 8);
      if (type == 13u && size >= 8u) {
        tableOffset = be32(meta, at + 8);
        haveTableOffset = true;
      }
      at += 8 + static_cast<qsizetype>(size);
    }
  }
  if (!haveTableOffset) {
    // Every package observed puts the table first; say so rather than treating
    // a missing metadata record as if it had said 0.
    error = QStringLiteral(
      "The package metadata does not say where its item table is (record 13 is "
      "missing), so its contents cannot be located.");
    return false;
  }

  // The declared key first; the other three only if it does not produce a
  // self-consistent table, so that a mislabelled package is still opened and
  // the key that worked is reported.
  QList<int> keyOrder{ info.keyType };
  for (const int candidate : { 1, 2, 3, 4 }) {
    if (!keyOrder.contains(candidate)) keyOrder.append(candidate);
  }
  QList<VitaPkgItem> items;
  int usedKeyType = 0;
  for (const int keyType : keyOrder) {
    const QByteArray key = packageKey(keyType);
    if (key.isEmpty()) continue;
    // Key types 2-4 derive the stream key from the package's own counter; type
    // 1 uses the key directly.
    info.itemKey = keyType == 1 ? key : crypto::aesEcbEncrypt(key, info.itemCounter);
    if (info.itemKey.size() != 16) continue;
    if (decodeItems(file, info, itemCount, tableOffset, items)) {
      usedKeyType = keyType;
      break;
    }
  }
  if (usedKeyType == 0) {
    error = QStringLiteral(
      "None of the four package keys decrypts this package's item table. Its "
      "header asks for key %1; either the file is damaged or it is a package "
      "format this reader does not know.")
        .arg(info.keyType);
    return false;
  }
  info.keyType = usedKeyType;
  info.items = items;

  // The two records the package keeps about itself.
  for (const VitaPkgItem& item : info.items) {
    if (item.isDirectory()) continue;
    if (item.name == QStringLiteral("sce_sys/param.sfo") && item.size <= 64 * 1024u) {
      QByteArray sfo;
      QHash<QString, QString> values;
      if (readDecrypted(file, info, item.offset, static_cast<qint64>(item.size), sfo) &&
          parseSonyPsf(sfo, values)) {
        info.title = values.value(QStringLiteral("TITLE"));
        info.titleId = values.value(QStringLiteral("TITLE_ID"));
        info.category = values.value(QStringLiteral("CATEGORY"));
        info.appVersion = values.value(QStringLiteral("APP_VER"));
      }
    } else if (item.name == QStringLiteral("sce_pfs/pflist") &&
               item.size <= 4 * 1024 * 1024u) {
      QByteArray list;
      if (readDecrypted(file, info, item.offset, static_cast<qint64>(item.size), list)) {
        parsePfsList(list, info.pfsProtectedFiles);
        info.havePfsList = true;
      }
    }
  }
  for (const VitaPkgItem& item : info.items) {
    if (!item.isDirectory() && item.name == QStringLiteral("eboot.bin")) {
      info.executableName = item.name;
      info.executableIsPfsProtected = info.isPfsProtected(item.name);
      break;
    }
  }
  return true;
}

bool readVitaPkgItem(const QString& path,
                     const VitaPkgInfo& info,
                     const VitaPkgItem& item,
                     qint64 maxSize,
                     QByteArray& out,
                     QString& error) {
  out.clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the package: %1").arg(file.errorString());
    return false;
  }
  const qint64 size = std::min<qint64>(maxSize, static_cast<qint64>(item.size));
  if (!readDecrypted(file, info, item.offset, size, out)) {
    error = QStringLiteral("%1 could not be read out of the package.").arg(item.name);
    return false;
  }
  return true;
}

bool extractVitaPkg(const QString& path,
                    const VitaPkgInfo& info,
                    const QString& destination,
                    const std::function<bool(const VitaPkgItem&)>& wanted,
                    QStringList& extracted,
                    QString& error) {
  extracted.clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the package: %1").arg(file.errorString());
    return false;
  }
  const QDir root(destination);
  if (!QDir().mkpath(destination)) {
    error = QStringLiteral("Could not create %1.").arg(destination);
    return false;
  }
  for (const VitaPkgItem& item : info.items) {
    // With a filter, the package's directory entries are skipped too: each
    // file creates the directories it needs, so the empty ones would be
    // structure the caller did not ask for.
    if (wanted && (item.isDirectory() || !wanted(item))) continue;
    // A package names its own paths; a name that climbs out of the destination
    // is refused rather than followed.
    const QString relative = QDir::cleanPath(item.name);
    if (relative.isEmpty() || relative.startsWith(QLatin1Char('/')) ||
        relative.startsWith(QStringLiteral("../")) ||
        relative.contains(QStringLiteral("/../"))) {
      error = QStringLiteral("The package contains an unusable path: %1").arg(item.name);
      return false;
    }
    const QString target = root.filePath(relative);
    if (item.isDirectory()) {
      if (!QDir().mkpath(target)) {
        error = QStringLiteral("Could not create %1.").arg(target);
        return false;
      }
      continue;
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
      error = QStringLiteral("Could not create %1.").arg(QFileInfo(target).absolutePath());
      return false;
    }
    if (item.offset > info.dataSize || item.size > info.dataSize - item.offset) {
      error = QStringLiteral("%1 lies outside the package's data area.").arg(item.name);
      return false;
    }
    crypto::CtrStream stream(info.itemKey, info.itemCounter);
    if (!stream.isValid()) {
      error = QStringLiteral("The package key could not be prepared.");
      return false;
    }
    stream.seek(item.offset);
    if (!file.seek(static_cast<qint64>(info.dataOffset + item.offset))) {
      error = QStringLiteral("Could not seek to %1 in the package.").arg(item.name);
      return false;
    }
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      error = QStringLiteral("Could not write %1: %2").arg(target, out.errorString());
      return false;
    }
    quint64 remaining = item.size;
    while (remaining > 0u) {
      QByteArray chunk = file.read(std::min<qint64>(kChunk, static_cast<qint64>(remaining)));
      if (chunk.isEmpty()) {
        error = QStringLiteral("The package ends inside %1.").arg(item.name);
        return false;
      }
      stream.apply(chunk.data(), chunk.size());
      if (out.write(chunk) != chunk.size()) {
        error = QStringLiteral("Could not write %1: %2").arg(target, out.errorString());
        return false;
      }
      remaining -= static_cast<quint64>(chunk.size());
    }
    if (!out.flush()) {
      error = QStringLiteral("Could not write %1: %2").arg(target, out.errorString());
      return false;
    }
    out.close();
    extracted.append(relative);
  }
  return true;
}

}  // namespace psxstudio
