#pragma once

#include <map>
#include <unordered_map>
#include <vector>

#include <common/interner.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/module.h>

#include <compiler/sema/ctypes.h>
#include <compiler/sema/itype.h>
#include <compiler/sema/lower_types.h>
#include <compiler/sema/symbol.h>

// cache of monomorphized instances: (generic SymbolId, type args) -> mono SymbolId
using MonoCache = std::map<std::pair<SymbolId, std::vector<CTypeId>>, SymbolId>;

// one pending mono instantiation queued for body-checking by the sema orchestrator.
struct PendingMono {
  SymbolId mono_id;
  u32 generic_mod_idx;
  u32 const_generic_scope_start;
  u32 const_generic_scope_count;
};

// bind all still-unresolved integer literal TypeVars to i32.
// call before infer_type_args so that integer literal args resolve naturally.
// TypeVars already bound by context (e.g., u64 function param) are left alone.
inline void flush_int_defaults(
    Unifier &unifier, TypeTable &types,
    const std::vector<std::pair<TypeVarId, NodeId>> &int_default_vars) {
  IType i32_t = IType::from(types.builtin(CTypeKind::I32));
  for (auto &[tv, _] : int_default_vars)
    if (unifier.resolve(IType::fresh(tv)).is_var)
      unifier.bindings[tv] = i32_t;
}

// infer concrete type args for a generic from actual argument ITypes.
// for each type-param T, find the first param typed as Named(T) and read
// the resolved concrete type of the corresponding argument.
// callers must flush_int_defaults() first so integer literal args are concrete.
inline std::vector<CTypeId> infer_type_args(
    const Symbol &gsym, const Module &g_mod, const TypeLowerer &g_lowerer,
    Unifier &unifier, const std::vector<IType> &arg_types) {
  std::vector<CTypeId> result;
  for (u32 k = 0; k < gsym.generics_count; ++k) {
    const GenericParam &gp = g_mod.generic_params[gsym.generics_start + k];
    if (!gp.is_type) {
      // const-generic: cannot infer from arg types; push 0 as placeholder.
      // explicit type args (e.g., SmallArray<i32, 10>::make()) are handled by
      // extracting them from the struct/path CType list before reaching here.
      result.push_back(0);
      continue;
    }
    CTypeId inferred = 0;
    for (u32 p = 0; p < gsym.sig.params_count && !inferred; ++p) {
      const FuncParam &fp = g_mod.params[gsym.sig.params_start + p];
      if (g_lowerer.type_ast.kind[fp.type] == TypeKind::Named &&
          g_lowerer.type_ast.a[fp.type] == gp.name && p < arg_types.size()) {
        IType at = unifier.resolve(arg_types[p]);
        if (!at.is_var) inferred = at.concrete;
      }
    }
    result.push_back(inferred);
  }
  return result;
}

// demand-driven monomorphization engine.
// request() is called from body-checking to prepare a mono instance.
// the sema orchestrator drains pending after each pass.
struct MonoEngine {
  SymbolTable &syms;
  TypeTable &types;
  const Interner &interner;
  const std::vector<ModuleContext> *module_contexts = nullptr;
  std::unordered_map<SymbolId, BodySema> *body_semas_out = nullptr;

  MonoCache cache;
  std::vector<PendingMono> pending;

  MonoEngine(SymbolTable &syms, TypeTable &types, const Interner &interner)
      : syms(syms), types(types), interner(interner) {}

  // returns mono SymbolId, registering a PendingMono if not already cached.
  SymbolId request(SymbolId generic_id, const std::vector<CTypeId> &type_args) {
    auto cache_key = std::make_pair(generic_id, type_args);
    auto cit = cache.find(cache_key);
    if (cit != cache.end()) return cit->second;
    return prepare(generic_id, type_args, cache_key);
  }

private:
  SymbolId prepare(SymbolId generic_id, const std::vector<CTypeId> &type_args,
                   const std::pair<SymbolId, std::vector<CTypeId>> &cache_key) {
    const Symbol &gsym = syms.get(generic_id);
    u32 gsym_mod = gsym.module_idx;

    const ModuleContext &gctx = (*module_contexts)[gsym_mod];
    const Module &g_mod = *gctx.mod;
    const std::unordered_map<SymId, u32> *g_import_map = gctx.import_map;

    TypeLowerer g_lowerer(*gctx.type_ast, syms, interner, types);
    g_lowerer.module_idx = gsym_mod;
    g_lowerer.module_contexts = module_contexts;
    g_lowerer.current_ir = gctx.ir;
    g_lowerer.import_map = g_import_map;

    // build substitution: type-param name -> concrete CTypeId.
    // also includes const-generic params (e.g. N) as ConstInt CTypeIds so
    // TypeLowerer::eval_const_int can resolve [N]T and @if(N < 32) conditions.
    std::unordered_map<SymId, CTypeId> subst;
    std::unordered_map<SymId, u32> const_values;
    for (u32 k = 0; k < gsym.generics_count && k < type_args.size(); ++k) {
      const GenericParam &gp = g_mod.generic_params[gsym.generics_start + k];
      CTypeId ctid = type_args[k];
      subst[gp.name] = ctid;
      if (!gp.is_type) {
        // const-generic: extract integer value for mono_const_values
        if (ctid < static_cast<u32>(types.types.size()) &&
            types.types[ctid].kind == CTypeKind::ConstInt)
          const_values[gp.name] = types.types[ctid].count;
      }
    }

    // clone symbol without generic params
    Symbol mono_sym = gsym;
    mono_sym.generics_start = 0;
    mono_sym.generics_count = 0;
    // keep impl_owner so name mangling still includes the type name prefix.
    // store the substitution for codegen (@size_of/@align_of in mono bodies).
    mono_sym.mono_type_subst = subst;
    mono_sym.mono_const_values = const_values;

    // pre-lower concrete ret/param types using the substitution so codegen can
    // declare the LLVM function type without needing the substitution later.
    {
      TypeLowerer ml = g_lowerer;
      ml.type_subst = subst;
      ml.lenient = true;
      mono_sym.is_mono_instance = true;
      if (gsym.sig.ret_type != 0) {
        auto r = ml.lower(gsym.sig.ret_type);
        if (r) mono_sym.mono_concrete_ret = *r;
        // else stays 0 = Void_ctid (lowering failed; codegen will use void)
      } else {
        mono_sym.mono_concrete_ret = types.builtin(CTypeKind::Void); // = 0
      }
      // detect &self / &mut self first param: store its type separately so
      // mono_concrete_params is a 1-to-1 map of explicit call arguments.
      u32 params_start = 0;
      if (gsym.sig.params_count > 0) {
        TypeId fp0_tid = g_mod.params[gsym.sig.params_start].type;
        if (fp0_tid != 0 && g_lowerer.type_ast.kind[fp0_tid] == TypeKind::Ref) {
          auto r = ml.lower(fp0_tid);
          if (r) mono_sym.mono_self_ctype = *r;
          params_start = 1;
        }
      }
      for (u32 k = params_start; k < gsym.sig.params_count; ++k) {
        const FuncParam &fp = g_mod.params[gsym.sig.params_start + k];
        CTypeId ctid = 0;
        if (fp.type != 0) {
          auto r = ml.lower(fp.type);
          if (r) ctid = *r;
        }
        mono_sym.mono_concrete_params.push_back(ctid);
      }
    }

    // append to symbol table (not to by_name; only reachable via cache)
    SymbolId mono_id = static_cast<SymbolId>(syms.symbols.size());
    syms.symbols.push_back(mono_sym);

    // register in cache before body-check (handles mutual recursion)
    cache[cache_key] = mono_id;

    // enqueue for body-checking by the sema orchestrator
    pending.push_back({mono_id, gsym_mod,
                       static_cast<u32>(gsym.generics_start),
                       static_cast<u32>(gsym.generics_count)});
    return mono_id;
  }
};
