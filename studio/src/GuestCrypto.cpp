#include "GuestCrypto.h"

#include <cstring>

namespace psxstudio::crypto {

namespace {

const quint8 kSbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

const quint8 kInvSbox[256] = {
  0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
  0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
  0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
  0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
  0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
  0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
  0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
  0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
  0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
  0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
  0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
  0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
  0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
  0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
  0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
  0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

inline quint8 xtime(quint8 value) {
  return static_cast<quint8>((value << 1) ^ ((value & 0x80u) ? 0x1Bu : 0x00u));
}

quint8 gfMul(quint8 a, quint8 b) {
  quint8 result = 0;
  while (b != 0) {
    if (b & 1u) result ^= a;
    a = xtime(a);
    b >>= 1u;
  }
  return result;
}

void addRoundKey(quint8 state[16], const quint8* roundKey) {
  for (int i = 0; i < 16; ++i) state[i] ^= roundKey[i];
}

void subShift(quint8 state[16]) {
  quint8 out[16];
  // SubBytes and ShiftRows together: column-major state, row r rotated left r.
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      out[column * 4 + row] = kSbox[state[((column + row) % 4) * 4 + row]];
    }
  }
  std::memcpy(state, out, 16);
}

void invSubShift(quint8 state[16]) {
  quint8 out[16];
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      out[((column + row) % 4) * 4 + row] = kInvSbox[state[column * 4 + row]];
    }
  }
  std::memcpy(state, out, 16);
}

void mixColumns(quint8 state[16]) {
  for (int column = 0; column < 4; ++column) {
    quint8* c = state + column * 4;
    const quint8 a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
    c[0] = static_cast<quint8>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
    c[1] = static_cast<quint8>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
    c[2] = static_cast<quint8>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
    c[3] = static_cast<quint8>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
  }
}

void invMixColumns(quint8 state[16]) {
  for (int column = 0; column < 4; ++column) {
    quint8* c = state + column * 4;
    const quint8 a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
    c[0] = static_cast<quint8>(gfMul(a0, 14) ^ gfMul(a1, 11) ^ gfMul(a2, 13) ^ gfMul(a3, 9));
    c[1] = static_cast<quint8>(gfMul(a0, 9) ^ gfMul(a1, 14) ^ gfMul(a2, 11) ^ gfMul(a3, 13));
    c[2] = static_cast<quint8>(gfMul(a0, 13) ^ gfMul(a1, 9) ^ gfMul(a2, 14) ^ gfMul(a3, 11));
    c[3] = static_cast<quint8>(gfMul(a0, 11) ^ gfMul(a1, 13) ^ gfMul(a2, 9) ^ gfMul(a3, 14));
  }
}

}  // namespace

bool Aes::setKey(const QByteArray& key) {
  rounds_ = 0;
  const int keyWords = key.size() == 16 ? 4 : (key.size() == 32 ? 8 : 0);
  if (keyWords == 0) return false;
  const int rounds = keyWords == 4 ? 10 : 14;
  const int total = (rounds + 1) * 4;  // words

  auto* words = encKeys_;
  std::memcpy(words, key.constData(), static_cast<size_t>(key.size()));
  quint8 rcon = 1;
  for (int word = keyWords; word < total; ++word) {
    quint8 temp[4];
    std::memcpy(temp, words + (word - 1) * 4, 4);
    if (word % keyWords == 0) {
      const quint8 rotated = temp[0];
      temp[0] = static_cast<quint8>(kSbox[temp[1]] ^ rcon);
      temp[1] = kSbox[temp[2]];
      temp[2] = kSbox[temp[3]];
      temp[3] = kSbox[rotated];
      rcon = xtime(rcon);
    } else if (keyWords == 8 && word % keyWords == 4) {
      for (int i = 0; i < 4; ++i) temp[i] = kSbox[temp[i]];
    }
    for (int i = 0; i < 4; ++i) {
      words[word * 4 + i] =
        static_cast<quint8>(words[(word - keyWords) * 4 + i] ^ temp[i]);
    }
  }

  // The equivalent inverse cipher's schedule: the same round keys, every one
  // but the first and last passed through InvMixColumns.
  std::memcpy(decKeys_, encKeys_, static_cast<size_t>(total) * 4);
  for (int round = 1; round < rounds; ++round) invMixColumns(decKeys_ + round * 16);

  rounds_ = rounds;
  return true;
}

void Aes::encryptBlock(const quint8 in[16], quint8 out[16]) const {
  if (rounds_ == 0) return;
  quint8 state[16];
  std::memcpy(state, in, 16);
  addRoundKey(state, encKeys_);
  for (int round = 1; round < rounds_; ++round) {
    subShift(state);
    mixColumns(state);
    addRoundKey(state, encKeys_ + round * 16);
  }
  subShift(state);
  addRoundKey(state, encKeys_ + rounds_ * 16);
  std::memcpy(out, state, 16);
}

void Aes::decryptBlock(const quint8 in[16], quint8 out[16]) const {
  if (rounds_ == 0) return;
  quint8 state[16];
  std::memcpy(state, in, 16);
  addRoundKey(state, decKeys_ + rounds_ * 16);
  for (int round = rounds_ - 1; round >= 1; --round) {
    invSubShift(state);
    invMixColumns(state);
    addRoundKey(state, decKeys_ + round * 16);
  }
  invSubShift(state);
  addRoundKey(state, decKeys_);
  std::memcpy(out, state, 16);
}

QByteArray aesEcbEncrypt(const QByteArray& key, const QByteArray& data) {
  const Aes aes(key);
  if (!aes.isValid() || data.isEmpty() || data.size() % 16 != 0) return {};
  QByteArray out(data.size(), Qt::Uninitialized);
  for (qsizetype at = 0; at < data.size(); at += 16) {
    aes.encryptBlock(reinterpret_cast<const quint8*>(data.constData() + at),
                     reinterpret_cast<quint8*>(out.data() + at));
  }
  return out;
}

QByteArray aesEcbDecrypt(const QByteArray& key, const QByteArray& data) {
  const Aes aes(key);
  if (!aes.isValid() || data.isEmpty() || data.size() % 16 != 0) return {};
  QByteArray out(data.size(), Qt::Uninitialized);
  for (qsizetype at = 0; at < data.size(); at += 16) {
    aes.decryptBlock(reinterpret_cast<const quint8*>(data.constData() + at),
                     reinterpret_cast<quint8*>(out.data() + at));
  }
  return out;
}

CtrStream::CtrStream(const QByteArray& key, const QByteArray& baseCounter) {
  if (baseCounter.size() != 16) return;
  if (!aes_.setKey(key)) return;
  std::memcpy(base_, baseCounter.constData(), 16);
  seek(0);
}

void CtrStream::loadCounter(quint64 block) {
  // base + block, as a 128-bit big-endian integer.
  quint32 carry = 0;
  for (int i = 15; i >= 0; --i) {
    const quint32 addend = static_cast<quint32>(block & 0xFFu) + carry;
    block >>= 8u;
    const quint32 sum = static_cast<quint32>(base_[i]) + addend;
    counter_[i] = static_cast<quint8>(sum & 0xFFu);
    carry = sum >> 8u;
  }
}

void CtrStream::nextBlock() {
  for (int i = 15; i >= 0; --i) {
    if (++counter_[i] != 0u) break;
  }
  aes_.encryptBlock(counter_, keystream_);
  used_ = 0;
}

void CtrStream::seek(quint64 offset) {
  if (!aes_.isValid()) return;
  loadCounter(offset / 16u);
  aes_.encryptBlock(counter_, keystream_);
  used_ = static_cast<int>(offset % 16u);
}

void CtrStream::apply(char* data, qsizetype size) {
  if (!aes_.isValid()) return;
  for (qsizetype i = 0; i < size; ++i) {
    if (used_ == 16) nextBlock();
    data[i] = static_cast<char>(static_cast<quint8>(data[i]) ^ keystream_[used_++]);
  }
}

bool aesXtsDecryptSectors(const QByteArray& key,
                          char* data,
                          qsizetype size,
                          quint64 firstSector,
                          qsizetype sectorSize) {
  if (key.size() != 32 && key.size() != 64) return false;
  if (sectorSize <= 0 || sectorSize % 16 != 0) return false;
  if (size <= 0 || size % sectorSize != 0) return false;

  const qsizetype half = key.size() / 2;
  Aes dataKey, tweakKey;
  if (!dataKey.setKey(key.left(half)) || !tweakKey.setKey(key.mid(half))) return false;

  quint64 sector = firstSector;
  for (qsizetype at = 0; at < size; at += sectorSize, ++sector) {
    quint8 tweak[16];
    // Big-endian sector number: Nintendo's convention, not IEEE 1619's.
    quint64 value = sector;
    for (int i = 15; i >= 0; --i) {
      tweak[i] = static_cast<quint8>(value & 0xFFu);
      value >>= 8u;
    }
    tweakKey.encryptBlock(tweak, tweak);

    for (qsizetype block = 0; block < sectorSize; block += 16) {
      auto* cipher = reinterpret_cast<quint8*>(data + at + block);
      quint8 buffer[16];
      for (int i = 0; i < 16; ++i) buffer[i] = static_cast<quint8>(cipher[i] ^ tweak[i]);
      dataKey.decryptBlock(buffer, buffer);
      for (int i = 0; i < 16; ++i) cipher[i] = static_cast<quint8>(buffer[i] ^ tweak[i]);

      quint8 carry = 0;
      for (int i = 0; i < 16; ++i) {
        const quint8 next = static_cast<quint8>((tweak[i] >> 7u) & 1u);
        tweak[i] = static_cast<quint8>((tweak[i] << 1u) | carry);
        carry = next;
      }
      if (carry) tweak[0] ^= 0x87u;
    }
  }
  return true;
}

}  // namespace psxstudio::crypto
