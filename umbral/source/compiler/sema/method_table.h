#pragma once

#include <unordered_map>

#include <common/error.h>
#include <common/types.h>
#include <compiler/frontend/module.h>

#include "ctypes.h"
#include "lower_types.h"
#include "symbol.h"

struct MethodTable {
  // key: upper 32-bits = SymbolId of type, lower 32-bits = SymId (method name)
  // Keying on the type's SymbolId (not CTypeId) means all instantiations of a
  // generic type (List<i32,5>, List<bool,3>, …) share the same method entries.
  std::unordered_map<u64, SymbolId> table;

  static u64 key(SymbolId type_sym, SymId method) {
    return (static_cast<u64>(type_sym) << 32) | static_cast<u64>(method);
  }

  void insert(SymbolId type_sym, SymId method, SymbolId sym) {
    table[key(type_sym, method)] = sym;
  }

  SymbolId lookup(SymbolId type_sym, SymId method) const {
    auto it = table.find(key(type_sym, method));
    return it != table.end() ? it->second : kInvalidSymbol;
  }
};

// Build the method table from impl blocks in mod.
// module_idx selects which namespace to search for the impl target type.
inline Result<MethodTable> build_method_table(const Module &mod,
                                              u32 module_idx,
                                              SymbolTable &syms,
                                              TypeLowerer &lowerer) {
  MethodTable mt;

  for (const ImplDecl &impl : mod.impls) {
    // Resolve the target type name → SymbolId
    SymbolId type_sym = syms.lookup(module_idx, impl.type_name);
    if (type_sym == kInvalidSymbol)
      return std::unexpected(Error{impl.span, "impl target type not found"});

    // Find each method's SymbolId by matching name AND impl_owner
    for (const Decl &m : impl.methods) {
      SymbolId method_sym = kInvalidSymbol;
      for (u32 i = 1; i < static_cast<u32>(syms.symbols.size()); ++i) {
        const Symbol &s = syms.symbols[i];
        if (s.kind == SymbolKind::Func &&
            s.name == m.name &&
            s.impl_owner == impl.type_name) {
          method_sym = i;
          break;
        }
      }
      if (method_sym != kInvalidSymbol)
        mt.insert(type_sym, m.name, method_sym);
    }
  }

  return mt;
}
