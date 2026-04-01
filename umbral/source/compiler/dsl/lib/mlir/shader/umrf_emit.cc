// emit UMRF vertex reflection blob directly from Module/TypeAst data.
// replaces the old ul/umrf_emit.cc which read from sidecar string tables.

#include <um/shader/umrf_emit.h>

#include <common/bin_io.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/module.h>
#include <runtime/gfx/gfx_refl.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace um::shader {

static uint32_t vk_format_for(std::string_view type_name) {
  if (type_name == "u32") return 98;
  if (type_name == "i32") return 99;
  if (type_name == "f32") return 100;
  if (type_name == "vec2") return 103;
  if (type_name == "vec3") return 106;
  if (type_name == "vec4") return 109;
  fprintf(stderr, "umrf: unrecognized type '%.*s'\n",
          (int)type_name.size(), type_name.data());
  return 0;
}

static uint32_t sizeof_type(std::string_view type_name) {
  if (type_name == "f32" || type_name == "i32" || type_name == "u32") return 4;
  if (type_name == "vec2") return 8;
  if (type_name == "vec3") return 12;
  if (type_name == "vec4") return 16;
  if (type_name == "mat4") return 64;
  fprintf(stderr, "umrf: unrecognized type '%.*s'\n",
          (int)type_name.size(), type_name.data());
  return 0;
}

bool emit_umrf(const LoadedModule &lm, const Interner &interner,
               SymId shader_type, std::string_view out_dir) {
  // find the @vs_in field on the @shader struct
  SymId vs_in_field = 0;
  for (const ShaderFieldAnnot &sfa : lm.mod.shader_field_annots) {
    if (sfa.struct_name == shader_type &&
        sfa.kind == ShaderFieldKind::VsIn) {
      vs_in_field = sfa.field_name;
      break;
    }
  }
  if (!vs_in_field) {
    fprintf(stderr, "umrf: shader '%s' has no @vs_in field\n",
            std::string(interner.view(shader_type)).c_str());
    return false;
  }

  // find the pod type name from the @shader struct field's TypeAst
  SymId pod_sym = 0;
  for (const Decl &d : lm.mod.decls) {
    if (!has(d.flags, DeclFlags::Shader) || d.name != shader_type) continue;
    if (!d.init || d.init >= lm.ir.nodes.kind.size()) continue;
    if (lm.ir.nodes.kind[d.init] != NodeKind::StructType) continue;
    u32 fs = lm.ir.nodes.b[d.init], fc = lm.ir.nodes.c[d.init];
    for (u32 fi = 0; fi < fc; ++fi) {
      SymId fname = lm.ir.nodes.list[fs + fi * 2];
      if (fname != vs_in_field) continue;
      TypeId tid = lm.ir.nodes.list[fs + fi * 2 + 1];
      if (tid < lm.type_ast.kind.size() &&
          lm.type_ast.kind[tid] == TypeKind::Named)
        pod_sym = lm.type_ast.a[tid];
      break;
    }
    break;
  }
  if (!pod_sym) {
    fprintf(stderr, "umrf: can't resolve @vs_in pod type for '%s'\n",
            std::string(interner.view(shader_type)).c_str());
    return false;
  }

  // find the @shader_pod struct and iterate fields
  const Decl *pod_decl = nullptr;
  for (const Decl &d : lm.mod.decls) {
    if (has(d.flags, DeclFlags::ShaderPod) && d.name == pod_sym) {
      pod_decl = &d;
      break;
    }
  }
  if (!pod_decl || !pod_decl->init) {
    fprintf(stderr, "umrf: pod type '%s' not found\n",
            std::string(interner.view(pod_sym)).c_str());
    return false;
  }

  // collect fields with their types and IO annotations
  struct Attr {
    uint32_t location;
    uint32_t vk_format;
    uint32_t offset;
  };

  NodeId sn = pod_decl->init;
  if (sn >= lm.ir.nodes.kind.size() ||
      lm.ir.nodes.kind[sn] != NodeKind::StructType) {
    fprintf(stderr, "umrf: pod decl init is not a StructType\n");
    return false;
  }

  u32 fs = lm.ir.nodes.b[sn], fc = lm.ir.nodes.c[sn];
  uint32_t stride = 0;
  std::vector<Attr> attrs;

  for (u32 fi = 0; fi < fc; ++fi) {
    SymId fname = lm.ir.nodes.list[fs + fi * 2];
    TypeId tid = lm.ir.nodes.list[fs + fi * 2 + 1];
    std::string_view type_name;
    if (tid < lm.type_ast.kind.size() &&
        lm.type_ast.kind[tid] == TypeKind::Named)
      type_name = interner.view(lm.type_ast.a[tid]);

    if (type_name.empty()) continue;
    uint32_t field_size = sizeof_type(type_name);
    uint32_t field_offset = stride;
    stride += field_size;

    // check IO annotation for this field
    for (const IOFieldAnnot &ifa : lm.mod.io_field_annots) {
      if (ifa.struct_name != pod_sym || ifa.field_name != fname) continue;
      if (ifa.kind == IOAnnotKind::Location) {
        attrs.push_back({ifa.location_index,
                         vk_format_for(type_name),
                         field_offset});
      }
      break;
    }
  }

  uint32_t attr_count = static_cast<uint32_t>(attrs.size());
  uint32_t total_bytes = 16 + 8 + 4 + attr_count * 12;

  BinWriter w;
  // umrf_header_t
  w.u32(UMRF_MAGIC);
  w.u16(UMRF_VERSION);
  w.u16(UMRF_ENDIAN_LE);
  w.u32(total_bytes);
  w.u32(0); // reserved
  // umrf_vertex_binding_t
  w.u32(stride);
  w.u32(UMRF_INPUT_RATE_VERTEX);
  // attrs
  w.u32(attr_count);
  for (const auto &a : attrs) {
    w.u32(a.location);
    w.u32(a.vk_format);
    w.u32(a.offset);
  }

  std::string path = std::string(out_dir) + "/" +
                     std::string(interner.view(shader_type)) + ".umrf";
  return w.write_file(path.c_str());
}

} // namespace um::shader
