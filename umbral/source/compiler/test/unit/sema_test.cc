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
    IntrinsicTable intrinsics;
    intrinsics.init(interner);
    parser.emplace(lex_result->value(), intrinsics);
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
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymbolId sid = table.lookup(0, interner.intern("add"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(table.get(sid).kind, SymbolKind::Func);
}

TEST_F(SemaFixture, CollectStructType) {
  parse("const Point: type = struct { x: i32, y: i32 }");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymbolId sid = table.lookup(0, interner.intern("Point"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(table.get(sid).kind, SymbolKind::Type);
}

TEST_F(SemaFixture, CollectEnumType) {
  parse("const Color: type = enum { Red, Green, Blue }");
  SymbolTable table;
  auto r = collect_module_symbols(parser->mod, parser->body_ir,
                                  parser->type_ast, 0, table, "");
  ASSERT_FALSE(r.has_value());
  SymbolId sid = table.lookup(0, interner.intern("Color"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(table.get(sid).kind, SymbolKind::Type);
}

TEST_F(SemaFixture, CollectGlobalVar) {
  parse("var x: i32 = 0");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymbolId sid = table.lookup(0, interner.intern("x"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(table.get(sid).kind, SymbolKind::GlobalVar);
  EXPECT_TRUE(table.get(sid).is_mut);
}

TEST_F(SemaFixture, CollectConstVar) {
  parse("const x: i32 = 0");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymbolId sid = table.lookup(0, interner.intern("x"));
  EXPECT_NE(sid, kInvalidSymbol);
  EXPECT_EQ(table.get(sid).kind, SymbolKind::GlobalVar);
  EXPECT_FALSE(table.get(sid).is_mut);
}

TEST_F(SemaFixture, CollectDuplicateNameError) {
  parse("const a: i32 = 0");
  // Manually construct module with two decls using the same interned name
  Module mod = parser->mod;
  mod.decls.push_back(mod.decls[0]); // duplicate
  SymbolTable table;
  auto err = collect_module_symbols(mod, parser->body_ir, parser->type_ast, 0,
                                    table, "");
  EXPECT_TRUE(err.has_value());
}

TEST_F(SemaFixture, CollectImplMethodHasImplOwner) {
  parse(""
        "const Foo: type = struct { x: i32 }"
        "impl Foo { const get := fn (&self) -> i32 { return self.x; } }");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  // Find the impl method
  SymId foo_sym = interner.intern("Foo");
  SymId get_sym = interner.intern("get");
  SymbolId method_id = kInvalidSymbol;
  for (u32 i = 1; i < static_cast<u32>(table.symbols.size()); ++i) {
    if (table.symbols[i].name == get_sym) {
      method_id = i;
      break;
    }
  }
  ASSERT_NE(method_id, kInvalidSymbol);
  EXPECT_EQ(table.symbols[method_id].impl_owner, foo_sym);
}

TEST_F(SemaFixture, CollectExternFunc) {
  parse("@extern const abs : fn(i32) -> i32;");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymbolId sid = table.lookup(0, interner.intern("abs"));
  ASSERT_NE(sid, kInvalidSymbol);
  const Symbol &sym = table.get(sid);
  EXPECT_EQ(sym.kind, SymbolKind::Func);
  EXPECT_TRUE(sym.is_extern);
  EXPECT_EQ(sym.body, 0u) << "extern func must have no body";
  EXPECT_NE(sym.sig.ret_type, 0u)
      << "extern func must have ret_type for call type-checking";
}

TEST_F(SemaFixture, CollectExternGlobalVar) {
  parse("@extern const errno : i32;");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymbolId sid = table.lookup(0, interner.intern("errno"));
  ASSERT_NE(sid, kInvalidSymbol);
  const Symbol &sym = table.get(sid);
  EXPECT_EQ(sym.kind, SymbolKind::GlobalVar);
  EXPECT_TRUE(sym.is_extern);
  EXPECT_NE(sym.annotate_type, 0u);
}

TEST_F(SemaFixture, CollectExternConst) {
  parse("@extern const EINVAL : i32;");
  SymbolTable table;
  auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                    parser->type_ast, 0, table, "");
  ASSERT_FALSE(err.has_value());
  SymbolId sid = table.lookup(0, interner.intern("EINVAL"));
  ASSERT_NE(sid, kInvalidSymbol);
  const Symbol &sym = table.get(sid);
  EXPECT_EQ(sym.kind, SymbolKind::GlobalVar);
  EXPECT_TRUE(sym.is_extern);
  EXPECT_FALSE(sym.is_mut);
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
    SymbolTable table;
    auto err = collect_module_symbols(parser->mod, parser->body_ir,
                                      parser->type_ast, 0, table, "");
    ASSERT_FALSE(err.has_value());
    syms.emplace(table);
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
  EXPECT_EQ(types->types[*r].symbol, syms->lookup(0, foo_sym));
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
  SymbolId foo_sid = sr->syms.lookup(0, foo_sym);
  ASSERT_NE(foo_sid, kInvalidSymbol);
  SymbolId method = sr->methods.lookup(foo_sid, get_sym);
  EXPECT_NE(method, kInvalidSymbol);
}

TEST_F(SemaFixture, MethodTableMultipleImpls) {
  auto sr =
      sema(""
           "const A: type = struct { v: i32 }"
           "const B: type = struct { v: i32 }"
           "impl A { const get := fn (&self) -> i32 { return self.v; } }"
           "impl B { const get := fn (&self) -> i32 { return self.v; } }");
  ASSERT_TRUE(sr.has_value());
  SymbolId a_sid = sr->syms.lookup(0, interner.intern("A"));
  SymbolId b_sid = sr->syms.lookup(0, interner.intern("B"));
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

TEST_F(SemaFixture, SemaExternFuncCallable) {
  auto sr = sema("@extern const abs : fn(i32) -> i32;"
                 "const f := fn () -> i32 { return abs(-1); }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaExternFuncReturnTypeFlows) {
  // The return type of the extern call must unify with the declared return
  // type.
  auto sr = sema("@extern const neg : fn(i32) -> i32;"
                 "const f := fn () -> i32 { return neg(5); }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, SemaExternGlobalReadable) {
  auto sr = sema("@extern const errno : i32;"
                 "const f := fn () -> i32 { return errno; }");
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

  Result<std::vector<LoadedModule>>
  load(const std::string &entry_rel) {
    return load_modules(root / entry_rel, root, interner);
  }

  Result<SemaResult> sema(const std::string &entry_rel) {
    auto lr = load(entry_rel);
    if (!lr) return std::unexpected(lr.error());
    return run_sema(*lr, interner);
  }
};

TEST_F(MultiModFixture, LoadSingleModule) {
  write("main.um", "@pub const x: i32 = 0;");
  auto lr = load("main.um");
  ASSERT_TRUE(lr.has_value()) << (lr.has_value() ? "" : lr.error().msg);
  EXPECT_EQ(lr->size(), 1u);
}

TEST_F(MultiModFixture, LoadTwoModules) {
  write("math.um", "@pub const add := fn (a: i32, b: i32) -> i32 { return a; }");
  write("main.um", "import math;");
  auto lr = load("main.um");
  ASSERT_TRUE(lr.has_value()) << (lr.has_value() ? "" : lr.error().msg);
  EXPECT_EQ(lr->size(), 2u);
  // math should come first (dependency before dependent)
  EXPECT_EQ(lr->front().rel_path, "math");
  EXPECT_EQ(lr->back().rel_path, "main");
}

TEST_F(MultiModFixture, CrossModuleFunctionCall) {
  write("math.um", ""
                   "@pub const add := fn (a: i32, b: i32) -> i32 { return a; }");
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
  EXPECT_NE(lr.error().msg.find("circular"), std::string::npos);
}

TEST_F(MultiModFixture, MissingModuleDetected) {
  write("main.um", "import missing;");
  auto lr = load("main.um");
  EXPECT_FALSE(lr.has_value());
}

TEST_F(MultiModFixture, CrossModuleTypeAlias) {
  write("types.um", ""
                    "@pub const Point: type = struct { x: i32, y: i32 }");
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
                             "@pub const Entity: type = struct { id: u32 }");
  write("main.um", ""
                   "import game.ecs.world => world;"
                   "const Entity := world::Entity;"
                   "const f := fn (e: Entity) -> u32 { return e.id; }");
  auto sr = sema("main.um");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(MultiModFixture, NestedModulePathImplicitAlias) {
  // import game.ecs.world  →  alias = "world" (last segment)
  write("game/ecs/world.um",
        ""
        "@pub const spawn := fn (id: u32) -> u32 { return id; }");
  write("main.um", ""
                   "import game.ecs.world;"
                   "const f := fn () -> u32 { return world::spawn(1); }");
  auto sr = sema("main.um");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(MultiModFixture, TransitiveDependency) {
  // main → mid → leaf: three levels, topological order verified
  write("leaf.um", ""
                   "@pub const val := fn () -> i32 { return 42; }");
  write("mid.um", ""
                  "import leaf;"
                  "@pub const double := fn () -> i32 { return leaf::val(); }");
  write("main.um", ""
                   "import mid;"
                   "const f := fn () -> i32 { return mid::double(); }");
  auto lr = load("main.um");
  ASSERT_TRUE(lr.has_value()) << (lr.has_value() ? "" : lr.error().msg);
  ASSERT_EQ(lr->size(), 3u);
  // leaf first, then mid, then main
  EXPECT_EQ((*lr)[0].rel_path, "leaf");
  EXPECT_EQ((*lr)[1].rel_path, "mid");
  EXPECT_EQ((*lr)[2].rel_path, "main");
  auto sr = sema("main.um");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

// ============================================================
// Integer literal widening
// ============================================================

TEST_F(SemaFixture, IntLiteralWidensToU64Param) {
  // Passing an integer literal to a u64 parameter should work — the literal
  // starts as a fresh TypeVar defaulting to i32 and should be rebound to u64.
  auto sr = sema(
      "const f := fn (x: u64) -> u64 { return x; }"
      "const g := fn () -> u64 { return f(42); }");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

TEST_F(SemaFixture, IntLiteralWidensInAssign) {
  // Assigning an integer literal to a u64 variable after declaration.
  auto sr = sema(
      "const f := fn () -> void {"
      "  var x: u64 = 0;"
      "  x = 99;"
      "}");
  EXPECT_TRUE(sr.has_value()) << (sr.has_value() ? "" : sr.error().msg);
}

// ============================================================
// Non-exported symbol errors
// ============================================================

TEST_F(MultiModFixture, NonExportedFunctionError) {
  write("math.um", "const hidden := fn () -> i32 { return 42; }");
  write("main.um",
        "import math;"
        "const f := fn () -> i32 { return math::hidden(); }");
  auto sr = sema("main.um");
  EXPECT_FALSE(sr.has_value());
  if (!sr.has_value())
    EXPECT_NE(sr.error().msg.find("not exported"), std::string::npos)
        << "error was: " << sr.error().msg;
}

TEST_F(MultiModFixture, NonExportedTypeError) {
  write("types.um", "const Hidden: type = struct { x: i32 }");
  write("main.um",
        "import types;"
        "const f := fn (h: types::Hidden) -> i32 { return h.x; }");
  auto sr = sema("main.um");
  EXPECT_FALSE(sr.has_value());
  if (!sr.has_value())
    EXPECT_NE(sr.error().msg.find("not exported"), std::string::npos)
        << "error was: " << sr.error().msg;
}
