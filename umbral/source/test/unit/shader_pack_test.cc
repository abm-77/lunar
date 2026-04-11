// unit tests for shader_pack: .vert.spv + .frag.spv + .umrf → .umshader

#include "pack.h"
#include <gtest/gtest.h>
#include <runtime/gfx/umshader.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

static void write_dummy(const char *path, const void *data, size_t len) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(data), len);
}

static std::vector<uint8_t> read_all(const char *path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto sz = f.tellg();
  std::vector<uint8_t> buf(sz);
  f.seekg(0);
  f.read(reinterpret_cast<char *>(buf.data()), sz);
  return buf;
}

class ShaderPackTest : public ::testing::Test {
protected:
  std::filesystem::path tmp;
  void SetUp() override {
    tmp = std::filesystem::temp_directory_path() / "shader_pack_test";
    std::filesystem::create_directories(tmp);
  }
  void TearDown() override { std::filesystem::remove_all(tmp); }
};

TEST_F(ShaderPackTest, PackRoundTrip) {
  // write dummy .spv files (with SPIR-V magic 0x07230203)
  uint32_t spv_magic = 0x07230203u;
  uint32_t vs_data[] = {spv_magic, 0x00010000, 0xDEAD};
  uint32_t fs_data[] = {spv_magic, 0x00010000, 0xBEEF, 0xCAFE};
  uint8_t umrf_data[] = {0x55, 0x4D, 0x52, 0x46, 0x01, 0x00};

  auto vert_path = (tmp / "test.vert.spv").string();
  auto frag_path = (tmp / "test.frag.spv").string();
  auto umrf_path = (tmp / "test.umrf").string();
  auto out_path = (tmp / "test.umshader").string();

  write_dummy(vert_path.c_str(), vs_data, sizeof(vs_data));
  write_dummy(frag_path.c_str(), fs_data, sizeof(fs_data));
  write_dummy(umrf_path.c_str(), umrf_data, sizeof(umrf_data));

  int rc = shader_pack(vert_path.c_str(), frag_path.c_str(), umrf_path.c_str(),
                       out_path.c_str());
  ASSERT_EQ(rc, 0);

  auto blob = read_all(out_path.c_str());
  ASSERT_GE(blob.size(), sizeof(umshader_header_t));

  auto *hdr = reinterpret_cast<const umshader_header_t *>(blob.data());
  EXPECT_EQ(hdr->magic, UMSHADER_MAGIC);
  EXPECT_EQ(hdr->version, UMSHADER_VERSION);
  EXPECT_EQ(hdr->vs_size, sizeof(vs_data));
  EXPECT_EQ(hdr->fs_size, sizeof(fs_data));
  EXPECT_EQ(hdr->umrf_size, sizeof(umrf_data));
}

TEST_F(ShaderPackTest, UnpackContents) {
  uint32_t vs_data[] = {0x07230203u, 0xAAAA};
  uint32_t fs_data[] = {0x07230203u, 0xBBBB, 0xCCCC};
  uint8_t umrf_data[] = {1, 2, 3, 4};

  auto vert_path = (tmp / "t.vert.spv").string();
  auto frag_path = (tmp / "t.frag.spv").string();
  auto umrf_path = (tmp / "t.umrf").string();
  auto out_path = (tmp / "t.umshader").string();

  write_dummy(vert_path.c_str(), vs_data, sizeof(vs_data));
  write_dummy(frag_path.c_str(), fs_data, sizeof(fs_data));
  write_dummy(umrf_path.c_str(), umrf_data, sizeof(umrf_data));

  ASSERT_EQ(0, shader_pack(vert_path.c_str(), frag_path.c_str(),
                           umrf_path.c_str(), out_path.c_str()));

  auto blob = read_all(out_path.c_str());
  auto *hdr = reinterpret_cast<const umshader_header_t *>(blob.data());
  size_t off = sizeof(umshader_header_t);

  // verify vertex spv bytes match
  ASSERT_EQ(off + hdr->vs_size + hdr->fs_size + hdr->umrf_size, blob.size());
  EXPECT_EQ(0, memcmp(blob.data() + off, vs_data, sizeof(vs_data)));
  off += hdr->vs_size;

  // verify fragment spv bytes match
  EXPECT_EQ(0, memcmp(blob.data() + off, fs_data, sizeof(fs_data)));
  off += hdr->fs_size;

  // verify umrf bytes match
  EXPECT_EQ(0, memcmp(blob.data() + off, umrf_data, sizeof(umrf_data)));
}
