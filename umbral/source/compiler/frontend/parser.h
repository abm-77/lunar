#pragma once

#include <optional>
#include <string>
#include <vector>

#include <common/error.h>
#include <common/interner.h>
#include <common/types.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/module.h>

class Parser {
public:
  Parser(const TokenStream &ts, const IntrinsicTable &intrinsics,
         Interner &interner, std::string_view src = {})
      : t(ts), intrinsics(intrinsics), interner(interner), src(src) {}
  std::optional<Error> error() const { return err; }

  BodyIR body_ir;
  TypeAst type_ast;
  Module mod;

  void parse_module() {
    while (!at(TokenKind::Eof) && !err) parse_module_item();
  }

private:
  const TokenStream &t;
  const IntrinsicTable &intrinsics;
  Interner &interner;
  std::string_view src;
  u32 i = 0;
  std::optional<Error> err;
  // set to the name of the enclosing module-level decl before parsing its init
  // expression, so the struct type parser can key field annotations to it.
  SymId current_struct_name = 0;

  // token helpers
  TokenKind k(u32 look = 0) const { return t.kind[i + look]; }
  Span sp(u32 look = 0) const { return {t.start[i + look], t.end[i + look]}; }
  u64 pl(u32 look = 0) const { return t.payload[i + look]; }
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
    if (!err) err = Error{s, msg};
  }

  // parse struct init field list: { field = expr, field = expr, ... }
  // opening '{' already consumed. consumes the closing '}'.
  // returns {list_start, field_count} for SymId/NodeId pairs in
  // body_ir.nodes.list.
  std::pair<u32, u32> parse_struct_init_fields() {
    std::vector<u32> packed;
    if (!match(TokenKind::RBrace)) {
      do {
        SymId field = expect_ident("expected field name");
        expect(TokenKind::Equal, "expected '=' in struct literal");
        NodeId val = parse_expr();
        packed.push_back(field);
        packed.push_back(val);
      } while (!err && match(TokenKind::Comma) && !at(TokenKind::RBrace));
      expect(TokenKind::RBrace, "expected '}'");
    }
    u32 list_start = static_cast<u32>(body_ir.nodes.list.size());
    body_ir.nodes.list.insert(body_ir.nodes.list.end(), packed.begin(),
                              packed.end());
    u32 field_count = static_cast<u32>(packed.size()) / 2;
    return {list_start, field_count};
  }

  // after consuming '.', reads a tuple index (Int token) or ident for the field
  // name.
  SymId parse_field_or_tuple_index() {
    if (at(TokenKind::Int)) {
      auto idx_sv = src.substr(t.start[i], t.end[i] - t.start[i]);
      SymId field = interner.intern(idx_sv);
      ++i;
      return field;
    }
    return expect_ident("expected field name");
  }

  // parse an expression optionally followed by an assignment operator and rhs.
  // returns AssignStmt or ExprStmt node.
  NodeId parse_assign_or_expr_stmt() {
    auto ps = sp();
    NodeId lhs = parse_expr();
    if (is_assign(k())) {
      TokenKind op = k();
      match(op);
      NodeId rhs = parse_expr();
      Span endsp = {ps.start, body_ir.nodes.span_e[rhs]};
      return body_ir.nodes.make(NodeKind::AssignStmt, endsp, lhs, rhs,
                                static_cast<u32>(op));
    }
    return body_ir.nodes.make(NodeKind::ExprStmt,
                              {ps.start, body_ir.nodes.span_e[lhs]}, lhs);
  }

  // parse a generic parameter list: < Name : type|Kind , ... >
  // called after consuming the opening '<'.
  // returns {start, count} into mod.generic_params.
  std::pair<u32, u32> parse_generic_params() {
    u32 start = static_cast<u32>(mod.generic_params.size());
    do {
      SymId pname = expect_ident("expected generic parameter name");
      expect(TokenKind::Colon, "expected ':' in generic parameter");
      if (match(TokenKind::KwType)) {
        mod.generic_params.push_back({pname, true, 0});
      } else {
        TypeId kind = parse_type();
        mod.generic_params.push_back({pname, false, kind});
      }
    } while (match(TokenKind::Comma));
    expect(TokenKind::Greater, "expected '>' after generic parameters");
    u32 count = static_cast<u32>(mod.generic_params.size()) - start;
    return {start, count};
  }

  // parse FnLit parameter list. opening '(' must already be consumed.
  // handles &self / &mut self receiver (first param only), named params,
  // and optional default values (= expr). consumes the closing ')'.
  // returns {params_start, params_count} into mod.params.
  std::pair<u32, u32> parse_fn_params() {
    u32 params_start = static_cast<u32>(mod.params.size());
    if (!match(TokenKind::RParen)) {
      bool first_param = true;
      do {
        auto ps = sp();
        if (first_param && at(TokenKind::Ampersand)) {
          ++i; // consume '&'
          bool is_self_mut = match(TokenKind::KwMut);
          if (!match(TokenKind::KwSelf_)) {
            set_error(sp(), "expected 'self' after '&'");
            break;
          }
          SymId pname = static_cast<SymId>(t.payload[i - 1]);
          Span name_sp = {t.start[i - 1], t.end[i - 1]};
          TypeId inner = type_ast.make(TypeKind::Named, name_sp, pname);
          Span ref_sp = {ps.start, t.end[i - 1]};
          TypeId ptype = type_ast.make(TypeKind::Ref, ref_sp,
                                       is_self_mut ? 1u : 0u, inner);
          mod.params.push_back({pname, ptype, 0, {ps.start, t.end[i - 1]}});
        } else {
          SymId pname = expect_ident("expected parameter name");
          expect(TokenKind::Colon, "expected ':' after parameter name");
          TypeId ptype = parse_type();
          NodeId pdefault = 0;
          if (match(TokenKind::Equal)) pdefault = parse_expr();
          mod.params.push_back(
              {pname, ptype, pdefault, {ps.start, t.end[i - 1]}});
        }
        first_param = false;
      } while (match(TokenKind::Comma));
      expect(TokenKind::RParen, "expected ')'");
    }
    u32 params_count = static_cast<u32>(mod.params.size()) - params_start;
    return {params_start, params_count};
  }

  // util
  SymId expect_ident(const char *msg) {
    if (!match(TokenKind::Ident)) {
      set_error(sp(), msg);
      return 0;
    }
    return static_cast<SymId>(t.payload[i - 1]);
  }

  // parse optional per-field annotations (@vs_in/@location(n)/etc.) then the
  // field SymId. records any annotations in mod.shader_field_annots /
  // mod.io_field_annots keyed by struct_name. called only from struct-type
  // body parsing.
  SymId parse_struct_field_name(SymId struct_name) {
    ShaderFieldKind sfk = ShaderFieldKind::None;
    IOAnnotKind iok = IOAnnotKind::None;
    u32 loc_idx = 0;
    while (at(TokenKind::At) && k(1) == TokenKind::Ident) {
      SymId iname = static_cast<SymId>(t.payload[i + 1]);
      auto ikind = intrinsics.lookup(iname);
      if (!ikind) break;
      bool consumed = true;
      switch (*ikind) {
      case IntrinsicKind::VsIn:
        i += 2;
        sfk = ShaderFieldKind::VsIn;
        break;
      case IntrinsicKind::VsOut:
        i += 2;
        sfk = ShaderFieldKind::VsOut;
        break;
      case IntrinsicKind::FsIn:
        i += 2;
        sfk = ShaderFieldKind::FsIn;
        break;
      case IntrinsicKind::FsOut:
        i += 2;
        sfk = ShaderFieldKind::FsOut;
        break;
      case IntrinsicKind::DrawData:
        i += 2;
        sfk = ShaderFieldKind::DrawData;
        break;
      case IntrinsicKind::Location: {
        i += 2;
        expect(TokenKind::LParen, "expected '(' after @location");
        if (at(TokenKind::Int)) {
          loc_idx = t.payload[i];
          ++i;
        } else set_error(sp(), "expected integer location index");
        expect(TokenKind::RParen, "expected ')'");
        iok = IOAnnotKind::Location;
        break;
      }
      case IntrinsicKind::ShaderBuiltin: {
        i += 2;
        expect(TokenKind::LParen, "expected '(' after @builtin");
        Span builtin_sp = sp();
        SymId builtin_id = expect_ident("expected 'position'");
        if (builtin_id != 0 && interner.view(builtin_id) != "position")
          set_error(builtin_sp, "@builtin: only 'position' is supported");
        expect(TokenKind::RParen, "expected ')'");
        iok = IOAnnotKind::BuiltinPosition;
        break;
      }
      default: consumed = false; break;
      }
      if (!consumed) break;
    }
    SymId field_name = expect_ident("expected field name");
    if (field_name != 0 && struct_name != 0) {
      if (sfk != ShaderFieldKind::None)
        mod.shader_field_annots.push_back({struct_name, field_name, sfk});
      if (iok != IOAnnotKind::None)
        mod.io_field_annots.push_back({struct_name, field_name, iok, loc_idx});
    }
    return field_name;
  }

  // peek-scan for generic args '<' ... '>' followed by '{' or '::'.
  // returns true (does NOT advance i) if the pattern looks like generic args.
  bool peek_generic_args_for(u32 from) const {
    u32 j = from;
    if (j >= t.kind.size() || t.kind[j] != TokenKind::Less) return false;
    ++j;
    int depth = 1;
    while (j < t.kind.size() && depth > 0) {
      TokenKind kj = t.kind[j];
      if (kj == TokenKind::Less) {
        ++depth;
        ++j;
        continue;
      }
      if (kj == TokenKind::Greater) {
        --depth;
        ++j;
        continue;
      }
      if (kj == TokenKind::Eof || kj == TokenKind::Semicolon ||
          kj == TokenKind::LBrace || kj == TokenKind::LParen)
        break;
      ++j;
    }
    if (depth != 0) return false;
    if (j >= t.kind.size()) return false;
    TokenKind after = t.kind[j];
    return after == TokenKind::LBrace || after == TokenKind::ColonColon ||
           after == TokenKind::LParen;
  }

  // true if current position looks like `for (var/const name := expr)` with no
  // semicolons.
  bool looks_like_for_range() const {
    if (k(0) != TokenKind::KwVar && k(0) != TokenKind::KwConst) return false;
    if (k(1) != TokenKind::Ident) return false;
    if (k(2) != TokenKind::ColonEqual) return false;
    u32 depth = 1;
    for (u32 j = i + 3; j < static_cast<u32>(t.kind.size()); ++j) {
      TokenKind tk = t.kind[j];
      if (tk == TokenKind::LParen) ++depth;
      else if (tk == TokenKind::RParen) {
        if (--depth == 0) return true;
      } else if (tk == TokenKind::Semicolon && depth == 1) return false;
      else if (tk == TokenKind::Eof) return false;
    }
    return false;
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
  // type :=  '&' ['mut'] Type
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
          // allow optional "name:" prefix for readability (name is discarded)
          if (at(TokenKind::Ident) && k(1) == TokenKind::Colon) i += 2;
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
    // array type: '[' [count | ident] ']' elem_type
    if (match(TokenKind::LBracket)) {
      u32 count = static_cast<u32>(-1);
      SymId count_ident = 0;
      if (!at(TokenKind::RBracket)) {
        if (at(TokenKind::Int)) {
          count = t.payload[i];
          ++i;
        } else if (at(TokenKind::Ident)) {
          // const generic parameter (e.g., [N]T) — store SymId in c field
          count_ident = static_cast<SymId>(t.payload[i]);
          ++i;
        } else {
          set_error(sp(), "expected integer count or identifier in array type");
          return type_ast.make(TypeKind::Named, start, 0);
        }
      }
      expect(TokenKind::RBracket, "expected ']'");
      TypeId elem = parse_type();
      Span endsp = {start.start, type_ast.span_e[elem]};
      return type_ast.make(TypeKind::Array, endsp, count, elem, count_ident);
    }

    // integer literal as const-generic argument (e.g., Array<i32, 10>)
    if (match(TokenKind::Int)) {
      return type_ast.make(TypeKind::ConstInt, start, t.payload[i - 1]);
    }

    // 'type' keyword used as a metatype annotation (e.g. const Foo: type = ...)
    if (match(TokenKind::KwType)) {
      SymId name = static_cast<SymId>(t.payload[i - 1]);
      return type_ast.make(TypeKind::Named, start, name);
    }

    // vec<T, N> — vector type
    if (match(TokenKind::KwVec)) {
      expect(TokenKind::Less, "expected '<' after vec");
      TypeId elem = parse_type();
      expect(TokenKind::Comma, "expected ',' in vec<T, N>");
      u32 count = t.payload[i];
      expect(TokenKind::Int, "expected integer dimension in vec<T, N>");
      expect(TokenKind::Greater, "expected '>'");
      Span endsp = {start.start, t.end[i - 1]};
      return type_ast.make(TypeKind::Vec, endsp, count, elem);
    }

    // mat<T, N, M> — matrix type (N cols × M rows)
    if (match(TokenKind::KwMat)) {
      expect(TokenKind::Less, "expected '<' after mat");
      TypeId elem = parse_type();
      expect(TokenKind::Comma, "expected ',' in mat<T, N, M>");
      u32 cols = t.payload[i];
      expect(TokenKind::Int, "expected column count in mat<T, N, M>");
      expect(TokenKind::Comma, "expected ',' in mat<T, N, M>");
      u32 rows = t.payload[i];
      expect(TokenKind::Int, "expected row count in mat<T, N, M>");
      expect(TokenKind::Greater, "expected '>'");
      Span endsp = {start.start, t.end[i - 1]};
      return type_ast.make(TypeKind::Mat, endsp, cols, rows, elem);
    }

    // named type: Ident [:: Ident] [< TypeArg, ... >]
    SymId name = expect_ident("expected type name");
    if (match(TokenKind::ColonColon)) {
      // module::Type [<TypeArg, ...>] qualified reference
      SymId mod_prefix = name;
      name = expect_ident("expected type segment");
      while (match(TokenKind::ColonColon))
        name = expect_ident("expected type segment");
      // list layout: [mod_prefix, targ0, targ1, ...]
      std::vector<TypeId> list_items{static_cast<TypeId>(mod_prefix)};
      if (match(TokenKind::Less)) {
        do {
          list_items.push_back(parse_type());
        } while (match(TokenKind::Comma));
        expect(TokenKind::Greater, "expected '>' after type arguments");
      }
      u32 targs_count = static_cast<u32>(list_items.size()) - 1;
      auto [ls, _cnt] =
          type_ast.push_list(list_items.data(), list_items.size());
      Span endsp = {start.start, t.end[i - 1]};
      return type_ast.make(TypeKind::QualNamed, endsp, name, ls, targs_count);
    }
    // optional generic type arguments: List<i32, 5>
    u32 targs_start = 0, targs_count = 0;
    if (match(TokenKind::Less)) {
      std::vector<TypeId> args;
      do {
        args.push_back(parse_type());
      } while (match(TokenKind::Comma));
      expect(TokenKind::Greater, "expected '>' after type arguments");
      auto [ls, cnt] = type_ast.push_list(args.data(), args.size());
      targs_start = ls;
      targs_count = cnt;
    }
    Span endsp = {start.start, t.end[i - 1]};
    return type_ast.make(TypeKind::Named, endsp, name, targs_start,
                         targs_count);
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
    case TokenKind::SlashEqual:
    case TokenKind::PercentEqual:
    case TokenKind::AmpEqual:
    case TokenKind::PipeEqual:
    case TokenKind::CaretEqual: return true;
    default: return false;
    }
  }

  static std::optional<BP> infix_bp(TokenKind kk) {
    switch (kk) {
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent: return BP{70, 71};
    case TokenKind::Plus:
    case TokenKind::Minus: return BP{60, 61};
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual: return BP{50, 51};
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual: return BP{40, 41};
    case TokenKind::Ampersand: return BP{38, 39}; // bitwise AND
    case TokenKind::Caret: return BP{37, 38};      // bitwise XOR
    case TokenKind::Pipe: return BP{36, 37};        // bitwise OR
    case TokenKind::AmpAmp: return BP{35, 36};
    case TokenKind::PipePipe: return BP{30, 31};
    default: return std::nullopt;
    }
  }

  NodeId parse_expr() { return parse_pratt(0); }

  // intrinsic shape helpers: parse args and produce the node.

  // @intrinsic() — no arguments
  NodeId parse_intrinsic_nullary(Span s, NodeKind nk) {
    expect(TokenKind::LParen, "expected '('");
    expect(TokenKind::RParen, "expected ')'");
    return body_ir.nodes.make(nk, s);
  }

  // @intrinsic(expr) — single expression argument
  NodeId parse_intrinsic_unary_expr(Span s, NodeKind nk) {
    expect(TokenKind::LParen, "expected '('");
    NodeId val = parse_expr();
    expect(TokenKind::RParen, "expected ')'");
    return body_ir.nodes.make(nk, s, val);
  }

  // @intrinsic(type) — single type argument
  NodeId parse_intrinsic_unary_type(Span s, NodeKind nk) {
    expect(TokenKind::LParen, "expected '('");
    TypeId ty = parse_type();
    expect(TokenKind::RParen, "expected ')'");
    return body_ir.nodes.make(nk, s, ty);
  }

  // @intrinsic(ident) — single identifier argument
  NodeId parse_intrinsic_unary_ident(Span s, NodeKind nk, const char *msg) {
    expect(TokenKind::LParen, "expected '('");
    SymId sym = expect_ident(msg);
    expect(TokenKind::RParen, "expected ')'");
    return body_ir.nodes.make(nk, s, sym);
  }

  // @intrinsic(expr, type) — expression then type
  NodeId parse_intrinsic_expr_type(Span s, NodeKind nk) {
    expect(TokenKind::LParen, "expected '('");
    NodeId val = parse_expr();
    expect(TokenKind::Comma, "expected ','");
    TypeId ty = parse_type();
    expect(TokenKind::RParen, "expected ')'");
    return body_ir.nodes.make(nk, s, val, ty);
  }

  // @intrinsic(expr, expr) — two expression arguments
  NodeId parse_intrinsic_binary_expr(Span s, NodeKind nk) {
    expect(TokenKind::LParen, "expected '('");
    NodeId a = parse_expr();
    expect(TokenKind::Comma, "expected ','");
    NodeId b = parse_expr();
    expect(TokenKind::RParen, "expected ')'");
    return body_ir.nodes.make(nk, s, a, b);
  }

  // @intrinsic(expr, expr, expr) — three expression arguments
  NodeId parse_intrinsic_triple_expr(Span s, NodeKind nk) {
    expect(TokenKind::LParen, "expected '('");
    NodeId a = parse_expr();
    expect(TokenKind::Comma, "expected ','");
    NodeId b = parse_expr();
    expect(TokenKind::Comma, "expected ','");
    NodeId c = parse_expr();
    expect(TokenKind::RParen, "expected ')'");
    return body_ir.nodes.make(nk, s, a, b, c);
  }

  NodeId parse_primary() {
    auto s = sp();

    // @intrinsic(...) — dispatch by argument shape
    if (match(TokenKind::At)) {
      if (!at(TokenKind::Ident)) {
        set_error(sp(), "expected intrinsic name after '@'");
        return 0;
      }
      SymId name = static_cast<SymId>(t.payload[i]);
      auto kind = intrinsics.lookup(name);
      if (!kind) {
        set_error(sp(), "unknown intrinsic");
        return 0;
      }
      ++i; // consume name
      switch (*kind) {
      // nullary: ()
      case IntrinsicKind::SiteId:
        return parse_intrinsic_nullary(s, NodeKind::SiteId);
      case IntrinsicKind::DrawId:
        return parse_intrinsic_nullary(s, NodeKind::ShaderDrawId);
      case IntrinsicKind::VertexId:
        return parse_intrinsic_nullary(s, NodeKind::ShaderVertexId);

      // unary expr: (expr)
      case IntrinsicKind::Iter:
        return parse_intrinsic_unary_expr(s, NodeKind::IterCreate);
      case IntrinsicKind::Texture2d:
        return parse_intrinsic_unary_expr(s, NodeKind::ShaderTexture2d);
      case IntrinsicKind::Sampler:
        return parse_intrinsic_unary_expr(s, NodeKind::ShaderSampler);
      case IntrinsicKind::DrawPacket:
        return parse_intrinsic_unary_expr(s, NodeKind::ShaderDrawPacket);

      // unary type: (type)
      case IntrinsicKind::SizeOf:
        return parse_intrinsic_unary_type(s, NodeKind::SizeOf);
      case IntrinsicKind::AlignOf:
        return parse_intrinsic_unary_type(s, NodeKind::AlignOf);

      // unary ident: (ident)
      case IntrinsicKind::MetaFields:
        return parse_intrinsic_unary_ident(s, NodeKind::FieldsOf,
                                           "expected struct type name");
      case IntrinsicKind::ShaderRef:
        return parse_intrinsic_unary_ident(s, NodeKind::ShaderRef,
                                           "expected shader struct type name");

      // expr + type: (expr, type)
      case IntrinsicKind::As:
        return parse_intrinsic_expr_type(s, NodeKind::CastAs);
      case IntrinsicKind::Bitcast:
        return parse_intrinsic_expr_type(s, NodeKind::Bitcast);
      case IntrinsicKind::SliceCast:
        return parse_intrinsic_expr_type(s, NodeKind::SliceCast);

      // expr + ident: (expr, ident)
      case IntrinsicKind::MetaField: {
        expect(TokenKind::LParen, "expected '('");
        NodeId obj = parse_expr();
        expect(TokenKind::Comma, "expected ','");
        SymId field_var = expect_ident("expected field variable name");
        expect(TokenKind::RParen, "expected ')'");
        return body_ir.nodes.make(NodeKind::MetaField, s, obj, field_var);
      }

      // triple expr: (expr, expr, expr)
      case IntrinsicKind::MemCpy:
        return parse_intrinsic_triple_expr(s, NodeKind::MemCpy);
      case IntrinsicKind::MemMov:
        return parse_intrinsic_triple_expr(s, NodeKind::MemMov);
      case IntrinsicKind::MemSet:
        return parse_intrinsic_triple_expr(s, NodeKind::MemSet);
      case IntrinsicKind::MemCmp:
        return parse_intrinsic_triple_expr(s, NodeKind::MemCmp);

      // binary expr: (expr, expr)
      case IntrinsicKind::Shl:
        return parse_intrinsic_binary_expr(s, NodeKind::Shl);
      case IntrinsicKind::Shr:
        return parse_intrinsic_binary_expr(s, NodeKind::Shr);
      case IntrinsicKind::Sample:
        return parse_intrinsic_triple_expr(s, NodeKind::ShaderSample);

      // special: @frame_read<T>(offset)
      case IntrinsicKind::FrameRead: {
        expect(TokenKind::Less, "expected '<' after @frame_read");
        TypeId elem_ty = parse_type();
        expect(TokenKind::Greater, "expected '>'");
        expect(TokenKind::LParen, "expected '('");
        NodeId offset = parse_expr();
        expect(TokenKind::RParen, "expected ')'");
        return body_ir.nodes.make(NodeKind::ShaderFrameRead, s, offset,
                                  elem_ty);
      }

      // annotation-only intrinsics: not valid in expression context
      case IntrinsicKind::Shader:
      case IntrinsicKind::ShaderPod:
      case IntrinsicKind::Stage:
      case IntrinsicKind::VsIn:
      case IntrinsicKind::VsOut:
      case IntrinsicKind::FsIn:
      case IntrinsicKind::FsOut:
      case IntrinsicKind::DrawData:
      case IntrinsicKind::Location:
      case IntrinsicKind::ShaderBuiltin:
        set_error(s, "annotation-only intrinsic in expression context");
        return 0;
      default: set_error(s, "unhandled intrinsic"); return 0;
      }
    }

    // fn ( <params> ) -> <ret> <block>   — FnLit (named params + body)
    // fn ( <types> ) -> <ret>            — FnType (anonymous type expression)
    if (match(TokenKind::KwFn)) {
      expect(TokenKind::LParen, "expected '('");

      // heuristic: ident followed by ':' means named params → FnLit
      // also fires for &self / &mut self receiver (first param only)
      bool is_fn_lit =
          (at(TokenKind::Ident) && k(1) == TokenKind::Colon) ||
          (at(TokenKind::Ampersand) && k(1) == TokenKind::KwSelf_) ||
          (at(TokenKind::Ampersand) && k(1) == TokenKind::KwMut &&
           k(2) == TokenKind::KwSelf_);

      if (is_fn_lit) {
        auto [params_start, params_count] = parse_fn_params();
        expect(TokenKind::Arrow, "expected '->'");
        TypeId ret = parse_type();
        NodeId body = parse_block();
        u32 idx = static_cast<u32>(body_ir.fn_lits.size());
        body_ir.fn_lits.push_back({params_start, params_count, ret, body});
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::FnLit, endsp, idx);
      } else {
        // collect type params (may be empty)
        std::vector<TypeId> param_types;
        if (!match(TokenKind::RParen)) {
          do {
            param_types.push_back(parse_type());
          } while (match(TokenKind::Comma));
          expect(TokenKind::RParen, "expected ')'");
        }
        expect(TokenKind::Arrow, "expected '->'");
        TypeId ret = parse_type();

        // if followed by a block, it's a FnLit with no named params
        if (at(TokenKind::LBrace)) {
          NodeId body = parse_block();
          u32 idx = static_cast<u32>(body_ir.fn_lits.size());
          body_ir.fn_lits.push_back({0, 0, ret, body});
          Span endsp = {s.start, t.end[i - 1]};
          return body_ir.nodes.make(NodeKind::FnLit, endsp, idx);
        }

        // no body: FnType expression node
        auto [ls, cnt] =
            body_ir.nodes.push_list(param_types.data(), param_types.size());
        Span endsp = {s.start, type_ast.span_e[ret]};
        return body_ir.nodes.make(NodeKind::FnType, endsp, ret, ls, cnt);
      }
    }

    // struct { ... }
    //   StructExpr: field = expr,  ...  (pairs [SymId, NodeId])
    //   StructType: field: Type,   ...  (pairs [SymId, TypeId])
    // disambiguation: after the first field name, '=' → StructExpr, ':' →
    // StructType.
    if (match(TokenKind::KwStruct)) {
      expect(TokenKind::LBrace, "expected '{'");

      // empty struct type, or AnonStructInit if followed by {
      if (match(TokenKind::RBrace)) {
        Span stspan = {s.start, t.end[i - 1]};
        NodeId stype_nid =
            body_ir.nodes.make(NodeKind::StructType, stspan, 0, 0, 0);
        if (!match(TokenKind::LBrace)) return stype_nid;
        expect(TokenKind::RBrace, "expected '}'");
        Span aspan = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::AnonStructInit, aspan, stype_nid, 0,
                                  0);
      }

      SymId fname = parse_struct_field_name(current_struct_name);

      if (match(TokenKind::Equal)) {
        // StructExpr: field = expr pairs
        NodeId fval = parse_expr();
        std::vector<u32> packed = {fname, fval};
        while (!err &&
               (match(TokenKind::Comma) || match(TokenKind::Semicolon)) &&
               !at(TokenKind::RBrace)) {
          SymId fn2 = parse_struct_field_name(current_struct_name);
          expect(TokenKind::Equal, "expected '='");
          NodeId fv2 = parse_expr();
          packed.push_back(fn2);
          packed.push_back(fv2);
        }
        expect(TokenKind::RBrace, "expected '}'");
        u32 field_count = static_cast<u32>(packed.size()) / 2;
        auto [ls, _] = body_ir.nodes.push_list(packed.data(), packed.size());
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::StructExpr, endsp, 0, ls,
                                  field_count);
      } else {
        // StructType: field: Type pairs (comma or semicolon separated)
        expect(TokenKind::Colon, "expected ':' or '='");
        TypeId ftype = parse_type();
        std::vector<u32> packed = {fname, ftype};
        while (!err &&
               (match(TokenKind::Comma) || match(TokenKind::Semicolon)) &&
               !at(TokenKind::RBrace)) {
          SymId fn2 = parse_struct_field_name(current_struct_name);
          expect(TokenKind::Colon, "expected ':'");
          TypeId ft2 = parse_type();
          packed.push_back(fn2);
          packed.push_back(ft2);
        }
        expect(TokenKind::RBrace, "expected '}'");
        u32 field_count = static_cast<u32>(packed.size()) / 2;
        auto [ls, _sf] = body_ir.nodes.push_list(packed.data(), packed.size());
        Span stspan = {s.start, t.end[i - 1]};
        NodeId stype_nid = body_ir.nodes.make(NodeKind::StructType, stspan, 0,
                                              ls, field_count);
        // struct { ... }{ field = expr } — anonymous struct init
        if (!match(TokenKind::LBrace)) return stype_nid;
        auto [istart, ifields_count] = parse_struct_init_fields();
        Span aspan = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::AnonStructInit, aspan, stype_nid,
                                  istart, ifields_count);
      }
    }

    // vec<T, N> — vector type expression (used in type aliases and constructors)
    if (match(TokenKind::KwVec)) {
      expect(TokenKind::Less, "expected '<' after vec");
      TypeId elem = parse_type();
      expect(TokenKind::Comma, "expected ',' in vec<T, N>");
      u32 count = t.payload[i];
      expect(TokenKind::Int, "expected integer dimension in vec<T, N>");
      expect(TokenKind::Greater, "expected '>'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::VecType, endsp, count, elem);
    }

    // mat<T, N, M> — matrix type expression
    if (match(TokenKind::KwMat)) {
      expect(TokenKind::Less, "expected '<' after mat");
      TypeId elem = parse_type();
      expect(TokenKind::Comma, "expected ',' in mat<T, N, M>");
      u32 cols = t.payload[i];
      expect(TokenKind::Int, "expected column count in mat<T, N, M>");
      expect(TokenKind::Comma, "expected ',' in mat<T, N, M>");
      u32 rows = t.payload[i];
      expect(TokenKind::Int, "expected row count in mat<T, N, M>");
      expect(TokenKind::Greater, "expected '>'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::MatType, endsp, cols, rows, elem);
    }

    // enum { Variant, ... }
    if (match(TokenKind::KwEnum)) {
      expect(TokenKind::LBrace, "expected '{'");
      std::vector<u32> variants;
      while (!at(TokenKind::RBrace) && !at(TokenKind::Eof) && !err) {
        variants.push_back(expect_ident("expected variant name"));
        if (!match(TokenKind::Comma)) break;
      }
      expect(TokenKind::RBrace, "expected '}'");
      auto [ls, cnt] =
          body_ir.nodes.push_list(variants.data(), variants.size());
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::EnumType, endsp, 0, ls, cnt);
    }

    if (match(TokenKind::Int)) {
      u32 idx = static_cast<u32>(body_ir.int_lits.size());
      body_ir.int_lits.push_back(t.payload[i - 1]);
      return body_ir.nodes.make(NodeKind::IntLit, s, idx);
    }
    if (match(TokenKind::Float)) {
      u32 idx = static_cast<u32>(body_ir.float_lits.size());
      Span fs = {t.start[i - 1], t.end[i - 1]};
      double val = 0.0;
      if (!src.empty()) {
        std::string raw(src.data() + fs.start, fs.end - fs.start);
        try {
          val = std::stod(raw);
        } catch (...) {
          val = 0.0;
        }
      }
      body_ir.float_lits.push_back(val);
      return body_ir.nodes.make(NodeKind::FloatLit, s, idx);
    }
    if (match(TokenKind::String))
      return body_ir.nodes.make(NodeKind::StrLit, s, t.payload[i - 1]);
    if (match(TokenKind::KwTrue_))
      return body_ir.nodes.make(NodeKind::BoolLit, s, 1);
    if (match(TokenKind::KwFalse_))
      return body_ir.nodes.make(NodeKind::BoolLit, s, 0);
    if (match(TokenKind::KwSelf_))
      return body_ir.nodes.make(NodeKind::Ident, s,
                                static_cast<u32>(t.payload[i - 1]));

    if (match(TokenKind::Ident)) {
      SymId sym = static_cast<SymId>(t.payload[i - 1]);

      // try to parse generic type args: Ident<T, ...> followed by '{' or '::'
      TypeId generic_type_id = 0;
      u32 targs_start = 0, targs_count = 0;
      if (peek_generic_args_for(i)) {
        // consume '<' and parse type args
        ++i; // consume '<'
        std::vector<TypeId> args;
        do {
          args.push_back(parse_type());
        } while (match(TokenKind::Comma));
        expect(TokenKind::Greater, "expected '>' after type arguments");
        auto [ls, cnt] = type_ast.push_list(args.data(), args.size());
        targs_start = ls;
        targs_count = static_cast<u32>(cnt);
        Span tspan = {s.start, t.end[i - 1]};
        generic_type_id = type_ast.make(TypeKind::Named, tspan, sym,
                                        targs_start, targs_count);
      }

      // path expression: Ident[<T,...>]:: Ident  e.g. Color::Red,
      // mod::Type<T>::create
      if (at(TokenKind::ColonColon)) {
        std::vector<u32> segs = {sym};
        while (match(TokenKind::ColonColon)) {
          SymId seg = expect_ident("expected path segment");
          segs.push_back(seg);
          // allow type args mid-path (e.g. mod::Type<T>::method).
          // when a module prefix is visible (segs has at least 2 elements,
          // i.e. segs[size-2] is the module), emit QualNamed so the type-arg
          // extraction in body_check can resolve it via import_map.
          if (peek_generic_args_for(i)) {
            ++i; // consume '<'
            std::vector<TypeId> mid_args;
            do {
              mid_args.push_back(parse_type());
            } while (match(TokenKind::Comma));
            expect(TokenKind::Greater, "expected '>' after type arguments");
            Span tspan = {s.start, t.end[i - 1]};
            if (segs.size() >= 2) {
              // segs[size-2] is the module prefix (e.g. "mem"); seg is the
              // type name (e.g. "Alloc"). Encode as QualNamed so lower() can
              // resolve it cross-module.
              // QualNamed layout: a = type_name, b = list_start,
              //   c = targs_count, list = [mod_prefix_symid, targ0, ...]
              SymId mod_prefix = static_cast<SymId>(segs[segs.size() - 2]);
              std::vector<TypeId> qlist;
              qlist.push_back(static_cast<TypeId>(mod_prefix));
              for (TypeId ta : mid_args) qlist.push_back(ta);
              auto [qls, _q] = type_ast.push_list(qlist.data(), qlist.size());
              generic_type_id =
                  type_ast.make(TypeKind::QualNamed, tspan, seg, qls,
                                static_cast<u32>(mid_args.size()));
            } else {
              // no module prefix yet (e.g. List<T>::method) — keep Named.
              auto [mls, mcnt] =
                  type_ast.push_list(mid_args.data(), mid_args.size());
              generic_type_id =
                  type_ast.make(TypeKind::Named, tspan, seg, mls, mcnt);
            }
          }
        }
        // cross-module struct init: mod::Type { field = value, ... }
        if (match(TokenKind::LBrace)) {
          Span tspan = {s.start, t.end[i - 2]};
          TypeId struct_type_id;
          if (generic_type_id != 0) {
            struct_type_id = generic_type_id;
          } else if (segs.size() >= 2) {
            SymId type_name = static_cast<SymId>(segs.back());
            SymId mod_prefix = static_cast<SymId>(segs[segs.size() - 2]);
            std::vector<TypeId> qlist = {static_cast<TypeId>(mod_prefix)};
            auto [qls, _q] = type_ast.push_list(qlist.data(), qlist.size());
            struct_type_id =
                type_ast.make(TypeKind::QualNamed, tspan, type_name, qls, 0);
          } else {
            struct_type_id = type_ast.make(TypeKind::Named, tspan, sym, 0, 0);
          }
          auto [list_start, pairs] = parse_struct_init_fields();
          Span endsp = {s.start, t.end[i - 1]};
          return body_ir.nodes.make(NodeKind::StructInit, endsp, struct_type_id,
                                    list_start, pairs);
        }

        auto [ls, cnt] = body_ir.nodes.push_list(segs.data(), segs.size());
        Span endsp = {s.start, t.end[i - 1]};
        // a = TypeId of the leading generic type (0 if none), b = segs_start, c
        // = segs_count
        return body_ir.nodes.make(NodeKind::Path, endsp, generic_type_id, ls,
                                  cnt);
      }

      // struct literal: TypeName[<T,...>] { field: expr, ... }
      if (match(TokenKind::LBrace)) {
        Span tspan = {s.start, t.end[i - 2]}; // before the '{'
        TypeId struct_type_id =
            (generic_type_id != 0)
                ? generic_type_id
                : type_ast.make(TypeKind::Named, tspan, sym, 0, 0);

        auto [list_start, pairs] = parse_struct_init_fields();
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::StructInit, endsp, struct_type_id,
                                  list_start, pairs);
      }

      // generic function call: name<T>(args) — emit as single-segment Path
      // so the call handler can extract type args from generic_type_id.
      if (generic_type_id != 0) {
        std::vector<u32> segs = {sym};
        auto [ls, cnt] = body_ir.nodes.push_list(segs.data(), segs.size());
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::Path, endsp, generic_type_id, ls,
                                  cnt);
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
        if (at(TokenKind::Int)) {
          explicit_count = t.payload[i];
          ++i;
        } else if (at(TokenKind::Ident)) {
          // const generic parameter (e.g. [N]T{}) — count unknown until
          // instantiation
          ++i;
        } else {
          set_error(
              sp(),
              "expected integer count or identifier or ']' in array literal");
          return body_ir.nodes.make(NodeKind::Ident, s, 0);
        }
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
      Span endsp = {s.start, t.end[i - 1]};
      if (explicit_count == static_cast<u32>(-1)) {
        explicit_count = vc; // []T{ ... } — infer count from elements
      }
      u32 idx = static_cast<u32>(body_ir.array_lits.size());
      body_ir.array_lits.push_back({explicit_count, elem_type, vs, vc});
      return body_ir.nodes.make(NodeKind::ArrayLit, endsp, idx);
    }

    // MetaBlock: { @stmt... } — used for @gen type bodies at module level.
    // detected by '{'  followed by '@' as the first content token.
    if (at(TokenKind::LBrace) && k(1) == TokenKind::At) {
      auto bs = sp();
      ++i; // consume '{'
      std::vector<u32> stmts;
      while (!at(TokenKind::RBrace) && !at(TokenKind::Eof) && !err)
        stmts.push_back(parse_meta_type_stmt());
      expect(TokenKind::RBrace, "expected '}'");
      auto [ls, cnt] = body_ir.nodes.push_list(stmts.data(), stmts.size());
      Span endsp = {bs.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::MetaBlock, endsp, 0, ls, cnt);
    }

    set_error(s, "expected expression");
    return body_ir.nodes.make(NodeKind::Ident, s, 0);
  }

  // parse @if(cond) { expr } [@else { expr }] — in type-body context.
  // then/else branches are single expressions (e.g. StructType), not full
  // blocks.
  NodeId parse_meta_type_if(Span s) {
    expect(TokenKind::LParen, "expected '('");
    NodeId cond = parse_expr();
    expect(TokenKind::RParen, "expected ')'");
    // parse then-branch as a single expression wrapped in { }
    expect(TokenKind::LBrace, "expected '{'");
    NodeId then_n = parse_expr();
    match(TokenKind::Semicolon);
    expect(TokenKind::RBrace, "expected '}'");
    NodeId else_n = 0;
    if (at(TokenKind::At) && k(1) == TokenKind::KwElse) {
      i += 2; // consume '@' 'else'
      expect(TokenKind::LBrace, "expected '{'");
      else_n = parse_expr();
      match(TokenKind::Semicolon);
      expect(TokenKind::RBrace, "expected '}'");
    } else if (at(TokenKind::At) && k(1) == TokenKind::Ident) {
      SymId iname = static_cast<SymId>(t.payload[i + 1]);
      auto ikind = intrinsics.lookup(iname);
      if (ikind && *ikind == IntrinsicKind::MetaElseIf) {
        auto es = sp();
        i += 2; // consume '@' 'else_if'
        else_n = parse_meta_type_if(es);
      }
    }
    Span endsp = {s.start, t.end[i - 1]};
    return body_ir.nodes.make(NodeKind::MetaIf, endsp, cond, then_n, else_n);
  }

  // parse one statement inside a @gen type body (MetaBlock content).
  NodeId parse_meta_type_stmt() {
    auto s = sp();
    if (!at(TokenKind::At))
      return parse_expr(); // bare expression (e.g. struct { ... })
    if (k(1) == TokenKind::KwIf) {
      i += 2; // consume '@' 'if'
      return parse_meta_type_if(s);
    }
    if (k(1) == TokenKind::Ident) {
      SymId iname = static_cast<SymId>(t.payload[i + 1]);
      auto ikind = intrinsics.lookup(iname);
      if (ikind && *ikind == IntrinsicKind::MetaAssert) {
        i += 2; // consume '@' 'assert'
        expect(TokenKind::LParen, "expected '('");
        NodeId cond = parse_expr();
        NodeId msg_n = 0;
        if (match(TokenKind::Comma)) msg_n = parse_expr();
        expect(TokenKind::RParen, "expected ')'");
        match(TokenKind::Semicolon);
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::MetaAssert, endsp, cond, msg_n);
      }
    }
    // unrecognized — try as expression anyway
    return parse_expr();
  }

  // parse @if(cond) { block } [@else_if(cond) { block }]* [@else { block }] —
  // function body. called after '@' and 'if'/'else_if' have been consumed.
  NodeId parse_meta_if_stmt(Span s) {
    expect(TokenKind::LParen, "expected '('");
    NodeId cond = parse_expr();
    expect(TokenKind::RParen, "expected ')'");
    NodeId then_n = parse_block();
    NodeId else_n = 0;
    if (at(TokenKind::At) && k(1) == TokenKind::KwElse) {
      i += 2; // consume '@' 'else'
      else_n = parse_block();
    } else if (at(TokenKind::At) && k(1) == TokenKind::Ident) {
      SymId iname = static_cast<SymId>(t.payload[i + 1]);
      auto ikind = intrinsics.lookup(iname);
      if (ikind && *ikind == IntrinsicKind::MetaElseIf) {
        auto es = sp();
        i += 2; // consume '@' 'else_if'
        else_n = parse_meta_if_stmt(es);
      }
    }
    Span endsp = {s.start, t.end[i - 1]};
    return body_ir.nodes.make(NodeKind::MetaIf, endsp, cond, then_n, else_n);
  }

  NodeId parse_place() {
    // place := primary ( '.' Ident | '[' expr ']' )*
    NodeId base = parse_primary();
    for (;;) {
      if (match(TokenKind::Dot)) {
        SymId field = parse_field_or_tuple_index();
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
          SymId field = parse_field_or_tuple_index();
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

    // const <name> [:= <expr> | [: <type>] [= <expr>]] ;
    if (match(TokenKind::KwConst)) {
      SymId name = expect_ident("expected variable name");
      TypeId ty = 0;
      NodeId init = 0;
      if (match(TokenKind::ColonEqual)) {
        init = parse_expr();
      } else {
        if (match(TokenKind::Colon)) ty = parse_type();
        if (match(TokenKind::Equal)) init = parse_expr();
      }
      expect(TokenKind::Semicolon, "expected ';'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::ConstStmt, endsp, name, ty, init);
    }

    // var <name> [:= <expr> | [: <type>] [= <expr>]] ;
    if (match(TokenKind::KwVar)) {
      SymId name = expect_ident("expected variable name");
      TypeId ty = 0;
      NodeId init = 0;
      if (match(TokenKind::ColonEqual)) {
        init = parse_expr();
      } else {
        if (match(TokenKind::Colon)) ty = parse_type();
        if (match(TokenKind::Equal)) init = parse_expr();
      }
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

    // break ;
    if (match(TokenKind::KwBreak)) {
      expect(TokenKind::Semicolon, "expected ';' after 'break'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::BreakStmt, endsp);
    }

    // continue ;
    if (match(TokenKind::KwContinue)) {
      expect(TokenKind::Semicolon, "expected ';' after 'continue'");
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::ContinueStmt, endsp);
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
      // for (var/const name := iter-expr) — range-for
      if (looks_like_for_range()) {
        ++i; // consume var/const
        SymId var_name = expect_ident("expected variable name");
        expect(TokenKind::ColonEqual, "expected ':='");
        NodeId iter_n = parse_expr();
        expect(TokenKind::RParen, "expected ')'");
        NodeId body_n = parse_block();
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::ForRange, endsp, var_name, iter_n,
                                  body_n);
      }
      NodeId init = 0;
      if (at(TokenKind::KwVar) || at(TokenKind::KwConst)) {
        // var/const declaration as for-init; parse_stmt also consumes the ';'
        init = parse_stmt();
      } else {
        if (!at(TokenKind::Semicolon)) init = parse_assign_or_expr_stmt();
        expect(TokenKind::Semicolon, "expected ';' after for-init");
      }
      NodeId cond = at(TokenKind::Semicolon) ? 0 : parse_expr();
      expect(TokenKind::Semicolon, "expected ';' after for-cond");
      NodeId step = 0;
      if (!at(TokenKind::RParen)) step = parse_assign_or_expr_stmt();
      expect(TokenKind::RParen, "expected ')' after for-step");
      NodeId body = parse_block();
      u32 idx = static_cast<u32>(body_ir.fors.size());
      body_ir.fors.push_back({init, cond, step, body});
      Span endsp = {s.start, t.end[i - 1]};
      return body_ir.nodes.make(NodeKind::ForStmt, endsp, idx);
    }

    if (match(TokenKind::LBrace)) return parse_block();

    // @if(cond) { block } [@else_if(cond) { block }]* [@else { block }]
    if (at(TokenKind::At) && k(1) == TokenKind::KwIf) {
      i += 2; // consume '@' 'if'
      return parse_meta_if_stmt(s);
    }

    // @assert(cond [, msg]) ;
    if (at(TokenKind::At) && k(1) == TokenKind::Ident) {
      SymId iname = static_cast<SymId>(t.payload[i + 1]);
      auto ikind = intrinsics.lookup(iname);
      if (ikind && *ikind == IntrinsicKind::MetaAssert) {
        i += 2; // consume '@' 'assert'
        expect(TokenKind::LParen, "expected '('");
        NodeId cond = parse_expr();
        NodeId msg_n = 0;
        if (match(TokenKind::Comma)) msg_n = parse_expr();
        expect(TokenKind::RParen, "expected ')'");
        match(TokenKind::Semicolon);
        Span endsp = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::MetaAssert, endsp, cond, msg_n);
      }
    }

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

  void parse_module_item() {
    auto s = sp();

    // impl <TypeName>[<T, N, ...>] { [const/var decls]* } [;]
    if (match(TokenKind::KwImpl)) {
      SymId type_name = expect_ident("expected type name after 'impl'");
      // optional generic params: impl List<T, N> — just the param names,
      // referencing the same type params declared on the struct/enum
      std::vector<SymId> impl_generics;
      if (match(TokenKind::Less)) {
        do {
          impl_generics.push_back(expect_ident("expected generic param name"));
        } while (match(TokenKind::Comma));
        expect(TokenKind::Greater, "expected '>' after impl generic params");
      }
      expect(TokenKind::LBrace, "expected '{'");
      ImplDecl impl_block;
      impl_block.type_name = type_name;
      impl_block.generic_params = std::move(impl_generics);
      while (!at(TokenKind::RBrace) && !at(TokenKind::Eof) && !err) {
        auto ms = sp();
        bool is_gen_method = false;
        SymId stage_name_sym = 0;
        bool is_shader_fn = false;
        while (at(TokenKind::At) && k(1) == TokenKind::Ident) {
          SymId iname = static_cast<SymId>(t.payload[i + 1]);
          auto ikind = intrinsics.lookup(iname);
          if (!ikind) break;
          if (*ikind == IntrinsicKind::MetaGen) {
            is_gen_method = true;
            i += 2;
          } else if (*ikind == IntrinsicKind::Stage) {
            i += 2;
            expect(TokenKind::LParen, "expected '(' after @stage");
            stage_name_sym = expect_ident("expected 'vertex' or 'fragment'");
            expect(TokenKind::RParen, "expected ')' after stage name");
          } else if (*ikind == IntrinsicKind::ShaderFn) {
            is_shader_fn = true;
            i += 2;
          } else break;
        }
        DeclKind mkind;
        if (match(TokenKind::KwConst)) mkind = DeclKind::Const;
        else if (match(TokenKind::KwVar)) mkind = DeclKind::Var;
        else {
          set_error(ms, "expected method declaration in impl block");
          ++i;
          continue;
        }
        SymId mname = expect_ident("expected method name");
        TypeId mty = 0;
        NodeId minit = 0;
        if (at(TokenKind::LParen)) {
          expect(TokenKind::LParen, "expected '('");
          auto [ps, pc] = parse_fn_params();
          TypeId ret = 0;
          if (match(TokenKind::Arrow)) ret = parse_type();
          NodeId body = parse_block();
          u32 idx = static_cast<u32>(body_ir.fn_lits.size());
          body_ir.fn_lits.push_back({ps, pc, ret, body});
          Span endsp = {ms.start, t.end[i - 1]};
          minit = body_ir.nodes.make(NodeKind::FnLit, endsp, idx);
          match(TokenKind::Semicolon);
        } else {
          if (match(TokenKind::ColonEqual)) {
            minit = parse_expr();
          } else {
            if (match(TokenKind::Colon)) mty = parse_type();
            if (match(TokenKind::Equal)) minit = parse_expr();
          }
          match(TokenKind::Semicolon);
        }
        Span endsp = {ms.start, t.end[i - 1]};
        impl_block.methods.push_back(
            {mname, mty, minit, 0, 0,
             is_gen_method ? DeclFlags::Gen : DeclFlags::None, mkind, endsp});
        if (stage_name_sym != 0)
          mod.shader_stages.push_back({type_name, mname, stage_name_sym});
        if (is_shader_fn) mod.shader_fns.push_back({type_name, mname});
      }
      expect(TokenKind::RBrace, "expected '}'");
      match(TokenKind::Semicolon); // optional trailing ';'
      impl_block.span = {s.start, t.end[i - 1]};
      mod.impls.push_back(std::move(impl_block));
      return;
    }

    // consume leading annotations: @pub, @gen, @extern, @shader, @shader_pod
    DeclFlags decl_flags = DeclFlags::None;
    while (at(TokenKind::At) && k(1) == TokenKind::Ident) {
      SymId iname = static_cast<SymId>(t.payload[i + 1]);
      auto ikind = intrinsics.lookup(iname);
      if (!ikind) break;
      if (*ikind == IntrinsicKind::Pub) {
        decl_flags = decl_flags | DeclFlags::Pub;
        i += 2;
      } else if (*ikind == IntrinsicKind::MetaGen) {
        decl_flags = decl_flags | DeclFlags::Gen;
        i += 2;
      } else if (*ikind == IntrinsicKind::Shader) {
        decl_flags = decl_flags | DeclFlags::Shader;
        i += 2;
      } else if (*ikind == IntrinsicKind::ShaderPod) {
        decl_flags = decl_flags | DeclFlags::ShaderPod;
        i += 2;
      } else if (*ikind == IntrinsicKind::Extern) {
        i += 2; // consume '@extern'
        if (!match(TokenKind::KwConst) && !match(TokenKind::KwVar))
          set_error(s, "expected 'const' or 'var' after @extern");
        // @extern name : type ;
        SymId ename = expect_ident("expected extern declaration name");
        expect(TokenKind::Colon, "expected ':'");
        TypeId ty = parse_type();
        match(TokenKind::Semicolon);
        Span endsp = {s.start, t.end[i - 1]};
        mod.decls.push_back({ename, ty, 0, 0, 0, decl_flags | DeclFlags::Extern,
                             DeclKind::Const, endsp});
        return;
      } else break;
    }

    // import <path> [=> <ident>] ;
    if (match(TokenKind::KwImport)) {
      auto [ls, cnt] = parse_module_path();
      SymId alias = 0;
      if (match(TokenKind::FatArrow))
        alias = expect_ident("expected alias name after '=>'");
      expect(TokenKind::Semicolon, "expected ';'");
      Span endsp = {s.start, t.end[i - 1]};
      mod.imports.push_back({ls, cnt, alias, endsp});
      return;
    }

    // [pub] const/var <name> [:= <expr> | [: <type>] [= <expr>]] [;]
    DeclKind kind;
    if (match(TokenKind::KwConst)) kind = DeclKind::Const;
    else if (match(TokenKind::KwVar)) kind = DeclKind::Var;
    else {
      set_error(sp(), "expected module item (import/const/var)");
      ++i; // recover
      return;
    }

    SymId name = expect_ident("expected name");
    u32 generics_start = 0, generics_count = 0;
    if (match(TokenKind::Less)) {
      auto [gs, gc] = parse_generic_params();
      generics_start = gs;
      generics_count = gc;
    }
    TypeId ty = 0;
    NodeId init = 0;
    // module-level function shorthand: const/var name[<...>](params) [-> ret] {
    // body }
    if (at(TokenKind::LParen)) {
      expect(TokenKind::LParen, "expected '('");
      auto [ps, pc] = parse_fn_params();
      TypeId ret = 0;
      if (match(TokenKind::Arrow)) ret = parse_type();
      NodeId body = parse_block();
      u32 idx = static_cast<u32>(body_ir.fn_lits.size());
      body_ir.fn_lits.push_back({ps, pc, ret, body});
      Span endsp = {s.start, t.end[i - 1]};
      init = body_ir.nodes.make(NodeKind::FnLit, endsp, idx);
      match(TokenKind::Semicolon);
      mod.decls.push_back({name, ty, init, generics_start, generics_count,
                           decl_flags, kind, endsp});
      return;
    }
    // set current_struct_name so the struct body parser can key field
    // annotations
    current_struct_name = name;
    if (match(TokenKind::ColonEqual)) {
      init = parse_expr();
    } else {
      if (match(TokenKind::Colon)) ty = parse_type();
      if (match(TokenKind::Equal)) {
        bool is_type_decl = ty != 0 &&
            type_ast.kind[ty] == TypeKind::Named &&
            interner.view(type_ast.a[ty]) == "type";
        if (is_type_decl && !at(TokenKind::KwStruct) && !at(TokenKind::KwEnum))
          ty = parse_type();
        else
          init = parse_expr();
      }
    }
    current_struct_name = 0;
    match(TokenKind::Semicolon); // optional at module level
    Span endsp = {s.start, t.end[i - 1]};
    mod.decls.push_back({name, ty, init, generics_start, generics_count,
                         decl_flags, kind, endsp});
  }
};
