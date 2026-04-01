#pragma once

// draw_packet_t field layout — shared between the runtime (gfx.h) and the
// compiler (shader MLIR lowering). field order matches the struct definition
// in gfx.h and the SSBO layout on the GPU. do NOT reorder without updating
// both the C struct and this table.

#ifdef __cplusplus

#include <cstdint>

struct DrawPacketField {
  const char *name;
  uint32_t index;
  uint32_t size; // bytes: 8 for u64, 4 for u32
};

// field 0-2 are u64; fields 3-12 are u32
inline constexpr DrawPacketField kDrawPacketFields[] = {
    {"pipeline_handle",      0, 8},
    {"vertex_buffer_handle", 1, 8},
    {"index_buffer_handle",  2, 8},
    {"first_index",          3, 4},
    {"index_count",          4, 4},
    {"vertex_count",         5, 4},
    {"instance_count",       6, 4},
    {"first_instance",       7, 4},
    {"draw_data_offset",     8, 4},
    {"material_data_offset", 9, 4},
    {"tex2d_index",         10, 4},
    {"sampler_index",       11, 4},
    {"flags",               12, 4},
};

inline constexpr uint32_t kDrawPacketFieldCount = 13;

// look up field index by name; returns -1 if not found.
inline int draw_packet_field_index(const char *name) {
  for (uint32_t i = 0; i < kDrawPacketFieldCount; ++i)
    if (__builtin_strcmp(kDrawPacketFields[i].name, name) == 0)
      return static_cast<int>(i);
  return -1;
}

// returns true if the field at the given index is u64 (first three fields).
inline bool draw_packet_field_is_u64(int index) {
  return index >= 0 && index < 3;
}

#endif // __cplusplus
