#include <gtest/gtest.h>

#include <compiler/frontend/lexer.h>
#include <compiler/frontend/parser.h>
#include <compiler/sema/collect.h>
#include <compiler/sema/ctypes.h>
#include <compiler/sema/lower_types.h>
#include <compiler/sema/method_table.h>
#include <compiler/sema/sema.h>
#include <compiler/sema/symbol.h>

// Fixture — parses a source string and exposes sema state

struct MetaFixture : ::testing::Test {
  Interner interner;
  KeywordTable kws;

  std::optional<Result<TokenStream>> lex_result;
  std::optional<Parser> parser;

  void SetUp() override { kws.init(interner); }

  Parser &parse(std::string_view src) {
    lex_result.emplace(lex_source(src, interner, kws));
    EXPECT_TRUE(lex_result->has_value()) << "lex failed";
    IntrinsicTable intrinsics;
    intrinsics.init(interner);
    parser.emplace(lex_result->value(), intrinsics, interner);
    parser->parse_module();
    EXPECT_FALSE(parser->error().has_value())
        << "parse error: " << (parser->error() ? parser->error()->msg : "");
    return *parser;
  }

  Result<SemaResult> sema(std::string_view src) {
    parse(src);
    return run_sema(parser->mod, parser->body_ir, parser->type_ast, interner,
                    src);
  }
};

struct MetaLowerFixture : MetaFixture {
  std::optional<SymbolTable> syms;
  std::optional<TypeTable> types;
  std::optional<TypeLowerer> lowerer;

  void setup(std::string_view src) {
    parse(src);
    SymbolTable table;
    auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                      parser->type_ast, 0, table, "");
    ASSERT_FALSE(err.has_value());
    syms.emplace(table);
    types.emplace();
    lowerer.emplace(parser->type_ast, *syms, interner, *types);
  }
};

// Generic collect

TEST_F(MetaFixture, CollectGenericFunctionParams) {
  parse("const id<T: type> := fn (a: T) -> T { return a; }");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymbolId sid = table.lookup(0, interner.intern("id"));
  ASSERT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(table.get(sid).generics_count, 1u);
}

TEST_F(MetaFixture, CollectGenericImplMethodInheritsGenericParams) {
  parse(""
        "const List<T: type> : type = struct { data: T }"
        "impl List<T> { const get := fn (&self) -> T { return self.data; } }");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymId get_sym = interner.intern("get");
  for (u32 i = 1; i < static_cast<u32>(table.symbols.size()); ++i) {
    if (table.symbols[i].name == get_sym) {
      EXPECT_EQ(table.symbols[i].generics_count, 1u)
          << "method should inherit T from List";
      return;
    }
  }
  FAIL() << "get method not found";
}

// Generic type lowering

TEST_F(MetaLowerFixture, LowerTypeSubstitution) {
  setup("");
  SymId t_sym = interner.intern("T");
  CTypeId i32_ct = types->builtin(CTypeKind::I32);
  lowerer->type_subst[t_sym] = i32_ct;
  TypeId tid = parser->type_ast.make(TypeKind::Named, {0, 0}, t_sym);
  auto r = lowerer->lower(tid);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, i32_ct);
}

TEST_F(MetaLowerFixture, GenericInstantiationsAreDistinct) {
  setup("const List<T: type> : type = struct { data: T }");
  SymId list_sym = interner.intern("List");
  SymId i32_sym = interner.intern("i32");
  SymId bool_sym = interner.intern("bool");

  TypeId i32_tid = parser->type_ast.make(TypeKind::Named, {0, 0}, i32_sym);
  auto [ls1, c1] = parser->type_ast.push_list(&i32_tid, 1);
  TypeId list_i32 =
      parser->type_ast.make(TypeKind::Named, {0, 0}, list_sym, ls1, c1);

  TypeId bool_tid = parser->type_ast.make(TypeKind::Named, {0, 0}, bool_sym);
  auto [ls2, c2] = parser->type_ast.push_list(&bool_tid, 1);
  TypeId list_bool =
      parser->type_ast.make(TypeKind::Named, {0, 0}, list_sym, ls2, c2);

  auto r1 = lowerer->lower(list_i32);
  auto r2 = lowerer->lower(list_bool);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  EXPECT_NE(*r1, *r2)
      << "List<i32> and List<bool> must have different CTypeIds";
}

// Generic method table

TEST_F(MetaFixture, MethodTableGenericImpl) {
  auto sr = sema(
      ""
      "const List<T: type> : type = struct { data: T }"
      "impl List<T> { const get := fn (&self) -> T { return self.data; } }");
  ASSERT_TRUE(sr.has_value());
  SymId list_sym = interner.intern("List");
  SymId get_sym = interner.intern("get");
  SymbolId list_sid = sr->syms.lookup(0, list_sym);
  ASSERT_NE(list_sid, kInvalidSymbol);
  SymbolId method = sr->methods.lookup(list_sid, get_sym);
  EXPECT_NE(method, kInvalidSymbol)
      << "generic impl methods should be in the method table";
}

// Monomorphization

TEST_F(MetaFixture, SemaGenericFunctionMonomorphized) {
  auto sr = sema(""
                 "const id<T: type> := fn (a: T) -> T { return a; }"
                 "const f := fn () -> i32 { return id(1); }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(MetaFixture, SemaGenericSkippedWithoutCall) {
  auto sr = sema(""
                 "const add<T: type> := fn (a: T, b: T) -> T { return a; }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}
