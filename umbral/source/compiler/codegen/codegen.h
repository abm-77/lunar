#pragma once

#include <expected>
#include <memory>
#include <string>
#include <unordered_map>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
// LLVM ≥17: llvm/TargetParser/Host.h; LLVM ≤16: llvm/Support/Host.h
#if __has_include(<llvm/TargetParser/Host.h>)
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

#include <common/error.h>
#include <common/interner.h>
#include <compiler/codegen/codegen_ctx.h>
#include <compiler/driver/loader.h>
#include <compiler/sema/body_check.h>
#include <compiler/sema/sema.h>

#include "func_emit.h"
#include "type_lower.h"

struct CodegenResult {
  std::string ir; // serialised LLVM IR text (.ll) — only populated with dump_ir

  // Ownership of the LLVM context and module, so the caller can use them
  // for object file emission after run_codegen() returns. LLVMContext must
  // outlive Module, so both are kept here together.
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
};

// name mangling scheme:
//   main                                      → "main"
//   free function `add` in game/ecs/world     →
//   "_U_game__ecs__world__add__<id>" impl method  `area` on `Quad` in foo →
//   "_U_foo__Quad__area__<id>"
//
// The SymbolId suffix makes monomorphized instances unconditionally distinct —
// two instantiations of `identity<T>` get different ids even though they share
// the same base name and module.
//
// Module path separators (`/`) become `__` in the output.
// impl_owner is the SymId of the type name (e.g. SymId("Quad")).
inline std::string mangle(SymbolId id, const CodegenCtx &cg) {
  const Symbol &sym = cg.sema.syms.symbols[id];
  std::string_view name = cg.interner.view(sym.name);

  if (name == "main") return "main";

  std::string out = "_U_";

  // module path: "game/ecs/world" → "game__ecs__world"
  for (char c : cg.modules[sym.module_idx].rel_path)
    if (c == '/') out += "__";
    else out += c;

  out += "__";

  // impl method: prepend type name
  if (sym.impl_owner != 0) {
    out += cg.interner.view(sym.impl_owner);
    out += "__";
  }

  out += name;
  out += "__";
  out += std::to_string(id);

  return out;
}

// recursively evaluate a constant expression to an llvm::Constant.
// handles literals, binary arithmetic, references to other const globals.
// returns null for anything that can't be const-folded.
static llvm::Constant *eval_const_expr(CodegenCtx &cg, const BodyIR &ir,
                                       u32 mod_idx, NodeId n, llvm::Type *ty) {
  if (n >= ir.nodes.kind.size()) return nullptr;
  switch (ir.nodes.kind[n]) {
  case NodeKind::IntLit:
    return llvm::ConstantInt::get(
        ty, static_cast<int64_t>(ir.int_lits[ir.nodes.a[n]]));
  case NodeKind::FloatLit:
    return llvm::ConstantFP::get(ty, ir.float_lits[ir.nodes.a[n]]);
  case NodeKind::BoolLit:
    return llvm::ConstantInt::getBool(ty->getContext(), ir.nodes.a[n] != 0);

  case NodeKind::Unary: {
    auto op = static_cast<TokenKind>(ir.nodes.a[n]);
    auto *child = eval_const_expr(cg, ir, mod_idx, ir.nodes.b[n], ty);
    if (!child) return nullptr;
    if (op == TokenKind::Minus) {
      if (ty->isIntegerTy())
        return llvm::ConstantExpr::getNeg(child);
      if (ty->isFloatingPointTy())
        return llvm::ConstantFP::get(
            ty->getContext(),
            llvm::cast<llvm::ConstantFP>(child)->getValueAPF().operator-());
    }
    if (op == TokenKind::Bang)
      return llvm::ConstantExpr::getNot(child);
    return nullptr;
  }

  case NodeKind::Ident: {
    // a = SymId (interned name); resolve to SymbolId via module namespace
    auto name = static_cast<SymId>(ir.nodes.a[n]);
    auto &ns = cg.sema.syms.module_namespaces[mod_idx];
    auto ns_it = ns.find(name);
    if (ns_it == ns.end()) return nullptr;
    auto it = cg.global_map.find(ns_it->second);
    if (it == cg.global_map.end()) return nullptr;
    return it->second->getInitializer();
  }

  case NodeKind::Binary: {
    auto op = static_cast<TokenKind>(ir.nodes.a[n]);
    auto *lhs = eval_const_expr(cg, ir, mod_idx, ir.nodes.b[n], ty);
    auto *rhs = eval_const_expr(cg, ir, mod_idx, ir.nodes.c[n], ty);
    if (!lhs || !rhs) return nullptr;

    if (ty->isIntegerTy()) {
      auto *li = llvm::cast<llvm::ConstantInt>(lhs);
      auto *ri = llvm::cast<llvm::ConstantInt>(rhs);
      switch (op) {
      case TokenKind::Plus:
        return llvm::ConstantInt::get(ty, li->getValue() + ri->getValue());
      case TokenKind::Minus:
        return llvm::ConstantInt::get(ty, li->getValue() - ri->getValue());
      case TokenKind::Star:
        return llvm::ConstantInt::get(ty, li->getValue() * ri->getValue());
      case TokenKind::Slash: {
        if (ri->isZero()) return nullptr;
        return llvm::ConstantInt::get(ty, li->getValue().udiv(ri->getValue()));
      }
      case TokenKind::Percent: {
        if (ri->isZero()) return nullptr;
        return llvm::ConstantInt::get(ty, li->getValue().urem(ri->getValue()));
      }
      case TokenKind::Ampersand:
        return llvm::ConstantInt::get(ty, li->getValue() & ri->getValue());
      case TokenKind::Pipe:
        return llvm::ConstantInt::get(ty, li->getValue() | ri->getValue());
      case TokenKind::Caret:
        return llvm::ConstantInt::get(ty, li->getValue() ^ ri->getValue());
      case TokenKind::KwShl:
        return llvm::ConstantInt::get(ty, li->getValue().shl(ri->getValue()));
      case TokenKind::KwShr:
        return llvm::ConstantInt::get(ty, li->getValue().lshr(ri->getValue()));
      default: return nullptr;
      }
    }

    if (ty->isFloatingPointTy()) {
      auto *lf = llvm::cast<llvm::ConstantFP>(lhs);
      auto *rf = llvm::cast<llvm::ConstantFP>(rhs);
      const auto &la = lf->getValueAPF(), &ra = rf->getValueAPF();
      llvm::APFloat result(la);
      switch (op) {
      case TokenKind::Plus:
        result.add(ra, llvm::APFloat::rmNearestTiesToEven);
        break;
      case TokenKind::Minus:
        result.subtract(ra, llvm::APFloat::rmNearestTiesToEven);
        break;
      case TokenKind::Star:
        result.multiply(ra, llvm::APFloat::rmNearestTiesToEven);
        break;
      case TokenKind::Slash:
        result.divide(ra, llvm::APFloat::rmNearestTiesToEven);
        break;
      default: return nullptr;
      }
      return llvm::ConstantFP::get(ty->getContext(), result);
    }

    return nullptr;
  }

  case NodeKind::Shl:
  case NodeKind::Shr: {
    auto *lhs = eval_const_expr(cg, ir, mod_idx, ir.nodes.a[n], ty);
    auto *rhs = eval_const_expr(cg, ir, mod_idx, ir.nodes.b[n], ty);
    if (!lhs || !rhs || !ty->isIntegerTy()) return nullptr;
    auto *li = llvm::cast<llvm::ConstantInt>(lhs);
    auto *ri = llvm::cast<llvm::ConstantInt>(rhs);
    if (ir.nodes.kind[n] == NodeKind::Shl)
      return llvm::ConstantInt::get(ty, li->getValue().shl(ri->getValue()));
    return llvm::ConstantInt::get(ty, li->getValue().lshr(ri->getValue()));
  }

  case NodeKind::Call: {
    // positional type constructor with constant args:
    // vec4(1.0, ...), mat2(col0, col1), MyStruct(a, b, c)
    NodeId callee_nid = ir.nodes.a[n];
    u32 args_start = ir.nodes.b[n], args_count = ir.nodes.c[n];

    // resolve callee to a Type symbol (local or cross-module).
    SymbolId callee_sym = 0;
    if (callee_nid < ir.nodes.kind.size()) {
      if (ir.nodes.kind[callee_nid] == NodeKind::Ident) {
        SymId name = ir.nodes.a[callee_nid];
        auto &ns = cg.sema.syms.module_namespaces[mod_idx];
        auto it = ns.find(name);
        if (it != ns.end()) callee_sym = it->second;
      } else if (ir.nodes.kind[callee_nid] == NodeKind::Path) {
        u32 ls = ir.nodes.b[callee_nid], cnt = ir.nodes.c[callee_nid];
        if (cnt >= 2) {
          SymId mod_seg = static_cast<SymId>(ir.nodes.list[ls]);
          SymId type_seg = static_cast<SymId>(ir.nodes.list[ls + cnt - 1]);
          auto &imp = cg.modules[mod_idx].import_map;
          auto mit = imp.find(mod_seg);
          if (mit != imp.end())
            callee_sym = cg.sema.syms.lookup_pub(mit->second, type_seg);
        }
      }
    }
    // inline vec<T,N>(...) or mat<T,N,M>(...) — callee is a type node, not a symbol.
    if (callee_sym == 0 && callee_nid < ir.nodes.kind.size()) {
      NodeKind cnk = ir.nodes.kind[callee_nid];
      if (cnk == NodeKind::VecType && llvm::isa<llvm::FixedVectorType>(ty)) {
        auto *vty = llvm::cast<llvm::FixedVectorType>(ty);
        std::vector<llvm::Constant *> elems;
        for (u32 k = 0; k < args_count; ++k) {
          auto *c = eval_const_expr(cg, ir, mod_idx,
                                    ir.nodes.list[args_start + k],
                                    vty->getElementType());
          if (!c) return nullptr;
          elems.push_back(c);
        }
        return llvm::ConstantVector::get(elems);
      }
      if (cnk == NodeKind::MatType && llvm::isa<llvm::ArrayType>(ty)) {
        auto *aty = llvm::cast<llvm::ArrayType>(ty);
        std::vector<llvm::Constant *> cols;
        for (u32 k = 0; k < args_count; ++k) {
          auto *c = eval_const_expr(cg, ir, mod_idx,
                                    ir.nodes.list[args_start + k],
                                    aty->getElementType());
          if (!c) return nullptr;
          cols.push_back(c);
        }
        return llvm::ConstantArray::get(aty, cols);
      }
    }

    if (callee_sym == 0) return nullptr;
    const Symbol &csym = cg.sema.syms.get(callee_sym);
    if (csym.kind != SymbolKind::Type) return nullptr;
    SymbolId real_sym = csym.aliased_sym ? csym.aliased_sym : callee_sym;
    const Symbol &rsym = cg.sema.syms.get(real_sym);
    const BodyIR &rsym_ir = cg.modules[rsym.module_idx].ir;
    if (rsym.type_node == 0) return nullptr;
    NodeKind tnk = rsym_ir.nodes.kind[rsym.type_node];

    if (tnk == NodeKind::VecType) {
      auto *vty = llvm::cast<llvm::FixedVectorType>(ty);
      std::vector<llvm::Constant *> elems;
      for (u32 k = 0; k < args_count; ++k) {
        auto *c = eval_const_expr(cg, ir, mod_idx,
                                  ir.nodes.list[args_start + k],
                                  vty->getElementType());
        if (!c) return nullptr;
        elems.push_back(c);
      }
      return llvm::ConstantVector::get(elems);
    }
    if (tnk == NodeKind::MatType) {
      auto *aty = llvm::cast<llvm::ArrayType>(ty);
      std::vector<llvm::Constant *> cols;
      for (u32 k = 0; k < args_count; ++k) {
        auto *c = eval_const_expr(cg, ir, mod_idx,
                                  ir.nodes.list[args_start + k],
                                  aty->getElementType());
        if (!c) return nullptr;
        cols.push_back(c);
      }
      return llvm::ConstantArray::get(aty, cols);
    }
    if (tnk == NodeKind::StructType) {
      auto *sty = llvm::cast<llvm::StructType>(ty);
      if (args_count != sty->getNumElements()) return nullptr;
      std::vector<llvm::Constant *> fields;
      for (u32 k = 0; k < args_count; ++k) {
        auto *c = eval_const_expr(cg, ir, mod_idx,
                                  ir.nodes.list[args_start + k],
                                  sty->getElementType(k));
        if (!c) return nullptr;
        fields.push_back(c);
      }
      return llvm::ConstantStruct::get(sty, fields);
    }
    return nullptr;
  }

  case NodeKind::StructInit: {
    // named struct init: TypeName { field = val, ... }
    u32 fields_start = ir.nodes.b[n], fields_count = ir.nodes.c[n];
    auto *sty = llvm::dyn_cast<llvm::StructType>(ty);
    if (!sty) return nullptr;

    // a = TypeId in the module's TypeAst; extract the type name from it.
    TypeId tid = ir.nodes.a[n];
    const TypeAst &ta = cg.modules[mod_idx].type_ast;
    if (tid >= ta.kind.size()) return nullptr;
    SymId type_name = ta.a[tid];
    SymbolId sid = cg.sema.syms.lookup(mod_idx, type_name);
    if (sid == kInvalidSymbol) return nullptr;
    const Symbol &tsym = cg.sema.syms.get(sid);
    if (tsym.aliased_sym) sid = tsym.aliased_sym;
    const Symbol &rsym = cg.sema.syms.get(sid);

    // start with all zeros, then fill in provided fields.
    std::vector<llvm::Constant *> vals(sty->getNumElements(), nullptr);
    for (u32 k = 0; k < sty->getNumElements(); ++k)
      vals[k] = llvm::Constant::getNullValue(sty->getElementType(k));

    for (u32 k = 0; k < fields_count; ++k) {
      SymId fname = ir.nodes.list[fields_start + k * 2];
      NodeId val_nid = ir.nodes.list[fields_start + k * 2 + 1];
      u32 idx = cg.type_lower.field_index(sid, fname);
      auto *c = eval_const_expr(cg, ir, mod_idx, val_nid,
                                sty->getElementType(idx));
      if (!c) return nullptr;
      vals[idx] = c;
    }
    return llvm::ConstantStruct::get(sty, vals);
  }

  default: return nullptr;
  }
}

static llvm::Constant *try_const_init(CodegenCtx &cg, const Symbol &sym,
                                      llvm::Type *ty) {
  if (!sym.init_expr) return nullptr;
  const BodyIR &ir = cg.modules[sym.module_idx].ir;
  return eval_const_expr(cg, ir, sym.module_idx, sym.init_expr, ty);
}

inline Result<void> declare_globals(CodegenCtx &cg) {
  for (SymbolId i = 1; i < cg.sema.syms.symbols.size(); ++i) {
    const Symbol &sym = cg.sema.syms.symbols[i];
    if (sym.kind != SymbolKind::GlobalVar) continue;

    TypeLowerer tl = cg.make_type_lowerer(sym.module_idx);

    assert(sym.annotate_type != 0 &&
           "global variables must have explicit type annotation");
    auto ctid = tl.lower(sym.annotate_type);
    assert(ctid && "failed to lower global type");

    llvm::Type *ty = cg.type_lower.lower(*ctid);
    llvm::GlobalVariable *gv;
    if (has(sym.flags, SymFlags::Extern)) {
      // extern global: external linkage, no initializer, unmangled name.
      auto sv = cg.interner.view(sym.name);
      gv = new llvm::GlobalVariable(*cg.module, ty,
                                    !has(sym.flags, SymFlags::Mut),
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    llvm::StringRef{sv.data(), sv.size()});
    } else {
      llvm::Constant *init = try_const_init(cg, sym, ty);
      if (!init && sym.init_expr) {
        const BodyIR &sym_ir = cg.modules[sym.module_idx].ir;
        Span sp{sym_ir.nodes.span_s[sym.init_expr],
                sym_ir.nodes.span_e[sym.init_expr]};
        return std::unexpected(Error{sp,
            "global initializer must be a compile-time constant",
            sym.module_idx});
      }
      if (!init) init = llvm::Constant::getNullValue(ty);
      gv = new llvm::GlobalVariable(
          *cg.module, ty, !has(sym.flags, SymFlags::Mut),
          llvm::GlobalValue::ExternalLinkage, init, mangle(i, cg));
    }
    cg.global_map[i] = gv;
  }
  return {};
}

inline void declare_functions(CodegenCtx &cg) {
  for (SymbolId i = 1; i < cg.sema.syms.symbols.size(); ++i) {
    const Symbol &sym = cg.sema.syms.symbols[i];
    if (sym.kind != SymbolKind::Func) continue;
    if (sym.generics_count > 0) continue;
    if (has(sym.flags, SymFlags::ShaderStage))
      continue; // lowered via MLIR shader pipeline
    if (has(sym.flags, SymFlags::ShaderFn))
      continue; // lowered via MLIR shader pipeline

    if (has(sym.flags, SymFlags::Extern)) {
      // Extern function: build LLVM type from the FnType TypeId in
      // annotate_type.
      assert(sym.annotate_type != 0 &&
             "extern func must have a type annotation");
      const TypeAst &ta = cg.modules[sym.module_idx].type_ast;
      assert(ta.kind[sym.annotate_type] == TypeKind::Fn &&
             "extern func type annotation must be a function type");
      TypeLowerer tl = cg.make_type_lowerer(sym.module_idx);

      TypeId ret_tid = ta.a[sym.annotate_type];
      llvm::Type *ret_type = llvm::Type::getVoidTy(cg.ctx);
      if (ret_tid != 0) {
        auto ret_r = tl.lower(ret_tid);
        assert(ret_r && "could not lower extern func return type");
        if (llvm::Type *t = cg.type_lower.lower(*ret_r)) ret_type = t;
      }

      u32 ps = ta.b[sym.annotate_type], pc = ta.c[sym.annotate_type];
      std::vector<llvm::Type *> param_types;
      param_types.reserve(pc);
      for (u32 j = 0; j < pc; ++j) {
        auto pt_r = tl.lower(ta.list[ps + j]);
        assert(pt_r && "could not lower extern func param type");
        param_types.push_back(cg.type_lower.lower(*pt_r));
      }

      auto *ft =
          llvm::FunctionType::get(ret_type, param_types, /*isVarArg=*/false);
      auto sv = cg.interner.view(sym.name);
      auto *fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                        llvm::StringRef{sv.data(), sv.size()},
                                        *cg.module);
      cg.fn_map[i] = fn;
      continue;
    }

    if (sym.body == 0) continue;

    // Mono instances store pre-lowered concrete CTypeIds; others use TypeAst
    // lowering.
    llvm::Type *ret_type;
    std::vector<llvm::Type *> param_types;
    if (sym.is_mono()) {
      const MonoInfo &mi = *sym.mono;
      llvm::Type *ret = cg.type_lower.lower(mi.concrete_ret);
      ret_type = ret ? ret : llvm::Type::getVoidTy(cg.ctx);
      param_types.reserve(mi.concrete_params.size() +
                          (mi.self_ctype != 0 ? 1u : 0u));
      if (mi.self_ctype != 0)
        param_types.push_back(cg.type_lower.lower(mi.self_ctype));
      for (CTypeId ctid : mi.concrete_params)
        param_types.push_back(cg.type_lower.lower(ctid));
    } else {
      TypeLowerer tl = cg.make_type_lowerer(sym.module_idx);
      inject_self_subst(sym, tl, cg);

      auto ret_r = tl.lower(sym.sig.ret_type);
      assert(ret_r && "could not lower return type for function");
      {
        llvm::Type *ret = cg.type_lower.lower(*ret_r);
        ret_type = ret ? ret : llvm::Type::getVoidTy(cg.ctx);
      }

      const Module &mod = cg.modules[sym.module_idx].mod;
      param_types.reserve(sym.sig.params_count);
      for (u32 j = 0; j < sym.sig.params_count; ++j) {
        const FuncParam &fp = mod.params[sym.sig.params_start + j];
        auto pt_r = tl.lower(fp.type);
        assert(pt_r && "could not lower param type");
        param_types.push_back(cg.type_lower.lower(*pt_r));
      }
    }

    // `main` must be declared as `i32 main()` per the C ABI — the Umbral
    // source writes `-> void` but the OS expects an exit code in rax.
    if (cg.interner.view(sym.name) == "main" && ret_type->isVoidTy())
      ret_type = llvm::Type::getInt32Ty(cg.ctx);

    auto *ft =
        llvm::FunctionType::get(ret_type, param_types, /*isVarArg=*/false);
    auto *fn = llvm::Function::Create(
        ft, llvm::Function::LinkageTypes::ExternalLinkage, mangle(i, cg),
        *cg.module);

    const Module &mod2 = cg.modules[sym.module_idx].mod;
    for (u32 j = 0; j < sym.sig.params_count; ++j)
      fn->getArg(j)->setName(
          cg.interner.view(mod2.params[sym.sig.params_start + j].name));

    cg.fn_map[i] = fn;

    if (cg.debug_info && cg.dibuilder && sym.body != 0) {
      llvm::DIFile *di_file = cg.get_di_file(sym.module_idx);
      auto [line, col] = cg.get_line_col(
          sym.module_idx, cg.modules[sym.module_idx].ir.nodes.span_s[sym.body]);
      (void)col;
      llvm::DISubroutineType *di_fn_ty = cg.dibuilder->createSubroutineType(
          cg.dibuilder->getOrCreateTypeArray({}));
      llvm::DISubprogram *sp = cg.dibuilder->createFunction(
          di_file, fn->getName(), fn->getName(), di_file, line, di_fn_ty,
          /*ScopeLine=*/line, llvm::DINode::FlagPrototyped,
          llvm::DISubprogram::SPFlagDefinition);
      fn->setSubprogram(sp);
    }
  }
}

inline void emit_function_bodies(CodegenCtx &cg) {
  for (auto &[sym_id, fn] : cg.fn_map) {
    const Symbol &sym = cg.sema.syms.symbols[sym_id];
    if (has(sym.flags, SymFlags::Extern)) continue; // extern: no body to emit
    const LoadedModule &lm = cg.modules[sym.module_idx];
    const BodySema &bsema = cg.sema.body_semas.at(sym_id);
    FuncEmitter emitter(cg, *fn, sym, lm.ir, lm.mod, bsema);
    emitter.emit();
  }
}

inline llvm::OptimizationLevel to_llvm_opt(u32 level) {
  switch (level) {
  default:
  case 0: return llvm::OptimizationLevel::O0;
  case 1: return llvm::OptimizationLevel::O1;
  case 2: return llvm::OptimizationLevel::O2;
  case 3: return llvm::OptimizationLevel::O3;
  }
}

inline llvm::CodeGenOptLevel to_codegen_opt(u32 level) {
  switch (level) {
  default:
  case 0: return llvm::CodeGenOptLevel::None;
  case 1: return llvm::CodeGenOptLevel::Less;
  case 2: return llvm::CodeGenOptLevel::Default;
  case 3: return llvm::CodeGenOptLevel::Aggressive;
  }
}

// compile mod to a native object file at obj_path.
// call this on CodegenResult::context / CodegenResult::module after
// run_codegen() succeeds (and only when not in --dump-ir mode).
inline Result<void> emit_object(llvm::LLVMContext & /*ctx*/, llvm::Module &mod,
                                const std::string &obj_path,
                                u32 opt_level = 0) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  mod.setTargetTriple(triple);

  std::string err;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
  if (err != "")
    return std::unexpected{Error{0, 0, "could not lookup target: " + err}};

  std::unique_ptr<llvm::TargetMachine> TM(target->createTargetMachine(
      triple, llvm::sys::getHostCPUName(), "", {}, llvm::Reloc::PIC_,
      llvm::CodeModel::Small, to_codegen_opt(opt_level)));

  mod.setDataLayout(TM->createDataLayout());

  std::error_code ec;
  llvm::raw_fd_ostream dest(obj_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    return std::unexpected{
        Error{0, 0, "could not write object file: " + ec.message()}};
  }

  llvm::legacy::PassManager pm;
  if (TM->addPassesToEmitFile(pm, dest, nullptr,
                              llvm::CodeGenFileType::ObjectFile)) {
    return std::unexpected{Error{0, 0, "cannot emit object file for target"}};
  }

  pm.run(mod);
  dest.flush();

  return {};
}

inline void emit_site_table(CodegenCtx &cg) {
  auto &ctx = cg.ctx;
  u32 n = static_cast<u32>(cg.sites.size());

  // { ptr, i32, i32 }
  llvm::Type *ptr_ty = llvm::PointerType::getUnqual(ctx);
  llvm::StructType *site_ty = llvm::StructType::get(
      ctx, {ptr_ty, llvm::Type::getInt32Ty(ctx), llvm::Type::getInt32Ty(ctx)});

  std::vector<llvm::Constant *> entries;
  entries.reserve(n);
  for (auto &se : cg.sites) {
    auto *file_str =
        llvm::ConstantDataArray::getString(ctx, se.file, /*AddNull=*/true);
    auto *gv = new llvm::GlobalVariable(*cg.module, file_str->getType(), true,
                                        llvm::GlobalValue::PrivateLinkage,
                                        file_str, ".site_file");
    llvm::Constant *zero32 =
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0);
    std::array<llvm::Constant *, 2> gep_idx = {zero32, zero32};
    llvm::Constant *file_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
        file_str->getType(), gv, gep_idx);
    entries.push_back(llvm::ConstantStruct::get(
        site_ty,
        {file_ptr, llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), se.line),
         llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), se.col)}));
  }

  llvm::ArrayType *arr_ty = llvm::ArrayType::get(site_ty, n);
  llvm::Constant *arr_init = n > 0 ? llvm::ConstantArray::get(arr_ty, entries)
                                   : llvm::ConstantAggregateZero::get(arr_ty);
  new llvm::GlobalVariable(*cg.module, arr_ty, true,
                           llvm::GlobalValue::ExternalLinkage, arr_init,
                           "__um_sites");
  new llvm::GlobalVariable(
      *cg.module, llvm::Type::getInt32Ty(ctx), true,
      llvm::GlobalValue::ExternalLinkage,
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), n),
      "__um_sites_count");
}

// emit the embedded .umpack data as LLVM globals so the runtime can find it.
// __umpack_embedded_data holds a pointer to the blob (or null if no assets).
// __umpack_embedded_size holds the byte count (or 0).
// these may already exist as extern declarations from @extern in asset.um;
// if so, we turn the declarations into definitions by setting initializers.
inline void emit_embedded_pack(CodegenCtx &cg,
                               const std::vector<uint8_t> &pack_data) {
  auto &ctx = cg.ctx;
  auto *ptr_ty = llvm::PointerType::getUnqual(ctx);
  auto *i64_ty = llvm::Type::getInt64Ty(ctx);

  llvm::Constant *data_ptr;
  uint64_t data_size;

  if (pack_data.empty()) {
    data_ptr = llvm::ConstantPointerNull::get(ptr_ty);
    data_size = 0;
  } else {
    // emit the blob as a private constant array
    llvm::StringRef data_ref(reinterpret_cast<const char *>(pack_data.data()),
                             pack_data.size());
    auto *arr = llvm::ConstantDataArray::getRaw(data_ref, pack_data.size(),
                                                llvm::Type::getInt8Ty(ctx));
    auto *blob = new llvm::GlobalVariable(
        *cg.module, arr->getType(), true, llvm::GlobalValue::PrivateLinkage,
        arr, "__umpack_blob");
    blob->setAlignment(llvm::Align(8));
    data_ptr = blob; // address of the array, type is ptr
    data_size = pack_data.size();
  }

  // __umpack_embedded_data: a global ptr that points to the blob
  if (auto *gv = cg.module->getGlobalVariable("__umpack_embedded_data")) {
    gv->setInitializer(data_ptr);
    gv->setConstant(true);
  } else {
    new llvm::GlobalVariable(*cg.module, ptr_ty, true,
                             llvm::GlobalValue::ExternalLinkage, data_ptr,
                             "__umpack_embedded_data");
  }

  // __umpack_embedded_size: a global i64 holding the byte count
  auto *size_val = llvm::ConstantInt::get(i64_ty, data_size);
  if (auto *gv = cg.module->getGlobalVariable("__umpack_embedded_size")) {
    gv->setInitializer(size_val);
    gv->setConstant(true);
  } else {
    new llvm::GlobalVariable(*cg.module, i64_ty, true,
                             llvm::GlobalValue::ExternalLinkage, size_val,
                             "__umpack_embedded_size");
  }
}

inline Result<CodegenResult>
run_codegen(SemaResult &sema, const std::vector<LoadedModule> &modules,
            const Interner &interner, std::string_view module_name = "umbral",
            bool debug_info = false, u32 opt_level = 0,
            const std::vector<uint8_t> &embedded_pack = {}) {
  CodegenCtx cg(sema, modules, interner, module_name, debug_info);
  auto globals_r = declare_globals(cg);
  if (!globals_r) return std::unexpected(globals_r.error());
  declare_functions(cg);

  // set DataLayout so @size_of/@align_of can use it during body emission.
  {
    llvm::InitializeNativeTarget();
    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    cg.module->setTargetTriple(triple);
    std::string err2;
    const llvm::Target *tgt = llvm::TargetRegistry::lookupTarget(triple, err2);
    if (!tgt) return std::unexpected{Error{{0, 0}, "codegen: " + err2}};
    std::unique_ptr<llvm::TargetMachine> tm(tgt->createTargetMachine(
        triple, llvm::sys::getHostCPUName(), "", {}, llvm::Reloc::PIC_,
        llvm::CodeModel::Small, to_codegen_opt(opt_level)));
    cg.module->setDataLayout(tm->createDataLayout());
  }

  emit_function_bodies(cg);
  emit_site_table(cg);
  emit_embedded_pack(cg, embedded_pack);

  if (cg.debug_info && cg.dibuilder) cg.dibuilder->finalize();

  std::string err;
  llvm::raw_string_ostream es(err);
  if (llvm::verifyModule(*cg.module, &es))
    return std::unexpected{Error{{0, 0}, "LLVM verification failed: " + err}};

  // run LLVM optimization passes when opt_level > 0.
  if (opt_level > 0) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    auto mpm = pb.buildPerModuleDefaultPipeline(to_llvm_opt(opt_level));
    mpm.run(*cg.module, mam);
  }

  // serialise IR text (only used by --dump-ir; always populated for now).
  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  cg.module->print(os, nullptr);

  // transfer ownership of context+module to the result so the caller can
  // use them for object-file emission without the context dying here.
  return CodegenResult{ir_str, std::move(cg.ctx_owned), std::move(cg.module)};
}
