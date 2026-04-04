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

static void fixup_spirv_decorations(llvm::SmallVectorImpl<uint32_t> &binary);

bool serialize_spirv_module(mlir::spirv::ModuleOp spirv_mod,
                            llvm::SmallVectorImpl<uint32_t> &binary) {
  // check if module contains any CombineSampledImageOps
  bool has_combine = false;
  spirv_mod.walk([&](CombineSampledImageOp) { has_combine = true; });

  if (!has_combine) {
    if (mlir::spirv::serialize(spirv_mod, binary).failed())
      return false;
    fixup_spirv_decorations(binary);
    return true;
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
    // use CompositeConstruct as a placeholder — same word count as
    // OpSampledImage (5 words). the serializer emits the correct operand IDs.
    // we patch the opcode from OpCompositeConstruct (80) to OpSampledImage (86)
    // after serialization.
    auto cc = mlir::spirv::CompositeConstructOp::create(
        b, op.getLoc(), result_ty,
        mlir::ValueRange{op.getImage(), op.getSampler()});
    combine_infos.push_back({op.getImage(), op.getSampler()});
    op.getResult().replaceAllUsesWith(cc.getResult());
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
    fixup_spirv_decorations(binary);
    cloned_op->destroy();
    return true;
  }

  // pass 2: rebuild binary, patching OpCompositeConstruct of SampledImage type
  // to OpSampledImage. the CompositeConstruct was emitted as a placeholder with
  // the correct operands (image, sampler); we just change the opcode.
  static constexpr uint32_t SPV_OP_COMPOSITE_CONSTRUCT = 80;

  binary.clear();
  for (uint32_t i = 0; i < 5 && i < raw_binary.size(); ++i)
    binary.push_back(raw_binary[i]);

  uint32_t pos = 5;
  while (pos < raw_binary.size()) {
    uint32_t wc = spv_word_count(raw_binary[pos]);
    uint32_t oc = spv_opcode(raw_binary[pos]);

    // patch OpCompositeConstruct → OpSampledImage when result type is SampledImage
    if (oc == SPV_OP_COMPOSITE_CONSTRUCT && wc == 5) {
      uint32_t type_id = raw_binary[pos + 1];
      if (sampled_image_type_ids.count(type_id)) {
        binary.push_back((wc << 16u) | SPV_OP_SAMPLED_IMAGE);
        for (uint32_t i = 1; i < wc && (pos + i) < raw_binary.size(); ++i)
          binary.push_back(raw_binary[pos + i]);
        pos += wc;
        continue;
      }
    }

    for (uint32_t i = 0; i < wc && (pos + i) < raw_binary.size(); ++i)
      binary.push_back(raw_binary[pos + i]);
    pos += wc;
  }

  // pass 3: patch the sampler variable's type chain.
  // the "samplers" variable is Binding=1, UniformConstant, typed as
  // ptr → RuntimeArray → i32. we need to change the i32 element type to
  // OpTypeSampler. find the pointer type → runtime array type → element type
  // and replace the element type's definition with OpTypeSampler.
  //
  // strategy: find OpVariable with StorageClass UniformConstant that is
  // decorated with Binding=1. then trace its type chain:
  //   OpTypePointer → OpTypeRuntimeArray → OpTypeInt 32 0
  // and replace OpTypeInt with OpTypeSampler (opcode 26, 2 words).
  {
    static constexpr uint32_t SPV_OP_DECORATE = 71;
    static constexpr uint32_t SPV_OP_VARIABLE = 59;
    static constexpr uint32_t SPV_OP_TYPE_POINTER = 32;
    static constexpr uint32_t SPV_OP_TYPE_RUNTIME_ARRAY = 29;
    static constexpr uint32_t SPV_OP_TYPE_INT = 21;
    static constexpr uint32_t SPV_OP_TYPE_SAMPLER = 26;
    static constexpr uint32_t SPV_DECORATION_BINDING = 33;
    static constexpr uint32_t SPV_STORAGE_CLASS_UNIFORM_CONSTANT = 0;

    // find the variable ID with Binding=1 decoration
    uint32_t sampler_var_id = 0;
    pos = 5;
    while (pos < binary.size()) {
      uint32_t wc = spv_word_count(binary[pos]);
      uint32_t oc = spv_opcode(binary[pos]);
      if (oc == SPV_OP_DECORATE && wc >= 4 &&
          binary[pos + 2] == SPV_DECORATION_BINDING &&
          binary[pos + 3] == 1)
        sampler_var_id = binary[pos + 1];
      pos += wc;
    }

    if (sampler_var_id) {
      // find the OpVariable to get its type ID (pointer type)
      uint32_t ptr_type_id = 0;
      pos = 5;
      while (pos < binary.size()) {
        uint32_t wc = spv_word_count(binary[pos]);
        uint32_t oc = spv_opcode(binary[pos]);
        if (oc == SPV_OP_VARIABLE && wc >= 4 && binary[pos + 2] == sampler_var_id)
          ptr_type_id = binary[pos + 1];
        pos += wc;
      }

      // find OpTypePointer to get the pointee type (runtime array)
      uint32_t rta_type_id = 0;
      if (ptr_type_id) {
        pos = 5;
        while (pos < binary.size()) {
          uint32_t wc = spv_word_count(binary[pos]);
          uint32_t oc = spv_opcode(binary[pos]);
          if (oc == SPV_OP_TYPE_POINTER && wc >= 4 &&
              binary[pos + 1] == ptr_type_id &&
              binary[pos + 2] == SPV_STORAGE_CLASS_UNIFORM_CONSTANT)
            rta_type_id = binary[pos + 3];
          pos += wc;
        }
      }

      // find OpTypeRuntimeArray to get the element type
      uint32_t elem_type_id = 0;
      if (rta_type_id) {
        pos = 5;
        while (pos < binary.size()) {
          uint32_t wc = spv_word_count(binary[pos]);
          uint32_t oc = spv_opcode(binary[pos]);
          if (oc == SPV_OP_TYPE_RUNTIME_ARRAY && wc >= 3 &&
              binary[pos + 1] == rta_type_id)
            elem_type_id = binary[pos + 2];
          pos += wc;
        }
      }

      // allocate new type IDs for OpTypeSampler and a new
      // OpTypePointer(UniformConstant, Sampler) so that both the runtime array
      // element type and the access chain result type are correct.
      if (elem_type_id) {
        uint32_t id_bound = binary[3];
        uint32_t sampler_type_id = id_bound;
        uint32_t sampler_ptr_type_id = id_bound + 1;
        binary[3] = id_bound + 2;

        // find the existing OpTypePointer(UniformConstant, elem_type_id)
        // used by OpAccessChain into the sampler array
        uint32_t old_elem_ptr_id = 0;
        pos = 5;
        while (pos < binary.size()) {
          uint32_t wc = spv_word_count(binary[pos]);
          uint32_t oc = spv_opcode(binary[pos]);
          if (oc == SPV_OP_TYPE_POINTER && wc >= 4 &&
              binary[pos + 2] == SPV_STORAGE_CLASS_UNIFORM_CONSTANT &&
              binary[pos + 3] == elem_type_id)
            old_elem_ptr_id = binary[pos + 1];
          pos += wc;
        }

        // two-pass rewrite: first collect access chain result IDs that index
        // into the sampler array, then rewrite types and loads precisely.

        // pass A: find access chain result IDs that use old_elem_ptr_id
        std::unordered_set<uint32_t> sampler_ac_results;
        if (old_elem_ptr_id) {
          pos = 5;
          while (pos < binary.size()) {
            uint32_t wc = spv_word_count(binary[pos]);
            uint32_t oc = spv_opcode(binary[pos]);
            if (oc == 65 && wc >= 4 && binary[pos + 1] == old_elem_ptr_id)
              sampler_ac_results.insert(binary[pos + 2]);
            pos += wc;
          }
        }

        // pass B: rewrite
        llvm::SmallVector<uint32_t> patched;
        patched.reserve(binary.size() + 8);
        for (uint32_t i = 0; i < 5 && i < binary.size(); ++i)
          patched.push_back(binary[i]);
        patched[3] = id_bound + 2;

        pos = 5;
        while (pos < binary.size()) {
          uint32_t wc = spv_word_count(binary[pos]);
          uint32_t oc = spv_opcode(binary[pos]);

          // before the runtime array def, emit OpTypeSampler + new pointer type
          if (oc == SPV_OP_TYPE_RUNTIME_ARRAY && wc >= 3 &&
              binary[pos + 1] == rta_type_id) {
            patched.push_back((2u << 16u) | SPV_OP_TYPE_SAMPLER);
            patched.push_back(sampler_type_id);
            patched.push_back((4u << 16u) | SPV_OP_TYPE_POINTER);
            patched.push_back(sampler_ptr_type_id);
            patched.push_back(SPV_STORAGE_CLASS_UNIFORM_CONSTANT);
            patched.push_back(sampler_type_id);
            patched.push_back(binary[pos]);
            patched.push_back(binary[pos + 1]);
            patched.push_back(sampler_type_id);
            pos += wc;
            continue;
          }

          // rewrite OpAccessChain into the sampler array
          if (oc == 65 && wc >= 4 && old_elem_ptr_id &&
              binary[pos + 1] == old_elem_ptr_id) {
            patched.push_back(binary[pos]);
            patched.push_back(sampler_ptr_type_id);
            for (uint32_t i = 2; i < wc && (pos + i) < binary.size(); ++i)
              patched.push_back(binary[pos + i]);
            pos += wc;
            continue;
          }

          // rewrite OpLoad only when the pointer operand is a sampler access chain
          if (oc == 61 && wc >= 4 &&
              sampler_ac_results.count(binary[pos + 3])) {
            patched.push_back(binary[pos]);
            patched.push_back(sampler_type_id);
            for (uint32_t i = 2; i < wc && (pos + i) < binary.size(); ++i)
              patched.push_back(binary[pos + i]);
            pos += wc;
            continue;
          }

          for (uint32_t i = 0; i < wc && (pos + i) < binary.size(); ++i)
            patched.push_back(binary[pos + i]);
          pos += wc;
        }
        binary = std::move(patched);
      }
    }
  }

  fixup_spirv_decorations(binary);

  if (has_combine) cloned_op->destroy();
  return true;
}

static void fixup_spirv_decorations(llvm::SmallVectorImpl<uint32_t> &binary) {
  // - Flat on all Input integer variables (required for fragment shaders)
  // - NonWritable on SSBO struct members (required when fragmentStoresAndAtomics
  //   is not enabled)
  uint32_t pos;
  {
    static constexpr uint32_t SPV_OP_DECORATE = 71;
    static constexpr uint32_t SPV_OP_MEMBER_DECORATE = 72;
    static constexpr uint32_t SPV_OP_VARIABLE = 59;
    static constexpr uint32_t SPV_OP_TYPE_POINTER = 32;
    static constexpr uint32_t SPV_OP_TYPE_STRUCT = 30;
    static constexpr uint32_t SPV_DECORATION_FLAT = 14;
    static constexpr uint32_t SPV_DECORATION_NON_WRITABLE = 24;
    static constexpr uint32_t SPV_SC_INPUT = 1;
    static constexpr uint32_t SPV_SC_STORAGE_BUFFER = 12;

    // check execution model: Flat is only valid on fragment shader inputs.
    // OpEntryPoint: opcode 15, word[1] = execution model (4 = Fragment)
    static constexpr uint32_t SPV_OP_ENTRY_POINT = 15;
    static constexpr uint32_t SPV_EXEC_MODEL_FRAGMENT = 4;
    bool is_fragment = false;
    pos = 5;
    while (pos < binary.size()) {
      uint32_t wc = spv_word_count(binary[pos]);
      uint32_t oc = spv_opcode(binary[pos]);
      if (oc == SPV_OP_ENTRY_POINT && wc >= 2 &&
          binary[pos + 1] == SPV_EXEC_MODEL_FRAGMENT)
        is_fragment = true;
      pos += wc;
    }

    // collect variable IDs by storage class, and struct type IDs for SSBOs
    std::unordered_set<uint32_t> input_int_var_ids; // Input vars with integer pointee
    std::unordered_set<uint32_t> ssbo_struct_type_ids;

    // find integer type IDs, pointer types, and variables
    static constexpr uint32_t SPV_OP_TYPE_INT = 21;
    std::unordered_set<uint32_t> int_type_ids;
    std::unordered_map<uint32_t, uint32_t> ptr_sc;
    std::unordered_map<uint32_t, uint32_t> ptr_pointee;

    pos = 5;
    while (pos < binary.size()) {
      uint32_t wc = spv_word_count(binary[pos]);
      uint32_t oc = spv_opcode(binary[pos]);
      if (oc == SPV_OP_TYPE_INT && wc >= 2)
        int_type_ids.insert(binary[pos + 1]);
      if (oc == SPV_OP_TYPE_POINTER && wc >= 4) {
        ptr_sc[binary[pos + 1]] = binary[pos + 2];
        ptr_pointee[binary[pos + 1]] = binary[pos + 3];
      }
      if (oc == SPV_OP_VARIABLE && wc >= 4) {
        uint32_t type_id = binary[pos + 1];
        uint32_t var_id = binary[pos + 2];
        auto sc_it = ptr_sc.find(type_id);
        if (sc_it != ptr_sc.end()) {
          if (sc_it->second == SPV_SC_INPUT) {
            auto pt_it = ptr_pointee.find(type_id);
            if (pt_it != ptr_pointee.end() &&
                int_type_ids.count(pt_it->second))
              input_int_var_ids.insert(var_id);
          }
          if (sc_it->second == SPV_SC_STORAGE_BUFFER) {
            auto pt_it = ptr_pointee.find(type_id);
            if (pt_it != ptr_pointee.end())
              ssbo_struct_type_ids.insert(pt_it->second);
          }
        }
      }
      pos += wc;
    }

    // check which input vars already have Flat decoration
    std::unordered_set<uint32_t> already_flat;
    pos = 5;
    while (pos < binary.size()) {
      uint32_t wc = spv_word_count(binary[pos]);
      uint32_t oc = spv_opcode(binary[pos]);
      if (oc == SPV_OP_DECORATE && wc >= 3 &&
          binary[pos + 2] == SPV_DECORATION_FLAT)
        already_flat.insert(binary[pos + 1]);
      pos += wc;
    }

    // check which SSBO structs already have NonWritable on member 0
    std::unordered_set<uint32_t> already_nonwritable;
    pos = 5;
    while (pos < binary.size()) {
      uint32_t wc = spv_word_count(binary[pos]);
      uint32_t oc = spv_opcode(binary[pos]);
      if (oc == SPV_OP_MEMBER_DECORATE && wc >= 4 &&
          binary[pos + 2] == 0 && binary[pos + 3] == SPV_DECORATION_NON_WRITABLE)
        already_nonwritable.insert(binary[pos + 1]);
      pos += wc;
    }

    // append missing decorations at the end of the decoration section
    // (before the first function). find the insertion point.
    llvm::SmallVector<uint32_t> extra_decorations;
    if (is_fragment) {
      for (uint32_t vid : input_int_var_ids) {
        if (already_flat.count(vid)) continue;
        extra_decorations.push_back((3u << 16u) | SPV_OP_DECORATE);
        extra_decorations.push_back(vid);
        extra_decorations.push_back(SPV_DECORATION_FLAT);
      }
    }
    for (uint32_t sid : ssbo_struct_type_ids) {
      if (already_nonwritable.count(sid)) continue;
      // find how many members the struct has and mark all NonWritable
      pos = 5;
      while (pos < binary.size()) {
        uint32_t wc = spv_word_count(binary[pos]);
        uint32_t oc = spv_opcode(binary[pos]);
        if (oc == SPV_OP_TYPE_STRUCT && wc >= 2 && binary[pos + 1] == sid) {
          uint32_t member_count = wc - 2;
          for (uint32_t m = 0; m < member_count; m++) {
            extra_decorations.push_back((4u << 16u) | SPV_OP_MEMBER_DECORATE);
            extra_decorations.push_back(sid);
            extra_decorations.push_back(m);
            extra_decorations.push_back(SPV_DECORATION_NON_WRITABLE);
          }
          break;
        }
        pos += wc;
      }
    }

    if (!extra_decorations.empty()) {
      // SPIR-V layout: decorations must come before type declarations.
      // insert after the last existing OpDecorate/OpMemberDecorate/OpGroupDecorate,
      // before the first OpType* instruction.
      llvm::SmallVector<uint32_t> final_binary;
      final_binary.reserve(binary.size() + extra_decorations.size());
      for (uint32_t i = 0; i < 5 && i < binary.size(); i++)
        final_binary.push_back(binary[i]);
      pos = 5;
      bool inserted = false;
      while (pos < binary.size()) {
        uint32_t wc = spv_word_count(binary[pos]);
        uint32_t oc = spv_opcode(binary[pos]);
        // type instructions start at opcode 19 (OpTypeVoid) through 39
        if (!inserted && oc >= 19 && oc <= 39) {
          for (auto w : extra_decorations) final_binary.push_back(w);
          inserted = true;
        }
        for (uint32_t i = 0; i < wc && (pos + i) < binary.size(); i++)
          final_binary.push_back(binary[pos + i]);
        pos += wc;
      }
      binary = std::move(final_binary);
    }
  }
}

} // namespace um::shader
