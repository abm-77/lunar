#pragma once
#include "umpack.h"
#include <stdint.h>

// build a .umpack bundle from a list of already-processed asset files.
// inputs must be in their final runtime form (.umtex, .umaudio, .spv, .umrf, etc.).

typedef struct {
  const char *path;   // null-terminated path to source asset file
} pack_input_t;

// pack_build — assemble a .umpack file.
//   inputs:      array of input file paths (processed assets)
//   input_count: number of entries in inputs
//   out_path:    null-terminated destination path for the .umpack file
//   compress:    non-zero to attempt LZ4 compression per entry;
//                compressed only when result is strictly smaller than original
//   returns 0 on success; -1 on error (prints reason to stderr)
int pack_build(const pack_input_t *inputs, uint32_t input_count,
               const char *out_path, int compress);
