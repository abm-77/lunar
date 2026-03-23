#pragma once

#include <optional>
#include <vector>

#include <common/error.h>
#include <common/interner.h>
#include <common/types.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/module.h>

class Parser {
public:
  Parser(const TokenStream &ts, const IntrinsicTable &intrinsics)
      : t(ts), intrinsics(intrinsics) {}
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
  u32 i = 0;
  std::optional<Error> err;

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
    if (!err) err = Error{s, msg};
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

  // peek-scan for generic args '<' ... '>' followed by '{' or '::'.
  // returns true (does NOT advance i) if the pattern looks like generic args.
  bool peek_generic_args_for(u32 from) const {
    u32 j = from;
    if (j >= t.kind.size() || t.kind[j] != TokenKind::Less) return false;
    ++j;
    int depth = 1;
    while (j < t.kind.size() && depth > 0) {
      TokenKind kj = t.kind[j];
      if (kj == TokenKind::Less)  { ++depth; ++j; continue; }
      if (kj == TokenKind::Greater) { --depth; ++j; continue; }
      if (kj == TokenKind::Eof || kj == TokenKind::Semicolon ||
          kj == TokenKind::LBrace || kj == TokenKind::LParen) break;
      ++j;
    }
    if (depth != 0) return false;
    if (j >= t.kind.size()) return false;
    TokenKind after = t.kind[j];
    return after == TokenKind::LBrace || after == TokenKind::ColonColon;
  }

  // true if current position looks like `for (var/const name := expr)` with no semicolons.
  bool looks_like_for_range() const {
    if (k(0) != TokenKind::KwVar && k(0) != TokenKind::KwConst) return false;
    if (k(1) != TokenKind::Ident) return false;
    if (k(2) != TokenKind::ColonEqual) return false;
    u32 depth = 1;
    for (u32 j = i + 3; j < static_cast<u32>(t.kind.size()); ++j) {
      TokenKind tk = t.kind[j];
      if (tk == TokenKind::LParen)    ++depth;
      else if (tk == TokenKind::RParen) { if (--depth == 0) return true; }
      else if (tk == TokenKind::Semicolon && depth == 1) return false;
      else if (tk == TokenKind::Eof)    return false;
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
      if (!at(TokenKind::RBracket)) {
        if (at(TokenKind::Int)) {
          count = t.payload[i];
          ++i;
        } else if (at(TokenKind::Ident)) {
          // const generic parameter (e.g., [N]T) — value unknown until
          // instantiation
          ++i;
        } else {
          set_error(sp(), "expected integer count or identifier in array type");
          return type_ast.make(TypeKind::Named, start, 0);
        }
      }
      expect(TokenKind::RBracket, "expected ']'");
      TypeId elem = parse_type();
      Span endsp = {start.start, type_ast.span_e[elem]};
      return type_ast.make(TypeKind::Array, endsp, count, elem);
    }

    // integer literal as const-generic argument (e.g., Array<i32, 10>)
    // represent as Named type with SymId=0; TypeLowerer will return Void for it.
    if (match(TokenKind::Int)) {
      return type_ast.make(TypeKind::Named, start, 0);
    }

    // 'type' keyword used as a metatype annotation (e.g. const Foo: type = ...)
    if (match(TokenKind::KwType)) {
      SymId name = static_cast<SymId>(t.payload[i - 1]);
      return type_ast.make(TypeKind::Named, start, name);
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
        do { list_items.push_back(parse_type()); } while (match(TokenKind::Comma));
        expect(TokenKind::Greater, "expected '>' after type arguments");
      }
      u32 targs_count = static_cast<u32>(list_items.size()) - 1;
      auto [ls, _cnt] = type_ast.push_list(list_items.data(), list_items.size());
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
    case TokenKind::AmpAmp:  return BP{35, 36};
    case TokenKind::PipePipe: return BP{30, 31};
    default: return std::nullopt;
    }
  }

  NodeId parse_expr() { return parse_pratt(0); }

  NodeId parse_primary() {
    auto s = sp();

    // @intrinsic(...) — per-intrinsic argument shapes
    if (match(TokenKind::At)) {
      if (!at(TokenKind::Ident)) { set_error(sp(), "expected intrinsic name after '@'"); return 0; }
      SymId name = static_cast<SymId>(t.payload[i]);
      auto kind = intrinsics.lookup(name);
      if (!kind) { set_error(sp(), "unknown intrinsic"); return 0; }
      ++i; // consume name
      switch (*kind) {
      case IntrinsicKind::As:
      case IntrinsicKind::Bitcast: {
        expect(TokenKind::LParen, "expected '(' after intrinsic");
        NodeId val = parse_expr();
        expect(TokenKind::Comma, "expected ','");
        TypeId ty = parse_type();
        expect(TokenKind::RParen, "expected ')'");
        return body_ir.nodes.make(
            *kind == IntrinsicKind::As ? NodeKind::CastAs : NodeKind::Bitcast,
            s, val, ty);
      }
      case IntrinsicKind::SiteId:
        expect(TokenKind::LParen, "expected '('");
        expect(TokenKind::RParen, "expected ')'");
        return body_ir.nodes.make(NodeKind::SiteId, s);
      case IntrinsicKind::SizeOf: {
        expect(TokenKind::LParen, "expected '('");
        TypeId ty = parse_type();
        expect(TokenKind::RParen, "expected ')'");
        return body_ir.nodes.make(NodeKind::SizeOf, s, ty);
      }
      case IntrinsicKind::AlignOf: {
        expect(TokenKind::LParen, "expected '('");
        TypeId ty = parse_type();
        expect(TokenKind::RParen, "expected ')'");
        return body_ir.nodes.make(NodeKind::AlignOf, s, ty);
      }
      case IntrinsicKind::SliceCast: {
        expect(TokenKind::LParen, "expected '('");
        NodeId val = parse_expr();
        expect(TokenKind::Comma, "expected ','");
        TypeId ty = parse_type();
        expect(TokenKind::RParen, "expected ')'");
        return body_ir.nodes.make(NodeKind::SliceCast, s, val, ty);
      }
      case IntrinsicKind::Iter: {
        expect(TokenKind::LParen, "expected '(' after '@iter'");
        NodeId val = parse_expr();
        expect(TokenKind::RParen, "expected ')'");
        return body_ir.nodes.make(NodeKind::IterCreate, s, val);
      }
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
    // disambiguation: after the first field name, '=' → StructExpr, ':' → StructType.
    if (match(TokenKind::KwStruct)) {
      expect(TokenKind::LBrace, "expected '{'");

      // empty struct type, or AnonStructInit if followed by {
      if (match(TokenKind::RBrace)) {
        Span stspan = {s.start, t.end[i - 1]};
        NodeId stype_nid = body_ir.nodes.make(NodeKind::StructType, stspan, 0, 0, 0);
        if (!match(TokenKind::LBrace)) return stype_nid;
        expect(TokenKind::RBrace, "expected '}'");
        Span aspan = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::AnonStructInit, aspan, stype_nid, 0, 0);
      }

      SymId fname = expect_ident("expected field name");

      if (match(TokenKind::Equal)) {
        // StructExpr: field = expr pairs
        NodeId fval = parse_expr();
        std::vector<u32> packed = {fname, fval};
        while (!err && match(TokenKind::Comma) && !at(TokenKind::RBrace)) {
          SymId fn2 = expect_ident("expected field name");
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
        // StructType: field: Type pairs
        expect(TokenKind::Colon, "expected ':' or '='");
        TypeId ftype = parse_type();
        std::vector<u32> packed = {fname, ftype};
        while (!err && match(TokenKind::Comma) && !at(TokenKind::RBrace)) {
          SymId fn2 = expect_ident("expected field name");
          expect(TokenKind::Colon, "expected ':'");
          TypeId ft2 = parse_type();
          packed.push_back(fn2);
          packed.push_back(ft2);
        }
        expect(TokenKind::RBrace, "expected '}'");
        u32 field_count = static_cast<u32>(packed.size()) / 2;
        auto [ls, _sf] = body_ir.nodes.push_list(packed.data(), packed.size());
        Span stspan = {s.start, t.end[i - 1]};
        NodeId stype_nid = body_ir.nodes.make(NodeKind::StructType, stspan, 0, ls,
                                              field_count);
        // struct { ... }{ field = expr } — anonymous struct init
        if (!match(TokenKind::LBrace)) return stype_nid;
        std::vector<u32> ipairs;
        if (!match(TokenKind::RBrace)) {
          do {
            SymId iname = expect_ident("expected field name in struct initializer");
            expect(TokenKind::Equal, "expected '=' in struct initializer");
            NodeId ival = parse_expr();
            ipairs.push_back(iname);
            ipairs.push_back(ival);
          } while (!err && match(TokenKind::Comma) && !at(TokenKind::RBrace));
          expect(TokenKind::RBrace, "expected '}'");
        }
        u32 ifields_count = static_cast<u32>(ipairs.size()) / 2;
        u32 istart = static_cast<u32>(body_ir.nodes.list.size());
        body_ir.nodes.list.insert(body_ir.nodes.list.end(), ipairs.begin(), ipairs.end());
        Span aspan = {s.start, t.end[i - 1]};
        return body_ir.nodes.make(NodeKind::AnonStructInit, aspan, stype_nid, istart,
                                  ifields_count);
      }
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

    if (match(TokenKind::Int)) return body_ir.nodes.make(NodeKind::IntLit, s, t.payload[i - 1]);
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
        generic_type_id = type_ast.make(TypeKind::Named, tspan, sym, targs_start, targs_count);
      }

      // path expression: Ident[<T,...>]:: Ident  e.g. Color::Red, mod::Type<T>::create
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
            do { mid_args.push_back(parse_type()); } while (match(TokenKind::Comma));
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
              generic_type_id = type_ast.make(TypeKind::QualNamed, tspan, seg,
                                              qls, static_cast<u32>(mid_args.size()));
            } else {
              // no module prefix yet (e.g. List<T>::method) — keep Named.
              auto [mls, mcnt] = type_ast.push_list(mid_args.data(), mid_args.size());
              generic_type_id = type_ast.make(TypeKind::Named, tspan, seg, mls, mcnt);
            }
          }
        }
        auto [ls, cnt] = body_ir.nodes.push_list(segs.data(), segs.size());
        Span endsp = {s.start, t.end[i - 1]};
        // a = TypeId of the leading generic type (0 if none), b = segs_start, c = segs_count
        return body_ir.nodes.make(NodeKind::Path, endsp, generic_type_id, ls, cnt);
      }

      // struct literal: TypeName[<T,...>] { field: expr, ... }
      if (match(TokenKind::LBrace)) {
        Span tspan = {s.start, t.end[i - 2]}; // before the '{'
        TypeId struct_type_id = (generic_type_id != 0)
            ? generic_type_id
            : type_ast.make(TypeKind::Named, tspan, sym, 0, 0);

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
        u32 list_start = body_ir.nodes.list.size();
        body_ir.nodes.list.insert(body_ir.nodes.list.end(), packed.begin(),
                                  packed.end());
        u32 pairs = packed.size() / 2;
        Span endsp = {s.start, t.end[i - 1]};
        // a = TypeId (was SymId)
        return body_ir.nodes.make(NodeKind::StructInit, endsp, struct_type_id, list_start,
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
        if (at(TokenKind::Int)) {
          explicit_count = t.payload[i];
          ++i;
        } else if (at(TokenKind::Ident)) {
          // const generic parameter (e.g. [N]T{}) — count unknown until instantiation
          ++i;
        } else {
          set_error(sp(), "expected integer count or identifier or ']' in array literal");
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
        return body_ir.nodes.make(NodeKind::ForRange, endsp, var_name, iter_n, body_n);
      }
      NodeId init = 0;
      if (at(TokenKind::KwVar) || at(TokenKind::KwConst)) {
        // var/const declaration as for-init; parse_stmt also consumes the ';'
        init = parse_stmt();
      } else {
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
            init = body_ir.nodes.make(
                NodeKind::ExprStmt, {ps.start, body_ir.nodes.span_e[lhs]}, lhs);
          }
        }
        expect(TokenKind::Semicolon, "expected ';' after for-init");
      }
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
        bool is_pub_method = match(TokenKind::KwPub);
        DeclKind mkind;
        if (match(TokenKind::KwConst)) mkind = DeclKind::Const;
        else if (match(TokenKind::KwVar)) mkind = DeclKind::Var;
        else {
          set_error(sp(), "expected method declaration in impl block");
          ++i;
          continue;
        }
        SymId mname = expect_ident("expected method name");
        TypeId mty = 0;
        NodeId minit = 0;
        if (match(TokenKind::LParen)) {
          // shorthand: name(params) [-> ret] { body }
          auto [ps, pc] = parse_fn_params();
          TypeId ret = 0;
          if (match(TokenKind::Arrow)) ret = parse_type();
          NodeId body = parse_block();
          u32 idx = static_cast<u32>(body_ir.fn_lits.size());
          body_ir.fn_lits.push_back({ps, pc, ret, body});
          Span mend2 = {ms.start, t.end[i - 1]};
          minit = body_ir.nodes.make(NodeKind::FnLit, mend2, idx);
        } else if (match(TokenKind::ColonEqual)) {
          minit = parse_expr();
        } else {
          if (match(TokenKind::Colon)) mty = parse_type();
          if (match(TokenKind::Equal)) minit = parse_expr();
        }
        match(TokenKind::Semicolon); // optional
        Span mend = {ms.start, t.end[i - 1]};
        impl_block.methods.push_back(
            {mname, mty, minit, 0, 0, is_pub_method, false, mkind, mend});
      }
      expect(TokenKind::RBrace, "expected '}'");
      match(TokenKind::Semicolon); // optional trailing ';'
      impl_block.span = {s.start, t.end[i - 1]};
      mod.impls.push_back(std::move(impl_block));
      return;
    }

    bool is_pub = match(TokenKind::KwPub);
    bool is_extern = match(TokenKind::KwExtern);

    // extern shorthand: extern name(params) -> ret;
    if (is_extern && at(TokenKind::Ident) && k(1) == TokenKind::LParen) {
      SymId ename = expect_ident("expected function name");
      expect(TokenKind::LParen, "expected '('");
      std::vector<TypeId> param_types;
      if (!match(TokenKind::RParen)) {
        do {
          expect_ident("expected param name");
          expect(TokenKind::Colon, "expected ':'");
          param_types.push_back(parse_type());
        } while (match(TokenKind::Comma));
        expect(TokenKind::RParen, "expected ')'");
      }
      expect(TokenKind::Arrow, "expected '->'");
      TypeId ret = parse_type();
      auto [ls, cnt] =
          type_ast.push_list(param_types.data(), param_types.size());
      Span endsp = {s.start, t.end[i - 1]};
      TypeId fn_type = type_ast.make(TypeKind::Fn, endsp, ret, ls, cnt);
      match(TokenKind::Semicolon);
      mod.decls.push_back(
          {ename, fn_type, 0, 0, 0, is_pub, true, DeclKind::Const, endsp});
      return;
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
    // module-level function shorthand: [pub] const/var name[<...>](params) [-> ret] { body }
    if (!is_extern && at(TokenKind::LParen)) {
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
                           is_pub, false, kind, endsp});
      return;
    }
    if (is_extern) {
      // extern declarations require an explicit type annotation and no body.
      expect(TokenKind::Colon, "expected ':' after extern declaration name");
      ty = parse_type();
    } else if (match(TokenKind::ColonEqual)) {
      init = parse_expr();
    } else {
      if (match(TokenKind::Colon)) ty = parse_type();
      if (match(TokenKind::Equal)) init = parse_expr();
    }
    match(TokenKind::Semicolon); // optional at module level
    Span endsp = {s.start, t.end[i - 1]};
    mod.decls.push_back(
        {name, ty, init, generics_start, generics_count, is_pub, is_extern, kind, endsp});
  }
};
