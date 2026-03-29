#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include <common/error.h>
#include <common/interner.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/module.h>
#include <compiler/meta/mono.h>

#include "ctypes.h"
#include "itype.h"
#include "lower_types.h"
#include "method_table.h"
#include "symbol.h"

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
  MonoEngine *mono_engine = nullptr; // set after construction by the orchestrator
  std::unordered_map<SymbolId, BodySema> *body_semas_out = nullptr;
  // injected by the sema drain loop for const generic params (e.g., N: i32)
  u32 const_generic_scope_start = 0;
  u32 const_generic_scope_count = 0;

  // multi-module support: set these after construction when checking a
  // specific module's functions.
  u32 module_idx = 0;
  // alias SymId → index in the loaded-modules vector.
  const std::unordered_map<SymId, u32> *import_map = nullptr;
  // per-module context vector (unifies the old 5 dep_* parallel arrays).
  const std::vector<ModuleContext> *module_contexts = nullptr;
  // integer literal TypeVars registered for lazy defaulting at end of check().
  // stores (TypeVar, NodeId) so we can report an error if the TypeVar resolves
  // to a non-numeric type (e.g., an integer literal used as a bool condition).
  std::vector<std::pair<TypeVarId, NodeId>> int_default_vars;
  // float literal TypeVars — default to f64 if unresolved by context.
  std::vector<std::pair<TypeVarId, NodeId>> float_default_vars;
  // for-range loop var SymId → struct SymbolId when iterating @fields(T).
  // set during ForRange check; used by MetaField and Field(field_var, "name").
  std::unordered_map<SymId, SymbolId> field_iter_struct;
  // set when checking a @stage method; used to enforce shader field access rules.
  std::optional<ShaderStage> current_shader_stage;
  SymId current_shader_type_name = 0;
  // set by AssignStmt check before checking the LHS so Field can detect writes.
  bool in_assign_lhs = false;

  BodyChecker(const BodyIR &ir, const Module &mod, SymbolTable &syms,
              MethodTable &methods, TypeTable &types, TypeLowerer lowerer,
              const Interner &interner, std::string_view src)
      : ir(ir), mod(mod), syms(syms), methods(methods), types(types),
        lowerer(std::move(lowerer)), interner(interner), src(src) {}

  Span node_span(NodeId n) const {
    return {ir.nodes.span_s[n], ir.nodes.span_e[n]};
  }
  void emit(Span sp, const char *msg) { errors.push_back(Error{sp, msg, module_idx}); }

  // overrides node_type[n] to slice ctid when expected is []T and actual is [N]T. returns true if applied.
  bool coerce_array_to_slice(NodeId n, IType expected, IType actual, BodySema &sema) {
    IType er = unifier.resolve(expected), ar = unifier.resolve(actual);
    if (er.is_var || ar.is_var) return false;
    const CType &ect = types.types[er.concrete];
    const CType &act = types.types[ar.concrete];
    if (ect.kind != CTypeKind::Slice || act.kind != CTypeKind::Array
        || ect.inner != act.inner) return false;
    sema.node_type[n] = IType::from(er.concrete);
    return true;
  }

  // lower a TypeId using the correct TypeAst for the symbol's module.
  // falls back to the current module's lowerer when no dep info is available.
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

  // return the BodyIR for the given module index (falls back to current ir).
  const BodyIR &body_for(u32 mod_idx) const {
    if (mod_idx == module_idx) return ir;
    if (module_contexts && mod_idx < module_contexts->size() &&
        (*module_contexts)[mod_idx].ir)
      return *(*module_contexts)[mod_idx].ir;
    return ir;
  }

  // core checking

  Result<BodySema> check(const Symbol &fn_sym) {
    BodySema sema;
    u32 node_count = static_cast<u32>(ir.nodes.kind.size());
    sema.node_type.assign(node_count,
                          IType::from(types.builtin(CTypeKind::Void)));
    sema.node_symbol.assign(node_count, kInvalidSymbol);

    // for impl methods, inject self → impl_type into the type substitution.
    // the &self param is typed Ref(Named("self")) in the TypeAst; resolve
    // "self" to the owner struct's CTypeId so lowering doesn't fail on the
    // unknown name.
    if (fn_sym.impl_owner != 0 && fn_sym.sig.params_count > 0) {
      const FuncParam &fp = mod.params[fn_sym.sig.params_start];
      const TypeAst &ta = lowerer.type_ast;
      if (fp.type != 0 && ta.kind[fp.type] == TypeKind::Ref) {
        TypeId inner_tid = ta.b[fp.type];
        if (ta.kind[inner_tid] == TypeKind::Named) {
          SymId self_sym = ta.a[inner_tid]; // SymId for "self"
          SymbolId owner_sid =
              syms.lookup(fn_sym.module_idx, fn_sym.impl_owner);
          if (owner_sid != kInvalidSymbol) {
            CType ct;
            ct.kind = CTypeKind::Struct;
            ct.symbol = owner_sid;
            // for mono instances, populate the owner struct's type args so
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

    // inject const generic params (e.g., N: i32) as i32-typed locals so the
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

    // detect @stage annotation for this function and set up shader context.
    for (const ShaderStageInfo &si : mod.shader_stages) {
      if (si.shader_type == fn_sym.impl_owner && si.method_name == fn_sym.name) {
        auto sv = interner.view(si.stage_name_sym);
        if (sv == "vertex") {
          current_shader_stage = ShaderStage::Vertex;
        } else if (sv == "fragment") {
          current_shader_stage = ShaderStage::Fragment;
        } else {
          emit(node_span(fn_sym.body), "@stage: expected 'vertex' or 'fragment'");
        }
        current_shader_type_name = si.shader_type;
        break;
      }
    }

    if (fn_sym.body != 0) check_block(fn_sym.body, sema, ret_type);

    sema.pop_scope();
    if (!errors.empty()) return std::unexpected(errors.front());

    // default any integer literal TypeVars that were never bound through
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
      // default float literals to f64 if not bound to a more specific type.
      IType f64_t = IType::from(types.builtin(CTypeKind::F64));
      for (auto &[tv, node] : float_default_vars) {
        IType resolved = unifier.resolve(IType::fresh(tv));
        if (resolved.is_var) unifier.bindings[tv] = f64_t;
      }
    }

    if (!errors.empty()) return std::unexpected(errors.front());

    // resolve all node_type entries to their concrete form before codegen reads
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
        if (!unifier.unify(var_type, init_t, types)) {
          bool ok = ann_type != 0 &&
                    coerce_array_to_slice(init_expr, var_type, init_t, sema);
          if (!ok) emit(node_span(n), "type mismatch in declaration");
        }
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
      in_assign_lhs = true;
      IType lt = check_expr(ir.nodes.a[n], sema);
      in_assign_lhs = false;
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
    case NodeKind::ForRange: {
      SymId var_name = static_cast<SymId>(ir.nodes.a[n]);
      NodeId iter_n  = ir.nodes.b[n];
      NodeId body_n  = ir.nodes.c[n];
      IType iter_t = check_expr(iter_n, sema);
      IType iter_c = unifier.resolve(iter_t);
      CTypeId elem_ctid = 0;
      bool is_field_iter = false;
      if (!iter_c.is_var) {
        const CType &ict = types.types[iter_c.concrete];
        if (ict.kind == CTypeKind::Iter)
          elem_ctid = ict.inner;
        else if (ict.kind == CTypeKind::FieldIter) {
          // @fields(T) — compile-time-only; record struct for @field lookups.
          is_field_iter = true;
          field_iter_struct[var_name] = ict.symbol;
          elem_ctid = types.builtin(CTypeKind::Void); // placeholder
        } else {
          emit(node_span(n), "for-range requires Iter<T>; wrap with @iter(...)");
        }
      } else {
        emit(node_span(n), "for-range: cannot infer iterator element type");
      }
      sema.push_scope();
      IType elem_t = (elem_ctid && !is_field_iter)
                         ? IType::from(elem_ctid)
                         : IType::fresh(unifier.fresh());
      sema.define(var_name, elem_t, /*is_mut=*/false);
      check_block(body_n, sema, ret_type);
      sema.pop_scope();
      if (is_field_iter) field_iter_struct.erase(var_name);
      break;
    }
    case NodeKind::MetaIf: {
      NodeId cond_n = ir.nodes.a[n];
      NodeId then_n = ir.nodes.b[n];
      NodeId else_n = ir.nodes.c[n];
      auto result = lowerer.eval_const_bool(cond_n, ir);
      if (!result || *result)
        if (then_n != 0) check_block(then_n, sema, ret_type);
      if (!result || !*result)
        if (else_n != 0) {
          if (ir.nodes.kind[else_n] == NodeKind::Block)
            check_block(else_n, sema, ret_type);
          else
            check_stmt(else_n, sema, ret_type); // chained @else_if
        }
      break;
    }
    case NodeKind::MetaAssert: {
      NodeId cond_n = ir.nodes.a[n];
      auto result = lowerer.eval_const_bool(cond_n, ir);
      if (result && !*result)
        emit(node_span(n), "@assert condition is false");
      break;
    }
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
    case NodeKind::FloatLit: {
      TypeVarId tv = unifier.fresh();
      float_default_vars.push_back({tv, n}); // default to f64 if unresolved
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
            // build the concrete fn(...)->ret CType so functions can be used
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
            // fallback for generic or cross-module functions: return the
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
        // positional struct construction: TypeName(arg0, arg1, ...)
        // fields are matched by position, not name.
        if (sym.kind == SymbolKind::Type) {
          SymbolId struct_sid = sym.aliased_sym != 0 ? sym.aliased_sym : callee_sym;
          const Symbol &ssym = syms.get(struct_sid);
          const BodyIR &ssym_ir = body_for(ssym.module_idx);
          NodeId tn = ssym.type_node;
          if (tn != 0 && ssym_ir.nodes.kind[tn] == NodeKind::StructType) {
            u32 fc = ssym_ir.nodes.c[tn];
            if (args_count != fc) {
              emit(node_span(n), "wrong number of arguments for struct construction");
            } else {
              u32 fs = ssym_ir.nodes.b[tn];
              std::optional<TypeLowerer> dep_fl;
              if (ssym.module_idx != module_idx && module_contexts &&
                  ssym.module_idx < module_contexts->size()) {
                const ModuleContext &mctx = (*module_contexts)[ssym.module_idx];
                if (mctx.type_ast) {
                  dep_fl.emplace(*mctx.type_ast, syms, interner, types);
                  dep_fl->module_idx = ssym.module_idx;
                  dep_fl->module_contexts = module_contexts;
                  dep_fl->current_ir = mctx.ir;
                  dep_fl->import_map = mctx.import_map;
                }
              }
              TypeLowerer &fl = dep_fl ? *dep_fl : lowerer;
              for (u32 k = 0; k < fc; ++k) {
                TypeId ftype = static_cast<TypeId>(ssym_ir.nodes.list[fs + k * 2 + 1]);
                auto ct = fl.lower(ftype);
                if (ct) unifier.unify(arg_types[k], IType::from(*ct), types);
              }
            }
            CType sc; sc.kind = CTypeKind::Struct; sc.symbol = struct_sid;
            result = IType::from(types.intern(sc));
            sema.node_type[n] = result;
            break;
          }
        }
        SymbolId resolved = callee_sym;
        if (sym.generics_count > 0) {
          std::vector<CTypeId> type_args;

          // for method calls (callee is a Field node), recover type args from
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

          // explicit type args from a generic path callee: Option<T>::create()
          if (type_args.empty() && ir.nodes.kind[callee_n] == NodeKind::Path) {
            TypeId path_tid = static_cast<TypeId>(ir.nodes.a[callee_n]);
            if (path_tid != 0) {
              auto ct_r = lowerer.lower(path_tid);
              // if lowering returned a non-Void type, extract type args from the
              // CType's list.  if it returned Void (lenient fallback when the type
              // isn't in the current module's namespace, e.g. mod::Alloc<T>), fall
              // through to the direct TypeAst extraction below.
              if (ct_r && *ct_r != 0) {
                const CType &pct = types.types[*ct_r];
                for (u32 k = 0; k < pct.list_count; ++k)
                  type_args.push_back(types.list[pct.list_start + k]);
              } else if (lowerer.type_ast.kind[path_tid] == TypeKind::Named) {
                // type not resolvable locally (e.g., mod::Alloc<i32>::create
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

          // fall back to inference from argument types (for generic free
          // functions). flush integer literal defaults first so that e.g.
          // `id(1)` has a concrete i32 arg for T inference.
          if (type_args.empty()) {
            flush_int_defaults(unifier, types, int_default_vars);
            type_args = infer_type_args(sym, mod, lowerer, unifier, arg_types);
          }

          resolved = mono_engine->request(callee_sym, type_args);
          sema.node_symbol[callee_n] = resolved;

          // use the pre-lowered return type from the mono instance (computed
          // with the correct dep-module TypeLowerer + substitution).
          const Symbol &msym = syms.get(resolved);
          if (msym.mono_concrete_ret != 0)
            result = IType::from(msym.mono_concrete_ret);

          // unify explicit arg types against concrete param types so integer
          // literal TypeVars bind to the expected width (e.g., u64 vs i32).
          // mono_concrete_params excludes self, so this is a direct 1-to-1 map.
          for (u32 k = 0; k < args_count; ++k) {
            if (k >= (u32)msym.mono_concrete_params.size()) break;
            CTypeId expected = msym.mono_concrete_params[k];
            if (expected == 0) continue;
            if (!unifier.unify(arg_types[k], IType::from(expected), types))
              coerce_array_to_slice(ir.nodes.list[args_start + k],
                                    IType::from(expected), arg_types[k], sema);
          }
        } else {
          const Symbol &rsym = syms.get(resolved);
          if (rsym.kind == SymbolKind::Func && rsym.sig.ret_type != 0) {
            auto ct = lower_for_sym(rsym, rsym.sig.ret_type);
            if (ct) result = IType::from(*ct);
          }
          // unify arg types against param types so integer literal TypeVars bind
          // to the expected width (e.g., u64 instead of i32).
          if (rsym.kind == SymbolKind::Func && rsym.sig.params_count > 0) {
            u32 rsym_mod = rsym.module_idx;
            const Module *pm = (rsym_mod == module_idx) ? &mod
                : (module_contexts && rsym_mod < module_contexts->size()
                       ? (*module_contexts)[rsym_mod].mod
                       : nullptr);
            if (pm) {
              // field callees are instance method calls: skip the implicit self.
              // path callees are static method or free function calls: no skip.
              u32 rsym_skip =
                  (ir.nodes.kind[callee_n] == NodeKind::Field) ? 1u : 0u;
              for (u32 k = 0; k < args_count &&
                   (k + rsym_skip) < rsym.sig.params_count; ++k) {
                TypeId pt =
                    pm->params[rsym.sig.params_start + k + rsym_skip].type;
                if (pt == 0) continue;
                auto ct = lower_for_sym(rsym, pt);
                if (!ct) continue;
                if (!unifier.unify(arg_types[k], IType::from(*ct), types))
                  coerce_array_to_slice(ir.nodes.list[args_start + k],
                                        IType::from(*ct), arg_types[k], sema);
              }
            }
          }
        }
      } else {
        // function pointer call: infer return type from the callee's fn CType.
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

      // field descriptor from @for_fields: field_var.name → []u8
      if (ir.nodes.kind[base_n] == NodeKind::Ident) {
        SymId base_sym = static_cast<SymId>(ir.nodes.a[base_n]);
        if (field_iter_struct.count(base_sym)) {
          // only .name is supported on field descriptors
          CType sc; sc.kind = CTypeKind::Slice; sc.inner = types.builtin(CTypeKind::U8);
          result = IType::from(types.intern(sc));
          sema.node_type[n] = result;
          break;
        }
      }

      if (!base_c.is_var) {
        // peel Ref so &self.field and self.field both work
        CTypeId sct_id = base_c.concrete;
        if (types.types[sct_id].kind == CTypeKind::Ref)
          sct_id = types.types[sct_id].inner;
        const CType &sct = types.types[sct_id];

        // shader field access enforcement: check read/write rules for @shader structs.
        if (current_shader_stage && current_shader_type_name != 0 &&
            sct.kind == CTypeKind::Struct &&
            syms.get(sct.symbol).name == current_shader_type_name) {
          ShaderFieldKind sfk = ShaderFieldKind::None;
          for (const ShaderFieldAnnot &a : mod.shader_field_annots) {
            if (a.struct_name == current_shader_type_name && a.field_name == field_nm) {
              sfk = a.kind;
              break;
            }
          }
          bool is_vertex = (*current_shader_stage == ShaderStage::Vertex);
          if (sfk == ShaderFieldKind::VsIn || sfk == ShaderFieldKind::VsOut) {
            if (!is_vertex)
              emit(node_span(n), "field not accessible in fragment stage");
          } else if (sfk == ShaderFieldKind::FsIn || sfk == ShaderFieldKind::FsOut) {
            if (is_vertex)
              emit(node_span(n), "field not accessible in vertex stage");
          }
          if (sfk == ShaderFieldKind::VsOut || sfk == ShaderFieldKind::FsOut) {
            if (!in_assign_lhs)
              emit(node_span(n), "write-only shader field read in expression context");
          } else if (sfk == ShaderFieldKind::VsIn || sfk == ShaderFieldKind::FsIn ||
                     sfk == ShaderFieldKind::DrawData) {
            if (in_assign_lhs)
              emit(node_span(n), "read-only shader field cannot be assigned");
          }
        }

        if (sct.kind == CTypeKind::Slice) {
          auto fname = interner.view(field_nm);
          if (fname == "data") {
            CType rc; rc.kind = CTypeKind::Ref; rc.inner = sct.inner;
            result = IType::from(types.intern(rc));
          } else if (fname == "len") {
            result = IType::from(types.builtin(CTypeKind::U64));
          }
          break;
        }
        if (sct.kind == CTypeKind::Iter) {
          auto fname = interner.view(field_nm);
          if (fname == "len" || fname == "count" || fname == "idx")
            result = IType::from(types.builtin(CTypeKind::U64));
          else
            emit(node_span(n), "Iter<T> has no such field");
          break;
        }
        if (sct.kind == CTypeKind::Tuple && sct.symbol != 0) {
          // anonymous struct: .symbol holds StructType NodeId; look up field type.
          NodeId stype_nid = static_cast<NodeId>(sct.symbol);
          u32 sf_start = ir.nodes.b[stype_nid];
          u32 sf_count = ir.nodes.c[stype_nid];
          for (u32 k = 0; k < sf_count; ++k) {
            SymId fname = static_cast<SymId>(ir.nodes.list[sf_start + k * 2]);
            TypeId ftype = static_cast<TypeId>(ir.nodes.list[sf_start + k * 2 + 1]);
            if (fname == field_nm) {
              auto r = lowerer.lower(ftype);
              if (r) result = IType::from(*r);
              break;
            }
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
              // lower the return type using the method's own module's TypeAst,
              // not the caller's — they may differ for cross-module types.
              std::optional<TypeLowerer> dep_ml;
              if (msym.module_idx != module_idx && module_contexts &&
                  msym.module_idx < module_contexts->size()) {
                const ModuleContext &mctx = (*module_contexts)[msym.module_idx];
                if (mctx.type_ast) {
                  dep_ml.emplace(*mctx.type_ast, syms, interner, types);
                  dep_ml->module_idx = msym.module_idx;
                  dep_ml->module_contexts = module_contexts;
                  dep_ml->current_ir = mctx.ir;
                  dep_ml->import_map = mctx.import_map;
                }
              }
              TypeLowerer &ml = dep_ml ? *dep_ml : lowerer;
              auto r = ml.lower(msym.sig.ret_type);
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
                (tsym_ir.nodes.kind[tsym.type_node] == NodeKind::StructType ||
                 tsym_ir.nodes.kind[tsym.type_node] == NodeKind::MetaBlock)) {
              // for MetaBlock (@gen type), evaluate to find the effective StructType
              NodeId effective_type_node = tsym.type_node;
              u32 tsym_mod_for_fields = tsym.module_idx;
              std::optional<TypeLowerer> dep_fl_early;
              if (tsym_mod_for_fields != module_idx && module_contexts &&
                  tsym_mod_for_fields < module_contexts->size()) {
                const ModuleContext &mctx = (*module_contexts)[tsym_mod_for_fields];
                if (mctx.type_ast) {
                  dep_fl_early.emplace(*mctx.type_ast, syms, interner, types);
                  dep_fl_early->module_idx = tsym_mod_for_fields;
                  dep_fl_early->module_contexts = module_contexts;
                  dep_fl_early->current_ir = mctx.ir;
                  dep_fl_early->import_map = mctx.import_map;
                }
              }
              TypeLowerer &fl_eval = dep_fl_early ? *dep_fl_early : lowerer;
              if (sct.list_count > 0) {
                const Module *tsym_mod_ptr_eval =
                    (tsym_mod_for_fields == module_idx)
                        ? &mod
                        : (module_contexts && tsym_mod_for_fields < module_contexts->size()
                               ? (*module_contexts)[tsym_mod_for_fields].mod
                               : nullptr);
                if (tsym_mod_ptr_eval) {
                  for (u32 j = 0; j < tsym.generics_count && j < sct.list_count; ++j) {
                    const GenericParam &gp =
                        tsym_mod_ptr_eval->generic_params[tsym.generics_start + j];
                    fl_eval.type_subst[gp.name] = types.list[sct.list_start + j];
                  }
                }
              }
              if (tsym_ir.nodes.kind[tsym.type_node] == NodeKind::MetaBlock) {
                NodeId evaled = fl_eval.eval_meta_block(tsym.type_node, tsym_ir, nullptr);
                if (evaled != 0) effective_type_node = evaled;
              }
              if (effective_type_node == 0 ||
                  tsym_ir.nodes.kind[effective_type_node] != NodeKind::StructType) {
                break; // can't resolve field
              }
              u32 fs = tsym_ir.nodes.b[effective_type_node];
              u32 fc = tsym_ir.nodes.c[effective_type_node];
              for (u32 k = 0; k < fc; ++k) {
                SymId fname =
                    static_cast<SymId>(tsym_ir.nodes.list[fs + k * 2]);
                TypeId ftype =
                    static_cast<TypeId>(tsym_ir.nodes.list[fs + k * 2 + 1]);
                if (fname == field_nm) {
                  // fl_eval already has the correct module context + type_subst
                  auto r = fl_eval.lower(ftype);
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
        // peel the reference: *(&mut T) → T
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
      // check element expressions and unify with declared elem type.
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
    case NodeKind::AnonStructInit: {
      // a = StructType NodeId, b = init_fields_start, c = init_fields_count
      NodeId stype_nid = static_cast<NodeId>(ir.nodes.a[n]);
      u32 sf_start = ir.nodes.b[stype_nid];
      u32 sf_count = ir.nodes.c[stype_nid];

      std::vector<CTypeId> field_ctids;
      field_ctids.reserve(sf_count);
      for (u32 k = 0; k < sf_count; ++k) {
        TypeId ftype = static_cast<TypeId>(ir.nodes.list[sf_start + k * 2 + 1]);
        auto r = lowerer.lower(ftype);
        if (!r) { emit(node_span(n), "unknown type in anonymous struct field"); break; }
        field_ctids.push_back(*r);
      }
      // represent as Tuple; .symbol = stype_nid so codegen can find field names.
      auto [list_s, list_c] = types.push_list(field_ctids.data(), sf_count);
      CType ct;
      ct.kind = CTypeKind::Tuple;
      ct.list_start = list_s;
      ct.list_count = list_c;
      ct.symbol = static_cast<SymbolId>(stype_nid);
      result = IType::from(types.intern(ct));
      // check each init expression.
      u32 ifs = ir.nodes.b[n], ifc = ir.nodes.c[n];
      for (u32 k = 0; k < ifc; ++k)
        check_expr(static_cast<NodeId>(ir.nodes.list[ifs + k * 2 + 1]), sema);
      break;
    }
    case NodeKind::Path: {
      // default: unknown type (enum variant, module path, or unresolved).
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
            // enum variant: Type::Variant where method not found.
            // return the enum's concrete CType so downstream expressions
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
                  // mono_engine->request() with type args from generic_type_id
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
    case NodeKind::IterCreate: {
      NodeId src = ir.nodes.a[n];
      IType src_t = check_expr(src, sema);
      IType src_c = unifier.resolve(src_t);
      CTypeId elem_ctid = 0;
      if (!src_c.is_var) {
        const CType &ct = types.types[src_c.concrete];
        if (ct.kind == CTypeKind::Array || ct.kind == CTypeKind::Slice)
          elem_ctid = ct.inner;
        else
          emit(node_span(n), "@iter requires array or slice operand");
      } else {
        emit(node_span(n), "@iter: cannot infer element type");
      }
      CType it; it.kind = CTypeKind::Iter; it.inner = elem_ctid;
      result = IType::from(types.intern(it));
      break;
    }
    case NodeKind::MemCpy:
    case NodeKind::MemMov: {
      // @memcpy/@memmov(dest, src, byte_count) → void
      check_expr(ir.nodes.a[n], sema); // dest
      check_expr(ir.nodes.b[n], sema); // src
      check_expr(ir.nodes.c[n], sema); // byte_count
      result = IType::from(types.builtin(CTypeKind::Void));
      break;
    }
    case NodeKind::MemSet: {
      // @memset(dest, value_u8, byte_count) → void
      check_expr(ir.nodes.a[n], sema); // dest
      check_expr(ir.nodes.b[n], sema); // value (u8)
      check_expr(ir.nodes.c[n], sema); // byte_count
      result = IType::from(types.builtin(CTypeKind::Void));
      break;
    }
    case NodeKind::MemCmp: {
      // @memcmp(a, b, byte_count) → i32
      check_expr(ir.nodes.a[n], sema); // lhs
      check_expr(ir.nodes.b[n], sema); // rhs
      check_expr(ir.nodes.c[n], sema); // byte_count
      result = IType::from(types.builtin(CTypeKind::I32));
      break;
    }
    case NodeKind::FieldsOf: {
      // @fields(TypeName) — returns a FieldIter for the named struct.
      SymId struct_sym = static_cast<SymId>(ir.nodes.a[n]);
      SymbolId sid = syms.lookup(module_idx, struct_sym);
      if (sid == kInvalidSymbol) {
        emit(node_span(n), "@fields: unknown type");
        result = IType::fresh(unifier.fresh());
      } else {
        result = IType::from(types.field_iter(sid));
      }
      break;
    }
    case NodeKind::MetaField: {
      // @field(obj, field_var) — field access where field_var is a compile-time
      // field descriptor from a @for_fields loop. return a fresh TypeVar since
      // the actual field type varies per iteration; codegen handles the unroll.
      check_expr(ir.nodes.a[n], sema);
      result = IType::fresh(unifier.fresh());
      break;
    }
    case NodeKind::ShaderTexture2d:
    case NodeKind::ShaderSampler:
    case NodeKind::ShaderDrawPacket: {
      if (!current_shader_stage)
        emit(node_span(n), "shader intrinsic used outside @stage method");
      check_expr(ir.nodes.a[n], sema);
      result = IType::fresh(unifier.fresh()); // opaque handle
      break;
    }
    case NodeKind::ShaderSample: {
      if (!current_shader_stage)
        emit(node_span(n), "shader intrinsic used outside @stage method");
      check_expr(ir.nodes.a[n], sema);
      check_expr(ir.nodes.b[n], sema);
      check_expr(ir.nodes.c[n], sema);
      // return type is vec4 — scan symbols for the vec4 struct type
      {
        SymbolId vec4_sid = kInvalidSymbol;
        for (u32 s = 1; s < syms.symbols.size() && vec4_sid == kInvalidSymbol; ++s) {
          const Symbol &sv = syms.symbols[s];
          if (sv.kind == SymbolKind::Type && interner.view(sv.name) == "vec4")
            vec4_sid = s;
        }
        if (vec4_sid != kInvalidSymbol) {
          CType sc; sc.kind = CTypeKind::Struct; sc.symbol = vec4_sid;
          result = IType::from(types.intern(sc));
        } else {
          result = IType::fresh(unifier.fresh());
        }
      }
      break;
    }
    case NodeKind::ShaderDrawId: {
      if (!current_shader_stage)
        emit(node_span(n), "shader intrinsic used outside @stage method");
      result = IType::from(types.builtin(CTypeKind::U32));
      break;
    }
    case NodeKind::ShaderFrameRead: {
      if (!current_shader_stage)
        emit(node_span(n), "shader intrinsic used outside @stage method");
      check_expr(ir.nodes.a[n], sema); // offset
      TypeId tid = static_cast<TypeId>(ir.nodes.b[n]);
      auto ct = lowerer.lower(tid);
      result = ct ? IType::from(*ct) : IType::fresh(unifier.fresh());
      break;
    }
    case NodeKind::ShaderRef: {
      if (!current_shader_stage)
        emit(node_span(n), "shader intrinsic used outside @stage method");
      SymbolId bundle_sid = kInvalidSymbol;
      for (u32 s = 1; s < syms.symbols.size() && bundle_sid == kInvalidSymbol; ++s) {
        const Symbol &sv = syms.symbols[s];
        if (sv.kind == SymbolKind::Type && interner.view(sv.name) == "shader_bundle")
          bundle_sid = s;
      }
      if (bundle_sid != kInvalidSymbol) {
        CType sc; sc.kind = CTypeKind::Struct; sc.symbol = bundle_sid;
        result = IType::from(types.intern(sc));
      } else {
        result = IType::fresh(unifier.fresh());
      }
      break;
    }
    default: break;
    }

    sema.node_type[n] = result;
    return result;
  }
};
