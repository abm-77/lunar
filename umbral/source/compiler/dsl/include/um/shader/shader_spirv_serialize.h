#pragma once

#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>

#include <llvm/ADT/SmallVector.h>

#include <cstdint>

namespace um::shader {

// serialize a spirv.module to SPIR-V binary words, handling both standard
// spirv.* ops and our custom CombineSampledImageOp (OpSampledImage, opcode 86).
// returns false on failure.
bool serialize_spirv_module(mlir::spirv::ModuleOp spirv_mod,
                            llvm::SmallVectorImpl<uint32_t> &binary);

} // namespace um::shader
