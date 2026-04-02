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

// try to extract the concrete CTypeId for a type param from a param/arg pair.
// walks the TypeAst of the param and the CType of the arg in parallel, peeling
// compound wrappers (Ref, Slice, Array) until the target param name is found.
inline CTypeId extract_type_arg(const TypeAst &ta, TypeId param_tid,
                                 CTypeId arg_ctid, SymId target,
                                 const TypeTable &types) {
  if (param_tid == 0 || arg_ctid == 0) return 0;
  TypeKind tk = ta.kind[param_tid];
  if (tk == TypeKind::Named && ta.a[param_tid] == target) return arg_ctid;
  const CType &act = types.types[arg_ctid];
  if (tk == TypeKind::Ref && act.kind == CTypeKind::Ref)
    return extract_type_arg(ta, ta.b[param_tid], act.inner, target, types);
  if (tk == TypeKind::Array) {
    if (act.kind == CTypeKind::Array || act.kind == CTypeKind::Slice)
      return extract_type_arg(ta, ta.b[param_tid], act.inner, target, types);
  }
  return 0;
}

// infer concrete type args for a generic from actual argument ITypes.
// walks param type ASTs to find where type param names appear (including inside
// &T, []T, [N]T), then extracts the corresponding concrete type from the arg.
// callers must flush_int_defaults() first so integer literal args are concrete.
inline std::vector<CTypeId> infer_type_args(
    const Symbol &gsym, const Module &g_mod, const TypeLowerer &g_lowerer,
    Unifier &unifier, const std::vector<IType> &arg_types) {
  std::vector<CTypeId> result;
  for (u32 k = 0; k < gsym.generics_count; ++k) {
    const GenericParam &gp = g_mod.generic_params[gsym.generics_start + k];
    if (!gp.is_type) {
      result.push_back(0);
      continue;
    }
    CTypeId inferred = 0;
    for (u32 p = 0; p < gsym.sig.params_count && !inferred; ++p) {
      if (p >= arg_types.size()) break;
      const FuncParam &fp = g_mod.params[gsym.sig.params_start + p];
      IType at = unifier.resolve(arg_types[p]);
      if (!at.is_var)
        inferred = extract_type_arg(g_lowerer.type_ast, fp.type, at.concrete,
                                     gp.name, g_lowerer.out);
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

    TypeLowerer g_lowerer = make_module_type_lowerer(
        gsym_mod, gctx, syms, interner, types, module_contexts);

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
    mono_sym.flags = mono_sym.flags | SymFlags::MonoInstance;

    // build MonoInfo with pre-lowered concrete types
    auto mi = std::make_shared<MonoInfo>();
    mi->type_subst = subst;
    mi->const_values = const_values;
    {
      TypeLowerer ml = g_lowerer;
      ml.type_subst = subst;
      ml.lenient = true;
      if (gsym.sig.ret_type != 0) {
        auto r = ml.lower(gsym.sig.ret_type);
        if (r) mi->concrete_ret = *r;
      }
      // detect &self first param: store separately so concrete_params is 1-to-1 with call args
      u32 params_start = 0;
      if (gsym.sig.params_count > 0) {
        TypeId fp0_tid = g_mod.params[gsym.sig.params_start].type;
        if (fp0_tid != 0 && g_lowerer.type_ast.kind[fp0_tid] == TypeKind::Ref) {
          auto r = ml.lower(fp0_tid);
          if (r) mi->self_ctype = *r;
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
        mi->concrete_params.push_back(ctid);
      }
    }
    mono_sym.mono = std::move(mi);

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
