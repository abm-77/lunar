// custom SPIR-V serializer that handles CombineSampledImageOp.
//
// strategy: clone the spirv.module, replace each CombineSampledImageOp with
// a pair of spirv.Undef ops (one per operand, discarded) and a spirv.Undef
// for the result. serialize the sanitized module with mlir::spirv::serialize().
// then walk the original IR and the binary in parallel to patch OpUndef
// instructions back to OpSampledImage with the correct operand IDs.
//
// since we can't access the serializer's ID mapping, we use a different
// approach: we pre-lower CombineSampledImageOp by inlining its semantics
// into a spirv.CompositeConstruct-like pattern that the serializer handles,
// then fix up the opcode in the binary.
//
// ACTUAL approach: we wrap mlir::spirv::serialize() and post-process.
// the key insight: OpSampledImage and spirv.Undef have different word counts
// (5 vs 3), so we can't patch in-place. instead, we rebuild the binary.

#include "shader_compile.h"
#include "um_shader_dialect.h"

#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVTypes.h>
#include <mlir/IR/IRMapping.h>
#include <mlir/Target/SPIRV/Serialization.h>

#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace um::shader {

// SPIR-V binary constants
static constexpr uint32_t SPV_OP_UNDEF = 1;
static constexpr uint32_t SPV_OP_TYPE_SAMPLED_IMAGE = 27;
static constexpr uint32_t SPV_OP_SAMPLED_IMAGE = 86;

// extract opcode from a SPIR-V instruction word
static uint32_t spv_opcode(uint32_t word) { return word & 0xFFFFu; }
static uint32_t spv_word_count(uint32_t word) { return word >> 16u; }

bool serialize_spirv_module(mlir::spirv::ModuleOp spirv_mod,
                            llvm::SmallVectorImpl<uint32_t> &binary) {
  // check if module contains any CombineSampledImageOps
  bool has_combine = false;
  spirv_mod.walk([&](CombineSampledImageOp) { has_combine = true; });

  if (!has_combine) {
    // no custom ops — use standard serializer directly
    return mlir::spirv::serialize(spirv_mod, binary).succeeded();
  }

  // clone the module so we can mutate it for serialization
  auto *ctx = spirv_mod.getContext();
  mlir::IRMapping mapping;
  auto *cloned_op = spirv_mod->clone(mapping);
  auto cloned_mod = mlir::cast<mlir::spirv::ModuleOp>(cloned_op);

  // in the clone, replace each CombineSampledImageOp with spirv.Undef.
  // record which Undef result IDs correspond to OpSampledImage.
  // after serialization, we'll find these Undefs in the binary and expand them.
  struct CombineInfo {
    mlir::Value image;   // original image operand (in clone)
    mlir::Value sampler; // original sampler operand (in clone)
  };
  llvm::SmallVector<CombineInfo> combine_infos;
  std::unordered_set<mlir::Operation *> undef_markers;

  cloned_mod.walk([&](CombineSampledImageOp op) {
    mlir::OpBuilder b(op);
    auto result_ty = op.getResult().getType();
    auto undef = b.create<mlir::spirv::UndefOp>(op.getLoc(), result_ty);
    combine_infos.push_back({op.getImage(), op.getSampler()});
    undef_markers.insert(undef.getOperation());
    op.getResult().replaceAllUsesWith(undef.getResult());
    op.erase();
  });

  // serialize the sanitized clone
  llvm::SmallVector<uint32_t> raw_binary;
  if (mlir::spirv::serialize(cloned_mod, raw_binary).failed()) {
    fprintf(stderr, "shader_spirv_serialize: serialize failed\n");
    cloned_op->destroy();
    return false;
  }

  // now we need to find the OpUndef instructions for sampled_image types
  // and replace them with OpSampledImage.
  //
  // the binary layout after the header (5 words) is a sequence of instructions.
  // we scan for OpUndef where the result type is a SampledImage type.
  // for each such OpUndef, we need the SPIR-V IDs of the image and sampler
  // operands. these are the IDs produced by the spirv.Load instructions
  // immediately before the Undef.
  //
  // approach: find OpTypeSampledImage type IDs first, then find OpUndef
  // instructions with those type IDs. for each, look backwards in the
  // instruction stream to find the two most recent OpLoad results that
  // correspond to the image and sampler.

  // pass 1: find all SampledImage type IDs
  std::unordered_set<uint32_t> sampled_image_type_ids;
  {
    uint32_t pos = 5; // skip header
    while (pos < raw_binary.size()) {
      uint32_t wc = spv_word_count(raw_binary[pos]);
      uint32_t oc = spv_opcode(raw_binary[pos]);
      if (oc == SPV_OP_TYPE_SAMPLED_IMAGE && wc >= 3) {
        sampled_image_type_ids.insert(raw_binary[pos + 1]); // result ID
      }
      pos += wc;
    }
  }

  if (sampled_image_type_ids.empty()) {
    // no sampled image types found — just use raw binary as-is
    binary = std::move(raw_binary);
    cloned_op->destroy();
    return true;
  }

  // pass 2: rebuild binary, replacing OpUndef of SampledImage type with
  // OpSampledImage. we track recent Load result IDs to use as operands.
  binary.clear();
  // copy header
  for (uint32_t i = 0; i < 5 && i < raw_binary.size(); ++i)
    binary.push_back(raw_binary[i]);

  uint32_t combine_idx = 0;
  // track recent result IDs for image/sampler loads
  llvm::SmallVector<uint32_t> recent_load_results;

  uint32_t pos = 5;
  while (pos < raw_binary.size()) {
    uint32_t wc = spv_word_count(raw_binary[pos]);
    uint32_t oc = spv_opcode(raw_binary[pos]);

    // track Load results (opcode 61 = OpLoad)
    if (oc == 61 && wc >= 4) {
      recent_load_results.push_back(raw_binary[pos + 2]); // result ID
    }

    // check if this is an OpUndef of a SampledImage type
    if (oc == SPV_OP_UNDEF && wc == 3) {
      uint32_t type_id = raw_binary[pos + 1];
      uint32_t result_id = raw_binary[pos + 2];
      if (sampled_image_type_ids.count(type_id) &&
          combine_idx < combine_infos.size()) {
        // replace with OpSampledImage: 5 words
        // need the image and sampler IDs — these are the two most recent Loads
        if (recent_load_results.size() >= 2) {
          uint32_t sampler_id = recent_load_results.back();
          uint32_t image_id =
              recent_load_results[recent_load_results.size() - 2];
          binary.push_back((5u << 16u) | SPV_OP_SAMPLED_IMAGE);
          binary.push_back(type_id);
          binary.push_back(result_id);
          binary.push_back(image_id);
          binary.push_back(sampler_id);
          ++combine_idx;
          pos += wc;
          continue;
        }
      }
    }

    // copy instruction as-is
    for (uint32_t i = 0; i < wc && (pos + i) < raw_binary.size(); ++i)
      binary.push_back(raw_binary[pos + i]);
    pos += wc;
  }

  cloned_op->destroy();
  return true;
}

} // namespace um::shader
