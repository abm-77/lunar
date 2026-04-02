#pragma once

#include <common/interner.h>
#include <compiler/driver/loader.h>
#include <compiler/sema/sema.h>

#include <llvm/ADT/SmallVector.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/IR/OwningOpRef.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace um::shader {

// .umsh output data types

struct UmshStageData {
  uint8_t kind; // 0 = vertex, 1 = fragment
  std::vector<uint32_t> spv_words;
};

struct UmshAttr {
  uint32_t location;
  uint32_t vk_format;
  uint32_t offset;
};

struct UmshReflData {
  uint32_t stride;
  uint32_t input_rate; // 0 = per-vertex
  std::vector<UmshAttr> attrs;
};

// pipeline options

struct ShaderCompileOptions {
  std::string out_dir;
};

// full pipeline: BodyIR → um.shader MLIR → SPIR-V → .umsh files.
bool shader_compile(std::span<const LoadedModule> modules,
                    const SemaResult &sema, Interner &interner,
                    const ShaderCompileOptions &opts);

// individual pipeline phases (used by shader_compile and tests)

mlir::OwningOpRef<mlir::ModuleOp>
lower_to_mlir(mlir::MLIRContext &ctx, std::span<const LoadedModule> modules,
              const SemaResult &sema, const Interner &interner);

bool run_spirv_lower(mlir::MLIRContext &ctx, mlir::ModuleOp mlir_mod,
                     std::span<const LoadedModule> modules,
                     const SemaResult &sema, Interner &interner);

bool serialize_spirv_module(mlir::spirv::ModuleOp spirv_mod,
                            llvm::SmallVectorImpl<uint32_t> &binary);

std::unordered_map<std::string, std::vector<UmshStageData>>
collect_spirv_stages(mlir::ModuleOp mlir_mod);

std::optional<UmshReflData> collect_refl(const LoadedModule &lm,
                                         const Interner &interner,
                                         SymId shader_type);

bool emit_umsh(std::string_view out_dir, std::string_view shader_name,
               std::span<const UmshStageData> stages, const UmshReflData *refl);

} // namespace um::shader
