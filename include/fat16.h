// fat16.h — deterministic FAT16 image builder for the Gotek WiFi stick.
//
// Geometry is fixed and matches `mkfs.fat -F 16 -s 2` for a 6 MiB image:
//   512-byte sectors, 1 KiB clusters, 2 FATs, 512-entry root dir, 24 FAT sectors.
// This yields 6103 data clusters — a valid FAT16 (>= 4085 clusters).
#pragma once

#include <stdint.h>

namespace fat16 {

constexpr uint32_t BYTES_PER_SECTOR    = 512;
constexpr uint32_t SECTORS_PER_CLUSTER = 2;
constexpr uint32_t CLUSTER_SIZE        = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER; // 1024
constexpr uint32_t RESERVED_SECTORS    = 2;
constexpr uint32_t NUM_FATS            = 2;
constexpr uint32_t ROOT_ENTRIES        = 512;
constexpr uint32_t ROOT_DIR_SECTORS    = (ROOT_ENTRIES * 32) / BYTES_PER_SECTOR; // 32
constexpr uint32_t FAT_SECTORS         = 24;
constexpr uint32_t TOTAL_SECTORS       = 12288;   // 6 MiB
constexpr uint32_t TOTAL_BYTES         = TOTAL_SECTORS * BYTES_PER_SECTOR;

constexpr uint32_t FAT1_SECTOR   = RESERVED_SECTORS;                       // 2
constexpr uint32_t FAT2_SECTOR   = FAT1_SECTOR + FAT_SECTORS;              // 26
constexpr uint32_t ROOT_SECTOR   = FAT2_SECTOR + FAT_SECTORS;              // 50
constexpr uint32_t DATA_SECTOR   = ROOT_SECTOR + ROOT_DIR_SECTORS;         // 82

constexpr uint32_t DATA_SECTORS  = TOTAL_SECTORS - DATA_SECTOR;            // 12206
constexpr uint32_t MAX_CLUSTERS  = DATA_SECTORS / SECTORS_PER_CLUSTER;     // 6103
constexpr uint32_t LAST_CLUSTER  = MAX_CLUSTERS + 1;                       // 6104 (max cluster index)

struct FileEntry {
  const char*    name;    // basename, ASCII, null-terminated (no path, no directory)
  uint32_t       size;    // file size in bytes
};

// Reads `n` bytes at `offset` of file `fileIndex` into `dst`.
// Returns true on success. The builder only reads within [0, files[i].size).
using FileReader = bool (*)(void* ctx, uint32_t fileIndex, uint32_t offset,
                            uint8_t* dst, uint32_t n);

// Builds a complete FAT16 image into `buf` (must be >= TOTAL_BYTES).
// Returns TOTAL_BYTES on success, or 0 if the files do not fit / an error occurs.
uint32_t build(uint8_t* buf, const FileEntry* files, uint32_t fileCount,
               FileReader reader, void* ctx);

// LFN checksum over an 11-byte 8.3 short name (exposed for the host test harness).
uint8_t lfnChecksum(const uint8_t shortName[11]);

} // namespace fat16
