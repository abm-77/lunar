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
    return llvm::ConstantInt::get(ty,
               static_cast<int64_t>(ir.int_lits[ir.nodes.a[n]]));
  case NodeKind::FloatLit:
    return llvm::ConstantFP::get(ty, ir.float_lits[ir.nodes.a[n]]);
  case NodeKind::BoolLit:
    return llvm::ConstantInt::getBool(ty->getContext(), ir.nodes.a[n] != 0);

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
      case TokenKind::Plus:  return llvm::ConstantInt::get(ty, li->getValue() + ri->getValue());
      case TokenKind::Minus: return llvm::ConstantInt::get(ty, li->getValue() - ri->getValue());
      case TokenKind::Star:  return llvm::ConstantInt::get(ty, li->getValue() * ri->getValue());
      case TokenKind::Slash: {
        if (ri->isZero()) return nullptr;
        return llvm::ConstantInt::get(ty, li->getValue().udiv(ri->getValue()));
      }
      case TokenKind::Percent: {
        if (ri->isZero()) return nullptr;
        return llvm::ConstantInt::get(ty, li->getValue().urem(ri->getValue()));
      }
      case TokenKind::Ampersand: return llvm::ConstantInt::get(ty, li->getValue() & ri->getValue());
      case TokenKind::Pipe:      return llvm::ConstantInt::get(ty, li->getValue() | ri->getValue());
      case TokenKind::Caret:     return llvm::ConstantInt::get(ty, li->getValue() ^ ri->getValue());
      default: return nullptr;
      }
    }

    if (ty->isFloatingPointTy()) {
      auto *lf = llvm::cast<llvm::ConstantFP>(lhs);
      auto *rf = llvm::cast<llvm::ConstantFP>(rhs);
      const auto &la = lf->getValueAPF(), &ra = rf->getValueAPF();
      llvm::APFloat result(la);
      switch (op) {
      case TokenKind::Plus:  result.add(ra, llvm::APFloat::rmNearestTiesToEven); break;
      case TokenKind::Minus: result.subtract(ra, llvm::APFloat::rmNearestTiesToEven); break;
      case TokenKind::Star:  result.multiply(ra, llvm::APFloat::rmNearestTiesToEven); break;
      case TokenKind::Slash: result.divide(ra, llvm::APFloat::rmNearestTiesToEven); break;
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

  default: return nullptr;
  }
}

static llvm::Constant *try_const_init(CodegenCtx &cg, const Symbol &sym,
                                      llvm::Type *ty) {
  if (!sym.init_expr) return nullptr;
  const BodyIR &ir = cg.modules[sym.module_idx].ir;
  return eval_const_expr(cg, ir, sym.module_idx, sym.init_expr, ty);
}

inline void declare_globals(CodegenCtx &cg) {
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
      if (!init) init = llvm::Constant::getNullValue(ty);
      gv = new llvm::GlobalVariable(
          *cg.module, ty, !has(sym.flags, SymFlags::Mut),
          llvm::GlobalValue::ExternalLinkage, init, mangle(i, cg));
    }
    cg.global_map[i] = gv;
  }
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

// compile mod to a native object file at obj_path.
// Call this on CodegenResult::context / CodegenResult::module after
// run_codegen() succeeds (and only when not in --dump-ir mode).
inline Result<void> emit_object(llvm::LLVMContext & /*ctx*/, llvm::Module &mod,
                                const std::string &obj_path) {
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
      llvm::CodeModel::Small, llvm::CodeGenOptLevel::None));

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

inline Result<CodegenResult>
run_codegen(SemaResult &sema, const std::vector<LoadedModule> &modules,
            const Interner &interner, std::string_view module_name = "umbral") {
  CodegenCtx cg(sema, modules, interner, module_name);
  declare_globals(cg);
  declare_functions(cg);

  // Set DataLayout so @size_of/@align_of can use it during body emission.
  {
    llvm::InitializeNativeTarget();
    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    cg.module->setTargetTriple(triple);
    std::string err2;
    const llvm::Target *tgt = llvm::TargetRegistry::lookupTarget(triple, err2);
    if (!tgt) return std::unexpected{Error{{0, 0}, "codegen: " + err2}};
    std::unique_ptr<llvm::TargetMachine> tm(tgt->createTargetMachine(
        triple, llvm::sys::getHostCPUName(), "", {}, llvm::Reloc::PIC_,
        llvm::CodeModel::Small, llvm::CodeGenOptLevel::None));
    cg.module->setDataLayout(tm->createDataLayout());
  }

  emit_function_bodies(cg);
  emit_site_table(cg);

  std::string err;
  llvm::raw_string_ostream es(err);
  if (llvm::verifyModule(*cg.module, &es))
    return std::unexpected{Error{{0, 0}, "LLVM verification failed: " + err}};

  // Serialise IR text (only used by --dump-ir; always populated for now).
  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  cg.module->print(os, nullptr);

  // Transfer ownership of context+module to the result so the caller can
  // use them for object-file emission without the context dying here.
  return CodegenResult{ir_str, std::move(cg.ctx_owned), std::move(cg.module)};
}
