#pragma once

#include <vector>

#include <common/types.h>

#include "symbol.h"

using CTypeId = u32;

// sentinel returned by lenient type lowering when a type can't be resolved.
// distinct from CTypeKind::Void (id 0) so callers don't confuse "unresolved"
// with "void".
static constexpr CTypeId kUnresolved = UINT32_MAX;

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
  Ref,       // inner, is_mut
  Array,     // inner=elem, count
  Tuple,     // list_start, list_count
  Fn,        // list_start = [ret, param0, param1, ...], list_count = params + 1
  Struct,    // symbol
  Enum,      // symbol
  Slice,     // inner = elem CTypeId
  Iter,      // inner = elem CTypeId  (runtime: { ptr*, i64 len, i64 idx })
  ConstInt,  // count = integer value — represents a const-generic value param
             // (e.g. N=10)
  FieldIter, // symbol = struct SymbolId — compile-time only; no runtime
             // representation
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
  std::vector<CTypeId> list; // flat pool for Type/Fn elem list

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
      if (e.kind != t.kind || e.is_mut != t.is_mut || e.inner != t.inner ||
          e.count != t.count || e.list_count != t.list_count ||
          e.symbol != t.symbol)
        continue;
      // compare list contents (not just list_start) to enable dedup when the
      // same type args are pushed multiple times at different offsets.
      if (e.list_count > 0 &&
          !std::equal(list.begin() + e.list_start,
                      list.begin() + e.list_start + e.list_count,
                      list.begin() + t.list_start))
        continue;
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

  // shorthand constructors that intern automatically
  CTypeId make_struct(SymbolId sym) {
    CType t;
    t.kind = CTypeKind::Struct;
    t.symbol = sym;
    return intern(t);
  }
  CTypeId make_enum(SymbolId sym) {
    CType t;
    t.kind = CTypeKind::Enum;
    t.symbol = sym;
    return intern(t);
  }
  CTypeId make_slice(CTypeId elem) {
    CType t;
    t.kind = CTypeKind::Slice;
    t.inner = elem;
    return intern(t);
  }
  CTypeId make_ref(CTypeId inner, bool is_mut = false) {
    CType t;
    t.kind = CTypeKind::Ref;
    t.inner = inner;
    t.is_mut = is_mut;
    return intern(t);
  }
  CTypeId make_iter(CTypeId elem) {
    CType t;
    t.kind = CTypeKind::Iter;
    t.inner = elem;
    return intern(t);
  }
  CTypeId make_array(CTypeId elem, u32 count) {
    CType t;
    t.kind = CTypeKind::Array;
    t.inner = elem;
    t.count = count;
    return intern(t);
  }

  // intern or retrieve a ConstInt CTypeId for the given value.
  CTypeId const_int(u32 value) {
    CType t;
    t.kind = CTypeKind::ConstInt;
    t.count = value;
    return intern(t);
  }

  // intern or retrieve a FieldIter CTypeId for the given struct SymbolId.
  CTypeId field_iter(SymbolId struct_sid) {
    CType t;
    t.kind = CTypeKind::FieldIter;
    t.symbol = struct_sid;
    return intern(t);
  }
};
