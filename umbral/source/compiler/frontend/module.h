#pragma once

#include <common/interner.h>
#include <common/types.h>
#include <compiler/frontend/ast.h>

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
  Span span{};
};

enum class DeclKind : u8 { Const, Var };

struct Decl {
  SymId name{};
  TypeId type = 0;   // 0 = inferred (:= form)
  NodeId init = 0;   // 0 = no initializer
  bool is_pub = false;
  DeclKind kind{};
  Span span{};
};

struct ImplDecl {
  SymId type_name{};
  std::vector<Decl> methods;
  Span span{};
};

struct Module {
  std::vector<u32>        sym_list; // for module paths etc. stores SymId
  std::vector<FuncParam>  params;
  std::vector<ImportDecl> imports;
  std::vector<Decl>       decls;
  std::vector<ImplDecl>   impls;
};
