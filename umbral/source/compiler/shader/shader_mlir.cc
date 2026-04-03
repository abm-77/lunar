// lower shader stage/helper BodyIR to um.shader + arith + scf + memref MLIR
// ops. no SPIR-V is emitted here — that happens in shader_spirv_lower.cc.

#include "shader_compile.h"
#include "um_shader_dialect.h"

#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVAttributes.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVDialect.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVTypes.h>
#include <mlir/Dialect/Vector/IR/VectorOps.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/MLIRContext.h>

#include <compiler/frontend/ast.h>
#include <compiler/frontend/module.h>
#include <compiler/sema/symbol.h>

#include <cstdio>
#include <span>
#include <string>
#include <unordered_map>

namespace um::shader {

static mlir::Type type_for_name(mlir::Builder &b, std::string_view name) {
  if (name == "f32") return b.getF32Type();
  if (name == "f64") return b.getF64Type();
  if (name == "i8" || name == "u8") return b.getIntegerType(8);
  if (name == "i16" || name == "u16") return b.getIntegerType(16);
  if (name == "i32" || name == "u32") return b.getI32Type();
  if (name == "i64" || name == "u64") return b.getI64Type();
  if (name == "bool") return b.getI1Type();
  auto f32 = b.getF32Type();
  if (name == "vec2") return mlir::VectorType::get({2}, f32);
  if (name == "vec3") return mlir::VectorType::get({3}, f32);
  if (name == "vec4") return mlir::VectorType::get({4}, f32);
  if (name == "mat4")
    return mlir::spirv::MatrixType::get(mlir::VectorType::get({4}, f32), 4);
  return {};
}

// byte size of an MLIR type for frame_read offset computation
static u32 byte_size_of(mlir::Type ty) {
  if (ty.isF32() || ty.isInteger(32)) return 4;
  if (ty.isF64() || ty.isInteger(64)) return 8;
  if (ty.isInteger(8)) return 1;
  if (ty.isInteger(16)) return 2;
  if (auto vt = mlir::dyn_cast<mlir::VectorType>(ty))
    return vt.getNumElements() * byte_size_of(vt.getElementType());
  return 0;
}

// return MLIR type for a TypeId from type_ast (Named or QualNamed).
// returns {} for anything that can't be resolved to a simple named type.
static mlir::Type type_from_typeid(mlir::Builder &b, const TypeAst &ta,
                                   const Interner &interner, TypeId tid) {
  if (!tid || tid >= ta.kind.size()) return {};
  if (ta.kind[tid] == TypeKind::Named)
    return type_for_name(b, interner.view(ta.a[tid]));
  // QualNamed: a = type_name SymId (the unqualified name)
  if (ta.kind[tid] == TypeKind::QualNamed)
    return type_for_name(b, interner.view(ta.a[tid]));
  return {};
}

struct ShaderEmitCtx {
  mlir::MLIRContext &ctx;
  mlir::Location loc;
  const BodyIR &ir;
  const TypeAst &type_ast;
  const Module &mod;
  const SemaResult &sema;
  const Interner &interner;
  u32 mod_idx;
  SymId self_sym;    // SymId of the "self" parameter; 0 if none
  SymId shader_type; // SymId of the shader struct type (e.g. "SpriteShader")
  std::span<const LoadedModule> all_modules; // all loaded modules for cross-module type resolution

  // SSA values for immutable bindings (ConstStmt)
  std::unordered_map<SymId, mlir::Value> sym_vals;
  // memref<T> allocas for mutable bindings (VarStmt)
  std::unordered_map<SymId, mlir::Value> var_ptrs;
  // @shader_fn helpers declared for this shader type
  std::unordered_map<SymId, mlir::func::FuncOp> helper_fns;
  // alloca insertion point (entry block of the function)
  mlir::Block *entry_block = nullptr;
  // exploded struct reads: synthetic handle → field name → field value.
  // @frame_read<StructType>(offset) is decomposed into per-field reads;
  // field access on the handle redirects to the individual values.
  std::unordered_map<uintptr_t, std::unordered_map<std::string, mlir::Value>>
      struct_reads;
  int struct_read_id = 0;

  std::string_view sym_name(SymId id) const { return interner.view(id); }

  // find the MLIR type for a field within a @shader or @shader_pod struct.
  // shader_or_pod_sym: SymId of "SpriteShader" or "SpriteVertex"
  // field_sym:         SymId of the field (e.g. "pos")
  mlir::Type struct_field_type(mlir::Builder &b, SymId struct_sym,
                               SymId field_sym) const {
    for (const Decl &d : mod.decls) {
      if (d.name != struct_sym) continue;
      if (!d.init || d.init >= ir.nodes.kind.size()) continue;
      if (ir.nodes.kind[d.init] != NodeKind::StructType) continue;
      u32 fs = ir.nodes.b[d.init], fc = ir.nodes.c[d.init];
      for (u32 fi = 0; fi < fc; ++fi) {
        SymId fname = ir.nodes.list[fs + fi * 2];
        if (fname != field_sym) continue;
        TypeId tid = ir.nodes.list[fs + fi * 2 + 1];
        return type_from_typeid(b, type_ast, interner, tid);
      }
    }
    return {};
  }

  // return MLIR type for self.field_name.sub_field_name IO access.
  // traces @shader struct → pod type → field type.
  mlir::Type io_field_type(mlir::Builder &b, std::string_view field_str,
                           std::string_view sub_str) const {
    // find the pod type SymId for field_str from the @shader struct
    SymId pod_sym = 0;
    for (const Decl &d : mod.decls) {
      if (!has(d.flags, DeclFlags::Shader) || d.name != shader_type) continue;
      if (!d.init || d.init >= ir.nodes.kind.size()) continue;
      if (ir.nodes.kind[d.init] != NodeKind::StructType) continue;
      u32 fs = ir.nodes.b[d.init], fc = ir.nodes.c[d.init];
      for (u32 fi = 0; fi < fc; ++fi) {
        SymId fname = ir.nodes.list[fs + fi * 2];
        if (sym_name(fname) != field_str) continue;
        TypeId tid = ir.nodes.list[fs + fi * 2 + 1];
        if (tid < type_ast.kind.size() && type_ast.kind[tid] == TypeKind::Named)
          pod_sym = type_ast.a[tid]; // SymId of the pod type name
        break;
      }
      break;
    }
    if (!pod_sym) return {};

    // find sub_str in the @shader_pod struct
    for (const Decl &d : mod.decls) {
      if (!has(d.flags, DeclFlags::ShaderPod) || d.name != pod_sym) continue;
      if (!d.init || d.init >= ir.nodes.kind.size()) continue;
      if (ir.nodes.kind[d.init] != NodeKind::StructType) continue;
      u32 fs = ir.nodes.b[d.init], fc = ir.nodes.c[d.init];
      for (u32 fi = 0; fi < fc; ++fi) {
        SymId fname = ir.nodes.list[fs + fi * 2];
        if (sym_name(fname) != sub_str) continue;
        TypeId tid = ir.nodes.list[fs + fi * 2 + 1];
        return type_from_typeid(b, type_ast, interner, tid);
      }
      break;
    }
    return {};
  }

  // detect IO access: Field(Field(Ident(self_sym), annot_sym), sub_sym).
  // returns true and fills field_str / sub_field_str if matched.
  bool detect_io_access(NodeId nid, std::string &field_str,
                        std::string &sub_field_str) const {
    if (!nid || nid >= ir.nodes.kind.size()) return false;
    if (ir.nodes.kind[nid] != NodeKind::Field) return false;
    NodeId base = ir.nodes.a[nid];
    SymId sub_sym = ir.nodes.b[nid];

    if (!base || base >= ir.nodes.kind.size()) return false;
    if (ir.nodes.kind[base] != NodeKind::Field) return false;
    NodeId base_base = ir.nodes.a[base];
    SymId annot_sym = ir.nodes.b[base];

    if (!base_base || base_base >= ir.nodes.kind.size()) return false;
    if (ir.nodes.kind[base_base] != NodeKind::Ident) return false;
    if (ir.nodes.a[base_base] != self_sym) return false;

    field_str = std::string(sym_name(annot_sym));
    sub_field_str = std::string(sym_name(sub_sym));
    return true;
  }

  // insert alloca in the function's entry block (before other ops).
  // keeps all allocas at the top for SPIR-V OpVariable Function compliance.
  mlir::Value insert_alloca(mlir::OpBuilder &b, mlir::Type elem_ty,
                            std::string_view name) {
    mlir::OpBuilder alloca_b(entry_block, entry_block->begin());
    // MemRefToSPIRV requires Function storage class on alloca memrefs
    auto fn_sc = mlir::spirv::StorageClassAttr::get(
        &ctx, mlir::spirv::StorageClass::Function);
    auto memref_ty =
        mlir::MemRefType::get({1}, elem_ty, mlir::AffineMap(), fn_sc);
    auto op = mlir::memref::AllocaOp::create(alloca_b, loc, memref_ty);
    op->setAttr("name", b.getStringAttr(name));
    return op.getResult();
  }

  // constant index 0 for rank-1 memref access
  mlir::ValueRange idx0(mlir::OpBuilder &b) {
    thread_local mlir::Value cached;
    if (!cached || cached.getContext() != &ctx)
      cached = mlir::arith::ConstantIndexOp::create(b, loc, 0).getResult();
    return cached;
  }

  // load from a rank-1 memref<1xT> var slot
  mlir::Value var_load(mlir::OpBuilder &b, mlir::Value ptr) {
    auto zero = mlir::arith::ConstantIndexOp::create(b, loc, 0).getResult();
    return mlir::memref::LoadOp::create(b, loc, ptr, mlir::ValueRange{zero})
        .getResult();
  }

  // store to a rank-1 memref<1xT> var slot
  void var_store(mlir::OpBuilder &b, mlir::Value val, mlir::Value ptr) {
    auto zero = mlir::arith::ConstantIndexOp::create(b, loc, 0).getResult();
    mlir::memref::StoreOp::create(b, loc, val, ptr, mlir::ValueRange{zero});
  }

  // resolve the type name from a TypeId, handling both Named and QualNamed.
  // returns the type name string (last segment for QualNamed).
  std::string_view resolve_type_name(TypeId tid) const {
    if (!tid || tid >= type_ast.kind.size()) return {};
    if (type_ast.kind[tid] == TypeKind::Named)
      return interner.view(type_ast.a[tid]);
    if (type_ast.kind[tid] == TypeKind::QualNamed) {
      // a = type_name SymId (the last segment)
      return interner.view(type_ast.a[tid]);
    }
    return {};
  }

  // look up a struct symbol by name across all module namespaces.
  SymbolId find_struct_symbol(std::string_view name) const {
    for (u32 mi = 0; mi < sema.syms.module_namespaces.size(); ++mi) {
      for (auto &[sym_id, sid] : sema.syms.module_namespaces[mi]) {
        const Symbol &s = sema.syms.symbols[sid];
        if (s.kind == SymbolKind::Type && interner.view(s.name) == name)
          return sid;
      }
    }
    return kInvalidSymbol;
  }

  // emit a @frame_read for a struct type: decompose into per-field FrameReadOps
  // and return a synthetic handle. field access on the handle is intercepted.
  mlir::Value emit_struct_frame_read(mlir::OpBuilder &b, mlir::Value offset,
                                     TypeId tid) {
    auto name = resolve_type_name(tid);
    if (name.empty()) return {};
    SymbolId sid = find_struct_symbol(name);
    if (sid == kInvalidSymbol) return {};
    const Symbol &sym = sema.syms.symbols[sid];
    if (sym.kind != SymbolKind::Type || !sym.type_node) return {};

    // get the defining module's IR to access struct fields
    u32 def_mod = sym.module_idx;
    if (def_mod >= all_modules.size()) return {};
    const BodyIR &def_ir = all_modules[def_mod].ir;
    const TypeAst &def_ta = all_modules[def_mod].type_ast;

    NodeId tn = sym.type_node;
    if (tn >= def_ir.nodes.kind.size()) return {};
    if (def_ir.nodes.kind[tn] != NodeKind::StructType) return {};

    u32 fields_start = def_ir.nodes.b[tn];
    u32 fields_count = def_ir.nodes.c[tn];
    if (fields_count == 0) return {};

    // emit per-field reads
    std::unordered_map<std::string, mlir::Value> field_map;
    u32 byte_off = 0;
    for (u32 i = 0; i < fields_count; ++i) {
      SymId fname = def_ir.nodes.list[fields_start + i * 2];
      TypeId ftype = def_ir.nodes.list[fields_start + i * 2 + 1];

      // resolve field type to MLIR via its name in the defining module
      std::string_view ftype_name;
      if (ftype < def_ta.kind.size() && def_ta.kind[ftype] == TypeKind::Named)
        ftype_name = interner.view(def_ta.a[ftype]);
      if (ftype_name.empty()) return {};

      mlir::Type fty = type_for_name(b, ftype_name);
      if (!fty) {
        // recursively handle nested struct (e.g. mat4 contains vec4 fields)
        // for now, only support flat structs with scalar/vector fields
        return {};
      }

      u32 fsz = byte_size_of(fty);
      if (!fsz) return {};

      // compute field offset: base + byte_off
      mlir::Value field_off = offset;
      if (byte_off > 0) {
        auto off_const = mlir::arith::ConstantOp::create(b, 
            loc, b.getI32IntegerAttr(byte_off));
        field_off =
            mlir::arith::AddIOp::create(b, loc, offset, off_const.getResult())
                .getResult();
      }
      auto fval = FrameReadOp::create(b, loc, fty, field_off,
                                        mlir::TypeAttr::get(fty))
                      .getResult();
      field_map[std::string(interner.view(fname))] = fval;
      byte_off += fsz;
    }

    // create a synthetic handle and store the field map
    auto handle = mlir::arith::ConstantOp::create(b, 
                       loc, b.getI32IntegerAttr(-(++struct_read_id)))
                      .getResult();
    struct_reads[reinterpret_cast<uintptr_t>(handle.getAsOpaquePointer())] =
        std::move(field_map);
    return handle;
  }

  // emit node nid; returns the SSA value (null for void/statement nodes).
  mlir::Value emit(mlir::OpBuilder &b, NodeId nid);
};

mlir::Value ShaderEmitCtx::emit(mlir::OpBuilder &b, NodeId nid) {
  if (!nid || nid >= ir.nodes.kind.size()) return {};
  auto nk = ir.nodes.kind[nid];
  u32 na = ir.nodes.a[nid], nb = ir.nodes.b[nid], nc = ir.nodes.c[nid];

  switch (nk) {

    // ---- literals ----

  case NodeKind::IntLit:
    return mlir::arith::ConstantOp::create(b,
            loc, b.getIntegerAttr(b.getI32Type(), static_cast<int64_t>(ir.int_lits[na])))
        .getResult();

  case NodeKind::FloatLit: {
    double dval = ir.float_lits[na];
    return mlir::arith::ConstantOp::create(b,
            loc, b.getFloatAttr(b.getF32Type(), static_cast<float>(dval)))
        .getResult();
  }

  case NodeKind::BoolLit:
    return mlir::arith::ConstantOp::create(b, loc, b.getBoolAttr(na != 0))
        .getResult();

    // ---- identifiers ----

  case NodeKind::Ident: {
    SymId sym = na;
    auto vp = var_ptrs.find(sym);
    if (vp != var_ptrs.end()) {
      return var_load(b, vp->second);
    }
    auto sv = sym_vals.find(sym);
    if (sv != sym_vals.end()) return sv->second;
    return {};
  }

    // ---- field access ----

  case NodeKind::Field: {
    std::string field_str, sub_str;
    if (detect_io_access(nid, field_str, sub_str)) {
      mlir::Type ty = io_field_type(b, field_str, sub_str);
      if (!ty) {
        fprintf(stderr, "shader_mlir: unknown IO type for %s.%s\n",
                field_str.c_str(), sub_str.c_str());
        return {};
      }
      return LoadInputOp::create(b, loc, ty, b.getStringAttr(field_str),
                               b.getStringAttr(sub_str))
          .getResult();
    }
    // general struct field access: emit base as value, extract by index
    mlir::Value base_val = emit(b, na);
    if (!base_val) return {};
    // exploded struct reads (from @frame_read<UserStruct>)
    auto sr_it = struct_reads.find(
        reinterpret_cast<uintptr_t>(base_val.getAsOpaquePointer()));
    if (sr_it != struct_reads.end()) {
      auto field_name = sym_name(nb);
      auto fi = sr_it->second.find(std::string(field_name));
      if (fi != sr_it->second.end()) return fi->second;
    }
    // mat4 column access: mvp.col0..col3 → spirv.CompositeExtract
    if (auto mat_ty =
            mlir::dyn_cast<mlir::spirv::MatrixType>(base_val.getType())) {
      auto field_name = sym_name(nb);
      int idx = -1;
      if (field_name == "col0") idx = 0;
      else if (field_name == "col1") idx = 1;
      else if (field_name == "col2") idx = 2;
      else if (field_name == "col3") idx = 3;
      if (idx >= 0) {
        int32_t indices[] = {idx};
        auto op = mlir::spirv::CompositeExtractOp::create(
            b, loc, base_val, llvm::ArrayRef<int32_t>(indices));
        return op.getResult();
      }
    }
    auto vec_ty = mlir::dyn_cast<mlir::VectorType>(base_val.getType());
    if (vec_ty) {
      // swizzle: .x/.y/.z/.w on vectors
      auto field_name = sym_name(nb);
      int idx = -1;
      if (field_name == "x" || field_name == "r") idx = 0;
      else if (field_name == "y" || field_name == "g") idx = 1;
      else if (field_name == "z" || field_name == "b") idx = 2;
      else if (field_name == "w" || field_name == "a") idx = 3;
      if (idx >= 0)
        return mlir::vector::ExtractOp::create(b, loc, base_val,
                                             static_cast<int64_t>(idx))
            .getResult();
    }
    // draw_packet field access → DrawPacketFieldOp (result is i32)
    if (mlir::isa<DrawPacketType>(base_val.getType())) {
      auto field_name = sym_name(nb);
      return DrawPacketFieldOp::create(b, loc, b.getI32Type(), base_val,
                                     b.getStringAttr(field_name))
          .getResult();
    }
    return {};
  }

    // ---- index ----

  case NodeKind::Index: {
    mlir::Value base = emit(b, na);
    mlir::Value idx = emit(b, nb);
    if (!base || !idx) return {};
    auto vec_ty = mlir::dyn_cast<mlir::VectorType>(base.getType());
    if (vec_ty)
      return mlir::vector::ExtractOp::create(b, loc, base, mlir::OpFoldResult(idx))
          .getResult();
    return {};
  }

    // ---- unary ----

  case NodeKind::Unary: {
    auto op = static_cast<TokenKind>(na);
    mlir::Value child = emit(b, nb);
    if (!child) return {};
    auto ty = child.getType();
    bool is_float = mlir::isa<mlir::FloatType>(ty) ||
                    (mlir::isa<mlir::VectorType>(ty) &&
                     mlir::isa<mlir::FloatType>(
                         mlir::cast<mlir::VectorType>(ty).getElementType()));
    if (op == TokenKind::Minus) {
      if (is_float)
        return mlir::arith::NegFOp::create(b, loc, child).getResult();
      // integer negation: 0 - child
      auto zero = mlir::arith::ConstantOp::create(b, 
                       loc, b.getIntegerAttr(child.getType(), 0))
                      .getResult();
      return mlir::arith::SubIOp::create(b, loc, zero, child).getResult();
    }
    if (op == TokenKind::Bang)
      return mlir::arith::XOrIOp::create(b,
              loc, child,
              mlir::arith::ConstantOp::create(b, loc, b.getBoolAttr(true))
                  .getResult())
          .getResult();
    return {};
  }

    // ---- binary ----

  case NodeKind::Binary: {
    auto op = static_cast<TokenKind>(na);
    mlir::Value lhs = emit(b, nb), rhs = emit(b, nc);
    if (!lhs || !rhs) return {};

    auto lhs_ty = lhs.getType();
    auto rhs_ty = rhs.getType();
    auto l_vec = mlir::dyn_cast<mlir::VectorType>(lhs_ty);
    auto r_vec = mlir::dyn_cast<mlir::VectorType>(rhs_ty);
    bool l_scalar = mlir::isa<mlir::FloatType>(lhs_ty) ||
                    mlir::isa<mlir::IntegerType>(lhs_ty);
    bool r_scalar = mlir::isa<mlir::FloatType>(rhs_ty) ||
                    mlir::isa<mlir::IntegerType>(rhs_ty);
    bool l_mat = mlir::isa<mlir::spirv::MatrixType>(lhs_ty);

    // vec * scalar or scalar * vec: per-element multiply via extract/insert
    if ((l_vec && r_scalar) || (l_scalar && r_vec)) {
      mlir::Value vec_val = l_vec ? lhs : rhs;
      mlir::Value scl_val = l_vec ? rhs : lhs;
      auto vec_ty = mlir::cast<mlir::VectorType>(vec_val.getType());
      int64_t n = vec_ty.getNumElements();
      mlir::Value result = vec_val;
      for (int64_t k = 0; k < n; ++k) {
        auto elem = mlir::vector::ExtractOp::create(b, loc, vec_val, k)
                        .getResult();
        mlir::Value r;
        switch (op) {
        case TokenKind::Star:
          r = mlir::arith::MulFOp::create(b, loc, elem, scl_val).getResult();
          break;
        case TokenKind::Plus:
          r = mlir::arith::AddFOp::create(b, loc, elem, scl_val).getResult();
          break;
        case TokenKind::Minus:
          r = l_vec
              ? mlir::arith::SubFOp::create(b, loc, elem, scl_val).getResult()
              : mlir::arith::SubFOp::create(b, loc, scl_val, elem).getResult();
          break;
        case TokenKind::Slash:
          r = mlir::arith::DivFOp::create(b, loc, elem, scl_val).getResult();
          break;
        default: return {};
        }
        result = mlir::vector::InsertOp::create(b, loc, r, result, k)
                     .getResult();
      }
      return result;
    }

    // mat4 * vec4 → spirv.MatrixTimesVector
    if (l_mat && r_vec && r_vec.getNumElements() == 4 &&
        op == TokenKind::Star) {
      return mlir::spirv::MatrixTimesVectorOp::create(b, loc, r_vec, lhs, rhs)
          .getResult();
    }

    bool is_float =
        mlir::isa<mlir::FloatType>(lhs_ty) ||
        (l_vec && mlir::isa<mlir::FloatType>(l_vec.getElementType()));
    switch (op) {
    case TokenKind::Plus:
      return (is_float
                  ? (mlir::Value)mlir::arith::AddFOp::create(b, loc, lhs, rhs)
                  : (mlir::Value)mlir::arith::AddIOp::create(b, loc, lhs, rhs));
    case TokenKind::Minus:
      return (is_float
                  ? (mlir::Value)mlir::arith::SubFOp::create(b, loc, lhs, rhs)
                  : (mlir::Value)mlir::arith::SubIOp::create(b, loc, lhs, rhs));
    case TokenKind::Star:
      return (is_float
                  ? (mlir::Value)mlir::arith::MulFOp::create(b, loc, lhs, rhs)
                  : (mlir::Value)mlir::arith::MulIOp::create(b, loc, lhs, rhs));
    case TokenKind::Slash:
      return (is_float
                  ? (mlir::Value)mlir::arith::DivFOp::create(b, loc, lhs, rhs)
                  : (mlir::Value)mlir::arith::DivSIOp::create(b, loc, lhs, rhs));
    case TokenKind::EqualEqual:
      return (is_float ? (mlir::Value)mlir::arith::CmpFOp::create(b, 
                             loc, mlir::arith::CmpFPredicate::OEQ, lhs, rhs)
                       : (mlir::Value)mlir::arith::CmpIOp::create(b, 
                             loc, mlir::arith::CmpIPredicate::eq, lhs, rhs));
    case TokenKind::BangEqual:
      return (is_float ? (mlir::Value)mlir::arith::CmpFOp::create(b, 
                             loc, mlir::arith::CmpFPredicate::ONE, lhs, rhs)
                       : (mlir::Value)mlir::arith::CmpIOp::create(b, 
                             loc, mlir::arith::CmpIPredicate::ne, lhs, rhs));
    case TokenKind::Less:
      return (is_float ? (mlir::Value)mlir::arith::CmpFOp::create(b, 
                             loc, mlir::arith::CmpFPredicate::OLT, lhs, rhs)
                       : (mlir::Value)mlir::arith::CmpIOp::create(b, 
                             loc, mlir::arith::CmpIPredicate::slt, lhs, rhs));
    case TokenKind::LessEqual:
      return (is_float ? (mlir::Value)mlir::arith::CmpFOp::create(b, 
                             loc, mlir::arith::CmpFPredicate::OLE, lhs, rhs)
                       : (mlir::Value)mlir::arith::CmpIOp::create(b, 
                             loc, mlir::arith::CmpIPredicate::sle, lhs, rhs));
    case TokenKind::Greater:
      return (is_float ? (mlir::Value)mlir::arith::CmpFOp::create(b, 
                             loc, mlir::arith::CmpFPredicate::OGT, lhs, rhs)
                       : (mlir::Value)mlir::arith::CmpIOp::create(b, 
                             loc, mlir::arith::CmpIPredicate::sgt, lhs, rhs));
    case TokenKind::GreaterEqual:
      return (is_float ? (mlir::Value)mlir::arith::CmpFOp::create(b, 
                             loc, mlir::arith::CmpFPredicate::OGE, lhs, rhs)
                       : (mlir::Value)mlir::arith::CmpIOp::create(b, 
                             loc, mlir::arith::CmpIPredicate::sge, lhs, rhs));
    case TokenKind::AmpAmp:
      return mlir::arith::AndIOp::create(b, loc, lhs, rhs).getResult();
    case TokenKind::PipePipe:
      return mlir::arith::OrIOp::create(b, loc, lhs, rhs).getResult();
    default: return {};
    }
  }

    // ---- call ----

  case NodeKind::Call: {
    NodeId callee_nid = na;
    u32 args_start = nb, args_count = nc;
    if (!callee_nid || callee_nid >= ir.nodes.kind.size()) return {};

    // vec2/vec3/vec4 construction → vector insert
    // callee may be Ident("vec4") or Path("types::vec4"); extract the last
    // segment.
    std::string_view cname;
    if (ir.nodes.kind[callee_nid] == NodeKind::Ident) {
      cname = sym_name(ir.nodes.a[callee_nid]);
    } else if (ir.nodes.kind[callee_nid] == NodeKind::Path) {
      u32 segs_start = ir.nodes.b[callee_nid],
          segs_count = ir.nodes.c[callee_nid];
      if (segs_count > 0)
        cname = sym_name(ir.nodes.list[segs_start + segs_count - 1]);
    }
    if (!cname.empty()) {
      if (cname == "vec2" || cname == "vec3" || cname == "vec4") {
        u32 n = (cname == "vec2") ? 2 : (cname == "vec3") ? 3 : 4;
        auto f32 = b.getF32Type();
        auto vec_ty = mlir::VectorType::get({(int64_t)n}, f32);
        llvm::SmallVector<float> zeros(n, 0.0f);
        mlir::Value vec = mlir::arith::ConstantOp::create(b, 
                               loc, mlir::DenseElementsAttr::get(
                                        vec_ty, llvm::ArrayRef<float>(zeros)))
                              .getResult();
        for (u32 k = 0; k < n && k < args_count; ++k) {
          mlir::Value elem = emit(b, ir.nodes.list[args_start + k]);
          if (!elem) continue;
          // coerce f64 → f32
          if (mlir::isa<mlir::Float64Type>(elem.getType()))
            elem = mlir::arith::TruncFOp::create(b, loc, f32, elem).getResult();
          vec = mlir::vector::InsertOp::create(b, loc, elem, vec,
                                                 static_cast<int64_t>(k))
                    .getResult();
        }
        return vec;
      }
    }

    // self.shader_fn_name(args) — call a declared @shader_fn helper
    if (ir.nodes.kind[callee_nid] == NodeKind::Field) {
      NodeId base_nid = ir.nodes.a[callee_nid];
      SymId method_sym = ir.nodes.b[callee_nid];
      if (base_nid && base_nid < ir.nodes.kind.size() &&
          ir.nodes.kind[base_nid] == NodeKind::Ident &&
          ir.nodes.a[base_nid] == self_sym) {
        auto it = helper_fns.find(method_sym);
        if (it != helper_fns.end()) {
          llvm::SmallVector<mlir::Value> arg_vals;
          for (u32 k = 0; k < args_count; ++k) {
            mlir::Value av = emit(b, ir.nodes.list[args_start + k]);
            if (av) arg_vals.push_back(av);
          }
          auto call = mlir::func::CallOp::create(b, loc, it->second, arg_vals);
          return call.getNumResults() > 0 ? call.getResult(0) : mlir::Value{};
        }
      }
    }
    return {};
  }

    // ---- statements ----

  case NodeKind::Block: {
    u32 stmt_start = nb, stmt_count = nc;
    for (u32 k = 0; k < stmt_count; ++k) emit(b, ir.nodes.list[stmt_start + k]);
    return {};
  }

  case NodeKind::ConstStmt: {
    SymId sym = na;
    if (nc) {
      mlir::Value v = emit(b, nc);
      if (v) sym_vals[sym] = v;
    }
    return {};
  }

  case NodeKind::VarStmt: {
    SymId sym = na;
    if (!nc) return {};
    mlir::Value init_val = emit(b, nc);
    if (!init_val) return {};
    mlir::Value ptr = insert_alloca(b, init_val.getType(), sym_name(sym));
    var_store(b, init_val, ptr);
    var_ptrs[sym] = ptr;
    return {};
  }

  case NodeKind::AssignStmt: {
    auto op = static_cast<TokenKind>(nc);
    // check for IO store: self.vout.field = rhs
    std::string field_str, sub_str;
    if (detect_io_access(na, field_str, sub_str)) {
      mlir::Value rhs = emit(b, nb);
      if (!rhs) return {};
      StoreOutputOp::create(b, loc, b.getStringAttr(field_str),
                              b.getStringAttr(sub_str), rhs);
      return {};
    }
    // mutable local variable assignment
    mlir::Value rhs = emit(b, nb);
    if (!rhs) return {};
    // find the target alloca
    auto get_ptr = [&](NodeId lhs_nid) -> mlir::Value {
      if (!lhs_nid || lhs_nid >= ir.nodes.kind.size()) return {};
      if (ir.nodes.kind[lhs_nid] == NodeKind::Ident) {
        SymId sym = ir.nodes.a[lhs_nid];
        auto it = var_ptrs.find(sym);
        return it != var_ptrs.end() ? it->second : mlir::Value{};
      }
      return {};
    };
    mlir::Value ptr = get_ptr(na);
    if (!ptr) return {};
    if (op == TokenKind::Equal) {
      var_store(b, rhs, ptr);
      return {};
    }
    // compound: load cur, compute, store
    mlir::Value cur = var_load(b, ptr);
    bool is_float = mlir::isa<mlir::FloatType>(rhs.getType());
    mlir::Value result;
    switch (op) {
    case TokenKind::PlusEqual:
      result = is_float
                   ? (mlir::Value)mlir::arith::AddFOp::create(b, loc, cur, rhs)
                   : (mlir::Value)mlir::arith::AddIOp::create(b, loc, cur, rhs);
      break;
    case TokenKind::MinusEqual:
      result = is_float
                   ? (mlir::Value)mlir::arith::SubFOp::create(b, loc, cur, rhs)
                   : (mlir::Value)mlir::arith::SubIOp::create(b, loc, cur, rhs);
      break;
    case TokenKind::StarEqual:
      result = is_float
                   ? (mlir::Value)mlir::arith::MulFOp::create(b, loc, cur, rhs)
                   : (mlir::Value)mlir::arith::MulIOp::create(b, loc, cur, rhs);
      break;
    case TokenKind::SlashEqual:
      result = is_float
                   ? (mlir::Value)mlir::arith::DivFOp::create(b, loc, cur, rhs)
                   : (mlir::Value)mlir::arith::DivSIOp::create(b, loc, cur, rhs);
      break;
    default: break;
    }
    if (result) var_store(b, result, ptr);
    return {};
  }

  case NodeKind::ExprStmt: emit(b, na); return {};

  case NodeKind::ReturnStmt:
    if (na) {
      mlir::Value v = emit(b, na);
      if (v) mlir::func::ReturnOp::create(b, loc, v);
      else mlir::func::ReturnOp::create(b, loc);
    } else {
      mlir::func::ReturnOp::create(b, loc);
    }
    return {};

    // ---- control flow ----

  case NodeKind::IfStmt: {
    mlir::Value cond = emit(b, na);
    if (!cond) return {};
    bool has_else = nc != 0;
    NodeId then_nid = nb, else_nid = nc;
    // scf.if with no results (purely side-effecting)
    mlir::scf::IfOp::create(b, 
        loc, cond,
        [&](mlir::OpBuilder &nb2, mlir::Location nl) {
          emit(nb2, then_nid);
          mlir::scf::YieldOp::create(nb2, nl);
        },
        has_else
            ? mlir::function_ref<void(mlir::OpBuilder &, mlir::Location)>(
                  [&](mlir::OpBuilder &nb2, mlir::Location nl) {
                    emit(nb2, else_nid);
                    mlir::scf::YieldOp::create(nb2, nl);
                  })
            : mlir::function_ref<void(mlir::OpBuilder &, mlir::Location)>{});
    return {};
  }

  case NodeKind::ForStmt: {
    // ForPayload: init, cond, step, body; all stored by index in ir.fors.
    const ForPayload &fp = ir.fors[na];
    // emit init before the loop
    if (fp.init) emit(b, fp.init);
    // scf.while: before region checks cond; do region runs body+step
    NodeId cond_nid = fp.cond, body_nid = fp.body, step_nid = fp.step;
    mlir::scf::WhileOp::create(b, 
        loc, mlir::TypeRange{}, mlir::ValueRange{},
        [&](mlir::OpBuilder &nb2, mlir::Location nl, mlir::ValueRange) {
          // before region: check condition (true = unconditional)
          mlir::Value cond_val;
          if (cond_nid) cond_val = emit(nb2, cond_nid);
          if (!cond_val)
            cond_val =
                mlir::arith::ConstantOp::create(nb2, nl, nb2.getBoolAttr(true))
                    .getResult();
          mlir::scf::ConditionOp::create(nb2, nl, cond_val, mlir::ValueRange{});
        },
        [&](mlir::OpBuilder &nb2, mlir::Location nl, mlir::ValueRange) {
          // do region: body then step
          if (body_nid) emit(nb2, body_nid);
          if (step_nid) emit(nb2, step_nid);
          mlir::scf::YieldOp::create(nb2, nl);
        });
    return {};
  }

  case NodeKind::ForRange: {
    // a = loop_var SymId, b = IterCreate source NodeId, c = body Block
    // emit as index loop over len(source)
    SymId loop_var = na;
    mlir::Value src = emit(b, nb);
    if (!src) return {};
    auto src_ty = src.getType();
    mlir::Value len_val;
    mlir::Type elem_ty;
    if (auto vec_ty = mlir::dyn_cast<mlir::VectorType>(src_ty)) {
      elem_ty = vec_ty.getElementType();
      len_val = mlir::arith::ConstantOp::create(b, 
                     loc, b.getI32IntegerAttr(vec_ty.getNumElements()))
                    .getResult();
    } else {
      // not expected in shader code; skip
      return {};
    }

    // allocate counter and loop var
    mlir::Value i_ptr = insert_alloca(b, b.getI32Type(), "__range_i");
    mlir::Value var_ptr = insert_alloca(b, elem_ty, sym_name(loop_var));
    var_ptrs[loop_var] = var_ptr;
    var_store(b,
              mlir::arith::ConstantOp::create(b, loc, b.getI32IntegerAttr(0))
                  .getResult(),
              i_ptr);

    NodeId body_nid = nc;
    mlir::scf::WhileOp::create(b, 
        loc, mlir::TypeRange{}, mlir::ValueRange{},
        [&](mlir::OpBuilder &nb2, mlir::Location nl, mlir::ValueRange) {
          auto z = mlir::arith::ConstantIndexOp::create(nb2, nl, 0).getResult();
          mlir::Value i =
              mlir::memref::LoadOp::create(nb2, nl, i_ptr, mlir::ValueRange{z})
                  .getResult();
          mlir::Value cond =
              mlir::arith::CmpIOp::create(nb2,
                     nl, mlir::arith::CmpIPredicate::slt, i, len_val)
                  .getResult();
          mlir::scf::ConditionOp::create(nb2, nl, cond, mlir::ValueRange{});
        },
        [&](mlir::OpBuilder &nb2, mlir::Location nl, mlir::ValueRange) {
          auto z = mlir::arith::ConstantIndexOp::create(nb2, nl, 0).getResult();
          mlir::Value i =
              mlir::memref::LoadOp::create(nb2, nl, i_ptr, mlir::ValueRange{z})
                  .getResult();
          mlir::Value elem = mlir::vector::ExtractOp::create(nb2,
                                    nl, src, mlir::OpFoldResult(i))
                                 .getResult();
          mlir::memref::StoreOp::create(nb2, nl, elem, var_ptr,
                                            mlir::ValueRange{z});
          if (body_nid) emit(nb2, body_nid);
          mlir::Value i1 = mlir::arith::AddIOp::create(nb2,
                                  nl, i,
                                  mlir::arith::ConstantOp::create(nb2,
                                         nl, nb2.getI32IntegerAttr(1))
                                      .getResult())
                               .getResult();
          mlir::memref::StoreOp::create(nb2, nl, i1, i_ptr, mlir::ValueRange{z});
          mlir::scf::YieldOp::create(nb2, nl);
        });
    return {};
  }

    // ---- shader intrinsics ----

  case NodeKind::ShaderTexture2d: {
    mlir::Value idx = emit(b, na);
    if (!idx) return {};
    auto tex_ty = TextureType::get(&ctx);
    return Texture2dOp::create(b, loc, tex_ty, idx).getResult();
  }

  case NodeKind::ShaderSampler: {
    mlir::Value idx = emit(b, na);
    if (!idx) return {};
    auto samp_ty = SamplerType::get(&ctx);
    return SamplerOp::create(b, loc, samp_ty, idx).getResult();
  }

  case NodeKind::ShaderSample: {
    mlir::Value tex = emit(b, na), samp = emit(b, nb), uv = emit(b, nc);
    if (!tex || !samp || !uv) return {};
    auto f32 = b.getF32Type();
    auto vec4_ty = mlir::VectorType::get({4}, f32);
    return SampleOp::create(b, loc, vec4_ty, tex, samp, uv).getResult();
  }

  case NodeKind::ShaderDrawId:
    return DrawIdOp::create(b, loc, b.getI32Type()).getResult();

  case NodeKind::ShaderVertexId:
    return VertexIdOp::create(b, loc, b.getI32Type()).getResult();

  case NodeKind::ShaderDrawPacket: {
    mlir::Value id = emit(b, na);
    if (!id) return {};
    auto dp_ty = DrawPacketType::get(&ctx);
    return DrawPacketOp::create(b, loc, dp_ty, id).getResult();
  }

  case NodeKind::ShaderFrameRead: {
    mlir::Value offset = emit(b, na);
    if (!offset) return {};
    TypeId tid = static_cast<TypeId>(nb);
    // try scalar/vector first
    mlir::Type elem_ty = type_from_typeid(b, type_ast, interner, tid);
    if (elem_ty)
      return FrameReadOp::create(b, loc, elem_ty, offset,
                                mlir::TypeAttr::get(elem_ty))
          .getResult();
    // try struct type: decompose into per-field reads
    return emit_struct_frame_read(b, offset, tid);
  }

  default: return {};
  }
}

// resolve the SymId of "self" from the method's FuncSig param list.
static SymId find_self_sym(const Symbol &sym, const Module &mod,
                           const Interner &interner) {
  for (u32 pi = 0; pi < sym.sig.params_count; ++pi) {
    const FuncParam &p = mod.params[sym.sig.params_start + pi];
    if (interner.view(p.name) == "self") return p.name;
  }
  return 0;
}

// return MLIR type for a function parameter TypeId (Named types only).
static mlir::Type param_type(mlir::Builder &b, const TypeAst &ta,
                             const Interner &interner, TypeId tid) {
  return type_from_typeid(b, ta, interner, tid);
}

// emit one @shader_fn helper or @stage method into the mlir::ModuleOp.
// returns the created func::FuncOp (which may be registered in a helper map).
static mlir::func::FuncOp
emit_func(mlir::OpBuilder &b, mlir::Location loc, mlir::ModuleOp mlir_mod,
          const LoadedModule &lm, const SemaResult &sema,
          const Interner &interner, u32 mod_idx, SymId shader_type,
          SymId type_sym_id_sym, SymbolId method_sym_id,
          std::string_view func_name,
          std::optional<std::string_view> stage_attr,
          std::unordered_map<SymId, mlir::func::FuncOp> &helper_fns,
          std::span<const LoadedModule> all_modules) {
  if (method_sym_id == kInvalidSymbol) return {};
  const Symbol &msym = sema.syms.symbols[method_sym_id];
  if (!msym.body) return {};

  // build function type: () → void for stages/helpers (self is implicit)
  llvm::SmallVector<mlir::Type> param_types;
  for (u32 pi = 0; pi < msym.sig.params_count; ++pi) {
    const FuncParam &p = lm.mod.params[msym.sig.params_start + pi];
    // skip self (Ref type)
    if (lm.type_ast.kind[p.type] == TypeKind::Ref) continue;
    mlir::Type pt = param_type(b, lm.type_ast, interner, p.type);
    if (pt) param_types.push_back(pt);
  }
  auto fn_ty = mlir::FunctionType::get(b.getContext(), param_types, {});

  // insert at module end
  mlir::OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToEnd(mlir_mod.getBody());
  auto fn = mlir::func::FuncOp::create(b, loc, func_name, fn_ty);
  fn.setPrivate();
  fn->setAttr("shader_type", b.getStringAttr(interner.view(shader_type)));
  if (stage_attr) fn->setAttr("stage", b.getStringAttr(*stage_attr));

  // create entry block and emit body
  mlir::Block *entry = fn.addEntryBlock();
  mlir::OpBuilder body_b(entry, entry->begin());

  ShaderEmitCtx ectx{
      *b.getContext(), loc,         lm.ir,
      lm.type_ast,     lm.mod,      sema,
      interner,        mod_idx,     find_self_sym(msym, lm.mod, interner),
      shader_type,     all_modules, {},
      {},              helper_fns,  entry,
      {},              0};

  // bind explicit params to sym_vals
  {
    u32 arg_idx = 0;
    for (u32 pi = 0; pi < msym.sig.params_count; ++pi) {
      const FuncParam &p = lm.mod.params[msym.sig.params_start + pi];
      if (lm.type_ast.kind[p.type] == TypeKind::Ref) continue;
      if (arg_idx < entry->getNumArguments())
        ectx.sym_vals[p.name] = entry->getArgument(arg_idx++);
    }
  }

  ectx.emit(body_b, msym.body);

  // add implicit void return if not already terminated
  if (entry->empty() || !entry->back().hasTrait<mlir::OpTrait::IsTerminator>())
    mlir::func::ReturnOp::create(body_b, loc);

  return fn;
}

mlir::OwningOpRef<mlir::ModuleOp>
lower_to_mlir(mlir::MLIRContext &ctx, std::span<const LoadedModule> modules,
              const SemaResult &sema, const Interner &interner) {
  ctx.loadDialect<UmShaderDialect, mlir::func::FuncDialect,
                  mlir::arith::ArithDialect, mlir::scf::SCFDialect,
                  mlir::memref::MemRefDialect, mlir::vector::VectorDialect,
                  mlir::spirv::SPIRVDialect>();

  auto loc = mlir::UnknownLoc::get(&ctx);
  mlir::OpBuilder b(&ctx);
  auto mlir_mod = mlir::ModuleOp::create(loc);
  b.setInsertionPointToEnd(mlir_mod.getBody());

  for (u32 mi = 0; mi < static_cast<u32>(modules.size()); ++mi) {
    const LoadedModule &lm = modules[mi];
    const Module &mod = lm.mod;
    if (mod.shader_stages.empty() && mod.shader_fns.empty()) continue;

    // for each @shader struct that has stage methods, emit helpers first
    // (so stages can call them), then emit stages.
    // collect all shader type names that appear in stages/fns for this module.
    std::unordered_map<SymId, std::unordered_map<SymId, mlir::func::FuncOp>>
        per_shader_helpers;

    // @shader_fn helpers
    for (const ShaderFnInfo &sfi : mod.shader_fns) {
      SymbolId type_sid = sema.syms.lookup(mi, sfi.shader_type);
      if (type_sid == kInvalidSymbol) continue;
      SymbolId method_sid = sema.methods.lookup(type_sid, sfi.method_name);
      if (method_sid == kInvalidSymbol) continue;

      std::string fn_name = std::string(interner.view(sfi.shader_type)) + "_" +
                            std::string(interner.view(sfi.method_name));
      auto &helper_map = per_shader_helpers[sfi.shader_type];
      auto fn =
          emit_func(b, loc, mlir_mod, lm, sema, interner, mi, sfi.shader_type,
                    type_sid, method_sid, fn_name, std::nullopt, helper_map,
                    modules);
      if (fn) helper_map[sfi.method_name] = fn;
    }

    // @stage methods
    for (const ShaderStageInfo &si : mod.shader_stages) {
      SymbolId type_sid = sema.syms.lookup(mi, si.shader_type);
      if (type_sid == kInvalidSymbol) continue;
      SymbolId method_sid = sema.methods.lookup(type_sid, si.method_name);
      if (method_sid == kInvalidSymbol) continue;

      auto sv = interner.view(si.stage_name_sym);
      std::string_view stage_str = (sv == "fragment") ? "fragment" : "vertex";
      std::string fn_name = std::string(interner.view(si.shader_type)) + "_" +
                            std::string(interner.view(si.method_name));
      auto &helper_map = per_shader_helpers[si.shader_type];
      emit_func(b, loc, mlir_mod, lm, sema, interner, mi, si.shader_type,
                type_sid, method_sid, fn_name, stage_str, helper_map,
                modules);
    }
  }

  return mlir::OwningOpRef<mlir::ModuleOp>(mlir_mod);
}

} // namespace um::shader
