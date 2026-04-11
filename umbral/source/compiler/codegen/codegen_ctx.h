#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <common/interner.h>
#include <compiler/sema/sema.h>

#include "type_lower.h"

struct SiteEntry {
  std::string file;
  u32 line;
  u32 col;
};

struct CodegenCtx {
  // heap-allocated so ownership can be transferred to CodegenResult after
  // codegen completes. LLVMContext must outlive the Module — keep both.
  std::unique_ptr<llvm::LLVMContext> ctx_owned;
  llvm::LLVMContext &ctx; // reference into *ctx_owned
  std::unique_ptr<llvm::Module> module;
  SemaResult &sema;
  const std::vector<LoadedModule> &modules;
  const Interner &interner;

  // SymbolId → declared llvm::Function* (populated during the declaration pass)
  std::unordered_map<SymbolId, llvm::Function *> fn_map;
  // SymbolId → declared llvm::GlobalVariable* (populated during the declaration
  // pass)
  std::unordered_map<SymbolId, llvm::GlobalVariable *> global_map;

  // type lowerer shared across all emitters.
  CTypeLowerer type_lower;

  // accumulated call-site table for @site_id() intrinsic.
  std::vector<SiteEntry> sites;
  // per-module context vector for cross-module type lowering in TypeLowerer.
  std::vector<ModuleContext> module_contexts;

  // debug info — only populated when debug_info == true.
  bool debug_info = false;
  std::unique_ptr<llvm::DIBuilder> dibuilder;
  llvm::DICompileUnit *di_cu = nullptr;
  std::unordered_map<u32, llvm::DIFile *> di_files; // module_idx → DIFile
  // line_tables[mod_idx][i] = byte offset of line i+1 start; built lazily.
  std::unordered_map<u32, std::vector<u32>> line_tables;
  std::unordered_map<CTypeId, llvm::DIType *> di_type_cache;

  CodegenCtx(SemaResult &sr, const std::vector<LoadedModule> &mods,
             const Interner &intern, std::string_view module_name,
             bool dbg = false)
      : ctx_owned(std::make_unique<llvm::LLVMContext>()), ctx(*ctx_owned),
        module(std::make_unique<llvm::Module>(
            llvm::StringRef{module_name.data(), module_name.size()},
            *ctx_owned)),
        sema(sr), modules(mods), interner(intern),
        type_lower(*ctx_owned, sr.types, sr.syms, mods, interner) {
    for (auto &lm : mods)
      module_contexts.push_back(
          {&lm.type_ast, &lm.ir, &lm.mod, lm.src, &lm.import_map});

    debug_info = dbg;
    if (debug_info) {
      dibuilder = std::make_unique<llvm::DIBuilder>(*module);
      const LoadedModule &entry_lm = mods.back();
      llvm::DIFile *entry_file = dibuilder->createFile(
          entry_lm.abs_path.filename().string(),
          entry_lm.abs_path.parent_path().string());
      di_cu = dibuilder->createCompileUnit(
          llvm::dwarf::DW_LANG_C, entry_file, "uc",
          /*isOptimized=*/false, /*Flags=*/"", /*RV=*/0);
    }
  }

  // get or create a DIFile node for the given module index.
  llvm::DIFile *get_di_file(u32 mod_idx) {
    auto it = di_files.find(mod_idx);
    if (it != di_files.end()) return it->second;
    const LoadedModule &lm = modules[mod_idx];
    llvm::DIFile *f = dibuilder->createFile(
        lm.abs_path.filename().string(),
        lm.abs_path.parent_path().string());
    di_files[mod_idx] = f;
    return f;
  }

  // compute 1-based line and column for byte_off in the given module's source.
  // builds a line-start table on first call per module (O(n) once, O(log n) after).
  std::pair<u32, u32> get_line_col(u32 mod_idx, u32 byte_off) {
    auto &table = line_tables[mod_idx];
    if (table.empty()) {
      const std::string &src = modules[mod_idx].src;
      table.push_back(0);
      for (u32 i = 0; i < static_cast<u32>(src.size()); ++i)
        if (src[i] == '\n') table.push_back(i + 1);
    }
    auto it = std::upper_bound(table.begin(), table.end(), byte_off);
    u32 line = static_cast<u32>(std::distance(table.begin(), it));
    u32 line_start = (it == table.begin()) ? 0 : *std::prev(it);
    u32 col = byte_off - line_start + 1;
    return {line, col};
  }

  // map a CTypeId to its LLVM DI type descriptor.
  // returns nullptr for compile-time-only types (Void, ConstInt, FieldIter).
  // references are always opaque pointers to avoid self-referential struct cycles.
  llvm::DIType *make_di_type(CTypeId ctid) {
    if (!debug_info || !dibuilder) return nullptr;
    if (ctid == 0) return nullptr;

    {
      auto it = di_type_cache.find(ctid);
      if (it != di_type_cache.end()) return it->second;
    }
    di_type_cache[ctid] = nullptr; // cycle-break sentinel

    const CType &ct = sema.types.types[ctid];
    const llvm::DataLayout &dl = module->getDataLayout();
    llvm::DIType *result = nullptr;

    using K = CTypeKind;
    switch (ct.kind) {
    case K::Bool: result = dibuilder->createBasicType("bool", 1,  llvm::dwarf::DW_ATE_boolean); break;
    case K::I8:   result = dibuilder->createBasicType("i8",   8,  llvm::dwarf::DW_ATE_signed);  break;
    case K::I16:  result = dibuilder->createBasicType("i16",  16, llvm::dwarf::DW_ATE_signed);  break;
    case K::I32:  result = dibuilder->createBasicType("i32",  32, llvm::dwarf::DW_ATE_signed);  break;
    case K::I64:  result = dibuilder->createBasicType("i64",  64, llvm::dwarf::DW_ATE_signed);  break;
    case K::U8:   result = dibuilder->createBasicType("u8",   8,  llvm::dwarf::DW_ATE_unsigned); break;
    case K::U16:  result = dibuilder->createBasicType("u16",  16, llvm::dwarf::DW_ATE_unsigned); break;
    case K::U32:  result = dibuilder->createBasicType("u32",  32, llvm::dwarf::DW_ATE_unsigned); break;
    case K::U64:  result = dibuilder->createBasicType("u64",  64, llvm::dwarf::DW_ATE_unsigned); break;
    case K::F32:  result = dibuilder->createBasicType("f32",  32, llvm::dwarf::DW_ATE_float);   break;
    case K::F64:  result = dibuilder->createBasicType("f64",  64, llvm::dwarf::DW_ATE_float);   break;

    // references are pointer-sized; don't recurse to avoid cycles.
    case K::Ref:
      result = dibuilder->createPointerType(nullptr, dl.getPointerSizeInBits());
      break;

    // enum variants are i32 tags at runtime.
    case K::Enum: {
      const Symbol &esym = sema.syms.get(ct.symbol);
      result = dibuilder->createBasicType(
          std::string(interner.view(esym.name)), 32, llvm::dwarf::DW_ATE_signed);
      break;
    }

    case K::Array: {
      llvm::Type *llvm_arr = type_lower.lower(ctid);
      llvm::Metadata *sub = dibuilder->getOrCreateSubrange(0, ct.count);
      result = dibuilder->createArrayType(
          dl.getTypeSizeInBits(llvm_arr),
          dl.getABITypeAlign(llvm_arr).value() * 8,
          make_di_type(ct.inner),
          dibuilder->getOrCreateArray({sub}));
      break;
    }

    // []T slice: { ptr, i64 len }
    case K::Slice: {
      llvm::Type *llvm_sl = type_lower.lower(ctid);
      auto *sl_st = llvm::cast<llvm::StructType>(llvm_sl);
      const llvm::StructLayout *layout = dl.getStructLayout(sl_st);
      llvm::DIFile *df = get_di_file(static_cast<u32>(modules.size()) - 1);
      llvm::DIType *ptr_di = dibuilder->createPointerType(
          make_di_type(ct.inner), dl.getPointerSizeInBits());
      llvm::DIType *len_di = dibuilder->createBasicType(
          "u64", 64, llvm::dwarf::DW_ATE_unsigned);
      auto *pm = dibuilder->createMemberType(di_cu, "ptr", df, 0,
          dl.getPointerSizeInBits(), dl.getPointerABIAlignment(0).value() * 8,
          layout->getElementOffsetInBits(0), llvm::DINode::FlagZero, ptr_di);
      auto *lm_di = dibuilder->createMemberType(di_cu, "len", df, 0,
          64, dl.getABITypeAlign(llvm::Type::getInt64Ty(ctx)).value() * 8,
          layout->getElementOffsetInBits(1), llvm::DINode::FlagZero, len_di);
      result = dibuilder->createStructType(di_cu, "slice", df, 0,
          dl.getTypeSizeInBits(sl_st),
          dl.getABITypeAlign(sl_st).value() * 8,
          llvm::DINode::FlagZero, nullptr,
          dibuilder->getOrCreateArray({pm, lm_di}));
      break;
    }

    // iter: { ptr, i64 len, i64 idx }
    case K::Iter: {
      llvm::Type *llvm_it = type_lower.lower(ctid);
      auto *it_st = llvm::cast<llvm::StructType>(llvm_it);
      const llvm::StructLayout *layout = dl.getStructLayout(it_st);
      llvm::DIFile *df = get_di_file(static_cast<u32>(modules.size()) - 1);
      llvm::DIType *ptr_di = dibuilder->createPointerType(
          make_di_type(ct.inner), dl.getPointerSizeInBits());
      llvm::DIType *i64_di = dibuilder->createBasicType(
          "u64", 64, llvm::dwarf::DW_ATE_unsigned);
      auto *pm = dibuilder->createMemberType(di_cu, "ptr", df, 0,
          dl.getPointerSizeInBits(), dl.getPointerABIAlignment(0).value() * 8,
          layout->getElementOffsetInBits(0), llvm::DINode::FlagZero, ptr_di);
      auto *lm_di = dibuilder->createMemberType(di_cu, "len", df, 0,
          64, dl.getABITypeAlign(llvm::Type::getInt64Ty(ctx)).value() * 8,
          layout->getElementOffsetInBits(1), llvm::DINode::FlagZero, i64_di);
      auto *idm = dibuilder->createMemberType(di_cu, "idx", df, 0,
          64, dl.getABITypeAlign(llvm::Type::getInt64Ty(ctx)).value() * 8,
          layout->getElementOffsetInBits(2), llvm::DINode::FlagZero, i64_di);
      result = dibuilder->createStructType(di_cu, "iter", df, 0,
          dl.getTypeSizeInBits(it_st),
          dl.getABITypeAlign(it_st).value() * 8,
          llvm::DINode::FlagZero, nullptr,
          dibuilder->getOrCreateArray({pm, lm_di, idm}));
      break;
    }

    // vec<T, N>: LLVM fixed-width SIMD vector
    case K::Vec: {
      llvm::Type *llvm_v = type_lower.lower(ctid);
      llvm::Metadata *sub = dibuilder->getOrCreateSubrange(0, ct.count);
      result = dibuilder->createVectorType(
          dl.getTypeSizeInBits(llvm_v),
          dl.getABITypeAlign(llvm_v).value() * 8,
          make_di_type(ct.inner),
          dibuilder->getOrCreateArray({sub}));
      break;
    }

    // mat<T, N, M>: array of N column vecs
    case K::Mat: {
      llvm::Type *llvm_m = type_lower.lower(ctid);
      llvm::Metadata *sub = dibuilder->getOrCreateSubrange(0, ct.count);
      result = dibuilder->createArrayType(
          dl.getTypeSizeInBits(llvm_m),
          dl.getABITypeAlign(llvm_m).value() * 8,
          make_di_type(ct.inner),
          dibuilder->getOrCreateArray({sub}));
      break;
    }

    // tuple: anonymous struct with fields _0, _1, ...
    case K::Tuple: {
      llvm::Type *llvm_t = type_lower.lower(ctid);
      auto *t_st = llvm::cast<llvm::StructType>(llvm_t);
      const llvm::StructLayout *layout = dl.getStructLayout(t_st);
      llvm::DIFile *df = get_di_file(static_cast<u32>(modules.size()) - 1);
      std::vector<llvm::Metadata *> members;
      members.reserve(ct.list_count);
      for (u32 i = 0; i < ct.list_count; ++i) {
        CTypeId elem_ctid = sema.types.list[ct.list_start + i];
        llvm::Type *elem_llvm = type_lower.lower(elem_ctid);
        llvm::DIType *elem_di = make_di_type(elem_ctid);
        if (!elem_di) elem_di = dibuilder->createBasicType(
            "ptr", dl.getPointerSizeInBits(), llvm::dwarf::DW_ATE_address);
        members.push_back(dibuilder->createMemberType(di_cu,
            "_" + std::to_string(i), df, 0,
            elem_llvm ? dl.getTypeSizeInBits(elem_llvm) : dl.getPointerSizeInBits(),
            elem_llvm ? dl.getABITypeAlign(elem_llvm).value() * 8
                      : dl.getPointerABIAlignment(0).value() * 8,
            layout->getElementOffsetInBits(i),
            llvm::DINode::FlagZero, elem_di));
      }
      result = dibuilder->createStructType(di_cu, "tuple", df, 0,
          dl.getTypeSizeInBits(t_st),
          dl.getABITypeAlign(t_st).value() * 8,
          llvm::DINode::FlagZero, nullptr,
          dibuilder->getOrCreateArray(members));
      break;
    }

    // fn pointer: represent as opaque pointer.
    case K::Fn:
      result = dibuilder->createPointerType(nullptr, dl.getPointerSizeInBits());
      break;

    // named struct: mirror lower_struct to recover field names and types.
    case K::Struct: {
      const Symbol &struct_sym = sema.syms.get(ct.symbol);
      std::string name = std::string(interner.view(struct_sym.name))
                       + "." + std::to_string(ct.symbol);
      u32 mod_idx = struct_sym.module_idx;

      llvm::Type *llvm_st = type_lower.lower(ctid);
      auto *st = llvm::cast<llvm::StructType>(llvm_st);
      const llvm::StructLayout *layout = dl.getStructLayout(st);
      llvm::DIFile *df = get_di_file(mod_idx);

      const BodyIR  &body_ir = modules[mod_idx].ir;
      const Module  &mod     = modules[mod_idx].mod;
      const TypeAst &ta      = modules[mod_idx].type_ast;

      std::unordered_map<SymId, CTypeId> subst;
      for (u32 i = 0; i < struct_sym.generics_count; ++i) {
        SymId pname = mod.generic_params[struct_sym.generics_start + i].name;
        subst[pname] = sema.types.list[ct.list_start + i];
      }

      TypeLowerer tl(ta, sema.syms, interner, sema.types);
      tl.type_subst      = subst;
      tl.module_idx      = mod_idx;
      tl.module_contexts = &module_contexts;
      tl.import_map      = &modules[mod_idx].import_map;

      NodeId stype_nid = struct_sym.type_node;
      if (body_ir.nodes.kind[stype_nid] == NodeKind::MetaBlock) {
        stype_nid = tl.eval_meta_block(stype_nid, body_ir, nullptr);
        assert(stype_nid != 0 && "failed to evaluate @gen type for DI");
      }
      assert(body_ir.nodes.kind[stype_nid] == NodeKind::StructType);

      u32 ls = body_ir.nodes.b[stype_nid], n = body_ir.nodes.c[stype_nid];
      std::vector<llvm::Metadata *> members;
      members.reserve(n);
      for (u32 i = 0; i < n; ++i) {
        SymId  fname   = body_ir.nodes.list[ls + i * 2];
        TypeId ftid    = body_ir.nodes.list[ls + i * 2 + 1];
        auto   fctid_r = tl.lower(ftid);
        assert(fctid_r && "failed to lower struct field type for DI");
        CTypeId fctid  = *fctid_r;

        llvm::Type *fllvm  = type_lower.lower(fctid);
        uint64_t fsize  = fllvm ? dl.getTypeSizeInBits(fllvm) : dl.getPointerSizeInBits();
        uint32_t falign = fllvm ? dl.getABITypeAlign(fllvm).value() * 8
                                : dl.getPointerABIAlignment(0).value() * 8;

        llvm::DIType *fdi = make_di_type(fctid);
        if (!fdi) fdi = dibuilder->createBasicType(
            "ptr", dl.getPointerSizeInBits(), llvm::dwarf::DW_ATE_address);

        members.push_back(dibuilder->createMemberType(di_cu,
            interner.view(fname), df, 0,
            fsize, falign, layout->getElementOffsetInBits(i),
            llvm::DINode::FlagZero, fdi));
      }

      result = dibuilder->createStructType(di_cu, name, df, 0,
          dl.getTypeSizeInBits(st),
          dl.getABITypeAlign(st).value() * 8,
          llvm::DINode::FlagZero, nullptr,
          dibuilder->getOrCreateArray(members));
      break;
    }

    case K::ConstInt:
    case K::FieldIter:
    case K::Void:
    default:
      result = nullptr;
      break;
    }

    di_type_cache[ctid] = result;
    return result;
  }

  // allocate a new call-site entry and return its index (= site ID).
  // scans src[0..byte_offset] for newlines to compute 1-based line/col.
  u32 alloc_site(std::string_view src, u32 byte_offset,
                 std::string_view filename) {
    u32 line = 1, col = 1;
    u32 end = byte_offset < static_cast<u32>(src.size())
                  ? byte_offset
                  : static_cast<u32>(src.size());
    for (u32 i = 0; i < end; ++i) {
      if (src[i] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    u32 idx = static_cast<u32>(sites.size());
    sites.push_back({std::string(filename), line, col});
    return idx;
  }

  // create a syntax-level TypeLowerer configured for the given module.
  // use this wherever a TypeId (from FuncSig, annotate_type, etc.) needs to
  // be resolved to a CTypeId before passing to type_lower.lower().
  TypeLowerer make_type_lowerer(u32 module_idx) {
    TypeLowerer tl(modules[module_idx].type_ast, sema.syms, interner,
                   type_lower.types);
    tl.module_idx = module_idx;
    tl.import_map = &modules[module_idx].import_map;
    if (!module_contexts.empty()) tl.module_contexts = &module_contexts;
    return tl;
  }
};

// inject the mono-instance type substitution (T→i32 etc.) into the TypeLowerer.
// required for @size_of(T) / @align_of(T) emitted inside mono function bodies.
inline void inject_mono_subst(const Symbol &sym, TypeLowerer &tl) {
  if (sym.mono)
    for (auto &[k, v] : sym.mono->type_subst) tl.type_subst[k] = v;
}

// for impl methods, inject "self" → owner struct CTypeId into the TypeLowerer's
// type_subst so that &self / &mut self parameters resolve correctly.
inline void inject_self_subst(const Symbol &sym, TypeLowerer &tl,
                              CodegenCtx &cg) {
  if (sym.impl_owner == 0 || sym.sig.params_count == 0) return;
  const Module &mod = cg.modules[sym.module_idx].mod;
  const TypeAst &ta = cg.modules[sym.module_idx].type_ast;
  const FuncParam &fp0 = mod.params[sym.sig.params_start];
  if (fp0.type == 0 || ta.kind[fp0.type] != TypeKind::Ref) return;
  TypeId inner = ta.b[fp0.type];
  if (ta.kind[inner] != TypeKind::Named) return;
  SymId self_sym = ta.a[inner];
  SymbolId owner_sid = cg.sema.syms.lookup(sym.module_idx, sym.impl_owner);
  if (owner_sid == kInvalidSymbol) return;
  CType ct;
  ct.kind = CTypeKind::Struct;
  ct.symbol = owner_sid;
  tl.type_subst[self_sym] = cg.sema.types.intern(ct);
}
