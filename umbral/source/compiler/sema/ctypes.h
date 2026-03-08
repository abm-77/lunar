#pragma once

#include <vector>

#include <common/types.h>

#include "symbol.h"

using CTypeId = u32;

enum class CTypeKind : u8 {
  Void,
  Bool,
  I8,
  I16,
  I32,
  I64,
  U8,
  U16,
  U32,
  U64,
  F32,
  F64,
  Ref,    // inner, is_mut
  Array,  // inner=elem, count
  Tuple,  // list_start, list_count
  Fn,     // list_start = [ret, param0, param1, ...], list_count = params + 1
  Struct, // symbol
  Enum,   // symbol
};

struct CType {
  CTypeKind kind{};
  bool is_mut = false;
  CTypeId inner = 0;
  u32 count = 0;
  u32 list_start = 0;
  u32 list_count = 0;
  SymbolId symbol = 0;
};

struct TypeTable {
  std::vector<CType> types;
  std::vector<CTypeId> list; // flat pool fo rTyple/Fn elem list

  TypeTable() {
    for (i32 k = static_cast<i32>(CTypeKind::Void);
         k <= static_cast<i32>(CTypeKind::F64); ++k) {
      CType t;
      t.kind = static_cast<CTypeKind>(k);
      types.push_back(t);
    }
  }

  CTypeId builtin(CTypeKind k) const { return static_cast<CTypeId>(k); }

  CTypeId intern(CType t) {
    for (u32 i = 0; i < static_cast<u32>(types.size()); ++i) {
      const auto &e = types[i];
      if (e.kind == t.kind && e.is_mut == t.is_mut && e.inner == t.inner &&
          e.count == t.count && e.list_start == t.list_start &&
          e.list_count == t.list_count && e.symbol == t.symbol)
        return i;
    }

    CTypeId id = static_cast<CTypeId>(types.size());
    types.push_back(t);
    return id;
  }

  std::pair<u32, u32> push_list(const CTypeId *items, u32 n) {
    u32 start = static_cast<u32>(list.size());
    list.insert(list.end(), items, items + n);
    return {start, n};
  }
};
