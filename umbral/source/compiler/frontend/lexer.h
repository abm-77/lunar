#pragma once

#include <array>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <common/interner.h>
#include <common/types.h>

#include "keywords.inc"

enum class TokenKind : u16 {
  Ident,
  Int,
  Float,
  String,

#define X(name, text) Kw##name,
  UMBRAL_KEYWORDS(X)
#undef X

      LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Semicolon,
  Dot,

  Plus,
  Minus,
  Star,
  Slash,
  Equal,
  EqualEqual,
  Bang,
  BangEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  PlusEqual,
  MinusEqual,
  StarEqual,
  SlashEqual,
  Arrow,
  Colon,
  ColonColon,
  Ampersand,
  Eof,
  Count
};

enum class Kw : u16 {
#define X(name, text) name,
  UMBRAL_KEYWORDS(X)
#undef X
      Count
};

static constexpr std::array<std::string_view, static_cast<size_t>(Kw::Count)>
    KW_TEXT = {{
#define X(name, text) text,
        UMBRAL_KEYWORDS(X)
#undef X
    }};

static constexpr std::array<TokenKind, static_cast<size_t>(Kw::Count)>
    KW_TOKEN = {{
#define X(name, text) TokenKind::Kw##name,
        UMBRAL_KEYWORDS(X)
#undef X
    }};

struct KeywordTable {
  std::array<SymId, static_cast<size_t>(Kw::Count)> ids{};

  void init(Interner &I) {
    for (size_t i = 0; i < static_cast<size_t>(Kw::Count); i++)
      ids[i] = I.intern(KW_TEXT[i]);
  }

  std::optional<TokenKind> as_keyword(SymId s) const {
    for (size_t i = 0; i < static_cast<size_t>(Kw::Count); i++)
      if (ids[i] == s) return KW_TOKEN[i];
    return std::nullopt;
  }
};

struct TokenStream {
  std::vector<TokenKind> kind;
  std::vector<u32> start;
  std::vector<u32> end;
  std::vector<u32> payload;

  void push(TokenKind k, Span sp, u32 pl = 0) {
    kind.push_back(k);
    start.push_back(sp.start);
    end.push_back(sp.end);
    payload.push_back(pl);
  }

  size_t size() const { return kind.size(); }
};

struct LexError {
  Span span{};
  const char *msg;
};

struct LexResult {
  TokenStream tokens;
  std::optional<LexError> err;
};

class Lexer {
public:
  Lexer(std::string_view src, Interner &interner, const KeywordTable &kws)
      : src(src), interner(interner), kws(kws) {}

  LexResult lex_all() {
    TokenStream out;
    out.kind.reserve(src.size() / 4);
    out.start.reserve(out.kind.capacity());
    out.end.reserve(out.kind.capacity());
    out.payload.reserve(out.kind.capacity());

    while (true) {
      skip_ws_and_comments();
      u32 start = pos;

      if (eof()) {
        out.push(TokenKind::Eof, {start, start});
        return {std::move(out), std::nullopt};
      }

      char c = peek();

      if (is_alpha(c) || c == '_') {
        auto [k, pl, sp] = lex_ident_or_kw();
        out.push(k, sp, pl);
        continue;
      }

      if (is_digit(c)) {
        auto [kind, sp, value, err] = lex_number();
        if (err) return {std::move(out), LexError{sp, err}};
        out.push(kind, sp, value);
        continue;
      }

      if (c == '"') {
        auto [ok, sp] = lex_string();
        if (!ok) return {std::move(out), LexError{sp, "unterminated string"}};
        out.push(TokenKind::String, sp);
        continue;
      }

      auto op = lex_op_or_punct();
      if (!op.has_value()) {
        Span sp{start, static_cast<u32>(pos + 1)};
        return {std::move(out), LexError{sp, "unexpected character"}};
      }
      out.push(op->first, op->second);
    }
  }

private:
  std::string_view src;
  size_t pos = 0;
  Interner &interner;
  const KeywordTable &kws;

  bool eof() const { return pos >= src.size(); }
  char peek() const { return eof() ? '\0' : src[pos]; }
  char peek_next() const {
    return (pos + 1 < src.size()) ? src[pos + 1] : '\0';
  }
  void advance() {
    if (!eof()) ++pos;
  }

  static bool is_alpha(char c) { return std::isalpha(c) != 0; }
  static bool is_digit(char c) { return std::isdigit(c) != 0; }
  static bool is_alnum(char c) { return std::isalnum(c) != 0; }
  static bool is_space(char c) { return std::isspace(c) != 0; }
  static bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  }

  void skip_ws_and_comments() {
    for (;;) {
      while (!eof() && is_space(peek())) advance();

      if (!eof() && peek() == '#') {
        while (!eof() && peek() != '\n') advance();
        continue;
      }
      break;
    }
  }

  // returns (kind, payload, span)
  std::tuple<TokenKind, u32, Span> lex_ident_or_kw() {
    u32 start = pos;
    advance();

    while (!eof()) {
      char c = peek();
      if (is_alnum(c) || c == '_') advance();
      else break;
    }
    u32 end = pos;
    std::string_view lexeme = src.substr(start, end - start);

    SymId sym = interner.intern(lexeme);
    if (auto kwtok = kws.as_keyword(sym)) return {*kwtok, sym, {start, end}};

    return {TokenKind::Ident, sym, {start, end}};
  }

  // value = parsed integer value (0 for floats); err = non-null on malformed input
  struct NumResult { TokenKind kind; Span span; u32 value = 0; const char *err = nullptr; };

  NumResult lex_number() {
    u32 start = pos;
    u32 value = 0;

    // Hex: 0x / 0X
    if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
      advance(); advance();
      if (eof() || !is_hex_digit(peek()))
        return {TokenKind::Int, {start, static_cast<u32>(pos)}, 0,
                "expected hex digit after '0x'"};
      while (!eof() && is_hex_digit(peek())) {
        char c = peek();
        u32 d = is_digit(c) ? static_cast<u32>(c - '0')
                            : static_cast<u32>(std::tolower(c) - 'a' + 10);
        value = value * 16 + d;
        advance();
      }
      return {TokenKind::Int, {start, static_cast<u32>(pos)}, value};
    }

    // Binary: 0b / 0B
    if (peek() == '0' && (peek_next() == 'b' || peek_next() == 'B')) {
      advance(); advance();
      if (eof() || (peek() != '0' && peek() != '1'))
        return {TokenKind::Int, {start, static_cast<u32>(pos)}, 0,
                "expected binary digit after '0b'"};
      while (!eof() && (peek() == '0' || peek() == '1')) {
        value = value * 2 + static_cast<u32>(peek() - '0');
        advance();
      }
      return {TokenKind::Int, {start, static_cast<u32>(pos)}, value};
    }

    // Decimal integer, or float if followed by '.' + digit
    while (!eof() && is_digit(peek())) {
      value = value * 10 + static_cast<u32>(peek() - '0');
      advance();
    }

    if (!eof() && peek() == '.' && is_digit(peek_next())) {
      advance(); // consume '.'
      while (!eof() && is_digit(peek())) advance();

      // Optional exponent: e / E, optional sign, digits
      if (!eof() && (peek() == 'e' || peek() == 'E')) {
        advance();
        if (!eof() && (peek() == '+' || peek() == '-')) advance();
        if (eof() || !is_digit(peek()))
          return {TokenKind::Float, {start, static_cast<u32>(pos)}, 0,
                  "expected digit in float exponent"};
        while (!eof() && is_digit(peek())) advance();
      }
      return {TokenKind::Float, {start, static_cast<u32>(pos)}, 0};
    }

    return {TokenKind::Int, {start, static_cast<u32>(pos)}, value};
  }

  std::pair<bool, Span> lex_string() {
    u32 start = pos;
    advance();

    while (!eof() && peek() != '"') {
      if (peek() == '\n') return {false, {start, static_cast<u32>(pos)}};
      advance();
    }

    if (eof()) return {false, {start, static_cast<u32>(pos)}};
    advance();
    return {true, {start, static_cast<u32>(pos)}};
  }

  std::optional<std::pair<TokenKind, Span>> lex_op_or_punct() {
    u32 start = pos;
    char c = peek();
    auto n = peek_next();

    auto one = [&](TokenKind k) -> std::pair<TokenKind, Span> {
      advance();
      return {k, {start, static_cast<u32>(pos)}};
    };

    auto two = [&](TokenKind k) -> std::pair<TokenKind, Span> {
      advance();
      advance();
      return {k, {start, static_cast<u32>(pos)}};
    };

    switch (c) {
    case '(': return one(TokenKind::LParen);
    case ')': return one(TokenKind::RParen);
    case '{': return one(TokenKind::LBrace);
    case '}': return one(TokenKind::RBrace);
    case '[': return one(TokenKind::LBracket);
    case ']': return one(TokenKind::RBracket);
    case ',': return one(TokenKind::Comma);
    case '.': return one(TokenKind::Dot);
    case ';': return one(TokenKind::Semicolon);
    case '+':
      return (n == '=') ? two(TokenKind::PlusEqual) : one(TokenKind::Plus);
    case '-':
      if (n == '=') return two(TokenKind::MinusEqual);
      if (n == '>') return two(TokenKind::Arrow);
      return one(TokenKind::Minus);
    case '*':
      return (n == '=') ? two(TokenKind::StarEqual) : one(TokenKind::Star);
    case '/':
      return (n == '=') ? two(TokenKind::SlashEqual) : one(TokenKind::Slash);
    case '=':
      return (n == '=') ? two(TokenKind::EqualEqual) : one(TokenKind::Equal);
    case '!':
      return (n == '=') ? two(TokenKind::BangEqual) : one(TokenKind::Bang);
    case '<':
      return (n == '=') ? two(TokenKind::LessEqual) : one(TokenKind::Less);
    case '>':
      return (n == '=') ? two(TokenKind::GreaterEqual)
                        : one(TokenKind::Greater);
    case ':':
      return (n == ':') ? two(TokenKind::ColonColon) : one(TokenKind::Colon);
    case '&': return one(TokenKind::Ampersand);
    default: return std::nullopt;
    }
  }
};

LexResult lex_source(std::string_view src, Interner &interner,
                     const KeywordTable &kws) {
  Lexer lexer(src, interner, kws);
  return lexer.lex_all();
}

#undef UMBRAL_KEYWORDS
