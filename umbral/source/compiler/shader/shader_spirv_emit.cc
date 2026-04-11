// collect spirv.module ops inside an mlir::ModuleOp into in-memory stage data.

#include "shader_compile.h"

#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>
#include <mlir/Target/SPIRV/Serialization.h>

#include <cstdio>

namespace um::shader {

std::unordered_map<std::string, std::vector<UmshStageData>>
collect_spirv_stages(mlir::ModuleOp mlir_mod) {
  std::unordered_map<std::string, std::vector<UmshStageData>> result;
  bool ok = true;

  mlir_mod.walk([&](mlir::spirv::ModuleOp spirv_mod) {
    if (!ok) return;
    auto shader_type_attr =
        spirv_mod->getAttrOfType<mlir::StringAttr>("shader_type");
    auto stage_attr = spirv_mod->getAttrOfType<mlir::StringAttr>("stage");
    if (!shader_type_attr || !stage_attr) {
      fprintf(stderr, "collect_spirv_stages: spirv.module missing "
                      "shader_type/stage attrs\n");
      ok = false;
      return;
    }
    std::string shader_name = shader_type_attr.getValue().str();
    std::string stage_str = stage_attr.getValue().str();

    llvm::SmallVector<uint32_t, 4096> words;
    if (mlir::spirv::serialize(spirv_mod, words).failed()) {
      fprintf(stderr, "collect_spirv_stages: serialization failed for %s.%s\n",
              shader_name.c_str(), stage_str.c_str());
      ok = false;
      return;
    }

    UmshStageData sd;
    sd.kind = (stage_str == "vertex") ? 0 : 1;
    sd.spv_words = std::vector<uint32_t>(words.begin(), words.end());
    result[shader_name].push_back(std::move(sd));
  });

  if (!ok) return {};
  return result;
}

} // namespace um::shader
