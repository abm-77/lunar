#include <gtest/gtest.h>

#include <compiler/frontend/lexer.h>
#include <compiler/frontend/parser.h>

// ============================================================
// Fixture
// ============================================================

struct ParseFixture : ::testing::Test {
  Interner interner;
  KeywordTable kws;

  // Keep LexResult alive so the Parser's TokenStream reference stays valid.
  std::optional<LexResult> lex_result;
  std::optional<Parser> p;

  void SetUp() override { kws.init(interner); }

  Parser &parse(std::string_view src) {
    lex_result.emplace(lex_source(src, interner, kws));
    p.emplace(lex_result->tokens);
    return *p;
  }

  Parser &parse_mod(std::string_view src) {
    parse(src).parse_module();
    return *p;
  }

  // Wrap body_src in a module + function so parse_module() exercises stmts.
  Parser &parse_fn(std::string_view body_src) {
    std::string src = "module t; fn f() -> void {";
    src += body_src;
    src += "}";
    return parse_mod(src);
  }

  // Returns the list of stmt NodeIds from the first function's body block.
  std::vector<NodeId> fn_stmts() {
    auto &nodes = p->body_ir.nodes;
    NodeId body = p->mod.funcs[0].body;
    u32 ls = nodes.b[body];
    u32 cnt = nodes.c[body];
    return {nodes.list.begin() + ls, nodes.list.begin() + ls + cnt};
  }
};

// ============================================================
// Module-level declarations
// ============================================================

TEST_F(ParseFixture, EmptyModule) {
  auto &p = parse_mod("module main;");
  EXPECT_FALSE(p.error().has_value());
  EXPECT_TRUE(p.mod.funcs.empty());
  EXPECT_TRUE(p.mod.structs.empty());
  EXPECT_TRUE(p.mod.imports.empty());
}

TEST_F(ParseFixture, ImportNoAlias) {
  auto &p = parse_mod("module t; import std.io;");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.imports.size(), 1u);
  EXPECT_EQ(p.mod.imports[0].path_list_count, 2u);
  EXPECT_EQ(p.mod.imports[0].alias, 0u);
}

TEST_F(ParseFixture, ImportWithAlias) {
  auto &p = parse_mod("module t; import std.io as io;");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.imports.size(), 1u);
  EXPECT_NE(p.mod.imports[0].alias, 0u);
  // alias should intern to "io"
  EXPECT_EQ(interner.view(p.mod.imports[0].alias), "io");
}

TEST_F(ParseFixture, StructDecl) {
  auto &p = parse_mod("module t; type Point = struct { x: u32, y: u32 }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.structs.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.structs[0].name), "Point");
  EXPECT_EQ(p.mod.structs[0].fields_count, 2u);
  EXPECT_FALSE(p.mod.structs[0].is_pub);
}

TEST_F(ParseFixture, PubStructDecl) {
  auto &p = parse_mod("module t; pub type Foo = struct { v: i32 }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.structs.size(), 1u);
  EXPECT_TRUE(p.mod.structs[0].is_pub);
}

TEST_F(ParseFixture, FuncDeclNoParams) {
  auto &p = parse_mod("module t; fn greet() -> void {}");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.funcs.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.funcs[0].name), "greet");
  EXPECT_EQ(p.mod.funcs[0].params_count, 0u);
  EXPECT_FALSE(p.mod.funcs[0].is_pub);
}

TEST_F(ParseFixture, PubFuncDecl) {
  auto &p = parse_mod("module t; pub fn add(a: i32, b: i32) -> i32 {}");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.funcs.size(), 1u);
  EXPECT_TRUE(p.mod.funcs[0].is_pub);
  EXPECT_EQ(p.mod.funcs[0].params_count, 2u);
  EXPECT_EQ(interner.view(p.mod.params[0].name), "a");
  EXPECT_EQ(interner.view(p.mod.params[1].name), "b");
}

TEST_F(ParseFixture, ImplBlock) {
  auto &p = parse_mod(
      "module t; impl Foo { fn bar() -> void {} pub fn baz() -> void {} }");
  EXPECT_FALSE(p.error().has_value());
  ASSERT_EQ(p.mod.impls.size(), 1u);
  EXPECT_EQ(interner.view(p.mod.impls[0].type_name), "Foo");
  EXPECT_EQ(p.mod.impls[0].methods_count, 2u);
}

// ============================================================
// Statements
// ============================================================

TEST_F(ParseFixture, LetNoTypeNoInit) {
  auto &p = parse_fn("const x;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  EXPECT_EQ(p.body_ir.nodes.kind[stmts[0]], NodeKind::LetStmt);
  EXPECT_EQ(p.body_ir.nodes.b[stmts[0]], 0u); // no type
  EXPECT_EQ(p.body_ir.nodes.c[stmts[0]], 0u); // no init
}

TEST_F(ParseFixture, LetWithTypeAndInit) {
  auto &p = parse_fn("const x: i32 = 42;");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId s = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[s], NodeKind::LetStmt);
  EXPECT_NE(p.body_ir.nodes.b[s], 0u); // has type (TypeId 0 = void, i32 is later)
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
  auto &p = parse_fn("Point { x: 1, y: 2 };");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::StructInit);
  EXPECT_EQ(p.body_ir.nodes.c[expr], 2u); // 2 field pairs
}

TEST_F(ParseFixture, StructExpr) {
  auto &p = parse_fn("const player = struct { x: i32 = 10, y: i32 = 4 };");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId let = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[let], NodeKind::LetStmt);
  NodeId init = p.body_ir.nodes.c[let];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::StructExpr);
  EXPECT_EQ(p.body_ir.nodes.c[init], 2u); // 2 fields
  // check first field name
  u32 ls = p.body_ir.nodes.b[init];
  EXPECT_EQ(interner.view(p.body_ir.nodes.list[ls]), "x");
}

TEST_F(ParseFixture, StructExprEmpty) {
  auto &p = parse_fn("const s = struct {};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId init = p.body_ir.nodes.c[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::StructExpr);
  EXPECT_EQ(p.body_ir.nodes.c[init], 0u);
}

TEST_F(ParseFixture, LambdaNoCapture) {
  auto &p = parse_fn("var add = fn (a: i32) -> i32 { return a; };");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  ASSERT_EQ(stmts.size(), 1u);
  NodeId var = stmts[0];
  EXPECT_EQ(p.body_ir.nodes.kind[var], NodeKind::VarStmt);
  NodeId init = p.body_ir.nodes.c[var];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::Lambda);
  u32 idx = p.body_ir.nodes.a[init];
  auto &lp = p.body_ir.lambdas[idx];
  EXPECT_EQ(lp.params_count, 1u);
  EXPECT_EQ(interner.view(p.mod.params[lp.params_start].name), "a");
}

TEST_F(ParseFixture, LambdaWithCapture) {
  // b is a free variable — parser just records it as an Ident, no special handling
  auto &p = parse_fn("var add = fn (a: i32) -> i32 { return a + b; };");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId init = p.body_ir.nodes.c[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::Lambda);
}

TEST_F(ParseFixture, LambdaNoParams) {
  auto &p = parse_fn("var f = fn () -> void {};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId init = p.body_ir.nodes.c[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[init], NodeKind::Lambda);
  u32 idx = p.body_ir.nodes.a[init];
  EXPECT_EQ(p.body_ir.lambdas[idx].params_count, 0u);
}

// ============================================================
// Types
// ============================================================

TEST_F(ParseFixture, NamedType) {
  auto &p = parse_mod("module t; fn f() -> i32 {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = p.mod.funcs[0].ret_type;
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Named);
  EXPECT_EQ(interner.view(p.type_ast.a[ret]), "i32");
}

TEST_F(ParseFixture, RefType) {
  auto &p = parse_mod("module t; fn f() -> &i32 {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = p.mod.funcs[0].ret_type;
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Ref);
  EXPECT_EQ(p.type_ast.a[ret], 0u); // not mut
}

TEST_F(ParseFixture, RefMutType) {
  auto &p = parse_mod("module t; fn f() -> &mut i32 {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = p.mod.funcs[0].ret_type;
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Ref);
  EXPECT_EQ(p.type_ast.a[ret], 1u); // mut
}

TEST_F(ParseFixture, TupleType) {
  auto &p = parse_mod("module t; fn f() -> (i32, u32) {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = p.mod.funcs[0].ret_type;
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Tuple);
  EXPECT_EQ(p.type_ast.c[ret], 2u);
}

TEST_F(ParseFixture, FnType) {
  auto &p = parse_mod("module t; fn f() -> fn(i32) -> void {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = p.mod.funcs[0].ret_type;
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Fn);
  EXPECT_EQ(p.type_ast.c[ret], 1u); // 1 param type
}

// ============================================================
// Array types
// ============================================================

TEST_F(ParseFixture, ArrayTypeWithCount) {
  auto &p = parse_mod("module t; fn f(x: [10]i32) -> void {}");
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
  auto &p = parse_mod("module t; fn f(x: []u8) -> void {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId param_ty = p.mod.params[0].type;
  EXPECT_EQ(p.type_ast.kind[param_ty], TypeKind::Array);
  EXPECT_EQ(p.type_ast.a[param_ty], static_cast<u32>(-1)); // no count
  TypeId elem = p.type_ast.b[param_ty];
  EXPECT_EQ(interner.view(p.type_ast.a[elem]), "u8");
}

TEST_F(ParseFixture, ArrayTypeAsReturnType) {
  auto &p = parse_mod("module t; fn f() -> [3]u8 {}");
  EXPECT_FALSE(p.error().has_value());
  TypeId ret = p.mod.funcs[0].ret_type;
  EXPECT_EQ(p.type_ast.kind[ret], TypeKind::Array);
  EXPECT_EQ(p.type_ast.a[ret], 3u);
}

TEST_F(ParseFixture, ArrayTypeNested) {
  // [2][3]i32 → array of 2 elements of type [3]i32
  auto &p = parse_mod("module t; fn f(x: [2][3]i32) -> void {}");
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
  auto &p = parse_fn("[]i32{1, 2, 3};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::ArrayLit);
  u32 idx = p.body_ir.nodes.a[expr];
  auto &al = p.body_ir.array_lits[idx];
  EXPECT_EQ(al.explicit_count, static_cast<u32>(-1));
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
  EXPECT_STREQ(p.error()->msg, "array count is less than number of initializers");
}

TEST_F(ParseFixture, ArrayLitEmpty) {
  auto &p = parse_fn("[]u8{};");
  EXPECT_FALSE(p.error().has_value());
  auto stmts = fn_stmts();
  NodeId expr = p.body_ir.nodes.a[stmts[0]];
  EXPECT_EQ(p.body_ir.nodes.kind[expr], NodeKind::ArrayLit);
  u32 idx = p.body_ir.nodes.a[expr];
  EXPECT_EQ(p.body_ir.array_lits[idx].values_count, 0u);
}

TEST_F(ParseFixture, ArrayLitTrailingComma) {
  auto &p = parse_fn("[]i32{1, 2,};");
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
  auto &p = parse_fn("[]i32{1 + 2, 3 * 4};");
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
  auto &p = parse_mod("module t; 42");
  EXPECT_TRUE(p.error().has_value());
}
