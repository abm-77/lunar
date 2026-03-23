#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include <common/types.h>
#include <compiler/codegen/codegen_ctx.h>
#include <compiler/frontend/ast.h>
#include <compiler/frontend/lexer.h>
#include <compiler/frontend/module.h>
#include <compiler/sema/body_check.h>
#include <compiler/sema/ctypes.h>
#include <compiler/sema/symbol.h>

struct FuncEmitter {
  CodegenCtx &cg;
  llvm::Function &fn;
  const Symbol &sym;
  const BodyIR &ir;
  const Module &mod;
  const BodySema &bsema; // node_type / node_symbol from sema phase
  llvm::IRBuilder<> builder;

  // scope stack: each entry maps SymId → alloca slot.
  std::vector<std::unordered_map<SymId, llvm::AllocaInst *>> scopes;

  // compile-time @for_fields state: loop var SymId → (fname, field_idx, field_ctid).
  // set during ForRange FieldIter unroll; used by emit_field and emit_meta_field.
  std::unordered_map<SymId, std::tuple<SymId, u32, CTypeId>> active_field;

  FuncEmitter(CodegenCtx &cg, llvm::Function &fn, const Symbol &sym,
              const BodyIR &ir, const Module &mod, const BodySema &bsema)
      : cg(cg), fn(fn), sym(sym), ir(ir), mod(mod), bsema(bsema),
        builder(fn.getContext()) {}

  void emit() {
    auto *entry = llvm::BasicBlock::Create(fn.getContext(), "entry", &fn);
    builder.SetInsertPoint(entry);

    // alloca and store each param so mem2reg can promote them.
    TypeLowerer tl = cg.make_type_lowerer(sym.module_idx);
    inject_self_subst(sym, tl, cg);
    push_scope(); // one scope covers both params and the function body
    for (u32 i = 0; i < sym.sig.params_count; ++i) {
      const FuncParam &fp = mod.params[sym.sig.params_start + i];
      llvm::Type *ty = nullptr;
      if (sym.is_mono_instance) {
        // mono_concrete_params excludes self; mono_self_ctype holds it separately.
        if (i == 0 && sym.mono_self_ctype != 0) {
          ty = cg.type_lower.lower(sym.mono_self_ctype);
        } else {
          u32 pi = i - (sym.mono_self_ctype != 0 ? 1u : 0u);
          if (pi < sym.mono_concrete_params.size())
            ty = cg.type_lower.lower(sym.mono_concrete_params[pi]);
        }
      }
      if (!ty) {
        auto pt_r = tl.lower(fp.type);
        assert(pt_r && "could not lower param type");
        ty = cg.type_lower.lower(*pt_r);
      }
      auto name = cg.interner.view(fp.name);
      auto *slot = create_entry_alloca(ty, name);
      define_local(fp.name, slot);
      builder.CreateStore(fn.getArg(i), slot);
    }

    emit_block(sym.body);
    pop_scope();

    if (!has_terminator()) {
      llvm::Type *llvm_ret = fn.getReturnType();
      if (llvm_ret->isVoidTy()) {
        builder.CreateRetVoid();
      } else if (llvm_ret->isIntegerTy(32) &&
                 cg.interner.view(sym.name) == "main") {
        // main() was promoted from void → i32; return 0 as exit code.
        builder.CreateRet(llvm::ConstantInt::get(llvm_ret, 0));
      } else {
        builder.CreateUnreachable();
      }
    }
  }

  void push_scope() { scopes.emplace_back(); }
  void pop_scope() {
    if (!scopes.empty()) scopes.pop_back();
  }

  void define_local(SymId name, llvm::AllocaInst *slot) {
    scopes.back()[name] = slot;
  }

  llvm::AllocaInst *lookup_local(SymId name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return f->second;
    }
    return nullptr;
  }

  // create an alloca in the entry block (optimal for mem2reg).
  llvm::AllocaInst *create_entry_alloca(llvm::Type *ty, std::string_view name) {
    auto &entry = fn.getEntryBlock();
    llvm::IRBuilder<> tmp(&entry, entry.begin());
    return tmp.CreateAlloca(ty, nullptr, llvm::StringRef{name.data(), name.size()});
  }

  // true if the current insert block ends with a terminator.
  bool has_terminator() const {
    auto *bb = builder.GetInsertBlock();
    return bb && bb->getTerminator() != nullptr;
  }

  void emit_block(NodeId n) {
    assert(ir.nodes.kind[n] == NodeKind::Block &&
           "calling emit_block on non-block node");
    u32 ls = ir.nodes.b[n], cnt = ir.nodes.c[n];
    push_scope();
    for (u32 i = 0; i < cnt; ++i) emit_stmt(ir.nodes.list[ls + i]);
    pop_scope();
  }

  void emit_stmt(NodeId n) {
    switch (ir.nodes.kind[n]) {
    case NodeKind::ConstStmt:
    case NodeKind::VarStmt: emit_const(n); break;
    case NodeKind::AssignStmt: emit_assign(n); break;
    case NodeKind::ReturnStmt: emit_return(n); break;
    case NodeKind::IfStmt: emit_if(n); break;
    case NodeKind::ForStmt: emit_for(n); break;
    case NodeKind::ForRange: emit_for_range(n); break;
    case NodeKind::Block: emit_block(n); break;
    case NodeKind::ExprStmt: emit_expr(ir.nodes.a[n]); break;
    case NodeKind::MetaAssert: break; // validated at sema; no runtime code
    case NodeKind::MetaIf: emit_meta_if(n); break;
    default: assert(false && "unhandled NodeKind");
    }
  }

  void emit_const(NodeId n) {
    // a = SymId (variable name), b = TypeId (0 if inferred), c = init NodeId
    auto var_name = ir.nodes.a[n];
    auto type_id = ir.nodes.b[n];
    auto init_nid = ir.nodes.c[n];

    CTypeId ctid = 0;
    if (type_id != 0) {
      auto tl = cg.make_type_lowerer(sym.module_idx);
      auto ctype_r = tl.lower(type_id);
      assert(ctype_r && "could not lower variable type");
      ctid = *ctype_r;
    } else {
      ctid = bsema.node_type[n].concrete;
    }
    llvm::Type *ty = cg.type_lower.lower(ctid);

    auto *slot = create_entry_alloca(ty, cg.interner.view(var_name));
    if (init_nid != 0)
      builder.CreateStore(emit_expr(init_nid), slot);
    define_local(var_name, slot);
  }

  void emit_assign(NodeId n) {
    // a = lhs NodeId, b = rhs NodeId, c = op TokenKind
    llvm::Value *place = emit_place(ir.nodes.a[n]);
    llvm::Value *rhs = emit_expr(ir.nodes.b[n]);
    auto op = static_cast<TokenKind>(ir.nodes.c[n]);

    if (op == TokenKind::Equal) {
      builder.CreateStore(rhs, place);
      return;
    }

    // compound assignment: load, apply op, store.
    CTypeId lhs_ctid = bsema.node_type[ir.nodes.a[n]].concrete;
    CTypeKind lk = cg.sema.types.types[lhs_ctid].kind;
    bool fp = is_float(lk);
    bool sign = is_signed(lk);
    llvm::Type *lhs_ty = cg.type_lower.lower(lhs_ctid);
    llvm::Value *cur = builder.CreateLoad(lhs_ty, place);

    llvm::Value *result = nullptr;
    switch (op) {
    case TokenKind::PlusEqual:
      result = fp ? builder.CreateFAdd(cur, rhs) : builder.CreateAdd(cur, rhs);
      break;
    case TokenKind::MinusEqual:
      result = fp ? builder.CreateFSub(cur, rhs) : builder.CreateSub(cur, rhs);
      break;
    case TokenKind::StarEqual:
      result = fp ? builder.CreateFMul(cur, rhs) : builder.CreateMul(cur, rhs);
      break;
    case TokenKind::SlashEqual:
      result = fp     ? builder.CreateFDiv(cur, rhs)
               : sign ? builder.CreateSDiv(cur, rhs)
                      : builder.CreateUDiv(cur, rhs);
      break;
    default: assert(false && "unhandled compound assignment operator"); break;
    }
    if (result) builder.CreateStore(result, place);
  }

  void emit_return(NodeId n) {
    if (ir.nodes.a[n] == 0) builder.CreateRetVoid();
    else builder.CreateRet(emit_expr(ir.nodes.a[n]));
  }

  void emit_if(NodeId n) {
    // a = cond NodeId, b = then-block NodeId, c = else-block NodeId (0 if none)
    auto &ctx = fn.getContext();
    NodeId else_nid = ir.nodes.c[n];

    llvm::Value *cond_v = emit_expr(ir.nodes.a[n]);
    auto *then_bb = llvm::BasicBlock::Create(ctx, "if.then", &fn);
    auto *merge_bb = llvm::BasicBlock::Create(ctx, "if.merge", &fn);
    auto *else_bb = else_nid != 0
                        ? llvm::BasicBlock::Create(ctx, "if.else", &fn)
                        : merge_bb;

    builder.CreateCondBr(cond_v, then_bb, else_bb);

    builder.SetInsertPoint(then_bb);
    emit_block(ir.nodes.b[n]);
    if (!has_terminator()) builder.CreateBr(merge_bb);

    if (else_nid != 0) {
      builder.SetInsertPoint(else_bb);
      // else branch may be a Block or a nested IfStmt (else if)
      emit_stmt(else_nid);
      if (!has_terminator()) builder.CreateBr(merge_bb);
    }

    builder.SetInsertPoint(merge_bb);
  }

  void emit_meta_if(NodeId n) {
    // a = cond NodeId, b = then NodeId, c = else NodeId (0 or next MetaIf)
    NodeId cond_n  = ir.nodes.a[n];
    NodeId then_n  = ir.nodes.b[n];
    NodeId else_n  = ir.nodes.c[n];
    TypeLowerer tl = make_body_tl();
    auto result = tl.eval_const_bool(cond_n, ir);
    if (!result || *result) {
      if (then_n != 0) {
        if (ir.nodes.kind[then_n] == NodeKind::Block) emit_block(then_n);
        else emit_stmt(then_n);
      }
    }
    if (!result || !*result) {
      if (else_n != 0) {
        if (ir.nodes.kind[else_n] == NodeKind::Block) emit_block(else_n);
        else emit_stmt(else_n);
      }
    }
  }

  void emit_for(NodeId n) {
    // a = index into ir.fors (ForPayload { init, cond, step, body })
    auto &ctx = fn.getContext();
    const ForPayload &fp = ir.fors[ir.nodes.a[n]];

    push_scope(); // init var is scoped to the loop

    if (fp.init != 0) emit_stmt(fp.init);

    auto *header_bb = llvm::BasicBlock::Create(ctx, "for.header", &fn);
    auto *body_bb = llvm::BasicBlock::Create(ctx, "for.body", &fn);
    auto *step_bb = llvm::BasicBlock::Create(ctx, "for.step", &fn);
    auto *exit_bb = llvm::BasicBlock::Create(ctx, "for.exit", &fn);

    builder.CreateBr(header_bb);

    builder.SetInsertPoint(header_bb);
    if (fp.cond != 0) {
      builder.CreateCondBr(emit_expr(fp.cond), body_bb, exit_bb);
    } else {
      builder.CreateBr(body_bb); // infinite loop
    }

    builder.SetInsertPoint(body_bb);
    emit_block(fp.body);
    if (!has_terminator()) builder.CreateBr(step_bb);

    builder.SetInsertPoint(step_bb);
    if (fp.step != 0) emit_stmt(fp.step);
    builder.CreateBr(header_bb); // step never terminates

    builder.SetInsertPoint(exit_bb);
    pop_scope();
  }

  llvm::Value *emit_expr(NodeId n) {
    switch (ir.nodes.kind[n]) {
    case NodeKind::IntLit: return emit_int_lit(n);
    case NodeKind::BoolLit: return emit_bool_lit(n);
    case NodeKind::StrLit: return emit_str_lit(n);
    case NodeKind::Ident: return emit_ident(n);
    case NodeKind::Binary: return emit_binary(n);
    case NodeKind::Unary: return emit_unary(n);
    case NodeKind::Call: return emit_call(n);
    case NodeKind::Field: return emit_field(n);
    case NodeKind::Index: return emit_index(n);
    case NodeKind::AddrOf: return emit_addr_of(n);
    case NodeKind::Deref: return emit_deref(n);
    case NodeKind::StructInit: return emit_struct_init(n);
    case NodeKind::ArrayLit: return emit_array_lit(n);
    case NodeKind::TupleLit: return emit_tuple_lit(n);
    case NodeKind::Path: return emit_path(n);
    case NodeKind::CastAs:   return emit_cast_as(n);
    case NodeKind::Bitcast:  return emit_bitcast(n);
    case NodeKind::SliceLit: return emit_slice_lit(n);
    case NodeKind::SiteId:   return emit_site_id(n);
    case NodeKind::SizeOf:   return emit_size_of(n);
    case NodeKind::AlignOf:  return emit_align_of(n);
    case NodeKind::SliceCast: return emit_slice_cast(n);
    case NodeKind::AnonStructInit: return emit_anon_struct_init(n);
    case NodeKind::IterCreate: return emit_iter_create(n);
    case NodeKind::MetaField: return emit_meta_field(n);
    case NodeKind::MemCpy:   return emit_memcpy(n);
    case NodeKind::MemMov:   return emit_memmov(n);
    case NodeKind::MemSet:   return emit_memset(n);
    case NodeKind::MemCmp:   return emit_memcmp(n);
    default: assert(false && "unhandled expression kind"); return nullptr;
    }
  }

  llvm::Value *emit_int_lit(NodeId n) {
    CTypeId ctid = bsema.node_type[n].concrete;
    llvm::Type *ty = cg.type_lower.lower(ctid);
    return llvm::ConstantInt::get(
        ty, ir.nodes.a[n],
        /*IsSigned=*/is_signed(cg.sema.types.types[ctid].kind));
  }

  llvm::Value *emit_bool_lit(NodeId n) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(fn.getContext()),
                                  ir.nodes.a[n]);
  }

  llvm::Value *emit_str_lit(NodeId n) {
    auto sv = cg.interner.view(ir.nodes.a[n]);
    llvm::Value *ptr = builder.CreateGlobalString(llvm::StringRef{sv.data(), sv.size()});
    llvm::Type *slice_ty = cg.type_lower.lower(bsema.node_type[n].concrete);
    llvm::Value *s = llvm::UndefValue::get(slice_ty);
    s = builder.CreateInsertValue(s, ptr, 0);
    s = builder.CreateInsertValue(
          s, llvm::ConstantInt::get(llvm::Type::getInt64Ty(fn.getContext()),
                                    sv.size()), 1);
    return s;
  }

  llvm::Value *emit_ident(NodeId n) {
    // a = SymId of the referenced name.
    SymId name = ir.nodes.a[n];
    if (auto *slot = lookup_local(name))
      return builder.CreateLoad(slot->getAllocatedType(), slot);

    SymbolId sym_id = bsema.node_symbol[n];
    if (auto it = cg.global_map.find(sym_id); it != cg.global_map.end()) {
      auto *gv = it->second;
      return builder.CreateLoad(gv->getValueType(), gv);
    }
    if (auto it = cg.fn_map.find(sym_id); it != cg.fn_map.end())
      return it->second;

    assert(false && "failed to emit code for identifier");
    return nullptr;
  }

  llvm::Value *emit_binary(NodeId n) {
    auto op = static_cast<TokenKind>(ir.nodes.a[n]);

    if (op == TokenKind::AmpAmp) return emit_and(n);
    if (op == TokenKind::PipePipe) return emit_or(n);

    llvm::Value *lv = emit_expr(ir.nodes.b[n]);
    llvm::Value *rv = emit_expr(ir.nodes.c[n]);

    CTypeKind lk =
        cg.sema.types.types[bsema.node_type[ir.nodes.b[n]].concrete].kind;
    bool fp = is_float(lk);
    bool sign = is_signed(lk);

    switch (op) {
    case TokenKind::Plus:
      return fp ? builder.CreateFAdd(lv, rv) : builder.CreateAdd(lv, rv);
    case TokenKind::Minus:
      return fp ? builder.CreateFSub(lv, rv) : builder.CreateSub(lv, rv);
    case TokenKind::Star:
      return fp ? builder.CreateFMul(lv, rv) : builder.CreateMul(lv, rv);
    case TokenKind::Slash:
      return fp     ? builder.CreateFDiv(lv, rv)
             : sign ? builder.CreateSDiv(lv, rv)
                    : builder.CreateUDiv(lv, rv);
    case TokenKind::EqualEqual:
      return fp ? builder.CreateFCmpOEQ(lv, rv) : builder.CreateICmpEQ(lv, rv);
    case TokenKind::BangEqual:
      return fp ? builder.CreateFCmpONE(lv, rv) : builder.CreateICmpNE(lv, rv);
    case TokenKind::Less:
      return fp     ? builder.CreateFCmpOLT(lv, rv)
             : sign ? builder.CreateICmpSLT(lv, rv)
                    : builder.CreateICmpULT(lv, rv);
    case TokenKind::LessEqual:
      return fp     ? builder.CreateFCmpOLE(lv, rv)
             : sign ? builder.CreateICmpSLE(lv, rv)
                    : builder.CreateICmpULE(lv, rv);
    case TokenKind::Greater:
      return fp     ? builder.CreateFCmpOGT(lv, rv)
             : sign ? builder.CreateICmpSGT(lv, rv)
                    : builder.CreateICmpUGT(lv, rv);
    case TokenKind::GreaterEqual:
      return fp     ? builder.CreateFCmpOGE(lv, rv)
             : sign ? builder.CreateICmpSGE(lv, rv)
                    : builder.CreateICmpUGE(lv, rv);
    default: assert(false && "unhandled binary operator"); return nullptr;
    }
  }

  // short-circuit &&: if lhs is false, result is false without evaluating rhs.
  llvm::Value *emit_and(NodeId n) {
    auto &ctx = fn.getContext();
    llvm::Value *lhs = emit_expr(ir.nodes.b[n]);
    auto *lhs_bb = builder.GetInsertBlock();
    auto *rhs_bb = llvm::BasicBlock::Create(ctx, "and.rhs", &fn);
    auto *merge_bb = llvm::BasicBlock::Create(ctx, "and.merge", &fn);

    // if lhs is false, short-circuit to merge; otherwise eval rhs.
    builder.CreateCondBr(lhs, rhs_bb, merge_bb);

    builder.SetInsertPoint(rhs_bb);
    llvm::Value *rhs = emit_expr(ir.nodes.c[n]);
    auto *rhs_end_bb = builder.GetInsertBlock();
    builder.CreateBr(merge_bb);

    builder.SetInsertPoint(merge_bb);
    auto *phi = builder.CreatePHI(llvm::Type::getInt1Ty(ctx), 2);
    phi->addIncoming(llvm::ConstantInt::getFalse(ctx), lhs_bb);
    phi->addIncoming(rhs, rhs_end_bb);
    return phi;
  }

  // short-circuit ||: if lhs is true, result is true without evaluating rhs.
  llvm::Value *emit_or(NodeId n) {
    auto &ctx = fn.getContext();
    llvm::Value *lhs = emit_expr(ir.nodes.b[n]);
    auto *lhs_bb = builder.GetInsertBlock();
    auto *rhs_bb = llvm::BasicBlock::Create(ctx, "or.rhs", &fn);
    auto *merge_bb = llvm::BasicBlock::Create(ctx, "or.merge", &fn);

    // if lhs is true, short-circuit to merge; otherwise eval rhs.
    builder.CreateCondBr(lhs, merge_bb, rhs_bb);

    builder.SetInsertPoint(rhs_bb);
    llvm::Value *rhs = emit_expr(ir.nodes.c[n]);
    auto *rhs_end_bb = builder.GetInsertBlock();
    builder.CreateBr(merge_bb);

    builder.SetInsertPoint(merge_bb);
    auto *phi = builder.CreatePHI(llvm::Type::getInt1Ty(ctx), 2);
    phi->addIncoming(llvm::ConstantInt::getTrue(ctx), lhs_bb);
    phi->addIncoming(rhs, rhs_end_bb);
    return phi;
  }

  llvm::Value *emit_unary(NodeId n) {
    auto op = static_cast<TokenKind>(ir.nodes.a[n]);
    NodeId child = ir.nodes.b[n];

    switch (op) {
    case TokenKind::Minus: {
      llvm::Value *v = emit_expr(child);
      CTypeKind ck = cg.sema.types.types[bsema.node_type[child].concrete].kind;
      return is_float(ck) ? builder.CreateFNeg(v) : builder.CreateNeg(v);
    }
    case TokenKind::Bang: return builder.CreateNot(emit_expr(child));
    case TokenKind::Star: {
      // unary * — dereference
      llvm::Value *ptr = emit_expr(child);
      CTypeId ptr_ctid = bsema.node_type[child].concrete;
      CTypeId inner_ctid = cg.sema.types.types[ptr_ctid].inner;
      return builder.CreateLoad(cg.type_lower.lower(inner_ctid), ptr);
    }
    case TokenKind::Ampersand: return emit_place(child);
    default: assert(false && "unhandled unary operator"); return nullptr;
    }
  }

  llvm::Value *emit_call(NodeId n) {
    // a = callee NodeId, b = args_start (into nodes.list), c = args_count.
    NodeId callee_nid = ir.nodes.a[n];
    u32 args_start = ir.nodes.b[n];
    u32 args_count = ir.nodes.c[n];

    llvm::FunctionType *ft = nullptr;
    llvm::Value *callee_val = nullptr;

    SymbolId callee_sym_id = bsema.node_symbol[callee_nid];
    if (callee_sym_id != 0) {
      if (auto it = cg.fn_map.find(callee_sym_id); it != cg.fn_map.end()) {
        // direct function/method call.
        llvm::Function *callee_fn = it->second;
        ft = callee_fn->getFunctionType();
        callee_val = callee_fn;
      }
    }

    if (!callee_val) {
      // function pointer call: emit callee as a value and look up its FunctionType.
      callee_val = emit_expr(callee_nid);
      CTypeId callee_ctid = bsema.node_type[callee_nid].concrete;
      auto fti = cg.type_lower.fn_type_cache.find(callee_ctid);
      assert(fti != cg.type_lower.fn_type_cache.end() &&
             "function pointer type not in fn_type_cache");
      ft = fti->second;
    }

    // if callee is a Field node, prepend base as implicit self arg.
    std::vector<llvm::Value *> args;
    args.reserve(args_count + 1);
    if (ir.nodes.kind[callee_nid] == NodeKind::Field) {
      NodeId base_nid = ir.nodes.a[callee_nid];
      u32 self_idx = 0;
      if (self_idx < ft->getNumParams() && ft->getParamType(self_idx)->isPointerTy())
        args.push_back(emit_place(base_nid));
      else
        args.push_back(emit_expr(base_nid));
    }

    for (u32 i = 0; i < args_count; ++i)
      args.push_back(emit_expr(ir.nodes.list[args_start + i]));

    // inject default args if fewer were provided than params.
    if (ft != nullptr && args.size() < ft->getNumParams() &&
        callee_sym_id != kInvalidSymbol) {
      const Symbol &callee_sym = cg.sema.syms.symbols[callee_sym_id];
      const Module &callee_mod_data = cg.modules[callee_sym.module_idx].mod;
      const BodyIR &callee_ir = cg.modules[callee_sym.module_idx].ir;
      u32 ps = callee_sym.sig.params_start;
      for (u32 ai = static_cast<u32>(args.size());
           ai < ft->getNumParams(); ++ai) {
        const FuncParam &fp = callee_mod_data.params[ps + ai];
        assert(fp.default_init != 0 && "missing argument with no default");
        args.push_back(emit_default(fp.default_init, callee_ir, callee_sym,
                                    ft->getParamType(ai)));
      }
    }

    return builder.CreateCall(ft, callee_val, args);
  }

  llvm::Value *emit_field(NodeId n) {
    // a = base NodeId, b = field SymId.
    NodeId base_nid = ir.nodes.a[n];

    // check if base is a @for_fields loop var: field.name → string literal.
    if (ir.nodes.kind[base_nid] == NodeKind::Ident) {
      SymId base_sym = ir.nodes.a[base_nid];
      auto ait = active_field.find(base_sym);
      if (ait != active_field.end()) {
        SymId fname = std::get<0>(ait->second);
        auto sv = cg.interner.view(fname);
        llvm::Value *ptr = builder.CreateGlobalString(llvm::StringRef{sv.data(), sv.size()});
        CTypeId slice_ctid = bsema.node_type[n].concrete;
        llvm::Type *slice_ty = cg.type_lower.lower(slice_ctid);
        llvm::Value *s = llvm::UndefValue::get(slice_ty);
        s = builder.CreateInsertValue(s, ptr, 0);
        s = builder.CreateInsertValue(
              s, llvm::ConstantInt::get(llvm::Type::getInt64Ty(fn.getContext()),
                                        sv.size()), 1);
        return s;
      }
    }

    auto *field_ptr = emit_field_ptr(n);
    CTypeId field_ctid = bsema.node_type[n].concrete;
    llvm::Type *field_ty = cg.type_lower.lower(field_ctid);
    return builder.CreateLoad(field_ty, field_ptr);
  }

  llvm::Value *emit_index(NodeId n) {
    // a = base NodeId, b = index NodeId.
    llvm::Value *elem_ptr = emit_index_ptr(n);
    CTypeId base_ctid = bsema.node_type[ir.nodes.a[n]].concrete;
    // peel Ref to get array CTypeId and its element type.
    const CType &base_ct = cg.sema.types.types[base_ctid];
    CTypeId arr_ctid = base_ct.kind == CTypeKind::Ref ? base_ct.inner : base_ctid;
    CTypeId elem_ctid = cg.sema.types.types[arr_ctid].inner;
    llvm::Type *elem_ty = cg.type_lower.lower(elem_ctid);
    return builder.CreateLoad(elem_ty, elem_ptr);
  }

  llvm::Value *emit_addr_of(NodeId n) {
    // a = is_mut (unused in codegen), b = place NodeId.
    return emit_place(ir.nodes.b[n]);
  }

  llvm::Value *emit_deref(NodeId n) {
    // a = ptr expression NodeId (Deref node, distinct from Unary(Star)).
    llvm::Value *ptr = emit_expr(ir.nodes.a[n]);
    CTypeId ptr_ctid = bsema.node_type[ir.nodes.a[n]].concrete;
    CTypeId inner_ctid = cg.sema.types.types[ptr_ctid].inner;
    return builder.CreateLoad(cg.type_lower.lower(inner_ctid), ptr);
  }

  llvm::Value *emit_struct_init(NodeId n) {
    // a = type SymId (string handle), b = fields_start, c = fields_count.
    // nodes.list[b..b+2*c] holds [SymId, NodeId] pairs.
    CTypeId ctid = bsema.node_type[n].concrete;
    llvm::Type *sty = cg.type_lower.lower(ctid);
    SymbolId struct_sym = cg.sema.types.types[ctid].symbol;
    auto *sty_s = llvm::cast<llvm::StructType>(sty);

    auto *slot = create_entry_alloca(sty, "sinit");

    u32 fields_start = ir.nodes.b[n];
    u32 fields_count = ir.nodes.c[n];
    for (u32 i = 0; i < fields_count; ++i) {
      SymId field_name = ir.nodes.list[fields_start + (i * 2)];
      NodeId val_nid = ir.nodes.list[fields_start + (i * 2) + 1];
      u32 idx = cg.type_lower.field_index(struct_sym, field_name, ctid);
      auto *field_ptr = builder.CreateStructGEP(sty, slot, idx);
      llvm::Value *val = coerce_int(emit_expr(val_nid), sty_s->getElementType(idx), val_nid);
      builder.CreateStore(val, field_ptr);
    }

    return builder.CreateLoad(sty, slot);
  }

  llvm::Value *emit_array_lit(NodeId n) {
    // a = index into ir.array_lits (ArrayLitPayload).
    const ArrayLitPayload &al = ir.array_lits[ir.nodes.a[n]];
    CTypeId ctid = bsema.node_type[n].concrete;
    const CType &ct = cg.sema.types.types[ctid];

    // node_type overridden to slice by sema; store array in temp alloca and return { ptr, len }.
    if (ct.kind == CTypeKind::Slice) {
      CTypeId elem_ctid = ct.inner;
      llvm::Type *ety  = cg.type_lower.lower(elem_ctid);
      u64 count        = al.explicit_count;
      llvm::Type *aty  = llvm::ArrayType::get(ety, count);
      llvm::Type *i64  = llvm::Type::getInt64Ty(fn.getContext());
      auto *slot = create_entry_alloca(aty, "a2s.arr");
      fill_array_slot(slot, aty, ety, al);
      llvm::Value *ptr = builder.CreateGEP(aty, slot,
          {builder.getInt32(0), builder.getInt32(0)});
      llvm::Value *s = llvm::UndefValue::get(cg.type_lower.lower(ctid));
      s = builder.CreateInsertValue(s, ptr, 0);
      s = builder.CreateInsertValue(s, llvm::ConstantInt::get(i64, count), 1);
      return s;
    }

    llvm::Type *aty      = cg.type_lower.lower(ctid);
    llvm::Type *ety      = cg.type_lower.lower(ct.inner);
    auto *slot = create_entry_alloca(aty, "arrinit");
    fill_array_slot(slot, aty, ety, al);
    return builder.CreateLoad(aty, slot);
  }

  void fill_array_slot(llvm::Value *slot, llvm::Type *aty, llvm::Type *ety,
                       const ArrayLitPayload &al) {
    if (al.values_count == 0) {
      builder.CreateStore(llvm::ConstantAggregateZero::get(aty), slot);
    } else {
      for (u32 i = 0; i < al.values_count; ++i) {
        NodeId val_nid = ir.nodes.list[al.values_start + i];
        auto *ep = builder.CreateGEP(aty, slot,
            {builder.getInt32(0), builder.getInt32(i)});
        builder.CreateStore(coerce_int(emit_expr(val_nid), ety, val_nid), ep);
      }
    }
  }

  llvm::Value *emit_tuple_lit(NodeId n) {
    // b = elems_start, c = elems_count.
    CTypeId ctid = bsema.node_type[n].concrete;
    llvm::Type *tty = cg.type_lower.lower(ctid);

    u32 elems_start = ir.nodes.b[n];
    u32 elems_count = ir.nodes.c[n];

    auto *slot = create_entry_alloca(tty, "tupinit");
    auto *tty_s = llvm::cast<llvm::StructType>(tty);
    for (u32 i = 0; i < elems_count; ++i) {
      NodeId val_nid = ir.nodes.list[elems_start + i];
      auto *fp = builder.CreateStructGEP(tty, slot, i);
      llvm::Value *val = coerce_int(emit_expr(val_nid), tty_s->getElementType(i), val_nid);
      builder.CreateStore(val, fp);
    }
    return builder.CreateLoad(tty, slot);
  }

  llvm::Value *emit_cast_as(NodeId n) {
    // a = source NodeId, b = target TypeId
    llvm::Value *src = emit_expr(ir.nodes.a[n]);
    CTypeId src_ctid = bsema.node_type[ir.nodes.a[n]].concrete;
    CTypeId dst_ctid = bsema.node_type[n].concrete;
    CTypeKind sk = cg.sema.types.types[src_ctid].kind;
    CTypeKind dk = cg.sema.types.types[dst_ctid].kind;
    llvm::Type *dst_ty = cg.type_lower.lower(dst_ctid);

    if (is_float(sk)) {
      if (is_float(dk)) return builder.CreateFPCast(src, dst_ty);
      if (is_signed(dk)) return builder.CreateFPToSI(src, dst_ty);
      return builder.CreateFPToUI(src, dst_ty);
    }
    if (is_float(dk)) {
      if (is_signed(sk)) return builder.CreateSIToFP(src, dst_ty);
      return builder.CreateUIToFP(src, dst_ty);
    }
    if (sk == CTypeKind::Ref) return builder.CreatePtrToInt(src, dst_ty);
    if (dk == CTypeKind::Ref) return builder.CreateIntToPtr(src, dst_ty);
    // int → int
    return builder.CreateIntCast(src, dst_ty, is_signed(sk));
  }

  llvm::Value *emit_bitcast(NodeId n) {
    // a = source NodeId, b = target TypeId
    llvm::Value *src = emit_expr(ir.nodes.a[n]);
    CTypeId dst_ctid = bsema.node_type[n].concrete;
    llvm::Type *dst_ty = cg.type_lower.lower(dst_ctid);
    return builder.CreateBitCast(src, dst_ty);
  }

  llvm::Value *emit_slice_lit(NodeId n) {
    // a = elem TypeId, b = vals_start, c = vals_count
    u32 cnt = ir.nodes.c[n];
    CTypeId slice_ctid = bsema.node_type[n].concrete;
    CTypeId elem_ctid = cg.sema.types.types[slice_ctid].inner;
    llvm::Type *elem_ty = cg.type_lower.lower(elem_ctid);
    llvm::Type *slice_ty = cg.type_lower.lower(slice_ctid);

    // stack-allocate [cnt x elem_ty]
    llvm::Type *arr_ty = llvm::ArrayType::get(elem_ty, cnt);
    auto *arr_slot = create_entry_alloca(arr_ty, "slice.arr");

    // store each element
    for (u32 k = 0; k < cnt; ++k) {
      NodeId val_nid = ir.nodes.list[ir.nodes.b[n] + k];
      auto *ep = builder.CreateGEP(
          arr_ty, arr_slot, {builder.getInt32(0), builder.getInt32(k)});
      builder.CreateStore(emit_expr(val_nid), ep);
    }

    // get pointer to first element
    llvm::Value *ptr = builder.CreateGEP(
        arr_ty, arr_slot, {builder.getInt32(0), builder.getInt32(0)});

    // build { ptr, i64(cnt) }
    llvm::Value *s = llvm::UndefValue::get(slice_ty);
    s = builder.CreateInsertValue(s, ptr, 0);
    s = builder.CreateInsertValue(
          s, llvm::ConstantInt::get(llvm::Type::getInt64Ty(fn.getContext()), cnt), 1);
    return s;
  }

  llvm::Value *emit_site_id(NodeId n) {
    const LoadedModule &lm = cg.modules[sym.module_idx];
    u32 byte_off = ir.nodes.span_s[n];
    u32 site_idx = cg.alloc_site(lm.src, byte_off, lm.rel_path);
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(fn.getContext()),
                                  site_idx);
  }

  // create a TypeLowerer for the current function body with all substitutions applied (self + mono type params).
  TypeLowerer make_body_tl() const {
    TypeLowerer tl = cg.make_type_lowerer(sym.module_idx);
    inject_self_subst(sym, tl, cg);
    inject_mono_subst(sym, tl);
    return tl;
  }

  llvm::Value *emit_size_of(NodeId n) {
    TypeId tid = ir.nodes.a[n];
    TypeLowerer tl = make_body_tl();
    auto ctid = tl.lower(tid);
    assert(ctid && "size_of: could not lower type");
    llvm::Type *ty = cg.type_lower.lower(*ctid);
    assert(ty && "size_of: lower returned null LLVM type (Void?)");
    auto &dl = cg.module->getDataLayout();
    assert(!dl.isDefault() && "size_of: DataLayout not set on module");
    u64 sz = dl.getTypeAllocSize(ty);
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(fn.getContext()), sz);
  }

  llvm::Value *emit_align_of(NodeId n) {
    TypeId tid = ir.nodes.a[n];
    TypeLowerer tl = make_body_tl();
    auto ctid = tl.lower(tid);
    assert(ctid && "align_of: could not lower type");
    llvm::Type *ty = cg.type_lower.lower(*ctid);
    auto &dl = cg.module->getDataLayout();
    u64 align = dl.getABITypeAlign(ty).value();
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(fn.getContext()),
                                  align);
  }

  llvm::Value *emit_slice_cast(NodeId n) {
    // pure type-level reinterpret: same {ptr, len} values, different element type.
    llvm::Value *src = emit_expr(ir.nodes.a[n]);
    llvm::Value *ptr = builder.CreateExtractValue(src, 0);
    llvm::Value *len = builder.CreateExtractValue(src, 1);
    CTypeId dst_ctid = bsema.node_type[n].concrete;
    llvm::Type *dst_ty = cg.type_lower.lower(dst_ctid);
    llvm::Value *s = llvm::UndefValue::get(dst_ty);
    s = builder.CreateInsertValue(s, ptr, 0);
    s = builder.CreateInsertValue(s, len, 1);
    return s;
  }

  llvm::Value *emit_memcpy(NodeId n) {
    // @memcpy(dest, src, byte_count) — no-overlap copy
    llvm::Value *dest = emit_expr(ir.nodes.a[n]);
    llvm::Value *src  = emit_expr(ir.nodes.b[n]);
    llvm::Value *cnt  = emit_expr(ir.nodes.c[n]);
    builder.CreateMemCpy(dest, llvm::Align(1), src, llvm::Align(1), cnt);
    return llvm::UndefValue::get(llvm::Type::getVoidTy(fn.getContext()));
  }

  llvm::Value *emit_memmov(NodeId n) {
    // @memmov(dest, src, byte_count) — overlap-safe move
    llvm::Value *dest = emit_expr(ir.nodes.a[n]);
    llvm::Value *src  = emit_expr(ir.nodes.b[n]);
    llvm::Value *cnt  = emit_expr(ir.nodes.c[n]);
    builder.CreateMemMove(dest, llvm::Align(1), src, llvm::Align(1), cnt);
    return llvm::UndefValue::get(llvm::Type::getVoidTy(fn.getContext()));
  }

  llvm::Value *emit_memset(NodeId n) {
    // @memset(dest, value_u8, byte_count) — fill bytes
    llvm::Value *dest = emit_expr(ir.nodes.a[n]);
    llvm::Value *val  = emit_expr(ir.nodes.b[n]);
    llvm::Value *cnt  = emit_expr(ir.nodes.c[n]);
    // memset value must be i8; truncate if the user passed a wider int
    llvm::Type *i8 = llvm::Type::getInt8Ty(fn.getContext());
    if (val->getType() != i8)
      val = builder.CreateTrunc(val, i8);
    builder.CreateMemSet(dest, val, cnt, llvm::Align(1));
    return llvm::UndefValue::get(llvm::Type::getVoidTy(fn.getContext()));
  }

  llvm::Value *emit_memcmp(NodeId n) {
    // @memcmp(a, b, byte_count) → i32 (negative / 0 / positive)
    llvm::Value *lhs = emit_expr(ir.nodes.a[n]);
    llvm::Value *rhs = emit_expr(ir.nodes.b[n]);
    llvm::Value *cnt = emit_expr(ir.nodes.c[n]);
    auto &ctx = fn.getContext();
    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *ptr = llvm::PointerType::getUnqual(ctx);
    if (cnt->getType() != i64)
      cnt = builder.CreateZExtOrTrunc(cnt, i64);
    auto *fty = llvm::FunctionType::get(i32, {ptr, ptr, i64}, false);
    auto callee = cg.module->getOrInsertFunction("memcmp", fty);
    return builder.CreateCall(fty, callee.getCallee(), {lhs, rhs, cnt});
  }

  llvm::Value *emit_anon_struct_init(NodeId n) {
    CTypeId ctid = bsema.node_type[n].concrete; // Tuple CTypeId
    llvm::Type *tty = cg.type_lower.lower(ctid);

    // build field name → position map from StructType node.
    NodeId stype_nid = static_cast<NodeId>(ir.nodes.a[n]);
    u32 sf_start = ir.nodes.b[stype_nid], sf_count = ir.nodes.c[stype_nid];
    std::unordered_map<SymId, u32> field_pos;
    for (u32 i = 0; i < sf_count; ++i)
      field_pos[static_cast<SymId>(ir.nodes.list[sf_start + i * 2])] = i;

    auto *slot = create_entry_alloca(tty, "asinit");
    builder.CreateStore(llvm::ConstantAggregateZero::get(tty), slot);

    auto *tty_s = llvm::cast<llvm::StructType>(tty);
    u32 ifs = ir.nodes.b[n], ifc = ir.nodes.c[n];
    for (u32 i = 0; i < ifc; ++i) {
      SymId iname = static_cast<SymId>(ir.nodes.list[ifs + i * 2]);
      NodeId ival  = static_cast<NodeId>(ir.nodes.list[ifs + i * 2 + 1]);
      u32 idx = field_pos.at(iname);
      auto *fp = builder.CreateStructGEP(tty, slot, idx);
      llvm::Value *val = coerce_int(emit_expr(ival), tty_s->getElementType(idx), ival);
      builder.CreateStore(val, fp);
    }
    return builder.CreateLoad(tty, slot);
  }

  llvm::Value *emit_iter_create(NodeId n) {
    NodeId src_n = ir.nodes.a[n];
    CTypeId src_ctid  = bsema.node_type[src_n].concrete;
    CTypeId iter_ctid = bsema.node_type[n].concrete;
    llvm::Type *iter_ty = cg.type_lower.lower(iter_ctid);
    llvm::Type *i64     = llvm::Type::getInt64Ty(fn.getContext());

    llvm::Value *data_ptr, *len_val;
    const CType &sct = cg.sema.types.types[src_ctid];
    if (sct.kind == CTypeKind::Array) {
      llvm::Type *arr_ty = cg.type_lower.lower(src_ctid);
      auto *arr_slot = create_entry_alloca(arr_ty, "iter.arr");
      builder.CreateStore(emit_expr(src_n), arr_slot);
      data_ptr = builder.CreateGEP(arr_ty, arr_slot,
          {llvm::ConstantInt::get(i64, 0), llvm::ConstantInt::get(i64, 0)});
      len_val = llvm::ConstantInt::get(i64, sct.count);
    } else { // Slice
      llvm::Value *sv = emit_expr(src_n);
      data_ptr = builder.CreateExtractValue(sv, 0);
      len_val  = builder.CreateExtractValue(sv, 1);
    }

    llvm::Value *it = llvm::UndefValue::get(iter_ty);
    it = builder.CreateInsertValue(it, data_ptr, 0);
    it = builder.CreateInsertValue(it, len_val,  1);
    it = builder.CreateInsertValue(it, llvm::ConstantInt::get(i64, 0), 2);
    return it;
  }

  llvm::Value *emit_meta_field(NodeId n) {
    // a = obj NodeId, b = field_var SymId
    NodeId obj_nid  = ir.nodes.a[n];
    SymId field_var = ir.nodes.b[n];

    auto ait = active_field.find(field_var);
    assert(ait != active_field.end() && "@field used outside @for_fields loop");
    auto [fname, fidx, fctid] = ait->second;

    CTypeId obj_ctid = bsema.node_type[obj_nid].concrete;
    llvm::Value *base_ptr;
    CTypeId struct_ctid;

    if (cg.sema.types.types[obj_ctid].kind == CTypeKind::Ref) {
      base_ptr = emit_expr(obj_nid);
      struct_ctid = cg.sema.types.types[obj_ctid].inner;
    } else {
      base_ptr = emit_place(obj_nid);
      struct_ctid = obj_ctid;
    }

    llvm::Type *struct_ty = cg.type_lower.lower(struct_ctid);
    auto *field_ptr = builder.CreateStructGEP(struct_ty, base_ptr, fidx);
    llvm::Type *field_ty = cg.type_lower.lower(fctid);
    return builder.CreateLoad(field_ty, field_ptr);
  }

  // compile-time unroll of @for_fields — iterates struct fields and emits body once per field.
  void emit_for_fields_unroll(SymId var_name, NodeId body_n, SymbolId struct_sym_id) {
    const Symbol &struct_sym = cg.sema.syms.symbols[struct_sym_id];
    const BodyIR &struct_ir = cg.modules[struct_sym.module_idx].ir;
    NodeId stype_nid = struct_sym.type_node;
    assert(struct_ir.nodes.kind[stype_nid] == NodeKind::StructType &&
           "@for_fields: expected plain StructType");

    u32 ls = struct_ir.nodes.b[stype_nid];
    u32 field_count = struct_ir.nodes.c[stype_nid];

    TypeLowerer field_tl = cg.make_type_lowerer(struct_sym.module_idx);

    push_scope();
    for (u32 fi = 0; fi < field_count; ++fi) {
      SymId fname = struct_ir.nodes.list[ls + fi * 2];
      TypeId ftid  = struct_ir.nodes.list[ls + fi * 2 + 1];
      auto fctid_r = field_tl.lower(ftid);
      CTypeId fctid = fctid_r ? *fctid_r : 0;
      active_field[var_name] = std::make_tuple(fname, fi, fctid);
      emit_block(body_n);
    }
    active_field.erase(var_name);
    pop_scope();
  }

  void emit_for_range(NodeId n) {
    SymId var_name = static_cast<SymId>(ir.nodes.a[n]);
    NodeId iter_n  = ir.nodes.b[n];
    NodeId body_n  = ir.nodes.c[n];
    auto &ctx = fn.getContext();

    CTypeId iter_ctid = bsema.node_type[iter_n].concrete;
    const CType &ict = cg.sema.types.types[iter_ctid];

    // compile-time @fields unroll — no runtime loop
    if (ict.kind == CTypeKind::FieldIter) {
      emit_for_fields_unroll(var_name, body_n, ict.symbol);
      return;
    }

    CTypeId elem_ctid = ict.inner;
    llvm::Type *iter_ty = cg.type_lower.lower(iter_ctid); // { ptr*, i64, i64 }
    llvm::Type *elem_ty = cg.type_lower.lower(elem_ctid);
    llvm::Type *i64     = llvm::Type::getInt64Ty(ctx);

    push_scope();

    auto *iter_slot = create_entry_alloca(iter_ty, "forin.iter");
    builder.CreateStore(emit_expr(iter_n), iter_slot);

    auto *var_slot = create_entry_alloca(elem_ty, cg.interner.view(var_name));
    define_local(var_name, var_slot);

    auto *header_bb = llvm::BasicBlock::Create(ctx, "forin.header", &fn);
    auto *body_bb   = llvm::BasicBlock::Create(ctx, "forin.body",   &fn);
    auto *step_bb   = llvm::BasicBlock::Create(ctx, "forin.step",   &fn);
    auto *exit_bb   = llvm::BasicBlock::Create(ctx, "forin.exit",   &fn);

    builder.CreateBr(header_bb);

    // header: check idx < len
    builder.SetInsertPoint(header_bb);
    auto *idx_ptr = builder.CreateStructGEP(iter_ty, iter_slot, 2);
    auto *len_ptr = builder.CreateStructGEP(iter_ty, iter_slot, 1);
    llvm::Value *idx_v = builder.CreateLoad(i64, idx_ptr, "forin.idx");
    llvm::Value *len_v = builder.CreateLoad(i64, len_ptr, "forin.len");
    builder.CreateCondBr(builder.CreateICmpULT(idx_v, len_v), body_bb, exit_bb);

    // body: load elem[idx] → var; execute body
    builder.SetInsertPoint(body_bb);
    auto *ptr_ptr = builder.CreateStructGEP(iter_ty, iter_slot, 0);
    llvm::Value *data_ptr = builder.CreateLoad(
        llvm::PointerType::getUnqual(ctx), ptr_ptr, "forin.ptr");
    llvm::Value *elem_ptr = builder.CreateGEP(elem_ty, data_ptr, idx_v);
    builder.CreateStore(builder.CreateLoad(elem_ty, elem_ptr), var_slot);
    emit_block(body_n);
    if (!has_terminator()) builder.CreateBr(step_bb);

    // step: idx += 1; back to header
    builder.SetInsertPoint(step_bb);
    llvm::Value *cur_idx = builder.CreateLoad(i64, idx_ptr);
    builder.CreateStore(builder.CreateAdd(cur_idx, llvm::ConstantInt::get(i64, 1)), idx_ptr);
    builder.CreateBr(header_bb);

    builder.SetInsertPoint(exit_bb);
    pop_scope();
  }


  // emit a default-parameter constant expression from the callee's BodyIR.
  // expected_ty: the LLVM type of the corresponding parameter (for IntLit).
  llvm::Value *emit_default(NodeId n, const BodyIR &src_ir,
                            const Symbol &csym,
                            llvm::Type *expected_ty = nullptr) {
    switch (src_ir.nodes.kind[n]) {
    case NodeKind::SiteId: {
      const LoadedModule &lm = cg.modules[csym.module_idx];
      u32 site_idx =
          cg.alloc_site(lm.src, src_ir.nodes.span_s[n], lm.rel_path);
      return llvm::ConstantInt::get(llvm::Type::getInt32Ty(fn.getContext()),
                                    site_idx);
    }
    case NodeKind::SizeOf: {
      TypeId tid = src_ir.nodes.a[n];
      TypeLowerer tl = cg.make_type_lowerer(csym.module_idx);
      inject_self_subst(csym, tl, cg);
      inject_mono_subst(csym, tl);
      auto ctid = tl.lower(tid);
      assert(ctid && "size_of default: could not lower type");
      llvm::Type *ty = cg.type_lower.lower(*ctid);
      u64 sz = cg.module->getDataLayout().getTypeAllocSize(ty);
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(fn.getContext()),
                                    sz);
    }
    case NodeKind::AlignOf: {
      TypeId tid = src_ir.nodes.a[n];
      TypeLowerer tl = cg.make_type_lowerer(csym.module_idx);
      inject_self_subst(csym, tl, cg);
      inject_mono_subst(csym, tl);
      auto ctid = tl.lower(tid);
      assert(ctid && "align_of default: could not lower type");
      llvm::Type *ty = cg.type_lower.lower(*ctid);
      u64 align = cg.module->getDataLayout().getABITypeAlign(ty).value();
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(fn.getContext()),
                                    align);
    }
    case NodeKind::IntLit: {
      llvm::Type *ty = expected_ty ? expected_ty
                                   : llvm::Type::getInt64Ty(fn.getContext());
      return llvm::ConstantInt::get(ty, src_ir.nodes.a[n]);
    }
    case NodeKind::BoolLit: {
      return llvm::ConstantInt::getBool(fn.getContext(),
                                        src_ir.nodes.a[n] != 0);
    }
    default:
      assert(false && "unsupported default expression kind");
      return nullptr;
    }
  }

  llvm::Value *emit_path(NodeId n) {
    // path nodes: either a sema-resolved function/method, or an enum variant
    // (sema leaves node_symbol == 0 for enum variants).
    u32 seg_start = ir.nodes.b[n];
    u32 seg_count = ir.nodes.c[n];

    SymbolId sym_id = bsema.node_symbol[n];
    if (sym_id != 0) {
      // sema resolved it — static method or cross-module function.
      auto it = cg.fn_map.find(sym_id);
      assert(it != cg.fn_map.end() && "resolved path symbol not in fn_map");
      return it->second;
    }

    // enum variant: look up the enum type from the first segment and find the
    // variant index by name from the last segment.
    assert(seg_count >= 2 && "unresolved Path must have >= 2 segments");
    SymId enum_name = ir.nodes.list[seg_start];
    SymId variant_name = ir.nodes.list[seg_start + seg_count - 1];

    SymbolId enum_sid = cg.sema.syms.lookup(sym.module_idx, enum_name);
    assert(enum_sid != kInvalidSymbol && "enum type not found");
    const Symbol &esym = cg.sema.syms.symbols[enum_sid];
    const BodyIR &enum_ir = cg.modules[esym.module_idx].ir;
    assert(enum_ir.nodes.kind[esym.type_node] == NodeKind::EnumType &&
           "first Path segment is not an enum type");

    u32 vs = enum_ir.nodes.b[esym.type_node];
    u32 vn = enum_ir.nodes.c[esym.type_node];
    for (u32 i = 0; i < vn; ++i) {
      if (enum_ir.nodes.list[vs + i] == variant_name)
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(fn.getContext()),
                                      i);
    }
    assert(false && "enum variant not found in type");
    return nullptr;
  }

  // returns a pointer to the storage location (not a loaded value).

  llvm::Value *emit_place(NodeId n) {
    switch (ir.nodes.kind[n]) {
    case NodeKind::Ident: {
      SymId name = ir.nodes.a[n];
      if (auto *slot = lookup_local(name)) return slot;
      SymbolId sym_id = bsema.node_symbol[n];
      if (auto it = cg.global_map.find(sym_id); it != cg.global_map.end())
        return it->second;
      assert(false && "ident not found as local or global in emit_place");
      return nullptr;
    }
    case NodeKind::Field: return emit_field_ptr(n);
    case NodeKind::Index: return emit_index_ptr(n);
    case NodeKind::Deref:
      // dereferencing: the pointer value is the place address.
      return emit_expr(ir.nodes.a[n]);
    default: assert(false && "not a valid place expression"); return nullptr;
    }
  }

  // shared helper for emit_field and emit_place Field; handles Ref base types.
  llvm::Value *emit_field_ptr(NodeId n) {
    NodeId base_nid = ir.nodes.a[n];
    SymId field_name = ir.nodes.b[n];

    CTypeId base_ctid = bsema.node_type[base_nid].concrete;
    CTypeId struct_ctid;
    llvm::Value *base_ptr;

    if (cg.sema.types.types[base_ctid].kind == CTypeKind::Ref) {
      // base is a reference: emit the loaded pointer.
      base_ptr = emit_expr(base_nid);
      struct_ctid = cg.sema.types.types[base_ctid].inner;
    } else {
      base_ptr = emit_place(base_nid);
      struct_ctid = base_ctid;
    }

    if (cg.sema.types.types[struct_ctid].kind == CTypeKind::Slice) {
      auto fname = cg.interner.view(field_name);
      u32 idx = (fname == "data") ? 0u : 1u;
      llvm::Type *slice_ty = cg.type_lower.lower(struct_ctid);
      return builder.CreateStructGEP(slice_ty, base_ptr, idx);
    }

    if (cg.sema.types.types[struct_ctid].kind == CTypeKind::Tuple &&
        cg.sema.types.types[struct_ctid].symbol != 0) {
      // anonymous struct stored as Tuple; .symbol holds the StructType NodeId.
      NodeId stype_nid = static_cast<NodeId>(cg.sema.types.types[struct_ctid].symbol);
      u32 sf_start = ir.nodes.b[stype_nid], sf_count = ir.nodes.c[stype_nid];
      for (u32 i = 0; i < sf_count; ++i) {
        SymId fname = static_cast<SymId>(ir.nodes.list[sf_start + i * 2]);
        if (fname == field_name) {
          llvm::Type *tuple_ty = cg.type_lower.lower(struct_ctid);
          return builder.CreateStructGEP(tuple_ty, base_ptr, i);
        }
      }
      assert(false && "anonymous struct field not found");
      return nullptr;
    }

    if (cg.sema.types.types[struct_ctid].kind == CTypeKind::Iter) {
      auto fname = cg.interner.view(field_name);
      llvm::Type *iter_ty = cg.type_lower.lower(struct_ctid);
      u32 idx = (fname == "len" || fname == "count") ? 1u
              : (fname == "idx")                      ? 2u : 0u;
      return builder.CreateStructGEP(iter_ty, base_ptr, idx);
    }

    SymbolId struct_sym = cg.sema.types.types[struct_ctid].symbol;
    u32 idx = cg.type_lower.field_index(struct_sym, field_name, struct_ctid);
    llvm::Type *struct_ty = cg.type_lower.lower(struct_ctid);
    return builder.CreateStructGEP(struct_ty, base_ptr, idx);
  }

  // shared helper for emit_index and emit_place Index.
  llvm::Value *emit_index_ptr(NodeId n) {
    NodeId base_nid = ir.nodes.a[n];
    CTypeId base_ctid = bsema.node_type[base_nid].concrete;
    const CType &base_ct = cg.sema.types.types[base_ctid];

    // peel Ref to get array or slice type.
    CTypeId actual_ctid =
        base_ct.kind == CTypeKind::Ref ? base_ct.inner : base_ctid;
    const CType &actual_ct = cg.sema.types.types[actual_ctid];

    llvm::Value *idx = emit_expr(ir.nodes.b[n]);
    llvm::Type *elem_ty = cg.type_lower.lower(actual_ct.inner);

    if (actual_ct.kind == CTypeKind::Slice) {
      // dynamic slice: extract data ptr then GEP(elem, idx).
      llvm::Value *slice_ptr = emit_place(base_nid);
      if (base_ct.kind == CTypeKind::Ref)
        slice_ptr = emit_expr(base_nid); // already a pointer to the slice
      llvm::Type *slice_ty = cg.type_lower.lower(actual_ctid);
      llvm::Value *data_ptr_ptr =
          builder.CreateStructGEP(slice_ty, slice_ptr, 0);
      llvm::Value *data_ptr = builder.CreateLoad(
          llvm::PointerType::getUnqual(fn.getContext()), data_ptr_ptr);
      return builder.CreateGEP(elem_ty, data_ptr, idx);
    }

    // fixed-size array: GEP with {0, idx}.
    llvm::Value *arr_ptr;
    CTypeId arr_ctid;
    if (base_ct.kind == CTypeKind::Ref) {
      arr_ptr = emit_expr(base_nid);
      arr_ctid = base_ct.inner;
    } else {
      arr_ptr = emit_place(base_nid);
      arr_ctid = base_ctid;
    }
    llvm::Type *arr_ty = cg.type_lower.lower(arr_ctid);
    return builder.CreateGEP(arr_ty, arr_ptr, {builder.getInt32(0), idx});
  }

  // true if k is a signed integer type.
  static bool is_signed(CTypeKind k) {
    return k == CTypeKind::I8 || k == CTypeKind::I16 || k == CTypeKind::I32 ||
           k == CTypeKind::I64;
  }

  // widen or truncate val to expected if both are integer types; no-op otherwise.
  llvm::Value *coerce_int(llvm::Value *val, llvm::Type *expected, NodeId src_n) {
    if (val->getType() == expected) return val;
    if (!val->getType()->isIntegerTy() || !expected->isIntegerTy()) return val;
    bool sign = is_signed(cg.sema.types.types[bsema.node_type[src_n].concrete].kind);
    return builder.CreateIntCast(val, expected, sign);
  }

  // true if k is a floating-point type.
  static bool is_float(CTypeKind k) {
    return k == CTypeKind::F32 || k == CTypeKind::F64;
  }
};
