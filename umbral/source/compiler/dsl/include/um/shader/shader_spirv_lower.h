#pragma once

#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>

#include <compiler/loader.h>
#include <compiler/sema/sema.h>
#include <common/interner.h>

#include <span>

namespace um::shader {

// lower um.shader + arith + scf + memref + vector ops inside mlir_mod to
// spirv.module ops. one spirv.module is emitted per stage func.func
// (identified by {stage = "vertex"|"fragment"} attribute). helper func.funcs
// are cloned into each spirv.module that calls them.
// returns false on failure (prints to stderr).
bool run_spirv_lower(mlir::MLIRContext &ctx,
                     mlir::ModuleOp mlir_mod,
                     std::span<const LoadedModule> modules,
                     const SemaResult &sema,
                     Interner &interner);

} // namespace um::shader
