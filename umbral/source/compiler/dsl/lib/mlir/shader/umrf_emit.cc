// collect vertex reflection data from Module/TypeAst for a @shader type.
// replaces the old emit_umrf which wrote a .umrf file; now returns UmshReflData.

#include <um/shader/umrf_emit.h>

#include <compiler/frontend/ast.h>
#include <compiler/frontend/module.h>
#include <runtime/gfx/gfx_refl.h>

#include <cstdio>
#include <cstring>
#include <optional>

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

std::optional<UmshReflData> collect_refl(const LoadedModule &lm,
                                          const Interner &interner,
                                          SymId shader_type) {
  // find the @vs_in field on the @shader struct
  SymId vs_in_field = 0;
  for (const ShaderFieldAnnot &sfa : lm.mod.shader_field_annots) {
    if (sfa.struct_name == shader_type && sfa.kind == ShaderFieldKind::VsIn) {
      vs_in_field = sfa.field_name;
      break;
    }
  }
  if (!vs_in_field) return std::nullopt; // no @vs_in; not an error

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
    return std::nullopt;
  }

  // find the @shader_pod struct and iterate its fields
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
    return std::nullopt;
  }

  NodeId sn = pod_decl->init;
  if (sn >= lm.ir.nodes.kind.size() ||
      lm.ir.nodes.kind[sn] != NodeKind::StructType) {
    fprintf(stderr, "umrf: pod decl init is not a StructType\n");
    return std::nullopt;
  }

  u32 fs = lm.ir.nodes.b[sn], fc = lm.ir.nodes.c[sn];
  UmshReflData refl;
  refl.stride     = 0;
  refl.input_rate = 0; // per-vertex

  for (u32 fi = 0; fi < fc; ++fi) {
    SymId fname = lm.ir.nodes.list[fs + fi * 2];
    TypeId tid  = lm.ir.nodes.list[fs + fi * 2 + 1];
    std::string_view type_name;
    if (tid < lm.type_ast.kind.size() &&
        lm.type_ast.kind[tid] == TypeKind::Named)
      type_name = interner.view(lm.type_ast.a[tid]);

    if (type_name.empty()) continue;
    uint32_t field_size   = sizeof_type(type_name);
    uint32_t field_offset = refl.stride;
    refl.stride += field_size;

    for (const IOFieldAnnot &ifa : lm.mod.io_field_annots) {
      if (ifa.struct_name != pod_sym || ifa.field_name != fname) continue;
      if (ifa.kind == IOAnnotKind::Location) {
        refl.attrs.push_back({ifa.location_index,
                              vk_format_for(type_name),
                              field_offset});
      }
      break;
    }
  }

  return refl;
}

} // namespace um::shader
