#pragma once

#include <vector>

#include <common/types.h>

#include "ctypes.h"
#include "lower_types.h"
#include "method_table.h"
#include "symbol.h"

// result of resolving a multi-segment path like `mod::Type::method` or
// `Type::Variant`.
struct ResolvedPath {
  enum Kind { Unresolved, Func, Type, Method, GlobalVar, EnumVariant };
  Kind kind = Unresolved;
  SymbolId symbol = 0;       // the final resolved symbol
  SymbolId type_symbol = 0;  // for methods/variants: the owning type symbol
  std::vector<CTypeId> type_args; // type args from the path or alias
  SymId variant_name = 0;    // for enum variants: the variant's SymId
};

// resolve a path given its segments, the module context, and available tables.
// handles import aliases, type aliases (including `: type =` annotate_type),
// methods, enum variants, and global variables.
inline ResolvedPath resolve_path(
    const SymId *segments, u32 seg_count,
    u32 caller_module_idx,
    const std::unordered_map<SymId, u32> &import_map,
    const SymbolTable &syms,
    const MethodTable &methods,
    TypeTable &types,
    const std::vector<ModuleContext> *module_contexts,
    const Interner &interner) {

  ResolvedPath result;
  if (seg_count == 0) return result;

  // determine starting scope: first segment may be an import alias
  u32 scope_mod = caller_module_idx;
  u32 seg_idx = 0;
  auto imp_it = import_map.find(segments[0]);
  if (imp_it != import_map.end()) {
    scope_mod = imp_it->second;
    seg_idx = 1;
  }

  // walk segments
  SymbolId current = 0;
  for (; seg_idx < seg_count; ++seg_idx) {
    SymId seg = segments[seg_idx];
    SymbolId sid = (scope_mod == caller_module_idx)
                       ? syms.lookup(scope_mod, seg)
                       : syms.lookup_pub(scope_mod, seg);
    if (sid == kInvalidSymbol) return result;

    // extract type args from alias BEFORE following the alias chain
    const Symbol &pre_alias = syms.symbols[sid];
    if (pre_alias.kind == SymbolKind::Type && pre_alias.annotate_type != 0 &&
        module_contexts &&
        pre_alias.module_idx < module_contexts->size()) {
      const auto &mctx = (*module_contexts)[pre_alias.module_idx];
      TypeLowerer def_fl(*mctx.type_ast, syms, interner, types);
      def_fl.module_idx = pre_alias.module_idx;
      def_fl.module_contexts = module_contexts;
      if (mctx.import_map) def_fl.import_map = mctx.import_map;
      auto ct_r = def_fl.lower(pre_alias.annotate_type);
      if (ct_r && *ct_r != kUnresolved) {
        const CType &pct = types.types[*ct_r];
        for (u32 k = 0; k < pct.list_count; ++k)
          result.type_args.push_back(types.list[pct.list_start + k]);
      }
    }

    // follow alias chain to canonical symbol
    if (syms.symbols[sid].aliased_sym != 0)
      sid = syms.symbols[sid].aliased_sym;

    const Symbol &s = syms.symbols[sid];

    if (s.kind == SymbolKind::Func) {
      result.kind = ResolvedPath::Func;
      result.symbol = sid;
      return result;
    }
    if (s.kind == SymbolKind::GlobalVar) {
      result.kind = ResolvedPath::GlobalVar;
      result.symbol = sid;
      return result;
    }

    if (s.kind == SymbolKind::Type) {
      if (seg_idx + 1 < seg_count) {
        SymId next = segments[seg_idx + 1];

        // try method
        SymbolId method_id = methods.lookup(sid, next);
        if (method_id != kInvalidSymbol) {
          result.kind = ResolvedPath::Method;
          result.symbol = method_id;
          result.type_symbol = sid;
          return result;
        }

        // enum variant
        result.kind = ResolvedPath::EnumVariant;
        result.symbol = sid;
        result.type_symbol = sid;
        result.variant_name = next;
        return result;
      }

      // bare type reference (e.g. type used as callee for constructor)
      result.kind = ResolvedPath::Type;
      result.symbol = sid;
      result.type_symbol = sid;
      return result;
    }

    current = sid;
  }

  // fell through — last segment was resolved but didn't match any pattern
  if (current != 0) {
    result.kind = ResolvedPath::Type;
    result.symbol = current;
  }
  return result;
}
