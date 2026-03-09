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

// ── FuncEmitter
// ─────────────────────────────────────────────────────────────── Emits LLVM IR
// for one function body.
//
// Design notes:
//   • All locals are stack-allocated via alloca in the function's entry block
//     so that the mem2reg pass can promote them to SSA registers.
//   • Place (lvalue) expressions return llvm::Value* pointing to the storage
//     slot; value expressions load from that slot.
//   • Short-circuit operators (&& / ||) use branch-based evaluation.
//   • &self / &mut self method parameters are passed as raw pointers; the
//     AllocaInst holding the "self" argument is the alloca for the first param.

struct FuncEmitter {
  CodegenCtx &cg;
  llvm::Function &fn;
  const Symbol &sym;
  const BodyIR &ir;
  const Module &mod;
  const BodySema &bsema; // node_type / node_symbol from sema phase
  llvm::IRBuilder<> builder;

  // Scope stack: each scope maps SymId → alloca slot.
  std::vector<std::unordered_map<SymId, llvm::AllocaInst *>> scopes;

  FuncEmitter(CodegenCtx &cg, llvm::Function &fn, const Symbol &sym,
              const BodyIR &ir, const Module &mod, const BodySema &bsema)
      : cg(cg), fn(fn), sym(sym), ir(ir), mod(mod), bsema(bsema),
        builder(fn.getContext()) {}

  void emit() {
    auto *entry = llvm::BasicBlock::Create(fn.getContext(), "entry", &fn);
    builder.SetInsertPoint(entry);

    // Alloca and store each parameter so mem2reg can promote them.
    TypeLowerer tl = cg.make_type_lowerer(sym.module_idx);
    inject_self_subst(sym, tl, cg);
    push_scope(); // one scope covers both params and the function body
    for (u32 i = 0; i < sym.sig.params_count; ++i) {
      const FuncParam &fp = mod.params[sym.sig.params_start + i];
      llvm::Type *ty = nullptr;
      if (sym.is_mono_instance && i < sym.mono_concrete_params.size()) {
        ty = cg.type_lower.lower(sym.mono_concrete_params[i]);
      } else {
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
      CTypeId ret_ctid =
          sym.is_mono_instance ? sym.mono_concrete_ret : [&]() -> CTypeId {
            auto ret_r = tl.lower(sym.sig.ret_type);
            assert(ret_r && "could not lower return type for function");
            return *ret_r;
          }();
      if (cg.type_lower.types.types[ret_ctid].kind == CTypeKind::Void)
        builder.CreateRetVoid();
      else builder.CreateUnreachable();
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

  // Create an alloca in the entry block (optimal for mem2reg).
  llvm::AllocaInst *create_entry_alloca(llvm::Type *ty, std::string_view name) {
    auto &entry = fn.getEntryBlock();
    llvm::IRBuilder<> tmp(&entry, entry.begin());
    return tmp.CreateAlloca(ty, nullptr, llvm::StringRef{name.data(), name.size()});
  }

  // Returns true if the current insert block already ends with a terminator
  // (ret / br / etc.) so we don't accidentally emit dead instructions.
  bool has_terminator() const {
    auto *bb = builder.GetInsertBlock();
    return bb && bb->getTerminator() != nullptr;
  }

  // ── Statements ────────────────────────────────────────────────────────────
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
    case NodeKind::Block: emit_block(n); break;
    case NodeKind::ExprStmt: emit_expr(ir.nodes.a[n]); break;
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
    if (init_nid != 0) builder.CreateStore(emit_expr(init_nid), slot);
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

    // Compound assignment: load current, apply op, store result.
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

  // ── Expressions (return llvm::Value* with value semantics) ───────────────
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
    // CreateGlobalString creates a global [n x i8] constant and returns a ptr.
    // With LLVM opaque pointers this is already usable as a char pointer.
    return builder.CreateGlobalString(llvm::StringRef{sv.data(), sv.size()});
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

  // Short-circuit &&: if lhs is false, result is false without evaluating rhs.
  llvm::Value *emit_and(NodeId n) {
    auto &ctx = fn.getContext();
    llvm::Value *lhs = emit_expr(ir.nodes.b[n]);
    auto *lhs_bb = builder.GetInsertBlock();
    auto *rhs_bb = llvm::BasicBlock::Create(ctx, "and.rhs", &fn);
    auto *merge_bb = llvm::BasicBlock::Create(ctx, "and.merge", &fn);

    // If lhs is false, short-circuit to merge; otherwise evaluate rhs.
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

  // Short-circuit ||: if lhs is true, result is true without evaluating rhs.
  llvm::Value *emit_or(NodeId n) {
    auto &ctx = fn.getContext();
    llvm::Value *lhs = emit_expr(ir.nodes.b[n]);
    auto *lhs_bb = builder.GetInsertBlock();
    auto *rhs_bb = llvm::BasicBlock::Create(ctx, "or.rhs", &fn);
    auto *merge_bb = llvm::BasicBlock::Create(ctx, "or.merge", &fn);

    // If lhs is true, short-circuit to merge; otherwise evaluate rhs.
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
        // Direct function/method call.
        llvm::Function *callee_fn = it->second;
        ft = callee_fn->getFunctionType();
        callee_val = callee_fn;
      }
    }

    if (!callee_val) {
      // Function pointer call: emit callee as a value and look up its FunctionType.
      callee_val = emit_expr(callee_nid);
      CTypeId callee_ctid = bsema.node_type[callee_nid].concrete;
      auto fti = cg.type_lower.fn_type_cache.find(callee_ctid);
      assert(fti != cg.type_lower.fn_type_cache.end() &&
             "function pointer type not in fn_type_cache");
      ft = fti->second;
    }

    // If callee is a Field node, the call is a method call: `base.method(args)`.
    // The receiver (base) is the implicit self argument and must be prepended.
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

    // Emit explicit arguments. Always use emit_expr: the caller is responsible
    // for wrapping in AddrOf/Deref as needed; we should not implicitly take
    // addresses of arguments.
    for (u32 i = 0; i < args_count; ++i) {
      NodeId arg_nid = ir.nodes.list[args_start + i];
      args.push_back(emit_expr(arg_nid));
    }

    return builder.CreateCall(ft, callee_val, args);
  }

  llvm::Value *emit_field(NodeId n) {
    // a = base NodeId, b = field SymId.
    auto *field_ptr = emit_field_ptr(n);
    CTypeId field_ctid = bsema.node_type[n].concrete;
    llvm::Type *field_ty = cg.type_lower.lower(field_ctid);
    return builder.CreateLoad(field_ty, field_ptr);
  }

  llvm::Value *emit_index(NodeId n) {
    // a = base NodeId, b = index NodeId.
    llvm::Value *elem_ptr = emit_index_ptr(n);
    CTypeId base_ctid = bsema.node_type[ir.nodes.a[n]].concrete;
    // Peel Ref to get the array CTypeId, then get its element type.
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

    auto *slot = create_entry_alloca(sty, "sinit");

    u32 fields_start = ir.nodes.b[n];
    u32 fields_count = ir.nodes.c[n];
    for (u32 i = 0; i < fields_count; ++i) {
      SymId field_name = ir.nodes.list[fields_start + (i * 2)];
      NodeId val_nid = ir.nodes.list[fields_start + (i * 2) + 1];
      u32 idx = cg.type_lower.field_index(struct_sym, field_name);
      auto *field_ptr = builder.CreateStructGEP(sty, slot, idx);
      builder.CreateStore(emit_expr(val_nid), field_ptr);
    }

    return builder.CreateLoad(sty, slot);
  }

  llvm::Value *emit_array_lit(NodeId n) {
    // a = index into ir.array_lits (ArrayLitPayload).
    const ArrayLitPayload &al = ir.array_lits[ir.nodes.a[n]];
    CTypeId ctid = bsema.node_type[n].concrete;
    llvm::Type *aty = cg.type_lower.lower(ctid);
    CTypeId elem_ctid = cg.sema.types.types[ctid].inner;
    llvm::Type *ety = cg.type_lower.lower(elem_ctid);

    auto *slot = create_entry_alloca(aty, "arrinit");

    if (al.values_count == 0) {
      builder.CreateStore(llvm::ConstantAggregateZero::get(aty), slot);
    } else {
      for (u32 i = 0; i < al.values_count; ++i) {
        NodeId val_nid = ir.nodes.list[al.values_start + i];
        auto *ep = builder.CreateGEP(
            ety, slot, {builder.getInt32(0), builder.getInt32(i)});
        builder.CreateStore(emit_expr(val_nid), ep);
      }
    }
    return builder.CreateLoad(aty, slot);
  }

  llvm::Value *emit_tuple_lit(NodeId n) {
    // b = elems_start, c = elems_count.
    CTypeId ctid = bsema.node_type[n].concrete;
    llvm::Type *tty = cg.type_lower.lower(ctid);

    u32 elems_start = ir.nodes.b[n];
    u32 elems_count = ir.nodes.c[n];

    auto *slot = create_entry_alloca(tty, "tupinit");
    for (u32 i = 0; i < elems_count; ++i) {
      NodeId val_nid = ir.nodes.list[elems_start + i];
      auto *fp = builder.CreateStructGEP(tty, slot, i);
      builder.CreateStore(emit_expr(val_nid), fp);
    }
    return builder.CreateLoad(tty, slot);
  }

  llvm::Value *emit_path(NodeId n) {
    // Path nodes: either a sema-resolved function/method, or an enum variant
    // (sema leaves node_symbol == 0 for enum variants).
    u32 seg_start = ir.nodes.b[n];
    u32 seg_count = ir.nodes.c[n];

    SymbolId sym_id = bsema.node_symbol[n];
    if (sym_id != 0) {
      // Sema resolved it — must be a static method or cross-module function.
      auto it = cg.fn_map.find(sym_id);
      assert(it != cg.fn_map.end() && "resolved path symbol not in fn_map");
      return it->second;
    }

    // Enum variant: look up the enum type from the first segment and find the
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

  // ── Place (lvalue) emission ───────────────────────────────────────────────
  // Returns a pointer to the storage location, NOT a loaded value.
  // Used by emit_assign, emit_addr_of, and indirectly by emit_field/emit_index.

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
      // Dereferencing a pointer: the pointer value IS the place address.
      return emit_expr(ir.nodes.a[n]);
    default: assert(false && "not a valid place expression"); return nullptr;
    }
  }

  // ── Helpers ───────────────────────────────────────────────────────────────

  // Shared helper for both emit_field (loads) and emit_place Field (returns ptr).
  // Handles the case where the base is a reference type by emitting as a value
  // (loading the pointer) rather than as a place (which would give ptr-to-ptr).
  llvm::Value *emit_field_ptr(NodeId n) {
    NodeId base_nid = ir.nodes.a[n];
    SymId field_name = ir.nodes.b[n];

    CTypeId base_ctid = bsema.node_type[base_nid].concrete;
    CTypeId struct_ctid;
    llvm::Value *base_ptr;

    if (cg.sema.types.types[base_ctid].kind == CTypeKind::Ref) {
      // Base is a reference: emit the value (loaded pointer) to dereference.
      base_ptr = emit_expr(base_nid);
      struct_ctid = cg.sema.types.types[base_ctid].inner;
    } else {
      base_ptr = emit_place(base_nid);
      struct_ctid = base_ctid;
    }

    SymbolId struct_sym = cg.sema.types.types[struct_ctid].symbol;
    u32 idx = cg.type_lower.field_index(struct_sym, field_name);
    llvm::Type *struct_ty = cg.type_lower.lower(struct_ctid);
    return builder.CreateStructGEP(struct_ty, base_ptr, idx);
  }

  // Shared helper for emit_index (loads) and emit_place Index (returns ptr).
  llvm::Value *emit_index_ptr(NodeId n) {
    NodeId base_nid = ir.nodes.a[n];
    CTypeId base_ctid = bsema.node_type[base_nid].concrete;
    const CType &base_ct = cg.sema.types.types[base_ctid];

    llvm::Value *arr_ptr;
    CTypeId arr_ctid;
    if (base_ct.kind == CTypeKind::Ref) {
      // Reference to array: load the pointer value (don't take address-of).
      arr_ptr = emit_expr(base_nid);
      arr_ctid = base_ct.inner;
    } else {
      arr_ptr = emit_place(base_nid);
      arr_ctid = base_ctid;
    }

    // GEP needs the aggregate type [N x T], not the element type.
    llvm::Type *arr_ty = cg.type_lower.lower(arr_ctid);
    llvm::Value *idx = emit_expr(ir.nodes.b[n]);
    return builder.CreateGEP(arr_ty, arr_ptr, {builder.getInt32(0), idx});
  }

  // True if this CTypeKind is a signed integer type.
  static bool is_signed(CTypeKind k) {
    return k == CTypeKind::I8 || k == CTypeKind::I16 || k == CTypeKind::I32 ||
           k == CTypeKind::I64;
  }

  // True if this CTypeKind is a floating-point type.
  static bool is_float(CTypeKind k) {
    return k == CTypeKind::F32 || k == CTypeKind::F64;
  }
};
