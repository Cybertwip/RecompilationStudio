#include "iso_reader.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <cstdio>

namespace PS1 {

// PS1 CD-ROM sector size (Mode 2, Form 1 user data)
constexpr size_t SECTOR_SIZE = 2048;

// Full sector size including headers/subchannel (2352 bytes for raw BIN files)
constexpr size_t RAW_SECTOR_SIZE = 2352;

// Offset to user data in raw sector (Mode 2, Form 1)
constexpr size_t RAW_DATA_OFFSET = 24;

// Primary Volume Descriptor location
constexpr uint32_t PVD_SECTOR = 16;

namespace {

struct CueFileSpec {
    std::string path;
    uint32_t sector_size = 0;
};

struct CueTrackSpec {
    int number = 0;
    bool is_audio = false;
    size_t file_index = 0;
    uint32_t index01_lba = 0;
    bool has_index01 = false;
    uint32_t sector_size = RAW_SECTOR_SIZE;
};

static bool ends_with_ci(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) return false;
    const size_t off = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); i++) {
        unsigned char a = static_cast<unsigned char>(value[off + i]);
        unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

static uint32_t track_sector_size(const char* type) {
    if (!type) return RAW_SECTOR_SIZE;
    const std::string t(type);
    if (t.find("/2048") != std::string::npos) return SECTOR_SIZE;
    return RAW_SECTOR_SIZE;  // MODE1/2352, MODE2/2352, AUDIO
}

static uint32_t cue_msf_to_lba(int mm, int ss, int ff) {
    return static_cast<uint32_t>(((mm * 60 + ss) * 75) + ff);
}

} // namespace

ISOReader::ISOReader()
    : is_open_(false) {
    root_dir_.lba = 0;
    root_dir_.size = 0;
}

ISOReader::~ISOReader() {
    Close();
}

bool ISOReader::Open(const std::string& filename) {
    Close();
    if (!std::filesystem::exists(filename)) return false;

    std::vector<CueFileSpec> file_specs;
    std::vector<CueTrackSpec> track_specs;
    const bool is_cue = ends_with_ci(filename, ".cue");

    if (is_cue) {
        std::ifstream cue_file(filename);
        if (!cue_file.is_open()) return false;

        std::string line;
        int current_file = -1;
        int current_track = -1;
        const std::filesystem::path cue_path(filename);
        while (std::getline(cue_file, line)) {
            const size_t file_pos = line.find("FILE");
            const size_t binary_pos = line.find("BINARY");
            if (file_pos != std::string::npos && binary_pos != std::string::npos) {
                const size_t quote1 = line.find('"', file_pos);
                const size_t quote2 = quote1 == std::string::npos
                    ? std::string::npos : line.find('"', quote1 + 1);
                if (quote1 == std::string::npos || quote2 == std::string::npos) return false;
                std::filesystem::path image_path(
                    line.substr(quote1 + 1, quote2 - quote1 - 1));
                if (image_path.is_relative()) image_path = cue_path.parent_path() / image_path;
                file_specs.push_back({image_path.lexically_normal().string(), 0});
                current_file = static_cast<int>(file_specs.size() - 1);
                current_track = -1;
                continue;
            }

            int tn = 0;
            char type[32] = {0};
            if (std::sscanf(line.c_str(), " TRACK %d %31s", &tn, type) == 2) {
                if (current_file < 0) return false;
                CueTrackSpec tr;
                tr.number = tn;
                tr.is_audio = std::strstr(type, "AUDIO") != nullptr;
                tr.file_index = static_cast<size_t>(current_file);
                tr.sector_size = track_sector_size(type);
                track_specs.push_back(tr);
                current_track = static_cast<int>(track_specs.size() - 1);
                uint32_t& file_sector_size = file_specs[(size_t)current_file].sector_size;
                if (file_sector_size == 0 || tr.sector_size > file_sector_size)
                    file_sector_size = tr.sector_size;
                continue;
            }

            int idx = 0, mm = 0, ss = 0, ff = 0;
            if (std::sscanf(line.c_str(), " INDEX %d %d:%d:%d", &idx, &mm, &ss, &ff) == 4 &&
                idx == 1 && current_track >= 0) {
                CueTrackSpec& tr = track_specs[(size_t)current_track];
                tr.index01_lba = cue_msf_to_lba(mm, ss, ff);
                tr.has_index01 = true;
            }
        }
        if (file_specs.empty()) return false;
    } else {
        file_specs.push_back({std::filesystem::path(filename).lexically_normal().string(), 0});
        CueTrackSpec tr;
        tr.number = 1;
        tr.is_audio = false;
        tr.file_index = 0;
        tr.index01_lba = 0;
        tr.has_index01 = true;
        track_specs.push_back(tr);
    }

    uint32_t disc_lba = 0;
    image_files_.reserve(file_specs.size());
    for (size_t i = 0; i < file_specs.size(); i++) {
        const CueFileSpec& spec = file_specs[i];
        if (!std::filesystem::exists(spec.path)) {
            Close();
            return false;
        }
        const uint64_t file_size = std::filesystem::file_size(spec.path);
        uint32_t sector_size = spec.sector_size;
        if (sector_size == 0) {
            sector_size = (file_size % RAW_SECTOR_SIZE == 0)
                ? static_cast<uint32_t>(RAW_SECTOR_SIZE)
                : static_cast<uint32_t>(SECTOR_SIZE);
        }
        if (file_size == 0 || file_size % sector_size != 0) {
            Close();
            return false;
        }

        auto image = std::make_unique<ImageFile>();
        image->path = spec.path;
        image->base_lba = disc_lba;
        image->sector_size = sector_size;
        image->sector_count = static_cast<uint32_t>(file_size / sector_size);
        image->audio_only = true;
        bool saw_track = false;
        for (const auto& tr : track_specs) {
            if (tr.file_index != i) continue;
            saw_track = true;
            if (!tr.is_audio) image->audio_only = false;
        }
        if (!saw_track) image->audio_only = false;
        image->stream.open(spec.path, std::ios::binary);
        if (!image->stream.is_open()) {
            Close();
            return false;
        }
        disc_lba += image->sector_count;
        image_files_.push_back(std::move(image));
    }

    for (const auto& spec : track_specs) {
        if (spec.file_index >= image_files_.size()) {
            Close();
            return false;
        }
        const ImageFile& image = *image_files_[spec.file_index];
        const uint32_t index_lba = spec.has_index01 ? spec.index01_lba : 0;
        if (index_lba > image.sector_count) {
            Close();
            return false;
        }
        CDTrack track;
        track.number = spec.number;
        track.is_audio = spec.is_audio;
        track.start_lba = image.base_lba + index_lba;
        track.file_base_lba = image.base_lba;
        track.file_index_lba = index_lba;
        track.sector_size = image.sector_size;
        track.file_path = image.path;
        tracks_.push_back(std::move(track));
    }

    if (tracks_.empty()) {
        const ImageFile& image = *image_files_.front();
        tracks_.push_back({1, false, image.base_lba, image.base_lba, 0,
                           image.sector_size, image.path});
    }
    std::sort(tracks_.begin(), tracks_.end(),
              [](const CDTrack& a, const CDTrack& b) { return a.number < b.number; });

    bin_path_ = image_files_.front()->path;
    for (const auto& track : tracks_) {
        if (!track.is_audio) {
            bin_path_ = track.file_path;
            break;
        }
    }

    is_open_ = true;
    if (!ParseVolumeDescriptor()) {
        volume_id_.clear();
        root_dir_.lba = 0;
        root_dir_.size = 0;
    }
    return true;
}

void ISOReader::Close() {
    for (auto& image : image_files_) {
        if (image && image->stream.is_open()) image->stream.close();
    }
    image_files_.clear();
    tracks_.clear();
    volume_id_.clear();
    bin_path_.clear();
    root_dir_.lba = 0;
    root_dir_.size = 0;
    is_open_ = false;
}

ISOReader::ImageFile* ISOReader::FindImageFile(uint32_t lba) {
    for (auto& image : image_files_) {
        if (image && lba >= image->base_lba &&
            lba - image->base_lba < image->sector_count)
            return image.get();
    }
    return nullptr;
}

const CDTrack* ISOReader::FindTrack(uint32_t lba) const {
    const CDTrack* found = nullptr;
    for (const auto& track : tracks_) {
        if (track.start_lba <= lba && (!found || track.start_lba >= found->start_lba))
            found = &track;
    }
    return found;
}

bool ISOReader::ReadSector(uint32_t lba, uint8_t* buffer) {
    if (!is_open_ || !buffer) return false;
    ImageFile* image = FindImageFile(lba);
    if (!image || image->audio_only) return false;
    const CDTrack* track = FindTrack(lba);
    if (track && track->is_audio && lba >= track->start_lba) return false;

    image->stream.clear();
    const uint64_t local_lba = static_cast<uint64_t>(lba - image->base_lba);
    if (image->sector_size == RAW_SECTOR_SIZE) {
        uint8_t raw[RAW_SECTOR_SIZE];
        image->stream.seekg(static_cast<std::streamoff>(local_lba * RAW_SECTOR_SIZE),
                            std::ios::beg);
        if (!image->stream.good()) { image->stream.clear(); return false; }
        image->stream.read(reinterpret_cast<char*>(raw), RAW_SECTOR_SIZE);
        if (image->stream.gcount() != static_cast<std::streamsize>(RAW_SECTOR_SIZE)) {
            image->stream.clear();
            return false;
        }
        const size_t data_offset = raw[15] == 1 ? 16u : RAW_DATA_OFFSET;
        if (data_offset + SECTOR_SIZE > RAW_SECTOR_SIZE) return false;
        std::memcpy(buffer, raw + data_offset, SECTOR_SIZE);
        image->stream.clear();
        return true;
    }

    image->stream.seekg(static_cast<std::streamoff>(local_lba * SECTOR_SIZE),
                        std::ios::beg);
    if (!image->stream.good()) { image->stream.clear(); return false; }
    image->stream.read(reinterpret_cast<char*>(buffer), SECTOR_SIZE);
    const bool ok = image->stream.gcount() == static_cast<std::streamsize>(SECTOR_SIZE);
    image->stream.clear();
    return ok;
}

bool ISOReader::ReadRawSector(uint32_t lba, uint8_t* buffer) {
    if (!is_open_ || !buffer) return false;
    ImageFile* image = FindImageFile(lba);
    if (!image || image->sector_size != RAW_SECTOR_SIZE) return false;

    image->stream.clear();
    const uint64_t local_lba = static_cast<uint64_t>(lba - image->base_lba);
    image->stream.seekg(static_cast<std::streamoff>(local_lba * RAW_SECTOR_SIZE),
                        std::ios::beg);
    if (!image->stream.good()) { image->stream.clear(); return false; }
    image->stream.read(reinterpret_cast<char*>(buffer), RAW_SECTOR_SIZE);
    const bool ok = image->stream.gcount() == static_cast<std::streamsize>(RAW_SECTOR_SIZE);
    image->stream.clear();
    return ok;
}

bool ISOReader::IsOpen() const {
    return is_open_;
}

std::string ISOReader::GetVolumeID() const {
    return volume_id_;
}

std::string ISOReader::GetBinPath() const {
    return bin_path_;
}

uint32_t ISOReader::GetSectorCount() {
    uint64_t total = 0;
    for (const auto& image : image_files_) {
        if (image) total += image->sector_count;
    }
    return total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
}

int ISOReader::TrackCount() const {
    return static_cast<int>(tracks_.size());
}

uint32_t ISOReader::TrackStartLBA(int track) const {
    for (const auto& t : tracks_) {
        if (t.number == track) return t.start_lba;
    }
    return 0;
}

bool ISOReader::TrackIsAudio(int track) const {
    for (const auto& t : tracks_) {
        if (t.number == track) return t.is_audio;
    }
    return false;
}

RootDirectoryInfo ISOReader::GetRootDirectory() const {
    return root_dir_;
}

uint32_t ISOReader::Read733(const uint8_t* data) const {
    // Read little-endian half of both-endian 32-bit value
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

bool ISOReader::ParseVolumeDescriptor() {
    // Read Primary Volume Descriptor from sector 16
    uint8_t pvd[SECTOR_SIZE];
    if (!ReadSector(PVD_SECTOR, pvd)) {
        return false;
    }

    // Verify ISO9660 signature: offset 1 should contain "CD001"
    if (pvd[0] != 0x01 || std::memcmp(&pvd[1], "CD001", 5) != 0) {
        return false;  // Not a valid ISO9660 disc
    }

    // Extract volume ID (offset 40, 32 bytes, space-padded ASCII)
    volume_id_.clear();
    for (int i = 0; i < 32; i++) {
        char c = pvd[40 + i];
        if (c != ' ' && c != '\0') {
            volume_id_ += c;
        }
    }

    // Extract root directory record (offset 156, 34 bytes)
    const uint8_t* root_record = &pvd[156];

    // Root directory LBA is at offset 2 within the directory record (both-endian 32-bit)
    root_dir_.lba = Read733(&root_record[2]);

    // Root directory size is at offset 10 within the directory record (both-endian 32-bit)
    root_dir_.size = Read733(&root_record[10]);

    return true;
}

bool ISOReader::ParseDirectoryRecord(const uint8_t* data, ISOFileEntry& entry) const {
    // Check record length (offset 0)
    uint8_t record_len = data[0];
    if (record_len == 0 || record_len < 33) {
        return false;  // Invalid or padding record
    }

    // Extract LBA (offset 2, both-endian 32-bit)
    entry.lba = Read733(&data[2]);

    // Extract file size (offset 10, both-endian 32-bit)
    entry.size = Read733(&data[10]);

    // Extract file flags (offset 25)
    uint8_t flags = data[25];
    entry.is_directory = (flags & 0x02) != 0;

    // Extract filename length (offset 32)
    uint8_t name_len = data[32];
    if (name_len == 0) {
        return false;  // Invalid record
    }

    // Extract filename (offset 33)
    const char* name_ptr = reinterpret_cast<const char*>(&data[33]);

    // Handle special directory entries
    if (name_len == 1 && name_ptr[0] == '\x00') {
        entry.name = ".";  // Current directory
        return true;
    }
    if (name_len == 1 && name_ptr[0] == '\x01') {
        entry.name = "..";  // Parent directory
        return true;
    }

    // Parse regular filename, strip version suffix (";1")
    entry.name.clear();
    for (uint8_t i = 0; i < name_len; i++) {
        char c = name_ptr[i];
        if (c == ';') {
            break;  // Stop at version separator
        }
        entry.name += c;
    }

    return true;
}

std::vector<ISOFileEntry> ISOReader::ListFilesByLBA(uint32_t lba, uint32_t dir_size) {
    std::vector<ISOFileEntry> results;

    if (!is_open_ || lba == 0 || dir_size == 0) {
        return results;
    }

    // Calculate number of sectors needed for directory data
    uint32_t num_sectors = (dir_size + 2047) / 2048;

    // Allocate buffer for directory data
    std::vector<uint8_t> dir_data(num_sectors * 2048, 0);

    // Read all directory sectors
    for (uint32_t i = 0; i < num_sectors; i++) {
        if (!ReadSector(lba + i, &dir_data[i * 2048])) {
            return results;  // Error reading sector
        }
    }

    // Parse directory records
    uint32_t offset = 0;
    while (offset < dir_size) {
        uint8_t record_len = dir_data[offset];

        if (record_len == 0) {
            // Skip to next sector boundary
            uint32_t sector_offset = offset % 2048;
            if (sector_offset != 0) {
                offset += (2048 - sector_offset);
                continue;
            }
            break;
        }

        if (offset + record_len > dir_data.size()) break;

        ISOFileEntry entry;
        if (ParseDirectoryRecord(&dir_data[offset], entry)) {
            if (entry.name != "." && entry.name != "..") {
                results.push_back(entry);
            }
        }

        offset += record_len;
    }

    return results;
}

std::vector<ISOFileEntry> ISOReader::ListFiles(const std::string& path) {
    std::vector<ISOFileEntry> results;

    // Check if file is open
    if (!is_open_) {
        return results;
    }

    if (path.empty()) {
        // List root directory
        RootDirectoryInfo root = GetRootDirectory();
        return ListFilesByLBA(root.lba, root.size);
    }

    // Non-empty path: navigate to that subdirectory within the root
    // Find the matching directory entry in root
    RootDirectoryInfo root = GetRootDirectory();
    std::vector<ISOFileEntry> root_entries = ListFilesByLBA(root.lba, root.size);

    std::string path_upper = path;
    std::transform(path_upper.begin(), path_upper.end(), path_upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    for (const auto& e : root_entries) {
        if (!e.is_directory) continue;
        std::string name_upper = e.name;
        std::transform(name_upper.begin(), name_upper.end(), name_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (name_upper == path_upper) {
            // Found the subdirectory — list its contents
            return ListFilesByLBA(e.lba, e.size > 0 ? e.size : 2048);
        }
    }

    return results;  // Directory not found
}

bool ISOReader::FindFile(const std::string& path, ISOFileEntry& entry) {
    // Check if file is open
    if (!is_open_) {
        return false;
    }

    // Check if path contains a directory separator
    size_t sep = path.find('/');
    if (sep == std::string::npos) {
        sep = path.find('\\');
    }

    if (sep != std::string::npos) {
        // Subdirectory path: "DIR/FILE" or "DIR\FILE"
        std::string dir_name  = path.substr(0, sep);
        std::string file_name = path.substr(sep + 1);

        // List the subdirectory
        std::vector<ISOFileEntry> sub_files = ListFiles(dir_name);

        std::string file_upper = file_name;
        std::transform(file_upper.begin(), file_upper.end(), file_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        for (const auto& f : sub_files) {
            std::string name_upper = f.name;
            std::transform(name_upper.begin(), name_upper.end(), name_upper.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (name_upper == file_upper) {
                entry = f;
                return true;
            }
        }
        return false;
    }

    // Root-level file: search root directory
    std::vector<ISOFileEntry> files = ListFiles("");

    // Search for matching filename (case-insensitive comparison)
    for (const auto& file : files) {
        std::string file_upper = file.name;
        std::string path_upper = path;

        std::transform(file_upper.begin(), file_upper.end(), file_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        std::transform(path_upper.begin(), path_upper.end(), path_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        if (file_upper == path_upper) {
            entry = file;
            return true;
        }
    }

    // File not found
    return false;
}

size_t ISOReader::ReadFile(const std::string& path, uint8_t* buffer, size_t max_size) {
    // Validate buffer pointer
    if (!buffer) {
        return 0;
    }

    // Find the file
    ISOFileEntry entry;
    if (!FindFile(path, entry)) {
        return 0;  // File not found
    }

    // Calculate how many bytes to read (min of file size and max_size)
    size_t bytes_to_read = std::min(static_cast<size_t>(entry.size), max_size);

    // Calculate number of sectors to read
    uint32_t sectors_to_read = (bytes_to_read + SECTOR_SIZE - 1) / SECTOR_SIZE;

    // Read sectors sequentially
    size_t bytes_read = 0;
    for (uint32_t i = 0; i < sectors_to_read; i++) {
        // Calculate how many bytes to read from this sector
        size_t bytes_remaining = bytes_to_read - bytes_read;
        size_t sector_bytes = std::min(bytes_remaining, SECTOR_SIZE);

        // Read sector into temporary buffer
        uint8_t sector_buffer[SECTOR_SIZE];
        if (!ReadSector(entry.lba + i, sector_buffer)) {
            return bytes_read;  // Error - return what we've read so far
        }

        // Copy data to output buffer
        std::memcpy(buffer + bytes_read, sector_buffer, sector_bytes);
        bytes_read += sector_bytes;
    }

    return bytes_read;
}

size_t ISOReader::GetFileSize(const std::string& path) {
    // Find the file
    ISOFileEntry entry;
    if (!FindFile(path, entry)) {
        return 0;  // File not found
    }

    // Return file size
    return entry.size;
}

} // namespace PS1
