// top-level shader compilation pipeline: BodyIR → um.shader → SPIR-V → .umsh

#include "shader_compile.h"

#include <mlir/IR/MLIRContext.h>

#include <compiler/frontend/module.h>

#include <cstdio>
#include <unordered_set>

namespace um::shader {

bool shader_compile(std::span<const LoadedModule> modules,
                    const SemaResult &sema, Interner &interner,
                    const ShaderCompileOptions &opts) {
  mlir::MLIRContext ctx;

  // phase 1: BodyIR → um.shader MLIR
  auto mlir_mod = lower_to_mlir(ctx, modules, sema, interner);
  if (!mlir_mod) {
    fprintf(stderr, "shader_compile: lower_to_mlir failed\n");
    return false;
  }

  // phase 2: um.shader → MLIR SPIR-V
  if (!run_spirv_lower(ctx, mlir_mod.get(), modules, sema, interner,
                       opts.opt_level)) {
    fprintf(stderr, "shader_compile: run_spirv_lower failed\n");
    return false;
  }

  // phase 3: collect SPIR-V stages into memory (one entry per shader name)
  auto stages_map = collect_spirv_stages(mlir_mod.get());
  if (stages_map.empty() && !modules.empty()) {
    // check if there really were shader stages to emit
    bool any_stages = false;
    for (const auto &lm : modules)
      if (!lm.mod.shader_stages.empty()) {
        any_stages = true;
        break;
      }
    if (any_stages) {
      fprintf(stderr, "shader_compile: collect_spirv_stages failed\n");
      return false;
    }
  }

  // phase 4: for each shader type, collect reflection and write .umsh
  std::unordered_set<SymId> emitted;
  for (const auto &lm : modules) {
    for (const ShaderStageInfo &si : lm.mod.shader_stages) {
      if (emitted.count(si.shader_type)) continue;
      emitted.insert(si.shader_type);

      std::string shader_name = std::string(interner.view(si.shader_type));
      auto it = stages_map.find(shader_name);
      if (it == stages_map.end()) {
        fprintf(stderr, "shader_compile: no SPIR-V collected for '%s'\n",
                shader_name.c_str());
        return false;
      }

      auto refl_opt = collect_refl(lm, interner, si.shader_type);
      // collect_refl returns nullopt both for "no @vs_in" and error; errors
      // already print to stderr; treat both as no refl for emit purposes.
      const UmshReflData *refl_ptr =
          refl_opt.has_value() ? &*refl_opt : nullptr;

      if (!emit_umsh(opts.out_dir, shader_name, it->second, refl_ptr)) {
        fprintf(stderr, "shader_compile: emit_umsh failed for '%s'\n",
                shader_name.c_str());
        return false;
      }
    }
  }

  return true;
}

} // namespace um::shader
