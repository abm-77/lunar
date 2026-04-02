#pragma once

#include <memory>
#include <unordered_map>

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

enum class SymFlags : u8 {
  None         = 0,
  Pub          = 1 << 0, // @pub
  Extern       = 1 << 1, // @extern
  ShaderStage  = 1 << 2, // @stage method — skip native codegen, lowered via MLIR shader pipeline
  ShaderFn     = 1 << 3, // @shader_fn method — skip native codegen, lowered via MLIR shader pipeline
  MonoInstance = 1 << 4, // monomorphized instance
  Mut          = 1 << 5, // mutable global (var)
};

// pre-lowered concrete type info for a monomorphized function instance.
// allocated only for mono symbols; non-mono symbols have mono == nullptr.
struct MonoInfo {
  u32 concrete_ret = 0;               // concrete return CTypeId
  u32 self_ctype = 0;                 // non-zero for methods (CTypeId of &self)
  std::vector<u32> concrete_params;   // explicit params only (excludes self)
  std::unordered_map<SymId, u32> type_subst;   // type-param name → concrete CTypeId
  std::unordered_map<SymId, u32> const_values; // const-generic name → integer value
};

struct Symbol {
  SymbolKind kind{};
  SymId name{};
  SymFlags flags = SymFlags::None;
  Span span{};
  u32 module_idx = 0; // which LoadedModule this symbol belongs to

  // type symbol
  NodeId type_node = 0;
  SymbolId aliased_sym = 0; // cross-module type alias target (0 = not alias)

  // func symbol
  FuncSig sig{};
  NodeId body = 0;
  SymId impl_owner = 0;   // non-zero for impl methods (= type name SymId)
  u32 generics_start = 0; // into Module::generic_params
  u32 generics_count = 0; // 0 = not generic

  // non-null for monomorphized instances
  std::shared_ptr<MonoInfo> mono;

  // global var
  TypeId annotate_type = 0;
  NodeId init_expr;

  bool is_mono() const { return mono != nullptr; }
};

struct SymbolTable {
  std::vector<Symbol> symbols;
  // per-module namespaces: module_namespaces[module_idx][name] = SymbolId.
  // module 0 is pre-created; call add_module_namespace() for each additional.
  std::vector<std::unordered_map<SymId, SymbolId>> module_namespaces;

  SymbolTable() {
    symbols.push_back({});           // index 0 reserved (kInvalidSymbol)
    module_namespaces.push_back({}); // module 0 namespace
  }

  // allocate a namespace slot for an additional module (call once per module > 0).
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

  // like lookup(mod_idx, name) but only returns if the symbol is pub.
  SymbolId lookup_pub(u32 mod_idx, SymId name) const {
    SymbolId sid = lookup(mod_idx, name);
    return (sid != kInvalidSymbol && has(symbols[sid].flags, SymFlags::Pub)) ? sid
                                                          : kInvalidSymbol;
  }

  Symbol &get(SymbolId id) { return symbols[id]; }
  const Symbol &get(SymbolId id) const { return symbols[id]; }
};
