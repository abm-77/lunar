#pragma once
#include <stdint.h>

// clang-format off
// .umpack binary format (v2).
// all integers little-endian.
//
// layout:
//   u32  magic        = UMPACK_MAGIC
//   u16  version      = UMPACK_VERSION (2)
//   u16  endian       = UMPACK_ENDIAN_LE  (reject if 0x3412)
//   u32  flags        (bit 0 = UMPACK_FLAG_COMPRESSED; entries may be LZ4-compressed)
//   u32  entry_count
//   [entry 0..entry_count-1]:
//     u32  path_len
//     u8   path[path_len]    (UTF-8, no null terminator; basename of the asset file)
//     u64  data_offset       (byte offset from start of file to this entry's data)
//     u32  compressed_len    (bytes on disk; equals original_len when uncompressed)
//     u32  original_len      (bytes after decompression)
//     u32  meta_type         (0=raw, 1=image, 2=audio)
//     u32  meta[4]           (type-specific metadata, 16 bytes)
//   [data region]:
//     entry 0 data ... entry N-1 data  (sequentially; offsets in manifest)
//
// meta_type=0 (raw): meta unused. data is the original file bytes.
// meta_type=1 (image): meta = {width, height, channels(=4), 0}.
//   data = decoded RGBA8 pixels, width * height * 4 bytes before compression.
// meta_type=2 (audio): meta = {frame_count_lo, frame_count_hi, channels, sample_rate}.
//   data = decoded float32 interleaved PCM, frame_count * channels * 4 bytes.
// clang-format on

#define UMPACK_MAGIC 0x554D504Bu // "UMPK"
#define UMPACK_VERSION 2u
#define UMPACK_ENDIAN_LE 0x1234u

#define UMPACK_FLAG_COMPRESSED 0x1u

#define UMPACK_META_RAW   0u
#define UMPACK_META_IMAGE 1u
#define UMPACK_META_AUDIO 2u

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t endian;
  uint32_t flags;
  uint32_t entry_count;
} umpack_header_t;

typedef struct {
  const char *path; // points into a caller-owned buffer; path_len bytes (not
                    // null-terminated)
  uint32_t path_len;
  uint64_t data_offset;
  uint32_t compressed_len;
  uint32_t original_len;
  uint32_t meta_type;
  uint32_t meta[4];
} umpack_entry_t;
