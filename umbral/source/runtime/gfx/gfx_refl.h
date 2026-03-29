#pragma once

// UMRF — Umbral Mesh Reflection Format.
// a minimal binary blob produced at asset-link time and consumed only by the
// runtime during pipeline creation to fill in
// VkPipelineVertexInputStateCreateInfo. the runtime never writes UMRF; it only
// parses it. all integer fields are little-endian; the endian sentinel allows
// detection of byte-swapped blobs.

#include <stdbool.h>
#include <stdint.h>

#define UMRF_MAGIC 0x554D5246u // ASCII "UMRF"
#define UMRF_VERSION 1u
// written as uint16_t 0x1234; if read back as 0x3412 the blob is big-endian and
// must be rejected
#define UMRF_ENDIAN_LE 0x1234u

// VK_VERTEX_INPUT_RATE_* mirror values
#define UMRF_INPUT_RATE_VERTEX 0u
#define UMRF_INPUT_RATE_INSTANCE 1u

// on-disk layout (sequential, no implicit  padding; assert sizes at parse time)
//  offset  size   field
//  0       16     umrf_header_t
//  16       8     umrf_vertex_binding_t
//  24       4     attr_count : uint32_t
//  28      12*N   umrf_vertex_attr_t[attr_count]

typedef struct {
  uint32_t magic;   // must equal UMRF_MAGIC; reject if not
  uint16_t version; // must equal UMRF_VERSION; reject if greater (future-proof)
  uint16_t endian;  // must read as UMRF_ENDIAN_LE; reject big-endian blobs
  uint32_t total_bytes; // full blob length in bytes including this header;
                        // sanity-check vs blob_len
  uint32_t _reserved;   // zero; reserved for flags in future versions
} umrf_header_t;

typedef struct {
  uint32_t stride;     // vertex stride in bytes; maps to
                       // VkVertexInputBindingDescription.stride
  uint32_t input_rate; // UMRF_INPUT_RATE_VERTEX or UMRF_INPUT_RATE_INSTANCE
} umrf_vertex_binding_t;

typedef struct {
  uint32_t location; // vertex shader input location: layout(location=N) in vec2
                     // aPos;
  uint32_t
      vk_format; // raw VkFormat enum value, e.g. VK_FORMAT_R32G32_SFLOAT = 103
  uint32_t offset; // byte offset from the vertex base to this attribute
} umrf_vertex_attr_t;

// parsed in-memory view; all pointer members alias into the original blob.
// blob must remain allocated for the lifetime of this struct.
typedef struct {
  const umrf_header_t *header;
  const umrf_vertex_binding_t *binding;
  uint32_t attr_count;
  const umrf_vertex_attr_t
      *attrs; // points into blob at offset 28; valid for attr_count entries
} umrf_parsed_t;

// validate and slice a UMRF blob in-place; zero-copy.
//   blob:     pointer to the raw bytes; must be non-NULL and at least
//   sizeof(umrf_header_t) long. blob_len: byte length of the blob. out: filled
//   with aliased pointers into blob on success; unmodified on failure. returns
//   true on success; false if:
//     - blob_len < sizeof(umrf_header_t) + sizeof(umrf_vertex_binding_t) + 4
//     - magic != UMRF_MAGIC
//     - endian != UMRF_ENDIAN_LE (big-endian machine or corrupted blob)
//     - version > UMRF_VERSION
//     - blob_len < required size to hold attr_count entries
bool umrf_parse(const uint8_t *blob, uint64_t blob_len, umrf_parsed_t *out);
