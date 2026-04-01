#include "shader_pack.h"
#include <runtime/gfx/umshader.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

static bool read_file(const char *path, std::vector<uint8_t> &out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    fprintf(stderr, "shader_pack: can't open %s\n", path);
    return false;
  }
  auto sz = f.tellg();
  if (sz < 0) return false;
  out.resize(static_cast<size_t>(sz));
  f.seekg(0);
  f.read(reinterpret_cast<char *>(out.data()), sz);
  return !!f;
}

int shader_pack(const char *vert_spv_path, const char *frag_spv_path,
                const char *umrf_path, const char *out_path) {
  std::vector<uint8_t> vs, fs, umrf;
  if (!read_file(vert_spv_path, vs)) return -1;
  if (!read_file(frag_spv_path, fs)) return -1;
  if (!read_file(umrf_path, umrf)) return -1;

  umshader_header_t hdr{};
  hdr.magic = UMSHADER_MAGIC;
  hdr.version = UMSHADER_VERSION;
  hdr._reserved = 0;
  hdr.vs_size = static_cast<uint32_t>(vs.size());
  hdr.fs_size = static_cast<uint32_t>(fs.size());
  hdr.umrf_size = static_cast<uint32_t>(umrf.size());

  std::ofstream f(out_path, std::ios::binary);
  if (!f) {
    fprintf(stderr, "shader_pack: can't create %s\n", out_path);
    return -1;
  }

  f.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
  f.write(reinterpret_cast<const char *>(vs.data()), vs.size());
  f.write(reinterpret_cast<const char *>(fs.data()), fs.size());
  f.write(reinterpret_cast<const char *>(umrf.data()), umrf.size());

  if (!f) {
    fprintf(stderr, "shader_pack: write failed for %s\n", out_path);
    return -1;
  }
  return 0;
}
