#pragma once

#include <compiler/loader.h>
#include <compiler/sema/sema.h>
#include <common/interner.h>

#include <span>
#include <string>

namespace um::shader {

struct ShaderCompileOptions {
  std::string out_dir; // directory to write .spv and .umrf files
};

// full shader compilation pipeline: BodyIR → um.shader MLIR → SPIR-V MLIR
// → .spv binaries + .umrf reflection blobs.
// returns false on failure (prints to stderr).
bool shader_compile(std::span<const LoadedModule> modules,
                    const SemaResult &sema,
                    Interner &interner,
                    const ShaderCompileOptions &opts);

} // namespace um::shader
