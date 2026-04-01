// serialize spirv.module ops inside an mlir::ModuleOp to .spv binary files.

#include <um/shader/shader_spirv_emit.h>
#include <um/shader/shader_spirv_serialize.h>

#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace um::shader {

bool emit_spirv_binaries(mlir::ModuleOp mlir_mod, std::string_view out_dir) {
  bool ok = true;

  mlir_mod.walk([&](mlir::spirv::ModuleOp spirv_mod) {
    // read shader_type and stage attrs set by shader_spirv_lower
    auto shader_type_attr = spirv_mod->getAttrOfType<mlir::StringAttr>("shader_type");
    auto stage_attr = spirv_mod->getAttrOfType<mlir::StringAttr>("stage");
    if (!shader_type_attr || !stage_attr) {
      fprintf(stderr, "shader_spirv_emit: spirv.module missing shader_type/stage attrs\n");
      ok = false;
      return;
    }
    std::string shader_name = shader_type_attr.getValue().str();
    std::string stage_ext = (stage_attr.getValue() == "vertex") ? "vert" : "frag";

    // serialize to binary words (handles CombineSampledImageOp → OpSampledImage)
    llvm::SmallVector<uint32_t, 4096> binary;
    if (!serialize_spirv_module(spirv_mod, binary)) {
      fprintf(stderr, "shader_spirv_emit: serialization failed for %s.%s\n",
              shader_name.c_str(), stage_ext.c_str());
      ok = false;
      return;
    }

    // write to file
    std::string path = std::string(out_dir) + "/" + shader_name + "." +
                       stage_ext + ".spv";
    std::ofstream f(path, std::ios::binary);
    if (!f) {
      fprintf(stderr, "shader_spirv_emit: can't open %s\n", path.c_str());
      ok = false;
      return;
    }
    f.write(reinterpret_cast<const char *>(binary.data()),
            binary.size() * sizeof(uint32_t));
    if (!f) {
      fprintf(stderr, "shader_spirv_emit: write failed for %s\n", path.c_str());
      ok = false;
    }
  });

  return ok;
}

} // namespace um::shader
