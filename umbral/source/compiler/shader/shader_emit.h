#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <common/interner.h>
#include <common/types.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/module.h>
#include <compiler/loader.h>
#include <compiler/sema/sema.h>

#include "umshaders.h"

// gather all SymIds from BodyIR nodes that store SymId values
// in specific fields, keyed by NodeKind. used to build the sym_names table.
static inline void collect_sym_ids(const BodyIR &ir,
                                   std::unordered_set<SymId> &out) {
  uint32_t n = static_cast<uint32_t>(ir.nodes.kind.size());
  for (uint32_t i = 1; i < n; ++i) {
    NodeKind kind = ir.nodes.kind[i];
    uint32_t a = ir.nodes.a[i];
    uint32_t b = ir.nodes.b[i];
    uint32_t c = ir.nodes.c[i];
    switch (kind) {
    case NodeKind::Ident:
    case NodeKind::ConstStmt:
    case NodeKind::VarStmt:
    case NodeKind::ForRange:
    case NodeKind::ShaderRef:
    case NodeKind::FieldsOf: out.insert(a); break;
    case NodeKind::Field:
    case NodeKind::MetaField: out.insert(b); break;
    case NodeKind::StructInit:
      out.insert(a);
      for (uint32_t j = 0; j < c; ++j) out.insert(ir.nodes.list[b + j * 2]);
      break;
    case NodeKind::StructExpr:
    case NodeKind::AnonStructInit:
      for (uint32_t j = 0; j < c; ++j) out.insert(ir.nodes.list[b + j * 2]);
      break;
    case NodeKind::Path:
      for (uint32_t j = 0; j < c; ++j) out.insert(ir.nodes.list[b + j]);
      break;
    default: break;
    }
  }
}

// write the full BodyIR for module lm with body_root first,
// then nodes/list/fors/array_lits/float_lits, then a sym_names table.
// the sym_names table maps every SymId that appears in a SymId-bearing field
// to its interned string name, enabling the asset linker to resolve
// identifiers without access to the compiler's Interner.
static inline void serialize_body_ir(UmshadersWriter &w, const LoadedModule &lm,
                                     uint32_t body_root,
                                     const Interner &interner) {
  const BodyIR &ir = lm.ir;

  w.u32(body_root);

  uint32_t node_count = static_cast<uint32_t>(ir.nodes.kind.size());
  w.u32(node_count);
  for (uint32_t i = 0; i < node_count; ++i) {
    w.u16(static_cast<uint16_t>(ir.nodes.kind[i]));
    w.u32(ir.nodes.span_s[i]);
    w.u32(ir.nodes.span_e[i]);
    w.u32(ir.nodes.a[i]);
    // ShaderFrameRead.b is a compiler-internal TypeId; translate to SymId
    // (the interned name of T) so the asset linker can resolve it via sym_names.
    uint32_t b_out = ir.nodes.b[i];
    if (static_cast<NodeKind>(ir.nodes.kind[i]) == NodeKind::ShaderFrameRead) {
      TypeId tid = static_cast<TypeId>(b_out);
      b_out = (tid > 0 && tid < lm.type_ast.kind.size() &&
               lm.type_ast.kind[tid] == TypeKind::Named)
                  ? static_cast<uint32_t>(lm.type_ast.a[tid])
                  : 0;
    }
    w.u32(b_out);
    w.u32(ir.nodes.c[i]);
  }

  w.u32(static_cast<uint32_t>(ir.nodes.list.size()));
  for (auto v : ir.nodes.list) w.u32(v);

  w.u32(static_cast<uint32_t>(ir.fors.size()));
  for (const auto &f : ir.fors) {
    w.u32(f.init);
    w.u32(f.cond);
    w.u32(f.step);
    w.u32(f.body);
  }

  w.u32(static_cast<uint32_t>(ir.array_lits.size()));
  for (const auto &al : ir.array_lits) {
    w.u32(al.explicit_count);
    w.u32(al.elem_type);
    w.u32(al.values_start);
    w.u32(al.values_count);
  }

  w.u32(static_cast<uint32_t>(ir.float_lits.size()));
  for (double d : ir.float_lits) w.f64(d);

  // sym_names: collect all SymIds that appear in SymId-bearing node fields
  std::unordered_set<SymId> sym_id_set;
  collect_sym_ids(ir, sym_id_set);
  // ShaderFrameRead.b is written as a SymId (translated above); ensure it's in the table
  for (uint32_t i = 1; i < node_count; ++i) {
    if (static_cast<NodeKind>(ir.nodes.kind[i]) == NodeKind::ShaderFrameRead) {
      TypeId tid = static_cast<TypeId>(ir.nodes.b[i]);
      if (tid > 0 && tid < lm.type_ast.kind.size() &&
          lm.type_ast.kind[tid] == TypeKind::Named)
        sym_id_set.insert(lm.type_ast.a[tid]);
    }
  }
  w.u32(static_cast<uint32_t>(sym_id_set.size()));
  for (SymId sid : sym_id_set) {
    w.u32(sid);
    w.str(interner.view(sid));
  }
}

// resolve a TypeId to a shader type name string ("f32", "vec2", etc.).
// returns "" for reference types (self params) and unrecognized types.
static inline std::string shader_type_name_for(TypeId tid, const TypeAst &ta,
                                               const Interner &interner) {
  if (tid == 0) return "";
  TypeKind k = ta.kind[tid];
  if (k == TypeKind::Named) return std::string(interner.view(ta.a[tid]));
  return ""; // Ref (self), Slice, Array, etc. — not serialized as params
}

// write one .umshaders file per module that has shader decls.
// out_dir: directory to write into (e.g., "/tmp"); filename =
// "<module_name>.umshaders".
inline void emit_umshaders(const std::vector<LoadedModule> &modules,
                           const SemaResult &sema, const Interner &interner,
                           const std::string &out_dir) {
  for (u32 mi = 0; mi < static_cast<u32>(modules.size()); ++mi) {
    const LoadedModule &lm = modules[mi];
    const Module &mod = lm.mod;

    // skip modules with no shader content
    bool has_shaders = !mod.shader_stages.empty() ||
                       !mod.shader_fns.empty() ||
                       !mod.shader_field_annots.empty() ||
                       !mod.io_field_annots.empty();
    if (!has_shaders) continue;

    UmshadersWriter w;

    // header
    w.u32(UMSHADERS_MAGIC);
    w.u16(UMSHADERS_VERSION);
    w.str(lm.rel_path); // module name from the relative path

    // @shader_pod structs — find them by is_shader_pod flag on Decl
    std::vector<const Decl *> pods;
    for (const Decl &d : mod.decls)
      if (has(d.flags, DeclFlags::ShaderPod)) pods.push_back(&d);

    w.u32(static_cast<uint32_t>(pods.size()));
    for (const Decl *d : pods) {
      w.str(interner.view(d->name));

      // collect io_field_annots for this pod struct
      std::vector<const IOFieldAnnot *> fields;
      for (const IOFieldAnnot &a : mod.io_field_annots)
        if (a.struct_name == d->name) fields.push_back(&a);

      w.u32(static_cast<uint32_t>(fields.size()));
      uint32_t byte_offset =
          0; // stride computed from annot order; linker fills actual offsets
      for (const IOFieldAnnot *a : fields) {
        w.str(interner.view(a->field_name));
        w.u8(static_cast<uint8_t>(a->kind));
        w.u32(a->location_index);
        w.u32(byte_offset); // placeholder; asset linker computes from UMRF
        w.str(""); // field_type_name: left empty for linker to fill from UMRF
        byte_offset += 4; // minimum; linker corrects via UMRF reflection
      }
    }

    // @shader structs — collect ShaderFieldAnnot entries grouped by struct
    std::vector<SymId> shader_structs;
    for (const Decl &d : mod.decls)
      if (has(d.flags, DeclFlags::Shader)) shader_structs.push_back(d.name);

    w.u32(static_cast<uint32_t>(shader_structs.size()));
    for (SymId sname : shader_structs) {
      w.str(interner.view(sname));

      std::vector<const ShaderFieldAnnot *> annots;
      for (const ShaderFieldAnnot &a : mod.shader_field_annots)
        if (a.struct_name == sname) annots.push_back(&a);

      w.u32(static_cast<uint32_t>(annots.size()));
      for (const ShaderFieldAnnot *a : annots) {
        w.str(interner.view(a->field_name));
        w.u8(static_cast<uint8_t>(a->kind));
        w.str(""); // type_name: filled by linker from the struct definition
      }
    }

    // @shader_fn methods — shader-side helpers callable from @stage or other @shader_fn
    w.u32(static_cast<uint32_t>(mod.shader_fns.size()));
    for (const ShaderFnInfo &sf : mod.shader_fns) {
      w.str(interner.view(sf.shader_type));
      w.str(interner.view(sf.method_name));

      // serialize explicit params (skip self: Ref types map to globals)
      uint32_t body_root = 0;
      u32 params_start = 0, params_count = 0;
      SymbolId type_sym_id = sema.syms.lookup(mi, sf.shader_type);
      if (type_sym_id != kInvalidSymbol) {
        SymbolId method_sym_id = sema.methods.lookup(type_sym_id, sf.method_name);
        if (method_sym_id != kInvalidSymbol) {
          const Symbol &msym = sema.syms.symbols[method_sym_id];
          body_root = msym.body;
          params_start = msym.sig.params_start;
          params_count = msym.sig.params_count;
        }
      }

      // count non-self params (skip Ref types used for self/&mut self)
      std::vector<std::pair<u32, std::string>> explicit_params;
      for (u32 pi = 0; pi < params_count; ++pi) {
        const FuncParam &p = mod.params[params_start + pi];
        std::string tname = shader_type_name_for(p.type, lm.type_ast, interner);
        if (!tname.empty())
          explicit_params.push_back({static_cast<u32>(p.name), tname});
      }
      w.u32(static_cast<uint32_t>(explicit_params.size()));
      for (const auto &[sym_id, tname] : explicit_params) {
        w.u32(sym_id);
        w.str(tname);
      }

      serialize_body_ir(w, lm, body_root, interner);
    }

    // @stage methods — write header + serialized BodyIR
    w.u32(static_cast<uint32_t>(mod.shader_stages.size()));
    for (const ShaderStageInfo &si : mod.shader_stages) {
      w.str(interner.view(si.shader_type));
      w.str(interner.view(si.method_name));
      auto sv = interner.view(si.stage_name_sym);
      w.u8(sv == "fragment" ? 1u : 0u); // 0=vertex, 1=fragment

      // find the method Symbol to get the body root NodeId
      uint32_t body_root = 0;
      SymbolId type_sym_id = sema.syms.lookup(mi, si.shader_type);
      if (type_sym_id != kInvalidSymbol) {
        SymbolId method_sym_id =
            sema.methods.lookup(type_sym_id, si.method_name);
        if (method_sym_id != kInvalidSymbol)
          body_root = sema.syms.symbols[method_sym_id].body;
      }
      serialize_body_ir(w, lm, body_root, interner);
    }

    // write to out_dir/<module_leaf>.umshaders (e.g., "game/sprite" →
    // "sprite.umshaders")
    std::string leaf = lm.rel_path;
    auto slash = leaf.rfind('/');
    std::string stem =
        (slash == std::string::npos) ? leaf : leaf.substr(slash + 1);
    std::string outpath = out_dir + "/" + stem + ".umshaders";

    w.write_file(outpath.c_str());
  }
}
