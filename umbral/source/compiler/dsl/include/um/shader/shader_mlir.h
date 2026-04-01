#pragma once

#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/OwningOpRef.h>

#include <compiler/loader.h>
#include <compiler/sema/sema.h>
#include <common/interner.h>

#include <span>

namespace um::shader {

// lower every @stage and @shader_fn method across all modules to an
// mlir::ModuleOp containing um.shader + arith + scf + cf + vector ops.
// one func.func per stage/helper, carrying a {stage = "vertex"|"fragment"}
// attribute on stage functions. returns nullptr on failure (prints to stderr).
mlir::OwningOpRef<mlir::ModuleOp>
lower_to_mlir(mlir::MLIRContext &ctx,
              std::span<const LoadedModule> modules,
              const SemaResult &sema,
              const Interner &interner);

} // namespace um::shader
