#pragma once

#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include <common/error.h>
#include <common/interner.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/module.h>

#include "ctypes.h"
#include "lower_types.h"
#include "method_table.h"
#include "symbol.h"

using TypeVarId = u32;

struct IType {
  bool is_var = false;
  union {
    CTypeId concrete;
    TypeVarId var;
  };

  static IType from(CTypeId id) {
    IType t;
    t.is_var = false;
    t.concrete = id;
    return t;
  }
  static IType fresh(TypeVarId id) {
    IType t;
    t.is_var = true;
    t.var = id;
    return t;
  }
};

struct Unifier {
  std::vector<std::optional<IType>> bindings;

  TypeVarId fresh() {
    TypeVarId id = static_cast<TypeVarId>(bindings.size());
    bindings.push_back(std::nullopt);
    return id;
  }

  IType resolve(IType t) const {
    while (t.is_var && bindings[t.var].has_value()) t = *bindings[t.var];
    return t;
  }

  bool unify(IType a, IType b, const TypeTable &types) {
    a = resolve(a);
    b = resolve(b);
    if (!a.is_var && !b.is_var) return a.concrete == b.concrete;
    if (a.is_var) bindings[a.var] = b;
    else bindings[b.var] = a;
    return true;
  }
};

struct BodySema {
  std::vector<IType> node_type;
  std::vector<SymbolId> node_symbol;

  struct Local {
    IType type;
    bool is_mut;
  };
  std::vector<std::unordered_map<SymId, Local>> scopes;

  void push_scope() { scopes.push_back({}); }
  void pop_scope() { scopes.pop_back(); }

  void define(SymId name, IType type, bool is_mut) {
    scopes.back()[name] = {type, is_mut};
  }

  std::optional<Local> lookup(SymId name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return f->second;
    }
    return std::nullopt;
  }
};

// Cache of monomorphized instances: (generic SymbolId, type args) -> mono
// SymbolId
using MonoCache = std::map<std::pair<SymbolId, std::vector<CTypeId>>, SymbolId>;

struct BodyChecker {
  const BodyIR &ir;
  const Module &mod;
  SymbolTable &syms;    // mutable: mono instances are appended
  MethodTable &methods; // mutable: mono methods may be registered
  TypeTable &types;
  TypeLowerer lowerer; // owned copy — type_subst may differ per function
  const Interner &interner;
  std::string_view src;
  Unifier unifier;
  std::vector<Error> errors;
  MonoCache &mono_cache; // shared across all BodyCheckers for one compilation
  // When set, monomorphize() stores body semas for mono instances here.
  std::unordered_map<SymbolId, BodySema> *body_semas_out = nullptr;
  // Set by monomorphize() to inject const generic params into expression scope
  u32 const_generic_scope_start = 0;
  u32 const_generic_scope_count = 0;

  // Multi-module support: set these after construction when checking a
  // specific module's functions.
  u32 module_idx = 0;
  // Alias SymId → index in the loaded-modules vector.
  const std::unordered_map<SymId, u32> *import_map = nullptr;
  // Per-module context vector (unifies the old 5 dep_* parallel arrays).
  const std::vector<ModuleContext> *module_contexts = nullptr;
  // Integer literal TypeVars registered for lazy defaulting at end of check().
  // Stores (TypeVar, NodeId) so we can report an error if the TypeVar resolves
  // to a non-numeric type (e.g., an integer literal used as a bool condition).
  std::vector<std::pair<TypeVarId, NodeId>> int_default_vars;

  BodyChecker(const BodyIR &ir, const Module &mod, SymbolTable &syms,
              MethodTable &methods, TypeTable &types, TypeLowerer lowerer,
              const Interner &interner, std::string_view src,
              MonoCache &mono_cache)
      : ir(ir), mod(mod), syms(syms), methods(methods), types(types),
        lowerer(std::move(lowerer)), interner(interner), src(src),
        mono_cache(mono_cache) {}

  Span node_span(NodeId n) const {
    return {ir.nodes.span_s[n], ir.nodes.span_e[n]};
  }
  void emit(Span sp, const char *msg) { errors.push_back(Error{sp, msg, module_idx}); }

  // Lower a TypeId using the correct TypeAst for the symbol's module.
  // Falls back to the current module's lowerer when no dep info is available.
  std::optional<CTypeId> lower_for_sym(const Symbol &sym, TypeId tid) {
    if (sym.module_idx == module_idx) {
      auto r = lowerer.lower(tid);
      return r ? std::optional<CTypeId>(*r) : std::nullopt;
    }
    if (module_contexts && sym.module_idx < module_contexts->size()) {
      const ModuleContext &mctx = (*module_contexts)[sym.module_idx];
      if (mctx.type_ast) {
        TypeLowerer dep_l(*mctx.type_ast, syms, interner, types);
        dep_l.module_idx = sym.module_idx;
        dep_l.module_contexts = module_contexts;
        dep_l.current_ir = mctx.ir;
        dep_l.import_map = mctx.import_map;
        auto r = dep_l.lower(tid);
        return r ? std::optional<CTypeId>(*r) : std::nullopt;
      }
    }
    return std::nullopt;
  }

  // Return the BodyIR for the given module index (falls back to current ir).
  const BodyIR &body_for(u32 mod_idx) const {
    if (mod_idx == module_idx) return ir;
    if (module_contexts && mod_idx < module_contexts->size() &&
        (*module_contexts)[mod_idx].ir)
      return *(*module_contexts)[mod_idx].ir;
    return ir;
  }

  // ── Monomorphization ──────────────────────────────────────────────────────

  // Bind all still-unresolved integer literal TypeVars to i32.
  // Call before infer_type_args so that integer literal args resolve naturally.
  // TypeVars already bound by context (e.g., u64 function param) are unaffected.
  void flush_int_defaults() {
    IType i32_t = IType::from(types.builtin(CTypeKind::I32));
    for (auto &[tv, _] : int_default_vars)
      if (unifier.resolve(IType::fresh(tv)).is_var)
        unifier.bindings[tv] = i32_t;
  }

  // Infer concrete type args for a generic from actual argument ITypes.
  // For each type-param T, find the first param typed as Named(T) and read
  // the resolved concrete type of the corresponding argument.
  // Callers must flush_int_defaults() first so integer literal args are concrete.
  std::vector<CTypeId> infer_type_args(const Symbol &gsym,
                                       const std::vector<IType> &arg_types) {
    std::vector<CTypeId> result;
    for (u32 k = 0; k < gsym.generics_count; ++k) {
      const GenericParam &gp = mod.generic_params[gsym.generics_start + k];
      if (!gp.is_type) {
        result.push_back(0);
        continue;
      }
      CTypeId inferred = 0;
      for (u32 p = 0; p < gsym.sig.params_count && !inferred; ++p) {
        const FuncParam &fp = mod.params[gsym.sig.params_start + p];
        if (lowerer.type_ast.kind[fp.type] == TypeKind::Named &&
            lowerer.type_ast.a[fp.type] == gp.name && p < arg_types.size()) {
          IType at = unifier.resolve(arg_types[p]);
          if (!at.is_var) inferred = at.concrete;
        }
      }
      result.push_back(inferred);
    }
    return result;
  }

  // Create (or look up) a monomorphized instance of a generic function.
  SymbolId monomorphize(SymbolId generic_id,
                        const std::vector<CTypeId> &type_args) {
    auto cache_key = std::make_pair(generic_id, type_args);
    auto cit = mono_cache.find(cache_key);
    if (cit != mono_cache.end()) return cit->second;

    const Symbol &gsym = syms.get(generic_id);

    // Select the module that owns this generic symbol (may differ from current).
    u32 gsym_mod = gsym.module_idx;
    bool is_cross = gsym_mod != module_idx && module_contexts != nullptr &&
                    gsym_mod < module_contexts->size();
    const ModuleContext *gctx_ptr =
        is_cross ? &(*module_contexts)[gsym_mod] : nullptr;
    const Module &g_mod = gctx_ptr ? *gctx_ptr->mod : mod;
    const BodyIR &g_ir = (gctx_ptr && gctx_ptr->ir) ? *gctx_ptr->ir : ir;
    std::string_view g_src = gctx_ptr ? gctx_ptr->src : src;
    const std::unordered_map<SymId, u32> *g_import_map =
        gctx_ptr ? gctx_ptr->import_map : import_map;
    TypeLowerer g_lowerer = (gctx_ptr && gctx_ptr->type_ast)
        ? TypeLowerer(*gctx_ptr->type_ast, syms, interner, types)
        : lowerer;
    if (gctx_ptr) {
      g_lowerer.module_idx = gsym_mod;
      g_lowerer.module_contexts = module_contexts;
      g_lowerer.current_ir = gctx_ptr->ir;
      g_lowerer.import_map = g_import_map;
    }

    // Build substitution: type-param name -> concrete CTypeId
    std::unordered_map<SymId, CTypeId> subst;
    u32 n_type = 0;
    for (u32 k = 0; k < gsym.generics_count; ++k) {
      const GenericParam &gp = g_mod.generic_params[gsym.generics_start + k];
      if (gp.is_type && n_type < type_args.size())
        subst[gp.name] = type_args[n_type++];
    }

    // Clone symbol without generic params
    Symbol mono_sym = gsym;
    mono_sym.generics_start = 0;
    mono_sym.generics_count = 0;
    // Keep impl_owner so name mangling still includes the type name prefix.
    // Store the substitution for codegen (@size_of/@align_of in mono bodies).
    mono_sym.mono_type_subst = subst;

    // Pre-lower concrete ret/param types using the substitution so codegen can
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
      // Detect &self / &mut self first param: store its type separately so
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

    // Append to symbol table (not to by_name; only reachable via mono_cache)
    SymbolId mono_id = static_cast<SymbolId>(syms.symbols.size());
    syms.symbols.push_back(mono_sym);

    // Register in cache before body-check (handles mutual recursion)
    mono_cache[cache_key] = mono_id;

    // Type-check the monomorphized body with the substitution applied
    TypeLowerer sub_lowerer = g_lowerer;
    sub_lowerer.type_subst = std::move(subst);
    sub_lowerer.lenient =
        true; // allow unknown const-generic names in type position
    BodyChecker sub(g_ir, g_mod, syms, methods, types, std::move(sub_lowerer),
                    interner, g_src, mono_cache);
    sub.body_semas_out = body_semas_out;
    sub.module_idx = gsym_mod;
    sub.import_map = g_import_map;
    sub.module_contexts = module_contexts;

    // Expose const generic params (e.g., N: i32) as i32-typed locals so the
    // body can reference them in expression position (e.g., i < N).
    sub.const_generic_scope_start = static_cast<u32>(gsym.generics_start);
    sub.const_generic_scope_count = static_cast<u32>(gsym.generics_count);

    const Symbol &msym = syms.symbols[mono_id];
    auto body_r = sub.check(msym);
    if (!body_r)
      for (auto &e : sub.errors) errors.push_back(e);
    else if (body_semas_out)
      (*body_semas_out)[mono_id] = std::move(*body_r);

    return mono_id;
  }

  // ── Core checking ─────────────────────────────────────────────────────────

  Result<BodySema> check(const Symbol &fn_sym) {
    BodySema sema;
    u32 node_count = static_cast<u32>(ir.nodes.kind.size());
    sema.node_type.assign(node_count,
                          IType::from(types.builtin(CTypeKind::Void)));
    sema.node_symbol.assign(node_count, kInvalidSymbol);

    // For impl methods, inject self → impl_type into the type substitution.
    // The &self param is typed Ref(Named("self")) in the TypeAst; resolve
    // "self" to the owner struct's CTypeId so lowering doesn't fail on the
    // unknown name.
    if (fn_sym.impl_owner != 0 && fn_sym.sig.params_count > 0) {
      const FuncParam &fp = mod.params[fn_sym.sig.params_start];
      const TypeAst &ta = lowerer.type_ast;
      if (fp.type != 0 && ta.kind[fp.type] == TypeKind::Ref) {
        TypeId inner_tid = ta.b[fp.type];
        if (ta.kind[inner_tid] == TypeKind::Named) {
          SymId self_sym = ta.a[inner_tid]; // SymId for the string "self"
          SymbolId owner_sid =
              syms.lookup(fn_sym.module_idx, fn_sym.impl_owner);
          if (owner_sid != kInvalidSymbol) {
            CType ct;
            ct.kind = CTypeKind::Struct;
            ct.symbol = owner_sid;
            // For mono instances, populate the owner struct's type args so
            // that field access on `self` (e.g. self.alloc : Alloc<T>) can
            // resolve T to its concrete type.
            if (!fn_sym.mono_type_subst.empty()) {
              const Symbol &owner_sym = syms.get(owner_sid);
              if (owner_sym.generics_count > 0) {
                u32 owner_mod_idx = owner_sym.module_idx;
                const Module *owner_mod_ptr =
                    (owner_mod_idx == module_idx)
                        ? &mod
                        : (module_contexts && owner_mod_idx < module_contexts->size()
                               ? (*module_contexts)[owner_mod_idx].mod
                               : nullptr);
                if (owner_mod_ptr) {
                  std::vector<CTypeId> targs;
                  for (u32 j = 0; j < owner_sym.generics_count; ++j) {
                    const GenericParam &gp =
                        owner_mod_ptr->generic_params[owner_sym.generics_start + j];
                    if (gp.is_type) {
                      auto it = fn_sym.mono_type_subst.find(gp.name);
                      targs.push_back(it != fn_sym.mono_type_subst.end()
                                          ? it->second
                                          : types.builtin(CTypeKind::Void));
                    }
                  }
                  if (!targs.empty()) {
                    auto [ls, cnt] =
                        types.push_list(targs.data(), (u32)targs.size());
                    ct.list_start = ls;
                    ct.list_count = cnt;
                  }
                }
              }
            }
            lowerer.type_subst[self_sym] = types.intern(ct);
          }
        }
      }
    }

    IType ret_type;
    if (fn_sym.sig.ret_type != 0) {
      auto r = lowerer.lower(fn_sym.sig.ret_type);
      if (!r) return std::unexpected(r.error());
      ret_type = IType::from(*r);
    } else {
      ret_type = IType::from(types.builtin(CTypeKind::Void));
    }

    sema.push_scope();

    // Inject const generic params (e.g., N: i32) as i32-typed locals so the
    // body can reference them in expression position (e.g., i < N).
    IType i32_type = IType::from(types.builtin(CTypeKind::I32));
    for (u32 k = 0; k < const_generic_scope_count; ++k) {
      const GenericParam &gp =
          mod.generic_params[const_generic_scope_start + k];
      if (!gp.is_type) sema.define(gp.name, i32_type, false);
    }

    for (u32 k = 0; k < fn_sym.sig.params_count; ++k) {
      const FuncParam &p = mod.params[fn_sym.sig.params_start + k];
      if (p.type == 0) continue;
      auto ct_r = lowerer.lower(p.type);
      if (!ct_r) {
        errors.push_back(ct_r.error());
        continue;
      }
      sema.define(p.name, IType::from(*ct_r), false);
    }

    if (fn_sym.body != 0) check_block(fn_sym.body, sema, ret_type);

    sema.pop_scope();
    if (!errors.empty()) return std::unexpected(errors.front());

    // Default any integer literal TypeVars that were never bound through
    // unification context to i32.  Vars bound to a typed context (e.g., u64
    // parameter) are already bound and skipped.  Vars that resolved to a
    // non-numeric type (e.g., bool — integer used as if-condition) are errors.
    {
      auto is_numeric = [&](CTypeKind k) {
        return k >= CTypeKind::I8 && k <= CTypeKind::F64;
      };
      IType i32_t = IType::from(types.builtin(CTypeKind::I32));
      for (auto &[tv, node] : int_default_vars) {
        IType resolved = unifier.resolve(IType::fresh(tv));
        if (resolved.is_var) {
          unifier.bindings[tv] = i32_t;
        } else if (!is_numeric(types.types[resolved.concrete].kind)) {
          emit(node_span(node), "integer literal used in non-numeric context");
        }
      }
    }

    if (!errors.empty()) return std::unexpected(errors.front());

    // Resolve all node_type entries to their concrete form before codegen reads
    // them.  During checking, integer literal TypeVars have been bound through
    // unification or defaulted above; resolving here ensures codegen always
    // sees a concrete CTypeId.
    for (auto &t : sema.node_type) t = unifier.resolve(t);

    return sema;
  }

  IType check_block(NodeId n, BodySema &sema, IType ret_type) {
    u32 ss = ir.nodes.b[n], sc = ir.nodes.c[n];
    sema.push_scope();
    for (u32 k = 0; k < sc; ++k)
      check_stmt(static_cast<NodeId>(ir.nodes.list[ss + k]), sema, ret_type);
    sema.pop_scope();
    return IType::from(types.builtin(CTypeKind::Void));
  }

  void check_stmt(NodeId n, BodySema &sema, IType ret_type) {
    NodeKind nk = ir.nodes.kind[n];
    switch (nk) {
    case NodeKind::ConstStmt:
    case NodeKind::VarStmt: {
      SymId var_name = static_cast<SymId>(ir.nodes.a[n]);
      TypeId ann_type = ir.nodes.b[n];
      NodeId init_expr = ir.nodes.c[n];
      IType var_type;
      if (ann_type != 0) {
        auto r = lowerer.lower(ann_type);
        if (!r) {
          emit(node_span(n), "unknown type in declaration");
          var_type = IType::fresh(unifier.fresh());
        } else var_type = IType::from(*r);
      } else {
        var_type = IType::fresh(unifier.fresh());
      }
      if (init_expr != 0) {
        IType init_t = check_expr(init_expr, sema);
        if (!unifier.unify(var_type, init_t, types))
          emit(node_span(n), "type mismatch in declaration");
      }
      sema.define(var_name, var_type, nk == NodeKind::VarStmt);
      sema.node_type[n] = var_type;
      break;
    }
    case NodeKind::ReturnStmt: {
      NodeId expr = ir.nodes.a[n];
      IType val_t = expr != 0 ? check_expr(expr, sema)
                              : IType::from(types.builtin(CTypeKind::Void));
      if (!unifier.unify(val_t, ret_type, types))
        emit(node_span(n), "return type mismatch");
      break;
    }
    case NodeKind::AssignStmt: {
      IType lt = check_expr(ir.nodes.a[n], sema);
      IType rt = check_expr(ir.nodes.b[n], sema);
      if (!unifier.unify(lt, rt, types))
        emit(node_span(n), "type mismatch in assignment");
      break;
    }
    case NodeKind::IfStmt: {
      NodeId cond = ir.nodes.a[n], then_blk = ir.nodes.b[n],
             else_blk = ir.nodes.c[n];
      IType cond_t = check_expr(cond, sema);
      if (!unifier.unify(cond_t, IType::from(types.builtin(CTypeKind::Bool)),
                         types))
        emit(node_span(cond), "condition must be bool");
      if (then_blk != 0) check_block(then_blk, sema, ret_type);
      if (else_blk != 0) {
        if (ir.nodes.kind[else_blk] == NodeKind::Block)
          check_block(else_blk, sema, ret_type);
        else check_stmt(else_blk, sema, ret_type);
      }
      break;
    }
    case NodeKind::ForStmt: {
      const ForPayload &fp = ir.fors[ir.nodes.a[n]];
      sema.push_scope();
      if (fp.init != 0) check_stmt(fp.init, sema, ret_type);
      if (fp.cond != 0) {
        IType ct = check_expr(fp.cond, sema);
        if (!unifier.unify(ct, IType::from(types.builtin(CTypeKind::Bool)),
                           types))
          emit(node_span(fp.cond), "for condition must be bool");
      }
      if (fp.step != 0) check_stmt(fp.step, sema, ret_type);
      if (fp.body != 0) check_block(fp.body, sema, ret_type);
      sema.pop_scope();
      break;
    }
    case NodeKind::ExprStmt: check_expr(ir.nodes.a[n], sema); break;
    case NodeKind::Block: check_block(n, sema, ret_type); break;
    default: check_expr(n, sema); break;
    }
  }

  IType check_expr(NodeId n, BodySema &sema) {
    NodeKind nk = ir.nodes.kind[n];
    IType result = IType::from(types.builtin(CTypeKind::Void));

    switch (nk) {
    case NodeKind::IntLit: {
      TypeVarId tv = unifier.fresh();
      int_default_vars.push_back({tv, n}); // default to i32 at end of check() if unresolved
      result = IType::fresh(tv);
      break;
    }
    case NodeKind::StrLit: {
      CType sc; sc.kind = CTypeKind::Slice; sc.inner = types.builtin(CTypeKind::U8);
      result = IType::from(types.intern(sc));
      break;
    }
    case NodeKind::BoolLit:
      result = IType::from(types.builtin(CTypeKind::Bool));
      break;
    case NodeKind::Ident: {
      SymId name = static_cast<SymId>(ir.nodes.a[n]);
      if (auto loc = sema.lookup(name)) {
        result = loc->type;
      } else {
        SymbolId sid = syms.lookup(module_idx, name);
        if (sid == kInvalidSymbol) {
          emit(node_span(n), "unknown identifier");
        } else {
          sema.node_symbol[n] = sid;
          const Symbol &sym = syms.get(sid);
          if (sym.kind == SymbolKind::Func && sym.generics_count == 0 &&
              sym.module_idx == module_idx) {
            // Build the concrete fn(...)->ret CType so functions can be used
            // as first-class values (stored in variables, passed as args).
            std::vector<CTypeId> fn_list;
            CTypeId ret_ctid = types.builtin(CTypeKind::Void);
            if (sym.sig.ret_type != 0) {
              auto r = lowerer.lower(sym.sig.ret_type);
              if (r) ret_ctid = *r;
            }
            fn_list.push_back(ret_ctid);
            for (u32 k = 0; k < sym.sig.params_count; ++k) {
              const FuncParam &fp = mod.params[sym.sig.params_start + k];
              auto p = lowerer.lower(fp.type);
              fn_list.push_back(p ? *p : types.builtin(CTypeKind::Void));
            }
            auto [ls, cnt] = types.push_list(fn_list.data(), fn_list.size());
            CType fct;
            fct.kind = CTypeKind::Fn;
            fct.list_start = ls;
            fct.list_count = cnt;
            result = IType::from(types.intern(fct));
          } else if (sym.kind == SymbolKind::Func && sym.sig.ret_type != 0) {
            // Fallback for generic or cross-module functions: return the
            // return type (sufficient for direct calls, not for value use).
            auto ct = lower_for_sym(sym, sym.sig.ret_type);
            if (ct) result = IType::from(*ct);
          } else if (sym.kind == SymbolKind::GlobalVar &&
                     sym.annotate_type != 0) {
            auto ct = lower_for_sym(sym, sym.annotate_type);
            if (ct) result = IType::from(*ct);
          }
        }
      }
      break;
    }
    case NodeKind::Unary: result = check_expr(ir.nodes.b[n], sema); break;
    case NodeKind::Binary: {
      IType lt = check_expr(ir.nodes.b[n], sema);
      IType rt = check_expr(ir.nodes.c[n], sema);
      if (!unifier.unify(lt, rt, types))
        emit(node_span(n), "type mismatch in binary expression");
      auto op = static_cast<TokenKind>(ir.nodes.a[n]);
      bool is_cmp = op == TokenKind::EqualEqual || op == TokenKind::BangEqual ||
                    op == TokenKind::Less || op == TokenKind::LessEqual ||
                    op == TokenKind::Greater || op == TokenKind::GreaterEqual ||
                    op == TokenKind::PipePipe || op == TokenKind::AmpAmp;
      result = is_cmp ? IType::from(types.builtin(CTypeKind::Bool)) : lt;
      break;
    }
    case NodeKind::Call: {
      NodeId callee_n = ir.nodes.a[n];
      u32 args_start = ir.nodes.b[n];
      u32 args_count = ir.nodes.c[n];

      check_expr(callee_n, sema);

      std::vector<IType> arg_types;
      for (u32 k = 0; k < args_count; ++k)
        arg_types.push_back(check_expr(
            static_cast<NodeId>(ir.nodes.list[args_start + k]), sema));

      SymbolId callee_sym = sema.node_symbol[callee_n];
      if (callee_sym != kInvalidSymbol) {
        const Symbol &sym = syms.get(callee_sym);
        SymbolId resolved = callee_sym;
        if (sym.generics_count > 0) {
          std::vector<CTypeId> type_args;

          // For method calls (callee is a Field node), recover type args from
          // the receiver's CTypeId (e.g., List<i32,5> carries [i32, 5]).
          if (ir.nodes.kind[callee_n] == NodeKind::Field) {
            NodeId base_n = ir.nodes.a[callee_n];
            IType recv = unifier.resolve(sema.node_type[base_n]);
            if (!recv.is_var) {
              const CType &rct = types.types[recv.concrete];
              for (u32 k = 0; k < rct.list_count; ++k)
                type_args.push_back(types.list[rct.list_start + k]);
            }
          }

          // Explicit type args from a generic path callee: Option<T>::create()
          if (type_args.empty() && ir.nodes.kind[callee_n] == NodeKind::Path) {
            TypeId path_tid = static_cast<TypeId>(ir.nodes.a[callee_n]);
            if (path_tid != 0) {
              auto ct_r = lowerer.lower(path_tid);
              // If lowering returned a non-Void type, extract type args from the
              // CType's list.  If it returned Void (lenient fallback when the type
              // isn't in the current module's namespace, e.g. mod::Alloc<T>), fall
              // through to the direct TypeAst extraction below.
              if (ct_r && *ct_r != 0) {
                const CType &pct = types.types[*ct_r];
                for (u32 k = 0; k < pct.list_count; ++k)
                  type_args.push_back(types.list[pct.list_start + k]);
              } else if (lowerer.type_ast.kind[path_tid] == TypeKind::Named) {
                // Type not resolvable locally (e.g., mod::Alloc<i32>::create
                // where Alloc lives in dep module) — lower type args directly.
                u32 tls = lowerer.type_ast.b[path_tid];
                u32 tcnt = lowerer.type_ast.c[path_tid];
                for (u32 k = 0; k < tcnt; ++k) {
                  auto a = lowerer.lower(lowerer.type_ast.list[tls + k]);
                  if (a) type_args.push_back(*a);
                }
              }
            }
          }

          // Fall back to inference from argument types (for generic free
          // functions). Flush integer literal defaults first so that e.g.
          // `id(1)` has a concrete i32 arg for T inference.
          if (type_args.empty()) {
            flush_int_defaults();
            type_args = infer_type_args(sym, arg_types);
          }

          resolved = monomorphize(callee_sym, type_args);
          sema.node_symbol[callee_n] = resolved;

          // Use the pre-lowered return type from the mono instance (computed
          // with the correct dep-module TypeLowerer + substitution).
          const Symbol &msym = syms.get(resolved);
          if (msym.mono_concrete_ret != 0)
            result = IType::from(msym.mono_concrete_ret);

          // Unify explicit arg types against concrete param types so integer
          // literal TypeVars bind to the expected width (e.g., u64 vs i32).
          // mono_concrete_params excludes self, so this is a direct 1-to-1 map.
          for (u32 k = 0; k < args_count; ++k) {
            if (k >= (u32)msym.mono_concrete_params.size()) break;
            CTypeId expected = msym.mono_concrete_params[k];
            if (expected == 0) continue;
            unifier.unify(arg_types[k], IType::from(expected), types);
          }
        } else {
          const Symbol &rsym = syms.get(resolved);
          if (rsym.kind == SymbolKind::Func && rsym.sig.ret_type != 0) {
            auto ct = lower_for_sym(rsym, rsym.sig.ret_type);
            if (ct) result = IType::from(*ct);
          }
          // Unify arg types against param types so integer literal TypeVars bind
          // to the expected width (e.g., u64 instead of i32).
          if (rsym.kind == SymbolKind::Func && rsym.sig.params_count > 0) {
            u32 rsym_mod = rsym.module_idx;
            const Module *pm = (rsym_mod == module_idx) ? &mod
                : (module_contexts && rsym_mod < module_contexts->size()
                       ? (*module_contexts)[rsym_mod].mod
                       : nullptr);
            if (pm) {
              // Field callees are instance method calls: skip the implicit self.
              // Path callees are static method or free function calls: no skip.
              u32 rsym_skip =
                  (ir.nodes.kind[callee_n] == NodeKind::Field) ? 1u : 0u;
              for (u32 k = 0; k < args_count &&
                   (k + rsym_skip) < rsym.sig.params_count; ++k) {
                TypeId pt =
                    pm->params[rsym.sig.params_start + k + rsym_skip].type;
                if (pt == 0) continue;
                auto ct = lower_for_sym(rsym, pt);
                if (!ct) continue;
                unifier.unify(arg_types[k], IType::from(*ct), types);
              }
            }
          }
        }
      } else {
        // Function pointer call: infer return type from the callee's fn CType.
        IType callee_t = unifier.resolve(sema.node_type[callee_n]);
        if (!callee_t.is_var) {
          const CType &ct = types.types[callee_t.concrete];
          if (ct.kind == CTypeKind::Fn && ct.list_count > 0)
            result = IType::from(types.list[ct.list_start]); // list[0] = ret type
        }
      }
      break;
    }
    case NodeKind::Field: {
      NodeId base_n = ir.nodes.a[n];
      SymId field_nm = static_cast<SymId>(ir.nodes.b[n]);
      IType base_t = check_expr(base_n, sema);
      IType base_c = unifier.resolve(base_t);

      if (!base_c.is_var) {
        // Peel Ref so &self.field and self.field both work
        CTypeId sct_id = base_c.concrete;
        if (types.types[sct_id].kind == CTypeKind::Ref)
          sct_id = types.types[sct_id].inner;
        const CType &sct = types.types[sct_id];

        if (sct.kind == CTypeKind::Slice) {
          auto fname = interner.view(field_nm);
          if (fname == "ptr") {
            CType rc; rc.kind = CTypeKind::Ref; rc.inner = sct.inner;
            result = IType::from(types.intern(rc));
          } else if (fname == "len") {
            result = IType::from(types.builtin(CTypeKind::U64));
          }
          break;
        }
        if (sct.kind == CTypeKind::Struct || sct.kind == CTypeKind::Enum) {
          // 1. Check method table first
          SymbolId msym_id = methods.lookup(sct.symbol, field_nm);
          if (msym_id != kInvalidSymbol) {
            sema.node_symbol[n] = msym_id;
            const Symbol &msym = syms.get(msym_id);
            if (msym.generics_count == 0 && msym.sig.ret_type != 0) {
              auto r = lowerer.lower(msym.sig.ret_type);
              if (r) result = IType::from(*r);
            } else {
              result = IType::fresh(unifier.fresh()); // resolved at call site
            }
          } else {
            // 2. Struct field lookup: StructType node stores [SymId, TypeId]
            // pairs
            const Symbol &tsym = syms.get(sct.symbol);
            const BodyIR &tsym_ir = body_for(tsym.module_idx);
            if (tsym.type_node != 0 &&
                tsym_ir.nodes.kind[tsym.type_node] == NodeKind::StructType) {
              u32 fs = tsym_ir.nodes.b[tsym.type_node];
              u32 fc = tsym_ir.nodes.c[tsym.type_node];
              for (u32 k = 0; k < fc; ++k) {
                SymId fname =
                    static_cast<SymId>(tsym_ir.nodes.list[fs + k * 2]);
                TypeId ftype =
                    static_cast<TypeId>(tsym_ir.nodes.list[fs + k * 2 + 1]);
                if (fname == field_nm) {
                  // Build a lowerer for the struct's module (ftype is in its
                  // TypeAst). Use optional + emplace to conditionally construct
                  // a dep lowerer without assignment (TypeLowerer has refs).
                  u32 tsym_mod = tsym.module_idx;
                  std::optional<TypeLowerer> dep_fl;
                  if (tsym_mod != module_idx && module_contexts &&
                      tsym_mod < module_contexts->size()) {
                    const ModuleContext &mctx = (*module_contexts)[tsym_mod];
                    if (mctx.type_ast) {
                      dep_fl.emplace(*mctx.type_ast, syms, interner, types);
                      dep_fl->module_idx = tsym_mod;
                      dep_fl->module_contexts = module_contexts;
                      dep_fl->current_ir = mctx.ir;
                      dep_fl->import_map = mctx.import_map;
                    }
                  }
                  TypeLowerer &fl = dep_fl ? *dep_fl : lowerer;
                  if (sct.list_count > 0) {
                    // Use the struct's own module for generic_params lookup.
                    const Module *tsym_mod_ptr =
                        (tsym_mod == module_idx)
                            ? &mod
                            : (module_contexts && tsym_mod < module_contexts->size()
                                   ? (*module_contexts)[tsym_mod].mod
                                   : nullptr);
                    if (tsym_mod_ptr) {
                      for (u32 j = 0;
                           j < tsym.generics_count && j < sct.list_count; ++j) {
                        const GenericParam &gp =
                            tsym_mod_ptr->generic_params[tsym.generics_start + j];
                        if (gp.is_type)
                          fl.type_subst[gp.name] = types.list[sct.list_start + j];
                      }
                    }
                  }
                  auto r = fl.lower(ftype);
                  if (r) result = IType::from(*r);
                  break;
                }
              }
            }
          }
        }
      }
      if (result.is_var && sema.node_symbol[n] == kInvalidSymbol)
        result = IType::fresh(unifier.fresh());
      break;
    }
    case NodeKind::Index: {
      IType base_t = check_expr(ir.nodes.a[n], sema);
      check_expr(ir.nodes.b[n], sema);
      IType base_r = unifier.resolve(base_t);
      if (!base_r.is_var) {
        const CType &base_ct = types.types[base_r.concrete];
        CTypeId actual_ctid = base_ct.kind == CTypeKind::Ref ? base_ct.inner
                                                              : base_r.concrete;
        const CType &actual_ct = types.types[actual_ctid];
        if (actual_ct.kind == CTypeKind::Array ||
            actual_ct.kind == CTypeKind::Slice)
          result = IType::from(actual_ct.inner);
        else
          result = IType::fresh(unifier.fresh());
      } else {
        result = IType::fresh(unifier.fresh());
      }
      break;
    }
    case NodeKind::AddrOf: {
      bool is_mut_ref = ir.nodes.a[n] != 0;
      IType inner_t = check_expr(ir.nodes.b[n], sema);
      IType inner_c = unifier.resolve(inner_t);
      CType ct;
      ct.kind = CTypeKind::Ref;
      ct.is_mut = is_mut_ref;
      if (!inner_c.is_var) ct.inner = inner_c.concrete;
      result = IType::from(types.intern(ct));
      break;
    }
    case NodeKind::Deref: {
      IType inner_t = check_expr(ir.nodes.a[n], sema);
      IType inner_c = unifier.resolve(inner_t);
      if (!inner_c.is_var && types.types[inner_c.concrete].kind == CTypeKind::Ref) {
        // Peel the reference: *(&mut T) → T
        CTypeId inner_ctid = types.types[inner_c.concrete].inner;
        result = IType::from(inner_ctid);
      } else {
        result = IType::fresh(unifier.fresh());
      }
      break;
    }
    case NodeKind::ArrayLit: {
      const ArrayLitPayload &al = ir.array_lits[ir.nodes.a[n]];
      CTypeId elem_ctid = 0;
      if (al.elem_type != 0) {
        auto r = lowerer.lower(al.elem_type);
        if (r) elem_ctid = *r;
      }
      // Check element expressions and unify with declared elem type.
      for (u32 k = 0; k < al.values_count; ++k) {
        IType et = check_expr(static_cast<NodeId>(ir.nodes.list[al.values_start + k]), sema);
        if (elem_ctid != 0) unifier.unify(et, IType::from(elem_ctid), types);
      }
      CType ct;
      ct.kind = CTypeKind::Array;
      ct.count = al.explicit_count;
      ct.inner = elem_ctid;
      result = IType::from(types.intern(ct));
      break;
    }
    case NodeKind::TupleLit: {
      u32 ls = ir.nodes.b[n], cnt = ir.nodes.c[n];
      std::vector<CTypeId> elems;
      for (u32 k = 0; k < cnt; ++k) {
        IType et = check_expr(static_cast<NodeId>(ir.nodes.list[ls + k]), sema);
        IType ec = unifier.resolve(et);
        elems.push_back(ec.is_var ? 0 : ec.concrete);
      }
      auto [start, count] = types.push_list(elems.data(), elems.size());
      CType ct;
      ct.kind = CTypeKind::Tuple;
      ct.list_start = start;
      ct.list_count = count;
      result = IType::from(types.intern(ct));
      break;
    }
    case NodeKind::StructInit: {
      // a = TypeId (Named type, possibly with generic args) into type_ast
      TypeId struct_tid = static_cast<TypeId>(ir.nodes.a[n]);
      if (struct_tid != 0) {
        auto ct_r = lowerer.lower(struct_tid);
        if (!ct_r) {
          emit(node_span(n), "unknown struct type");
        } else {
          const CType &ct = types.types[*ct_r];
          if (ct.kind == CTypeKind::Struct || ct.kind == CTypeKind::Enum)
            sema.node_symbol[n] = ct.symbol;
          result = IType::from(*ct_r);
        }
      }
      u32 fs = ir.nodes.b[n], fc = ir.nodes.c[n];
      for (u32 k = 0; k < fc; ++k)
        check_expr(static_cast<NodeId>(ir.nodes.list[fs + k * 2 + 1]), sema);
      break;
    }
    case NodeKind::Path: {
      // Default: unknown type (enum variant, module path, or unresolved).
      result = IType::fresh(unifier.fresh());
      u32 ls = ir.nodes.b[n], cnt = ir.nodes.c[n];
      if (cnt >= 2) {
        SymId first_seg = static_cast<SymId>(ir.nodes.list[ls]);
        SymId second_seg = static_cast<SymId>(ir.nodes.list[ls + 1]);
        SymbolId first_sid = syms.lookup(module_idx, first_seg);

        if (first_sid != kInvalidSymbol &&
            syms.get(first_sid).kind == SymbolKind::Type) {
          // Type::method — static method dispatch
          SymbolId method_sid = methods.lookup(first_sid, second_seg);
          if (method_sid != kInvalidSymbol) {
            sema.node_symbol[n] = method_sid;
            const Symbol &msym = syms.get(method_sid);
            if (msym.generics_count == 0 && msym.sig.ret_type != 0) {
              auto r = lowerer.lower(msym.sig.ret_type);
              if (r) result = IType::from(*r);
            }
            // else: generic method — result stays fresh, resolved at call site
          } else {
            // Enum variant: Type::Variant where method not found.
            // Return the enum's concrete CType so downstream expressions
            // know the type of the variant (e.g. Direction::North : Direction).
            const Symbol &type_sym = syms.get(first_sid);
            if (type_sym.type_node != 0 &&
                ir.nodes.kind[type_sym.type_node] == NodeKind::EnumType) {
              CType ct;
              ct.kind = CTypeKind::Enum;
              ct.symbol = first_sid;
              result = IType::from(types.intern(ct));
            }
          }
        } else if (import_map) {
          // module::symbol — cross-module function/type lookup
          auto mit = import_map->find(first_seg);
          if (mit != import_map->end()) {
            u32 dep_mod_idx = mit->second;
            SymbolId dep_sid = syms.lookup_pub(dep_mod_idx, second_seg);
            if (dep_sid == kInvalidSymbol &&
                syms.lookup(dep_mod_idx, second_seg) != kInvalidSymbol) {
              emit(node_span(n), "symbol is not exported from module");
            }
            if (dep_sid != kInvalidSymbol) {
              sema.node_symbol[n] = dep_sid;
              const Symbol &dep_sym = syms.get(dep_sid);
              if (dep_sym.kind == SymbolKind::Func &&
                  dep_sym.generics_count == 0 && dep_sym.sig.ret_type != 0 &&
                  module_contexts && dep_mod_idx < module_contexts->size()) {
                const ModuleContext &mctx = (*module_contexts)[dep_mod_idx];
                if (mctx.type_ast) {
                  TypeLowerer dep_lowerer(*mctx.type_ast, syms, interner, types);
                  dep_lowerer.module_idx = dep_mod_idx;
                  dep_lowerer.module_contexts = module_contexts;
                  dep_lowerer.current_ir = mctx.ir;
                  dep_lowerer.import_map = mctx.import_map;
                  auto r = dep_lowerer.lower(dep_sym.sig.ret_type);
                  if (r) result = IType::from(*r);
                }
              } else if (dep_sym.kind == SymbolKind::Type && cnt >= 3) {
                // module::Type::method — cross-module static method call
                SymId third_seg =
                    static_cast<SymId>(ir.nodes.list[ls + 2]);
                SymbolId method_sid = methods.lookup(dep_sid, third_seg);
                if (method_sid != kInvalidSymbol) {
                  sema.node_symbol[n] = method_sid;
                  // result type resolved later in Call handling via
                  // monomorphize() with type args from generic_type_id
                }
              }
            }
          }
        }
      }
      break;
    }
    case NodeKind::FnLit: result = IType::fresh(unifier.fresh()); break;
    case NodeKind::CastAs:
    case NodeKind::Bitcast: {
      check_expr(ir.nodes.a[n], sema);
      TypeId target_tid = ir.nodes.b[n];
      auto ct_r = lowerer.lower(target_tid);
      if (!ct_r) { emit(node_span(n), "unknown type in cast"); break; }
      result = IType::from(*ct_r);
      break;
    }
    case NodeKind::SliceLit: {
      TypeId elem_tid = ir.nodes.a[n];
      auto elem_r = lowerer.lower(elem_tid);
      if (!elem_r) { emit(node_span(n), "unknown element type in slice literal"); break; }
      for (u32 k = 0; k < ir.nodes.c[n]; ++k)
        check_expr(static_cast<NodeId>(ir.nodes.list[ir.nodes.b[n] + k]), sema);
      CType sc; sc.kind = CTypeKind::Slice; sc.inner = *elem_r;
      result = IType::from(types.intern(sc));
      break;
    }
    case NodeKind::SiteId:
      result = IType::from(types.builtin(CTypeKind::U32));
      break;
    case NodeKind::SizeOf:
    case NodeKind::AlignOf:
      // type argument (a = TypeId) — return u64; actual value computed at codegen
      result = IType::from(types.builtin(CTypeKind::U64));
      break;
    case NodeKind::SliceCast: {
      check_expr(ir.nodes.a[n], sema); // source slice
      TypeId elem_tid = ir.nodes.b[n];
      auto elem_r = lowerer.lower(elem_tid);
      if (!elem_r) { emit(node_span(n), "unknown element type in @slice_cast"); break; }
      CType sc; sc.kind = CTypeKind::Slice; sc.inner = *elem_r;
      result = IType::from(types.intern(sc));
      break;
    }
    default: break;
    }

    sema.node_type[n] = result;
    return result;
  }
};
