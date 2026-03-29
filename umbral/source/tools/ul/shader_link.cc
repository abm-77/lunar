#include "shader_link.h"
#include "compiler/shader/umshaders.h"
#include "spirv_emit.h"
#include "umrf_emit.h"
#include <common/bin_io.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

// clang-format off
// deserialize a BodyIR block from the reader into body.
//   immediately follows stage_kind in the binary;
//
// binary layout:
//   u32  body_root  (NodeId of the method's body Block within the node array)
//   u32  node_count
//   [each node — 22 bytes each]:
//     u16  kind     (NodeKind ordinal; must match ast.h enum order exactly)
//     u32  span_s
//     u32  span_e
//     u32  a
//     u32  b
//     u32  c
//   u32  list_count
//   [each list entry]: u32
//   u32  fors_count
//   [each ForPayload — 16 bytes]:
//     u32  init   (NodeId; 0 = no init)
//     u32  cond   (NodeId; 0 = unconditional)
//     u32  step   (NodeId; 0 = no step)
//     u32  body   (NodeId of the body Block)
//   u32  array_lits_count
//   [each ArrayLitPayload — 16 bytes]:
//     u32  explicit_count  (0xFFFFFFFF = inferred from values)
//     u32  elem_type       (TypeId)
//     u32  values_start    (index into list pool)
//     u32  values_count
//   u32  float_lits_count
//   [each float_lit]: f64 (IEEE 754 double)
//   u32  sym_count
//   [each sym_entry]:
//     u32  sym_id  (interner SymId)
//     str  name    (u32 len + UTF-8 bytes)
//
// node ids are body-local (0 = null sentinel; real nodes start at 1).
// FnLitPayload is NOT serialized — stage bodies must not contain nested fn literals.
// clang-format on
static int read_body(BinReader &r, StageBody &body) {
  body.body_root = r.u32();
  uint32_t node_count = r.u32();
  body.nodes.resize(node_count);
  for (uint32_t i = 0; i < node_count; i++) {
    body.nodes[i].kind = r.u16();
    body.nodes[i].span_s = r.u32();
    body.nodes[i].span_e = r.u32();
    body.nodes[i].a = r.u32();
    body.nodes[i].b = r.u32();
    body.nodes[i].c = r.u32();
  }
  uint32_t list_count = r.u32();
  body.list.resize(list_count);
  for (uint32_t i = 0; i < list_count; i++) body.list[i] = r.u32();
  uint32_t fors_count = r.u32();
  body.fors.resize(fors_count);
  for (uint32_t i = 0; i < fors_count; i++) {
    body.fors[i].init = r.u32();
    body.fors[i].cond = r.u32();
    body.fors[i].step = r.u32();
    body.fors[i].body = r.u32();
  }
  uint32_t array_lits_count = r.u32();
  body.array_lits.resize(array_lits_count);
  for (uint32_t i = 0; i < array_lits_count; i++) {
    body.array_lits[i].explicit_count = r.u32();
    body.array_lits[i].elem_type = r.u32();
    body.array_lits[i].values_start = r.u32();
    body.array_lits[i].values_count = r.u32();
  }
  uint32_t float_lits_count = r.u32();
  body.float_lits.resize(float_lits_count);
  for (uint32_t i = 0; i < float_lits_count; i++) body.float_lits[i] = r.f64();
  uint32_t sym_count = r.u32();
  for (uint32_t i = 0; i < sym_count; i++) {
    uint32_t sid = r.u32();
    body.sym_names[sid] = r.str();
  }
  return r.ok ? 0 : -1;
}

// parse a .umshaders blob into a Sidecar struct.
//  data, len: raw file bytes
//  out:       filled on success
//  returns 0 on success; -1 on format error
static int sidecar_read(const uint8_t *data, size_t len, Sidecar &out) {
  BinReader r{data, len};

  if (r.u32() != UMSHADERS_MAGIC) return -1;
  if (r.u16() != UMSHADERS_VERSION) return -1;
  out.module_name = r.str();

  uint32_t pod_count = r.u32();
  for (u32 p = 0; p < pod_count; ++p) {
    auto pod_name = r.str();
    uint32_t field_count = r.u32();
    std::vector<PodField> fields;
    fields.reserve(field_count);
    for (u32 f = 0; f < field_count; ++f) {
      fields.push_back({
          .name = r.str(),
          .io_kind = r.u8(),
          .location_index = r.u32(),
          .byte_offset = r.u32(),
          .type_name = r.str(),
      });
    }
    out.pods.push_back({.name = pod_name, .fields = std::move(fields)});
  }

  uint32_t shader_count = r.u32();
  for (u32 s = 0; s < shader_count; ++s) {
    auto shader_name = r.str();
    uint32_t annot_count = r.u32();
    std::vector<ShaderAnnot> annots;
    annots.reserve(annot_count);
    for (u32 a = 0; a < annot_count; ++a) {
      annots.push_back({
          .field_name = r.str(),
          .shader_field_kind = r.u8(),
          .pod_type_name = r.str(),
      });
    }
    out.shaders.push_back({.name = shader_name, .annots = std::move(annots)});
  }

  uint32_t stage_count = r.u32();
  for (u32 s = 0; s < stage_count; ++s) {
    out.stages.push_back({
        .shader_type_name = r.str(),
        .method_name = r.str(),
        .stage_kind = r.u8(),
    });
    read_body(r, out.stages[s].body);
  }

  return r.ok ? 0 : -1;
}

// load_sidecar — read and parse sidecar_path into sc.
// returns 0 on success; -1 on file or parse error.
static int load_sidecar(const char *sidecar_path, Sidecar &sc) {
  FILE *f = fopen(sidecar_path, "rb");
  if (!f) {
    fprintf(stderr, "shader_link: cannot open %s\n", sidecar_path);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long file_len = ftell(f);
  rewind(f);
  std::vector<uint8_t> buf((size_t)file_len);
  fread(buf.data(), 1, (size_t)file_len, f);
  fclose(f);
  return sidecar_read(buf.data(), buf.size(), sc);
}

int shader_link_dump_ir(const char *sidecar_path) {
  spirv_init_target();
  Sidecar sc;
  if (load_sidecar(sidecar_path, sc) != 0) return -1;
  for (const StageInfo &stg : sc.stages)
    if (spirv_dump_stage_ir(sc, stg) != 0) return -1;
  return 0;
}

int shader_link(const char *sidecar_path, const char *out_dir) {
  Sidecar sc;
  if (load_sidecar(sidecar_path, sc) != 0) return -1;

  for (size_t si = 0; si < sc.shaders.size(); si++) {
    const ShaderType &sh = sc.shaders[si];
    for (const StageInfo &stg : sc.stages) {
      if (stg.shader_type_name != sh.name) continue;
      const char *suffix = stg.stage_kind == 0 ? "_vs.spv" : "_fs.spv";
      std::string spv_path = std::string(out_dir) + "/" + sh.name + suffix;
      if (spirv_emit_stage(sc, stg, spv_path.c_str()) != 0) return -1;
    }
    std::string umrf_path = std::string(out_dir) + "/" + sh.name + ".umrf";
    if (umrf_emit(sc, si, umrf_path.c_str()) != 0) return -1;
  }
  return 0;
}
