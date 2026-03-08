#pragma once

#include <optional>
#include <string_view>

#include "symbol.h"
#include <common/error.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/module.h>

// Collect symbols from one module into a pre-existing (possibly shared)
// SymbolTable. module_idx is the namespace slot to write into.
// Returns an Error on the first duplicate name within that module.
inline std::optional<Error> collect_module_symbols(const Module &mod,
                                                    const BodyIR &ir,
                                                    u32 module_idx,
                                                    SymbolTable &table,
                                                    std::string_view src) {
  auto add = [&](Symbol s) -> std::optional<Error> {
    if (table.lookup(module_idx, s.name) != kInvalidSymbol)
      return Error{s.span, "duplicate name in module"};
    table.add(module_idx, std::move(s));
    return std::nullopt;
  };

  for (const Decl &d : mod.decls) {
    Symbol s;
    s.name       = d.name;
    s.is_pub     = d.is_pub;
    s.span       = d.span;
    s.module_idx = module_idx;

    if (d.init != 0) {
      NodeKind nk = ir.nodes.kind[d.init];

      switch (nk) {
      case NodeKind::FnLit: {
        s.kind = SymbolKind::Func;
        u32 idx = ir.nodes.a[d.init];
        const FnLitPayload &fl = ir.fn_lits[idx];
        s.sig = {.ret_type = fl.ret_type,
                 .params_start = fl.params_start,
                 .params_count = fl.params_count};
        s.body = fl.body;
      } break;

      case NodeKind::StructType:
      case NodeKind::EnumType:
      case NodeKind::FnType: {
        s.kind      = SymbolKind::Type;
        s.type_node = d.init;
      } break;

      case NodeKind::Path: {
        // Cross-module type alias: const Entity := world::Entity
        // The Path node is resolved during type lowering using the import map.
        s.kind      = SymbolKind::Type;
        s.type_node = d.init;
      } break;

      default: {
        s.kind         = SymbolKind::GlobalVar;
        s.annotate_type = d.type;
        s.init_expr    = d.init;
        s.is_mut       = (d.kind == DeclKind::Var);
      } break;
      }
    } else {
      s.kind         = SymbolKind::GlobalVar;
      s.annotate_type = d.type;
      s.is_mut       = (d.kind == DeclKind::Var);
    }

    s.generics_start = d.generics_start;
    s.generics_count = d.generics_count;

    if (auto err = add(std::move(s))) return err;
  }

  for (const ImplDecl &impl : mod.impls) {
    u32 impl_generics_start = 0, impl_generics_count = 0;
    if (!impl.generic_params.empty()) {
      SymbolId type_sid = table.lookup(module_idx, impl.type_name);
      if (type_sid != kInvalidSymbol) {
        const Symbol &type_sym = table.get(type_sid);
        impl_generics_start = type_sym.generics_start;
        impl_generics_count = type_sym.generics_count;
      }
    }

    for (const Decl &m : impl.methods) {
      if (m.init == 0 || ir.nodes.kind[m.init] != NodeKind::FnLit) continue;

      Symbol s;
      s.kind       = SymbolKind::Func;
      s.name       = m.name;
      s.is_pub     = m.is_pub;
      s.span       = m.span;
      s.module_idx = module_idx;
      u32 idx = ir.nodes.a[m.init];
      const FnLitPayload &fl = ir.fn_lits[idx];
      s.sig = {.ret_type = fl.ret_type,
               .params_start = fl.params_start,
               .params_count = fl.params_count};
      s.body           = fl.body;
      s.impl_owner     = impl.type_name;
      s.generics_start = impl_generics_start;
      s.generics_count = impl_generics_count;

      // impl methods may share names across types; don't dedup-check
      table.add(module_idx, std::move(s));
    }
  }

  return std::nullopt;
}

// Single-module convenience wrapper (backward compat for tests).
inline Result<SymbolTable> collect_symbols(const Module &mod, const BodyIR &ir,
                                           std::string_view src) {
  SymbolTable table;
  if (auto err = collect_module_symbols(mod, ir, 0, table, src))
    return std::unexpected(*err);
  return table;
}
