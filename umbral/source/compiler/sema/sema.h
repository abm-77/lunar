#pragma once

#include "body_check.h"
#include "collect.h"
#include "ctypes.h"
#include "lower_types.h"
#include "method_table.h"
#include "symbol.h"
#include <compiler/loader.h>
#include <compiler/meta/mono.h>

struct SemaResult {
  SymbolTable syms;
  TypeTable types;
  MethodTable methods;
  std::unordered_map<SymbolId, BodySema> body_semas;
};

// single-module entry point (used by tests)
inline Result<SemaResult> run_sema(const Module &mod, const BodyIR &ir,
                                   const TypeAst &type_ast,
                                   const Interner &interner,
                                   std::string_view src) {

  // phase 1: collect declarations
  SymbolTable syms;
  auto syms_r = collect_module_symbols(mod, ir, type_ast, 0, syms, src);
  if (syms_r) return std::unexpected(*syms_r);

  // phase 2: canonical type table + lowerer
  TypeTable types;
  std::vector<ModuleContext> single_ctx;
  single_ctx.push_back({&type_ast, &ir, &mod, src, nullptr});
  TypeLowerer lowerer(type_ast, syms, interner, types);
  lowerer.module_idx = 0;
  lowerer.module_contexts = &single_ctx;
  lowerer.current_ir = &ir;

  // phase 3: method table
  auto methods_r = build_method_table(mod, /*module_idx=*/0, syms, lowerer);
  if (!methods_r) return std::unexpected(methods_r.error());
  MethodTable methods = std::move(*methods_r);

  // phase 4: type-check non-generic function bodies.
  MonoEngine mono_engine{syms, types, interner};
  mono_engine.module_contexts = &single_ctx;
  u32 n_original_syms = static_cast<u32>(syms.symbols.size());
  std::unordered_map<SymbolId, BodySema> body_semas;
  for (u32 i = 1; i < n_original_syms; ++i) {
    const Symbol &sym = syms.symbols[i];
    if (sym.kind != SymbolKind::Func || sym.body == 0) continue;
    if (sym.generics_count > 0) continue;
    BodyChecker checker(ir, mod, syms, methods, types, lowerer, interner, src);
    checker.mono_engine = &mono_engine;
    checker.body_semas_out = &body_semas;
    auto body_r = checker.check(sym);
    if (!body_r) return std::unexpected(body_r.error());
    body_semas[i] = std::move(*body_r);
  }

  // drain mono work queue — new instantiations may enqueue more pending entries.
  while (!mono_engine.pending.empty()) {
    auto batch = std::move(mono_engine.pending);
    mono_engine.pending.clear();
    for (const PendingMono &pm : batch) {
      const Symbol &msym = syms.symbols[pm.mono_id];
      TypeLowerer ml(type_ast, syms, interner, types);
      if (msym.mono) ml.type_subst = msym.mono->type_subst;
      ml.lenient = true;
      ml.module_idx = pm.generic_mod_idx;
      ml.module_contexts = &single_ctx;
      ml.current_ir = &ir;

      BodyChecker sub(ir, mod, syms, methods, types, std::move(ml), interner, src);
      sub.mono_engine = &mono_engine;
      sub.body_semas_out = &body_semas;
      sub.const_generic_scope_start = pm.const_generic_scope_start;
      sub.const_generic_scope_count = pm.const_generic_scope_count;

      auto body_r = sub.check(msym);
      if (!body_r) return std::unexpected(body_r.error());
      body_semas[pm.mono_id] = std::move(*body_r);
    }
  }

  return SemaResult{std::move(syms), std::move(types), std::move(methods),
                    std::move(body_semas)};
}

// multi-module entry point.
// modules must be topologically ordered (dependencies before dependents).
// the vector is taken by reference so LoadedModule TypeAsts stay alive during
// sema (dep_type_asts holds pointers into them).
inline Result<SemaResult> run_sema(std::vector<LoadedModule> &modules,
                                   const Interner &interner) {
  // phase 1: allocate per-module namespaces and collect all symbols.
  SymbolTable syms;
  for (u32 i = 1; i < static_cast<u32>(modules.size()); ++i)
    syms.add_module_namespace();

  for (u32 i = 0; i < static_cast<u32>(modules.size()); ++i) {
    auto &lm = modules[i];
    if (auto err =
            collect_module_symbols(lm.mod, lm.ir, lm.type_ast, i, syms, lm.src))
      return std::unexpected(*err);
  }

  // phase 2: canonical type table.
  TypeTable types;

  // phase 3: build method tables for each module and merge into one.
  MethodTable methods;
  for (u32 i = 0; i < static_cast<u32>(modules.size()); ++i) {
    auto &lm = modules[i];
    TypeLowerer lowerer(lm.type_ast, syms, interner, types);
    lowerer.module_idx = i;
    lowerer.import_map = &lm.import_map;
    auto mt_r = build_method_table(lm.mod, i, syms, lowerer);
    if (!mt_r) return std::unexpected(mt_r.error());
    for (auto &[k, v] : mt_r->table) methods.table[k] = v;
  }

  // phase 4a: resolve cross-module type aliases (const E := world::E).
  // after collect, any Type symbol whose type_node is a Path in its module's
  // BodyIR is a cross-module alias. resolve it to the actual target SymbolId
  // and store in aliased_sym so TypeLowerer can follow the indirection.
  for (auto &sym : syms.symbols) {
    if (sym.kind != SymbolKind::Type || sym.type_node == 0 ||
        sym.module_idx >= static_cast<u32>(modules.size()))
      continue;
    const BodyIR &sym_ir = modules[sym.module_idx].ir;
    if (sym_ir.nodes.kind[sym.type_node] != NodeKind::Path) continue;
    u32 ls = sym_ir.nodes.b[sym.type_node];
    u32 cnt = sym_ir.nodes.c[sym.type_node];
    if (cnt < 2) continue;
    SymId first_seg = static_cast<SymId>(sym_ir.nodes.list[ls]);
    SymId second_seg = static_cast<SymId>(sym_ir.nodes.list[ls + 1]);
    const auto &imp = modules[sym.module_idx].import_map;
    auto mit = imp.find(first_seg);
    if (mit == imp.end()) continue;
    u32 dep_mod_idx = mit->second;
    SymbolId actual = syms.lookup_pub(dep_mod_idx, second_seg);
    if (actual != kInvalidSymbol) sym.aliased_sym = actual;
  }

  // phase 4b: build a unified per-module context vector replacing the old five
  // parallel dep_* arrays. all five pieces of per-module data travel together.
  std::vector<ModuleContext> module_contexts;
  module_contexts.reserve(modules.size());
  for (auto &lm : modules)
    module_contexts.push_back({&lm.type_ast, &lm.ir, &lm.mod, lm.src, &lm.import_map});

  // phase 5: type-check each non-generic function body.
  // each symbol carries module_idx; use it to select the right module data.
  MonoEngine mono_engine{syms, types, interner};
  mono_engine.module_contexts = &module_contexts;
  u32 n_original_syms = static_cast<u32>(syms.symbols.size());
  std::unordered_map<SymbolId, BodySema> body_semas;
  for (u32 i = 1; i < n_original_syms; ++i) {
    const Symbol &sym = syms.symbols[i];
    if (sym.kind != SymbolKind::Func || sym.body == 0) continue;
    if (sym.generics_count > 0) continue;

    u32 mod_i = sym.module_idx;
    auto &lm = modules[mod_i];
    TypeLowerer lowerer(lm.type_ast, syms, interner, types);
    lowerer.module_idx = mod_i;
    lowerer.module_contexts = &module_contexts;
    lowerer.current_ir = &lm.ir;
    lowerer.import_map = &lm.import_map;

    BodyChecker checker(lm.ir, lm.mod, syms, methods, types, lowerer, interner,
                        lm.src);
    checker.mono_engine = &mono_engine;
    checker.module_idx = mod_i;
    checker.import_map = &lm.import_map;
    checker.module_contexts = &module_contexts;
    checker.body_semas_out = &body_semas;

    auto body_r = checker.check(sym);
    if (!body_r) return std::unexpected(body_r.error());
    body_semas[i] = std::move(*body_r);
  }

  // drain mono work queue — new instantiations may enqueue more pending entries.
  while (!mono_engine.pending.empty()) {
    auto batch = std::move(mono_engine.pending);
    mono_engine.pending.clear();
    for (const PendingMono &pm : batch) {
      const Symbol &msym = syms.symbols[pm.mono_id];
      u32 mod_i = pm.generic_mod_idx;
      auto &lm = modules[mod_i];
      TypeLowerer ml(lm.type_ast, syms, interner, types);
      if (msym.mono) ml.type_subst = msym.mono->type_subst;
      ml.lenient = true;
      ml.module_idx = mod_i;
      ml.module_contexts = &module_contexts;
      ml.current_ir = &lm.ir;
      ml.import_map = &lm.import_map;

      BodyChecker sub(lm.ir, lm.mod, syms, methods, types, std::move(ml),
                      interner, lm.src);
      sub.mono_engine = &mono_engine;
      sub.module_idx = mod_i;
      sub.import_map = &lm.import_map;
      sub.module_contexts = &module_contexts;
      sub.body_semas_out = &body_semas;
      sub.const_generic_scope_start = pm.const_generic_scope_start;
      sub.const_generic_scope_count = pm.const_generic_scope_count;

      auto body_r = sub.check(msym);
      if (!body_r) return std::unexpected(body_r.error());
      body_semas[pm.mono_id] = std::move(*body_r);
    }
  }

  return SemaResult{std::move(syms), std::move(types), std::move(methods),
                    std::move(body_semas)};
}
