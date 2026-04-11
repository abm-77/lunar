#pragma once

#include <unordered_map>
#include <vector>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>

#include <common/types.h>
#include <compiler/driver/loader.h>
#include <compiler/frontend/ast.h>
#include <compiler/sema/ctypes.h>
#include <compiler/sema/lower_types.h>
#include <compiler/sema/symbol.h>

struct CTypeLowerer {
  llvm::LLVMContext &ctx;
  TypeTable &types;
  const SymbolTable &syms;
  const std::vector<LoadedModule> &modules;
  const Interner &interner;

  // CTypeId → cached LLVM type (null means lowering failed / type is void)
  std::unordered_map<CTypeId, llvm::Type *> cache;

  // CTypeId → cached LLVM function type (null means lowering failed / type is
  // void)
  std::unordered_map<CTypeId, llvm::FunctionType *> fn_type_cache;


  CTypeLowerer(llvm::LLVMContext &c, TypeTable &t, const SymbolTable &s,
               const std::vector<LoadedModule> &m, const Interner &i)
      : ctx(c), types(t), syms(s), modules(m), interner(i) {}

  // returns the LLVM type for the given canonical type.
  // returns nullptr only for CTypeKind::Void (callers must handle that).
  llvm::Type *lower(CTypeId id) {
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    llvm::Type *result = lower_impl(id);
    cache[id] = result;
    return result;
  }

  // returns the u32 index of a named field within its LLVM struct layout.
  // field order matches the order fields appear in the StructType node.
  u32 field_index(SymbolId struct_sym, SymId field_name,
                  CTypeId struct_ctid = 0) {
    Symbol sym = syms.get(struct_sym);
    const BodyIR &ir = modules[sym.module_idx].ir;
    NodeId type_node = resolve_struct_type_node(sym, ir, struct_ctid);
    assert(ir.nodes.kind[type_node] == NodeKind::StructType &&
           "attempting to find field of non-struct type");
    u32 ls = ir.nodes.b[type_node], n = ir.nodes.c[type_node];
    for (u32 i = 0; i < n; ++i) { // list holds [SymId, TypeId] pairs
      SymId id = ir.nodes.list[ls + (i * 2)]; // even offset = SymId (name)
      if (id == field_name) return i;
    }
    return 0;
  }

private:
  // resolve sym.type_node to effective StructType NodeId.
  // for @gen types (MetaBlock), evaluates the block using type args from
  // struct_ctid.
  NodeId resolve_struct_type_node(const Symbol &sym, const BodyIR &ir,
                                  CTypeId struct_ctid) {
    NodeId type_node = sym.type_node;
    if (ir.nodes.kind[type_node] != NodeKind::MetaBlock) return type_node;

    const CType &sct =
        types.types[struct_ctid]; // may be Void if struct_ctid==0
    const Module &mod = modules[sym.module_idx].mod;
    const TypeAst &ta = modules[sym.module_idx].type_ast;

    std::unordered_map<SymId, CTypeId> subst;
    for (u32 i = 0; i < sym.generics_count && i < sct.list_count; ++i) {
      SymId pname = mod.generic_params[sym.generics_start + i].name;
      subst[pname] = types.list[sct.list_start + i];
    }

    std::vector<ModuleContext> local_ctx;
    local_ctx.reserve(modules.size());
    for (const auto &m : modules)
      local_ctx.push_back({&m.type_ast, &m.ir, &m.mod, m.src, &m.import_map});

    TypeLowerer tl(ta, syms, interner, types);
    tl.type_subst = subst;
    tl.module_idx = sym.module_idx;
    tl.module_contexts = &local_ctx;
    tl.import_map = &modules[sym.module_idx].import_map;

    NodeId evaled = tl.eval_meta_block(type_node, ir, nullptr);
    return evaled != 0 ? evaled : type_node;
  }

  llvm::Type *lower_impl(CTypeId id) {
    const CType &ct = types.types[id];
    switch (ct.kind) {
    // primitives
    case CTypeKind::Void: return nullptr; // callers check for void specially
    case CTypeKind::Bool: return llvm::Type::getInt1Ty(ctx);
    case CTypeKind::I8: return llvm::Type::getInt8Ty(ctx);
    case CTypeKind::I16: return llvm::Type::getInt16Ty(ctx);
    case CTypeKind::I32: return llvm::Type::getInt32Ty(ctx);
    case CTypeKind::I64: return llvm::Type::getInt64Ty(ctx);
    case CTypeKind::U8: return llvm::Type::getInt8Ty(ctx);
    case CTypeKind::U16: return llvm::Type::getInt16Ty(ctx);
    case CTypeKind::U32: return llvm::Type::getInt32Ty(ctx);
    case CTypeKind::U64: return llvm::Type::getInt64Ty(ctx);
    case CTypeKind::F32: return llvm::Type::getFloatTy(ctx);
    case CTypeKind::F64: return llvm::Type::getDoubleTy(ctx);
    case CTypeKind::Vec:
      return llvm::FixedVectorType::get(lower(ct.inner), ct.count);
    case CTypeKind::Mat:
      return llvm::ArrayType::get(lower(ct.inner), ct.count);

    // reference → opaque pointer
    case CTypeKind::Ref: return llvm::PointerType::get(ctx, 0);

    // [N x T]
    case CTypeKind::Array: {
      llvm::Type *elem = lower(ct.inner);
      assert(types.types[ct.inner].kind != CTypeKind::Void &&
             "cannot have array with void element type");
      return llvm::ArrayType::get(elem, ct.count);
    }

    // anonymous struct { T0, T1, ... }
    case CTypeKind::Tuple: {
      std::vector<llvm::Type *> elems;
      elems.reserve(ct.list_count);
      for (u32 i = 0; i < ct.list_count; ++i)
        elems.push_back(lower(types.list[ct.list_start + i]));
      return llvm::StructType::get(ctx, elems);
    }

    // fn(params) -> ret → pointer to FunctionType; list = [ret_type, param0,
    // param1, ...]
    case CTypeKind::Fn: {
      llvm::Type *ret = lower(types.list[ct.list_start]);
      llvm::Type *ret_type = ret ? ret : llvm::Type::getVoidTy(ctx);
      std::vector<llvm::Type *> params;
      params.reserve(ct.list_count);
      for (u32 i = 1; i < ct.list_count; ++i)
        params.push_back(lower(types.list[ct.list_start + i]));
      auto *ft = llvm::FunctionType::get(ret_type, params, /*isVarArg=*/false);
      fn_type_cache[id] = ft;
      return llvm::PointerType::get(ctx, 0);
    }

    // named struct
    case CTypeKind::Struct: return lower_struct(id, ct);

    // enum → i32 tag
    case CTypeKind::Enum: return llvm::Type::getInt32Ty(ctx);

    // slice → { ptr, i64 }
    case CTypeKind::Slice: {
      std::vector<llvm::Type *> fields = {llvm::PointerType::getUnqual(ctx),
                                          llvm::Type::getInt64Ty(ctx)};
      return llvm::StructType::get(ctx, fields);
    }

    case CTypeKind::Iter: { // { ptr*, i64 len, i64 idx }
      std::vector<llvm::Type *> fields = {llvm::PointerType::getUnqual(ctx),
                                          llvm::Type::getInt64Ty(ctx),
                                          llvm::Type::getInt64Ty(ctx)};
      return llvm::StructType::get(ctx, fields);
    }
    }
    return nullptr; // unreachable
  }

  llvm::Type *lower_struct(CTypeId id, const CType &ct) {
    const Symbol &sym = syms.get(ct.symbol);
    std::string name =
        std::string(interner.view(sym.name)) + "." + std::to_string(ct.symbol);

    auto st = llvm::StructType::create(ctx, name);
    cache[id] = st;

    const BodyIR &ir = modules[sym.module_idx].ir;
    const Module &mod = modules[sym.module_idx].mod;
    const TypeAst &ta = modules[sym.module_idx].type_ast;

    // build type substitution param name -> concrete CTypeId
    std::unordered_map<SymId, CTypeId> subst;
    for (u32 i = 0; i < sym.generics_count; ++i) {
      SymId param_name = mod.generic_params[sym.generics_start + i].name;
      CTypeId concrete = types.list[ct.list_start + i];
      subst[param_name] = concrete;
    }

    // build module_contexts so the TypeLowerer can distinguish enum vs struct
    // fields from any module.
    std::vector<ModuleContext> local_ctx;
    local_ctx.reserve(modules.size());
    for (const auto &m : modules)
      local_ctx.push_back({&m.type_ast, &m.ir, &m.mod, m.src, &m.import_map});

    TypeLowerer tl(ta, syms, interner, types);
    tl.type_subst = subst;
    tl.module_idx = sym.module_idx;
    tl.module_contexts = &local_ctx;
    tl.import_map = &modules[sym.module_idx].import_map;

    // resolve the effective StructType NodeId (@gen types have a MetaBlock
    // type_node)
    NodeId stype_nid = sym.type_node;
    if (ir.nodes.kind[stype_nid] == NodeKind::MetaBlock) {
      stype_nid = tl.eval_meta_block(stype_nid, ir, nullptr);
      assert(stype_nid != 0 && "failed to evaluate @gen type body");
    }
    assert(ir.nodes.kind[stype_nid] == NodeKind::StructType &&
           "attempting struct lowering for non-struct type");

    u32 ls = ir.nodes.b[stype_nid], n = ir.nodes.c[stype_nid];
    std::vector<llvm::Type *> field_types;
    field_types.reserve(n);
    for (u32 i = 0; i < n; ++i) {
      TypeId field_tid = ir.nodes.list[ls + (i * 2) + 1];
      auto ctid = tl.lower(field_tid);
      assert(ctid && "failed to lower field type");
      field_types.push_back(lower(*ctid));
    }

    st->setBody(field_types);
    return st;
  }
};
