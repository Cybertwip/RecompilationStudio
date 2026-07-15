#include "BiosBrandingPatcher.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

namespace psxstudio {

namespace {

constexpr qint64 kBiosSize = 512 * 1024;
constexpr auto kCanonicalSha256 = "71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3";
constexpr int kBrandingGateMarkerOffset = 0x5E2D0;
constexpr std::array<char, 16> kBrandingGateMarker{
  'P', 'S', 'X', 'B', 'R', 'G', 'A', 'T', 'E', '0', '0', '1',
  0, 0, 0, 1
};

quint16 read16(const QByteArray& bytes, int offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint16>(p[0] | (p[1] << 8));
}

quint32 read32(const QByteArray& bytes, int offset) {
  const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + offset);
  return static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8) |
         (static_cast<quint32>(p[2]) << 16) | (static_cast<quint32>(p[3]) << 24);
}

void write16(QByteArray& bytes, int offset, quint16 value) {
  auto* p = reinterpret_cast<uchar*>(bytes.data() + offset);
  p[0] = static_cast<uchar>(value & 0xffu);
  p[1] = static_cast<uchar>(value >> 8);
}

void write32(QByteArray& bytes, int offset, quint32 value) {
  auto* p = reinterpret_cast<uchar*>(bytes.data() + offset);
  p[0] = static_cast<uchar>(value & 0xffu);
  p[1] = static_cast<uchar>((value >> 8) & 0xffu);
  p[2] = static_cast<uchar>((value >> 16) & 0xffu);
  p[3] = static_cast<uchar>((value >> 24) & 0xffu);
}

QString sha256(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString sha256File(const QString& path, QString& error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read %1: %2").arg(path, file.errorString());
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) {
    error = QStringLiteral("Could not hash %1.").arg(path);
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

struct TimInfo {
  int offset{};
  int clutDataOffset{};
  int imageDataOffset{};
  int width{};
  int height{};
  int imageWords{};
  int totalSize{};
  quint16 clutX{};
  quint16 clutY{};
  quint16 imageX{};
  quint16 imageY{};
};

bool parseTim(const QByteArray& bios, int offset, int expectedWidth, int expectedHeight,
              TimInfo& info, QString& error) {
  if (offset < 0 || offset + 32 > bios.size() || read32(bios, offset) != 0x10u ||
      read32(bios, offset + 4) != 0x08u) {
    error = QStringLiteral("Expected a 4-bit TIM at BIOS offset 0x%1.").arg(offset, 0, 16);
    return false;
  }
  const quint32 clutLength = read32(bios, offset + 8);
  const quint16 clutX = read16(bios, offset + 12);
  const quint16 clutY = read16(bios, offset + 14);
  const quint16 clutWidth = read16(bios, offset + 16);
  const quint16 clutHeight = read16(bios, offset + 18);
  if (clutLength != 44u || clutWidth != 16u || clutHeight != 1u) {
    error = QStringLiteral("Unexpected TIM CLUT layout at BIOS offset 0x%1.").arg(offset, 0, 16);
    return false;
  }
  const int imageBlock = offset + 8 + static_cast<int>(clutLength);
  const quint32 imageLength = read32(bios, imageBlock);
  const quint16 imageX = read16(bios, imageBlock + 4);
  const quint16 imageY = read16(bios, imageBlock + 6);
  const quint16 imageWords = read16(bios, imageBlock + 8);
  const quint16 height = read16(bios, imageBlock + 10);
  const int width = imageWords * 4;
  if (width != expectedWidth || height != expectedHeight ||
      imageLength != static_cast<quint32>(12 + imageWords * height * 2)) {
    error = QStringLiteral("Unexpected TIM dimensions at BIOS offset 0x%1: %2x%3.")
              .arg(offset, 0, 16).arg(width).arg(height);
    return false;
  }
  const int total = imageBlock + static_cast<int>(imageLength) - offset;
  if (offset + total > bios.size()) {
    error = QStringLiteral("TIM at BIOS offset 0x%1 is truncated.").arg(offset, 0, 16);
    return false;
  }
  info = { offset, offset + 20, imageBlock + 12, width, height,
           static_cast<int>(imageWords), total, clutX, clutY, imageX, imageY };
  return true;
}

QImage readImage(const QString& path, QString& error) {
  QImageReader reader(path);
  reader.setAutoTransform(true);
  QImage image = reader.read();
  if (image.isNull()) {
    error = QStringLiteral("Could not decode image %1: %2").arg(path, reader.errorString());
    return {};
  }
  return image.convertToFormat(QImage::Format_ARGB32);
}

int colorDistance(QRgb a, QRgb b) {
  const int dr = qRed(a) - qRed(b);
  const int dg = qGreen(a) - qGreen(b);
  const int db = qBlue(a) - qBlue(b);
  return dr * dr + dg * dg + db * db;
}

QRect contentBounds(const QImage& image, QRgb background, int threshold, bool brightnessMode) {
  int minX = image.width(), minY = image.height(), maxX = -1, maxY = -1;
  const int thresholdSq = threshold * threshold;
  for (int y = 0; y < image.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const QRgb pixel = row[x];
      const bool content = brightnessMode
        ? std::max({ qRed(pixel), qGreen(pixel), qBlue(pixel) }) > threshold
        : colorDistance(pixel, background) > thresholdSq;
      if (!content) continue;
      minX = std::min(minX, x); minY = std::min(minY, y);
      maxX = std::max(maxX, x); maxY = std::max(maxY, y);
    }
  }
  if (maxX < minX || maxY < minY) return image.rect();
  QRect bounds(QPoint(minX, minY), QPoint(maxX, maxY));
  const int padX = std::max(2, bounds.width() / 25);
  const int padY = std::max(2, bounds.height() / 12);
  bounds.adjust(-padX, -padY, padX, padY);
  return bounds.intersected(image.rect());
}

QImage makeInitialSplash(const QImage& source, int width, int height) {
  return source.scaled(width, height, Qt::IgnoreAspectRatio, Qt::FastTransformation)
               .convertToFormat(QImage::Format_ARGB32);
}

QImage makeHandoffSplash(const QImage& source) {
  const QImage content = source.copy(contentBounds(source, qRgb(0, 0, 0), 30, true));
  QImage target(200, 40, QImage::Format_ARGB32);
  target.fill(Qt::transparent);
  const QImage scaled = content.scaled(194, 38, Qt::KeepAspectRatio, Qt::FastTransformation);
  QPainter painter(&target);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.drawImage((target.width() - scaled.width()) / 2,
                    (target.height() - scaled.height()) / 2, scaled);
  painter.end();
  for (int y = 0; y < target.height(); ++y) {
    auto* row = reinterpret_cast<QRgb*>(target.scanLine(y));
    for (int x = 0; x < target.width(); ++x) {
      const int peak = std::max({ qRed(row[x]), qGreen(row[x]), qBlue(row[x]) });
      if (peak < 10) row[x] = qRgba(0, 0, 0, 0);
      else row[x] = qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), 255);
    }
  }
  return target;
}

struct WeightedColor { int r{}, g{}, b{}, count{}; };
struct ColorBox { std::vector<int> indices; int count{}; int rMin{}, rMax{}, gMin{}, gMax{}, bMin{}, bMax{}; };

void updateBox(ColorBox& box, const std::vector<WeightedColor>& colors) {
  box.count = 0; box.rMin = box.gMin = box.bMin = 255;
  box.rMax = box.gMax = box.bMax = 0;
  for (int index : box.indices) {
    const auto& c = colors[index]; box.count += c.count;
    box.rMin = std::min(box.rMin, c.r); box.rMax = std::max(box.rMax, c.r);
    box.gMin = std::min(box.gMin, c.g); box.gMax = std::max(box.gMax, c.g);
    box.bMin = std::min(box.bMin, c.b); box.bMax = std::max(box.bMax, c.b);
  }
}

std::vector<QRgb> makePalette(const QImage& image, int colorCount, bool transparent) {
  std::array<int, 32768> histogram{};
  for (int y = 0; y < image.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      if (transparent && qAlpha(row[x]) < 128) continue;
      const int key = (qRed(row[x]) >> 3) | ((qGreen(row[x]) >> 3) << 5) |
                      ((qBlue(row[x]) >> 3) << 10);
      histogram[key]++;
    }
  }
  std::vector<WeightedColor> colors;
  for (int key = 0; key < static_cast<int>(histogram.size()); ++key) {
    if (!histogram[key]) continue;
    colors.push_back({ ((key & 31) << 3) | 4, (((key >> 5) & 31) << 3) | 4,
                       (((key >> 10) & 31) << 3) | 4, histogram[key] });
  }
  if (colors.empty()) colors.push_back({ 255, 255, 255, 1 });
  ColorBox first;
  first.indices.resize(colors.size());
  std::iota(first.indices.begin(), first.indices.end(), 0);
  updateBox(first, colors);
  std::vector<ColorBox> boxes{ first };
  while (static_cast<int>(boxes.size()) < colorCount) {
    auto best = boxes.end();
    long long bestScore = -1;
    for (auto it = boxes.begin(); it != boxes.end(); ++it) {
      if (it->indices.size() < 2) continue;
      const int range = std::max({ it->rMax - it->rMin, it->gMax - it->gMin,
                                   it->bMax - it->bMin });
      const long long score = static_cast<long long>(range + 1) * it->count;
      if (score > bestScore) { bestScore = score; best = it; }
    }
    if (best == boxes.end()) break;
    ColorBox box = *best;
    boxes.erase(best);
    const int rRange = box.rMax - box.rMin, gRange = box.gMax - box.gMin,
              bRange = box.bMax - box.bMin;
    const int channel = (rRange >= gRange && rRange >= bRange) ? 0 : (gRange >= bRange ? 1 : 2);
    std::sort(box.indices.begin(), box.indices.end(), [&](int lhs, int rhs) {
      const auto& a = colors[lhs]; const auto& b = colors[rhs];
      return channel == 0 ? a.r < b.r : channel == 1 ? a.g < b.g : a.b < b.b;
    });
    int accumulated = 0, split = 0;
    for (; split < static_cast<int>(box.indices.size()) - 1; ++split) {
      accumulated += colors[box.indices[split]].count;
      if (accumulated * 2 >= box.count) { ++split; break; }
    }
    ColorBox left, right;
    left.indices.assign(box.indices.begin(), box.indices.begin() + split);
    right.indices.assign(box.indices.begin() + split, box.indices.end());
    updateBox(left, colors); updateBox(right, colors);
    boxes.push_back(std::move(left)); boxes.push_back(std::move(right));
  }
  std::vector<QRgb> palette;
  if (transparent) palette.push_back(qRgba(0, 0, 0, 0));
  for (const auto& box : boxes) {
    long long r = 0, g = 0, b = 0, count = 0;
    for (int index : box.indices) {
      const auto& c = colors[index];
      r += static_cast<long long>(c.r) * c.count;
      g += static_cast<long long>(c.g) * c.count;
      b += static_cast<long long>(c.b) * c.count;
      count += c.count;
    }
    palette.push_back(qRgb(static_cast<int>(r / count), static_cast<int>(g / count),
                           static_cast<int>(b / count)));
  }
  while (palette.size() < 16) palette.push_back(palette.back());
  palette.resize(16);
  return palette;
}

int nearestColor(QRgb pixel, const std::vector<QRgb>& palette, int first) {
  int best = first;
  int bestDistance = std::numeric_limits<int>::max();
  for (int i = first; i < static_cast<int>(palette.size()); ++i) {
    const int distance = colorDistance(pixel, palette[i]);
    if (distance < bestDistance) { bestDistance = distance; best = i; }
  }
  return best;
}

quint16 toPsxColor(QRgb color, bool transparent) {
  if (transparent) return 0;
  quint16 value = static_cast<quint16>((qRed(color) >> 3) |
                                      ((qGreen(color) >> 3) << 5) |
                                      ((qBlue(color) >> 3) << 10));
  if ((value & 0x7fffu) == 0) value = 1;
  return value;
}

bool patchTim(QByteArray& bios, int offset, int width, int height, const QImage& image,
              bool transparent, const QString& description, QJsonArray& patches, QString& error) {
  TimInfo info;
  if (!parseTim(bios, offset, width, height, info, error)) return false;
  if (image.width() != width || image.height() != height) {
    error = QStringLiteral("Internal image size mismatch for %1.").arg(description);
    return false;
  }
  const QByteArray before = bios.mid(offset, info.totalSize);
  const auto palette = makePalette(image, transparent ? 15 : 16, transparent);
  for (int i = 0; i < 16; ++i)
    write16(bios, info.clutDataOffset + i * 2, toPsxColor(palette[i], transparent && i == 0));
  int outputOffset = info.imageDataOffset;
  for (int y = 0; y < height; ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < width; x += 2) {
      const int low = transparent && qAlpha(row[x]) < 128 ? 0 : nearestColor(row[x], palette, transparent ? 1 : 0);
      const int high = transparent && qAlpha(row[x + 1]) < 128 ? 0 : nearestColor(row[x + 1], palette, transparent ? 1 : 0);
      bios[outputOffset++] = static_cast<char>(low | (high << 4));
    }
  }
  const QByteArray after = bios.mid(offset, info.totalSize);
  patches.append(QJsonObject{
    { QStringLiteral("type"), QStringLiteral("tim") },
    { QStringLiteral("description"), description },
    { QStringLiteral("offset"), QStringLiteral("0x%1").arg(offset, 0, 16).toUpper() },
    { QStringLiteral("size"), info.totalSize },
    { QStringLiteral("width"), width }, { QStringLiteral("height"), height },
    { QStringLiteral("before_sha256"), sha256(before) },
    { QStringLiteral("after_sha256"), sha256(after) },
  });
  return true;
}

bool rewriteTimSlot(QByteArray& bios, const TimInfo& original, int slotEnd,
                    const QImage& image, const std::vector<QRgb>& palette,
                    bool transparent, const QString& description,
                    QJsonArray& patches, QString& error) {
  if (image.width() <= 0 || image.height() <= 0 || (image.width() & 3) != 0 ||
      palette.size() != 16 || slotEnd <= original.offset || slotEnd > bios.size()) {
    error = QStringLiteral("Invalid replacement TIM slot for %1.").arg(description);
    return false;
  }
  const int imageWords = image.width() / 4;
  const int encodedSize = 64 + imageWords * image.height() * 2;
  const int slotSize = slotEnd - original.offset;
  if (encodedSize > slotSize) {
    error = QStringLiteral("Replacement TIM for %1 needs %2 bytes but its verified slot has %3.")
              .arg(description).arg(encodedSize).arg(slotSize);
    return false;
  }

  const QByteArray before = bios.mid(original.offset, slotSize);
  std::fill(bios.begin() + original.offset, bios.begin() + slotEnd, char(0));
  write32(bios, original.offset, 0x10u);
  write32(bios, original.offset + 4, 0x08u);
  write32(bios, original.offset + 8, 44u);
  write16(bios, original.offset + 12, original.clutX);
  write16(bios, original.offset + 14, original.clutY);
  write16(bios, original.offset + 16, 16);
  write16(bios, original.offset + 18, 1);
  for (int i = 0; i < 16; ++i)
    write16(bios, original.offset + 20 + i * 2,
            toPsxColor(palette[i], transparent && i == 0));

  const int imageBlock = original.offset + 52;
  write32(bios, imageBlock, 12 + imageWords * image.height() * 2);
  write16(bios, imageBlock + 4, original.imageX);
  write16(bios, imageBlock + 6, original.imageY);
  write16(bios, imageBlock + 8, imageWords);
  write16(bios, imageBlock + 10, image.height());
  int outputOffset = imageBlock + 12;
  for (int y = 0; y < image.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); x += 2) {
      const int low = transparent && qAlpha(row[x]) < 128
        ? 0 : nearestColor(row[x], palette, transparent ? 1 : 0);
      const int high = transparent && qAlpha(row[x + 1]) < 128
        ? 0 : nearestColor(row[x + 1], palette, transparent ? 1 : 0);
      bios[outputOffset++] = static_cast<char>(low | (high << 4));
    }
  }

  const QByteArray after = bios.mid(original.offset, slotSize);
  patches.append(QJsonObject{
    { QStringLiteral("type"), QStringLiteral("tim_slot") },
    { QStringLiteral("description"), description },
    { QStringLiteral("offset"), QStringLiteral("0x%1").arg(original.offset, 0, 16).toUpper() },
    { QStringLiteral("slot_size"), slotSize },
    { QStringLiteral("encoded_size"), encodedSize },
    { QStringLiteral("width"), image.width() },
    { QStringLiteral("height"), image.height() },
    { QStringLiteral("before_sha256"), sha256(before) },
    { QStringLiteral("after_sha256"), sha256(after) },
  });
  return true;
}

bool patchExpandedInitialTims(QByteArray& bios, const QImage& image,
                              QJsonArray& patches, QString& error) {
  TimInfo sony, computer, trademark;
  if (!parseTim(bios, 0x5CC30, 240, 48, sony, error) ||
      !parseTim(bios, 0x5E2F0, 240, 60, computer, error) ||
      !parseTim(bios, 0x5FF50, 24, 12, trademark, error)) return false;
  if (image.width() != 120 || image.height() != 90) {
    error = QStringLiteral("Internal image size mismatch for expanded initial splash.");
    return false;
  }

  // The stock object is a POLY_FT4, not a fixed-size SPRT. Keep a legal
  // eight-bit 120x90 source texture in the exact SONY slot and scale its quad
  // to the 640x480 render area in the verified setup routine below.
  const auto palette = makePalette(image, 16, false);
  QImage blankComputer(240, 60, QImage::Format_ARGB32);
  blankComputer.fill(Qt::transparent);
  QImage blankTrademark(24, 12, QImage::Format_ARGB32);
  blankTrademark.fill(Qt::transparent);
  return rewriteTimSlot(bios, sony, computer.offset,
                        image, palette, false,
                        QStringLiteral("Full-frame initial splash source texture"), patches, error) &&
         patchTim(bios, computer.offset, 240, 60, blankComputer, true,
                  QStringLiteral("Remove COMPUTER ENTERTAINMENT TIM"), patches, error) &&
         patchTim(bios, trademark.offset, 24, 12, blankTrademark, true,
                  QStringLiteral("Remove initial trademark TIM"), patches, error);
}

bool blankTim(QByteArray& bios, int offset, int width, int height, const QString& description,
              QJsonArray& patches, QString& error) {
  QImage blank(width, height, QImage::Format_ARGB32);
  blank.fill(Qt::transparent);
  return patchTim(bios, offset, width, height, blank, true, description, patches, error);
}

bool patchWord(QByteArray& bios, int offset, quint32 expected, quint32 replacement,
               const QString& description, QJsonArray& patches, QString& error) {
  if (offset < 0 || offset + 4 > bios.size() || read32(bios, offset) != expected) {
    error = QStringLiteral("Original instruction mismatch at BIOS offset 0x%1 for %2.")
              .arg(offset, 0, 16).arg(description);
    return false;
  }
  write32(bios, offset, replacement);
  patches.append(QJsonObject{
    { QStringLiteral("type"), QStringLiteral("instruction") },
    { QStringLiteral("description"), description },
    { QStringLiteral("offset"), QStringLiteral("0x%1").arg(offset, 0, 16).toUpper() },
    { QStringLiteral("before"), QStringLiteral("0x%1").arg(expected, 8, 16, QLatin1Char('0')).toUpper() },
    { QStringLiteral("after"), QStringLiteral("0x%1").arg(replacement, 8, 16, QLatin1Char('0')).toUpper() },
  });
  return true;
}

bool installBrandingGateMarker(QByteArray& bios, QJsonArray& patches, QString& error) {
  if (kBrandingGateMarkerOffset < 0 ||
      kBrandingGateMarkerOffset + static_cast<int>(kBrandingGateMarker.size()) > bios.size()) {
    error = QStringLiteral("Branding presentation marker is outside the BIOS image.");
    return false;
  }
  const QByteArray before = bios.mid(kBrandingGateMarkerOffset,
                                    static_cast<int>(kBrandingGateMarker.size()));
  if (!std::all_of(before.cbegin(), before.cend(), [](char value) { return value == 0; })) {
    error = QStringLiteral("Branding presentation marker slot is not empty.");
    return false;
  }
  std::copy(kBrandingGateMarker.cbegin(), kBrandingGateMarker.cend(),
            bios.begin() + kBrandingGateMarkerOffset);
  const QByteArray after = bios.mid(kBrandingGateMarkerOffset,
                                   static_cast<int>(kBrandingGateMarker.size()));
  patches.append(QJsonObject{
    { QStringLiteral("type"), QStringLiteral("branding_present_gate") },
    { QStringLiteral("description"),
      QStringLiteral("Mark BIOS for frontend black-until-logo presentation gating") },
    { QStringLiteral("offset"),
      QStringLiteral("0x%1").arg(kBrandingGateMarkerOffset, 0, 16).toUpper() },
    { QStringLiteral("before_sha256"), sha256(before) },
    { QStringLiteral("after_sha256"), sha256(after) },
  });
  return true;
}

} // namespace

bool BiosBrandingPatcher::patch(const BiosBrandingPatchOptions& options, QString& error) {
  QFile input(options.inputBiosPath);
  if (!input.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read BIOS: %1").arg(input.errorString());
    return false;
  }
  QByteArray bios = input.readAll();
  input.close();
  if (bios.size() != kBiosSize || sha256(bios) != QString::fromLatin1(kCanonicalSha256)) {
    error = QStringLiteral("BIOS branding patch requires the canonical 512 KiB SCPH1001.BIN.");
    return false;
  }
  if (options.initialSplashImagePath.isEmpty() != options.handoffImagePath.isEmpty()) {
    error = QStringLiteral("BIOS branding requires both the initial and handover images.");
    return false;
  }

  QJsonArray patches;
  QJsonObject sourceImages;
  if (!options.initialSplashImagePath.isEmpty()) {
    QImage image = readImage(options.initialSplashImagePath, error);
    if (image.isNull()) return false;
    const QImage splash = makeInitialSplash(image, 120, 90);
    if (!patchExpandedInitialTims(bios, splash, patches, error)) return false;
    const struct { int offset; quint32 before; quint32 after; const char* description; } initialSetupPatches[] = {
      { 0x28570, 0x0C0102E1u, 0x00000000u, "Skip earlier three-polygon SCE diamond builder" },
      { 0x28594, 0x240400B4u, 0x00002021u, "Clear first-logo renderer to black (red)" },
      { 0x28598, 0x240500B4u, 0x00002821u, "Clear first-logo renderer to black (green)" },
      { 0x285A0, 0x240600B4u, 0x00003021u, "Clear first-logo renderer to black (blue)" },
      { 0x286BC, 0x0C014601u, 0x00000000u, "Skip earlier SCE diamond primitive submission A" },
      { 0x286F4, 0x0C014601u, 0x00000000u, "Skip earlier SCE diamond primitive submission B" },
      { 0x2872C, 0x0C014601u, 0x00000000u, "Skip earlier SCE diamond primitive submission C" },
      { 0x287F8, 0x240400B4u, 0x00002021u, "Keep first-logo renderer black (red)" },
      { 0x287FC, 0x240500B4u, 0x00002821u, "Keep first-logo renderer black (green)" },
      { 0x28804, 0x240600B4u, 0x00003021u, "Keep first-logo renderer black (blue)" },
      { 0x289FC, 0x240B0050u, 0x240B00F0u, "Center full-frame Microsoft quad vertically" },
      { 0x28A58, 0x12600004u, 0x00000000u, "Retain current-buffer Microsoft quad for full-frame sizing" },
      { 0x28A5C, 0x24020002u, 0x24050280u, "Set Microsoft quad width to 640" },
      { 0x28A60, 0x10000002u, 0x0C011262u, "Call textured-quad size helper" },
      { 0x28A64, 0x24020001u, 0x240601E0u, "Set Microsoft quad height to 480" },
      { 0x28A68, 0x24020002u, 0x10000033u, "Skip unused initial text-quad positioning" },
      { 0x28A6C, 0x240D019Cu, 0x00000000u, "Full-frame setup branch delay slot" },
      { 0x29B34, 0xAFA200A8u, 0xAFA000A8u, "Suppress SCE diamond model count while preserving the replacement render loop" },
    };
    for (const auto& patch : initialSetupPatches)
      if (!patchWord(bios, patch.offset, patch.before, patch.after,
                     QString::fromLatin1(patch.description), patches, error)) return false;
    sourceImages.insert(QStringLiteral("initial"), QJsonObject{
      { QStringLiteral("path"), options.initialSplashImagePath },
      { QStringLiteral("sha256"), sha256File(options.initialSplashImagePath, error) }
    });
    if (!error.isEmpty()) return false;
    if (!installBrandingGateMarker(bios, patches, error)) return false;
  }
  if (!options.handoffImagePath.isEmpty()) {
    QImage image = readImage(options.handoffImagePath, error);
    if (image.isNull()) return false;
    const QImage splash = makeHandoffSplash(image);
    if (!patchTim(bios, 0x4EF60, 200, 40, splash, true,
                  QStringLiteral("Replace registered handover wordmark TIM"), patches, error) ||
        !patchTim(bios, 0x4FF40, 200, 40, splash, true,
                  QStringLiteral("Replace trademark handover wordmark TIM"), patches, error) ||
        !blankTim(bios, 0x50F20, 20, 8,
                  QStringLiteral("Remove handoff trademark TIM"), patches, error)) return false;
    const struct { int offset; quint32 before; quint32 after; const char* description; } handoffSetupPatches[] = {
      { 0x274AC, 0x240F0122u, 0x240F00F0u, "Center MVII handoff sprite vertically" },
      { 0x274B8, 0x24050148u, 0x24050140u, "Center MVII handoff sprite horizontally" },
    };
    for (const auto& patch : handoffSetupPatches)
      if (!patchWord(bios, patch.offset, patch.before, patch.after,
                     QString::fromLatin1(patch.description), patches, error)) return false;
    sourceImages.insert(QStringLiteral("handoff"), QJsonObject{
      { QStringLiteral("path"), options.handoffImagePath },
      { QStringLiteral("sha256"), sha256File(options.handoffImagePath, error) }
    });
    if (!error.isEmpty()) return false;
  }
  if (options.removeStockPsGlyph) {
    if (!patchWord(bios, 0x27868, 0x27BDFFC0u, 0x03E00008u,
                   QStringLiteral("Return immediately from exclusive PS-glyph primitive builder"), patches, error) ||
        !patchWord(bios, 0x2786C, 0xAFBF003Cu, 0x00000000u,
                   QStringLiteral("PS-glyph early-return delay slot"), patches, error)) return false;
  }
  if (options.muteBootAudio) {
    const struct { int offset; quint32 before; quint32 after; const char* description; } audioPatches[] = {
      { 0x3B230, 0xA70D0188u, 0xA7000188u, "Suppress BIOS-shell SPU key-on low path A" },
      { 0x3B244, 0xA72E018Au, 0xA720018Au, "Suppress BIOS-shell SPU key-on high path A" },
      { 0x3C390, 0xA60B0188u, 0xA6000188u, "Suppress BIOS-shell SPU key-on low path B" },
      { 0x3C3A4, 0xA60D018Au, 0xA600018Au, "Suppress BIOS-shell SPU key-on high path B" },
    };
    for (const auto& patch : audioPatches) {
      if (!patchWord(bios, patch.offset, patch.before, patch.after,
                     QString::fromLatin1(patch.description), patches, error)) return false;
    }
  }

  QSaveFile output(options.outputBiosPath);
  if (!output.open(QIODevice::WriteOnly) || output.write(bios) != bios.size() || !output.commit()) {
    error = QStringLiteral("Could not write patched BIOS to %1: %2")
              .arg(options.outputBiosPath, output.errorString());
    return false;
  }

  if (!options.manifestPath.isEmpty()) {
    QJsonObject manifest{
      { QStringLiteral("schema"), 1 },
      { QStringLiteral("patcher"), QStringLiteral("PSXRecomp BIOS Branding Patcher") },
      { QStringLiteral("input_bios"), options.inputBiosPath },
      { QStringLiteral("input_sha256"), QString::fromLatin1(kCanonicalSha256) },
      { QStringLiteral("output_bios"), options.outputBiosPath },
      { QStringLiteral("output_sha256"), sha256(bios) },
      { QStringLiteral("mute_boot_audio"), options.muteBootAudio },
      { QStringLiteral("remove_stock_ps_glyph"), options.removeStockPsGlyph },
      { QStringLiteral("source_images"), sourceImages },
      { QStringLiteral("patches"), patches },
    };
    QSaveFile manifestFile(options.manifestPath);
    const QByteArray json = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(json) != json.size() ||
        !manifestFile.commit()) {
      error = QStringLiteral("Patched BIOS was written, but the manifest could not be saved: %1")
                .arg(manifestFile.errorString());
      return false;
    }
  }
  return true;
}

} // namespace psxstudio
