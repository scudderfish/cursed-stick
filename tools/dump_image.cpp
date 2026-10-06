// dump_image.cpp — host-side harness for fat16::layout()/fat16::readBytes() and
// fat16::storableName() (the name /upload falls back to when the filesystem
// refuses what the client sent).
// Builds a sample FAT16 volume by rendering every sector through readBytes() —
// the same call the device's MSC callback makes — and writes it (and the source
// files) to disk so tools/host_check.sh can validate with fsck.fat + mtools.
//
//   g++ -std=c++17 -I include src/fat16.cpp tools/dump_image.cpp -o dump_image
//   ./dump_image <image.img> <expected_dir/>
#include "fat16.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cctype>

struct Src { std::string name; std::vector<uint8_t> bytes; };

static bool reader(void* ctx, uint32_t fileIndex, uint32_t offset,
                   uint8_t* dst, uint32_t n) {
  auto* files = (std::vector<Src>*)ctx;
  const auto& f = (*files)[fileIndex];
  if (offset + n > f.bytes.size()) return false;
  memcpy(dst, f.bytes.data() + offset, n);
  return true;
}

static void fill(std::vector<uint8_t>& v, const char* pattern, size_t n) {
  size_t plen = strlen(pattern);
  v.resize(n);
  for (size_t i = 0; i < n; i++) v[i] = (uint8_t)pattern[i % plen];
}

// --- what the stick stores: the client's name, shortened only when the
// filesystem refuses it (a uploadfs-seeded LittleFS caps names at 32 bytes).
// The name presented over USB is generated from what is stored, so this must
// keep the extension and stay deterministic.
static int nameFailures = 0;

// expect == nullptr asserts only the invariants (fits, deterministic, and for a
// shortened name: '~' + 6 hex digits + the whole extension preserved).
static void checkStorable(const char* name, uint32_t maxLen, const char* expect) {
  char out[256];
  if (!fat16::storableName(name, maxLen, out)) {
    printf("  NAME %-88s -> (refused)\n", name);
    nameFailures++;
    return;
  }
  uint32_t len = (uint32_t)strlen(out);
  char again[256];
  bool stable = fat16::storableName(name, maxLen, again) && !strcmp(out, again);
  bool fits = len <= maxLen;
  bool shape = true;
  if (expect == nullptr) {
    const char* tilde = strchr(out, '~');
    shape = tilde != nullptr && strlen(tilde + 1) >= 6 && isxdigit((unsigned char)tilde[1]);
    for (int i = 0; i < 6; i++)
      if (!isxdigit((unsigned char)tilde[1 + i])) shape = false;
    const char* dot = strrchr(name, '.');
    if (dot && dot != name) shape = shape && !strcmp(out + len - strlen(dot), dot);
  }
  bool ok = fits && stable && shape && (expect == nullptr || !strcmp(out, expect));
  printf("  NAME %-88s -> %-24s %s\n", name, out,
         ok ? "ok" : (fits ? (stable ? (shape ? "MISMATCH" : "BAD SHAPE") : "UNSTABLE")
                           : "TOO LONG"));
  if (!ok) nameFailures++;
}

static void checkNames() {
  printf("== storableName (what /upload stores when the FS refuses the name) ==\n");
  checkStorable("short.adf", 32, "short.adf");           // fits: kept verbatim
  checkStorable("FF.CFG", 32, "FF.CFG");                 // override path unchanged
  checkStorable("TurricanII.adf", 32, "TurricanII.adf");
  checkStorable("Workbench v3.1 rev 40.42 (1994)(Commodore)(Disk 1 of 6)(Install).adf",
                32, nullptr);                            // shortened, extension kept
  checkStorable("Very Long Amiga Disk Image Name Here.dsk", 32, nullptr);
  checkStorable("a-very-long-name-with-no-extension-at-all", 32, nullptr);

  // The other end of the budget range. Once the firmware has formatted the
  // filesystem itself (see AGENTS.md) names up to 255 bytes — the VFAT LFN
  // limit — are stored verbatim; anything longer is shortened to that budget.
  {
    std::string max = "Workbench v3.1 rev 40.42 (1994)(Commodore)(Disk 1 of 6)(Install)[m drive definitions]";
    while (max.size() < 251) max += "X";
    max += ".adf";                                      // exactly 255 bytes
    checkStorable(max.c_str(), 255, max.c_str());       // at the limit: verbatim
    std::string over = max;
    over.insert(over.size() - 4, "YYYYY");              // 260 bytes
    checkStorable(over.c_str(), 255, nullptr);          // shortened to fit 255
  }

  // The whole point: look-alike long names stay distinct (so one disk cannot
  // clobber another) while the same name is stable (so a re-upload replaces it).
  const char* long1 = "Workbench v3.1 rev 40.42 (1994)(Commodore)(Disk 1 of 6)(Install).adf";
  const char* long2 = "Workbench v3.1 rev 40.42 (1994)(Commodore)(Disk 1 of 6)(Extras).adf";
  char a[64], b[64];
  bool ok = fat16::storableName(long1, 32, a) && fat16::storableName(long2, 32, b) &&
            strcmp(a, b) != 0;
  printf("  %-101s %s\n", "look-alike long names stay distinct", ok ? "ok" : "MISMATCH");
  if (!ok) nameFailures++;
  printf("       '%s'\n       '%s'\n", a, b);

  // Too small a budget cannot hold the digest and extension: report failure
  // rather than emitting a name that will not store.
  char tiny[16];
  ok = !fat16::storableName("Some Long Name.adf", 10, tiny);
  printf("  %-101s %s\n", "budget 10 with .adf -> refused", ok ? "ok" : "MISMATCH");
  if (!ok) nameFailures++;

  if (nameFailures) printf("  %d NAME CHECK(S) FAILED\n", nameFailures);
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <image.img> <expected_dir/>\n", argv[0]); return 2; }

  checkNames();

  std::vector<Src> src;
  { Src s; s.name = "TurricanII.adf"; fill(s.bytes, "ADFTESTDATA-", 4096); src.push_back(std::move(s)); }
  { Src s; s.name = "This Is A Very Long Filename That Exceeds Eight Three.adf";
    fill(s.bytes, "LONGFILE-", 12345); src.push_back(std::move(s)); }
  { Src s; s.name = "short.adf"; fill(s.bytes, "SHORT", 5); src.push_back(std::move(s)); }
  { Src s; s.name = "Disk.ADF"; fill(s.bytes, "DISK", 901120); src.push_back(std::move(s)); } // full 880KiB
  // A file stored under the shortened name /upload uses when the filesystem will
  // not take the original: the image must still present it with its extension.
  { char stored[33];
    if (!fat16::storableName(
            "Workbench v3.1 rev 40.42 (1994)(Commodore)(Disk 1 of 6)(Install).adf",
            32, stored)) { fprintf(stderr, "storableName failed\n"); return 1; }
    Src s; s.name = stored; fill(s.bytes, "TRUNCATED-", 2048); src.push_back(std::move(s)); }
  // And the other end of the range: a full-length name, as a firmware-formatted
  // filesystem now accepts it verbatim — 255 bytes is 20 VFAT LFN entries plus
  // the 8.3 entry, and fsck.fat/mdir/mcopy must handle that many end to end.
  // The name deliberately avoids '[' and ']': mtools treats those as wildcard
  // classes, so `mcopy "::Workbench[m foo].adf"` says "not found" even though
  // the LFN is perfect (mtools cannot read back its own such images either).
  { char stored[256];
    std::string max = "Workbench v3.1 rev 40.42 (1994)(Commodore)(Disk 1 of 6)(Install) m drive definitions";
    while (max.size() < 251) max += "X";
    max += ".adf";
    if (!fat16::storableName(max.c_str(), 255, stored)) {
      fprintf(stderr, "storableName failed (255)\n"); return 1;
    }
    Src s; s.name = stored; fill(s.bytes, "LONGNAME-", 3072); src.push_back(std::move(s)); }

  std::vector<fat16::FileEntry> entries;
  for (auto& s : src) entries.push_back({ s.name.c_str(), (uint32_t)s.bytes.size() });

  fat16::Volume vol;
  if (!fat16::layout(entries.data(), (uint32_t)entries.size(), &vol)) {
    fprintf(stderr, "layout failed\n"); return 1;
  }

  // Render the volume exactly the way the device's MSC callback does: every byte
  // is produced on demand by fat16::readBytes(). The fsck.fat + mtools checks
  // below therefore validate the streamed result, not a one-shot buffer.
  std::vector<uint8_t> img(fat16::TOTAL_BYTES);
  for (uint32_t off = 0; off < fat16::TOTAL_BYTES; off += fat16::BYTES_PER_SECTOR) {
    if (!fat16::readBytes(vol, entries.data(), reader, &src, off,
                          img.data() + off, fat16::BYTES_PER_SECTOR)) {
      fprintf(stderr, "readBytes failed at offset %u\n", off); return 1;
    }
  }

  // The MSC callback may ask for any (offset, length), so a request that does
  // not line up with a sector boundary or a file boundary must return the same
  // bytes as the whole-sector render above.
  {
    std::vector<uint8_t> scratch(4096);
    bool same = true;
    for (uint32_t off = 0; off + 4096 <= fat16::TOTAL_BYTES && same; off += 4096) {
      same = fat16::readBytes(vol, entries.data(), reader, &src, off + 1,
                              scratch.data(), 4096 - 3) &&
             memcmp(scratch.data(), img.data() + off + 1, 4096 - 3) == 0;
      if (!same) break;
      same = fat16::readBytes(vol, entries.data(), reader, &src, off + 3000,
                              scratch.data(), 512) &&
             memcmp(scratch.data(), img.data() + off + 3000, 512) == 0;
    }
    printf("  %-101s %s\n", "partial reads match whole-sector reads", same ? "ok" : "MISMATCH");
    if (!same) { fprintf(stderr, "partial read mismatch\n"); return 1; }
  }

  // The volume spans the whole LittleFS partition, so a file larger than it (or
  // a set of names too long for the 512-entry root directory) must be refused by
  // layout() rather than wrapped around into other files' clusters.
  {
    fat16::FileEntry big[1] = { { "too-big.adf", fat16::TOTAL_BYTES } };
    fat16::Volume v;
    bool refused = !fat16::layout(big, 1, &v);
    printf("  %-101s %s\n", "file larger than the volume refused", refused ? "ok" : "MISMATCH");
    if (!refused) { fprintf(stderr, "layout accepted an oversized file\n"); return 1; }
  }
  {
    static std::string names[fat16::MAX_FILES];
    fat16::FileEntry many[fat16::MAX_FILES];
    for (uint32_t i = 0; i < fat16::MAX_FILES; i++) {
      names[i] = "Disk " + std::to_string(i) + " ";
      while (names[i].size() < 251) names[i] += "Y";   // 255 bytes with ".adf"
      names[i] += ".adf";
      many[i].name = names[i].c_str();
      many[i].size = 1024;
    }
    fat16::Volume v;
    bool refused = !fat16::layout(many, fat16::MAX_FILES, &v);
    printf("  %-101s %s\n", "root directory overflow refused", refused ? "ok" : "MISMATCH");
    if (!refused) { fprintf(stderr, "layout overfilled the root directory\n"); return 1; }
  }

  FILE* f = fopen(argv[1], "wb");
  if (!f) { perror("fopen image"); return 1; }
  fwrite(img.data(), 1, img.size(), f);
  fclose(f);

  // write expected source files for roundtrip comparison
  for (auto& s : src) {
    std::string path = std::string(argv[2]) + "/" + s.name;
    FILE* g = fopen(path.c_str(), "wb");
    if (!g) { perror("fopen expected"); return 1; }
    fwrite(s.bytes.data(), 1, s.bytes.size(), g);
    fclose(g);
  }

  printf("built %zu bytes, %zu files\n", img.size(), entries.size());
  if (nameFailures) { fprintf(stderr, "%d name check(s) failed\n", nameFailures); return 1; }
  return 0;
}
