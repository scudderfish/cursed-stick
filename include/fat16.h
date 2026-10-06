// fat16.h — deterministic FAT16 volume for the Gotek WiFi stick.
//
// The volume is *computed* a byte range at a time (see readBytes()) instead of
// being materialised in RAM, so no file-sized buffer exists anywhere: the
// firmware needs no PSRAM and the volume can span the whole filesystem.
//
// Geometry is fixed: 512-byte sectors, 1 KiB clusters, 2 FATs, a 512-entry root
// directory and 40 FAT sectors, giving 10119 data clusters — a valid FAT16
// (>= 4085 clusters; FatFs picks FAT12 below that and rejects FAT32 above it).
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
constexpr uint32_t ROOT_DIR_BYTES      = ROOT_ENTRIES * 32;

// Volume size: the whole `spiffs` (LittleFS) partition from partitions.csv, so
// the Gotek sees everything the stick can hold (about eleven 880 KiB ADFs).
constexpr uint32_t TOTAL_SECTORS = 20352;                  // 9.9375 MiB
constexpr uint32_t TOTAL_BYTES   = TOTAL_SECTORS * BYTES_PER_SECTOR;

// One FAT sector holds 256 entries, so 40 of them cover 10240 clusters — more
// than the 10119 data clusters below, which is what keeps this volume FAT16.
constexpr uint32_t FAT_SECTORS = 40;

constexpr uint32_t FAT1_SECTOR = RESERVED_SECTORS;                   // 2
constexpr uint32_t FAT2_SECTOR = FAT1_SECTOR + FAT_SECTORS;          // 42
constexpr uint32_t ROOT_SECTOR = FAT2_SECTOR + FAT_SECTORS;          // 82
constexpr uint32_t DATA_SECTOR = ROOT_SECTOR + ROOT_DIR_SECTORS;     // 114

constexpr uint32_t DATA_SECTORS = TOTAL_SECTORS - DATA_SECTOR;       // 20238
constexpr uint32_t MAX_CLUSTERS = DATA_SECTORS / SECTORS_PER_CLUSTER; // 10119
constexpr uint32_t LAST_CLUSTER = MAX_CLUSTERS + 1;                   // 10120

constexpr uint32_t MAX_FILES = 64;

static_assert(MAX_CLUSTERS >= 4085 && MAX_CLUSTERS <= 0xFFF5,
              "cluster count must stay in FatFs's FAT16 range");
static_assert((LAST_CLUSTER + 1) * 2 <= FAT_SECTORS * BYTES_PER_SECTOR,
              "FAT_SECTORS cannot hold an entry for LAST_CLUSTER");
static_assert(DATA_SECTOR + DATA_SECTORS == TOTAL_SECTORS, "geometry mismatch");

struct FileEntry {
  const char* name;    // basename, ASCII, null-terminated (no path, no directory)
  uint32_t    size;    // file size in bytes
};

// Reads `n` bytes at `offset` of file `fileIndex` into `dst`.
// Returns true on success. The volume only ever reads within [0, files[i].size).
using FileReader = bool (*)(void* ctx, uint32_t fileIndex, uint32_t offset,
                            uint8_t* dst, uint32_t n);

// Where one file lives: its 8.3 name and its cluster run. Clusters are handed
// out contiguously and never shared, which is what lets the FAT be computed
// (a staircase) instead of stored.
struct Placement {
  uint8_t  shortName[11];
  uint32_t startCluster;   // 0 when the file is empty
  uint32_t clusters;
};

struct Volume {
  uint32_t  fileCount;
  uint32_t  rootEntries;   // root slots used: volume label + LFN chains + shorts
  Placement files[MAX_FILES];
};

// Works out where each file lands and how many root slots its name needs. Reads
// no file data and allocates nothing. Returns false when the files do not fit:
// the root directory is full, the cluster space is exhausted, a name exceeds the
// VFAT limit (255 bytes), or two names cannot be told apart in 8.3.
bool layout(const FileEntry* files, uint32_t count, Volume* out);

// Copies `len` bytes of the volume starting at byte offset `off` into `dst`:
// the boot sector, either FAT, the root directory, or file data read through
// `reader`. `off + len` must be within TOTAL_BYTES, and any offset/length is
// allowed — the caller is a USB MSC callback, which splits transfers wherever
// the host asks. Returns false when `reader` fails.
bool readBytes(const Volume& vol, const FileEntry* files, FileReader reader,
               void* ctx, uint32_t off, uint8_t* dst, uint32_t len);

// Storage-safe name for `name`, for when the filesystem refuses the name the
// client sent (a LittleFS seeded with `pio run -t uploadfs` is capped at 32
// bytes for good — see AGENTS.md). `name` that already fits `maxLen` is
// returned unchanged. Otherwise the stem is truncated so that
// "STEM~DDDDDD.EXT" fits: the whole extension is kept (FlashFloppy picks the
// image handler from it) along with a 6-hex-digit digest of the full name, so
// the same client name always maps to the same stored name (a re-upload
// replaces its file) while look-alike names stay apart. The name presented over
// USB is generated from whatever ends up stored, so it does not have to be the
// name we store. Returns false when even the digest and extension do not fit.
// `out` must have room for maxLen + 1 bytes.
bool storableName(const char* name, uint32_t maxLen, char* out);

} // namespace fat16
