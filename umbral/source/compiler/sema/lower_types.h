#pragma once

#include <string_view>
#include <unordered_map>

#include <common/error.h>
#include <common/interner.h>
#include <compiler/frontend/ast.h>

#include "ctypes.h"
#include "symbol.h"

// Forward declaration — full type available in module.h.
struct Module;

// Bundles all per-module data for cross-module type resolution.
// All pointers are non-owning; the LoadedModule vector keeps them alive.
struct ModuleContext {
  const TypeAst                        *type_ast   = nullptr;
  const BodyIR                         *ir         = nullptr;
  const Module                         *mod        = nullptr;
  std::string_view                      src;
  const std::unordered_map<SymId, u32> *import_map = nullptr;
};

struct TypeLowerer {
  const TypeAst &type_ast;
  const SymbolTable &syms;
  const Interner &interner;
  TypeTable &out;
  std::unordered_map<SymId, CTypeId> type_subst; // type-param substitution
  u32 module_idx = 0; // which module namespace to search for user-defined types
  bool lenient = false; // if true, unknown named types return Void instead of error
  // Per-module context vector for struct/enum node kind distinction.
  const std::vector<ModuleContext> *module_contexts = nullptr;
  // Fallback BodyIR for the current (or only) module when module_contexts is null.
  const BodyIR *current_ir = nullptr;
  // Import alias map (alias SymId → module index) for resolving module::Type.
  const std::unordered_map<SymId, u32> *import_map = nullptr;

  TypeLowerer(const TypeAst &ta, const SymbolTable &s, const Interner &i,
              TypeTable &o)
      : type_ast(ta), syms(s), interner(i), out(o) {}

  // Returns the CTypeId for a primitive type name, or nullopt if not a builtin.
  static std::optional<CTypeId> builtin_from_name(std::string_view sv,
                                                   TypeTable &out) {
    if (sv == "void") return out.builtin(CTypeKind::Void);
    if (sv == "bool") return out.builtin(CTypeKind::Bool);
    if (sv == "i8")   return out.builtin(CTypeKind::I8);
    if (sv == "i16")  return out.builtin(CTypeKind::I16);
    if (sv == "i32")  return out.builtin(CTypeKind::I32);
    if (sv == "i64")  return out.builtin(CTypeKind::I64);
    if (sv == "u8")   return out.builtin(CTypeKind::U8);
    if (sv == "u16")  return out.builtin(CTypeKind::U16);
    if (sv == "u32")  return out.builtin(CTypeKind::U32);
    if (sv == "u64")  return out.builtin(CTypeKind::U64);
    if (sv == "f32")  return out.builtin(CTypeKind::F32);
    if (sv == "f64")  return out.builtin(CTypeKind::F64);
    return std::nullopt;
  }

  Result<CTypeId> lower(TypeId tid) {
    TypeKind tk = type_ast.kind[tid];

    switch (tk) {
    case TypeKind::Named: {
      SymId name = type_ast.a[tid];
      if (name == 0) return out.builtin(CTypeKind::Void); // const-generic int placeholder
      auto sit = type_subst.find(name);
      if (sit != type_subst.end()) return sit->second;

      std::string_view sv = interner.view(name);
      if (auto b = builtin_from_name(sv, out)) return *b;

      // user-defined type
      SymbolId sid = syms.lookup(module_idx, name);
      if (sid == kInvalidSymbol) {
        if (lenient) return out.builtin(CTypeKind::Void);
        Span sp{type_ast.span_s[tid], type_ast.span_e[tid]};
        return std::unexpected(Error{sp, "unknown type name"});
      }
      // Follow cross-module type alias to the actual struct/enum symbol
      if (syms.get(sid).aliased_sym != 0) sid = syms.get(sid).aliased_sym;
      CType ct;
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
          } else if (tnk == NodeKind::Ident) {
            // Simple builtin type alias: const AllocHandle := u64
            SymId alias_name = ir->nodes.a[resolved.type_node];
            std::string_view asv = interner.view(alias_name);
            if (auto b = builtin_from_name(asv, out)) return *b;
            // non-builtin ident alias: fall through to struct resolution
          }
        }
      }
      ct.kind = ct_kind;
      ct.symbol = sid;

      // lower generic type args (e.g., List<i32, 5>) and store in ct.list
      u32 targs_ls = type_ast.b[tid], targs_cnt = type_ast.c[tid];
      if (targs_cnt > 0) {
        std::vector<CTypeId> lowered;
        for (u32 k = 0; k < targs_cnt; ++k) {
          auto a = lower(type_ast.list[targs_ls + k]);
          if (!a) return a;
          lowered.push_back(*a);
        }
        auto [ls, cnt] = out.push_list(lowered.data(), lowered.size());
        ct.list_start = ls;
        ct.list_count = cnt;
      }

      return out.intern(ct);
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
        if (lenient) return out.builtin(CTypeKind::Void);
        return std::unexpected(Error{sp, "no import map for qualified type"});
      }
      auto mit = import_map->find(mod_prefix);
      if (mit == import_map->end()) {
        if (lenient) return out.builtin(CTypeKind::Void);
        return std::unexpected(Error{sp, "unknown module prefix in qualified type"});
      }
      u32 dep_mod_idx = mit->second;
      SymbolId sid = syms.lookup_pub(dep_mod_idx, type_name);
      if (sid == kInvalidSymbol) {
        if (lenient) return out.builtin(CTypeKind::Void);
        // Distinguish between "not found" and "not exported".
        if (syms.lookup(dep_mod_idx, type_name) != kInvalidSymbol)
          return std::unexpected(Error{sp, "type is not exported from module"});
        return std::unexpected(Error{sp, "unknown type in qualified reference"});
      }
      if (syms.get(sid).aliased_sym != 0) sid = syms.get(sid).aliased_sym;
      CType ct;
      CTypeKind ct_kind = CTypeKind::Struct;
      {
        const Symbol &resolved = syms.get(sid);
        const BodyIR *ir = nullptr;
        if (module_contexts && dep_mod_idx < static_cast<u32>(module_contexts->size()))
          ir = (*module_contexts)[dep_mod_idx].ir;
        if (!ir) ir = current_ir;
        if (ir && resolved.type_node != 0) {
          NodeKind tnk = ir->nodes.kind[resolved.type_node];
          if (tnk == NodeKind::EnumType) {
            ct_kind = CTypeKind::Enum;
          } else if (tnk == NodeKind::Ident) {
            // Simple builtin type alias: pub const AllocHandle := u64
            SymId alias_name = ir->nodes.a[resolved.type_node];
            std::string_view asv = interner.view(alias_name);
            if (auto b = builtin_from_name(asv, out)) return *b;
          }
        }
      }
      ct.kind = ct_kind;
      ct.symbol = sid;

      // Lower generic type args (e.g., mem::Alloc<i32>) and store in ct.list.
      if (targs_count > 0) {
        std::vector<CTypeId> lowered;
        for (u32 k = 0; k < targs_count; ++k) {
          auto a = lower(type_ast.list[list_start + 1 + k]);
          if (!a) return a;
          lowered.push_back(*a);
        }
        auto [ls, cnt] = out.push_list(lowered.data(), lowered.size());
        ct.list_start = ls;
        ct.list_count = cnt;
      }

      return out.intern(ct);
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

    case TypeKind::Array: {
      u32 count = type_ast.a[tid];
      auto elem = lower(type_ast.b[tid]);
      if (!elem) return elem;
      if (count == static_cast<u32>(-1)) {
        // []T — slice type
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
