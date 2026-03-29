#include "umrf_emit.h"
#include <common/bin_io.h>
#include <runtime/gfx/gfx_refl.h>
#include <cstdio>
#include <cstring>

uint32_t vk_format_for(const char *type_name) {
  if (!strcmp(type_name, "u32")) return 98;
  if (!strcmp(type_name, "i32")) return 99;
  if (!strcmp(type_name, "f32")) return 100;
  if (!strcmp(type_name, "vec2")) return 103;
  if (!strcmp(type_name, "vec3")) return 106;
  if (!strcmp(type_name, "vec4")) return 109;
  fprintf(stderr, "vk_format_for: unrecognized type '%s'\n", type_name);
  return 0;
}

uint32_t sizeof_umbral_type(const char *type_name) {
  if (!strcmp(type_name, "f32") || !strcmp(type_name, "i32") ||
      !strcmp(type_name, "u32"))
    return 4;
  if (!strcmp(type_name, "vec2")) return 8;
  if (!strcmp(type_name, "vec3")) return 12;
  if (!strcmp(type_name, "vec4")) return 16;
  if (!strcmp(type_name, "mat4")) return 64;
  fprintf(stderr, "sizeof_umbral_type: unrecognized type '%s'\n", type_name);
  return 0;
}

int umrf_emit(const Sidecar &sc, size_t shader_idx, const char *out_path) {
  if (shader_idx >= sc.shaders.size()) {
    fprintf(stderr, "umrf_emit: shader_idx out of range\n");
    return -1;
  }
  const ShaderType &shader = sc.shaders[shader_idx];

  // find the @vs_in annotated field (shader_field_kind == 1)
  const ShaderAnnot *vs_in_annot = nullptr;
  for (const auto &a : shader.annots) {
    if (a.shader_field_kind == 1) { vs_in_annot = &a; break; }
  }
  if (!vs_in_annot) {
    fprintf(stderr, "umrf_emit: shader '%s' has no @vs_in field\n",
            shader.name.c_str());
    return -1;
  }

  // find the pod type by name
  const PodType *pod = nullptr;
  for (const auto &p : sc.pods) {
    if (p.name == vs_in_annot->pod_type_name) { pod = &p; break; }
  }
  if (!pod) {
    fprintf(stderr, "umrf_emit: pod type '%s' not found\n",
            vs_in_annot->pod_type_name.c_str());
    return -1;
  }

  // accumulate stride; collect Location-bound attributes
  struct Attr { uint32_t location, vk_format, offset; };
  uint32_t stride = 0;
  std::vector<Attr> attrs;
  for (const auto &field : pod->fields) {
    stride += sizeof_umbral_type(field.type_name.c_str());
    if (field.io_kind == IOKind::Location)
      attrs.push_back({field.location_index,
                       vk_format_for(field.type_name.c_str()),
                       field.byte_offset});
    // BuiltinPosition has no VkFormat; skip for vertex attributes
  }

  uint32_t attr_count = static_cast<uint32_t>(attrs.size());
  // total_bytes: header(16) + binding(8) + attr_count(4) + attrs(12*N)
  uint32_t total_bytes = 16 + 8 + 4 + attr_count * 12;

  BinWriter w;
  // umrf_header_t
  w.u32(UMRF_MAGIC);
  w.u16(UMRF_VERSION);
  w.u16(UMRF_ENDIAN_LE);
  w.u32(total_bytes);
  w.u32(0); // _reserved
  // umrf_vertex_binding_t
  w.u32(stride);
  w.u32(UMRF_INPUT_RATE_VERTEX);
  // attr_count + umrf_vertex_attr_t[]
  w.u32(attr_count);
  for (const auto &a : attrs) {
    w.u32(a.location);
    w.u32(a.vk_format);
    w.u32(a.offset);
  }

  return w.write_file(out_path) ? 0 : -1;
}
