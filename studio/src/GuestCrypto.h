#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace psxstudio::crypto {

// AES, because opening a PSN package or an NSP requires it and nothing Studio
// already links supplies it: Qt has no block cipher, and the vendored QuaZip is
// built without its AES sources. This is the plain FIPS-197 cipher plus the two
// modes the two containers use — CTR for PKG items and NCA sections, XTS for
// NCA headers — and nothing else.
//
// It is used for reading formats, never for protecting anything, so it is
// written for clarity over speed and makes no constant-time claim.

class Aes {
 public:
  Aes() = default;
  explicit Aes(const QByteArray& key) { setKey(key); }

  // 16 or 32 bytes (AES-128 / AES-256). Any other length leaves the object
  // invalid rather than truncating or padding a key that was misread.
  bool setKey(const QByteArray& key);
  bool isValid() const { return rounds_ != 0; }

  void encryptBlock(const quint8 in[16], quint8 out[16]) const;
  void decryptBlock(const quint8 in[16], quint8 out[16]) const;

 private:
  quint8 encKeys_[15 * 16]{};
  quint8 decKeys_[15 * 16]{};
  int rounds_{ 0 };
};

/* Whole-buffer ECB. `data` must be a multiple of 16 bytes; a short buffer
 * returns empty rather than a partly-transformed one. Used for the two key
 * derivations these formats specify — the PKG item key and the NSP title
 * key — both of which are exactly one block. */
QByteArray aesEcbEncrypt(const QByteArray& key, const QByteArray& data);
QByteArray aesEcbDecrypt(const QByteArray& key, const QByteArray& data);

/* AES-CTR positioned by absolute offset, so a caller can decrypt one item out
 * of the middle of a package without walking the stream to get there. The
 * counter is the value at offset 0 and is incremented as a 128-bit big-endian
 * integer, which is what both formats do. */
class CtrStream {
 public:
  CtrStream(const QByteArray& key, const QByteArray& baseCounter);

  bool isValid() const { return aes_.isValid(); }
  void seek(quint64 offset);
  void apply(char* data, qsizetype size);  // XOR in place; CTR is symmetric

 private:
  void loadCounter(quint64 block);
  void nextBlock();

  Aes aes_;
  quint8 base_[16]{};
  quint8 counter_[16]{};
  quint8 keystream_[16]{};
  int used_{ 16 };
};

/* AES-XTS as Nintendo uses it for NCA headers: `key` is data-key||tweak-key
 * (32 bytes for AES-128-XTS), the data unit is `sectorSize`, and the tweak is
 * the sector number written big-endian — the reverse of the IEEE 1619 order,
 * which is why this cannot be a stock XTS call.
 *
 * `size` must be a whole number of sectors and each sector a whole number of
 * blocks; anything else returns false rather than decrypting part of it. */
bool aesXtsDecryptSectors(const QByteArray& key,
                          char* data,
                          qsizetype size,
                          quint64 firstSector,
                          qsizetype sectorSize);

}  // namespace psxstudio::crypto
