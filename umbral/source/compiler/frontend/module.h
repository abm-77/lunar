#pragma once

#include <common/interner.h>
#include <common/types.h>
#include <compiler/frontend/ast.h>

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

struct Decl {
  SymId name{};
  TypeId type = 0; // 0 = inferred (:= form)
  NodeId init = 0; // 0 = no initializer
  u32 generics_start = 0;
  u32 generics_count = 0;
  bool is_pub = false;
  bool is_extern = false;
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
};
