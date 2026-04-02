// asset.c — runtime asset registry backed by a .umpack file.
// assets are identified by the FNV-1a 64-bit hash of their basename (e.g., "TriShader.umsh").
// at init time the entire .umpack is loaded into memory and a flat table is built.
// rt_asset_load returns a pointer directly into that buffer (uncompressed entries only).
// compressed entries are a TODO: they print a warning and return an empty slice.

#include "asset.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// fnv1a64 — local copy to avoid depending on the C++ header.
static uint64_t asset_fnv1a64(const char *s, size_t n) {
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; ++i)
    h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
  return h;
}

// .umpack header layout constants (same as umpack.h in ul/).
#define UMPACK_MAGIC      0x554D504Bu
#define UMPACK_ENDIAN_LE  0x1234u
#define UMPACK_FLAG_COMPRESSED 0x1u

#define RT_MAX_ASSETS 1024u

typedef struct {
  uint64_t       id;
  const uint8_t *ptr;
  uint64_t       len;
} asset_entry_t;

static uint8_t      *s_pack_data     = NULL;  // entire .umpack loaded in memory
static uint64_t      s_pack_len      = 0;
static asset_entry_t s_entries[RT_MAX_ASSETS];
static uint32_t      s_entry_count   = 0;

// rt_assets_init — passed as (ptr, len) from Umbral's []u8 slice calling convention.
void rt_assets_init(const uint8_t *path_ptr, uint64_t path_len) {
  // null-terminate the path for fopen
  char path_buf[512];
  if (path_len >= sizeof(path_buf)) {
    fprintf(stderr, "rt_assets_init: pack path too long\n");
    return;
  }
  memcpy(path_buf, path_ptr, path_len);
  path_buf[path_len] = '\0';

  // free any previously loaded pack
  free(s_pack_data);
  s_pack_data   = NULL;
  s_pack_len    = 0;
  s_entry_count = 0;

  FILE *f = fopen(path_buf, "rb");
  if (!f) {
    fprintf(stderr, "rt_assets_init: cannot open '%s'\n", path_buf);
    return;
  }
  fseek(f, 0, SEEK_END);
  long file_sz = ftell(f);
  rewind(f);

  if (file_sz <= 0) {
    fclose(f);
    fprintf(stderr, "rt_assets_init: empty pack file '%s'\n", path_buf);
    return;
  }

  s_pack_data = (uint8_t *)malloc((size_t)file_sz);
  if (!s_pack_data) {
    fclose(f);
    fprintf(stderr, "rt_assets_init: out of memory\n");
    return;
  }
  s_pack_len = (uint64_t)file_sz;
  fread(s_pack_data, 1, (size_t)file_sz, f);
  fclose(f);

  // parse header
  if (s_pack_len < 16) { goto bad_format; }
  uint32_t magic;
  uint16_t version_unused, endian;
  uint32_t flags, entry_count;
  memcpy(&magic,        s_pack_data + 0,  4);
  memcpy(&version_unused, s_pack_data + 4, 2);
  memcpy(&endian,       s_pack_data + 6,  2);
  memcpy(&flags,        s_pack_data + 8,  4);
  memcpy(&entry_count,  s_pack_data + 12, 4);

  if (magic  != UMPACK_MAGIC)    { goto bad_format; }
  if (endian != UMPACK_ENDIAN_LE) { goto bad_format; }

  uint64_t cur = 16; // offset after the header
  for (uint32_t i = 0; i < entry_count; ++i) {
    if (cur + 4 > s_pack_len) goto bad_format;
    uint32_t name_len;
    memcpy(&name_len, s_pack_data + cur, 4);
    cur += 4;

    if (cur + name_len + 8 + 4 + 4 > s_pack_len) goto bad_format;
    const char *name = (const char *)(s_pack_data + cur);
    cur += name_len;

    uint64_t data_offset;
    uint32_t compressed_len, original_len;
    memcpy(&data_offset,    s_pack_data + cur,     8); cur += 8;
    memcpy(&compressed_len, s_pack_data + cur,     4); cur += 4;
    memcpy(&original_len,   s_pack_data + cur,     4); cur += 4;

    if (s_entry_count >= RT_MAX_ASSETS) {
      fprintf(stderr, "rt_assets_init: too many assets (max %u)\n", RT_MAX_ASSETS);
      break;
    }

    if (compressed_len != original_len) {
      // TODO: add LZ4 decompression support to the runtime
      fprintf(stderr, "rt_assets_init: compressed asset '%.*s' not yet supported; skipping\n",
              (int)name_len, name);
      continue;
    }

    if (data_offset + original_len > s_pack_len) goto bad_format;

    asset_entry_t *e = &s_entries[s_entry_count++];
    e->id  = asset_fnv1a64(name, name_len);
    e->ptr = s_pack_data + data_offset;
    e->len = original_len;
  }
  return;

bad_format:
  fprintf(stderr, "rt_assets_init: malformed pack file '%s'\n", path_buf);
  free(s_pack_data);
  s_pack_data = NULL;
  s_pack_len  = 0;
  s_entry_count = 0;
}

um_slice_u8_t rt_asset_load(uint64_t id) {
  for (uint32_t i = 0; i < s_entry_count; ++i) {
    if (s_entries[i].id == id)
      return (um_slice_u8_t){ s_entries[i].ptr, s_entries[i].len };
  }
  return (um_slice_u8_t){ NULL, 0 };
}

void rt_asset_release(uint64_t id) {
  // no-op in v0: all assets are borrowed from s_pack_data for the process lifetime
  (void)id;
}
