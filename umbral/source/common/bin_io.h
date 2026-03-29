#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

struct BinWriter {
  std::vector<uint8_t> buf;

  void u8(uint8_t v) { buf.push_back(v); }

  void u16(uint16_t v) {
    buf.push_back(v & 0xff);
    buf.push_back((v >> 8) & 0xff);
  }

  void u32(uint32_t v) {
    buf.push_back(v & 0xff);
    buf.push_back((v >> 8) & 0xff);
    buf.push_back((v >> 16) & 0xff);
    buf.push_back((v >> 24) & 0xff);
  }

  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v));
    u32(static_cast<uint32_t>(v >> 32));
  }

  void f64(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, 8);
    u64(bits);
  }

  // u32 byte-length prefix followed by UTF-8 bytes (no null terminator)
  void str(std::string_view s) {
    u32(static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
  }

  void bytes(const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
  }

  // write buf to path; returns false and prints to stderr on error
  bool write_file(const char *path) const {
    FILE *f = fopen(path, "wb");
    if (!f) {
      fprintf(stderr, "bin_io: cannot write %s\n", path);
      return false;
    }
    if (!buf.empty()) fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    return true;
  }
};

struct BinReader {
  const uint8_t *data;
  size_t size;
  size_t pos = 0;
  bool ok = true;

  bool has(size_t n) const { return pos + n <= size; }

  uint8_t u8() {
    if (!has(1)) {
      ok = false;
      return 0;
    }
    return data[pos++];
  }

  uint16_t u16() {
    if (!has(2)) {
      ok = false;
      return 0;
    }
    uint16_t v;
    memcpy(&v, data + pos, 2);
    pos += 2;
    return v;
  }

  uint32_t u32() {
    if (!has(4)) {
      ok = false;
      return 0;
    }
    uint32_t v;
    memcpy(&v, data + pos, 4);
    pos += 4;
    return v;
  }

  uint64_t u64() {
    if (!has(8)) {
      ok = false;
      return 0;
    }
    uint64_t v;
    memcpy(&v, data + pos, 8);
    pos += 8;
    return v;
  }

  double f64() {
    if (!has(8)) {
      ok = false;
      return 0.0;
    }
    double v;
    memcpy(&v, data + pos, 8);
    pos += 8;
    return v;
  }

  // reads u32 length then that many bytes; returns empty string on error
  std::string str() {
    uint32_t len = u32();
    if (!ok || !has(len)) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char *>(data + pos), len);
    pos += len;
    return s;
  }
};
