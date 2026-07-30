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
        }
      }
    }
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

  Pfs0Entry program;
  bool foundProgram = false;
  for (const Pfs0Entry& entry : info.entries) {
    if (entry.name == info.programNca) {
      program = entry;
      foundProgram = true;
    }
  }
  if (!foundProgram) {
    info.missingKeys.append(QStringLiteral(
      "(the package's own .cnmt.xml names %1 as its program, but the archive "
      "does not contain it)").arg(info.programNca));
    return true;
  }
  if (program.size < static_cast<quint64>(kNcaHeaderSize)) {
    info.missingKeys.append(QStringLiteral("(the program NCA is truncated)"));
    return true;
  }
  QByteArray header;
  if (!readFile(program.offset, kNcaHeaderSize, header)) return true;
  if (!crypto::aesXtsDecryptSectors(headerKey, header.data(), header.size(), 0,
                                    kNcaSector)) {
    return true;
  }
  const QByteArray magic = header.mid(0x200, 4);
  if (magic != QByteArrayLiteral("NCA3") && magic != QByteArrayLiteral("NCA2") &&
      magic != QByteArrayLiteral("NCA0")) {
    // Loud, not silent: the key file is present but is not this package's.
    info.missingKeys.append(QStringLiteral(
      "header_key (the one that was loaded does not decrypt this package's NCA "
      "header)"));
    return true;
  }
  info.ncaFormat = QString::fromLatin1(magic);
  if (magic != QByteArrayLiteral("NCA3")) {
    info.missingKeys.append(QStringLiteral(
      "(this reader decrypts NCA3 sections; this package is %1)").arg(info.ncaFormat));
    return true;
  }

  const auto cryptoTypeOld = static_cast<quint8>(header.at(0x206));
  const auto keyAreaIndex = static_cast<quint8>(header.at(0x207));
  const auto cryptoTypeNew = static_cast<quint8>(header.at(0x220));
  int masterKeyRevision = std::max<int>(cryptoTypeOld, cryptoTypeNew);
  if (masterKeyRevision > 0) --masterKeyRevision;  // 0 and 1 are both master key 0
  info.masterKeyRevision = masterKeyRevision;

  const QByteArray rightsId = header.mid(0x230, 0x10);
  const bool titleKeyed = rightsId != QByteArray(0x10, '\0');

  QByteArray bodyKey;
  if (titleKeyed) {
    const QString titlekek = hexName(QStringLiteral("titlekek"), masterKeyRevision);
    if (info.encryptedTitleKey.size() != 16) {
      info.missingKeys.append(QStringLiteral(
        "(the package's ticket carries no title key, so %1 has nothing to unwrap)")
          .arg(titlekek));
      return true;
    }
    if (!keys.has(titlekek)) {
      info.missingKeys.append(titlekek);
      return true;
    }
    bodyKey = crypto::aesEcbDecrypt(keys.value(titlekek), info.encryptedTitleKey);
  } else {
    static const char* const kKekNames[] = {
      "key_area_key_application", "key_area_key_ocean", "key_area_key_system",
    };
    if (keyAreaIndex > 2u) {
      info.missingKeys.append(QStringLiteral(
        "(the NCA asks for key area key %1, which is not one of the three the "
        "format defines)").arg(keyAreaIndex));
      return true;
    }
    const QString kek =
      hexName(QString::fromLatin1(kKekNames[keyAreaIndex]), masterKeyRevision);
    if (!keys.has(kek)) {
      info.missingKeys.append(kek);
      return true;
    }
    bodyKey = crypto::aesEcbDecrypt(keys.value(kek), header.mid(0x300 + 2 * 0x10, 0x10));
  }
  if (bodyKey.size() != 16) {
    info.missingKeys.append(QStringLiteral("(the section key could not be unwrapped)"));
    return true;
  }

  // The section that holds ExeFS: a PFS0 partition, found by looking for the
  // module rather than by assuming an index.
  for (int section = 0; section < 4; ++section) {
    const qsizetype entry = 0x240 + static_cast<qsizetype>(section) * 0x10;
    const quint64 mediaStart = le32(header, entry);
    const quint64 mediaEnd = le32(header, entry + 4);
    if (mediaEnd <= mediaStart) continue;
    const qsizetype fsHeader = 0x400 + static_cast<qsizetype>(section) * 0x200;
    if (fsHeader + 0x200 > header.size()) continue;
    const auto partitionType = static_cast<quint8>(header.at(fsHeader + 0x02));
    const auto cryptType = static_cast<quint8>(header.at(fsHeader + 0x04));
    if (partitionType != 1u) continue;  // 1 = PFS0, 0 = RomFS
    if (cryptType != 3u) continue;      // 3 = AES-CTR; ExeFS is always CTR
    const quint64 sectionOffset = mediaStart * static_cast<quint64>(kNcaSector);
    const quint64 pfs0Offset = le64(header, fsHeader + 0x40);
    const quint64 pfs0Size = le64(header, fsHeader + 0x48);
    if (pfs0Size == 0u) continue;

    // The section counter: the fs header's eight bytes, reversed, then the
    // NCA-relative offset in units of 16 bytes.
    QByteArray counter(0x10, '\0');
    for (int i = 0; i < 8; ++i) counter[i] = header.at(fsHeader + 0x140 + (7 - i));

    const RangeReader readSection =
      [&](quint64 offset, qint64 size, QByteArray& out) {
        out.clear();
        if (size < 0) return false;
        if (offset + static_cast<quint64>(size) > program.size) return false;
        crypto::CtrStream stream(bodyKey, counter);
        if (!stream.isValid()) return false;
        stream.seek(offset);
        if (!file.seek(static_cast<qint64>(program.offset + offset))) return false;
        out = file.read(size);
        if (out.size() != size) { out.clear(); return false; }
        stream.apply(out.data(), out.size());
        return true;
      };

    QList<Pfs0Entry> exeFs;
    QString partitionError;
    if (!parsePfs0(readSection, sectionOffset + pfs0Offset, exeFs, partitionError)) {
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
    info.programNcaOffset = program.offset;
    info.sectionKey = bodyKey;
    info.sectionCounter = counter;
    readSection(main->offset,
                static_cast<qint64>(std::min<quint64>(main->size, 0x1000u)),
                info.mainModuleHead);
    info.programDecrypted = true;
    break;
  }
  if (!info.programDecrypted) {
    info.missingKeys.append(QStringLiteral(
      "(no ExeFS partition holding a `main` module was found in the program NCA)"));
  }
  return true;
}

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
  crypto::CtrStream stream(info.sectionKey, info.sectionCounter);
  if (!stream.isValid()) {
    error = QStringLiteral("The section key could not be prepared.");
    return false;
  }
  stream.seek(info.mainOffset);
  if (!file.seek(static_cast<qint64>(info.programNcaOffset + info.mainOffset))) {
    error = QStringLiteral("Could not seek to the module inside the package.");
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
  quint64 remaining = info.mainSize;
  while (remaining > 0u) {
    QByteArray chunk = file.read(std::min<qint64>(kChunk, static_cast<qint64>(remaining)));
    if (chunk.isEmpty()) {
      error = QStringLiteral("The package ends inside its ExeFS module.");
      return false;
    }
    stream.apply(chunk.data(), chunk.size());
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
