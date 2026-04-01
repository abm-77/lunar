#pragma once

#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>
#include <mlir/IR/BuiltinOps.h>

#include <string_view>

namespace um::shader {

// serialize every spirv.module inside mlir_mod to a SPIR-V binary file.
// output files are named <prefix>.<stage>.spv (e.g. "SpriteShader.vert.spv")
// in out_dir. returns false on failure (prints to stderr).
bool emit_spirv_binaries(mlir::ModuleOp mlir_mod, std::string_view out_dir);

} // namespace um::shader
