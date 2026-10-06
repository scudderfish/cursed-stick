// fat16.cpp — deterministic FAT16 volume, generated on demand.
//
// layout() decides where everything goes (a few hundred bytes of state in
// Volume); readBytes() then answers any byte range of the volume from that
// state, pulling file data through the caller's FileReader. Nothing here
// allocates a file-sized buffer, so the firmware holds no copy of the ADFs and
// needs no PSRAM.
#include "fat16.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

namespace fat16 {

// ---------------------------------------------------------------------------
// little-endian helpers
// ---------------------------------------------------------------------------
static inline void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static const char VOLUME_LABEL[11] = { 'C','U','R','S','E','D','S','T','I','C','K' };

// ---------------------------------------------------------------------------
// boot sector
// ---------------------------------------------------------------------------
static void writeBootSector(uint8_t* buf) {
  memset(buf, 0, BYTES_PER_SECTOR);
  buf[0] = 0xEB; buf[1] = 0x3C; buf[2] = 0x90;
  memcpy(buf + 3, "MSDOS5.0", 8);
  put16(buf + 0x0B, BYTES_PER_SECTOR);                 // bytes per sector
  buf[0x0D] = (uint8_t)SECTORS_PER_CLUSTER;            // sectors per cluster
  put16(buf + 0x0E, RESERVED_SECTORS);                 // reserved sectors
  buf[0x10] = (uint8_t)NUM_FATS;                       // number of FATs
  put16(buf + 0x11, ROOT_ENTRIES);                     // root directory entries
  put16(buf + 0x13, TOTAL_SECTORS);                    // total sectors (16-bit)
  buf[0x15] = 0xF8;                                    // media descriptor (fixed disk)
  put16(buf + 0x16, FAT_SECTORS);                      // sectors per FAT
  put16(buf + 0x18, 63);                               // sectors per track (cosmetic)
  put16(buf + 0x1A, 255);                              // heads (cosmetic)
  put32(buf + 0x1C, 0);                                // hidden sectors
  put32(buf + 0x20, 0);                                // total sectors (32-bit, unused FAT16)
  buf[0x24] = 0x80;                                    // drive number
  buf[0x25] = 0;
  buf[0x26] = 0x29;                                    // extended boot signature
  put32(buf + 0x27, 0x41444631u);                      // volume serial "ADF1"
  memcpy(buf + 0x2B, VOLUME_LABEL, 11);                // volume label (must equal rootDir's)
  memcpy(buf + 0x36, "FAT16   ", 8);                   // filesystem type
  buf[510] = 0x55; buf[511] = 0xAA;                    // boot signature
}

// ---------------------------------------------------------------------------
// short names (8.3)
// ---------------------------------------------------------------------------
static uint8_t lfnChecksum(const uint8_t shortName[11]) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + shortName[i]);
  return sum;
}

static char sanitize(char c) {
  c = (char)toupper((unsigned char)c);
  if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
  switch (c) {
    case ' ' : case '$' : case '%' : case '\'' : case '-' : case '_' :
    case '!' : case '(' : case ')' : case '~' : case '@' : case '^' :
    case '&' : case '#' : case '{' : case '}' : case '`' :
      return c;
    default:
      return '_';
  }
}

// Split into sanitized stem/ext. Stem is upper-cased, illegal chars -> '_',
// spaces dropped (LFN preserves the real name; the 8.3 name just needs to be
// unique and valid).
static void splitName(const char* name, char* stem, int* sl, char* ext, int* el) {
  const char* dot = strrchr(name, '.');
  const char* stemEnd;
  const char* extStart;
  if (dot && dot != name) { stemEnd = dot; extStart = dot + 1; }
  else { stemEnd = name + strlen(name); extStart = stemEnd; }

  *sl = 0;
  for (const char* p = name; p < stemEnd && *sl < 63; p++) {
    char c = sanitize(*p);
    if (c == ' ') continue;
    stem[(*sl)++] = c;
  }
  if (*sl == 0) { memcpy(stem, "FILE", 4); *sl = 4; }
  stem[*sl] = 0;

  *el = 0;
  for (const char* p = extStart; *p && *el < 3; p++) {
    char c = sanitize(*p);
    if (c == ' ') continue;
    ext[(*el)++] = c;
  }
  ext[*el] = 0;
}

static void makeShort(char out[11], const char* stem, int sl, const char* ext, int el) {
  memset(out, ' ', 11);
  int n = sl < 8 ? sl : 8;
  memcpy(out, stem, n);
  int m = el < 3 ? el : 3;
  memcpy(out + 8, ext, m);
}

// DOS-style "STEM~N" tilde name (8 chars, space padded).
static void makeTilde(char out8[8], const char* stem, int sl, int n) {
  memset(out8, ' ', 8);
  char num[8];
  int nd = snprintf(num, sizeof num, "%d", n);
  int baseLen = 7 - nd;                  // base + '~' + digits = 8
  if (baseLen > 6) baseLen = 6;
  if (baseLen < 1) baseLen = 1;
  if (baseLen > sl) baseLen = sl;
  memcpy(out8, stem, baseLen);
  out8[baseLen] = '~';
  memcpy(out8 + baseLen + 1, num, nd);
}

// "STEM~N" plus extension, packed into the 11-byte short-name form.
static void makeTildeShort(char cand[11], const char* stem, int sl,
                           const char* ext, int el, int n) {
  char tilde[8];
  makeTilde(tilde, stem, sl, n);
  memset(cand, ' ', 11);
  memcpy(cand, tilde, 8);
  int m = el < 3 ? el : 3;
  memcpy(cand + 8, ext, m);
}

// Candidate 8.3 name for one file. n == 0 selects the plain truncated form,
// which only exists when the stem and extension fit; n > 0 the DOS-style
// "STEM~N" form used to keep distinct long names apart. Returns false when the
// candidate does not fit in 8.3 at all.
static bool makeCandidate(char cand[11], const char* stem, int sl,
                          const char* ext, int el, int n) {
  if (n == 0) {
    if (sl > 8 || el > 3) return false;
    makeShort(cand, stem, sl, ext, el);
    return true;
  }
  makeTildeShort(cand, stem, sl, ext, el, n);
  return true;
}

struct ShortName { char bytes[11]; };

static bool isUsed(const ShortName* out, uint32_t count, const char* cand) {
  for (uint32_t j = 0; j < count; j++)
    if (memcmp(out[j].bytes, cand, 11) == 0) return true;
  return false;
}

static bool resolveShortNames(const FileEntry* files, uint32_t count, ShortName* out) {
  for (uint32_t i = 0; i < count; i++) {
    char stem[64], ext[4];
    int sl, el;
    splitName(files[i].name, stem, &sl, ext, &el);

    bool assigned = false;
    for (int n = 0; n <= 9999 && !assigned; n++) {
      char cand[11];
      if (makeCandidate(cand, stem, sl, ext, el, n) && !isUsed(out, i, cand)) {
        memcpy(out[i].bytes, cand, 11);
        assigned = true;
      }
    }
    if (!assigned) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// storage-safe names (LittleFS)
// ---------------------------------------------------------------------------
// The FAT-facing name is generated by layout(); this maps a name the
// *filesystem* will accept. It lives here because fat16.cpp is the only unit
// the host check compiles (see tools/host_check.sh).

// FNV-1a — small, allocation-free and stable across builds, unlike std::hash.
static uint32_t digestOf(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) {
    h ^= (uint8_t)*s;
    h *= 16777619u;
  }
  return h;
}

bool storableName(const char* name, uint32_t maxLen, char* out) {
  uint32_t len = (uint32_t)strlen(name);
  if (len <= maxLen) {
    memcpy(out, name, len + 1);
    return true;
  }

  // Keep the whole extension — FlashFloppy takes the image handler from it — so
  // only the stem gives way. `dot != name` keeps a leading dot with the stem.
  const char* dot = strrchr(name, '.');
  uint32_t stemLen = len, extLen = 0;
  const char* ext = "";
  if (dot && dot != name) {
    stemLen = (uint32_t)(dot - name);
    ext = dot;
    extLen = len - stemLen;
  }

  const uint32_t suffix = 1 /* '~' */ + 6 /* digest digits */;
  if (extLen + suffix > maxLen) return false;

  uint32_t keep = maxLen - extLen - suffix;
  if (keep > stemLen) keep = stemLen;

  char digest[8];
  snprintf(digest, sizeof digest, "%06x", digestOf(name) & 0xFFFFFFu);

  uint32_t k = 0;
  memcpy(out + k, name, keep); k += keep;
  out[k++] = '~';
  memcpy(out + k, digest, 6); k += 6;
  if (extLen) { memcpy(out + k, ext, extLen); k += extLen; }
  out[k] = 0;
  return true;
}

// ---------------------------------------------------------------------------
// directory entries
// ---------------------------------------------------------------------------
static void writeLfnEntry(uint8_t* entry, uint8_t seq, const uint16_t chars[13], uint8_t checksum) {
  memset(entry, 0, 32);
  entry[0]  = seq;
  entry[11] = 0x0F;                 // LFN attribute
  entry[12] = 0x00;                 // type
  entry[13] = checksum;
  entry[26] = 0x00; entry[27] = 0x00; // first cluster = 0
  put16(entry + 1,  chars[0]);
  put16(entry + 3,  chars[1]);
  put16(entry + 5,  chars[2]);
  put16(entry + 7,  chars[3]);
  put16(entry + 9,  chars[4]);
  put16(entry + 14, chars[5]);
  put16(entry + 16, chars[6]);
  put16(entry + 18, chars[7]);
  put16(entry + 20, chars[8]);
  put16(entry + 22, chars[9]);
  put16(entry + 24, chars[10]);
  put16(entry + 28, chars[11]);
  put16(entry + 30, chars[12]);
}

static void writeShortEntry(uint8_t* entry, const uint8_t shortName[11],
                            uint16_t firstCluster, uint32_t size) {
  memset(entry, 0, 32);
  memcpy(entry, shortName, 11);
  entry[11] = 0x20;                 // archive attribute
  put16(entry + 14, 0x0000);        // creation time
  put16(entry + 16, 0x5221);        // creation date (2021-01-01)
  put16(entry + 18, 0x5221);        // last access date
  entry[20] = 0; entry[21] = 0;     // high cluster (FAT16: 0)
  put16(entry + 22, 0x0000);        // last write time
  put16(entry + 24, 0x5221);        // last write date
  put16(entry + 26, firstCluster);
  put32(entry + 28, size);
}

// LFN entries for one file, written the way the volume stores them: highest
// chunk first (seq 0x40|N), lowest last (seq 1).
static uint32_t lfnEntryCount(const char* name, uint32_t nameLen) {
  (void)name;
  return (nameLen + 1 + 12) / 13;   // + null terminator, 13 chars per entry
}

static void writeOneLfnChunk(const char* name, uint32_t nameLen, uint32_t chunkIndex,
                             uint32_t lfnCount, uint8_t checksum, uint8_t* entry) {
  uint16_t chunk[13];
  for (int k = 0; k < 13; k++) {
    uint32_t idx = chunkIndex * 13 + (uint32_t)k;
    if (idx < nameLen)       chunk[k] = (uint16_t)(uint8_t)name[idx];  // ASCII
    else if (idx == nameLen) chunk[k] = 0x0000;                        // terminator
    else                     chunk[k] = 0xFFFF;                        // padding
  }
  uint8_t seq = (uint8_t)(chunkIndex + 1);
  if (chunkIndex == lfnCount - 1) seq |= 0x40;
  writeLfnEntry(entry, seq, chunk, checksum);
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
bool layout(const FileEntry* files, uint32_t count, Volume* out) {
  if (!out) return false;
  if (count > MAX_FILES) return false;
  if (count && !files) return false;

  out->fileCount = count;
  out->rootEntries = 1;                     // slot 0 = volume label
  uint32_t nextCluster = 2;                 // clusters 0 and 1 are reserved

  for (uint32_t i = 0; i < count; i++) {
    if (!files[i].name) return false;
    uint32_t nameLen = (uint32_t)strlen(files[i].name);
    if (nameLen == 0 || nameLen > 255) return false;   // VFAT limit

    uint32_t lfnCount = lfnEntryCount(files[i].name, nameLen);
    if (out->rootEntries + lfnCount + 1 > ROOT_ENTRIES) return false;

    uint32_t clusters = files[i].size
        ? (files[i].size + CLUSTER_SIZE - 1) / CLUSTER_SIZE : 0;
    if (clusters && nextCluster + clusters - 1 > LAST_CLUSTER) return false;

    out->files[i].startCluster = clusters ? nextCluster : 0;
    out->files[i].clusters = clusters;
    out->rootEntries += lfnCount + 1;
    nextCluster += clusters;
  }

  ShortName names[MAX_FILES];
  if (!resolveShortNames(files, count, names)) return false;
  for (uint32_t i = 0; i < count; i++) memcpy(out->files[i].shortName, names[i].bytes, 11);
  return true;
}

// ---------------------------------------------------------------------------
// volume contents
// ---------------------------------------------------------------------------
// The FAT of a volume whose files occupy contiguous runs: every cluster points
// at the next one, and the last of a run is the end-of-chain marker. Clusters
// that belong to no file are free (0).
static void writeFatSector(const Volume& vol, uint32_t sectorInFat, uint8_t* dst) {
  const uint32_t entriesPerSector = BYTES_PER_SECTOR / 2;
  uint32_t first = sectorInFat * entriesPerSector;
  for (uint32_t i = 0; i < entriesPerSector; i++) {
    uint32_t cluster = first + i;
    uint16_t value = 0;
    if (cluster == 0) {
      value = 0xFFF8;                                  // media descriptor
    } else if (cluster == 1) {
      value = 0xFFFF;
    } else if (cluster <= LAST_CLUSTER) {
      for (uint32_t f = 0; f < vol.fileCount; f++) {
        const Placement& p = vol.files[f];
        if (!p.clusters) continue;
        if (cluster < p.startCluster || cluster >= p.startCluster + p.clusters) continue;
        value = (cluster == p.startCluster + p.clusters - 1)
              ? 0xFFFF : (uint16_t)(cluster + 1);
        break;
      }
    }
    put16(dst + i * 2, value);
  }
}

// One 512-byte root-directory sector (16 entries), starting at `firstEntry`.
// Entries past the last file are zero, which is the directory's end marker.
static bool writeRootSector(const Volume& vol, const FileEntry* files,
                            uint32_t sectorInRoot, uint8_t* dst) {
  const uint32_t firstEntry = sectorInRoot * (BYTES_PER_SECTOR / 32);
  const uint32_t lastEntry  = firstEntry + BYTES_PER_SECTOR / 32;
  memset(dst, 0, BYTES_PER_SECTOR);

  uint32_t slot = 0;
  struct Emitter {
    uint8_t* dst; uint32_t first, last, slot;
    void emit(const uint8_t* entry) {
      if (slot >= first && slot < last) memcpy(dst + (slot - first) * 32, entry, 32);
      slot++;
    }
  } out = { dst, firstEntry, lastEntry, 0 };

  {                                        // volume label, matching the boot sector
    uint8_t e[32];
    memset(e, 0, 32);
    memcpy(e, VOLUME_LABEL, 11);
    e[11] = 0x08;                           // volume label attribute
    out.emit(e);
  }

  for (uint32_t i = 0; i < vol.fileCount; i++) {
    const char* name = files[i].name;
    uint32_t nameLen = (uint32_t)strlen(name);
    uint32_t lfnCount = lfnEntryCount(name, nameLen);
    uint8_t checksum = lfnChecksum(vol.files[i].shortName);

    if (out.slot < lastEntry) {              // skip whole files we are past
      for (uint32_t e = 0; e < lfnCount; e++) {
        uint32_t chunkIndex = lfnCount - 1 - e;
        uint8_t entry[32];
        writeOneLfnChunk(name, nameLen, chunkIndex, lfnCount, checksum, entry);
        out.emit(entry);
      }
      uint8_t entry[32];
      writeShortEntry(entry, vol.files[i].shortName,
                      (uint16_t)vol.files[i].startCluster, files[i].size);
      out.emit(entry);
    } else {
      out.slot += lfnCount + 1;
    }
  }
  return true;
}

// File data: clusters map straight onto offsets inside a file. Anything past the
// end of the last cluster's worth of data reads as zero, so a partial final
// cluster looks the same as it does on a real disk.
static bool readDataRange(const Volume& vol, const FileEntry* files, FileReader reader,
                          void* ctx, uint32_t dataOffset, uint8_t* dst, uint32_t len) {
  while (len) {
    uint32_t cluster   = 2 + dataOffset / CLUSTER_SIZE;
    uint32_t inCluster = dataOffset % CLUSTER_SIZE;
    uint32_t chunk = CLUSTER_SIZE - inCluster;
    if (chunk > len) chunk = len;

    const Placement* owner = nullptr;
    for (uint32_t i = 0; i < vol.fileCount; i++) {
      const Placement& p = vol.files[i];
      if (!p.clusters) continue;
      if (cluster >= p.startCluster && cluster < p.startCluster + p.clusters) {
        owner = &p;
        break;
      }
    }

    uint32_t copied = 0;
    if (owner) {
      uint32_t fileIndex = (uint32_t)(owner - vol.files);
      uint32_t fileOffset = (cluster - owner->startCluster) * CLUSTER_SIZE + inCluster;
      uint32_t size = files[fileIndex].size;
      if (fileOffset < size) {
        uint32_t n = size - fileOffset;
        if (n > chunk) n = chunk;
        if (n && !reader(ctx, fileIndex, fileOffset, dst, n)) return false;
        copied = n;
      }
    }
    if (copied < chunk) memset(dst + copied, 0, chunk - copied);

    dst += chunk;
    dataOffset += chunk;
    len -= chunk;
  }
  return true;
}

static bool emitSector(const Volume& vol, const FileEntry* files, FileReader reader,
                       void* ctx, uint32_t sector, uint8_t* dst) {
  if (sector == 0) { writeBootSector(dst); return true; }
  if (sector < FAT1_SECTOR) { memset(dst, 0, BYTES_PER_SECTOR); return true; }   // reserved
  if (sector < FAT2_SECTOR) { writeFatSector(vol, sector - FAT1_SECTOR, dst); return true; }
  if (sector < ROOT_SECTOR) { writeFatSector(vol, sector - FAT2_SECTOR, dst); return true; }
  if (sector < DATA_SECTOR) { return writeRootSector(vol, files, sector - ROOT_SECTOR, dst); }
  return readDataRange(vol, files, reader, ctx,
                       (sector - DATA_SECTOR) * BYTES_PER_SECTOR, dst, BYTES_PER_SECTOR);
}

bool readBytes(const Volume& vol, const FileEntry* files, FileReader reader,
               void* ctx, uint32_t off, uint8_t* dst, uint32_t len) {
  if (off > TOTAL_BYTES || len > TOTAL_BYTES - off) return false;
  if (!len) return true;
  if (!dst) return false;
  if (vol.fileCount && !files) return false;

  while (len) {
    uint32_t sector   = off / BYTES_PER_SECTOR;
    uint32_t inSector = off % BYTES_PER_SECTOR;
    uint32_t chunk = BYTES_PER_SECTOR - inSector;
    if (chunk > len) chunk = len;

    if (inSector == 0 && chunk == BYTES_PER_SECTOR) {
      if (!emitSector(vol, files, reader, ctx, sector, dst)) return false;
    } else {
      uint8_t tmp[BYTES_PER_SECTOR];
      if (!emitSector(vol, files, reader, ctx, sector, tmp)) return false;
      memcpy(dst, tmp + inSector, chunk);
    }

    dst += chunk;
    off += chunk;
    len -= chunk;
  }
  return true;
}

} // namespace fat16
