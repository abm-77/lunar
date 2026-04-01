// top-level shader compilation pipeline: BodyIR → um.shader → SPIR-V → .spv + .umrf

#include <um/shader/shader_compile.h>
#include <um/shader/shader_mlir.h>
#include <um/shader/shader_spirv_lower.h>
#include <um/shader/shader_spirv_emit.h>
#include <um/shader/umrf_emit.h>

#include <mlir/IR/MLIRContext.h>

#include <compiler/frontend/module.h>

#include <cstdio>
#include <unordered_set>

namespace um::shader {

bool shader_compile(std::span<const LoadedModule> modules,
                    const SemaResult &sema,
                    Interner &interner,
                    const ShaderCompileOptions &opts) {
  mlir::MLIRContext ctx;

  // phase 1: BodyIR → um.shader MLIR
  auto mlir_mod = lower_to_mlir(ctx, modules, sema, interner);
  if (!mlir_mod) {
    fprintf(stderr, "shader_compile: lower_to_mlir failed\n");
    return false;
  }

  // phase 2: um.shader → MLIR SPIR-V
  if (!run_spirv_lower(ctx, mlir_mod.get(), modules, sema, interner)) {
    fprintf(stderr, "shader_compile: run_spirv_lower failed\n");
    return false;
  }

  // phase 3: serialize SPIR-V modules to .spv files
  if (!emit_spirv_binaries(mlir_mod.get(), opts.out_dir)) {
    fprintf(stderr, "shader_compile: emit_spirv_binaries failed\n");
    return false;
  }

  // phase 4: emit .umrf for each shader type that has a @vs_in field
  std::unordered_set<SymId> emitted;
  for (const auto &lm : modules) {
    for (const ShaderStageInfo &si : lm.mod.shader_stages) {
      if (emitted.count(si.shader_type)) continue;
      emitted.insert(si.shader_type);
      // only emit umrf if this shader has a @vs_in annotation
      bool has_vs_in = false;
      for (const ShaderFieldAnnot &sfa : lm.mod.shader_field_annots) {
        if (sfa.struct_name == si.shader_type &&
            sfa.kind == ShaderFieldKind::VsIn) {
          has_vs_in = true;
          break;
        }
      }
      if (has_vs_in) {
        if (!emit_umrf(lm, interner, si.shader_type, opts.out_dir)) {
          fprintf(stderr, "shader_compile: emit_umrf failed for %s\n",
                  std::string(interner.view(si.shader_type)).c_str());
          return false;
        }
      }
    }
  }

  return true;
}

} // namespace um::shader
