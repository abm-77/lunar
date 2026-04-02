#pragma once

// .umshader bundle format — packed by ul from pre-built .spv + .umrf files.
// layout: header, vs_spv, fs_spv, umrf (all lengths in bytes).

#include <stdint.h>

#define UMSHADER_MAGIC 0x554D5341u // ASCII "UMSA"
#define UMSHADER_VERSION 1u

typedef struct {
  uint32_t magic;     // must equal UMSHADER_MAGIC
  uint16_t version;   // must equal UMSHADER_VERSION
  uint16_t _reserved; // zero
  uint32_t vs_size;   // bytes of vertex SPIR-V
  uint32_t fs_size;   // bytes of fragment SPIR-V
  uint32_t umrf_size; // bytes of UMRF blob
} umshader_header_t;

// after the header:
//   uint8_t vs_spv[vs_size]
//   uint8_t fs_spv[fs_size]
//   uint8_t umrf[umrf_size]
