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
    IType t; t.is_var = false; t.concrete = id; return t;
  }
  static IType fresh(TypeVarId id) {
    IType t; t.is_var = true; t.var = id; return t;
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
    a = resolve(a); b = resolve(b);
    if (!a.is_var && !b.is_var) return a.concrete == b.concrete;
    if (a.is_var) bindings[a.var] = b;
    else          bindings[b.var] = a;
    return true;
  }
};

struct BodySema {
  std::vector<IType>     node_type;
  std::vector<SymbolId>  node_symbol;

  struct Local { IType type; bool is_mut; };
  std::vector<std::unordered_map<SymId, Local>> scopes;

  void push_scope() { scopes.push_back({}); }
  void pop_scope()  { scopes.pop_back(); }

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

// Cache of monomorphized instances: (generic SymbolId, type args) -> mono SymbolId
using MonoCache = std::map<std::pair<SymbolId, std::vector<CTypeId>>, SymbolId>;

struct BodyChecker {
  const BodyIR   &ir;
  const Module   &mod;
  SymbolTable    &syms;    // mutable: mono instances are appended
  MethodTable    &methods; // mutable: mono methods may be registered
  TypeTable      &types;
  TypeLowerer     lowerer; // owned copy — type_subst may differ per function
  const Interner &interner;
  std::string_view src;
  Unifier          unifier;
  std::vector<Error> errors;
  MonoCache        &mono_cache; // shared across all BodyCheckers for one compilation
  // Set by monomorphize() to inject const generic params into expression scope
  u32 const_generic_scope_start = 0;
  u32 const_generic_scope_count = 0;

  // Multi-module support: set these after construction when checking a
  // specific module's functions.
  u32 module_idx = 0;
  // Alias SymId → index in the loaded-modules vector.
  const std::unordered_map<SymId, u32> *import_map = nullptr;
  // Per-loaded-module TypeAst pointers for cross-module type lowering.
  const std::vector<const TypeAst *> *dep_type_asts = nullptr;
  // Per-loaded-module BodyIR pointers for cross-module struct field lookup.
  const std::vector<const BodyIR *> *dep_irs = nullptr;

  BodyChecker(const BodyIR &ir, const Module &mod, SymbolTable &syms,
              MethodTable &methods, TypeTable &types, TypeLowerer lowerer,
              const Interner &interner, std::string_view src,
              MonoCache &mono_cache)
      : ir(ir), mod(mod), syms(syms), methods(methods), types(types),
        lowerer(std::move(lowerer)), interner(interner), src(src),
        mono_cache(mono_cache) {}

  Span node_span(NodeId n) const { return {ir.nodes.span_s[n], ir.nodes.span_e[n]}; }
  void emit(Span sp, const char *msg) { errors.push_back(Error{sp, msg}); }

  // Lower a TypeId using the correct TypeAst for the symbol's module.
  // Falls back to the current module's lowerer when no dep info is available.
  std::optional<CTypeId> lower_for_sym(const Symbol &sym, TypeId tid) {
    if (sym.module_idx == module_idx) {
      auto r = lowerer.lower(tid);
      return r ? std::optional<CTypeId>(*r) : std::nullopt;
    }
    if (dep_type_asts && sym.module_idx < dep_type_asts->size() &&
        (*dep_type_asts)[sym.module_idx] != nullptr) {
      TypeLowerer dep_l(*(*dep_type_asts)[sym.module_idx], syms, interner, types);
      dep_l.module_idx = sym.module_idx;
      auto r = dep_l.lower(tid);
      return r ? std::optional<CTypeId>(*r) : std::nullopt;
    }
    return std::nullopt;
  }

  // Return the BodyIR for the given module index (falls back to current ir).
  const BodyIR &body_for(u32 mod_idx) const {
    if (mod_idx == module_idx) return ir;
    if (dep_irs && mod_idx < dep_irs->size() && (*dep_irs)[mod_idx])
      return *(*dep_irs)[mod_idx];
    return ir;
  }

  // ── Monomorphization ──────────────────────────────────────────────────────

  // Infer concrete type args for a generic from actual argument ITypes.
  // For each type-param T, find the first param typed as Named(T) and read
  // the resolved concrete type of the corresponding argument.
  std::vector<CTypeId> infer_type_args(const Symbol &gsym,
                                        const std::vector<IType> &arg_types) {
    std::vector<CTypeId> result;
    for (u32 k = 0; k < gsym.generics_count; ++k) {
      const GenericParam &gp = mod.generic_params[gsym.generics_start + k];
      if (!gp.is_type) { result.push_back(0); continue; }
      CTypeId inferred = 0;
      for (u32 p = 0; p < gsym.sig.params_count && !inferred; ++p) {
        const FuncParam &fp = mod.params[gsym.sig.params_start + p];
        if (lowerer.type_ast.kind[fp.type] == TypeKind::Named &&
            lowerer.type_ast.a[fp.type] == gp.name &&
            p < arg_types.size()) {
          IType at = unifier.resolve(arg_types[p]);
          if (!at.is_var) inferred = at.concrete;
        }
      }
      result.push_back(inferred);
    }
    return result;
  }

  // Create (or look up) a monomorphized instance of a generic function.
  SymbolId monomorphize(SymbolId generic_id, const std::vector<CTypeId> &type_args) {
    auto cache_key = std::make_pair(generic_id, type_args);
    auto cit = mono_cache.find(cache_key);
    if (cit != mono_cache.end()) return cit->second;

    const Symbol &gsym = syms.get(generic_id);

    // Build substitution: type-param name -> concrete CTypeId
    std::unordered_map<SymId, CTypeId> subst;
    u32 n_type = 0;
    for (u32 k = 0; k < gsym.generics_count; ++k) {
      const GenericParam &gp = mod.generic_params[gsym.generics_start + k];
      if (gp.is_type && n_type < type_args.size())
        subst[gp.name] = type_args[n_type++];
    }

    // Clone symbol without generic params
    Symbol mono_sym = gsym;
    mono_sym.generics_start = 0;
    mono_sym.generics_count = 0;
    mono_sym.impl_owner = 0;

    // Append to symbol table (not to by_name; only reachable via mono_cache)
    SymbolId mono_id = static_cast<SymbolId>(syms.symbols.size());
    syms.symbols.push_back(mono_sym);

    // Register in cache before body-check (handles mutual recursion)
    mono_cache[cache_key] = mono_id;

    // Type-check the monomorphized body with the substitution applied
    TypeLowerer sub_lowerer = lowerer; // copy; type_subst already holds our subst
    sub_lowerer.type_subst = std::move(subst);
    BodyChecker sub(ir, mod, syms, methods, types, std::move(sub_lowerer), interner,
                    src, mono_cache);

    // Expose const generic params (e.g., N: i32) as i32-typed locals so the
    // body can reference them in expression position (e.g., i < N).
    sub.const_generic_scope_start = static_cast<u32>(gsym.generics_start);
    sub.const_generic_scope_count = static_cast<u32>(gsym.generics_count);

    const Symbol &msym = syms.symbols[mono_id];
    auto body_r = sub.check(msym);
    if (!body_r)
      for (auto &e : sub.errors) errors.push_back(e);

    return mono_id;
  }

  // ── Core checking ─────────────────────────────────────────────────────────

  Result<BodySema> check(const Symbol &fn_sym) {
    BodySema sema;
    u32 node_count = static_cast<u32>(ir.nodes.kind.size());
    sema.node_type.assign(node_count, IType::from(types.builtin(CTypeKind::Void)));
    sema.node_symbol.assign(node_count, kInvalidSymbol);

    // For impl methods, inject self → impl_type into the type substitution.
    // The &self param is typed Ref(Named("self")) in the TypeAst; resolve "self"
    // to the owner struct's CTypeId so lowering doesn't fail on the unknown name.
    if (fn_sym.impl_owner != 0 && fn_sym.sig.params_count > 0) {
      const FuncParam &fp = mod.params[fn_sym.sig.params_start];
      const TypeAst &ta = lowerer.type_ast;
      if (fp.type != 0 && ta.kind[fp.type] == TypeKind::Ref) {
        TypeId inner_tid = ta.b[fp.type];
        if (ta.kind[inner_tid] == TypeKind::Named) {
          SymId self_sym = ta.a[inner_tid]; // SymId for the string "self"
          SymbolId owner_sid = syms.lookup(fn_sym.module_idx, fn_sym.impl_owner);
          if (owner_sid != kInvalidSymbol) {
            CType ct; ct.kind = CTypeKind::Struct; ct.symbol = owner_sid;
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
      const GenericParam &gp = mod.generic_params[const_generic_scope_start + k];
      if (!gp.is_type)
        sema.define(gp.name, i32_type, false);
    }

    for (u32 k = 0; k < fn_sym.sig.params_count; ++k) {
      const FuncParam &p = mod.params[fn_sym.sig.params_start + k];
      if (p.type == 0) continue;
      auto ct_r = lowerer.lower(p.type);
      if (!ct_r) { errors.push_back(ct_r.error()); continue; }
      sema.define(p.name, IType::from(*ct_r), false);
    }

    if (fn_sym.body != 0)
      check_block(fn_sym.body, sema, ret_type);

    sema.pop_scope();
    if (!errors.empty()) return std::unexpected(errors.front());
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
    case NodeKind::LetStmt:
    case NodeKind::VarStmt: {
      SymId  var_name  = static_cast<SymId>(ir.nodes.a[n]);
      TypeId ann_type  = ir.nodes.b[n];
      NodeId init_expr = ir.nodes.c[n];
      IType var_type;
      if (ann_type != 0) {
        auto r = lowerer.lower(ann_type);
        if (!r) { emit(node_span(n), "unknown type in declaration"); var_type = IType::fresh(unifier.fresh()); }
        else var_type = IType::from(*r);
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
      NodeId cond = ir.nodes.a[n], then_blk = ir.nodes.b[n], else_blk = ir.nodes.c[n];
      IType cond_t = check_expr(cond, sema);
      if (!unifier.unify(cond_t, IType::from(types.builtin(CTypeKind::Bool)), types))
        emit(node_span(cond), "condition must be bool");
      if (then_blk != 0) check_block(then_blk, sema, ret_type);
      if (else_blk != 0) {
        if (ir.nodes.kind[else_blk] == NodeKind::Block) check_block(else_blk, sema, ret_type);
        else                                             check_stmt(else_blk, sema, ret_type);
      }
      break;
    }
    case NodeKind::ForStmt: {
      const ForPayload &fp = ir.fors[ir.nodes.a[n]];
      sema.push_scope();
      if (fp.init != 0) check_stmt(fp.init, sema, ret_type);
      if (fp.cond != 0) {
        IType ct = check_expr(fp.cond, sema);
        if (!unifier.unify(ct, IType::from(types.builtin(CTypeKind::Bool)), types))
          emit(node_span(fp.cond), "for condition must be bool");
      }
      if (fp.step != 0) check_stmt(fp.step, sema, ret_type);
      if (fp.body != 0) check_block(fp.body, sema, ret_type);
      sema.pop_scope();
      break;
    }
    case NodeKind::ExprStmt:
      check_expr(ir.nodes.a[n], sema);
      break;
    case NodeKind::Block:
      check_block(n, sema, ret_type);
      break;
    default:
      check_expr(n, sema);
      break;
    }
  }

  IType check_expr(NodeId n, BodySema &sema) {
    NodeKind nk = ir.nodes.kind[n];
    IType result = IType::from(types.builtin(CTypeKind::Void));

    switch (nk) {
    case NodeKind::IntLit: {
      TypeVarId tv = unifier.fresh();
      result = IType::fresh(tv);
      unifier.bindings[tv] = IType::from(types.builtin(CTypeKind::I32));
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
          if (sym.kind == SymbolKind::Func && sym.sig.ret_type != 0) {
            auto ct = lower_for_sym(sym, sym.sig.ret_type);
            if (ct) result = IType::from(*ct);
          } else if (sym.kind == SymbolKind::GlobalVar && sym.annotate_type != 0) {
            auto ct = lower_for_sym(sym, sym.annotate_type);
            if (ct) result = IType::from(*ct);
          }
        }
      }
      break;
    }
    case NodeKind::Unary:
      result = check_expr(ir.nodes.b[n], sema);
      break;
    case NodeKind::Binary: {
      IType lt = check_expr(ir.nodes.b[n], sema);
      IType rt = check_expr(ir.nodes.c[n], sema);
      if (!unifier.unify(lt, rt, types))
        emit(node_span(n), "type mismatch in binary expression");
      auto op = static_cast<TokenKind>(ir.nodes.a[n]);
      bool is_cmp = op == TokenKind::EqualEqual || op == TokenKind::BangEqual ||
                    op == TokenKind::Less        || op == TokenKind::LessEqual ||
                    op == TokenKind::Greater     || op == TokenKind::GreaterEqual;
      result = is_cmp ? IType::from(types.builtin(CTypeKind::Bool)) : lt;
      break;
    }
    case NodeKind::Call: {
      NodeId callee_n = ir.nodes.a[n];
      u32 args_start  = ir.nodes.b[n];
      u32 args_count  = ir.nodes.c[n];

      check_expr(callee_n, sema);

      std::vector<IType> arg_types;
      for (u32 k = 0; k < args_count; ++k)
        arg_types.push_back(
            check_expr(static_cast<NodeId>(ir.nodes.list[args_start + k]), sema));

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

          // Fall back to inference from argument types (for generic free functions)
          if (type_args.empty())
            type_args = infer_type_args(sym, arg_types);

          resolved = monomorphize(callee_sym, type_args);
          sema.node_symbol[callee_n] = resolved;

          // Lower the generic ret_type using the type args substitution,
          // since the outer lowerer has no binding for T/U/... type params.
          if (sym.sig.ret_type != 0) {
            TypeLowerer ml = lowerer;
            u32 n_type = 0;
            for (u32 k = 0; k < sym.generics_count; ++k) {
              const GenericParam &gp = mod.generic_params[sym.generics_start + k];
              if (gp.is_type && n_type < type_args.size())
                ml.type_subst[gp.name] = type_args[n_type++];
            }
            auto r = ml.lower(sym.sig.ret_type);
            if (r) result = IType::from(*r);
          }
        } else {
          const Symbol &rsym = syms.get(resolved);
          if (rsym.kind == SymbolKind::Func && rsym.sig.ret_type != 0) {
            auto ct = lower_for_sym(rsym, rsym.sig.ret_type);
            if (ct) result = IType::from(*ct);
          }
        }
      }
      break;
    }
    case NodeKind::Field: {
      NodeId base_n   = ir.nodes.a[n];
      SymId  field_nm = static_cast<SymId>(ir.nodes.b[n]);
      IType  base_t   = check_expr(base_n, sema);
      IType  base_c   = unifier.resolve(base_t);

      if (!base_c.is_var) {
        // Peel Ref so &self.field and self.field both work
        CTypeId sct_id = base_c.concrete;
        if (types.types[sct_id].kind == CTypeKind::Ref)
          sct_id = types.types[sct_id].inner;
        const CType &sct = types.types[sct_id];

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
            // 2. Struct field lookup: StructType node stores [SymId, TypeId] pairs
            const Symbol &tsym = syms.get(sct.symbol);
            const BodyIR &tsym_ir = body_for(tsym.module_idx);
            if (tsym.type_node != 0 &&
                tsym_ir.nodes.kind[tsym.type_node] == NodeKind::StructType) {
              u32 fs = tsym_ir.nodes.b[tsym.type_node];
              u32 fc = tsym_ir.nodes.c[tsym.type_node];
              for (u32 k = 0; k < fc; ++k) {
                SymId  fname = static_cast<SymId>(tsym_ir.nodes.list[fs + k * 2]);
                TypeId ftype = static_cast<TypeId>(tsym_ir.nodes.list[fs + k * 2 + 1]);
                if (fname == field_nm) {
                  // Build a lowerer for the struct's module (ftype is in its TypeAst).
                  // TypeLowerer has reference members so it can't be assigned;
                  // use optional + emplace to conditionally construct a dep lowerer.
                  std::optional<TypeLowerer> dep_fl;
                  if (tsym.module_idx != module_idx && dep_type_asts &&
                      tsym.module_idx < dep_type_asts->size() &&
                      (*dep_type_asts)[tsym.module_idx] != nullptr) {
                    dep_fl.emplace(*(*dep_type_asts)[tsym.module_idx], syms,
                                   interner, types);
                    dep_fl->module_idx = tsym.module_idx;
                  }
                  TypeLowerer &fl = dep_fl ? *dep_fl : lowerer;
                  if (sct.list_count > 0) {
                    for (u32 j = 0; j < tsym.generics_count && j < sct.list_count; ++j) {
                      const GenericParam &gp = mod.generic_params[tsym.generics_start + j];
                      if (gp.is_type)
                        fl.type_subst[gp.name] = types.list[sct.list_start + j];
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
      check_expr(ir.nodes.a[n], sema);
      check_expr(ir.nodes.b[n], sema);
      result = IType::fresh(unifier.fresh()); // TODO: peel array type
      break;
    }
    case NodeKind::AddrOf: {
      bool is_mut_ref = ir.nodes.a[n] != 0;
      IType inner_t = check_expr(ir.nodes.b[n], sema);
      IType inner_c = unifier.resolve(inner_t);
      CType ct; ct.kind = CTypeKind::Ref; ct.is_mut = is_mut_ref;
      if (!inner_c.is_var) ct.inner = inner_c.concrete;
      result = IType::from(types.intern(ct));
      break;
    }
    case NodeKind::Deref:
      check_expr(ir.nodes.a[n], sema);
      result = IType::fresh(unifier.fresh()); // TODO: peel ref type
      break;
    case NodeKind::TupleLit: {
      u32 ls = ir.nodes.b[n], cnt = ir.nodes.c[n];
      std::vector<CTypeId> elems;
      for (u32 k = 0; k < cnt; ++k) {
        IType et = check_expr(static_cast<NodeId>(ir.nodes.list[ls + k]), sema);
        IType ec = unifier.resolve(et);
        elems.push_back(ec.is_var ? 0 : ec.concrete);
      }
      auto [start, count] = types.push_list(elems.data(), elems.size());
      CType ct; ct.kind = CTypeKind::Tuple; ct.list_start = start; ct.list_count = count;
      result = IType::from(types.intern(ct));
      break;
    }
    case NodeKind::StructInit: {
      SymId type_name = static_cast<SymId>(ir.nodes.a[n]);
      SymbolId sid = syms.lookup(module_idx, type_name);
      if (sid == kInvalidSymbol) {
        emit(node_span(n), "unknown struct type");
      } else {
        sema.node_symbol[n] = sid;
        CType ct; ct.kind = CTypeKind::Struct; ct.symbol = sid;
        result = IType::from(types.intern(ct));
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
        SymId first_seg  = static_cast<SymId>(ir.nodes.list[ls]);
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
          }
          // else: enum variant or unknown member — result stays fresh
        } else if (import_map) {
          // module::symbol — cross-module function lookup
          auto mit = import_map->find(first_seg);
          if (mit != import_map->end()) {
            u32 dep_mod_idx = mit->second;
            SymbolId dep_sid = syms.lookup_pub(dep_mod_idx, second_seg);
            if (dep_sid != kInvalidSymbol) {
              sema.node_symbol[n] = dep_sid;
              const Symbol &dep_sym = syms.get(dep_sid);
              if (dep_sym.kind == SymbolKind::Func &&
                  dep_sym.generics_count == 0 &&
                  dep_sym.sig.ret_type != 0 &&
                  dep_type_asts &&
                  dep_mod_idx < dep_type_asts->size() &&
                  (*dep_type_asts)[dep_mod_idx] != nullptr) {
                TypeLowerer dep_lowerer(*(*dep_type_asts)[dep_mod_idx], syms,
                                        interner, types);
                dep_lowerer.module_idx = dep_mod_idx;
                auto r = dep_lowerer.lower(dep_sym.sig.ret_type);
                if (r) result = IType::from(*r);
              }
            }
          }
        }
      }
      break;
    }
    case NodeKind::FnLit:
      result = IType::fresh(unifier.fresh());
      break;
    default:
      break;
    }

    sema.node_type[n] = result;
    return result;
  }
};
