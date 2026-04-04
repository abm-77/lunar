#include "pack.h"
#include "umpack.h"
#include <common/bin_io.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lz4.h>
#include <string>
#include <vector>

// return the filename component of path (after last slash).
static std::string path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return std::string(slash ? slash + 1 : path);
}

// read entire file into a vector; returns empty vector on error.
static std::vector<uint8_t> load_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  std::vector<uint8_t> buf(sz > 0 ? (size_t)sz : 0u);
  if (sz > 0) fread(buf.data(), 1, (size_t)sz, f);
  fclose(f);
  return buf;
}

int pack_build(const pack_input_t *inputs, uint32_t input_count,
               const char *out_path, int compress) {
  struct Entry {
    std::string name;
    std::vector<uint8_t> payload; // on-disk bytes (compressed or raw)
    uint32_t original_len;
    uint32_t compressed_len;
    uint32_t meta_type;
    uint32_t meta[4];
  };

  std::vector<Entry> entries;
  entries.reserve(input_count);

  for (uint32_t i = 0; i < input_count; ++i) {
    // get the raw data: either from decoded_data or by reading the file
    std::vector<uint8_t> raw;
    if (inputs[i].decoded_data && inputs[i].decoded_len > 0) {
      raw.assign(inputs[i].decoded_data,
                 inputs[i].decoded_data + inputs[i].decoded_len);
    } else {
      raw = load_file(inputs[i].path);
      if (raw.empty()) {
        FILE *probe = fopen(inputs[i].path, "rb");
        if (!probe) {
          fprintf(stderr, "pack_build: cannot open %s\n", inputs[i].path);
          return -1;
        }
        fclose(probe);
      }
    }

    Entry e;
    e.name = path_basename(inputs[i].path);
    e.original_len = static_cast<uint32_t>(raw.size());
    e.meta_type = inputs[i].meta_type;
    memcpy(e.meta, inputs[i].meta, sizeof(e.meta));

    if (compress && !raw.empty()) {
      int bound = LZ4_compressBound(static_cast<int>(raw.size()));
      std::vector<uint8_t> comp(static_cast<size_t>(bound));
      int comp_sz =
          LZ4_compress_default(reinterpret_cast<const char *>(raw.data()),
                               reinterpret_cast<char *>(comp.data()),
                               static_cast<int>(raw.size()), bound);
      if (comp_sz > 0 && comp_sz < static_cast<int>(raw.size())) {
        comp.resize(static_cast<size_t>(comp_sz));
        e.compressed_len = static_cast<uint32_t>(comp_sz);
        e.payload = std::move(comp);
      } else {
        e.compressed_len = e.original_len;
        e.payload = std::move(raw);
      }
    } else {
      e.compressed_len = e.original_len;
      e.payload = std::move(raw);
    }

    entries.push_back(std::move(e));
  }

  // header: 16 bytes
  // per entry: 4(name_len) + name_len + 8(offset) + 4(comp) + 4(orig) + 4(meta_type) + 16(meta)
  const uint32_t HEADER_BYTES = 16;
  uint32_t manifest_bytes = 0;
  for (const auto &e : entries)
    manifest_bytes += 4 + static_cast<uint32_t>(e.name.size()) + 8 + 4 + 4 + 4 + 16;

  BinWriter w;
  w.u32(UMPACK_MAGIC);
  w.u16(UMPACK_VERSION);
  w.u16(UMPACK_ENDIAN_LE);
  w.u32(compress ? UMPACK_FLAG_COMPRESSED : 0u);
  w.u32(input_count);

  uint64_t cur_offset = HEADER_BYTES + manifest_bytes;
  for (const auto &e : entries) {
    w.u32(static_cast<uint32_t>(e.name.size()));
    w.bytes(reinterpret_cast<const uint8_t *>(e.name.data()), e.name.size());
    w.u64(cur_offset);
    w.u32(e.compressed_len);
    w.u32(e.original_len);
    w.u32(e.meta_type);
    for (int j = 0; j < 4; ++j) w.u32(e.meta[j]);
    cur_offset += e.compressed_len;
  }

  for (const auto &e : entries) w.bytes(e.payload.data(), e.payload.size());

  return w.write_file(out_path) ? 0 : -1;
}
