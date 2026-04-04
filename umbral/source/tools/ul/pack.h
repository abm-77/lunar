#pragma once
#include "umpack.h"
#include <stdint.h>

// input to pack_build. path is the original source file. if decoded_data is
// non-null, it replaces the file contents (used when ul decodes images/audio
// at pack time). meta_type and meta are written into the manifest.

typedef struct {
  const char *path;                // null-terminated path to source file
  const uint8_t *decoded_data;     // if non-null, use this instead of reading path
  uint32_t decoded_len;            // byte length of decoded_data
  uint32_t meta_type;              // UMPACK_META_RAW / IMAGE / AUDIO
  uint32_t meta[4];                // type-specific metadata
} pack_input_t;

// pack_build — assemble a .umpack v2 file.
//   inputs:      array of input entries
//   input_count: number of entries
//   out_path:    null-terminated destination path
//   compress:    non-zero to LZ4-compress each entry when smaller
//   returns 0 on success; -1 on error (prints reason to stderr)
int pack_build(const pack_input_t *inputs, uint32_t input_count,
               const char *out_path, int compress);
