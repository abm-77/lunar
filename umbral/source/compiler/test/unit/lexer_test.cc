#include <gtest/gtest.h>

#include <compiler/frontend/lexer.h>

// ============================================================
// Helpers
// ============================================================

struct LexFixture : ::testing::Test {
  Interner interner;
  KeywordTable kws;

  void SetUp() override { kws.init(interner); }

  LexResult lex(std::string_view src) {
    return lex_source(src, interner, kws);
  }

  // Token kinds from a lex, excluding the trailing Eof.
  std::vector<TokenKind> kinds(std::string_view src) {
    auto r = lex(src);
    std::vector<TokenKind> out;
    for (auto k : r.tokens.kind)
      if (k != TokenKind::Eof) out.push_back(k);
    return out;
  }
};

// ============================================================
// TokenStream
// ============================================================

TEST(TokenStream, StartsEmpty) {
  TokenStream ts;
  EXPECT_EQ(ts.size(), 0u);
}

TEST(TokenStream, PushStoresAllFields) {
  TokenStream ts;
  ts.push(TokenKind::Plus,  {0, 1}, 10u);
  ts.push(TokenKind::Minus, {1, 2}, 20u);
  ts.push(TokenKind::Star,  {2, 3});

  ASSERT_EQ(ts.size(), 3u);
  EXPECT_EQ(ts.kind[0],    TokenKind::Plus);
  EXPECT_EQ(ts.start[0],   0u);
  EXPECT_EQ(ts.end[0],     1u);
  EXPECT_EQ(ts.payload[0], 10u);

  EXPECT_EQ(ts.kind[1],    TokenKind::Minus);
  EXPECT_EQ(ts.payload[1], 20u);

  EXPECT_EQ(ts.kind[2],    TokenKind::Star);
  EXPECT_EQ(ts.payload[2], 0u); // default payload
}

// ============================================================
// KeywordTable
// ============================================================

TEST(KeywordTable, AllKeywordsRecognised) {
  Interner I;
  KeywordTable kws;
  kws.init(I);

  struct Case { std::string_view text; TokenKind expected; };
  Case cases[] = {
    {"fn",     TokenKind::KwFn},
    {"let",    TokenKind::KwLet},
    {"mut",    TokenKind::KwMut},
    {"if",     TokenKind::KwIf},
    {"else",   TokenKind::KwElse},
    {"for",    TokenKind::KwFor},
    {"return", TokenKind::KwReturn},
    {"struct", TokenKind::KwStruct},
    {"type",   TokenKind::KwType},
    {"impl",   TokenKind::KwImpl},
    {"module", TokenKind::KwModule},
    {"import", TokenKind::KwImport},
    {"pub",    TokenKind::KwPub},
    {"true",   TokenKind::KwTrue_},
    {"false",  TokenKind::KwFalse_},
    {"var",    TokenKind::KwVar},
  };
  for (auto &c : cases) {
    SymId id = I.intern(c.text);
    auto result = kws.as_keyword(id);
    ASSERT_TRUE(result.has_value()) << "keyword not found: " << c.text;
    EXPECT_EQ(*result, c.expected) << "wrong kind for: " << c.text;
  }
}

TEST(KeywordTable, NonKeywordReturnsNullopt) {
  Interner I;
  KeywordTable kws;
  kws.init(I);
  EXPECT_FALSE(kws.as_keyword(I.intern("foobar")).has_value());
  EXPECT_FALSE(kws.as_keyword(I.intern("Fn")).has_value()); // case-sensitive
  EXPECT_FALSE(kws.as_keyword(I.intern("")).has_value());
}

// ============================================================
// Empty / whitespace / comments
// ============================================================

TEST_F(LexFixture, EmptyInputProducesEof) {
  auto r = lex("");
  ASSERT_FALSE(r.err.has_value());
  ASSERT_EQ(r.tokens.size(), 1u);
  EXPECT_EQ(r.tokens.kind[0], TokenKind::Eof);
}

TEST_F(LexFixture, WhitespaceOnlyProducesEof) {
  auto r = lex("   \t\n\r\n  ");
  ASSERT_FALSE(r.err.has_value());
  ASSERT_EQ(r.tokens.size(), 1u);
  EXPECT_EQ(r.tokens.kind[0], TokenKind::Eof);
}

TEST_F(LexFixture, CommentOnlyProducesEof) {
  auto r = lex("# this is a comment\n");
  ASSERT_FALSE(r.err.has_value());
  ASSERT_EQ(r.tokens.size(), 1u);
  EXPECT_EQ(r.tokens.kind[0], TokenKind::Eof);
}

TEST_F(LexFixture, CommentStripsToEndOfLine) {
  auto k = kinds("42 # comment\n");
  ASSERT_EQ(k.size(), 1u);
  EXPECT_EQ(k[0], TokenKind::Int);
}

TEST_F(LexFixture, MultipleCommentLines) {
  EXPECT_TRUE(kinds("# line 1\n# line 2\n").empty());
}

// ============================================================
// Punctuation — single-char
// ============================================================

TEST_F(LexFixture, SingleCharPunctuation) {
  using TK = TokenKind;
  struct Case { const char *src; TK expected; };
  Case cases[] = {
    {"(", TK::LParen},   {")", TK::RParen},
    {"{", TK::LBrace},   {"}", TK::RBrace},
    {"[", TK::LBracket}, {"]", TK::RBracket},
    {",", TK::Comma},    {".", TK::Dot},    {";", TK::Semicolon},
    {"+", TK::Plus},     {"-", TK::Minus},
    {"*", TK::Star},     {"/", TK::Slash},
    {"=", TK::Equal},    {"!", TK::Bang},
    {"<", TK::Less},     {">", TK::Greater},
    {":", TK::Colon},    {"&", TK::Ampersand},
  };
  for (auto &c : cases) {
    auto k = kinds(c.src);
    ASSERT_EQ(k.size(), 1u) << "src: " << c.src;
    EXPECT_EQ(k[0], c.expected) << "src: " << c.src;
  }
}

// ============================================================
// Operators — two-char
// ============================================================

TEST_F(LexFixture, TwoCharOperators) {
  using TK = TokenKind;
  struct Case { const char *src; TK expected; };
  Case cases[] = {
    {"==", TK::EqualEqual},  {"!=", TK::BangEqual},
    {"<=", TK::LessEqual},   {">=", TK::GreaterEqual},
    {"+=", TK::PlusEqual},   {"-=", TK::MinusEqual},
    {"*=", TK::StarEqual},   {"/=", TK::SlashEqual},
    {"->", TK::Arrow},       {"::", TK::ColonColon},
  };
  for (auto &c : cases) {
    auto k = kinds(c.src);
    ASSERT_EQ(k.size(), 1u) << "src: " << c.src;
    EXPECT_EQ(k[0], c.expected) << "src: " << c.src;
  }
}

TEST_F(LexFixture, TwoCharDoesNotConsumeExtra) {
  // "===" → EqualEqual, Equal
  auto k = kinds("===");
  ASSERT_EQ(k.size(), 2u);
  EXPECT_EQ(k[0], TokenKind::EqualEqual);
  EXPECT_EQ(k[1], TokenKind::Equal);
}

// ============================================================
// Identifiers and keywords
// ============================================================

TEST_F(LexFixture, Identifier) {
  EXPECT_EQ(kinds("foobar")[0], TokenKind::Ident);
}

TEST_F(LexFixture, IdentifierWithUnderscoreAndDigits) {
  EXPECT_EQ(kinds("my_var_2")[0], TokenKind::Ident);
}

TEST_F(LexFixture, UnderscoreOnlyIsIdent) {
  EXPECT_EQ(kinds("_")[0], TokenKind::Ident);
}

TEST_F(LexFixture, KeywordsProduceKwTokens) {
  EXPECT_EQ(kinds("fn")[0],     TokenKind::KwFn);
  EXPECT_EQ(kinds("let")[0],    TokenKind::KwLet);
  EXPECT_EQ(kinds("mut")[0],    TokenKind::KwMut);
  EXPECT_EQ(kinds("if")[0],     TokenKind::KwIf);
  EXPECT_EQ(kinds("else")[0],   TokenKind::KwElse);
  EXPECT_EQ(kinds("return")[0], TokenKind::KwReturn);
  EXPECT_EQ(kinds("struct")[0], TokenKind::KwStruct);
  EXPECT_EQ(kinds("pub")[0],    TokenKind::KwPub);
  EXPECT_EQ(kinds("true")[0],   TokenKind::KwTrue_);
  EXPECT_EQ(kinds("false")[0],  TokenKind::KwFalse_);
  EXPECT_EQ(kinds("var")[0],    TokenKind::KwVar);
}

TEST_F(LexFixture, KeywordPrefixIsIdent) {
  EXPECT_EQ(kinds("fns")[0], TokenKind::Ident);
}

TEST_F(LexFixture, IdentPayloadIsInternedSym) {
  auto r = lex("hello hello");
  ASSERT_FALSE(r.err.has_value());
  ASSERT_GE(r.tokens.size(), 2u);
  EXPECT_EQ(r.tokens.kind[0], TokenKind::Ident);
  EXPECT_EQ(r.tokens.kind[1], TokenKind::Ident);
  EXPECT_EQ(r.tokens.payload[0], r.tokens.payload[1]);
}

// ============================================================
// Integers
// ============================================================

TEST_F(LexFixture, IntegerLiteral) {
  EXPECT_EQ(kinds("123")[0], TokenKind::Int);
}

TEST_F(LexFixture, IntegerSpan) {
  auto r = lex("  42  ");
  ASSERT_FALSE(r.err.has_value());
  ASSERT_GE(r.tokens.size(), 1u);
  EXPECT_EQ(r.tokens.kind[0],  TokenKind::Int);
  EXPECT_EQ(r.tokens.start[0], 2u);
  EXPECT_EQ(r.tokens.end[0],   4u);
}

TEST_F(LexFixture, MultipleIntegers) {
  auto k = kinds("1 22 333");
  ASSERT_EQ(k.size(), 3u);
  for (auto kind : k) EXPECT_EQ(kind, TokenKind::Int);
}

// ============================================================
// String literals
// ============================================================

TEST_F(LexFixture, StringLiteral) {
  EXPECT_EQ(kinds(R"("hello")")[0], TokenKind::String);
}

TEST_F(LexFixture, EmptyStringLiteral) {
  EXPECT_EQ(kinds(R"("")")[0], TokenKind::String);
}

TEST_F(LexFixture, StringSpanIncludesQuotes) {
  auto r = lex(R"("hi")");
  ASSERT_FALSE(r.err.has_value());
  ASSERT_GE(r.tokens.size(), 1u);
  EXPECT_EQ(r.tokens.start[0], 0u);
  EXPECT_EQ(r.tokens.end[0],   4u);
}

TEST_F(LexFixture, UnterminatedStringIsError) {
  auto r = lex(R"("unterminated)");
  ASSERT_TRUE(r.err.has_value());
  EXPECT_STREQ(r.err->msg, "unterminated string");
}

TEST_F(LexFixture, StringWithNewlineIsError) {
  auto r = lex("\"line1\nline2\"");
  ASSERT_TRUE(r.err.has_value());
  EXPECT_STREQ(r.err->msg, "unterminated string");
}

// ============================================================
// Errors
// ============================================================

TEST_F(LexFixture, UnexpectedCharIsError) {
  auto r = lex("@");
  ASSERT_TRUE(r.err.has_value());
  EXPECT_STREQ(r.err->msg, "unexpected character");
}

TEST_F(LexFixture, ValidTokensBeforeErrorAreEmitted) {
  auto r = lex("fn @");
  ASSERT_TRUE(r.err.has_value());
  ASSERT_GE(r.tokens.size(), 1u);
  EXPECT_EQ(r.tokens.kind[0], TokenKind::KwFn);
}

// ============================================================
// Span accuracy
// ============================================================

TEST_F(LexFixture, IdentifierSpan) {
  auto r = lex("  foo  ");
  ASSERT_FALSE(r.err.has_value());
  ASSERT_GE(r.tokens.size(), 1u);
  EXPECT_EQ(r.tokens.start[0], 2u);
  EXPECT_EQ(r.tokens.end[0],   5u);
}

TEST_F(LexFixture, TwoCharOperatorSpan) {
  auto r = lex(" == ");
  ASSERT_FALSE(r.err.has_value());
  ASSERT_GE(r.tokens.size(), 1u);
  EXPECT_EQ(r.tokens.start[0], 1u);
  EXPECT_EQ(r.tokens.end[0],   3u);
}

// ============================================================
// Integration
// ============================================================

TEST_F(LexFixture, SimpleDeclaration) {
  using TK = TokenKind;
  auto k = kinds("let x = 42;");
  std::vector<TK> expected = {
    TK::KwLet, TK::Ident, TK::Equal, TK::Int, TK::Semicolon,
  };
  EXPECT_EQ(k, expected);
}

TEST_F(LexFixture, FunctionSignature) {
  using TK = TokenKind;
  auto k = kinds("pub fn add(a: i32, b: i32) -> i32");
  std::vector<TK> expected = {
    TK::KwPub, TK::KwFn, TK::Ident,
    TK::LParen,
    TK::Ident, TK::Colon, TK::Ident, TK::Comma,
    TK::Ident, TK::Colon, TK::Ident,
    TK::RParen, TK::Arrow, TK::Ident,
  };
  EXPECT_EQ(k, expected);
}

TEST_F(LexFixture, StructDefinition) {
  using TK = TokenKind;
  auto k = kinds("type Foo = struct { x: u32, }");
  std::vector<TK> expected = {
    TK::KwType, TK::Ident, TK::Equal, TK::KwStruct,
    TK::LBrace, TK::Ident, TK::Colon, TK::Ident, TK::Comma,
    TK::RBrace,
  };
  EXPECT_EQ(k, expected);
}

TEST_F(LexFixture, ModuleAndImport) {
  using TK = TokenKind;
  auto k = kinds("module main;\nimport std.io;");
  std::vector<TK> expected = {
    TK::KwModule, TK::Ident, TK::Semicolon,
    TK::KwImport, TK::Ident, TK::Dot, TK::Ident, TK::Semicolon,
  };
  EXPECT_EQ(k, expected);
}
