#pragma once

#include <common/interner.h>
#include <common/types.h>
#include <compiler/frontend/ast.h>

using SymbolId = u32;
static constexpr SymbolId kInvalidSymbol = 0;

enum class SymbolKind : u8 { Type, Func, GlobalVar };

struct FuncSig {
  TypeId ret_type = 0;
  u32 params_start = 0; // index into Module::params
  u32 params_count = 0;
};

struct Symbol {
  SymbolKind kind{};
  SymId name{};
  bool is_pub = false;
  bool is_extern = false;
  Span span{};
  u32 module_idx = 0; // which LoadedModule this symbol belongs to

  // Type symbol
  // init NodeId from Decl (StructType / EnumType / FnType / Path)
  NodeId type_node = 0;
  // For cross-module type aliases (type_node = Path): the SymbolId of the
  // actual target type after path resolution. 0 = not a cross-module alias.
  SymbolId aliased_sym = 0;

  // Func symbol
  FuncSig sig{};
  NodeId body = 0;
  SymId impl_owner = 0;   // non-zero for impl methods (= type name SymId)
  u32 generics_start = 0; // into Module::generic_params
  u32 generics_count = 0; // 0 = not generic

  // For monomorphized instances: pre-lowered concrete CTypeIds (= u32) for the
  // function signature (avoids re-lowering with the substitution at codegen).
  bool is_mono_instance = false;
  u32 mono_concrete_ret = 0; // valid only when is_mono_instance
  std::vector<u32> mono_concrete_params;

  // GlobalVar
  TypeId annotate_type = 0; // syntax level (0 if inferred)
  NodeId init_expr;
  bool is_mut = false;
};

struct SymbolTable {
  std::vector<Symbol> symbols;
  // Per-module namespaces: module_namespaces[module_idx][name] = SymbolId.
  // Module 0 is pre-created; call add_module_namespace() for each additional.
  std::vector<std::unordered_map<SymId, SymbolId>> module_namespaces;

  SymbolTable() {
    symbols.push_back({});           // index 0 reserved (kInvalidSymbol)
    module_namespaces.push_back({}); // module 0 namespace
  }

  // Allocate a namespace slot for an additional module (call once per module >
  // 0).
  void add_module_namespace() { module_namespaces.push_back({}); }

  SymbolId add(u32 mod_idx, Symbol s) {
    SymbolId id = static_cast<SymbolId>(symbols.size());
    if (mod_idx < static_cast<u32>(module_namespaces.size()))
      module_namespaces[mod_idx][s.name] = id;
    symbols.push_back(std::move(s));
    return id;
  }

  SymbolId lookup(u32 mod_idx, SymId name) const {
    if (mod_idx < static_cast<u32>(module_namespaces.size())) {
      auto it = module_namespaces[mod_idx].find(name);
      if (it != module_namespaces[mod_idx].end()) return it->second;
    }
    return kInvalidSymbol;
  }

  // Like lookup(mod_idx, name) but only returns if the symbol is pub.
  SymbolId lookup_pub(u32 mod_idx, SymId name) const {
    SymbolId sid = lookup(mod_idx, name);
    return (sid != kInvalidSymbol && symbols[sid].is_pub) ? sid
                                                          : kInvalidSymbol;
  }

  Symbol &get(SymbolId id) { return symbols[id]; }
  const Symbol &get(SymbolId id) const { return symbols[id]; }
};
