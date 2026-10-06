// fat16.cpp — deterministic FAT16 image builder.
//
#include "fat16.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

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

// ---------------------------------------------------------------------------
// boot sector
// ---------------------------------------------------------------------------
static void writeBootSector(uint8_t* buf) {
  memset(buf, 0, BYTES_PER_SECTOR);
  buf[0] = 0xEB; buf[1] = 0x3C; buf[2] = 0x90;
  memcpy(buf + 3, "MSDOS5.0", 8);
  put16(buf + 0x0B, BYTES_PER_SECTOR);                 // bytes per sector
  buf[0x0D] = (uint8_t)SECTORS_PER_CLUSTER;           // sectors per cluster
  put16(buf + 0x0E, RESERVED_SECTORS);                // reserved sectors
  buf[0x10] = (uint8_t)NUM_FATS;                      // number of FATs
  put16(buf + 0x11, ROOT_ENTRIES);                    // root directory entries
  put16(buf + 0x13, TOTAL_SECTORS);                   // total sectors (16-bit)
  buf[0x15] = 0xF8;                                   // media descriptor (fixed disk)
  put16(buf + 0x16, FAT_SECTORS);                     // sectors per FAT
  put16(buf + 0x18, 63);                              // sectors per track (cosmetic)
  put16(buf + 0x1A, 255);                             // heads (cosmetic)
  put32(buf + 0x1C, 0);                               // hidden sectors
  put32(buf + 0x20, 0);                               // total sectors (32-bit, unused FAT16)
  buf[0x24] = 0x80;                                   // drive number
  buf[0x25] = 0;
  buf[0x26] = 0x29;                                   // extended boot signature
  put32(buf + 0x27, 0x41444631u);                     // volume serial "ADF1"
  memcpy(buf + 0x2B, "CURSEDSTICK", 11);              // volume label (must equal rootDir's)
  memcpy(buf + 0x36, "FAT16   ", 8);                  // filesystem type
  buf[510] = 0x55; buf[511] = 0xAA;                   // boot signature
}

// ---------------------------------------------------------------------------
// FAT
// ---------------------------------------------------------------------------
static inline uint8_t* fatPtr(uint8_t* buf, int fatIndex) {
  return buf + (uint32_t)(FAT1_SECTOR + fatIndex * FAT_SECTORS) * BYTES_PER_SECTOR;
}

static void initFats(uint8_t* buf) {
  for (int f = 0; f < NUM_FATS; f++) {
    uint8_t* fat = fatPtr(buf, f);
    memset(fat, 0, FAT_SECTORS * BYTES_PER_SECTOR);
    fat[0] = 0xF8; fat[1] = 0xFF;    // FAT[0] = 0xFFF8
    fat[2] = 0xFF; fat[3] = 0xFF;    // FAT[1] = 0xFFFF
  }
}

static void setFatEntry(uint8_t* buf, uint32_t cluster, uint16_t value) {
  uint32_t off = cluster * 2;
  for (int f = 0; f < NUM_FATS; f++) put16(fatPtr(buf, f) + off, value);
}

static inline uint8_t* dataPtr(uint8_t* buf, uint32_t cluster) {
  return buf + (uint32_t)(DATA_SECTOR + (cluster - 2) * SECTORS_PER_CLUSTER) * BYTES_PER_SECTOR;
}

// ---------------------------------------------------------------------------
// short names (8.3)
// ---------------------------------------------------------------------------
uint8_t lfnChecksum(const uint8_t shortName[11]) {
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
// The FAT-facing name is generated by build(); this maps a name the *filesystem*
// will accept. It lives here because fat16.cpp is the only unit the host check
// compiles (see tools/host_check.sh).

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

// ---------------------------------------------------------------------------
// build
// ---------------------------------------------------------------------------
uint32_t build(uint8_t* buf, const FileEntry* files, uint32_t count,
               FileReader reader, void* ctx) {
  if (!buf) return 0;
  if (count && (!files || !reader)) return 0;

  // boot sector, FATs, root dir, data region
  writeBootSector(buf);
  initFats(buf);
  uint8_t* rootDir = buf + (uint32_t)ROOT_SECTOR * BYTES_PER_SECTOR;
  memset(rootDir, 0, ROOT_DIR_SECTORS * BYTES_PER_SECTOR);
  memset(buf + (uint32_t)DATA_SECTOR * BYTES_PER_SECTOR, 0,
         DATA_SECTORS * BYTES_PER_SECTOR);

  // volume label entry (optional, matches mkfs.fat)
  {
    uint8_t* e = rootDir  ;
    memcpy(e, "CURSEDSTICK", 11);
    e[11] = 0x08;                   // volume label attribute
  }

  ShortName* shortNames = (ShortName*)calloc(count ? count : 1, sizeof(ShortName));
  if (!shortNames) return 0;
  if (!resolveShortNames(files, count, shortNames)) { free(shortNames); return 0; }

  uint32_t rootSlot = 1;            // slot 0 = volume label
  uint32_t nextCluster = 2;

  for (uint32_t i = 0; i < count; i++) {
    const char* nm = files[i].name;
    uint32_t nameLen = (uint32_t)strlen(nm);
    if (nameLen > 255) { free(shortNames); return 0; }

    uint16_t units[256];
    for (uint32_t k = 0; k < nameLen; k++) units[k] = (uint16_t)(uint8_t)nm[k]; // ASCII
    uint32_t unitCount = nameLen + 1;              // + null terminator
    uint32_t lfnCount = (unitCount + 12) / 13;     // 13 chars per entry

    uint32_t clustersNeeded = files[i].size
        ? (files[i].size + CLUSTER_SIZE - 1) / CLUSTER_SIZE : 0;

    if (rootSlot + lfnCount + 1 > ROOT_ENTRIES) { free(shortNames); return 0; }
    if (clustersNeeded && nextCluster + clustersNeeded - 1 > LAST_CLUSTER) {
      free(shortNames); return 0;
    }

    uint16_t firstCluster = 0;
    if (clustersNeeded) {
      firstCluster = (uint16_t)nextCluster;
      for (uint32_t c = 0; c < clustersNeeded; c++) {
        uint32_t cl = nextCluster + c;
        uint16_t next = (c == clustersNeeded - 1) ? 0xFFFF : (uint16_t)(cl + 1);
        setFatEntry(buf, cl, next);
      }
      uint32_t remaining = files[i].size;
      uint32_t srcOff = 0;
      for (uint32_t c = 0; c < clustersNeeded; c++) {
        uint32_t cl = nextCluster + c;
        uint32_t chunk = remaining < CLUSTER_SIZE ? remaining : CLUSTER_SIZE;
        uint8_t* dst = dataPtr(buf, cl);
        if (chunk) {
          if (!reader(ctx, i, srcOff, dst, chunk)) { free(shortNames); return 0; }
          memset(dst + chunk, 0, CLUSTER_SIZE - chunk);
        } else {
          memset(dst, 0, CLUSTER_SIZE);
        }
        srcOff += chunk;
        remaining -= chunk;
      }
      nextCluster += clustersNeeded;
    }

    // LFN entries: chunk (N-1) first (seq 0x40|N) ... chunk 0 last (seq 1)
    uint8_t checksum = lfnChecksum((const uint8_t*)shortNames[i].bytes);
    uint8_t* slot = rootDir + rootSlot * 32;
    for (uint32_t e = 0; e < lfnCount; e++) {
      uint32_t chunkIndex = lfnCount - 1 - e;
      uint16_t chunk[13];
      for (int k = 0; k < 13; k++) {
        uint32_t idx = chunkIndex * 13 + k;
        if (idx < nameLen)       chunk[k] = units[idx];
        else if (idx == nameLen) chunk[k] = 0x0000;
        else                     chunk[k] = 0xFFFF;
      }
      uint8_t seq = (uint8_t)(chunkIndex + 1);
      if (chunkIndex == lfnCount - 1) seq |= 0x40;
      writeLfnEntry(slot, seq, chunk, checksum);
      slot += 32;
      rootSlot++;
    }

    writeShortEntry(slot, (const uint8_t*)shortNames[i].bytes, firstCluster, files[i].size);
    rootSlot++;
  }

  free(shortNames);
  return TOTAL_BYTES;
}

} // namespace fat16
