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

struct FieldDecl {
  SymId name{};
  TypeId type{};
  Span span{};
};

struct StructDecl {
  SymId name{};
  u32 fields_start = 0;
  u32 fields_count = 0;
  bool is_pub = false;
  Span span{};
};

struct FuncParam {
  SymId name{};
  TypeId type{};
  Span span{};
};

struct FuncDecl {
  SymId name{};
  u32 params_start = 0;
  u32 params_count = 0;
  TypeId ret_type = 0;
  NodeId body = 0; // block node in ExprAst
  bool is_pub = false;
  Span span{};
};

struct ImplDecl {
  SymId type_name{};
  u32 methods_start = 0;
  u32 methods_count = 0;
  bool is_pub = false;
  Span span{};
};

struct Module {
  std::vector<u32> sym_list; // for module paths etc. stores SymId
  std::vector<FieldDecl> fields;
  std::vector<FuncParam> params;
  std::vector<ImportDecl> imports;
  std::vector<StructDecl> structs;
  std::vector<FuncDecl> funcs;
  std::vector<ImplDecl> impls;
};
