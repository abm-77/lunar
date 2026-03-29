#pragma once

#include "shader_link.h"
#include <stdint.h>

// map an Umbral type name to its VkFormat enum value.
// returns 0 if the type name is unrecognized.
//
// table:
//   "f32"  → VK_FORMAT_R32_SFLOAT         = 100
//   "vec2" → VK_FORMAT_R32G32_SFLOAT      = 103
//   "vec3" → VK_FORMAT_R32G32B32_SFLOAT   = 106
//   "vec4" → VK_FORMAT_R32G32B32A32_SFLOAT = 109
//   "u32"  → VK_FORMAT_R32_UINT           = 98
//   "i32"  → VK_FORMAT_R32_SINT           = 99
uint32_t vk_format_for(const char *type_name);

// return the byte size of a shader-legal Umbral type.
// used to compute the per-vertex stride.
uint32_t sizeof_umbral_type(const char *type_name);

// build a UMRF blob for the @vs_in pod of a shader and write it.
//   sc:         parsed .umshaders data
//   shader_idx: index into sc.shaders for the target shader type
//   out_path:   null-terminated destination path (e.g. "sprite.umrf")
//   returns 0 on success; -1 on error
int umrf_emit(const Sidecar &sc, size_t shader_idx, const char *out_path);
