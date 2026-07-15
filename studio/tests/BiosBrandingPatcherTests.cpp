#include "BiosBrandingPatcher.h"

#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <algorithm>

namespace {

int failures = 0;

void check(bool ok, const QString& message) {
  if (!ok) {
    qCritical().noquote() << "FAIL:" << message;
    ++failures;
  }
}

quint16 read16(const QByteArray& bytes, int offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint16>(p[0] | (p[1] << 8));
}

quint32 read32(const QByteArray& bytes, int offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8) |
         (static_cast<quint32>(p[2]) << 16) | (static_cast<quint32>(p[3]) << 24);
}

bool allZero(const QByteArray& bytes) {
  return std::all_of(bytes.cbegin(), bytes.cend(), [](char value) { return value == 0; });
}

} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  QTemporaryDir dir;
  check(dir.isValid(), QStringLiteral("temp dir"));
  if (!dir.isValid()) return 1;

  QFile input(QStringLiteral(PSX_BRANDING_TEST_BIOS));
  check(input.open(QIODevice::ReadOnly), QStringLiteral("open canonical BIOS"));
  const QByteArray original = input.readAll();

  QImage first(667, 500, QImage::Format_RGB32);
  first.fill(qRgb(16, 72, 150));
  QPainter p1(&first);
  p1.setPen(Qt::white);
  QFont font = p1.font();
  font.setPixelSize(110);
  font.setBold(true);
  p1.setFont(font);
  p1.drawText(first.rect(), Qt::AlignCenter, QStringLiteral("TEST"));
  p1.end();

  QImage second(512, 384, QImage::Format_RGB32);
  second.fill(Qt::black);
  QPainter p2(&second);
  p2.setPen(qRgb(255, 40, 20));
  font.setPixelSize(120);
  p2.setFont(font);
  p2.drawText(second.rect(), Qt::AlignCenter, QStringLiteral("MVII"));
  p2.end();

  const QString firstPath = dir.filePath(QStringLiteral("first.png"));
  const QString secondPath = dir.filePath(QStringLiteral("second.png"));
  const QString outputPath = dir.filePath(QStringLiteral("patched.bin"));
  const QString manifestPath = dir.filePath(QStringLiteral("patch.json"));
  first.save(firstPath);
  second.save(secondPath);

  psxstudio::BiosBrandingPatchOptions options{
    QStringLiteral(PSX_BRANDING_TEST_BIOS), outputPath, firstPath, secondPath,
    manifestPath, true, true
  };
  QString error;
  check(psxstudio::BiosBrandingPatcher::patch(options, error), error);

  QFile output(outputPath);
  check(output.open(QIODevice::ReadOnly), QStringLiteral("open patched BIOS"));
  const QByteArray patched = output.readAll();
  check(patched.size() == original.size(), QStringLiteral("BIOS size unchanged"));
  if (patched.size() != original.size()) return 1;

  // The complete source image lives in one legal 120x90 TIM and the BIOS
  // textured-quad helper scales it to the full 640x480 render area.
  check(read16(patched, 0x5CC30 + 60) == 30u, QStringLiteral("initial TIM width is 120"));
  check(read16(patched, 0x5CC30 + 62) == 90u, QStringLiteral("initial TIM height is 90"));
  check(read16(patched, 0x5E2F0 + 60) == 60u, QStringLiteral("blank second TIM keeps stock width"));
  check(read16(patched, 0x5E2F0 + 62) == 60u, QStringLiteral("blank second TIM keeps stock height"));
  check(read32(patched, 0x28570) == 0x00000000u, QStringLiteral("earlier diamond builder skipped"));
  check(read32(patched, 0x28594) == 0x00002021u, QStringLiteral("early clear red is black"));
  check(read32(patched, 0x28598) == 0x00002821u, QStringLiteral("early clear green is black"));
  check(read32(patched, 0x285A0) == 0x00003021u, QStringLiteral("early clear blue is black"));
  check(read32(patched, 0x286BC) == 0x00000000u, QStringLiteral("diamond primitive A not submitted"));
  check(read32(patched, 0x286F4) == 0x00000000u, QStringLiteral("diamond primitive B not submitted"));
  check(read32(patched, 0x2872C) == 0x00000000u, QStringLiteral("diamond primitive C not submitted"));
  check(read32(patched, 0x287F8) == 0x00002021u, QStringLiteral("loop clear red remains black"));
  check(read32(patched, 0x287FC) == 0x00002821u, QStringLiteral("loop clear green remains black"));
  check(read32(patched, 0x28804) == 0x00003021u, QStringLiteral("loop clear blue remains black"));
  check(read32(patched, 0x289FC) == 0x240B00F0u, QStringLiteral("Microsoft quad vertically centered"));
  check(read32(patched, 0x28A58) == 0x00000000u, QStringLiteral("current-buffer Microsoft quad retained for sizing"));
  check(read32(patched, 0x28A5C) == 0x24050280u, QStringLiteral("Microsoft quad width is 640"));
  check(read32(patched, 0x28A60) == 0x0C011262u, QStringLiteral("quad size helper called"));
  check(read32(patched, 0x28A64) == 0x240601E0u, QStringLiteral("Microsoft quad height is 480"));
  check(read32(patched, 0x28A68) == 0x10000033u, QStringLiteral("unused text positioning skipped"));

  // First animation keeps the existing Microsoft frame, removes its model,
  // while handover explicitly clears that frame before drawing MVII.
  check(patched.mid(0x5E2D0, 12) == QByteArray("PSXBRGATE001", 12),
        QStringLiteral("branding presentation gate marker installed"));
  check(read32(patched, 0x29B34) == 0xAFA000A8u, QStringLiteral("SCE diamond model count suppressed"));
  check(read32(patched, 0x29BCC) == 0xA020BC9Fu, QStringLiteral("handover clear setup left stock"));
  check(read32(patched, 0x29BD0) == 0x3C01800Fu, QStringLiteral("handover control flow left stock"));
  check(read32(patched, 0x29BD8) == 0xA020A87Fu, QStringLiteral("handover draw environment left stock"));
  check(read32(patched, 0x186E4) == 0x24040001u, QStringLiteral("logo caller mode left stock"));
  check(read32(patched, 0x186FC) == 0x24040001u, QStringLiteral("alternate logo caller left stock"));
  check(read32(patched, 0x27434) == 0x3C0E8008u, QStringLiteral("handover source selector left stock"));
  check(read32(patched, 0x27438) == 0x8DCE9DECu, QStringLiteral("handover region selector left stock"));

  check(patched.mid(0x4EF60, 0xFE0) != original.mid(0x4EF60, 0xFE0),
        QStringLiteral("registered handover TIM changed"));
  check(patched.mid(0x4FF40, 0xFE0) != original.mid(0x4FF40, 0xFE0),
        QStringLiteral("trademark handover TIM changed"));
  check(patched.mid(0x4EF60, 0xFE0) == patched.mid(0x4FF40, 0xFE0),
        QStringLiteral("both handover variants use the same selected image"));
  check(read32(patched, 0x27478) == 0x0C0110BCu, QStringLiteral("blank trademark sprite load retained"));
  check(read32(patched, 0x2756C) == 0x0C0111FDu, QStringLiteral("blank trademark position retained"));
  check(allZero(patched.mid(0x50F20 + 64, 80)), QStringLiteral("handover trademark pixels blank"));

  check(read32(patched, 0x274AC) == 0x240F00F0u, QStringLiteral("MVII vertically centered"));
  check(read32(patched, 0x274B8) == 0x24050140u, QStringLiteral("MVII horizontally centered"));
  check(read32(patched, 0x274F0) == 0x24180026u, QStringLiteral("MVII native height preserved"));
  check(read32(patched, 0x274FC) == 0x240500C8u, QStringLiteral("MVII native width preserved"));
  check(read32(patched, 0x27868) == 0x03E00008u, QStringLiteral("PS glyph builder patched"));
  check(read32(patched, 0x3B230) == 0xA7000188u, QStringLiteral("SPU key-on low patched"));
  check(read32(patched, 0x3C3A4) == 0xA600018Au, QStringLiteral("SPU key-on high patched"));
  check(QFile::exists(manifestPath), QStringLiteral("manifest exists"));

  if (!failures) qInfo() << "BIOS branding patcher tests passed";
  return failures ? 1 : 0;
}
