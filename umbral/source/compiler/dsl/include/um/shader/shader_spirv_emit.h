#pragma once

#include <um/shader/umsh_emit.h>

#include <mlir/IR/BuiltinOps.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace um::shader {

// collect_spirv_stages — serialize all spirv.module ops inside mlir_mod.
// returns a map from shader_name → list of UmshStageData (one per stage).
// each spirv.module must have "shader_type" and "stage" string attrs (set by
// shader_spirv_lower). returns an empty map on failure (prints to stderr).
std::unordered_map<std::string, std::vector<UmshStageData>>
collect_spirv_stages(mlir::ModuleOp mlir_mod);

} // namespace um::shader
