// dump_image.cpp — host-side harness to exercise fat16::build().
// Builds a sample FAT16 image and writes it (and the source files) to disk
// so tools/host_check.sh can validate with fsck.fat + mtools.
//
//   g++ -std=c++17 -I include src/fat16.cpp tools/dump_image.cpp -o dump_image
//   ./dump_image <image.img> <expected_dir/>
#include "fat16.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>

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

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <image.img> <expected_dir/>\n", argv[0]); return 2; }

  std::vector<Src> src;
  { Src s; s.name = "TurricanII.adf"; fill(s.bytes, "ADFTESTDATA-", 4096); src.push_back(std::move(s)); }
  { Src s; s.name = "This Is A Very Long Filename That Exceeds Eight Three.adf";
    fill(s.bytes, "LONGFILE-", 12345); src.push_back(std::move(s)); }
  { Src s; s.name = "short.adf"; fill(s.bytes, "SHORT", 5); src.push_back(std::move(s)); }
  { Src s; s.name = "Disk.ADF"; fill(s.bytes, "DISK", 901120); src.push_back(std::move(s)); } // full 880KiB

  std::vector<fat16::FileEntry> entries;
  for (auto& s : src) entries.push_back({ s.name.c_str(), (uint32_t)s.bytes.size() });

  std::vector<uint8_t> img(fat16::TOTAL_BYTES);
  uint32_t used = fat16::build(img.data(), entries.data(), (uint32_t)entries.size(),
                               reader, &src);
  if (used != fat16::TOTAL_BYTES) { fprintf(stderr, "build failed: %u\n", used); return 1; }

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
  return 0;
}
