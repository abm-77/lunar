#pragma once

#include <string>
#include <unordered_map>
#include <vector>

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
  // Heap-allocated so ownership can be transferred to CodegenResult after
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

  // Type lowerer shared across all emitters.
  CTypeLowerer type_lower;

  // Accumulated call-site table for @site_id() intrinsic.
  std::vector<SiteEntry> sites;
  // Per-module context vector for cross-module type lowering in TypeLowerer.
  std::vector<ModuleContext> module_contexts;

  CodegenCtx(SemaResult &sr, const std::vector<LoadedModule> &mods,
             const Interner &intern, std::string_view module_name)
      : ctx_owned(std::make_unique<llvm::LLVMContext>()),
        ctx(*ctx_owned),
        module(std::make_unique<llvm::Module>(
            llvm::StringRef{module_name.data(), module_name.size()}, *ctx_owned)),
        sema(sr), modules(mods), interner(intern),
        type_lower(*ctx_owned, sr.types, sr.syms, mods, interner) {
    for (auto &lm : mods)
      module_contexts.push_back({&lm.type_ast, &lm.ir, &lm.mod, lm.src, &lm.import_map});
  }

  // Allocate a new call-site entry and return its index (= site ID).
  // Scans src[0..byte_offset] for newlines to compute 1-based line/col.
  u32 alloc_site(std::string_view src, u32 byte_offset, std::string_view filename) {
    u32 line = 1, col = 1;
    u32 end = byte_offset < static_cast<u32>(src.size())
                  ? byte_offset
                  : static_cast<u32>(src.size());
    for (u32 i = 0; i < end; ++i) {
      if (src[i] == '\n') { ++line; col = 1; }
      else { ++col; }
    }
    u32 idx = static_cast<u32>(sites.size());
    sites.push_back({std::string(filename), line, col});
    return idx;
  }

  // Create a syntax-level TypeLowerer configured for the given module.
  // Use this wherever a TypeId (from FuncSig, annotate_type, etc.) needs to
  // be resolved to a CTypeId before passing to type_lower.lower().
  TypeLowerer make_type_lowerer(u32 module_idx) {
    TypeLowerer tl(modules[module_idx].type_ast, sema.syms, interner,
                   type_lower.types);
    tl.module_idx = module_idx;
    tl.current_ir = &modules[module_idx].ir;
    tl.import_map = &modules[module_idx].import_map;
    if (!module_contexts.empty()) tl.module_contexts = &module_contexts;
    return tl;
  }
};

// Inject the mono-instance type substitution (T→i32 etc.) into the TypeLowerer.
// Required for @size_of(T) / @align_of(T) emitted inside mono function bodies.
inline void inject_mono_subst(const Symbol &sym, TypeLowerer &tl) {
  for (auto &[k, v] : sym.mono_type_subst) tl.type_subst[k] = v;
}

// For impl methods, inject "self" → owner struct CTypeId into the TypeLowerer's
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
