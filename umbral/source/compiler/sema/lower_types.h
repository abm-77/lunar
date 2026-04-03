#pragma once

#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <common/error.h>
#include <common/interner.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/lexer.h>

#include "ctypes.h"
#include "symbol.h"

// forward declaration — full type available in module.h.
struct Module;

// bundles all per-module data for cross-module type resolution.
// all pointers are non-owning; the LoadedModule vector keeps them alive.
struct ModuleContext {
  const TypeAst *type_ast = nullptr;
  const BodyIR *ir = nullptr;
  const Module *mod = nullptr;
  std::string_view src;
  const std::unordered_map<SymId, u32> *import_map = nullptr;
};

// construct a TypeLowerer configured for a specific module's context.
struct TypeLowerer {
  const TypeAst &type_ast;
  const SymbolTable &syms;
  const Interner &interner;
  TypeTable &out;
  std::unordered_map<SymId, CTypeId> type_subst; // type-param substitution
  u32 module_idx = 0; // which module namespace to search for user-defined types
  bool lenient =
      false; // if true, unknown named types return Void instead of error
  // per-module context vector for struct/enum node kind distinction.
  const std::vector<ModuleContext> *module_contexts = nullptr;
  // fallback BodyIR for the current (or only) module when module_contexts is
  // null.
  const BodyIR *current_ir = nullptr;
  // import alias map (alias SymId → module index) for resolving module::Type.
  const std::unordered_map<SymId, u32> *import_map = nullptr;

  TypeLowerer(const TypeAst &ta, const SymbolTable &s, const Interner &i,
              TypeTable &o)
      : type_ast(ta), syms(s), interner(i), out(o) {}

  // returns the CTypeId for a primitive type name, or nullopt if not a builtin.
  static std::optional<CTypeId> builtin_from_name(std::string_view sv,
                                                  TypeTable &out) {
    if (sv == "void") return out.builtin(CTypeKind::Void);
    if (sv == "bool") return out.builtin(CTypeKind::Bool);
    if (sv == "i8") return out.builtin(CTypeKind::I8);
    if (sv == "i16") return out.builtin(CTypeKind::I16);
    if (sv == "i32") return out.builtin(CTypeKind::I32);
    if (sv == "i64") return out.builtin(CTypeKind::I64);
    if (sv == "u8") return out.builtin(CTypeKind::U8);
    if (sv == "u16") return out.builtin(CTypeKind::U16);
    if (sv == "u32") return out.builtin(CTypeKind::U32);
    if (sv == "u64") return out.builtin(CTypeKind::U64);
    if (sv == "f32") return out.builtin(CTypeKind::F32);
    if (sv == "f64") return out.builtin(CTypeKind::F64);
    return std::nullopt;
  }

  // evaluate a constant integer expression using type_subst for const-generic
  // values. returns nullopt if the expression is not compile-time evaluable.
  std::optional<i64> eval_const_int(NodeId n, const BodyIR &ir) const {
    if (n == 0) return std::nullopt;
    switch (ir.nodes.kind[n]) {
    case NodeKind::IntLit: return static_cast<i64>(ir.nodes.a[n]);
    case NodeKind::BoolLit: return static_cast<i64>(ir.nodes.a[n]);
    case NodeKind::Ident: {
      SymId sym = ir.nodes.a[n];
      auto it = type_subst.find(sym);
      if (it != type_subst.end() &&
          it->second < static_cast<u32>(out.types.size())) {
        const CType &ct = out.types[it->second];
        if (ct.kind == CTypeKind::ConstInt) return static_cast<i64>(ct.count);
      }
      return std::nullopt;
    }
    case NodeKind::Unary: {
      auto child = eval_const_int(ir.nodes.b[n], ir);
      if (!child) return std::nullopt;
      auto op = static_cast<TokenKind>(ir.nodes.a[n]);
      if (op == TokenKind::Minus) return -*child;
      if (op == TokenKind::Bang) return !*child ? 1 : 0;
      return std::nullopt;
    }
    case NodeKind::Binary: {
      auto lv = eval_const_int(ir.nodes.b[n], ir);
      auto rv = eval_const_int(ir.nodes.c[n], ir);
      if (!lv || !rv) return std::nullopt;
      auto op = static_cast<TokenKind>(ir.nodes.a[n]);
      switch (op) {
      case TokenKind::Plus: return *lv + *rv;
      case TokenKind::Minus: return *lv - *rv;
      case TokenKind::Star: return *lv * *rv;
      case TokenKind::Slash: return *rv != 0 ? *lv / *rv : std::optional<i64>{};
      case TokenKind::Less: return static_cast<i64>(*lv < *rv);
      case TokenKind::LessEqual: return static_cast<i64>(*lv <= *rv);
      case TokenKind::Greater: return static_cast<i64>(*lv > *rv);
      case TokenKind::GreaterEqual: return static_cast<i64>(*lv >= *rv);
      case TokenKind::EqualEqual: return static_cast<i64>(*lv == *rv);
      case TokenKind::BangEqual: return static_cast<i64>(*lv != *rv);
      default: return std::nullopt;
      }
    }
    default: return std::nullopt;
    }
  }

  std::optional<bool> eval_const_bool(NodeId n, const BodyIR &ir) const {
    if (n == 0) return std::nullopt;
    if (ir.nodes.kind[n] == NodeKind::BoolLit) return ir.nodes.a[n] != 0;
    auto iv = eval_const_int(n, ir);
    if (iv) return *iv != 0;
    return std::nullopt;
  }

  // evaluate a @if chain or bare expr node from a MetaBlock.
  // returns the NodeId of the selected branch body (StructType or sub-expr), or
  // 0. appends errors to errors_out if non-null.
  NodeId eval_meta_if_chain(NodeId n, const BodyIR &ir,
                            std::vector<Error> *errors_out) const {
    while (n != 0) {
      NodeKind nk = ir.nodes.kind[n];
      if (nk == NodeKind::MetaIf) {
        NodeId cond_n = ir.nodes.a[n];
        NodeId then_n = ir.nodes.b[n];
        NodeId else_n = ir.nodes.c[n];
        auto result = eval_const_bool(cond_n, ir);
        if (!result) {
          // unevaluable — return then branch (conservative: take first branch)
          return then_n;
        }
        if (*result) return then_n;
        n = else_n; // try else branch
      } else {
        return n; // bare expr (e.g. StructType) is the result
      }
    }
    return 0;
  }

  // evaluate a MetaBlock node to find the effective type expression NodeId.
  // processes @asserts (fails if false), @if chains, and returns the final
  // expr.
  NodeId eval_meta_block(NodeId meta_block_nid, const BodyIR &ir,
                         std::vector<Error> *errors_out) const {
    u32 ls = ir.nodes.b[meta_block_nid];
    u32 cnt = ir.nodes.c[meta_block_nid];
    NodeId result_nid = 0;
    for (u32 k = 0; k < cnt; ++k) {
      NodeId stmt = static_cast<NodeId>(ir.nodes.list[ls + k]);
      if (stmt == 0) continue;
      NodeKind nk = ir.nodes.kind[stmt];
      if (nk == NodeKind::MetaAssert) {
        NodeId cond_n = ir.nodes.a[stmt];
        auto val = eval_const_bool(cond_n, ir);
        if (val && !*val) {
          NodeId msg_n = ir.nodes.b[stmt];
          const char *msg = "@assert failed";
          if (msg_n != 0 && ir.nodes.kind[msg_n] == NodeKind::StrLit) {
            // note: interner.view() data is valid for the lifetime of the
            // interner store as a std::string in the error (Error::msg is const
            // char*) for simplicity, use a static fallback; the msg will be in
            // interner
            msg = "@assert condition is false";
          }
          if (errors_out)
            errors_out->push_back(
                Error{{ir.nodes.span_s[stmt], ir.nodes.span_e[stmt]}, msg});
          return 0;
        }
      } else if (nk == NodeKind::MetaIf) {
        result_nid = eval_meta_if_chain(stmt, ir, errors_out);
      } else {
        result_nid = stmt; // bare expr (StructType, etc.)
      }
    }
    return result_nid;
  }

  // shared lowering for a resolved SymbolId with type args.
  // handles alias resolution, enum/struct/MetaBlock detection, and type arg
  // lowering.
  Result<CTypeId> lower_user_type(SymbolId sid, const TypeId *targs,
                                  u32 targs_count, Span sp) {
    if (syms.get(sid).aliased_sym != 0) sid = syms.get(sid).aliased_sym;
    CTypeKind ct_kind = CTypeKind::Struct;
    {
      const Symbol &resolved = syms.get(sid);
      const BodyIR *ir = nullptr;
      if (module_contexts) {
        u32 mod = resolved.module_idx;
        if (mod < static_cast<u32>(module_contexts->size()))
          ir = (*module_contexts)[mod].ir;
      }
      if (!ir) ir = current_ir;
      if (ir && resolved.type_node != 0) {
        NodeKind tnk = ir->nodes.kind[resolved.type_node];
        if (tnk == NodeKind::EnumType) {
          ct_kind = CTypeKind::Enum;
        } else if (tnk == NodeKind::VecType) {
          u32 count = ir->nodes.a[resolved.type_node];
          TypeId elem_tid = ir->nodes.b[resolved.type_node];
          // lower element type using the defining module's TypeAst
          const TypeAst &def_ta = module_contexts
              ? *(*module_contexts)[resolved.module_idx].type_ast
              : type_ast;
          TypeLowerer def_fl(def_ta, syms, interner, out);
          def_fl.module_idx = resolved.module_idx;
          auto elem = def_fl.lower(elem_tid);
          if (!elem) return elem;
          return out.make_vec(*elem, count);
        } else if (tnk == NodeKind::MatType) {
          u32 cols = ir->nodes.a[resolved.type_node];
          u32 rows = ir->nodes.b[resolved.type_node];
          TypeId elem_tid = ir->nodes.c[resolved.type_node];
          const TypeAst &def_ta = module_contexts
              ? *(*module_contexts)[resolved.module_idx].type_ast
              : type_ast;
          TypeLowerer def_fl(def_ta, syms, interner, out);
          def_fl.module_idx = resolved.module_idx;
          auto elem = def_fl.lower(elem_tid);
          if (!elem) return elem;
          auto col_vec = out.make_vec(*elem, rows);
          return out.make_mat(col_vec, cols);
        } else if (tnk == NodeKind::Ident) {
          SymId alias_name = ir->nodes.a[resolved.type_node];
          if (auto b = builtin_from_name(interner.view(alias_name), out))
            return *b;
        } else if (tnk == NodeKind::MetaBlock) {
          NodeId effective_nid =
              eval_meta_block(resolved.type_node, *ir, nullptr);
          if (effective_nid == 0)
            return std::unexpected(Error{sp, "meta type evaluation failed"});
          ct_kind = CTypeKind::Struct;
        }
      }
    }
    CType ct;
    ct.kind = ct_kind;
    ct.symbol = sid;
    if (targs_count > 0) {
      std::vector<CTypeId> lowered;
      for (u32 k = 0; k < targs_count; ++k) {
        auto a = lower(targs[k]);
        if (!a) return a;
        lowered.push_back(*a);
      }
      auto [ls, cnt] = out.push_list(lowered.data(), lowered.size());
      ct.list_start = ls;
      ct.list_count = cnt;
    }
    return out.intern(ct);
  }

  Result<CTypeId> lower(TypeId tid) {
    TypeKind tk = type_ast.kind[tid];

    switch (tk) {
    case TypeKind::Named: {
      SymId name = type_ast.a[tid];
      if (name == 0)
        return out.builtin(CTypeKind::Void); // const-generic int placeholder
      auto sit = type_subst.find(name);
      if (sit != type_subst.end()) return sit->second;

      std::string_view sv = interner.view(name);
      if (auto b = builtin_from_name(sv, out)) return *b;

      // user-defined type
      SymbolId sid = syms.lookup(module_idx, name);
      if (sid == kInvalidSymbol) {
        if (lenient) return kUnresolved;
        Span sp{type_ast.span_s[tid], type_ast.span_e[tid]};
        return std::unexpected(Error{sp, "unknown type name"});
      }
      u32 targs_ls = type_ast.b[tid], targs_cnt = type_ast.c[tid];
      Span sp{type_ast.span_s[tid], type_ast.span_e[tid]};
      return lower_user_type(sid,
                             targs_cnt > 0 ? &type_ast.list[targs_ls] : nullptr,
                             targs_cnt, sp);
    } break;

    case TypeKind::QualNamed: {
      // module::Type [<targ,...>] — resolve via import_map
      // list layout: [mod_prefix, targ0, targ1, ...]
      SymId type_name = type_ast.a[tid];
      u32 list_start = type_ast.b[tid];
      u32 targs_count = type_ast.c[tid];
      SymId mod_prefix = static_cast<SymId>(type_ast.list[list_start]);
      Span sp{type_ast.span_s[tid], type_ast.span_e[tid]};
      if (!import_map) {
        if (lenient) return kUnresolved;
        return std::unexpected(Error{sp, "no import map for qualified type"});
      }
      auto mit = import_map->find(mod_prefix);
      if (mit == import_map->end()) {
        if (lenient) return kUnresolved;
        return std::unexpected(
            Error{sp, "unknown module prefix in qualified type"});
      }
      u32 dep_mod_idx = mit->second;
      SymbolId sid = syms.lookup_pub(dep_mod_idx, type_name);
      if (sid == kInvalidSymbol) {
        if (lenient) return kUnresolved;
        // distinguish between "not found" and "not exported".
        if (syms.lookup(dep_mod_idx, type_name) != kInvalidSymbol)
          return std::unexpected(Error{sp, "type is not exported from module"});
        return std::unexpected(
            Error{sp, "unknown type in qualified reference"});
      }
      const TypeId *targ_ids =
          targs_count > 0 ? &type_ast.list[list_start + 1] : nullptr;
      return lower_user_type(sid, targ_ids, targs_count, sp);
    } break;

    case TypeKind::Ref: {
      bool is_mut = type_ast.a[tid] != 0;
      auto inner = lower(type_ast.b[tid]);
      if (!inner) return inner;
      CType ct;
      ct.kind = CTypeKind::Ref;
      ct.is_mut = is_mut;
      ct.inner = *inner;
      return out.intern(ct);
    } break;

    case TypeKind::ConstInt: return out.const_int(type_ast.a[tid]);

    case TypeKind::Array: {
      u32 count = type_ast.a[tid];
      auto elem = lower(type_ast.b[tid]);
      if (!elem) return elem;
      if (count == static_cast<u32>(-1)) {
        // []T — slice type (or [N]T where N is a const-generic ident)
        SymId count_ident = type_ast.c[tid];
        if (count_ident != 0) {
          // [N]T with const-generic N — look up N in type_subst
          auto it = type_subst.find(count_ident);
          if (it != type_subst.end() &&
              it->second < static_cast<u32>(out.types.size())) {
            const CType &nct = out.types[it->second];
            if (nct.kind == CTypeKind::ConstInt) {
              CType ct;
              ct.kind = CTypeKind::Array;
              ct.inner = *elem;
              ct.count = nct.count;
              return out.intern(ct);
            }
          }
          // N not yet substituted — fall through to slice (lenient mode or
          // error)
        }
        CType ct;
        ct.kind = CTypeKind::Slice;
        ct.inner = *elem;
        return out.intern(ct);
      }
      CType ct;
      ct.kind = CTypeKind::Array;
      ct.inner = *elem;
      ct.count = count;
      return out.intern(ct);

    } break;

    case TypeKind::Tuple: {
      u32 ls = type_ast.b[tid], n = type_ast.c[tid];
      std::vector<CTypeId> elems;
      for (u32 k = 0; k < n; ++k) {
        auto e = lower(type_ast.list[ls + k]);
        if (!e) return e;
        elems.push_back(*e);
      }
      auto [start, cnt] = out.push_list(elems.data(), elems.size());
      CType ct;
      ct.kind = CTypeKind::Tuple;
      ct.list_start = start;
      ct.list_count = cnt;
      return out.intern(ct);
    } break;

    case TypeKind::Vec: {
      u32 count = type_ast.a[tid];
      auto elem = lower(type_ast.b[tid]);
      if (!elem) return elem;
      return out.make_vec(*elem, count);
    } break;

    case TypeKind::Mat: {
      u32 cols = type_ast.a[tid];
      u32 rows = type_ast.b[tid];
      TypeId elem_tid = type_ast.c[tid];
      auto elem = lower(elem_tid);
      if (!elem) return elem;
      auto col_vec = out.make_vec(*elem, rows);
      return out.make_mat(col_vec, cols);
    } break;

    case TypeKind::Fn: {
      TypeId ret_tid = type_ast.a[tid];
      u32 ls = type_ast.b[tid], n = type_ast.c[tid];
      auto ret = lower(ret_tid);
      if (!ret) return ret;

      // list = [ret, param0, param1, ...]
      std::vector<CTypeId> list{*ret};
      for (u32 k = 0; k < n; ++k) {
        auto p = lower(type_ast.list[ls + k]);
        if (!p) return p;
        list.push_back(*p);
      }

      auto [start, cnt] = out.push_list(list.data(), list.size());
      CType ct;
      ct.kind = CTypeKind::Fn;
      ct.list_start = start;
      ct.list_count = cnt;
      return out.intern(ct);
    } break;
    }

    Span sp{type_ast.span_s[tid], type_ast.span_e[tid]};
    return std::unexpected(Error{sp, "unhandled type kind in lowering"});
  }
};

inline TypeLowerer
make_module_type_lowerer(u32 mod_idx, const ModuleContext &ctx,
                         const SymbolTable &syms, const Interner &interner,
                         TypeTable &types,
                         const std::vector<ModuleContext> *all_contexts) {
  TypeLowerer tl(*ctx.type_ast, syms, interner, types);
  tl.module_idx = mod_idx;
  tl.module_contexts = all_contexts;
  tl.current_ir = ctx.ir;
  tl.import_map = ctx.import_map;
  return tl;
}
