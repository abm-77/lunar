#pragma once

#include <expected>
#include <string>
#include <unordered_map>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <common/error.h>
#include <common/interner.h>
#include <compiler/codegen/codegen_ctx.h>
#include <compiler/loader.h>
#include <compiler/sema/body_check.h>
#include <compiler/sema/sema.h>

#include "func_emit.h"
#include "type_lower.h"

struct CodegenResult {
  std::string ir; // serialised LLVM IR text (.ll)
};

// ── Name mangling
// ───────────────────────────────────────────────────────────── Scheme:
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
    llvm::Constant *init = llvm::Constant::getNullValue(ty);
    auto *gv = new llvm::GlobalVariable(*cg.module, ty, !sym.is_mut,
                                        llvm::GlobalValue::ExternalLinkage,
                                        init, mangle(i, cg));
    cg.global_map[i] = gv;
  }
}

inline void declare_functions(CodegenCtx &cg) {
  for (SymbolId i = 1; i < cg.sema.syms.symbols.size(); ++i) {
    const Symbol &sym = cg.sema.syms.symbols[i];
    if (sym.kind != SymbolKind::Func || sym.body == 0) continue;
    if (sym.generics_count > 0) continue;

    // Mono instances store pre-lowered concrete CTypeIds; others use TypeAst lowering.
    llvm::Type *ret_type;
    std::vector<llvm::Type *> param_types;
    if (sym.is_mono_instance) {
      llvm::Type *ret = cg.type_lower.lower(sym.mono_concrete_ret);
      ret_type = ret ? ret : llvm::Type::getVoidTy(cg.ctx);
      param_types.reserve(sym.mono_concrete_params.size());
      for (CTypeId ctid : sym.mono_concrete_params)
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
    const LoadedModule &lm = cg.modules[sym.module_idx];
    const BodySema &bsema = cg.sema.body_semas.at(sym_id);
    FuncEmitter emitter(cg, *fn, sym, lm.ir, lm.mod, bsema);
    emitter.emit();
  }
}

inline Result<CodegenResult>
run_codegen(SemaResult &sema, const std::vector<LoadedModule> &modules,
            const Interner &interner, std::string_view module_name = "umbral") {
  CodegenCtx cg(sema, modules, interner, module_name);
  declare_globals(cg);
  declare_functions(cg);
  emit_function_bodies(cg);

  std::string err;
  llvm::raw_string_ostream es(err);
  if (llvm::verifyModule(*cg.module, &es))
    return std::unexpected{Error{{0, 0}, "LLVM verification failed: " + err}};

  std::string ir_str;
  llvm::raw_string_ostream os(ir_str);
  cg.module->print(os, nullptr);
  return CodegenResult{ir_str};
}
