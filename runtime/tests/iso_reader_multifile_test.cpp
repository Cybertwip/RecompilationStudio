#include "iso_reader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::array<uint8_t, 2352> mode2_sector(uint8_t marker) {
    std::array<uint8_t, 2352> s{};
    static const uint8_t sync[12] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
    std::copy(sync, sync + 12, s.begin());
    s[15] = 2;
    s[16] = 0; s[17] = 0; s[18] = 8; s[19] = 0;
    s[20] = 0; s[21] = 0; s[22] = 8; s[23] = 0;
    std::fill(s.begin() + 24, s.begin() + 24 + 2048, marker);
    return s;
}

static std::array<uint8_t, 2352> audio_sector(uint8_t marker) {
    std::array<uint8_t, 2352> s{};
    std::fill(s.begin(), s.end(), marker);
    return s;
}

static void write_sectors(const fs::path& path,
                          const std::vector<std::array<uint8_t, 2352>>& sectors) {
    std::ofstream out(path, std::ios::binary);
    for (const auto& sector : sectors)
        out.write(reinterpret_cast<const char*>(sector.data()), sector.size());
}

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp = fs::temp_directory_path() /
        ("psx-iso-multifile-" + std::to_string(static_cast<unsigned long long>(nonce)));
    fs::create_directories(tmp);
    const fs::path data = tmp / "track1.bin";
    const fs::path audio = tmp / "track2.bin";
    const fs::path cue = tmp / "disc.cue";

    write_sectors(data, {mode2_sector(0x30), mode2_sector(0x31), mode2_sector(0x32)});
    write_sectors(audio, {audio_sector(0xA0), audio_sector(0xA1), audio_sector(0xA2)});
    {
        std::ofstream out(cue);
        out << "FILE \"track1.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n"
               "FILE \"track2.bin\" BINARY\n"
               "  TRACK 02 AUDIO\n"
               "    INDEX 00 00:00:00\n"
               "    INDEX 01 00:00:01\n";
    }

    PS1::ISOReader reader;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::cerr << "FAIL: " << what << "\n"; failures++; }
    };

    check(reader.Open(cue.string()), "open multi-file cue");
    check(reader.GetSectorCount() == 6, "disc sector count sums both files");
    check(reader.TrackCount() == 2, "track count");
    check(reader.TrackStartLBA(1) == 0, "track 1 start");
    check(reader.TrackStartLBA(2) == 4, "track 2 global INDEX 01 start");
    check(!reader.TrackIsAudio(1), "track 1 is data");
    check(reader.TrackIsAudio(2), "track 2 is audio");
    check(fs::equivalent(reader.GetBinPath(), data), "primary BIN path is the data track");

    std::array<uint8_t, 2048> cooked{};
    check(reader.ReadSector(1, cooked.data()), "read data-track cooked sector");
    check(cooked[0] == 0x31 && cooked[2047] == 0x31,
          "data-track user bytes come from track1.bin");

    std::array<uint8_t, 2352> raw{};
    check(reader.ReadRawSector(3, raw.data()), "read audio-file pregap sector");
    check(raw[0] == 0xA0 && raw[2351] == 0xA0,
          "global LBA 3 maps to track2 file sector 0");
    check(reader.ReadRawSector(4, raw.data()), "read audio track INDEX 01 sector");
    check(raw[0] == 0xA1 && raw[2351] == 0xA1,
          "global LBA 4 maps to track2 file sector 1");
    check(!reader.ReadSector(4, cooked.data()), "audio sectors are not exposed as ISO data");

    reader.Close();
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (failures) return 1;
    std::cout << "PASS: multi-file CUE data, TOC, pregap, and audio mapping are correct.\n";
    return 0;
}
