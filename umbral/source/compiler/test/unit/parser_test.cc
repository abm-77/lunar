#include <gtest/gtest.h>

#include <compiler/frontend/lexer.h>
#include <compiler/frontend/parser.h>

// ============================================================
// Fixture
// ============================================================

struct ParseFixture : ::testing::Test {
  Interner interner;
  KeywordTable kws;

  // Keep the lex result alive so the Parser's TokenStream reference stays
  // valid.
  std::optional<Result<TokenStream>> lex_result;
  std::optional<Parser> p;

  void SetUp() override { kws.init(interner); }

  Parser &parse(std::string_view src) {
    lex_result.emplace(lex_source(src, interner, kws));
    IntrinsicTable intrinsics;
    intrinsics.init(interner);
    p.emplace(lex_result->value(), intrinsics);
    return *p;
  }

  Parser &parse_mod(std::string_view src) {
    parse(src).parse_module();
    return *p;
  }

  // Wrap body_src in a module + const fn so parse_module() exercises stmts.
  Parser &parse_fn(std::string_view body_src) {
    std::string src = "const f := fn() -> void {";
    src += body_src;
    src += "}";
    return parse_mod(src);
  }

  // Returns the list of stmt NodeIds from the first decl's fn_lit body block.
  std::vector<NodeId> fn_stmts() {
    auto &nodes = p->body_ir.nodes;
    NodeId fn_lit = p->mod.decls[0].init;
    u32 idx = nodes.a[fn_lit];
    NodeId body = p->body_ir.fn_lits[idx].body;
    u32 ls = nodes.b[body];
    u32 cnt = nodes.c[body];
    return {nodes.list.begin() + ls, nodes.list.begin() + ls + cnt};
  }
};

// ============================================================
// Module-level declarations
// ============================================================

TEST_F(ParseFixture, EmptyModule) {
  auto &p = parse_mod("");
  EXPECT_FALSE(p.error().has_value());
  EXPECT_TRUE(p.mod.decls.empty());
  EXPECT_TRUE(p.mod.imports.empty());
}

TEST_F(ParseFixture, ImportNoAlias) {
  auto &p = parse_mod("import std.io;");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.imports.size(), 1u);
  EXPECT_EQ(p.mod.imports[0].path_list_count, 2u);
  EXPECT_EQ(p.mod.imports[0].alias, 0u);
}

TEST_F(ParseFixture, ImportWithAlias) {
  auto &p = parse_mod("import std.io => io;");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.imports.size(), 1u);
  EXPECT_NE(p.mod.imports[0].alias, 0u);
  // alias should intern to "io"
  EXPECT_EQ(interner.view(p.mod.imports[0].alias), "io");
}

TEST_F(ParseFixture, StructDecl) {
  // const Point := struct { x: u32, y: u32 }  — init node is StructType
  auto &p = parse_mod("const Point := struct { x: u32, y: u32 }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.decls[0].name), "Point");
  EXPECT_FALSE(p.mod.decls[0].is_pub);
  EXPECT_EQ(p.mod.decls[0].kind, DeclKind::Const);
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::StructType);
  EXPECT_EQ(p.body_ir.nodes.c[init], 2u); // 2 fields
}

TEST_F(ParseFixture, PubStructDecl) {
  auto &p = parse_mod("pub const Foo := struct { v: i32 }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_TRUE(p.mod.decls[0].is_pub);
  EXPECT_EQ(p.body_ir.nodes.kind[p.mod.decls[0].init], NodeKind::StructType);
}

TEST_F(ParseFixture, FuncDeclNoParams) {
  // const greet := fn() -> void {}  — init node is FnLit
  auto &p = parse_mod("const greet := fn() -> void {}");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.decls[0].name), "greet");
  EXPECT_FALSE(p.mod.decls[0].is_pub);
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::FnLit);
  u32 idx = p.body_ir.nodes.a[init];
  EXPECT_EQ(p.body_ir.fn_lits[idx].params_count, 0u);
}

TEST_F(ParseFixture, PubFuncDecl) {
  auto &p = parse_mod("pub const add := fn(a: i32, b: i32) -> i32 {}");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_TRUE(p.mod.decls[0].is_pub);
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::FnLit);
  u32 idx = p.body_ir.nodes.a[init];
  EXPECT_EQ(p.body_ir.fn_lits[idx].params_count, 2u);
  u32 ps = p.body_ir.fn_lits[idx].params_start;
  EXPECT_EQ(interner.view(p.mod.params[ps].name), "a");
  EXPECT_EQ(interner.view(p.mod.params[ps + 1].name), "b");
}

TEST_F(ParseFixture, MultipleDecls) {
  auto &p = parse_mod("const x := 1; var y: i32 = 2; pub const z := 3");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 3u);
  EXPECT_EQ(p.mod.decls[0].kind, DeclKind::Const);
  EXPECT_EQ(p.mod.decls[1].kind, DeclKind::Var);
  EXPECT_TRUE(p.mod.decls[2].is_pub);
}

// ============================================================
// Statements
// ============================================================

TEST_F(ParseFixture, LetNoTypeNoInit) {
  auto &p = parse_fn("const x;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(p.body_ir.nodes.kind[stmts[0]], NodeKind::ConstStmt);
  EXPECT_EQ(p.body_ir.nodes.b[stmts[0]], 0u); // no type
  EXPECT_EQ(p.body_ir.nodes.c[stmts[0]], 0u); // no init
}

TEST_F(ParseFixture, LetWithTypeAndInit) {
  auto &p = parse_fn("const x: i32 = 42;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::ConstStmt);
  EXPECT_NE(p.body_ir.nodes.b[s],
            0u); // has type (TypeId 0 = void, i32 is later)
  // init is NodeId 0 (first node) — check it's an IntLit rather than != 0
  EXPECT_EQ(p.body_ir.nodes.kind[p.body_ir.nodes.c[s]], NodeKind::IntLit);
}

TEST_F(ParseFixture, VarStmt) {
  auto &p = parse_fn("var y: u32 = 0;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(p.body_ir.nodes.kind[stmts[0]], NodeKind::VarStmt);
}

TEST_F(ParseFixture, ReturnEmpty) {
  auto &p = parse_fn("return;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::ReturnStmt);
  EXPECT_EQ(p.body_ir.nodes.a[s], 0u);
}

TEST_F(ParseFixture, ReturnExpr) {
  auto &p = parse_fn("return 1;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::ReturnStmt);
  // return value may be NodeId 0 (first node) — check it's an IntLit
  EXPECT_EQ(p.body_ir.nodes.kind[p.body_ir.nodes.a[s]], NodeKind::IntLit);
}

TEST_F(ParseFixture, IfNoElse) {
  auto &p = parse_fn("if (true) {}");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::IfStmt);
  EXPECT_EQ(p.body_ir.nodes.c[s], 0u); // no else
}

TEST_F(ParseFixture, IfWithElse) {
  auto &p = parse_fn("if (x) {} else {}");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::IfStmt);
  EXPECT_NE(p.body_ir.nodes.c[s], 0u); // has else
}

TEST_F(ParseFixture, IfElseIf) {
  auto &p = parse_fn("if (a) {} else if (b) {} else {}");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId outer = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[outer], NodeKind::IfStmt);
  NodeId mid = p.body_ir.nodes.c[outer]; // else branch = another IfStmt
  EXPECT_EQ(p.body_ir.nodes.kind[mid], NodeKind::IfStmt);
  EXPECT_NE(p.body_ir.nodes.c[mid], 0u); // has final else block
}

TEST_F(ParseFixture, ForInfinite) {
  auto &p = parse_fn("for (;;) {}");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::ForStmt);
  u32 idx = p.body_ir.nodes.a[s];
  auto &fp = p.body_ir.fors[idx];
  EXPECT_EQ(fp.init, 0u);
  EXPECT_EQ(fp.cond, 0u);
  EXPECT_EQ(fp.step, 0u);
}

TEST_F(ParseFixture, ForCondOnly) {
  auto &p = parse_fn("for (; x;) {}");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  u32 idx = p.body_ir.nodes.a[s];
  auto &fp = p.body_ir.fors[idx];
  EXPECT_EQ(fp.init, 0u);
  // cond may be NodeId 0 (first node) — check it's an Ident rather than == 0
  EXPECT_EQ(p.body_ir.nodes.kind[fp.cond], NodeKind::Ident);
  EXPECT_EQ(fp.step, 0u);
}

TEST_F(ParseFixture, ForFull) {
  auto &p = parse_fn("for (i = 0; i < 10; i += 1) {}");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  u32 idx = p.body_ir.nodes.a[s];
  auto &fp = p.body_ir.fors[idx];
  EXPECT_NE(fp.init, 0u);
  EXPECT_NE(fp.cond, 0u);
  EXPECT_NE(fp.step, 0u);
  // step should be a compound assign
  EXPECT_EQ(p.body_ir.nodes.kind[fp.step], NodeKind::AssignStmt);
  EXPECT_EQ(static_cast<TokenKind>(p.body_ir.nodes.c[fp.step]),
            TokenKind::PlusEqual);
}

TEST_F(ParseFixture, AssignStmt) {
  auto &p = parse_fn("x = 5;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::AssignStmt);
  EXPECT_EQ(static_cast<TokenKind>(p.body_ir.nodes.c[s]), TokenKind::Equal);
}

TEST_F(ParseFixture, CompoundAssignStmt) {
  auto &p = parse_fn("x -= 3;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::AssignStmt);
  EXPECT_EQ(static_cast<TokenKind>(p.body_ir.nodes.c[s]),
            TokenKind::MinusEqual);
}

TEST_F(ParseFixture, ExprStmt) {
  auto &p = parse_fn("foo();");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(p.body_ir.nodes.kind[stmts[0]], NodeKind::ExprStmt);
}

// ============================================================
// Expressions
// ============================================================

TEST_F(ParseFixture, IntLit) {
  auto &p = parse_fn("42;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId es = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[p.body_ir.nodes.a[es]], NodeKind::IntLit);
}

TEST_F(ParseFixture, BoolLitTrue) {
  auto &p = parse_fn("true;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::BoolLit);
  EXPECT_EQ(p.body_ir.nodes.a[expr], 1u);
}

TEST_F(ParseFixture, BoolLitFalse) {
  auto &p = parse_fn("false;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::BoolLit);
  EXPECT_EQ(p.body_ir.nodes.a[expr], 0u);
}

TEST_F(ParseFixture, IdentExpr) {
  auto &p = parse_fn("myvar;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Ident);
  EXPECT_EQ(interner.view(p.body_ir.nodes.a[expr]), "myvar");
}

TEST_F(ParseFixture, BinaryAdd) {
  auto &p = parse_fn("1 + 2;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Binary);
  EXPECT_EQ(static_cast<TokenKind>(p.body_ir.nodes.a[expr]), TokenKind::Plus);
}

TEST_F(ParseFixture, BinaryPrecedenceMulBeforeAdd) {
  // 1 + 2 * 3  =>  Binary(+, 1, Binary(*, 2, 3))
  auto &p = parse_fn("1 + 2 * 3;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId root = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[root], NodeKind::Binary);
  EXPECT_EQ(static_cast<TokenKind>(p.body_ir.nodes.a[root]), TokenKind::Plus);
  NodeId rhs = p.body_ir.nodes.c[root];
  EXPECT_EQ(p.body_ir.nodes.kind[rhs], NodeKind::Binary);
  EXPECT_EQ(static_cast<TokenKind>(p.body_ir.nodes.a[rhs]), TokenKind::Star);
}

TEST_F(ParseFixture, UnaryNeg) {
  auto &p = parse_fn("-x;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Unary);
  EXPECT_EQ(static_cast<TokenKind>(p.body_ir.nodes.a[expr]), TokenKind::Minus);
}

TEST_F(ParseFixture, UnaryNot) {
  auto &p = parse_fn("!flag;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Unary);
  EXPECT_EQ(static_cast<TokenKind>(p.body_ir.nodes.a[expr]), TokenKind::Bang);
}

TEST_F(ParseFixture, CallNoArgs) {
  auto &p = parse_fn("foo();");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Call);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 0u); // 0 args
}

TEST_F(ParseFixture, CallWithArgs) {
  auto &p = parse_fn("add(1, 2, 3);");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Call);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 3u); // 3 args
}

TEST_F(ParseFixture, FieldAccess) {
  auto &p = parse_fn("obj.field;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Field);
  EXPECT_EQ(interner.view(p.body_ir.nodes.b[expr]), "field");
}

TEST_F(ParseFixture, IndexExpr) {
  auto &p = parse_fn("arr[0];");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Index);
}

TEST_F(ParseFixture, AddrOf) {
  auto &p = parse_fn("&x;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::AddrOf);
  EXPECT_EQ(p.body_ir.nodes.a[expr], 0u); // not mut
}

TEST_F(ParseFixture, AddrOfMut) {
  auto &p = parse_fn("&mut x;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::AddrOf);
  EXPECT_EQ(p.body_ir.nodes.a[expr], 1u); // is mut
}

TEST_F(ParseFixture, Deref) {
  auto &p = parse_fn("*ptr;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Deref);
}

TEST_F(ParseFixture, TupleLitEmpty) {
  auto &p = parse_fn("();");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::TupleLit);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 0u);
}

TEST_F(ParseFixture, TupleLitTwoElems) {
  auto &p = parse_fn("(a, b);");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::TupleLit);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 2u);
}

TEST_F(ParseFixture, StructInitNoFields) {
  auto &p = parse_fn("Foo {};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::StructInit);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 0u); // 0 field pairs
}

TEST_F(ParseFixture, StructInitWithFields) {
  auto &p = parse_fn("Point { x = 1, y = 2 };");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::StructInit);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 2u); // 2 field pairs
}

TEST_F(ParseFixture, StructExpr) {
  auto &p = parse_fn("const player := struct { x = 10, y = 4 };");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId let = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[let], NodeKind::ConstStmt);
  NodeId init = p.body_ir.nodes.c[let];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::StructExpr);
  EXPECT_EQ(p.body_ir.nodes.c[init], 2u); // 2 field pairs
  // check first field name (pairs: [SymId, NodeId])
  u32 ls = p.body_ir.nodes.b[init];
  EXPECT_EQ(interner.view(p.body_ir.nodes.list[ls]), "x");
}

TEST_F(ParseFixture, StructExprEmpty) {
  // struct {} with no fields → StructType (not StructExpr)
  auto &p = parse_fn("const s = struct {};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId init = p.body_ir.nodes.c[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::StructType);
  EXPECT_EQ(p.body_ir.nodes.c[init], 0u);
}

TEST_F(ParseFixture, FnLitNoCapture) {
  auto &p = parse_fn("var add = fn (a: i32) -> i32 { return a; };");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId var = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[var], NodeKind::VarStmt);
  NodeId init = p.body_ir.nodes.c[var];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::FnLit);
  u32 idx = p.body_ir.nodes.a[init];
  auto &lp = p.body_ir.fn_lits[idx];
  EXPECT_EQ(lp.params_count, 1u);
  EXPECT_EQ(interner.view(p.mod.params[lp.params_start].name), "a");
}

TEST_F(ParseFixture, FnLitWithCapture) {
  // b is a free variable — parser just records it as an Ident, no special
  // handling
  auto &p = parse_fn("var add = fn (a: i32) -> i32 { return a + b; };");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId init = p.body_ir.nodes.c[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::FnLit);
}

TEST_F(ParseFixture, FnLitNoParams) {
  auto &p = parse_fn("var f = fn () -> void {};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId init = p.body_ir.nodes.c[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::FnLit);
  u32 idx = p.body_ir.nodes.a[init];
  EXPECT_EQ(p.body_ir.fn_lits[idx].params_count, 0u);
}

// ============================================================
// Types
// ============================================================

// Helper to get the ret_type of the fn_lit stored in decls[0].init
static TypeId fn_lit_ret(Parser &p) {
  NodeId fn_lit = p.mod.decls[0].init;
  u32 idx = p.body_ir.nodes.a[fn_lit];
  return p.body_ir.fn_lits[idx].ret_type;
}

TEST_F(ParseFixture, NamedType) {
  auto &p = parse_mod("const f := fn() -> i32 {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = fn_lit_ret(p);
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Named);
  EXPECT_EQ(interner.view(p.type_ast.a[ret]), "i32");
}

TEST_F(ParseFixture, RefType) {
  auto &p = parse_mod("const f := fn() -> &i32 {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = fn_lit_ret(p);
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Ref);
  EXPECT_EQ(p.type_ast.a[ret], 0u); // not mut
}

TEST_F(ParseFixture, RefMutType) {
  auto &p = parse_mod("const f := fn() -> &mut i32 {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = fn_lit_ret(p);
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Ref);
  EXPECT_EQ(p.type_ast.a[ret], 1u); // mut
}

TEST_F(ParseFixture, TupleType) {
  auto &p = parse_mod("const f := fn() -> (i32, u32) {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = fn_lit_ret(p);
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Tuple);
  EXPECT_EQ(p.type_ast.c[ret], 2u);
}

TEST_F(ParseFixture, FnTypeAsReturnType) {
  // fn type in type position (return type annotation)
  auto &p = parse_mod("const f := fn() -> fn(i32) -> void {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = fn_lit_ret(p);
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Fn);
  EXPECT_EQ(p.type_ast.c[ret], 1u); // 1 param type
}

// ============================================================
// Array types
// ============================================================

TEST_F(ParseFixture, ArrayTypeWithCount) {
  auto &p = parse_mod("const f := fn(x: [10]i32) -> void {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId param_ty = p.mod.params[0].type;
  EXPECT_EQ(p.type_ast.kind[param_ty], TypeKind::Array);
  EXPECT_EQ(p.type_ast.a[param_ty], 10u);
  // elem type should be Named "i32"
  TypeId elem = p.type_ast.b[param_ty];
  EXPECT_EQ(p.type_ast.kind[elem], TypeKind::Named);
  EXPECT_EQ(interner.view(p.type_ast.a[elem]), "i32");
}

TEST_F(ParseFixture, ArrayTypeUnsized) {
  auto &p = parse_mod("const f := fn(x: []u8) -> void {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId param_ty = p.mod.params[0].type;
  EXPECT_EQ(p.type_ast.kind[param_ty], TypeKind::Array);
  EXPECT_EQ(p.type_ast.a[param_ty], static_cast<u32>(-1)); // no count
  TypeId elem = p.type_ast.b[param_ty];
  EXPECT_EQ(interner.view(p.type_ast.a[elem]), "u8");
}

TEST_F(ParseFixture, ArrayTypeAsReturnType) {
  auto &p = parse_mod("const f := fn() -> [3]u8 {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = fn_lit_ret(p);
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Array);
  EXPECT_EQ(p.type_ast.a[ret], 3u);
}

TEST_F(ParseFixture, ArrayTypeNested) {
  // [2][3]i32 → array of 2 elements of type [3]i32
  auto &p = parse_mod("const f := fn(x: [2][3]i32) -> void {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId outer = p.mod.params[0].type;
  EXPECT_EQ(p.type_ast.kind[outer], TypeKind::Array);
  EXPECT_EQ(p.type_ast.a[outer], 2u);
  TypeId inner = p.type_ast.b[outer];
  EXPECT_EQ(p.type_ast.kind[inner], TypeKind::Array);
  EXPECT_EQ(p.type_ast.a[inner], 3u);
}

// ============================================================
// Array literals
// ============================================================

TEST_F(ParseFixture, ArrayLitInferred) {
  // []T{...} now produces SliceLit; use explicit count for ArrayLit
  auto &p = parse_fn("[3]i32{1, 2, 3};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::ArrayLit);
  u32 idx = p.body_ir.nodes.a[expr];
  auto &al = p.body_ir.array_lits[idx];
  EXPECT_EQ(al.explicit_count, 3u);
  EXPECT_EQ(al.values_count, 3u);
  EXPECT_EQ(interner.view(p.type_ast.a[al.elem_type]), "i32");
}

TEST_F(ParseFixture, ArrayLitExplicitCount) {
  auto &p = parse_fn("[3]i32{10, 20, 30};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::ArrayLit);
  u32 idx = p.body_ir.nodes.a[expr];
  auto &al = p.body_ir.array_lits[idx];
  EXPECT_EQ(al.explicit_count, 3u);
  EXPECT_EQ(al.values_count, 3u);
}

TEST_F(ParseFixture, ArrayLitCountGreaterThanValues) {
  // [5]i32{1, 2, 3} — count 5 >= 3 values, valid
  auto &p = parse_fn("[5]i32{1, 2, 3};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  u32 idx = p.body_ir.nodes.a[expr];
  auto &al = p.body_ir.array_lits[idx];
  EXPECT_EQ(al.explicit_count, 5u);
  EXPECT_EQ(al.values_count, 3u);
}

TEST_F(ParseFixture, ArrayLitCountLessThanValuesIsError) {
  // [2]i32{1, 2, 3} — count 2 < 3 values, error
  auto &p = parse_fn("[2]i32{1, 2, 3};");
  EXPECT_TRUE(p.error().has_value());
  EXPECT_EQ(p.error()->msg, "array count is less than number of initializers");
}

TEST_F(ParseFixture, ArrayLitEmpty) {
  auto &p = parse_fn("[0]u8{};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::ArrayLit);
  u32 idx = p.body_ir.nodes.a[expr];
  EXPECT_EQ(p.body_ir.array_lits[idx].values_count, 0u);
}

TEST_F(ParseFixture, ArrayLitTrailingComma) {
  auto &p = parse_fn("[2]i32{1, 2,};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  u32 idx = p.body_ir.nodes.a[expr];
  EXPECT_EQ(p.body_ir.array_lits[idx].values_count, 2u);
}

TEST_F(ParseFixture, ArrayLitVarDecl) {
  auto &p = parse_fn("var xs: [3]i32 = [3]i32{1, 2, 3};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId var = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[var], NodeKind::VarStmt);
  // type annotation is [3]i32
  TypeId ty = p.body_ir.nodes.b[var];
  EXPECT_EQ(p.type_ast.kind[ty], TypeKind::Array);
  EXPECT_EQ(p.type_ast.a[ty], 3u);
  // initializer is ArrayLit
  NodeId init = p.body_ir.nodes.c[var];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::ArrayLit);
}

TEST_F(ParseFixture, ArrayLitElementsAreExprs) {
  auto &p = parse_fn("[2]i32{1 + 2, 3 * 4};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  u32 idx = p.body_ir.nodes.a[expr];
  auto &al = p.body_ir.array_lits[idx];
  EXPECT_EQ(al.values_count, 2u);
  // first element should be a Binary node
  NodeId first = p.body_ir.nodes.list[al.values_start];
  EXPECT_EQ(p.body_ir.nodes.kind[first], NodeKind::Binary);
}

// ============================================================
// Error cases
// ============================================================

TEST_F(ParseFixture, MissingSemicolonAfterLet) {
  auto &p = parse_fn("const x = 1");
  EXPECT_TRUE(p.error().has_value());
}

TEST_F(ParseFixture, UnknownModuleItem) {
  auto &p = parse_mod("42");
  EXPECT_TRUE(p.error().has_value());
}

// ============================================================
// New unified binding model tests
// ============================================================

TEST_F(ParseFixture, ColonEqualShorthandInStmt) {
  // const x := 42;  — no explicit type
  auto &p = parse_fn("const x := 42;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::ConstStmt);
  EXPECT_EQ(p.body_ir.nodes.b[s], 0u); // no explicit type
  EXPECT_EQ(p.body_ir.nodes.kind[p.body_ir.nodes.c[s]], NodeKind::IntLit);
}

TEST_F(ParseFixture, VarColonEqualShorthandInStmt) {
  // var x := 42;  — no explicit type
  auto &p = parse_fn("var x := 42;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(p.body_ir.nodes.kind[stmts[0]], NodeKind::VarStmt);
  EXPECT_EQ(p.body_ir.nodes.b[stmts[0]], 0u); // no explicit type
}

TEST_F(ParseFixture, StructTypeNode) {
  // const S := struct { x: u32, y: u32 }  — StructType expression node
  auto &p = parse_mod("const S := struct { x: u32, y: u32 }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::StructType);
  EXPECT_EQ(p.body_ir.nodes.c[init], 2u); // 2 fields
  // First pair in list: [SymId(x), TypeId]
  u32 ls = p.body_ir.nodes.b[init];
  EXPECT_EQ(interner.view(p.body_ir.nodes.list[ls]), "x");
}

TEST_F(ParseFixture, FnTypeExprNode) {
  // const F := fn(i32, i32) -> i32  — FnType expression node (no body)
  auto &p = parse_mod("const F := fn(i32, i32) -> i32");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::FnType);
  EXPECT_EQ(p.body_ir.nodes.c[init], 2u); // 2 param types
  // ret TypeId is in a field
  TypeId ret = p.body_ir.nodes.a[init];
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Named);
  EXPECT_EQ(interner.view(p.type_ast.a[ret]), "i32");
}

TEST_F(ParseFixture, VarWithTypeNoInit) {
  // var fp: fn(i32) -> i32  — type annotation, no initializer
  auto &p = parse_mod("var fp: fn(i32) -> i32");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_EQ(p.mod.decls[0].kind, DeclKind::Var);
  EXPECT_EQ(p.mod.decls[0].init, 0u); // no initializer
  TypeId ty = p.mod.decls[0].type;
  EXPECT_NE(ty, 0u);
  EXPECT_EQ(p.type_ast.kind[ty], TypeKind::Fn);
  EXPECT_EQ(p.type_ast.c[ty], 1u); // 1 param type
}

TEST_F(ParseFixture, ConstWithExplicitType) {
  // const a: i32 = 10  — module-level, has both explicit type and initializer
  // Use two decls so i32 is not TypeId 0 (which doubles as "no type" sentinel).
  // First decl: const f := fn() -> void {}  → parses void as TypeId 0.
  // Second decl: const a: i32 = 10         → i32 gets TypeId 1 (non-zero).
  auto &p = parse_mod("const f := fn() -> void {}; const a: i32 = 10");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 2u);
  EXPECT_EQ(p.mod.decls[1].kind, DeclKind::Const);
  TypeId ty = p.mod.decls[1].type;
  EXPECT_NE(ty, 0u); // i32 is TypeId 1, not the "no type" sentinel
  EXPECT_EQ(p.type_ast.kind[ty], TypeKind::Named);
  EXPECT_EQ(interner.view(p.type_ast.a[ty]), "i32");
  NodeId init = p.mod.decls[1].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::IntLit);
}

// ============================================================
// Path expressions
// ============================================================

TEST_F(ParseFixture, PathTwoSegments) {
  auto &p = parse_fn("Color::Red;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Path);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 2u);
  u32 ls = p.body_ir.nodes.b[expr];
  EXPECT_EQ(interner.view(p.body_ir.nodes.list[ls + 0]), "Color");
  EXPECT_EQ(interner.view(p.body_ir.nodes.list[ls + 1]), "Red");
}

TEST_F(ParseFixture, PathThreeSegments) {
  auto &p = parse_fn("a::b::c;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Path);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 3u);
}

TEST_F(ParseFixture, SingleIdentIsNotPath) {
  auto &p = parse_fn("x;");
  EXPECT_FALSE(p.error().has_value());
  NodeId expr = p.body_ir.nodes.a[fn_stmts()[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Ident);
}

TEST_F(ParseFixture, PathInComparison) {
  // color == Color::Red
  auto &p = parse_fn("color == Color::Red;");
  EXPECT_FALSE(p.error().has_value());
  NodeId expr = p.body_ir.nodes.a[fn_stmts()[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::Binary);
  NodeId rhs = p.body_ir.nodes.c[expr];
  EXPECT_EQ(p.body_ir.nodes.kind[rhs], NodeKind::Path);
}

// ============================================================
// Enum types
// ============================================================

TEST_F(ParseFixture, EnumTypeDecl) {
  auto &p = parse_mod("const Color : type = enum { Red, Green, Blue }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::EnumType);
  EXPECT_EQ(p.body_ir.nodes.c[init], 3u); // 3 variants
  u32 ls = p.body_ir.nodes.b[init];
  EXPECT_EQ(interner.view(p.body_ir.nodes.list[ls + 0]), "Red");
  EXPECT_EQ(interner.view(p.body_ir.nodes.list[ls + 1]), "Green");
  EXPECT_EQ(interner.view(p.body_ir.nodes.list[ls + 2]), "Blue");
}

TEST_F(ParseFixture, EnumTypeInferred) {
  auto &p = parse_mod("const Color2 := enum { Red, Green, Blue }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::EnumType);
  EXPECT_EQ(p.body_ir.nodes.c[init], 3u);
}

TEST_F(ParseFixture, EnumTypeTrailingComma) {
  auto &p = parse_mod("const E := enum { A, B, }");
  EXPECT_FALSE(p.error().has_value());
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::EnumType);
  EXPECT_EQ(p.body_ir.nodes.c[init], 2u);
}

TEST_F(ParseFixture, EnumTypeEmpty) {
  auto &p = parse_mod("const E := enum {}");
  EXPECT_FALSE(p.error().has_value());
  NodeId init = p.mod.decls[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::EnumType);
  EXPECT_EQ(p.body_ir.nodes.c[init], 0u);
}

// ============================================================
// impl blocks
// ============================================================

TEST_F(ParseFixture, ImplBasic) {
  auto &p = parse_mod("impl Foo { const method := fn (&self) -> void {} };");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.impls.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.impls[0].type_name), "Foo");
  ASSERT_EQ(p.mod.impls[0].methods.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.impls[0].methods[0].name), "method");
  EXPECT_EQ(p.mod.impls[0].methods[0].kind, DeclKind::Const);
}

TEST_F(ParseFixture, ImplSelfReceiver) {
  auto &p =
      parse_mod("impl Foo { const area := fn (&self) -> u32 { return 0; } };");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.impls.size(), 1u);
  NodeId init = p.mod.impls[0].methods[0].init;
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::FnLit);
  u32 idx = p.body_ir.nodes.a[init];
  auto &lp = p.body_ir.fn_lits[idx];
  EXPECT_EQ(lp.params_count, 1u);
  TypeId self_ty = p.mod.params[lp.params_start].type;
  EXPECT_EQ(p.type_ast.kind[self_ty], TypeKind::Ref);
  EXPECT_EQ(p.type_ast.a[self_ty], 0u); // not mut
  EXPECT_EQ(interner.view(p.mod.params[lp.params_start].name), "self");
}

TEST_F(ParseFixture, ImplMutSelfWithParams) {
  auto &p =
      parse_mod("impl Foo { const set := fn (&mut self, x: i32) -> void {} };");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.impls.size(), 1u);
  NodeId init = p.mod.impls[0].methods[0].init;
  u32 idx = p.body_ir.nodes.a[init];
  auto &lp = p.body_ir.fn_lits[idx];
  EXPECT_EQ(lp.params_count, 2u); // self + x
  TypeId self_ty = p.mod.params[lp.params_start].type;
  EXPECT_EQ(p.type_ast.kind[self_ty], TypeKind::Ref);
  EXPECT_EQ(p.type_ast.a[self_ty], 1u); // mut
}

TEST_F(ParseFixture, ImplStaticMethod) {
  // No receiver — regular named params only
  auto &p = parse_mod("impl Foo { const new := fn (x: i32) -> Foo {} };");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.impls.size(), 1u);
  NodeId init = p.mod.impls[0].methods[0].init;
  u32 idx = p.body_ir.nodes.a[init];
  EXPECT_EQ(p.body_ir.fn_lits[idx].params_count, 1u);
}

TEST_F(ParseFixture, ImplMultipleMethods) {
  auto &p = parse_mod("impl Foo {"
                      "  const area := fn (&self) -> u32 { return 0; }"
                      "  const max := fn (a: Foo, b: Foo) -> Foo { return a; }"
                      "};");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.impls.size(), 1u);
  EXPECT_EQ(p.mod.impls[0].methods.size(), 2u);
  EXPECT_EQ(interner.view(p.mod.impls[0].methods[0].name), "area");
  EXPECT_EQ(interner.view(p.mod.impls[0].methods[1].name), "max");
}

// ============================================================
// Generic declarations
// ============================================================

TEST_F(ParseFixture, GenericStructOneTypeParam) {
  auto &p = parse_mod("const List<T: type> : type = struct { data: T }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.decls[0].name), "List");
  EXPECT_EQ(p.mod.decls[0].generics_count, 1u);
  EXPECT_TRUE(p.mod.generic_params[p.mod.decls[0].generics_start].is_type);
  EXPECT_EQ(
      interner.view(p.mod.generic_params[p.mod.decls[0].generics_start].name),
      "T");
}

TEST_F(ParseFixture, GenericStructTwoParams) {
  auto &p =
      parse_mod("const List<T: type, N: i32> : type = struct { data: [N]T }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_EQ(p.mod.decls[0].generics_count, 2u);
  u32 gs = p.mod.decls[0].generics_start;
  EXPECT_TRUE(p.mod.generic_params[gs].is_type);      // T: type
  EXPECT_FALSE(p.mod.generic_params[gs + 1].is_type); // N: i32
  EXPECT_EQ(interner.view(p.mod.generic_params[gs].name), "T");
  EXPECT_EQ(interner.view(p.mod.generic_params[gs + 1].name), "N");
}

TEST_F(ParseFixture, GenericFunctionInferred) {
  auto &p = parse_mod("const id<T: type> := fn (a: T) -> T { return a; }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_EQ(p.mod.decls[0].generics_count, 1u);
  // Should parse as a FnLit (has body)
  EXPECT_EQ(p.body_ir.nodes.kind[p.mod.decls[0].init], NodeKind::FnLit);
}

TEST_F(ParseFixture, GenericFunctionExplicitType) {
  auto &p = parse_mod(
      "const add<T: type>: fn(T,T)->T = fn(a: T, b: T)->T { return a; }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  EXPECT_EQ(p.mod.decls[0].generics_count, 1u);
}

TEST_F(ParseFixture, GenericImplBlock) {
  auto &p = parse_mod("const List<T: type> : type = struct { data: T }"
                      "impl List<T> {"
                      "  const get := fn (&self) -> T { return self.data; }"
                      "}");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.impls.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.impls[0].type_name), "List");
  EXPECT_EQ(p.mod.impls[0].generic_params.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.impls[0].generic_params[0]), "T");
  EXPECT_EQ(p.mod.impls[0].methods.size(), 1u);
}

TEST_F(ParseFixture, ForVarInit) {
  auto &p = parse_fn("for (var i = 0; i < 10; i += 1) {}");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(p.body_ir.nodes.kind[stmts[0]], NodeKind::ForStmt);
  // The for init should be a VarStmt
  u32 for_idx = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[p.body_ir.fors[for_idx].init],
            NodeKind::VarStmt);
}

TEST_F(ParseFixture, ForConstInit) {
  auto &p = parse_fn("for (const i = 0; i < 10; i += 1) {}");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  u32 for_idx = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[p.body_ir.fors[for_idx].init],
            NodeKind::ConstStmt);
}

TEST_F(ParseFixture, TypeArgInTypeAnnotation) {
  // List<i32> in type position should parse without error
  auto &p = parse_mod("var x: List<i32>;");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  // The type annotation TypeId should be a Named node with 1 type arg
  TypeId ty = p.mod.decls[0].type;
  EXPECT_EQ(p.type_ast.kind[ty], TypeKind::Named);
  EXPECT_EQ(p.type_ast.c[ty], 1u); // 1 type arg
}

TEST_F(ParseFixture, TypeArgTwoArgs) {
  auto &p = parse_mod("var x: Map<i32, bool>;");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  TypeId ty = p.mod.decls[0].type;
  EXPECT_EQ(p.type_ast.kind[ty], TypeKind::Named);
  EXPECT_EQ(p.type_ast.c[ty], 2u); // 2 type args
}

TEST_F(ParseFixture, ArrayTypeIdentCount) {
  // [N]T — identifier used as array count (const generic param)
  auto &p = parse_mod("var x: [N]i32;");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.decls.size(), 1u);
  TypeId ty = p.mod.decls[0].type;
  EXPECT_EQ(p.type_ast.kind[ty], TypeKind::Array);
  // count stored as -1 (unsized) since N is not a literal
  EXPECT_EQ(p.type_ast.a[ty], static_cast<u32>(-1));
}
