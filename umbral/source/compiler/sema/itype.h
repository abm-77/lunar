#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include <common/interner.h>
#include <common/types.h>

#include "ctypes.h"
#include "symbol.h"

using TypeVarId = u32;

struct IType {
  bool is_var = false;
  union {
    CTypeId concrete;
    TypeVarId var;
  };

  static IType from(CTypeId id) {
    IType t;
    t.is_var = false;
    t.concrete = id;
    return t;
  }
  static IType fresh(TypeVarId id) {
    IType t;
    t.is_var = true;
    t.var = id;
    return t;
  }
};

struct Unifier {
  std::vector<std::optional<IType>> bindings;

  TypeVarId fresh() {
    TypeVarId id = static_cast<TypeVarId>(bindings.size());
    bindings.push_back(std::nullopt);
    return id;
  }

  IType resolve(IType t) const {
    while (t.is_var && bindings[t.var].has_value()) t = *bindings[t.var];
    return t;
  }

  bool unify(IType a, IType b, const TypeTable &types) {
    a = resolve(a);
    b = resolve(b);
    if (!a.is_var && !b.is_var) return a.concrete == b.concrete;
    if (a.is_var) bindings[a.var] = b;
    else bindings[b.var] = a;
    return true;
  }
};

struct BodySema {
  std::vector<IType> node_type;
  std::vector<SymbolId> node_symbol;

  struct Local {
    IType type;
    bool is_mut;
  };
  std::vector<std::unordered_map<SymId, Local>> scopes;

  void push_scope() { scopes.push_back({}); }
  void pop_scope() { scopes.pop_back(); }

  void define(SymId name, IType type, bool is_mut) {
    scopes.back()[name] = {type, is_mut};
  }

  std::optional<Local> lookup(SymId name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return f->second;
    }
    return std::nullopt;
  }
};
