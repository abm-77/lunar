#pragma once

#include "body_check.h"
#include "collect.h"
#include "ctypes.h"
#include "lower_types.h"
#include "method_table.h"
#include "symbol.h"
#include <compiler/loader.h>

struct SemaResult {
  SymbolTable syms;
  TypeTable   types;
  MethodTable methods;
};

// ── Single-module entry point (used by tests) ─────────────────────────────

inline Result<SemaResult> run_sema(const Module &mod, const BodyIR &ir,
                                   const TypeAst &type_ast,
                                   const Interner &interner,
                                   std::string_view src) {
  // Phase 1: collect declarations
  auto syms_r = collect_symbols(mod, ir, src);
  if (!syms_r) return std::unexpected(syms_r.error());
  SymbolTable syms = std::move(*syms_r);

  // Phase 2: canonical type table + lowerer
  TypeTable   types;
  TypeLowerer lowerer(type_ast, syms, interner, types);

  // Phase 3: method table
  auto methods_r = build_method_table(mod, /*module_idx=*/0, syms, lowerer);
  if (!methods_r) return std::unexpected(methods_r.error());
  MethodTable methods = std::move(*methods_r);

  // Phase 4: type-check non-generic function bodies.
  MonoCache mono_cache;
  u32 n_original_syms = static_cast<u32>(syms.symbols.size());
  for (u32 i = 1; i < n_original_syms; ++i) {
    const Symbol &sym = syms.symbols[i];
    if (sym.kind != SymbolKind::Func || sym.body == 0) continue;
    if (sym.generics_count > 0) continue;
    BodyChecker checker(ir, mod, syms, methods, types, lowerer, interner, src,
                        mono_cache);
    auto body_r = checker.check(sym);
    if (!body_r) return std::unexpected(body_r.error());
  }

  return SemaResult{std::move(syms), std::move(types), std::move(methods)};
}

// ── Multi-module entry point ───────────────────────────────────────────────
// modules must be topologically ordered (dependencies before dependents).
// The vector is taken by reference so LoadedModule TypeAsts stay alive during
// sema (dep_type_asts holds pointers into them).

inline Result<SemaResult> run_sema(std::vector<LoadedModule> &modules,
                                   const Interner &interner) {
  // Phase 1: allocate per-module namespaces and collect all symbols.
  SymbolTable syms;
  for (u32 i = 1; i < static_cast<u32>(modules.size()); ++i)
    syms.add_module_namespace();

  for (u32 i = 0; i < static_cast<u32>(modules.size()); ++i) {
    auto &lm = modules[i];
    if (auto err = collect_module_symbols(lm.mod, lm.ir, i, syms, lm.src))
      return std::unexpected(*err);
  }

  // Phase 2: canonical type table.
  TypeTable types;

  // Phase 3: build method tables for each module and merge into one.
  MethodTable methods;
  for (u32 i = 0; i < static_cast<u32>(modules.size()); ++i) {
    auto &lm = modules[i];
    TypeLowerer lowerer(lm.type_ast, syms, interner, types);
    lowerer.module_idx = i;
    auto mt_r = build_method_table(lm.mod, i, syms, lowerer);
    if (!mt_r) return std::unexpected(mt_r.error());
    for (auto &[k, v] : mt_r->table)
      methods.table[k] = v;
  }

  // Phase 4a: resolve cross-module type aliases (const E := world::E).
  // After collect, any Type symbol whose type_node is a Path in its module's
  // BodyIR is a cross-module alias. Resolve it to the actual target SymbolId
  // and store in aliased_sym so TypeLowerer can follow the indirection.
  for (auto &sym : syms.symbols) {
    if (sym.kind != SymbolKind::Type || sym.type_node == 0 ||
        sym.module_idx >= static_cast<u32>(modules.size()))
      continue;
    const BodyIR &sym_ir = modules[sym.module_idx].ir;
    if (sym_ir.nodes.kind[sym.type_node] != NodeKind::Path) continue;
    u32 ls  = sym_ir.nodes.b[sym.type_node];
    u32 cnt = sym_ir.nodes.c[sym.type_node];
    if (cnt < 2) continue;
    SymId first_seg  = static_cast<SymId>(sym_ir.nodes.list[ls]);
    SymId second_seg = static_cast<SymId>(sym_ir.nodes.list[ls + 1]);
    const auto &imp = modules[sym.module_idx].import_map;
    auto mit = imp.find(first_seg);
    if (mit == imp.end()) continue;
    u32 dep_mod_idx = mit->second;
    SymbolId actual = syms.lookup_pub(dep_mod_idx, second_seg);
    if (actual != kInvalidSymbol)
      sym.aliased_sym = actual;
  }

  // Phase 4b: collect per-module TypeAst and BodyIR pointers for cross-module
  // type resolution and struct field access.
  std::vector<const TypeAst *> dep_type_asts;
  std::vector<const BodyIR *>  dep_irs;
  dep_type_asts.reserve(modules.size());
  dep_irs.reserve(modules.size());
  for (auto &lm : modules) {
    dep_type_asts.push_back(&lm.type_ast);
    dep_irs.push_back(&lm.ir);
  }

  // Phase 5: type-check each non-generic function body.
  // Each symbol carries module_idx; use it to select the right module data.
  MonoCache mono_cache;
  u32 n_original_syms = static_cast<u32>(syms.symbols.size());
  for (u32 i = 1; i < n_original_syms; ++i) {
    const Symbol &sym = syms.symbols[i];
    if (sym.kind != SymbolKind::Func || sym.body == 0) continue;
    if (sym.generics_count > 0) continue;

    u32 mod_i  = sym.module_idx;
    auto &lm   = modules[mod_i];
    TypeLowerer lowerer(lm.type_ast, syms, interner, types);
    lowerer.module_idx = mod_i;

    BodyChecker checker(lm.ir, lm.mod, syms, methods, types, lowerer,
                        interner, lm.src, mono_cache);
    checker.module_idx    = mod_i;
    checker.import_map    = &lm.import_map;
    checker.dep_type_asts = &dep_type_asts;
    checker.dep_irs       = &dep_irs;

    auto body_r = checker.check(sym);
    if (!body_r) return std::unexpected(body_r.error());
  }

  return SemaResult{std::move(syms), std::move(types), std::move(methods)};
}
