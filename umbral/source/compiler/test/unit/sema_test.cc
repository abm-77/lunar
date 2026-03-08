#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <compiler/frontend/lexer.h>
#include <compiler/frontend/parser.h>
#include <compiler/loader.h>
#include <compiler/sema/body_check.h>
#include <compiler/sema/collect.h>
#include <compiler/sema/ctypes.h>
#include <compiler/sema/lower_types.h>
#include <compiler/sema/method_table.h>
#include <compiler/sema/sema.h>
#include <compiler/sema/symbol.h>

// ============================================================
// Fixture — parses a source string and exposes sema state
// ============================================================

struct SemaFixture : ::testing::Test {
  Interner interner;
  KeywordTable kws;

  std::optional<Result<TokenStream>> lex_result;
  std::optional<Parser> parser;

  void SetUp() override { kws.init(interner); }

  Parser &parse(std::string_view src) {
    lex_result.emplace(lex_source(src, interner, kws));
    EXPECT_TRUE(lex_result->has_value()) << "lex failed";
    parser.emplace(lex_result->value());
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

// ============================================================
// Phase 1: collect_symbols
// ============================================================

TEST_F(SemaFixture, CollectFunction) {
  parse("const add := fn (a: i32, b: i32) -> i32 { return a; }");
  auto r = collect_symbols(parser->mod, parser->body_ir, "");
  ASSERT_TRUE(r.has_value());
  SymbolId sid = r->lookup(interner.intern("add"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(r->get(sid).kind, SymbolKind::Func);
}

TEST_F(SemaFixture, CollectStructType) {
  parse("const Point: type = struct { x: i32, y: i32 }");
  auto r = collect_symbols(parser->mod, parser->body_ir, "");
  ASSERT_TRUE(r.has_value());
  SymbolId sid = r->lookup(interner.intern("Point"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(r->get(sid).kind, SymbolKind::Type);
}

TEST_F(SemaFixture, CollectEnumType) {
  parse("const Color: type = enum { Red, Green, Blue }");
  auto r = collect_symbols(parser->mod, parser->body_ir, "");
  ASSERT_TRUE(r.has_value());
  SymbolId sid = r->lookup(interner.intern("Color"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(r->get(sid).kind, SymbolKind::Type);
}

TEST_F(SemaFixture, CollectGlobalVar) {
  parse("var x: i32 = 0");
  auto r = collect_symbols(parser->mod, parser->body_ir, "");
  ASSERT_TRUE(r.has_value());
  SymbolId sid = r->lookup(interner.intern("x"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(r->get(sid).kind, SymbolKind::GlobalVar);
  EXPECT_TRUE(r->get(sid).is_mut);
}

TEST_F(SemaFixture, CollectConstVar) {
  parse("const x: i32 = 0");
  auto r = collect_symbols(parser->mod, parser->body_ir, "");
  ASSERT_TRUE(r.has_value());
  SymbolId sid = r->lookup(interner.intern("x"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(r->get(sid).kind, SymbolKind::GlobalVar);
  EXPECT_FALSE(r->get(sid).is_mut);
}

TEST_F(SemaFixture, CollectDuplicateNameError) {
  parse("const a: i32 = 0");
  // Manually construct module with two decls using the same interned name
  Module mod = parser->mod;
  mod.decls.push_back(mod.decls[0]); // duplicate
  auto r = collect_symbols(mod, parser->body_ir, "");
  EXPECT_FALSE(r.has_value());
}

TEST_F(SemaFixture, CollectGenericFunctionParams) {
  parse("const id<T: type> := fn (a: T) -> T { return a; }");
  auto r = collect_symbols(parser->mod, parser->body_ir, "");
  ASSERT_TRUE(r.has_value());
  SymbolId sid = r->lookup(interner.intern("id"));
  ASSERT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(r->get(sid).generics_count, 1u);
}

TEST_F(SemaFixture, CollectImplMethodHasImplOwner) {
  parse(""
        "const Foo: type = struct { x: i32 }"
        "impl Foo { const get := fn (&self) -> i32 { return self.x; } }");
  auto r = collect_symbols(parser->mod, parser->body_ir, "");
  ASSERT_TRUE(r.has_value());
  // Find the impl method
  SymId foo_sym = interner.intern("Foo");
  SymId get_sym = interner.intern("get");
  SymbolId method_id = kInvalidSymbol;
  for (u32 i = 1; i < static_cast<u32>(r->symbols.size()); ++i) {
    if (r->symbols[i].name == get_sym) {
      method_id = i;
      break;
    }
  }
  ASSERT_NE(method_id, kInvalidSymbol);
  EXPECT_EQ(r->symbols[method_id].impl_owner, foo_sym);
}

TEST_F(SemaFixture, CollectGenericImplMethodInheritsGenericParams) {
  parse(""
        "const List<T: type> : type = struct { data: T }"
        "impl List<T> { const get := fn (&self) -> T { return self.data; } }");
  auto r = collect_symbols(parser->mod, parser->body_ir, "");
  ASSERT_TRUE(r.has_value());
  SymId get_sym = interner.intern("get");
  for (u32 i = 1; i < static_cast<u32>(r->symbols.size()); ++i) {
    if (r->symbols[i].name == get_sym) {
      EXPECT_EQ(r->symbols[i].generics_count, 1u)
          << "method should inherit T from List";
      return;
    }
  }
  FAIL() << "get method not found";
}

// ============================================================
// Phase 2: lower_types
// ============================================================

struct LowerFixture : SemaFixture {
  std::optional<SymbolTable> syms;
  std::optional<TypeTable> types;
  std::optional<TypeLowerer> lowerer;

  void setup(std::string_view src) {
    parse(src);
    auto r = collect_symbols(parser->mod, parser->body_ir, "");
    ASSERT_TRUE(r.has_value());
    syms.emplace(std::move(*r));
    types.emplace();
    lowerer.emplace(parser->type_ast, *syms, interner, *types);
  }
};

TEST_F(LowerFixture, LowerI32) {
  setup("");
  // Make a Named TypeId for "i32"
  SymId i32_sym = interner.intern("i32");
  TypeId tid = parser->type_ast.make(TypeKind::Named, {0, 0}, i32_sym);
  auto r = lowerer->lower(tid);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, types->builtin(CTypeKind::I32));
}

TEST_F(LowerFixture, LowerBool) {
  setup("");
  SymId sym = interner.intern("bool");
  TypeId tid = parser->type_ast.make(TypeKind::Named, {0, 0}, sym);
  auto r = lowerer->lower(tid);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, types->builtin(CTypeKind::Bool));
}

TEST_F(LowerFixture, LowerRef) {
  setup("");
  SymId i32_sym = interner.intern("i32");
  TypeId inner = parser->type_ast.make(TypeKind::Named, {0, 0}, i32_sym);
  TypeId ref = parser->type_ast.make(TypeKind::Ref, {0, 0}, 0, inner);
  auto r = lowerer->lower(ref);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(types->types[*r].kind, CTypeKind::Ref);
  EXPECT_FALSE(types->types[*r].is_mut);
  EXPECT_EQ(types->types[types->types[*r].inner].kind, CTypeKind::I32);
}

TEST_F(LowerFixture, LowerMutRef) {
  setup("");
  SymId i32_sym = interner.intern("i32");
  TypeId inner = parser->type_ast.make(TypeKind::Named, {0, 0}, i32_sym);
  TypeId ref = parser->type_ast.make(TypeKind::Ref, {0, 0}, 1, inner);
  auto r = lowerer->lower(ref);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(types->types[*r].is_mut);
}

TEST_F(LowerFixture, LowerUnknownTypeError) {
  setup("");
  SymId sym = interner.intern("NoSuchType");
  TypeId tid = parser->type_ast.make(TypeKind::Named, {0, 0}, sym);
  auto r = lowerer->lower(tid);
  EXPECT_FALSE(r.has_value());
}

TEST_F(LowerFixture, LowerUserDefinedStruct) {
  setup("const Foo: type = struct { x: i32 }");
  SymId foo_sym = interner.intern("Foo");
  TypeId tid = parser->type_ast.make(TypeKind::Named, {0, 0}, foo_sym);
  auto r = lowerer->lower(tid);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(types->types[*r].kind, CTypeKind::Struct);
  EXPECT_EQ(types->types[*r].symbol, syms->lookup(foo_sym));
}

TEST_F(LowerFixture, LowerTypeSubstitution) {
  setup("");
  SymId t_sym = interner.intern("T");
  SymId i32sym = interner.intern("i32");
  CTypeId i32_ct = types->builtin(CTypeKind::I32);
  lowerer->type_subst[t_sym] = i32_ct;
  TypeId tid = parser->type_ast.make(TypeKind::Named, {0, 0}, t_sym);
  auto r = lowerer->lower(tid);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, i32_ct);
}

TEST_F(LowerFixture, GenericInstantiationsAreDistinct) {
  setup("const List<T: type> : type = struct { data: T }");
  SymId list_sym = interner.intern("List");
  SymId i32_sym = interner.intern("i32");
  SymId bool_sym = interner.intern("bool");

  // Build List<i32> type id
  TypeId i32_tid = parser->type_ast.make(TypeKind::Named, {0, 0}, i32_sym);
  auto [ls1, c1] = parser->type_ast.push_list(&i32_tid, 1);
  TypeId list_i32 =
      parser->type_ast.make(TypeKind::Named, {0, 0}, list_sym, ls1, c1);

  // Build List<bool> type id
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

// ============================================================
// Phase 3: method_table
// ============================================================

TEST_F(SemaFixture, MethodTableBasic) {
  auto sr =
      sema(""
           "const Foo: type = struct { x: i32 }"
           "impl Foo { const get := fn (&self) -> i32 { return self.x; } }");
  ASSERT_TRUE(sr.has_value());
  SymId foo_sym = interner.intern("Foo");
  SymId get_sym = interner.intern("get");
  SymbolId foo_sid = sr->syms.lookup(foo_sym);
  ASSERT_NE(foo_sid, kInvalidSymbol);
  SymbolId method = sr->methods.lookup(foo_sid, get_sym);
  EXPECT_NE(method, kInvalidSymbol);
}

TEST_F(SemaFixture, MethodTableGenericImpl) {
  auto sr = sema(
      ""
      "const List<T: type> : type = struct { data: T }"
      "impl List<T> { const get := fn (&self) -> T { return self.data; } }");
  ASSERT_TRUE(sr.has_value());
  SymId list_sym = interner.intern("List");
  SymId get_sym = interner.intern("get");
  SymbolId list_sid = sr->syms.lookup(list_sym);
  ASSERT_NE(list_sid, kInvalidSymbol);
  SymbolId method = sr->methods.lookup(list_sid, get_sym);
  EXPECT_NE(method, kInvalidSymbol)
      << "generic impl methods should be in the method table";
}

TEST_F(SemaFixture, MethodTableMultipleImpls) {
  auto sr =
      sema(""
           "const A: type = struct { v: i32 }"
           "const B: type = struct { v: i32 }"
           "impl A { const get := fn (&self) -> i32 { return self.v; } }"
           "impl B { const get := fn (&self) -> i32 { return self.v; } }");
  ASSERT_TRUE(sr.has_value());
  SymbolId a_sid = sr->syms.lookup(interner.intern("A"));
  SymbolId b_sid = sr->syms.lookup(interner.intern("B"));
  SymId get_sym = interner.intern("get");
  EXPECT_NE(sr->methods.lookup(a_sid, get_sym), kInvalidSymbol);
  EXPECT_NE(sr->methods.lookup(b_sid, get_sym), kInvalidSymbol);
  EXPECT_NE(sr->methods.lookup(a_sid, get_sym),
            sr->methods.lookup(b_sid, get_sym))
      << "A::get and B::get should be different symbols";
}

// ============================================================
// Phase 4: body_check / full sema pipeline
// ============================================================

TEST_F(SemaFixture, SemaSimpleFunction) {
  auto sr = sema("const add := fn (a: i32, b: i32) -> i32 { return a; }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaReturnTypeMismatch) {
  auto sr = sema("const f := fn () -> i32 { return true; }");
  EXPECT_FALSE(sr.has_value()) << "bool returned from i32 function should fail";
}

TEST_F(SemaFixture, SemaLetInference) {
  auto sr = sema("const f := fn () -> void { const x := 1; }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaIfCondMustBeBool) {
  auto sr = sema("const f := fn () -> void { if (1) {} }");
  EXPECT_FALSE(sr.has_value()) << "integer condition should fail";
}

TEST_F(SemaFixture, SemaIfBoolCond) {
  auto sr = sema("const f := fn () -> void { if (true) {} }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaForVarInit) {
  auto sr = sema("const f := fn () -> void {"
                 "  for (var i = 0; i < 10; i += 1) {}"
                 "}");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaStructFieldAccess) {
  auto sr = sema(""
                 "const Point: type = struct { x: i32, y: i32 }"
                 "const f := fn (p: Point) -> i32 { return p.x; }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaMethodCall) {
  auto sr = sema(""
                 "const Foo: type = struct { v: i32 }"
                 "impl Foo {"
                 "  const get := fn (&self) -> i32 { return self.v; }"
                 "}"
                 "const f := fn (x: Foo) -> i32 { return x.get(); }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaGenericFunctionMonomorphized) {
  // id<T> called with i32 arg should monomorphize and type-check cleanly
  auto sr = sema(""
                 "const id<T: type> := fn (a: T) -> T { return a; }"
                 "const f := fn () -> i32 { return id(1); }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaGenericSkippedWithoutCall) {
  // A generic function that is never called should not cause errors
  auto sr = sema(""
                 "const add<T: type> := fn (a: T, b: T) -> T { return a; }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaStaticMethodCall) {
  // Quad::max(q1, q2) — static method via Type::method path
  auto sr = sema(
      ""
      "const Quad: type = struct { w: u32, h: u32 }"
      "impl Quad {"
      "  const max := fn (q1: Quad, q2: Quad) -> Quad { return q1; }"
      "}"
      "const f := fn (a: Quad, b: Quad) -> Quad { return Quad::max(a, b); }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaImplSelfField) {
  auto sr = sema(""
                 "const Rect: type = struct { w: u32, h: u32 }"
                 "impl Rect {"
                 "  const area := fn (&self) -> u32 { return self.w * self.h; }"
                 "}");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

// ============================================================
// Multi-module fixture — writes temp files and calls load_modules
// ============================================================

struct MultiModFixture : ::testing::Test {
  std::filesystem::path root;
  Interner interner;
  KeywordTable kws;

  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("umbral_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    kws.init(interner);
  }

  void TearDown() override { std::filesystem::remove_all(root); }

  void write(const std::string &rel, std::string_view content) {
    auto p = root / rel;
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << content;
  }

  std::expected<std::vector<LoadedModule>, std::string>
  load(const std::string &entry_rel) {
    return load_modules(root / entry_rel, root, interner, kws);
  }

  Result<SemaResult> sema(const std::string &entry_rel) {
    auto lr = load(entry_rel);
    if (!lr) return std::unexpected(Error{{}, lr.error().c_str()});
    return run_sema(*lr, interner);
  }
};

TEST_F(MultiModFixture, LoadSingleModule) {
  write("main.um", "pub const x: i32 = 0;");
  auto lr = load("main.um");
  ASSERT_TRUE(lr.has_value()) << (lr.has_value() ? "" : lr.error());
  EXPECT_EQ(lr->size(), 1u);
}

TEST_F(MultiModFixture, LoadTwoModules) {
  write("math.um", "pub const add := fn (a: i32, b: i32) -> i32 { return a; }");
  write("main.um", "import math;");
  auto lr = load("main.um");
  ASSERT_TRUE(lr.has_value()) << (lr.has_value() ? "" : lr.error());
  EXPECT_EQ(lr->size(), 2u);
  // math should come first (dependency before dependent)
  EXPECT_EQ(lr->front().rel_path, "math");
  EXPECT_EQ(lr->back().rel_path, "main");
}

TEST_F(MultiModFixture, CrossModuleFunctionCall) {
  write("math.um", ""
                   "pub const add := fn (a: i32, b: i32) -> i32 { return a; }");
  write("main.um", ""
                   "import math;"
                   "const f := fn () -> i32 { return math::add(1, 2); }");
  auto sr = sema("main.um");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(MultiModFixture, CircularImportDetected) {
  write("a.um", "import b;");
  write("b.um", "import a;");
  auto lr = load("a.um");
  EXPECT_FALSE(lr.has_value());
  EXPECT_NE(lr.error().find("circular"), std::string::npos);
}

TEST_F(MultiModFixture, MissingModuleDetected) {
  write("main.um", "import missing;");
  auto lr = load("main.um");
  EXPECT_FALSE(lr.has_value());
}

TEST_F(MultiModFixture, CrossModuleTypeAlias) {
  write("types.um", ""
                    "pub const Point: type = struct { x: i32, y: i32 }");
  write("main.um", ""
                   "import types;"
                   "const Point := types::Point;"
                   "const f := fn (p: Point) -> i32 { return p.x; }");
  auto sr = sema("main.um");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(MultiModFixture, NestedModulePath) {
  // game.ecs.world → game/ecs/world.um
  write("game/ecs/world.um", ""
                             "pub const Entity: type = struct { id: u32 }");
  write("main.um", ""
                   "import game.ecs.world as world;"
                   "const Entity := world::Entity;"
                   "const f := fn (e: Entity) -> u32 { return e.id; }");
  auto sr = sema("main.um");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(MultiModFixture, NestedModulePathImplicitAlias) {
  // import game.ecs.world  →  alias = "world" (last segment)
  write("game/ecs/world.um",
        ""
        "pub const spawn := fn (id: u32) -> u32 { return id; }");
  write("main.um", ""
                   "import game.ecs.world;"
                   "const f := fn () -> u32 { return world::spawn(1); }");
  auto sr = sema("main.um");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(MultiModFixture, TransitiveDependency) {
  // main → mid → leaf: three levels, topological order verified
  write("leaf.um", ""
                   "pub const val := fn () -> i32 { return 42; }");
  write("mid.um", ""
                  "import leaf;"
                  "pub const double := fn () -> i32 { return leaf::val(); }");
  write("main.um", ""
                   "import mid;"
                   "const f := fn () -> i32 { return mid::double(); }");
  auto lr = load("main.um");
  ASSERT_TRUE(lr.has_value()) << (lr.has_value() ? "" : lr.error());
  ASSERT_EQ(lr->size(), 3u);
  // leaf first, then mid, then main
  EXPECT_EQ((*lr)[0].rel_path, "leaf");
  EXPECT_EQ((*lr)[1].rel_path, "mid");
  EXPECT_EQ((*lr)[2].rel_path, "main");
  auto sr = sema("main.um");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}
