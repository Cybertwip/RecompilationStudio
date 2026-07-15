#include "DiscInspector.h"

#include "iso_reader.h"
#include "ps1_exe_parser.h"

#include <QRegularExpression>

#include <algorithm>
#include <vector>

namespace psxstudio {

namespace {

QString normalizeSerial(const QString& bootName) {
  QString compact;
  for (const auto c : bootName) {
    if (c.isLetterOrNumber()) {
      compact.append(c.toUpper());
    }
  }
  if (compact.size() < 9) {
    return {};
  }
  return compact.left(4) + QStringLiteral("-") + compact.mid(4, 5);
}

QString parseBootPath(const QByteArray& systemCnf) {
  const QString text = QString::fromLatin1(systemCnf);
  const QRegularExpression pattern(
    QStringLiteral(R"((?im)^\s*BOOT\s*=\s*cdrom:\s*[\\/]*([^;\r\n]+))"));
  const auto match = pattern.match(text);
  if (!match.hasMatch()) {
    return {};
  }
  QString path = match.captured(1).trimmed();
  path.replace('\\', '/');
  while (path.startsWith('/')) {
    path.remove(0, 1);
  }
  return path;
}

bool readIsoFile(PS1::ISOReader& reader, const std::string& path, QByteArray& out) {
  const auto size = reader.GetFileSize(path);
  if (size == 0) {
    return false;
  }
  std::vector<uint8_t> bytes(size);
  const auto read = reader.ReadFile(path, bytes.data(), bytes.size());
  if (read != size) {
    return false;
  }
  out = QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
  return true;
}

} // namespace

bool DiscInspector::inspect(const DiscDescription& disc, GameDescription& out, QString& error) {
  out = {};
  PS1::ISOReader reader;
  if (!reader.Open(disc.cuePath.toStdString())) {
    error = QStringLiteral("The BIN/CUE set could not be opened as a PlayStation disc.");
    return false;
  }

  QByteArray systemCnf;
  if (!readIsoFile(reader, "SYSTEM.CNF", systemCnf)) {
    error = QStringLiteral("SYSTEM.CNF was not found in the disc root.");
    return false;
  }
  const QString bootPath = parseBootPath(systemCnf);
  if (bootPath.isEmpty()) {
    error = QStringLiteral("SYSTEM.CNF does not contain a valid cdrom: BOOT entry.");
    return false;
  }

  QByteArray exeBytes;
  if (!readIsoFile(reader, bootPath.toStdString(), exeBytes)) {
    error = QStringLiteral("The boot executable named by SYSTEM.CNF was not found: %1").arg(bootPath);
    return false;
  }

  std::vector<uint8_t> buffer(static_cast<size_t>(exeBytes.size()));
  std::copy(exeBytes.begin(), exeBytes.end(), reinterpret_cast<char*>(buffer.data()));
  std::string parseError;
  auto executable = PSXRecomp::PS1ExeParser::parse_buffer(buffer, parseError);
  if (!executable) {
    error = QStringLiteral("The disc boot file is not a valid PS-X EXE: %1")
              .arg(QString::fromStdString(parseError));
    return false;
  }

  const auto& parsed = *executable;
  out.bootPath = bootPath;
  out.bootFileName = bootPath.section('/', -1);
  out.serial = normalizeSerial(out.bootFileName);
  out.volumeId = QString::fromStdString(reader.GetVolumeID()).trimmed();
  out.executableBytes = exeBytes;
  out.codeBytes = QByteArray(reinterpret_cast<const char*>(parsed.code_data.data()),
                             static_cast<qsizetype>(parsed.code_data.size()));
  out.loadAddress = parsed.load_address();
  out.entryPc = parsed.entry_point();
  out.textSize = parsed.code_size();
  out.stackBase = parsed.header.stack_base != 0 ? parsed.header.stack_base : 0x801FFFF0u;
  return true;
}

} // namespace psxstudio
