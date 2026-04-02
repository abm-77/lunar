#pragma once

// umsh.h — .umsh unified shader asset format.
// a section-based binary container for one shader's SPIR-V stages and optional
// vertex reflection data. produced by uc; consumed by the runtime and ul.
//
// file layout:
//   umsh_header_t   (24 bytes)
//   umsh_section_t  (24 bytes) × section_count
//   section data (8-byte aligned)
//
// section IDs:
//   1 (STAGES): per-stage SPIR-V records + payloads
//   2 (REFL):   vertex input reflection; only present when shader has @vs_in
//
// unknown section IDs are ignored; callers skip using offset+size.

#include <stdbool.h>
#include <stdint.h>

#define UMSH_MAGIC 0x554D5348u // "UMSH"
#define UMSH_VERSION 1u
// written as uint16_t 0x1234; big-endian reads it as 0x3412 → reject
#define UMSH_ENDIAN_LE 0x1234u

#define UMSH_SECTION_STAGES 1u // stage records + SPIR-V payloads
#define UMSH_SECTION_REFL 2u   // vertex reflection (optional)

#define UMSH_STAGE_VERTEX 0u
#define UMSH_STAGE_FRAGMENT 1u

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t endian; // must read as UMSH_ENDIAN_LE
  uint32_t flags;  // reserved; must be 0
  uint32_t section_count;
  uint32_t total_bytes; // total file size in bytes including this header
  uint32_t _pad;        // padding to 24 bytes; must be 0
} umsh_header_t;

// each section descriptor; offsets are from start of file, 8-byte aligned.
typedef struct {
  uint32_t id;     // UMSH_SECTION_*
  uint32_t flags;  // reserved; 0
  uint64_t offset; // byte offset from file start to section data
  uint64_t size;   // byte count of section data
} umsh_section_t;

// per-stage record inside the STAGES section (12 bytes, tightly packed).
typedef struct {
  uint8_t kind; // UMSH_STAGE_VERTEX or UMSH_STAGE_FRAGMENT
  uint8_t _pad[3];
  uint32_t spv_offset; // byte offset from start of STAGES section data
  uint32_t spv_size;   // byte count; multiple of 4
} umsh_stage_record_t;

// STAGES section layout (starts immediately after the section descriptor
// offset):
//   u32 stage_count
//   stage_count × umsh_stage_record_t (12 bytes each)
//   SPIR-V payloads (concatenated, 4-byte aligned)

// REFL section layout (only present when shader has a @vs_in field):
//   u32 stride        vertex stride in bytes
//   u32 input_rate    0 = per-vertex, 1 = per-instance
//   u32 attr_count
//   attr_count × { u32 location, u32 vk_format, u32 offset } (12 bytes each)

// parsed view returned by umsh_find_section; aliases into the original blob.
typedef struct {
  const uint8_t *data; // pointer to section data start
  uint64_t size;       // size of section data in bytes
} umsh_section_view_t;

// validate the .umsh header in blob[0..blob_len).
// returns false if magic/endian/version are wrong or blob is too small.
// on success *section_data points to the first umsh_section_t in blob.
static inline bool umsh_parse_header(const uint8_t *blob, uint64_t blob_len,
                                     const umsh_header_t **out_hdr,
                                     uint32_t *out_section_count) {
  if (blob_len < sizeof(umsh_header_t)) return false;
  const umsh_header_t *h = (const umsh_header_t *)blob;
  if (h->magic != UMSH_MAGIC) return false;
  if (h->endian != UMSH_ENDIAN_LE) return false;
  if (h->version > UMSH_VERSION) return false;
  if (h->total_bytes > blob_len) return false;
  uint64_t min_size = sizeof(umsh_header_t) +
                      (uint64_t)h->section_count * sizeof(umsh_section_t);
  if (blob_len < min_size) return false;
  *out_hdr = h;
  *out_section_count = h->section_count;
  return true;
}

// find a section by id; returns false if not found.
// blob must have passed umsh_parse_header.
static inline bool umsh_find_section(const uint8_t *blob, uint64_t blob_len,
                                     uint32_t section_count, uint32_t id,
                                     umsh_section_view_t *out) {
  const umsh_section_t *secs =
      (const umsh_section_t *)(blob + sizeof(umsh_header_t));
  for (uint32_t i = 0; i < section_count; ++i) {
    if (secs[i].id != id) continue;
    if (secs[i].offset + secs[i].size > blob_len) return false;
    out->data = blob + secs[i].offset;
    out->size = secs[i].size;
    return true;
  }
  return false;
}

// locate SPIR-V words for a given stage inside a STAGES section view.
// out_words and out_word_count are set on success; they alias into sec.data.
static inline bool umsh_find_stage(umsh_section_view_t sec, uint8_t stage_kind,
                                   const uint32_t **out_words,
                                   uint32_t *out_word_count) {
  if (sec.size < 4) return false;
  const uint8_t *p = sec.data;
  uint32_t stage_count;
  __builtin_memcpy(&stage_count, p, 4);
  p += 4;
  if (sec.size < 4 + (uint64_t)stage_count * sizeof(umsh_stage_record_t))
    return false;
  const umsh_stage_record_t *recs = (const umsh_stage_record_t *)p;
  for (uint32_t i = 0; i < stage_count; ++i) {
    if (recs[i].kind != stage_kind) continue;
    uint64_t end = (uint64_t)recs[i].spv_offset + recs[i].spv_size;
    if (end > sec.size) return false;
    *out_words = (const uint32_t *)(sec.data + recs[i].spv_offset);
    *out_word_count = recs[i].spv_size / 4;
    return true;
  }
  return false;
}
