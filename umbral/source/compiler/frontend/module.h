#pragma once

#include <common/interner.h>
#include <common/types.h>
#include <compiler/frontend/ast.h>

// shader annotation kinds for @shader struct fields
enum class ShaderFieldKind : u8 { None, VsIn, VsOut, FsIn, FsOut, DrawData };
// IO annotation kinds for @shader_pod struct fields
enum class IOAnnotKind : u8 { None, Location, BuiltinPosition };
// resolved stage kind — set by body_check after comparing stage_name_sym
// against the interned strings "vertex" and "fragment".
enum class ShaderStage : u8 { Vertex, Fragment };

// per-field annotation on a @shader struct (e.g. @vs_in vin: SpriteVertex)
struct ShaderFieldAnnot {
  SymId struct_name;
  SymId field_name;
  ShaderFieldKind kind;
};

// per-field IO annotation on a @shader_pod struct (e.g. @location(0) pos: vec2)
struct IOFieldAnnot {
  SymId struct_name;
  SymId field_name;
  IOAnnotKind kind;
  u32 location_index; // valid when kind == Location
};

// @stage(vertex|fragment) annotation on an impl method.
struct ShaderStageInfo {
  SymId shader_type;    // the impl type name
  SymId method_name;    // the method annotated with @stage
  SymId stage_name_sym; // interned SymId of "vertex" or "fragment"; resolved in body_check
};

// @shader_fn annotation on an impl method — shader-side helper callable from @stage/@shader_fn.
struct ShaderFnInfo {
  SymId shader_type;  // the impl type name
  SymId method_name;  // the method annotated with @shader_fn
};

struct GenericParam {
  SymId name;        // e.g., T
  bool is_type;      // true = type param; false = const generic
  TypeId const_kind; // if !is_type: the kind type (e.g., TypeId for i32)
};

struct ImportDecl {
  // module path as a list of SymId segments
  u32 path_list_start = 0;
  u32 path_list_count = 0;
  SymId alias = 0; // 0 means no alias (alias = last segment)
  Span span{};
};

struct FuncParam {
  SymId name{};
  TypeId type{};
  NodeId default_init = 0; // 0 = no default; else NodeId in the owning module's BodyIR
  Span span{};
};

enum class DeclKind : u8 { Const, Var };

enum class DeclFlags : u8 {
  None      = 0,
  Pub       = 1 << 0, // @pub
  Extern    = 1 << 1, // @extern
  Gen       = 1 << 2, // @gen
  Shader    = 1 << 3, // @shader
  ShaderPod = 1 << 4, // @shader_pod
};

struct Decl {
  SymId name{};
  TypeId type = 0; // 0 = inferred (:= form)
  NodeId init = 0; // 0 = no initializer
  u32 generics_start = 0;
  u32 generics_count = 0;
  DeclFlags flags = DeclFlags::None;
  DeclKind kind{};
  Span span{};
};

struct ImplDecl {
  SymId type_name{};
  std::vector<SymId> generic_params; // param names from impl List<T, N>
  std::vector<Decl> methods;
  Span span{};
};

struct Module {
  std::vector<SymId> sym_list; // for module paths etc. stores SymId
  std::vector<FuncParam> params;
  std::vector<ImportDecl> imports;
  std::vector<Decl> decls;
  std::vector<ImplDecl> impls;
  std::vector<GenericParam> generic_params;
  // populated by the parser for @shader and @shader_pod declarations
  std::vector<ShaderFieldAnnot> shader_field_annots;
  std::vector<IOFieldAnnot>     io_field_annots;
  std::vector<ShaderStageInfo>  shader_stages;
  std::vector<ShaderFnInfo>     shader_fns;
};
