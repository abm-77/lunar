#pragma once

#include <vector>

#include <common/types.h>

template <class IdT, class KindT, class ListElemT = u32> struct NodeStore {
  std::vector<KindT> kind;
  std::vector<u32> span_s, span_e;
  std::vector<u32> a, b, c;
  std::vector<ListElemT> list; // L list pool

  // Reserve index 0 as the invalid/null sentinel so that 0 can be used
  // as "no node" / "no type" throughout without colliding with real nodes.
  NodeStore() {
    kind.push_back(KindT{});
    span_s.push_back(0);
    span_e.push_back(0);
    a.push_back(0);
    b.push_back(0);
    c.push_back(0);
  }

  IdT make(KindT k, Span sp, u32 A = 0, u32 B = 0, u32 C = 0) {
    IdT id = static_cast<IdT>(kind.size());
    kind.push_back(k);
    span_s.push_back(sp.start);
    span_e.push_back(sp.end);
    a.push_back(A);
    b.push_back(B);
    c.push_back(C);
    return id;
  }

  std::pair<u32, u32> push_list(const ListElemT *items, u32 n) {
    u32 start = static_cast<u32>(list.size());
    list.insert(list.end(), items, items + n);
    return {start, n};
  }
};

using NodeId = u32;
enum class NodeKind : u16 {
  IntLit,
  StrLit,
  BoolLit,
  Ident,
  Unary,      // a = op, b = child
  Binary,     // a = op, b = lhs, c = rhs
  Call,       // a = callee, b = args_start, c = args_count
  Field,      // a = base, b = field SymId
  Index,      // a = base, b = index
  AddrOf,     // a = mut?, b = place
  Deref,      // a = expr
  TupleLit,   // b = elems_start, c = elems_count
  ArrayLit,   // a = index into BodyIR::array_lits
  StructInit, // a = type SymId, b = fields_start, c = fields_count (fields are
              //   pairs [SymId, NodeId] in list pool)
  StructExpr, // b = fields_start, c = fields_count (fields are
              //   pairs [SymId, NodeId] in list pool)
  Block,      // b = stmt_start, c = stmt_count
  ConstStmt,  // a = SymId, b = TypeId or 0, c = init ExprId (or 0 if none)
  VarStmt,    // same as ConstStmt
  AssignStmt, // a = lhs place ExprId, b = rhs ExprId, c = op TokenKind
              // (Equal/PlusEqual/...)
  ReturnStmt, // a = expr (or 0 if empty return)
  IfStmt,     // a = cond, b = then block, c = else block
  ForStmt,    // a = index into BodyIR::fors
  ExprStmt,   // a = expr
  FnLit,      // a = index into BodyIR::fn_lits
  StructType, // b = fields_start, c = fields_count (pairs [SymId, TypeId] in
              // list)
  FnType,     // a = ret TypeId, b = params_start, c = params_count (TypeIds in
              // list)
  EnumType,   // b = variants_start, c = variants_count (SymIds in list)
  Path,       // b = segments_start, c = segments_count (SymIds in list, e.g.
              // Color::Red)
};

using TypeId = u32;
enum class TypeKind : u16 {
  Named, // a = SymId, b = targs_start, c = targs_count
  Ref,   // a = mutable?, b = inner TypeId
  Tuple, // b = list_start, c = list_count
  Fn,    // a = ret TypeId, b = list_start, c = list_count
  Array, // a = count (static_cast<u32>(-1) if unsized), b = elem TypeId
};

using ExprAst = NodeStore<NodeId, NodeKind, NodeId>;
using TypeAst = NodeStore<TypeId, TypeKind, TypeId>;

struct ForPayload {
  NodeId init = 0; // 0 if none
  NodeId cond = 0; // 0 means unconditional (true)
  NodeId step = 0; // 0 if none
  NodeId body = 0;
};

struct FnLitPayload {
  u32 params_start = 0;
  u32 params_count = 0;
  TypeId ret_type = 0;
  NodeId body = 0;
};

// explicit_count == static_cast<u32>(-1) means count was omitted (inferred from
// values)
struct ArrayLitPayload {
  u32 explicit_count = static_cast<u32>(-1);
  TypeId elem_type = 0;
  u32 values_start = 0;
  u32 values_count = 0;
};

struct BodyIR {
  ExprAst nodes;
  std::vector<ForPayload> fors;
  std::vector<FnLitPayload> fn_lits;
  std::vector<ArrayLitPayload> array_lits;
};
