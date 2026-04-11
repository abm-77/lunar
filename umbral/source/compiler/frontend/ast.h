#pragma once

#include <vector>

#include <common/types.h>

template <class IdT, class KindT, class ListElemT = u32> struct NodeStore {
  std::vector<KindT> kind;
  std::vector<u32> span_s, span_e;
  std::vector<u32> a, b, c;
  std::vector<ListElemT> list; // L list pool

  // reserve index 0 as the invalid/null sentinel so that 0 can be used
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
  IntLit,   // a = index into BodyIR::int_lits (u64 value), b = LitSuffix
  FloatLit, // a = index into BodyIR::float_lits (f64 value), b = LitSuffix
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
  StructInit, // a = TypeId, b = fields_start, c = fields_count (fields are
              //   pairs [SymId, NodeId] in list pool)
  StructExpr, // b = fields_start, c = fields_count (fields are
              //   pairs [SymId, NodeId] in list pool)
  AnonStructInit, // a = StructType NodeId, b = init_fields_start, c =
                  // init_fields_count
                  //   init fields: [SymId, NodeId] pairs in list pool
  Block,          // b = stmt_start, c = stmt_count
  ConstStmt,      // a = SymId, b = TypeId or 0, c = init ExprId (or 0 if none)
  VarStmt,        // same as ConstStmt
  AssignStmt,     // a = lhs place ExprId, b = rhs ExprId, c = op TokenKind
                  // (Equal/PlusEqual/...)
  ReturnStmt,     // a = expr (or 0 if empty return)
  BreakStmt,      // no fields — jumps to enclosing loop exit
  ContinueStmt,   // no fields — jumps to enclosing loop step
  IfStmt,         // a = cond, b = then block, c = else block
  ForStmt,        // a = index into BodyIR::fors
  ForRange,   // a = SymId (loop var name), b = Iter<T> NodeId, c = body NodeId
  ExprStmt,   // a = expr
  FnLit,      // a = index into BodyIR::fn_lits
  StructType, // b = fields_start, c = fields_count (pairs [SymId, TypeId] in
              // list)
  VecType, // a = count (2/3/4), b = element TypeId
  MatType, // a = cols, b = rows, c = element TypeId
  FnType,     // a = ret TypeId, b = params_start, c = params_count (TypeIds in
              // list)
  EnumType,   // b = variants_start, c = variants_count (SymIds in list)
  Path,       // b = segments_start, c = segments_count (SymIds in list, e.g.
              // Color::Red)
  SliceLit,   // a = elem TypeId, b = vals_start (list pool), c = vals_count
  CastAs,     // a = source NodeId, b = target TypeId  (@as)
  Bitcast,    // a = source NodeId, b = target TypeId  (@bitcast)
  SiteId,     // no args — compile-time call-site u32
  SizeOf,     // a = TypeId — sizeof(T) as u64
  AlignOf,    // a = TypeId — alignof(T) as u64
  SliceCast,  // a = source NodeId ([]u8), b = elem TypeId → []T
  IterCreate, // a = source NodeId (Array or Slice expr) → produces Iter<T>
  MemCpy,     // a = dest NodeId, b = src NodeId, c = byte_count NodeId → void
  MemMov,     // a = dest NodeId, b = src NodeId, c = byte_count NodeId → void
  MemSet, // a = dest NodeId, b = value NodeId (u8), c = byte_count NodeId →
          // void
  MemCmp, // a = lhs NodeId, b = rhs NodeId, c = byte_count NodeId → i32
  MetaIf, // a = cond NodeId, b = then NodeId (Block or type expr), c = else
          // NodeId (0 or next MetaIf or body)
  Shl,        // a = lhs NodeId, b = rhs NodeId → lhs << rhs
  Shr,        // a = lhs NodeId, b = rhs NodeId → lhs >> rhs
  MetaAssert, // a = cond NodeId, b = msg NodeId (StrLit or 0)
  MetaField,  // a = obj NodeId, b = field_var SymId
  FieldsOf,   // a = struct_type_name SymId — @fields(TypeName)
  MetaBlock,  // b = stmts_start, c = stmts_count (list of NodeIds:
              // MetaAsserts/MetaIfs/bare expr)
  // shader-only intrinsics — never compiled to LLVM IR; lowered via MLIR shader
  // pipeline
  ShaderTexture2d,  // a = index_expr (u32) → opaque texture handle
  ShaderSampler,    // a = index_expr (u32) → opaque sampler handle
  ShaderSample,     // a = tex_expr, b = samp_expr, c = uv_expr → vec4
  ShaderDrawId,     // no args → u32 (gl_InstanceIndex / draw id)
  ShaderVertexId,   // no args → u32 (gl_VertexIndex)
  ShaderDrawPacket, // a = id_expr → opaque draw_packet ref
  ShaderFrameRead,  // a = offset_expr (u32), b = TypeId for T → T
  ShaderRef,        // a = SymId of shader struct type → shader_bundle
};

using TypeId = u32;
enum class TypeKind : u16 {
  Named,     // a = SymId, b = targs_start, c = targs_count
  QualNamed, // a = type_name SymId, b = list_start, c = targs_count
             // list[b] = mod_prefix SymId; list[b+1..b+1+c] = type arg TypeIds
  Ref,       // a = mutable?, b = inner TypeId
  Tuple,     // b = list_start, c = list_count
  Fn,        // a = ret TypeId, b = list_start, c = list_count
  Array,     // a = count (static_cast<u32>(-1) if unsized), b = elem TypeId,
             //   c = count_ident SymId (non-zero when count is a const-generic
             //   param like [N]T)
  ConstInt, // a = integer value — const-generic value type arg (e.g. the 10 in
            // List<i32, 10>)
  Vec, // a = count (2/3/4), b = element TypeId
  Mat, // a = cols, b = rows, c = element TypeId
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
  std::vector<u64> int_lits;      // indexed by IntLit node's a field
  std::vector<double> float_lits; // indexed by FloatLit node's a field
};
