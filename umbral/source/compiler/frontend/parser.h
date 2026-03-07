#pragma once

#include <optional>
#include <vector>

#include <common/interner.h>
#include <common/types.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/module.h>

struct ParserError {
  Span span{};
  const char *msg;
};

class Parser {
public:
  Parser(const TokenStream &ts) : t(ts) {}
  std::optional<ParserError> error() const { return err; }

  BodyIR body_ir;
  TypeAst type_ast;
  Module mod;

  void parse_module() {
    while (!at(TokenKind::Eof) && !err) parse_module_item();
  }

private:
  const TokenStream &t;
  u32 i = 0;
  std::optional<ParserError> err;

  // token helpers
  TokenKind k(u32 look = 0) const { return t.kind[i + look]; }
  Span sp(u32 look = 0) const { return {t.start[i + look], t.end[i + look]}; }
  u32 pl(u32 look = 0) const { return t.payload[i + look]; }
  bool at(TokenKind kk) const { return k() == kk; }
  bool match(TokenKind kk) {
    if (k() == kk) {
      ++i;
      return true;
    }
    return false;
  }
  void expect(TokenKind kk, const char *msg) {
    if (!match(kk)) set_error(sp(), msg);
  }
  void set_error(Span s, const char *msg) {
    if (!err) err = ParserError{s, msg};
  }

  // util
  SymId expect_ident(const char *msg) {
    if (!match(TokenKind::Ident)) {
      set_error(sp(), msg);
      return 0;
    }
    return static_cast<SymId>(t.payload[i - 1]);
  }

  // module path: Ident ('.' Ident)*
  std::pair<u32, u32> parse_module_path() {
    std::vector<u32> segs;
    segs.push_back(expect_ident("expected module name"));
    while (match(TokenKind::Dot))
      segs.push_back(expect_ident("expected module segment"));

    u32 start = mod.sym_list.size();
    mod.sym_list.insert(mod.sym_list.end(), segs.begin(), segs.end());
    return {start, static_cast<u32>(segs.size())};
  }

  // type
  // Type :=  '&' ['mut'] Type
  //      | '(' Type (',' Type)+ ')' (tuples)
  //      | 'fn' '(' [Type, (',' Type)*] ')' '->' Type (functions)
  //      | Ident ( '::' Ident )* (named path)
  TypeId parse_type() {
    auto start = sp();

    // & / &mut
    if (match(TokenKind::Ampersand)) {
      bool is_mut = match(TokenKind::KwMut);
      TypeId inner = parse_type();
      Span end = {start.start, type_ast.span_e[inner]};
      return type_ast.make(TypeKind::Ref, end, is_mut ? 1u : 0u, inner);
    }

    // tuple type
    if (match(TokenKind::LParen)) {
      TypeId first = parse_type();
      if (!match(TokenKind::Comma)) {
        expect(TokenKind::RParen, "expected ')'");
        // treat (T) as just T
        return first;
      }

      std::vector<u32> elems;
      elems.push_back(first);
      do {
        elems.push_back(parse_type());
      } while (match(TokenKind::Comma));
      expect(TokenKind::RParen, "expected ')'");
      auto [ls, cnt] = type_ast.push_list(elems.data(), elems.size());
      Span endsp = {start.start, t.end[i - 1]};
      return type_ast.make(TypeKind::Tuple, endsp, 0, ls, cnt);
    }

    // fn type
    if (match(TokenKind::KwFn)) {
      expect(TokenKind::LParen, "expected '('");
      std::vector<u32> params;
      if (!match(TokenKind::RParen)) {
        do {
          params.push_back(parse_type());
        } while (match(TokenKind::Comma));
        expect(TokenKind::RParen, "expected ')'");
      }
      expect(TokenKind::Arrow, "expected '->'");
      TypeId ret = parse_type();
      auto [ls, cnt] = type_ast.push_list(params.data(), params.size());
      Span endsp = {start.start, type_ast.span_e[ret]};
      return type_ast.make(TypeKind::Fn, endsp, ret, ls, cnt);
    }
    // array type: '[' [count] ']' elem_type
    if (match(TokenKind::LBracket)) {
      u32 count = static_cast<u32>(-1);
      if (!at(TokenKind::RBracket)) {
        if (!at(TokenKind::Int)) {
          set_error(sp(), "expected integer count in array type");
          return type_ast.make(TypeKind::Named, start, 0);
        }
        count = t.payload[i];
        ++i;
      }
      expect(TokenKind::RBracket, "expected ']'");
      TypeId elem = parse_type();
      Span endsp = {start.start, type_ast.span_e[elem]};
      return type_ast.make(TypeKind::Array, endsp, count, elem);
    }

    // named type: Ident ( :: Ident)*
    SymId name = expect_ident("expected type name");
    while (match(TokenKind::ColonColon))
      name = expect_ident("expected type segment");
    Span endsp = {start.start, t.end[i - 1]};
    return type_ast.make(TypeKind::Named, endsp, name);
  }

  // expressions

  // binding power
  struct BP {
    i32 lbp, rbp;
  };

  static i32 postfix_bp(TokenKind kk) {
    switch (kk) {
    case TokenKind::Dot:
    case TokenKind::LBracket:
    case TokenKind::LParen: return 90;
    default: return -1;
    }
  }

  static bool is_assign(TokenKind kk) {
    switch (kk) {
    case TokenKind::Equal:
    case TokenKind::PlusEqual:
    case TokenKind::MinusEqual:
    case TokenKind::StarEqual:
    case TokenKind::SlashEqual: return true;
    default: return false;
    }
  }

  static std::optional<BP> infix_bp(TokenKind kk) {
    switch (kk) {
    case TokenKind::Star: return BP{70, 71};
    case TokenKind::Slash: return BP{70, 71};
    case TokenKind::Plus: return BP{60, 61};
    case TokenKind::Minus: return BP{60, 61};
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual: return BP{50, 51};
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual: return BP{40, 41};
    default: return std::nullopt;
    }
  }

  NodeId parse_expr() { return parse_pratt(0); }

  NodeId parse_primary() {
    auto s = sp();

    // fn ( <params> ) -> <ret> <block>
    if (match(TokenKind::KwFn)) {
      expect(TokenKind::LParen, "expected '('");
      u32 params_start = static_cast<u32>(mod.params.size());
      if (!match(TokenKind::RParen)) {
        do {
          auto ps = sp();
          SymId pname = expect_ident("expected parameter name");
          expect(TokenKind::Colon, "expected ':' after parameter name");
          TypeId ptype = parse_type();
          mod.params.push_back({pname, ptype, {ps.start, t.end[i - 1]}});
        } while (match(TokenKind::Comma));
        expect(TokenKind::RParen, "expected ')'");
      }
      u32 params_count = static_cast<u32>(mod.params.size()) - params_start;
      expect(TokenKind::Arrow, "expected '->'");
      TypeId ret = parse_type();
      NodeId body = parse_block();
      u32 idx = static_cast<u32>(body_ir.lambdas.size());
      body_ir.lambdas.push_back({params_start, params_count, ret, body});
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::Lambda, endsp, idx);
    }

    // struct { field: Type = expr, ... }
    if (match(TokenKind::KwStruct)) {
      expect(TokenKind::LBrace, "expected '{'");
      std::vector<u32> packed; // triples: [SymId, TypeId, NodeId]
      if (!match(TokenKind::RBrace)) {
        do {
          SymId fname = expect_ident("expected field name");
          expect(TokenKind::Colon, "expected ':'");
          TypeId ftype = parse_type();
          expect(TokenKind::Equal, "expected '='");
          NodeId fval = parse_expr();
          packed.push_back(fname);
          packed.push_back(ftype);
          packed.push_back(fval);
        } while (!err && match(TokenKind::Comma) && !at(TokenKind::RBrace));
        expect(TokenKind::RBrace, "expected '}'");
      }
      u32 field_count = static_cast<u32>(packed.size()) / 3;
      auto [ls, _] = body_ir.nodes.push_list(packed.data(), packed.size());
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::StructExpr, endsp, 0, ls,
                                field_count);
    }

    if (match(TokenKind::Int)) return body_ir.nodes.make(NodeKind::IntLit, s);
    if (match(TokenKind::String))
      return body_ir.nodes.make(NodeKind::StrLit, s);
    if (match(TokenKind::KwTrue_))
      return body_ir.nodes.make(NodeKind::BoolLit, s, 1);
    if (match(TokenKind::KwFalse_))
      return body_ir.nodes.make(NodeKind::BoolLit, s, 0);

    if (match(TokenKind::Ident)) {
      SymId sym = static_cast<SymId>(t.payload[i - 1]);

      // struct literal: TypeName { field: expr, ...}
      if (match(TokenKind::LBrace)) {
        // parse fields
        std::vector<u32> packed; // [ fieldSym, exprId, fieldSym, exprId, ...]
        if (!match(TokenKind::RBrace)) {
          do {
            SymId field = expect_ident("expected field name");
            expect(TokenKind::Colon, "expected ':' in struct literal");
            NodeId val = parse_expr();
            packed.push_back(field);
            packed.push_back(val);
          } while (!err && match(TokenKind::Comma) && !at(TokenKind::RBrace));
          expect(TokenKind::RBrace, "expected '}'");
        }
        u32 start = body_ir.nodes.list.size();
        body_ir.nodes.list.insert(body_ir.nodes.list.end(), packed.begin(),
                                  packed.end());
        u32 pairs = packed.size() / 2;
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::StructInit, endsp, sym, start,
                                  pairs);
      }

      return body_ir.nodes.make(NodeKind::Ident, s, sym);
    }

    if (match(TokenKind::LParen)) {
      // tuple literal or parens expr
      if (match(TokenKind::RParen)) {
        // unit as empty tuple
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::TupleLit, endsp, 0, 0, 0);
      }
      NodeId first = parse_expr();
      if (match(TokenKind::Comma)) {
        std::vector<u32> elems;
        elems.push_back(first);
        do {
          elems.push_back(parse_expr());
        } while (match(TokenKind::Comma));
        expect(TokenKind::RParen, "expected ')'");
        auto [ls, cnt] = body_ir.nodes.push_list(elems.data(), elems.size());
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::TupleLit, endsp, 0, ls, cnt);
      }
      expect(TokenKind::RParen, "expected ')'");
      return first;
    }

    // [<opt_count>]<type>{<values>}  — array literal
    if (match(TokenKind::LBracket)) {
      u32 explicit_count = static_cast<u32>(-1);
      if (!at(TokenKind::RBracket)) {
        if (!at(TokenKind::Int)) {
          set_error(sp(), "expected integer count or ']' in array literal");
          return body_ir.nodes.make(NodeKind::Ident, s, 0);
        }
        explicit_count = t.payload[i];
        ++i;
      }
      expect(TokenKind::RBracket, "expected ']'");
      TypeId elem_type = parse_type();
      expect(TokenKind::LBrace, "expected '{'");
      std::vector<NodeId> values;
      if (!at(TokenKind::RBrace)) {
        do {
          values.push_back(parse_expr());
        } while (!err && match(TokenKind::Comma) && !at(TokenKind::RBrace));
      }
      expect(TokenKind::RBrace, "expected '}'");
      if (!err && explicit_count != static_cast<u32>(-1) &&
          explicit_count < static_cast<u32>(values.size())) {
        set_error({s.start, t.end[i - 1]},
                  "array count is less than number of initializers");
      }
      auto [vs, vc] = body_ir.nodes.push_list(values.data(),
                                               static_cast<u32>(values.size()));
      u32 idx = static_cast<u32>(body_ir.array_lits.size());
      body_ir.array_lits.push_back({explicit_count, elem_type, vs, vc});
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::ArrayLit, endsp, idx);
    }

    set_error(s, "expected expression");
    return body_ir.nodes.make(NodeKind::Ident, s, 0);
  }

  NodeId parse_place() {
    // place := primary ( '.' Ident | '[' expr ']' )*
    NodeId base = parse_primary();
    for (;;) {
      if (match(TokenKind::Dot)) {
        SymId field = expect_ident("expected field name");
        Span endsp = {body_ir.nodes.span_s[base], t.end[i - 1]};
        base = body_ir.nodes.make(NodeKind::Field, endsp, base, field);
        continue;
      }
      if (match(TokenKind::LBracket)) {
        NodeId idx = parse_expr();
        expect(TokenKind::RBracket, "expected ']");
        Span endsp = {body_ir.nodes.span_s[base], t.end[i - 1]};
        base = body_ir.nodes.make(NodeKind::Index, endsp, base, idx);
        continue;
      }
      break;
    }
    return base;
  }

  NodeId parse_nud() {
    auto s = sp();
    // & / &mut place
    if (match(TokenKind::Ampersand)) {
      bool is_mut = match(TokenKind::KwMut);
      NodeId place = parse_place();
      Span endsp = {s.start, body_ir.nodes.span_e[place]};
      return body_ir.nodes.make(NodeKind::AddrOf, endsp, is_mut ? 1u : 0u,
                                place);
    }

    // *expr
    if (match(TokenKind::Star)) {
      NodeId rhs = parse_pratt(80);
      Span endsp = {s.start, body_ir.nodes.span_e[rhs]};
      return body_ir.nodes.make(NodeKind::Deref, endsp, rhs);
    }

    // unary - !
    if (k() == TokenKind::Minus || k() == TokenKind::Bang) {
      TokenKind op = k();
      match(op);
      NodeId rhs = parse_pratt(80);
      Span endsp = {s.start, body_ir.nodes.span_e[rhs]};
      return body_ir.nodes.make(NodeKind::Unary, endsp, static_cast<u32>(op),
                                rhs);
    }

    return parse_primary();
  }

  NodeId parse_pratt(int min_bp) {
    NodeId lhs = parse_nud();

    for (;;) {
      TokenKind op = k();

      // postfix
      int pb = postfix_bp(op);
      if (pb >= 0 && pb >= min_bp) {
        if (match(TokenKind::Dot)) {
          SymId field = expect_ident("expected field  name");
          Span endsp = {body_ir.nodes.span_s[lhs], t.end[i - 1]};
          lhs = body_ir.nodes.make(NodeKind::Field, endsp, lhs, field);
          continue;
        }
        if (match(TokenKind::LBracket)) {
          NodeId idx = parse_expr();
          expect(TokenKind::RBracket, "expected ']'");
          Span endsp = {body_ir.nodes.span_s[lhs], t.end[i - 1]};
          lhs = body_ir.nodes.make(NodeKind::Index, endsp, lhs, idx);
          continue;
        }
        if (match(TokenKind::LParen)) {
          std::vector<u32> args;
          if (!match(TokenKind::RParen)) {
            do {
              args.push_back(parse_expr());
            } while (match(TokenKind::Comma));
            expect(TokenKind::RParen, "expected ')'");
          }
          auto [ls, cnt] = body_ir.nodes.push_list(args.data(), args.size());
          Span endsp = {body_ir.nodes.span_s[lhs], t.end[i - 1]};
          lhs = body_ir.nodes.make(NodeKind::Call, endsp, lhs, ls, cnt);
          continue;
        }
      }

      // infix
      auto bp = infix_bp(op);
      if (!bp || bp->lbp < min_bp) break;

      match(op);
      NodeId rhs = parse_pratt(bp->rbp);
      Span endsp = {body_ir.nodes.span_s[lhs], body_ir.nodes.span_e[rhs]};
      lhs = body_ir.nodes.make(NodeKind::Binary, endsp, static_cast<u32>(op),
                               lhs, rhs);
    }

    return lhs;
  }

  // statements

  NodeId parse_stmt() {
    auto s = sp();

    // let <name> [: <type>] [= <expr>] ;
    if (match(TokenKind::KwConst)) {
      SymId name = expect_ident("expected variable name");
      TypeId ty = 0;
      if (match(TokenKind::Colon)) ty = parse_type();
      NodeId init = 0;
      if (match(TokenKind::Equal)) init = parse_expr();
      expect(TokenKind::Semicolon, "expected ';'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::LetStmt, endsp, name, ty, init);
    }

    // var <name> [: <type>] [= <expr>] ;
    if (match(TokenKind::KwVar)) {
      SymId name = expect_ident("expected variable name");
      TypeId ty = 0;
      if (match(TokenKind::Colon)) ty = parse_type();
      NodeId init = 0;
      if (match(TokenKind::Equal)) init = parse_expr();
      expect(TokenKind::Semicolon, "expected ';'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::VarStmt, endsp, name, ty, init);
    }

    // return [<expr>] ;
    if (match(TokenKind::KwReturn)) {
      NodeId val = 0;
      if (!at(TokenKind::Semicolon)) val = parse_expr();
      expect(TokenKind::Semicolon, "expected ';'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::ReturnStmt, endsp, val);
    }

    // if ( <expr> ) <block> [else <block>]
    if (match(TokenKind::KwIf)) {
      expect(TokenKind::LParen, "expected '(' after 'if'");
      NodeId cond = parse_expr();
      expect(TokenKind::RParen, "expected ')' after if condition");
      NodeId then = parse_block();
      NodeId else_ = 0;
      if (match(TokenKind::KwElse))
        else_ = at(TokenKind::KwIf) ? parse_stmt() : parse_block();
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::IfStmt, endsp, cond, then, else_);
    }

    // for ( [init] ; [cond] ; [step] ) <block>
    if (match(TokenKind::KwFor)) {
      expect(TokenKind::LParen, "expected '(' after 'for'");
      NodeId init = 0;
      if (!at(TokenKind::Semicolon)) {
        auto ps = sp();
        NodeId lhs = parse_expr();
        if (is_assign(k())) {
          TokenKind op = k();
          match(op);
          NodeId rhs = parse_expr();
          Span endsp = {ps.start, body_ir.nodes.span_e[rhs]};
          init = body_ir.nodes.make(NodeKind::AssignStmt, endsp, lhs, rhs,
                                    static_cast<u32>(op));
        } else {
          init = body_ir.nodes.make(NodeKind::ExprStmt,
                                    {ps.start, body_ir.nodes.span_e[lhs]}, lhs);
        }
      }
      expect(TokenKind::Semicolon, "expected ';' after for-init");
      NodeId cond = at(TokenKind::Semicolon) ? 0 : parse_expr();
      expect(TokenKind::Semicolon, "expected ';' after for-cond");
      NodeId step = 0;
      if (!at(TokenKind::RParen)) {
        auto ps = sp();
        NodeId lhs = parse_expr();
        if (is_assign(k())) {
          TokenKind op = k();
          match(op);
          NodeId rhs = parse_expr();
          Span endsp = {ps.start, body_ir.nodes.span_e[rhs]};
          step = body_ir.nodes.make(NodeKind::AssignStmt, endsp, lhs, rhs,
                                    static_cast<u32>(op));
        } else {
          step = body_ir.nodes.make(NodeKind::ExprStmt,
                                    {ps.start, body_ir.nodes.span_e[lhs]}, lhs);
        }
      }
      expect(TokenKind::RParen, "expected ')' after for-step");
      NodeId body = parse_block();
      u32 idx = static_cast<u32>(body_ir.fors.size());
      body_ir.fors.push_back({init, cond, step, body});
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::ForStmt, endsp, idx);
    }

    if (match(TokenKind::LBrace)) return parse_block();

    // <expr> [<assign-op> <expr>] ';'  ->  AssignStmt or ExprStmt
    NodeId lhs = parse_expr();
    if (is_assign(k())) {
      TokenKind op = k();
      match(op);
      NodeId rhs = parse_expr();
      expect(TokenKind::Semicolon, "expected ';'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::AssignStmt, endsp, lhs, rhs,
                                static_cast<u32>(op));
    }
    expect(TokenKind::Semicolon, "expected ';'");
    Span endsp = {s.start, t.end[i - 1]};
    return body_ir.nodes.make(NodeKind::ExprStmt, endsp, lhs);
  }

  NodeId parse_block() {
    auto s = sp();
    expect(TokenKind::LBrace, "expect '{");
    std::vector<u32> stmts;
    while (!at(TokenKind::RBrace) && !at(TokenKind::Eof) && !err) {
      stmts.push_back(parse_stmt());
    }
    expect(TokenKind::RBrace, "expect '}'");
    auto [ls, cnt] = body_ir.nodes.push_list(stmts.data(), stmts.size());
    Span endsp = {s.start, t.end[i - 1]};
    return body_ir.nodes.make(NodeKind::Block, endsp, 0, ls, cnt);
  }

  FuncDecl parse_function_decl(bool is_method, bool is_pub) {
    auto s = sp();
    expect(TokenKind::KwFn, "expected 'fn'");
    SymId name = expect_ident("expected function name");

    expect(TokenKind::LParen, "expected '('");
    u32 params_start = static_cast<u32>(mod.params.size());
    if (!match(TokenKind::RParen)) {
      do {
        auto ps = sp();
        SymId pname = expect_ident("expected parameter name");
        expect(TokenKind::Colon, "expected ':' after parameter name");
        TypeId ptype = parse_type();
        Span pspan = {ps.start, t.end[i - 1]};
        mod.params.push_back({pname, ptype, pspan});
      } while (match(TokenKind::Comma));
      expect(TokenKind::RParen, "expected ')'");
    }
    u32 params_count = static_cast<u32>(mod.params.size()) - params_start;

    expect(TokenKind::Arrow, "expected '->'");
    TypeId ret = parse_type();
    NodeId body = parse_block();
    Span endsp = {s.start, t.end[i - 1]};
    return {name, params_start, params_count, ret, body, is_pub, endsp};
  }

  void parse_module_item() {
    auto s = sp();

    // module <name> ; — consume module declaration
    if (match(TokenKind::KwModule)) {
      parse_module_path();
      expect(TokenKind::Semicolon, "expected ';'");
      return;
    }

    bool is_pub = match(TokenKind::KwPub);

    // import <path> [as <ident>] ;
    if (match(TokenKind::KwImport)) {
      auto [ls, cnt] = parse_module_path();
      SymId alias = 0;
      if (match(TokenKind::KwAs))
        alias = expect_ident("expected alias name");
      expect(TokenKind::Semicolon, "expected ';'");
      Span endsp = {s.start, t.end[i - 1]};
      mod.imports.push_back({ls, cnt, alias, endsp});
      return;
    }

    // [pub] type <name> = struct { <field>: <type>, ... }
    if (match(TokenKind::KwType)) {
      SymId name = expect_ident("expected type name");
      expect(TokenKind::Equal, "expected '='");
      expect(TokenKind::KwStruct, "expected 'struct'");
      expect(TokenKind::LBrace, "expected '{'");
      u32 fields_start = static_cast<u32>(mod.fields.size());
      while (!at(TokenKind::RBrace) && !at(TokenKind::Eof) && !err) {
        auto fs = sp();
        SymId fname = expect_ident("expected field name");
        expect(TokenKind::Colon, "expected ':'");
        TypeId ftype = parse_type();
        Span fspan = {fs.start, t.end[i - 1]};
        mod.fields.push_back({fname, ftype, fspan});
        if (!match(TokenKind::Comma)) break;
      }
      expect(TokenKind::RBrace, "expected '}'");
      u32 fields_count = static_cast<u32>(mod.fields.size()) - fields_start;
      Span endsp = {s.start, t.end[i - 1]};
      mod.structs.push_back({name, fields_start, fields_count, is_pub, endsp});
      return;
    }

    // [pub] fn <name> (...) <ret> <block>
    if (at(TokenKind::KwFn)) {
      mod.funcs.push_back(parse_function_decl(false, is_pub));
      return;
    }

    // [pub] impl <TypeName> { [pub] fn ... }
    if (match(TokenKind::KwImpl)) {
      SymId type_name = expect_ident("expected type name");
      expect(TokenKind::LBrace, "expected '{'");
      u32 methods_start = static_cast<u32>(mod.funcs.size());
      while (!at(TokenKind::RBrace) && !at(TokenKind::Eof) && !err) {
        bool method_pub = match(TokenKind::KwPub);
        if (!at(TokenKind::KwFn)) {
          set_error(sp(), "expected 'fn' in impl block");
          break;
        }
        mod.funcs.push_back(parse_function_decl(true, method_pub));
      }
      expect(TokenKind::RBrace, "expected '}'");
      u32 methods_count = static_cast<u32>(mod.funcs.size()) - methods_start;
      Span endsp = {s.start, t.end[i - 1]};
      mod.impls.push_back({type_name, methods_start, methods_count, is_pub, endsp});
      return;
    }

    set_error(sp(), "expected module item (import/type/fn/impl)");
    ++i; // recover
  }
};
