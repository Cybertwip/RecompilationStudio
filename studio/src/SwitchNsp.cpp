#include "SwitchNsp.h"

#include "GuestCrypto.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QXmlStreamReader>

#include <algorithm>
#include <functional>

namespace psxstudio {

namespace {

constexpr quint32 kPfs0Magic = 0x30534650u;  // 'PFS0'
constexpr qint64 kNcaHeaderSize = 0xC00;
constexpr qint64 kNcaSector = 0x200;
constexpr qint64 kChunk = 1 << 20;
constexpr quint32 kMaxPfs0Entries = 8192u;
constexpr int kIvfcMaxLevel = 6;
constexpr quint32 kRomFsMaxNameSize = 0x300u;
constexpr int kRomFsListLimit = 64;
// The metadata tables of a RomFS are proportional to its entry count, not to
// its data: 10 KB for UNDERTALE's 216 files. This bound is what makes reading
// them into memory safe on a package whose header is damaged or hostile.
constexpr qint64 kRomFsMetaLimit = 64 << 20;
constexpr qint64 kControlNacpLimit = 1 << 20;
constexpr qint64 kControlIconLimit = 4 << 20;

quint32 le32(const QByteArray& bytes, qsizetype at) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + at);
  return static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8u) |
         (static_cast<quint32>(p[2]) << 16u) | (static_cast<quint32>(p[3]) << 24u);
}

quint64 le64(const QByteArray& bytes, qsizetype at) {
  return static_cast<quint64>(le32(bytes, at)) |
         (static_cast<quint64>(le32(bytes, at + 4)) << 32u);
}

// A range of bytes out of something, already decrypted if it needed to be.
using RangeReader = std::function<bool(quint64 offset, qint64 size, QByteArray& out)>;

/* PFS0's own table: 'PFS0', a u32 entry count and a u32 string-table size,
 * then `count` 0x18-byte entries {offset u64, size u64, name offset u32, pad},
 * then the string table, then the data the offsets are relative to. */
bool parsePfs0(const RangeReader& read, quint64 base, QList<Pfs0Entry>& entries,
               QString& error) {
  entries.clear();
  QByteArray header;
  if (!read(base, 0x10, header) || header.size() < 0x10) {
    error = QStringLiteral("The PFS0 header could not be read.");
    return false;
  }
  if (le32(header, 0) != kPfs0Magic) {
    error = QStringLiteral("This is not a PFS0 archive.");
    return false;
  }
  const quint32 count = le32(header, 4);
  const quint32 stringTableSize = le32(header, 8);
  if (count == 0u || count > kMaxPfs0Entries || stringTableSize > (1u << 20u)) {
    error = QStringLiteral("The PFS0 table is not usable: %1 entries, %2-byte names.")
              .arg(count).arg(stringTableSize);
    return false;
  }
  const qint64 tableSize = static_cast<qint64>(count) * 0x18;
  QByteArray table;
  if (!read(base + 0x10, tableSize + stringTableSize, table) ||
      table.size() != tableSize + stringTableSize) {
    error = QStringLiteral("The PFS0 table is truncated.");
    return false;
  }
  const quint64 dataBase =
    base + 0x10 + static_cast<quint64>(tableSize) + stringTableSize;
  for (quint32 index = 0; index < count; ++index) {
    const qsizetype at = static_cast<qsizetype>(index) * 0x18;
    Pfs0Entry entry;
    entry.offset = dataBase + le64(table, at);
    entry.size = le64(table, at + 8);
    const quint32 nameOffset = le32(table, at + 0x10);
    if (nameOffset >= stringTableSize) continue;
    const qsizetype nameAt = tableSize + static_cast<qsizetype>(nameOffset);
    qsizetype end = table.indexOf('\0', nameAt);
    if (end < 0) end = table.size();
    entry.name = QString::fromUtf8(table.mid(nameAt, end - nameAt));
    entries.append(entry);
  }
  return true;
}

/* `name = hexadecimal` lines, which is the one format every Switch key dump
 * uses. Comments and anything that is not a hex value are skipped rather than
 * failing the file: a key file usually holds many keys and we want three. */
bool loadKeyFile(const QString& path, SwitchKeySet& set) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  bool any = false;
  while (!file.atEnd()) {
    const QByteArray line = file.readLine().trimmed();
    if (line.isEmpty() || line.startsWith('#') || line.startsWith(';')) continue;
    const qsizetype split = line.indexOf('=');
    if (split <= 0) continue;
    const QString name =
      QString::fromLatin1(line.left(split).trimmed()).toLower();
    const QByteArray hex = line.mid(split + 1).trimmed();
    const QByteArray value = QByteArray::fromHex(hex);
    if (value.isEmpty() || value.size() * 2 != hex.size()) continue;
    set.keys.insert(name, value);
    any = true;
  }
  return any;
}

QString hexName(const QString& prefix, int revision) {
  return QStringLiteral("%1_%2").arg(prefix,
    QString::number(revision, 16).rightJustified(2, QLatin1Char('0')));
}

/* The .cnmt.xml an NSP ships beside its NCAs, in the clear. It is the package's
 * own statement of what it contains, so the Program NCA is identified from it
 * rather than by guessing which of several NCAs is the biggest. */
void parseCnmtXml(const QByteArray& xml, SwitchNspInfo& info) {
  QXmlStreamReader reader(xml);
  // <ContentMeta> carries its own <Type>/<Id> and then one <Content> per NCA
  // with the same two element names, so which is being read depends only on
  // whether we are inside a <Content>.
  bool insideContent = false;
  QString type, id;
  quint64 size = 0;
  while (!reader.atEnd()) {
    reader.readNext();
    const QString name = reader.name().toString();
    if (reader.isStartElement()) {
      if (name == QLatin1String("Content")) {
        insideContent = true;
        type.clear();
        id.clear();
        size = 0;
      } else if (name == QLatin1String("Type")) {
        (insideContent ? type : info.contentMetaType) = reader.readElementText();
      } else if (name == QLatin1String("Id")) {
        (insideContent ? id : info.titleId) = reader.readElementText();
      } else if (name == QLatin1String("Size")) {
        if (insideContent) size = reader.readElementText().toULongLong();
      } else if (name == QLatin1String("PatchId")) {
        info.patchId = reader.readElementText();
      }
    } else if (reader.isEndElement() && name == QLatin1String("Content")) {
      insideContent = false;
      if (!id.isEmpty()) {
        info.contents.append({ type, id, size });
        if (type == QLatin1String("Program")) {
          info.programNca = id + QStringLiteral(".nca");
        } else if (type == QLatin1String("Control")) {
          info.controlNca = id + QStringLiteral(".nca");
        }
      }
    }
  }
}

// ── NCA sections ───────────────────────────────────────────────────────────

/* One entry of the NCA's section table, joined to its FS header. `counter` is
 * empty when the section is stored unencrypted, which is how the logo partition
 * every retail NCA carries is stored; `dataOffset`/`dataSize` are what the FS
 * header's superblock says about the partition inside it, relative to
 * `offset`. */
struct NcaSection {
  int index{ -1 };
  quint8 partitionType{ 0xFFu };  // 0 = RomFS, 1 = PFS0
  quint8 cryptType{ 0u };         // 1 = none, 3 = AES-CTR, 4 = BKTR
  quint64 offset{ 0 };            // NCA-relative
  quint64 size{ 0 };
  QByteArray counter;
  quint64 dataOffset{ 0 };  // relative to `offset`
  quint64 dataSize{ 0 };
};

/* An NCA whose header has been decrypted and whose body key has been unwrapped:
 * everything needed to read any of its sections. `missing` names the keys that
 * were wanted and absent, and is the only thing populated when `ok` is false —
 * a key that is not here is said by name rather than reported as "encrypted". */
struct OpenNca {
  bool ok{ false };
  QString format;
  int masterKeyRevision{ 0 };
  QByteArray bodyKey;
  QList<NcaSection> sections;
  QStringList missing;
};

/* Decrypts `entry`'s header and unwraps its body key. `encryptedTitleKey` is
 * the package ticket's, used only when the NCA carries a rights ID. */
OpenNca openNca(const RangeReader& readFile,
                const Pfs0Entry& entry,
                const SwitchKeySet& keys,
                const QByteArray& encryptedTitleKey) {
  OpenNca nca;
  if (entry.size < static_cast<quint64>(kNcaHeaderSize)) {
    nca.missing.append(QStringLiteral("(%1 is truncated)").arg(entry.name));
    return nca;
  }
  const QByteArray headerKey = keys.value(QStringLiteral("header_key"));
  if (headerKey.size() != 32) {
    nca.missing.append(QStringLiteral("header_key (must be 32 bytes)"));
    return nca;
  }
  QByteArray header;
  if (!readFile(entry.offset, kNcaHeaderSize, header)) {
    nca.missing.append(QStringLiteral("(%1 could not be read)").arg(entry.name));
    return nca;
  }
  if (!crypto::aesXtsDecryptSectors(headerKey, header.data(), header.size(), 0,
                                    kNcaSector)) {
    nca.missing.append(QStringLiteral("(%1's header could not be decrypted)")
                         .arg(entry.name));
    return nca;
  }
  const QByteArray magic = header.mid(0x200, 4);
  if (magic != QByteArrayLiteral("NCA3") && magic != QByteArrayLiteral("NCA2") &&
      magic != QByteArrayLiteral("NCA0")) {
    // Loud, not silent: the key file is present but is not this package's.
    nca.missing.append(QStringLiteral(
      "header_key (the one that was loaded does not decrypt this package's NCA "
      "header)"));
    return nca;
  }
  nca.format = QString::fromLatin1(magic);
  if (magic != QByteArrayLiteral("NCA3")) {
    nca.missing.append(QStringLiteral(
      "(this reader decrypts NCA3 sections; this package is %1)").arg(nca.format));
    return nca;
  }

  const auto cryptoTypeOld = static_cast<quint8>(header.at(0x206));
  const auto keyAreaIndex = static_cast<quint8>(header.at(0x207));
  const auto cryptoTypeNew = static_cast<quint8>(header.at(0x220));
  int masterKeyRevision = std::max<int>(cryptoTypeOld, cryptoTypeNew);
  if (masterKeyRevision > 0) --masterKeyRevision;  // 0 and 1 are both master key 0
  nca.masterKeyRevision = masterKeyRevision;

  const QByteArray rightsId = header.mid(0x230, 0x10);
  if (rightsId != QByteArray(0x10, '\0')) {
    const QString titlekek = hexName(QStringLiteral("titlekek"), masterKeyRevision);
    if (encryptedTitleKey.size() != 16) {
      nca.missing.append(QStringLiteral(
        "(the package's ticket carries no title key, so %1 has nothing to unwrap)")
          .arg(titlekek));
      return nca;
    }
    if (!keys.has(titlekek)) {
      nca.missing.append(titlekek);
      return nca;
    }
    nca.bodyKey = crypto::aesEcbDecrypt(keys.value(titlekek), encryptedTitleKey);
  } else {
    static const char* const kKekNames[] = {
      "key_area_key_application", "key_area_key_ocean", "key_area_key_system",
    };
    if (keyAreaIndex > 2u) {
      nca.missing.append(QStringLiteral(
        "(the NCA asks for key area key %1, which is not one of the three the "
        "format defines)").arg(keyAreaIndex));
      return nca;
    }
    const QString kek =
      hexName(QString::fromLatin1(kKekNames[keyAreaIndex]), masterKeyRevision);
    if (!keys.has(kek)) {
      nca.missing.append(kek);
      return nca;
    }
    nca.bodyKey = crypto::aesEcbDecrypt(keys.value(kek), header.mid(0x300 + 2 * 0x10, 0x10));
  }
  if (nca.bodyKey.size() != 16) {
    nca.missing.append(QStringLiteral("(the section key could not be unwrapped)"));
    return nca;
  }

  for (int index = 0; index < 4; ++index) {
    const qsizetype at = 0x240 + static_cast<qsizetype>(index) * 0x10;
    const quint64 mediaStart = le32(header, at);
    const quint64 mediaEnd = le32(header, at + 4);
    if (mediaEnd <= mediaStart) continue;
    const qsizetype fsHeader = 0x400 + static_cast<qsizetype>(index) * 0x200;
    if (fsHeader + 0x200 > header.size()) continue;

    NcaSection section;
    section.index = index;
    section.partitionType = static_cast<quint8>(header.at(fsHeader + 0x02));
    section.cryptType = static_cast<quint8>(header.at(fsHeader + 0x04));
    section.offset = mediaStart * static_cast<quint64>(kNcaSector);
    section.size = (mediaEnd - mediaStart) * static_cast<quint64>(kNcaSector);
    if (section.offset + section.size > entry.size) continue;

    if (section.cryptType == 3u) {
      // The section counter: the fs header's generation field, big-endian, then
      // the NCA-relative offset in units of 16 bytes, which CtrStream::seek adds.
      QByteArray counter(0x10, '\0');
      for (int i = 0; i < 8; ++i) counter[i] = header.at(fsHeader + 0x140 + (7 - i));
      section.counter = counter;
    } else if (section.cryptType != 1u) {
      // 4 is BKTR: a patch NCA's relocated RomFS, which is only readable with
      // the base game's NCA beside it. Recorded so a caller can say so, never
      // read as if it were plain CTR.
      section.dataSize = 0;
      nca.sections.append(section);
      continue;
    }

    if (section.partitionType == 1u) {  // PFS0 superblock
      section.dataOffset = le64(header, fsHeader + 0x40);
      section.dataSize = le64(header, fsHeader + 0x48);
    } else if (section.partitionType == 0u) {  // RomFS: an IVFC superblock
      if (header.mid(fsHeader + 0x08, 4) != QByteArrayLiteral("IVFC")) continue;
      const quint32 levels = le32(header, fsHeader + 0x14);
      // The header describes at most six levels and counts the master hash as
      // one, so the data is the last described level: index levels - 2.
      const int data =
        std::clamp<int>(static_cast<int>(levels) - 2, 0, kIvfcMaxLevel - 1);
      const qsizetype level =
        fsHeader + 0x18 + static_cast<qsizetype>(data) * 0x18;
      section.dataOffset = le64(header, level);
      section.dataSize = le64(header, level + 8);
    } else {
      continue;
    }
    if (section.dataOffset + section.dataSize > section.size) continue;
    nca.sections.append(section);
  }
  nca.ok = true;
  return nca;
}

/* Reads a range of `section`, NCA-relative, decrypting it when it is stored
 * encrypted. `offset` is relative to the NCA, not to the section, because that
 * is what the CTR counter is defined against. */
RangeReader ncaSectionReader(QFile& file,
                             quint64 ncaOffset,
                             quint64 ncaSize,
                             const QByteArray& key,
                             const QByteArray& counter) {
  return [&file, ncaOffset, ncaSize, key, counter](quint64 offset, qint64 size,
                                                   QByteArray& out) {
    out.clear();
    if (size < 0) return false;
    if (offset > ncaSize || static_cast<quint64>(size) > ncaSize - offset) return false;
    if (!file.seek(static_cast<qint64>(ncaOffset + offset))) return false;
    out = file.read(size);
    if (out.size() != size) { out.clear(); return false; }
    if (counter.isEmpty()) return true;
    crypto::CtrStream stream(key, counter);
    if (!stream.isValid()) { out.clear(); return false; }
    stream.seek(offset);
    stream.apply(out.data(), out.size());
    return true;
  };
}

// ── RomFS ──────────────────────────────────────────────────────────────────
//
// The image is self-describing: a 0x50-byte header of offsets, then a directory
// metadata table and a file metadata table, then the file data. Studio reads the
// two tables to say what the image holds — a count and a size that can be
// checked against the staged file — and to find one file by name in the root,
// which is how the Control NCA's NACP and icon are reached. It does not unpack
// the tree; see extractSwitchNspRomFs for why.

struct RomFsHeader {
  quint64 dirMetaOffset{ 0 }, dirMetaSize{ 0 };
  quint64 fileMetaOffset{ 0 }, fileMetaSize{ 0 };
  quint64 dataOffset{ 0 };
};

bool readRomFsHeader(const RangeReader& read, quint64 base, quint64 imageSize,
                     RomFsHeader& out) {
  QByteArray header;
  if (imageSize < 0x50u || !read(base, 0x50, header) || header.size() != 0x50) return false;
  if (le64(header, 0x00) != 0x50u) return false;  // header_size
  // Ten u64s: header size, then the directory hash table and the directory
  // metadata table, then the file hash table and the file metadata table, then
  // the data. The hash tables are the lookup acceleration and the metadata
  // tables are the entries themselves — reading one for the other yields a
  // table that parses to nothing, which is how this was caught.
  out.dirMetaOffset = le64(header, 0x18);
  out.dirMetaSize = le64(header, 0x20);
  out.fileMetaOffset = le64(header, 0x38);
  out.fileMetaSize = le64(header, 0x40);
  out.dataOffset = le64(header, 0x48);
  const auto inImage = [imageSize](quint64 offset, quint64 size) {
    return offset <= imageSize && size <= imageSize - offset;
  };
  return inImage(out.dirMetaOffset, out.dirMetaSize) &&
         inImage(out.fileMetaOffset, out.fileMetaSize) &&
         out.dataOffset <= imageSize;
}

/* The file metadata table is the entries packed one after another, so a linear
 * walk visits every file in the image exactly once without following the tree.
 * Each is {parent u32, sibling u32, offset u64, size u64, hash u32, name size
 * u32} and then the name, padded to four bytes. */
void walkRomFsFiles(const QByteArray& table, quint64& count, quint64& bytes) {
  count = 0;
  bytes = 0;
  qsizetype at = 0;
  while (at + 0x20 <= table.size()) {
    const quint64 size = le64(table, at + 0x10);
    const quint32 nameSize = le32(table, at + 0x1C);
    if (nameSize > kRomFsMaxNameSize) break;
    ++count;
    bytes += size;
    at += 0x20 + ((static_cast<qsizetype>(nameSize) + 3) & ~qsizetype{ 3 });
  }
}

QString romFsName(const QByteArray& table, qsizetype at, qsizetype headerSize) {
  const quint32 nameSize = le32(table, at + headerSize - 4);
  if (nameSize > kRomFsMaxNameSize || at + headerSize + nameSize > table.size()) return {};
  return QString::fromUtf8(table.mid(at + headerSize, static_cast<qsizetype>(nameSize)));
}

/* The names directly under the root: subdirectories first, then files, each
 * chain followed by its `sibling` link. Bounded, because this is for a summary
 * and a package can hold tens of thousands of entries. */
QStringList romFsRootEntries(const QByteArray& dirs, const QByteArray& files) {
  QStringList names;
  if (dirs.size() < 0x18) return names;
  const quint32 kNone = 0xFFFFFFFFu;
  quint32 at = le32(dirs, 0x08);  // root's first child directory
  int guard = 0;
  while (at != kNone && at + 0x18 <= dirs.size() && names.size() < kRomFsListLimit &&
         ++guard < 4 * kRomFsListLimit) {
    const QString name = romFsName(dirs, at, 0x18);
    if (!name.isEmpty()) names.append(name + QLatin1Char('/'));
    at = le32(dirs, at + 0x04);
  }
  at = le32(dirs, 0x0C);  // root's first file
  guard = 0;
  while (at != kNone && at + 0x20 <= files.size() && names.size() < kRomFsListLimit &&
         ++guard < 4 * kRomFsListLimit) {
    const QString name = romFsName(files, at, 0x20);
    if (!name.isEmpty()) names.append(name);
    at = le32(files, at + 0x04);
  }
  return names;
}

/* One file out of the root of a RomFS, by name. Used for the Control NCA's
 * `control.nacp` and its icon, both of which sit at the root. */
bool readRomFsRootFile(const RangeReader& read, quint64 base, const RomFsHeader& header,
                       const QByteArray& files,
                       const std::function<bool(const QString&)>& wanted,
                       QString& foundName, qint64 limit, QByteArray& out) {
  const quint32 kNone = 0xFFFFFFFFu;
  QByteArray dirs;
  if (!read(base + header.dirMetaOffset, static_cast<qint64>(header.dirMetaSize), dirs))
    return false;
  if (dirs.size() < 0x18) return false;
  quint32 at = le32(dirs, 0x0C);
  int guard = 0;
  while (at != kNone && at + 0x20 <= files.size() && ++guard < 65536) {
    const QString name = romFsName(files, at, 0x20);
    if (!name.isEmpty() && wanted(name)) {
      const quint64 offset = le64(files, at + 0x08);
      const quint64 size = le64(files, at + 0x10);
      if (size > static_cast<quint64>(limit)) return false;
      foundName = name;
      return read(base + header.dataOffset + offset, static_cast<qint64>(size), out);
    }
    at = le32(files, at + 0x04);
  }
  return false;
}

/* The NACP a Control NCA carries: sixteen 0x300-byte language entries, each a
 * 0x200-byte name and a 0x100-byte publisher, in the fixed language order whose
 * first slot is AmericanEnglish. A title that ships no English entry still has
 * one filled in somewhere, so the first non-empty is the fallback rather than a
 * blank name. */
void parseNacp(const QByteArray& nacp, SwitchNspInfo& info) {
  constexpr qsizetype kEntry = 0x300;
  constexpr qsizetype kNameSize = 0x200;
  constexpr int kLanguages = 16;
  const auto text = [](const QByteArray& field) {
    const qsizetype end = field.indexOf('\0');
    return QString::fromUtf8(end < 0 ? field : field.left(end)).trimmed();
  };
  for (int language = 0; language < kLanguages; ++language) {
    const qsizetype at = static_cast<qsizetype>(language) * kEntry;
    if (at + kEntry > nacp.size()) break;
    const QString name = text(nacp.mid(at, kNameSize));
    if (name.isEmpty()) continue;
    info.displayTitle = name;
    info.publisher = text(nacp.mid(at + kNameSize, kEntry - kNameSize));
    break;
  }
}

/* A Switch ticket. The signature type fixes where the body starts; from there
 * the layout is fixed too. */
void parseTicket(const QByteArray& ticket, SwitchNspInfo& info) {
  if (ticket.size() < 0x180) return;
  const quint32 signatureType = le32(ticket, 0);
  qsizetype body = 0;
  switch (signatureType) {
    case 0x00010000u: case 0x00010003u: body = 0x240; break;  // RSA-4096
    case 0x00010001u: case 0x00010004u: body = 0x140; break;  // RSA-2048
    case 0x00010002u: case 0x00010005u: body = 0x080; break;  // ECDSA
    default: return;
  }
  if (ticket.size() < body + 0x180) return;
  info.encryptedTitleKey = ticket.mid(body + 0x40, 0x10);
  info.rightsId = ticket.mid(body + 0x160, 0x10);
}

}  // namespace

SwitchKeySet loadSwitchKeys(const QString& besidePath) {
  SwitchKeySet set;
  QStringList candidates;
  const QString fromEnvironment =
    QProcessEnvironment::systemEnvironment().value(QStringLiteral("PSXRECOMP_SWITCH_KEYS"));
  if (!fromEnvironment.isEmpty()) {
    candidates.append(QFileInfo(fromEnvironment).isDir()
                        ? QDir(fromEnvironment).filePath(QStringLiteral("prod.keys"))
                        : fromEnvironment);
  }
  if (!besidePath.isEmpty()) {
    candidates.append(QFileInfo(besidePath).absoluteDir().filePath(QStringLiteral("prod.keys")));
  }
  candidates.append(QDir(QDir::homePath()).filePath(QStringLiteral(".switch/prod.keys")));

  for (const QString& candidate : candidates) {
    set.searchedPaths.append(candidate);
    if (!QFileInfo(candidate).isFile()) continue;
    if (loadKeyFile(candidate, set)) {
      set.loadedFrom = candidate;
      break;
    }
  }
  return set;
}

bool readSwitchNsp(const QString& path, SwitchNspInfo& info, QString& error) {
  info = {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the package: %1").arg(file.errorString());
    return false;
  }
  const qint64 fileSize = file.size();
  const RangeReader readFile = [&](quint64 offset, qint64 size, QByteArray& out) {
    out.clear();
    if (size < 0 || offset > static_cast<quint64>(fileSize) ||
        static_cast<quint64>(size) > static_cast<quint64>(fileSize) - offset) {
      return false;
    }
    if (!file.seek(static_cast<qint64>(offset))) return false;
    out = file.read(size);
    return out.size() == size;
  };

  if (!parsePfs0(readFile, 0, info.entries, error)) return false;

  // Tier one: what the package says about itself, in the clear.
  for (const Pfs0Entry& entry : info.entries) {
    if (entry.name.endsWith(QStringLiteral(".cnmt.xml")) && entry.size <= 1 << 20) {
      QByteArray xml;
      if (readFile(entry.offset, static_cast<qint64>(entry.size), xml)) {
        parseCnmtXml(xml, info);
      }
    } else if (entry.name.endsWith(QStringLiteral(".tik")) && entry.size <= 4096u) {
      QByteArray ticket;
      if (readFile(entry.offset, static_cast<qint64>(entry.size), ticket)) {
        info.ticketName = entry.name;
        parseTicket(ticket, info);
      }
    }
  }
  if (info.programNca.isEmpty()) {
    // No .cnmt.xml, or one that names no Program: fall back to the meta NCA's
    // own name only if exactly one plain .nca is present, and otherwise say
    // nothing rather than pick one.
    QStringList plain;
    for (const Pfs0Entry& entry : info.entries) {
      if (entry.name.endsWith(QStringLiteral(".nca")) &&
          !entry.name.endsWith(QStringLiteral(".cnmt.nca"))) {
        plain.append(entry.name);
      }
    }
    if (plain.size() == 1) info.programNca = plain.constFirst();
  }

  // Tier two.
  const SwitchKeySet keys = loadSwitchKeys(path);
  info.keySource = keys.loadedFrom;
  info.keySearchPaths = keys.searchedPaths;
  info.keysLoaded = !keys.isEmpty();
  if (!keys.has(QStringLiteral("header_key"))) {
    info.missingKeys.append(QStringLiteral("header_key"));
    return true;
  }
  const QByteArray headerKey = keys.value(QStringLiteral("header_key"));
  if (headerKey.size() != 32) {
    info.missingKeys.append(QStringLiteral("header_key (must be 32 bytes)"));
    return true;
  }
  if (info.programNca.isEmpty()) return true;

  const auto findEntry = [&info](const QString& name, Pfs0Entry& out) {
    for (const Pfs0Entry& entry : info.entries) {
      if (entry.name == name) { out = entry; return true; }
    }
    return false;
  };

  Pfs0Entry program;
  if (!findEntry(info.programNca, program)) {
    info.missingKeys.append(QStringLiteral(
      "(the package's own .cnmt.xml names %1 as its program, but the archive "
      "does not contain it)").arg(info.programNca));
    return true;
  }

  const OpenNca nca = openNca(readFile, program, keys, info.encryptedTitleKey);
  info.ncaFormat = nca.format;
  info.masterKeyRevision = nca.masterKeyRevision;
  if (!nca.ok) {
    info.missingKeys.append(nca.missing);
    return true;
  }
  info.programNcaOffset = program.offset;
  info.sectionKey = nca.bodyKey;

  // The ExeFS: the PFS0 partition holding `main`, found by looking for the
  // module rather than by assuming an index. A retail NCA carries a second PFS0
  // — the unencrypted logo partition — so the index really is not fixed.
  for (const NcaSection& section : nca.sections) {
    if (section.partitionType != 1u || section.dataSize == 0u) continue;
    const RangeReader readSection = ncaSectionReader(
      file, program.offset, program.size, nca.bodyKey, section.counter);
    QList<Pfs0Entry> exeFs;
    QString partitionError;
    if (!parsePfs0(readSection, section.offset + section.dataOffset, exeFs,
                   partitionError)) {
      continue;
    }
    const auto main = std::find_if(exeFs.cbegin(), exeFs.cend(), [](const Pfs0Entry& e) {
      return e.name == QStringLiteral("main");
    });
    if (main == exeFs.cend()) continue;

    info.exeFsEntries = exeFs;
    info.mainModule = main->name;
    info.mainOffset = main->offset;
    info.mainSize = main->size;
    info.sectionCounter = section.counter;
    readSection(main->offset,
                static_cast<qint64>(std::min<quint64>(main->size, 0x1000u)),
                info.mainModuleHead);
    info.programDecrypted = true;
    break;
  }
  if (!info.programDecrypted) {
    info.missingKeys.append(QStringLiteral(
      "(no ExeFS partition holding a `main` module was found in the program NCA)"));
    return true;
  }

  // The RomFS: the game's data, and the larger half of the package by far. Its
  // own metadata tables are read so the staged image can be described — a file
  // count and a byte total that a later check has something to compare against.
  for (const NcaSection& section : nca.sections) {
    if (section.partitionType != 0u || section.dataSize == 0u) continue;
    const RangeReader readSection = ncaSectionReader(
      file, program.offset, program.size, nca.bodyKey, section.counter);
    const quint64 base = section.offset + section.dataOffset;
    RomFsHeader romFs;
    if (!readRomFsHeader(readSection, base, section.dataSize, romFs)) continue;

    info.romFsPresent = true;
    info.romFsOffset = base;
    info.romFsSize = section.dataSize;
    info.romFsCounter = section.counter;
    QByteArray files, dirs;
    if (romFs.fileMetaSize <= static_cast<quint64>(kRomFsMetaLimit) &&
        readSection(base + romFs.fileMetaOffset,
                    static_cast<qint64>(romFs.fileMetaSize), files)) {
      walkRomFsFiles(files, info.romFsFileCount, info.romFsFileBytes);
      if (romFs.dirMetaSize <= static_cast<quint64>(kRomFsMetaLimit) &&
          readSection(base + romFs.dirMetaOffset,
                      static_cast<qint64>(romFs.dirMetaSize), dirs)) {
        info.romFsRootEntries = romFsRootEntries(dirs, files);
      }
    }
    break;
  }

  // The Control NCA, when the package names one: the title's own name and its
  // icon, both at the root of its RomFS. A package that carries neither is not
  // a failure — the file name stands in — so nothing here is appended to
  // missingKeys.
  Pfs0Entry control;
  if (info.controlNca.isEmpty() || !findEntry(info.controlNca, control)) return true;
  const OpenNca controlNca =
    openNca(readFile, control, keys, info.encryptedTitleKey);
  if (!controlNca.ok) return true;
  for (const NcaSection& section : controlNca.sections) {
    if (section.partitionType != 0u || section.dataSize == 0u) continue;
    const RangeReader readSection = ncaSectionReader(
      file, control.offset, control.size, controlNca.bodyKey, section.counter);
    const quint64 base = section.offset + section.dataOffset;
    RomFsHeader romFs;
    if (!readRomFsHeader(readSection, base, section.dataSize, romFs)) continue;
    if (romFs.fileMetaSize > static_cast<quint64>(kRomFsMetaLimit)) break;
    QByteArray files;
    if (!readSection(base + romFs.fileMetaOffset,
                     static_cast<qint64>(romFs.fileMetaSize), files)) {
      break;
    }
    QString found;
    QByteArray blob;
    if (readRomFsRootFile(readSection, base, romFs, files,
                          [](const QString& name) {
                            return name == QStringLiteral("control.nacp");
                          },
                          found, kControlNacpLimit, blob)) {
      parseNacp(blob, info);
    }
    // AmericanEnglish first because that is language slot 0; any other icon is
    // the same artwork and is better than none.
    if (readRomFsRootFile(readSection, base, romFs, files,
                          [](const QString& name) {
                            return name == QStringLiteral("icon_AmericanEnglish.dat");
                          },
                          found, kControlIconLimit, blob) ||
        readRomFsRootFile(readSection, base, romFs, files,
                          [](const QString& name) {
                            return name.startsWith(QStringLiteral("icon_")) &&
                                   name.endsWith(QStringLiteral(".dat"));
                          },
                          found, kControlIconLimit, blob)) {
      info.iconEntryName = found;
      info.icon = blob;
    }
    break;
  }
  return true;
}

namespace {

/* Streams `size` bytes out of the program NCA at NCA-relative `offset` into
 * `destinationFile`, decrypting on the way when the section is encrypted.
 * Chunked, because the RomFS of a real title is hundreds of megabytes and
 * reading one into memory to write it back out is not a thing to do. */
bool writeNcaRange(QFile& file,
                   quint64 ncaOffset,
                   quint64 offset,
                   quint64 size,
                   const QByteArray& key,
                   const QByteArray& counter,
                   const QString& what,
                   const QString& destinationFile,
                   QString& error) {
  crypto::CtrStream stream(key, counter);
  if (!counter.isEmpty() && !stream.isValid()) {
    error = QStringLiteral("The section key could not be prepared.");
    return false;
  }
  if (!counter.isEmpty()) stream.seek(offset);
  if (!file.seek(static_cast<qint64>(ncaOffset + offset))) {
    error = QStringLiteral("Could not seek to %1 inside the package.").arg(what);
    return false;
  }
  if (!QDir().mkpath(QFileInfo(destinationFile).absolutePath())) {
    error = QStringLiteral("Could not create %1.")
              .arg(QFileInfo(destinationFile).absolutePath());
    return false;
  }
  QFile out(destinationFile);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    error = QStringLiteral("Could not write %1: %2").arg(destinationFile, out.errorString());
    return false;
  }
  quint64 remaining = size;
  while (remaining > 0u) {
    QByteArray chunk = file.read(std::min<qint64>(kChunk, static_cast<qint64>(remaining)));
    if (chunk.isEmpty()) {
      error = QStringLiteral("The package ends inside %1.").arg(what);
      return false;
    }
    if (!counter.isEmpty()) stream.apply(chunk.data(), chunk.size());
    if (out.write(chunk) != chunk.size()) {
      error = QStringLiteral("Could not write %1: %2").arg(destinationFile, out.errorString());
      return false;
    }
    remaining -= static_cast<quint64>(chunk.size());
  }
  if (!out.flush()) {
    error = QStringLiteral("Could not write %1: %2").arg(destinationFile, out.errorString());
    return false;
  }
  return true;
}

}  // namespace

bool extractSwitchNspMain(const QString& path,
                          const SwitchNspInfo& info,
                          const QString& destinationFile,
                          QString& error) {
  if (!info.programDecrypted || info.mainSize == 0u) {
    error = QStringLiteral("The package's ExeFS was not decrypted, so there is no "
                           "module to write.");
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the package: %1").arg(file.errorString());
    return false;
  }
  return writeNcaRange(file, info.programNcaOffset, info.mainOffset, info.mainSize,
                       info.sectionKey, info.sectionCounter,
                       QStringLiteral("the ExeFS module"), destinationFile, error);
}

bool extractSwitchNspExeFs(const QString& path,
                           const SwitchNspInfo& info,
                           const QString& destination,
                           QStringList& extracted,
                           QString& error) {
  extracted.clear();
  if (!info.programDecrypted || info.exeFsEntries.isEmpty()) {
    error = QStringLiteral("The package's ExeFS was not decrypted, so there is "
                           "nothing to write.");
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the package: %1").arg(file.errorString());
    return false;
  }
  if (!QDir().mkpath(destination)) {
    error = QStringLiteral("Could not create %1.").arg(destination);
    return false;
  }
  for (const Pfs0Entry& entry : info.exeFsEntries) {
    // An ExeFS is flat, so a name with a separator in it did not come from one.
    // Refused rather than sanitised: a path that escapes the destination is a
    // damaged or hostile package, and quietly renaming it hides that.
    const QString name = entry.name;
    if (name.isEmpty() || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\')) || name.startsWith(QLatin1Char('.'))) {
      error = QStringLiteral("The ExeFS holds an entry named `%1`, which is not a "
                             "flat partition name.").arg(name);
      return false;
    }
    if (!writeNcaRange(file, info.programNcaOffset, entry.offset, entry.size,
                       info.sectionKey, info.sectionCounter,
                       QStringLiteral("ExeFS %1").arg(name),
                       QDir(destination).filePath(name), error)) {
      return false;
    }
    extracted.append(name);
  }
  return true;
}

bool extractSwitchNspRomFs(const QString& path,
                           const SwitchNspInfo& info,
                           const QString& destinationFile,
                           QString& error) {
  if (!info.programDecrypted || !info.romFsPresent || info.romFsSize == 0u) {
    error = QStringLiteral("The package's program NCA carries no RomFS section, "
                           "so there is no data image to write.");
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the package: %1").arg(file.errorString());
    return false;
  }
  return writeNcaRange(file, info.programNcaOffset, info.romFsOffset, info.romFsSize,
                       info.sectionKey, info.romFsCounter,
                       QStringLiteral("the RomFS"), destinationFile, error);
}

bool extractSwitchNspPlaintext(const QString& path,
                               const SwitchNspInfo& info,
                               const QString& destination,
                               QStringList& extracted,
                               QString& error) {
  extracted.clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the package: %1").arg(file.errorString());
    return false;
  }
  if (!QDir().mkpath(destination)) {
    error = QStringLiteral("Could not create %1.").arg(destination);
    return false;
  }
  for (const Pfs0Entry& entry : info.entries) {
    const bool wanted = entry.name.endsWith(QStringLiteral(".xml")) ||
                        entry.name.endsWith(QStringLiteral(".tik")) ||
                        entry.name.endsWith(QStringLiteral(".cert"));
    if (!wanted || entry.size > 1 << 20) continue;
    const QString name = QFileInfo(entry.name).fileName();
    if (name.isEmpty()) continue;
    if (!file.seek(static_cast<qint64>(entry.offset))) continue;
    const QByteArray blob = file.read(static_cast<qint64>(entry.size));
    if (blob.size() != static_cast<qsizetype>(entry.size)) continue;
    QFile out(QDir(destination).filePath(name));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      error = QStringLiteral("Could not write %1: %2").arg(out.fileName(), out.errorString());
      return false;
    }
    if (out.write(blob) != blob.size()) {
      error = QStringLiteral("Could not write %1: %2").arg(out.fileName(), out.errorString());
      return false;
    }
    out.close();
    extracted.append(name);
  }
  return true;
}

}  // namespace psxstudio
