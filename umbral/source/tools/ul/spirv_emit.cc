#include "spirv_emit.h"
// NodeKind and payload structs come via shader_link.h → compiler/frontend/ast.h
#include "compiler/frontend/lexer.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

extern "C" {
void LLVMInitializeSPIRVTargetInfo(void);
void LLVMInitializeSPIRVTarget(void);
void LLVMInitializeSPIRVTargetMC(void);
void LLVMInitializeSPIRVAsmPrinter(void);
}

void spirv_init_target(void) {
  LLVMInitializeSPIRVTargetInfo();
  LLVMInitializeSPIRVTarget();
  LLVMInitializeSPIRVTargetMC();
  LLVMInitializeSPIRVAsmPrinter();
}

// SPIR-V decoration ordinals (Unified Specification §3.20 Decoration)
static constexpr uint32_t SPV_DECO_BUILTIN        = 11;
static constexpr uint32_t SPV_DECO_LOCATION        = 30;
static constexpr uint32_t SPV_DECO_BINDING         = 33;
static constexpr uint32_t SPV_DECO_DESCRIPTOR_SET  = 34;

// SPIR-V BuiltIn variable values (§3.21 BuiltIn)
static constexpr uint32_t SPV_BUILTIN_POSITION    = 0;
static constexpr uint32_t SPV_BUILTIN_DRAW_INDEX  = 4418;

// LLVM SPIR-V backend address space encoding
static constexpr unsigned SPIRV_AS_UNIFORM_CONSTANT = 0;  // textures, samplers
static constexpr unsigned SPIRV_AS_INPUT            = 1;  // stage inputs
static constexpr unsigned SPIRV_AS_OUTPUT           = 3;  // stage outputs
static constexpr unsigned SPIRV_AS_STORAGE_BUFFER   = 12; // SSBOs

// OpTypeImage integer parameter layout: {Dim, Depth, Arrayed, MS, Sampled, ImageFormat}
// values below describe a plain 2D sampled color image (§3.11 Image Operands)
static constexpr uint32_t SPV_IMAGE_DIM_2D          = 1;
static constexpr uint32_t SPV_IMAGE_DEPTH_NONE       = 0;
static constexpr uint32_t SPV_IMAGE_NOT_ARRAYED      = 0;
static constexpr uint32_t SPV_IMAGE_SINGLE_SAMPLE    = 0;
static constexpr uint32_t SPV_IMAGE_SAMPLED          = 1;
static constexpr uint32_t SPV_IMAGE_FORMAT_UNKNOWN   = 0;

// return the SymId of the "self" parameter in body.sym_names; 0 if absent.
static uint32_t find_self_sym_id(const StageBody &body) {
  for (const auto &[sid, name] : body.sym_names)
    if (name == "self") return sid;
  return 0;
}

// return the index into sc.shaders whose name matches stage.shader_type_name.
static size_t find_shader_idx(const Sidecar &sc, const StageInfo &stage) {
  for (size_t s = 0; s < sc.shaders.size(); ++s)
    if (sc.shaders[s].name == stage.shader_type_name) return s;
  return 0;
}

// map a PodField type_name string to the corresponding
// llvm::Type*. returns nullptr for unrecognized names.
static llvm::Type *llvm_type_for(llvm::LLVMContext &ctx,
                                 const std::string &type_name) {
  auto *f32 = llvm::Type::getFloatTy(ctx);
  if (type_name == "f32") return f32;
  if (type_name == "f64") return llvm::Type::getDoubleTy(ctx);
  if (type_name == "i32") return llvm::Type::getInt32Ty(ctx);
  if (type_name == "u32") return llvm::Type::getInt32Ty(ctx);
  if (type_name == "i64") return llvm::Type::getInt64Ty(ctx);
  if (type_name == "u64") return llvm::Type::getInt64Ty(ctx);
  if (type_name == "bool") return llvm::Type::getInt1Ty(ctx);
  if (type_name == "vec2") return llvm::FixedVectorType::get(f32, 2);
  if (type_name == "vec3") return llvm::FixedVectorType::get(f32, 3);
  if (type_name == "vec4") return llvm::FixedVectorType::get(f32, 4);
  // mat4 as [4 x <4 x float>] — column-major, matches SPIR-V OpTypeMatrix
  if (type_name == "mat4")
    return llvm::ArrayType::get(llvm::FixedVectorType::get(f32, 4), 4);
  return nullptr;
}

// build an anonymous struct LLVM type for a @shader_pod.
// returns nullptr if any field type is unrecognized.
static llvm::Type *llvm_pod_type(llvm::LLVMContext &ctx, const PodType &pod) {
  std::vector<llvm::Type *> ftys;
  ftys.reserve(pod.fields.size());
  for (const auto &f : pod.fields) {
    llvm::Type *ft = llvm_type_for(ctx, f.type_name);
    if (!ft) return nullptr;
    ftys.push_back(ft);
  }
  return llvm::StructType::get(ctx, ftys);
}

// LLVM struct type mirroring draw_packet_t (runtime/gfx/gfx.h).
// field order is fixed ABI; must match the C struct exactly.
static llvm::StructType *draw_packet_struct_type(llvm::LLVMContext &ctx) {
  auto *i32 = llvm::Type::getInt32Ty(ctx);
  auto *i64 = llvm::Type::getInt64Ty(ctx);
  return llvm::StructType::get(
      ctx, {i64, i64, i64,           // pipeline_handle, vertex_buffer_handle, index_buffer_handle
            i32, i32, i32, i32, i32, // first_index, index_count, vertex_count, instance_count, first_instance
            i32, i32,                // draw_data_offset, material_data_offset
            i32, i32, i32});         // tex2d_index, sampler_index, flags
}

// field index within draw_packet_t by name; -1 if not found.
static int draw_packet_field_index(const std::string &name) {
  static const std::pair<const char *, int> kFields[] = {
      {"pipeline_handle", 0},      {"vertex_buffer_handle", 1},
      {"index_buffer_handle", 2},  {"first_index", 3},
      {"index_count", 4},          {"vertex_count", 5},
      {"instance_count", 6},       {"first_instance", 7},
      {"draw_data_offset", 8},     {"material_data_offset", 9},
      {"tex2d_index", 10},         {"sampler_index", 11},
      {"flags", 12},
  };
  for (const auto &[fname, idx] : kFields)
    if (name == fname) return idx;
  return -1;
}

// maps (shader_field_kind, pod_field_name) → GlobalVariable* in the LLVM
// module. built during declare_io_vars; used during body walk for Field node
// resolution.
//
// shader_field_kind values: 1=vs_in, 2=vs_out, 3=fs_in, 4=fs_out, 5=draw_data
struct IOVarKey {
  uint8_t shader_field_kind;
  std::string pod_field_name;
  bool operator==(const IOVarKey &o) const {
    return shader_field_kind == o.shader_field_kind &&
           pod_field_name == o.pod_field_name;
  }
};
struct IOVarKeyHash {
  size_t operator()(const IOVarKey &k) const {
    return std::hash<std::string>{}(k.pod_field_name) ^
           (k.shader_field_kind * 2654435761u);
  }
};
using IOVarMap =
    std::unordered_map<IOVarKey, llvm::GlobalVariable *, IOVarKeyHash>;

// attach a !spirv.Decorations MDNode to gvar.
//   each element of deco_words is a decoration operand (first = decoration id).
//   the backend lowers these to OpDecorate instructions.
static void spv_decoration(llvm::Module &mod, llvm::GlobalVariable *gvar,
                           std::initializer_list<uint32_t> deco_words) {
  llvm::SmallVector<llvm::Metadata *, 4> members;
  auto *i32 = llvm::Type::getInt32Ty(mod.getContext());
  for (uint32_t w : deco_words)
    members.push_back(
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, w)));
  auto *inner = llvm::MDNode::get(mod.getContext(), members);
  auto *outer = llvm::MDNode::get(mod.getContext(), {inner});
  gvar->setMetadata("spirv.Decorations", outer);
}

// emit one GlobalVariable per IO pod field.
// the LLVM SPIR-V backend maps GlobalVariables in address space 1 (Input) and
// 3 (Output) to OpVariables with Input/Output storage class.
// Location decorations and Builtin decorations are expressed as
// !spirv.Decorations metadata; the backend emits corresponding OpDecorate.
// the OpEntryPoint interface list is built automatically from all
// Input/Output GlobalVariables in the module — nothing extra is needed here.
static void declare_io_vars(llvm::Module &mod, const Sidecar &sc,
                            size_t shader_idx, uint8_t stage_kind,
                            IOVarMap &io_var_map) {
  for (const auto &annot : sc.shaders[shader_idx].annots) {
    const PodType *pod_type = find_ptr(sc.pods, [&](const PodType &p) {
      return p.name == annot.pod_type_name;
    });
    if (!pod_type) continue;

    // vs_in, fs_in → Input; vs_out, fs_out → Output
    unsigned addr_space = 0;
    switch (annot.shader_field_kind) {
    case SFKIND_VS_IN:
    case SFKIND_FS_IN:  addr_space = SPIRV_AS_INPUT;  break;
    case SFKIND_VS_OUT:
    case SFKIND_FS_OUT: addr_space = SPIRV_AS_OUTPUT; break;
    default: continue;
    }

    for (const auto &field : pod_type->fields) {
      if (field.io_kind == IOKind::None) continue;
      llvm::Type *ty = llvm_type_for(mod.getContext(), field.type_name);
      if (!ty) continue;
      auto *gvar = new llvm::GlobalVariable(
          mod, ty, /*isConst*/ false, llvm::GlobalValue::ExternalLinkage,
          nullptr, field.name, nullptr, llvm::GlobalVariable::NotThreadLocal,
          addr_space);
      if (field.io_kind == IOKind::Location) {
        spv_decoration(mod, gvar, {SPV_DECO_LOCATION, field.location_index});
      } else if (field.io_kind == IOKind::Position) {
        spv_decoration(mod, gvar, {SPV_DECO_BUILTIN, SPV_BUILTIN_POSITION});
      }
      io_var_map[{annot.shader_field_kind, field.name}] = gvar;
    }
  }
}

// wrappers for the three SPIR-V opaque types needed for texture sampling.
// spirv.Image params: elem_ty, {Dim=1(2D), Depth=0, Arrayed=0, MS=0,
//   Sampled=1, Format=0(Unknown)}.
static llvm::TargetExtType *spirv_image2d_ty(llvm::LLVMContext &ctx) {
  return llvm::TargetExtType::get(
      ctx, "spirv.Image", {llvm::Type::getFloatTy(ctx)},
      {SPV_IMAGE_DIM_2D, SPV_IMAGE_DEPTH_NONE, SPV_IMAGE_NOT_ARRAYED,
       SPV_IMAGE_SINGLE_SAMPLE, SPV_IMAGE_SAMPLED, SPV_IMAGE_FORMAT_UNKNOWN});
}
static llvm::TargetExtType *spirv_sampler_ty(llvm::LLVMContext &ctx) {
  return llvm::TargetExtType::get(ctx, "spirv.Sampler", {}, {});
}
static llvm::TargetExtType *spirv_sampled_image_ty(llvm::LLVMContext &ctx) {
  return llvm::TargetExtType::get(ctx, "spirv.SampledImage",
                                  {spirv_image2d_ty(ctx)}, {});
}

// emit one GlobalVariable per bindless resource
// slot used by this stage. only emits globals for resource kinds that appear
//  in the body (avoids unused-binding validation errors in Vulkan).
//
// set=0 binding=0 — texture2d RuntimeArray  (ShaderTexture2d nodes present)
// set=0 binding=1 — sampler RuntimeArray    (ShaderSampler nodes present)
// set=0 binding=2 — frame arena SSBO        (ShaderFrameRead nodes present)
// set=0 binding=3 — draw_packet SSBO        (ShaderDrawPacket nodes present)
//
// texture/sampler: address space 0 (UniformConstant).
// SSBOs: address space 12 (StorageBuffer).
// DescriptorSet decoration id = 33; Binding decoration id = 34.
static void declare_descriptor_bindings(llvm::Module &mod,
                                        const StageBody &body) {
  bool need_tex = false, need_samp = false, need_frame = false,
       need_pkt = false;
  for (const auto &nd : body.nodes) {
    switch (static_cast<NodeKind>(nd.kind)) {
    case NodeKind::ShaderTexture2d: need_tex = true; break;
    case NodeKind::ShaderSampler: need_samp = true; break;
    case NodeKind::ShaderFrameRead: need_frame = true; break;
    case NodeKind::ShaderDrawPacket: need_pkt = true; break;
    default: break;
    }
  }

  auto *i8 = llvm::Type::getInt8Ty(mod.getContext());
  auto *tex_arr = llvm::ArrayType::get(spirv_image2d_ty(mod.getContext()), 0);
  auto *samp_arr = llvm::ArrayType::get(spirv_sampler_ty(mod.getContext()), 0);
  // frame_arena: byte SSBO for arbitrary byte-addressed reads via @frame_read<T>
  auto *frame_arr = llvm::ArrayType::get(i8, 0);
  // draw_packets: typed struct SSBO; fields accessed via @draw_packet(id).field
  auto *pkt_arr = llvm::ArrayType::get(draw_packet_struct_type(mod.getContext()), 0);

  auto emit_binding = [&](const char *name, llvm::Type *arr_ty,
                          unsigned binding, unsigned addr_space) {
    auto *gvar = new llvm::GlobalVariable(
        mod, arr_ty, /*isConst*/ false, llvm::GlobalValue::ExternalLinkage,
        nullptr, name, nullptr, llvm::GlobalVariable::NotThreadLocal,
        addr_space);
    auto *i32 = llvm::Type::getInt32Ty(mod.getContext());
    auto *ds_inner = llvm::MDNode::get(
        mod.getContext(),
        {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, SPV_DECO_DESCRIPTOR_SET)),
         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, 0))});
    auto *bd_inner = llvm::MDNode::get(
        mod.getContext(),
        {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, SPV_DECO_BINDING)),
         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, binding))});
    auto *outer = llvm::MDNode::get(mod.getContext(), {ds_inner, bd_inner});
    gvar->setMetadata("spirv.Decorations", outer);
    return gvar;
  };

  if (need_tex) emit_binding("textures_2d", tex_arr, 0, SPIRV_AS_UNIFORM_CONSTANT);
  if (need_samp) emit_binding("samplers", samp_arr, 1, SPIRV_AS_UNIFORM_CONSTANT);
  if (need_frame) emit_binding("frame_arena", frame_arr, 2, SPIRV_AS_STORAGE_BUFFER);
  if (need_pkt) emit_binding("draw_packets", pkt_arr, 3, SPIRV_AS_STORAGE_BUFFER);
}

// create void main() and mark it as a shader entry point
// via the "hlsl.shader" function attribute. the SPIR-V backend derives the
// OpEntryPoint execution model from this attribute (vertex → Vertex,
// pixel → Fragment). for Fragment, OriginUpperLeft is injected by the
// backend.
static void declare_entry_point(llvm::Module &mod, llvm::Function **out_fn,
                                uint8_t stage_kind,
                                const IOVarMap &io_var_map) {
  auto *ret_type = llvm::Type::getVoidTy(mod.getContext());
  auto *fn_type = llvm::FunctionType::get(ret_type, {}, /*isVarArg*/ false);
  auto *fn = llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage,
                                    "main", mod);
  // stage_kind: 0 = vertex → "vertex"; 1 = fragment → "pixel" (HLSL convention)
  fn->addFnAttr("hlsl.shader", stage_kind == 0 ? "vertex" : "pixel");
  if (out_fn) *out_fn = fn;
}

struct EmitCtx {
  llvm::LLVMContext &llvm_ctx;
  llvm::Module &mod;
  llvm::IRBuilder<> b;
  llvm::Function *fn;
  const StageBody &body;
  const IOVarMap &io_var_map;
  const Sidecar &sc;
  size_t shader_idx;
  uint32_t self_sym_id; // SymId of the "self" parameter

  // SymId → alloca pointer for mutable local vars (VarStmt)
  std::unordered_map<uint32_t, llvm::Value *> var_allocas;
  // SymId → SSA value for immutable bindings and function params (ConstStmt,
  // args)
  std::unordered_map<uint32_t, llvm::Value *> sym_values;
  // method_name → LLVM function for @shader_fn helpers callable from this stage
  std::unordered_map<std::string, llvm::Function *> shader_fn_map;

  EmitCtx(llvm::LLVMContext &ctx, llvm::Module &mod_, llvm::Function *fn_,
          const StageBody &body_, const IOVarMap &ivm, const Sidecar &sc_,
          size_t shader_idx_, uint32_t self_sym_id_)
      : llvm_ctx(ctx), mod(mod_), b(ctx), fn(fn_), body(body_), io_var_map(ivm),
        sc(sc_), shader_idx(shader_idx_), self_sym_id(self_sym_id_) {
    auto *entry_bb = llvm::BasicBlock::Create(ctx, "entry", fn);
    b.SetInsertPoint(entry_bb);
  }

  // look up a SymId in the stage's sym_names table.
  // returns "" if not found (shouldn't happen for well-formed sidecar data).
  const std::string &sym_name(uint32_t sym_id) const {
    auto it = body.sym_names.find(sym_id);
    static const std::string empty;
    return it != body.sym_names.end() ? it->second : empty;
  }

  // lower node_id to LLVM IR.  returns the SSA Value* result, or
  // nullptr for statement nodes and unimplemented cases.
  // is_lhs = true when the caller needs a pointer (lhs of an AssignStmt or
  // GEP base); the callee returns the alloca/global pointer without a load.
  llvm::Value *emit(uint32_t node_id, bool is_lhs = false);
};

llvm::Value *EmitCtx::emit(uint32_t node_id, bool is_lhs) {
  if (node_id == 0 || node_id >= body.nodes.size()) return nullptr;
  const BodyNode &nd = body.nodes[node_id];
  auto nk = static_cast<NodeKind>(nd.kind);
  auto *i32_ty = llvm::Type::getInt32Ty(llvm_ctx);

  switch (nk) {

  case NodeKind::IntLit:
    return llvm::ConstantInt::get(i32_ty, nd.a, /*isSigned*/ true);

  case NodeKind::FloatLit:
    return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_ctx),
                                 body.float_lits[nd.a]);

  case NodeKind::BoolLit:
    return nd.a ? llvm::ConstantInt::getTrue(llvm_ctx)
                : llvm::ConstantInt::getFalse(llvm_ctx);

  case NodeKind::Ident: {
    uint32_t sym_id = nd.a;
    auto alloca_it = var_allocas.find(sym_id);
    if (alloca_it != var_allocas.end()) {
      if (is_lhs) return alloca_it->second;
      auto *alloca = llvm::cast<llvm::AllocaInst>(alloca_it->second);
      return b.CreateLoad(alloca->getAllocatedType(), alloca_it->second,
                          sym_name(sym_id));
    }
    auto ssa_it = sym_values.find(sym_id);
    if (ssa_it != sym_values.end()) return ssa_it->second;
    return nullptr;
  }

  case NodeKind::Field: {
    uint32_t base_nid = nd.a;
    uint32_t field_sym = nd.b;
    const std::string &fname = sym_name(field_sym);

    // self.annot_field.pod_field — IO GlobalVariable access
    // detects the 2-level pattern: Field(Field(Ident(self), annot_sym),
    // pod_sym)
    if (base_nid != 0 && base_nid < body.nodes.size()) {
      const BodyNode &base_nd = body.nodes[base_nid];
      if (static_cast<NodeKind>(base_nd.kind) == NodeKind::Field) {
        uint32_t base_base_nid = base_nd.a;
        if (base_base_nid != 0 && base_base_nid < body.nodes.size()) {
          const BodyNode &base_base_nd = body.nodes[base_base_nid];
          if (static_cast<NodeKind>(base_base_nd.kind) == NodeKind::Ident &&
              base_base_nd.a == self_sym_id) {
            const std::string &annot_name = sym_name(base_nd.b);
            uint8_t skind = 0;
            for (const auto &annot : sc.shaders[shader_idx].annots)
              if (annot.field_name == annot_name) {
                skind = annot.shader_field_kind;
                break;
              }
            auto it = io_var_map.find({skind, fname});
            if (it == io_var_map.end()) {
              fprintf(stderr, "spirv_emit: IO var not found: %s.%s\n",
                      annot_name.c_str(), fname.c_str());
              return nullptr;
            }
            llvm::GlobalVariable *gvar = it->second;
            if (is_lhs) return gvar;
            return b.CreateLoad(gvar->getValueType(), gvar, fname);
          }
        }
      }
    }

    // general struct field: emit base as a value and extract the named field.
    // handles draw_packet_t (hardcoded layout) and @shader_pod types from the sidecar.
    {
      llvm::Value *base_val = emit(base_nid);
      if (base_val) {
        if (auto *st = llvm::dyn_cast<llvm::StructType>(base_val->getType())) {
          // draw_packet_t
          if (st == draw_packet_struct_type(llvm_ctx)) {
            int idx = draw_packet_field_index(fname);
            if (idx >= 0)
              return b.CreateExtractValue(base_val, {(unsigned)idx}, fname);
          }
          // @shader_pod types listed in the sidecar
          for (const auto &pod : sc.pods) {
            llvm::Type *pod_ty = llvm_pod_type(llvm_ctx, pod);
            if (pod_ty != st) continue;
            for (unsigned fi = 0; fi < pod.fields.size(); ++fi) {
              if (pod.fields[fi].name == fname)
                return b.CreateExtractValue(base_val, {fi}, fname);
            }
          }
        }
      }
    }
    return nullptr;
  }

  case NodeKind::Index: {
    // a=base, b=index — GEP into an array alloca or GlobalVariable
    llvm::Value *base_ptr = emit(nd.a, /*is_lhs=*/true);
    llvm::Value *idx = emit(nd.b);
    if (!base_ptr || !idx) return nullptr;
    // get array type from allocation source (opaque-pointer safe)
    llvm::Type *alloc_ty = nullptr;
    if (auto *ai = llvm::dyn_cast<llvm::AllocaInst>(base_ptr))
      alloc_ty = ai->getAllocatedType();
    else if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(base_ptr))
      alloc_ty = gv->getValueType();
    if (!alloc_ty) return nullptr;
    auto *arr_ty = llvm::dyn_cast<llvm::ArrayType>(alloc_ty);
    if (!arr_ty) return nullptr;
    auto *elem_ptr =
        b.CreateGEP(arr_ty, base_ptr, {llvm::ConstantInt::get(i32_ty, 0), idx});
    if (is_lhs) return elem_ptr;
    return b.CreateLoad(arr_ty->getElementType(), elem_ptr);
  }

  case NodeKind::Deref: {
    // pointer element type is not recoverable without a full type map in
    // opaque-pointer mode; Deref is not used in shader bodies in practice.
    (void)nd;
    return nullptr;
  }

  case NodeKind::Unary: {
    auto op = static_cast<TokenKind>(nd.a);
    llvm::Value *child = emit(nd.b);
    if (!child) return nullptr;
    if (op == TokenKind::Minus) {
      return child->getType()->isFloatingPointTy() ? b.CreateFNeg(child)
                                                   : b.CreateNeg(child);
    }
    if (op == TokenKind::Bang) return b.CreateNot(child);
    return nullptr;
  }

  case NodeKind::Binary: {
    auto op = static_cast<TokenKind>(nd.a);
    llvm::Value *lhs = emit(nd.b);
    llvm::Value *rhs = emit(nd.c);
    if (!lhs || !rhs) return nullptr;
    bool is_float = lhs->getType()->isFloatingPointTy() ||
                    (lhs->getType()->isVectorTy() &&
                     llvm::cast<llvm::VectorType>(lhs->getType())
                         ->getElementType()
                         ->isFloatingPointTy());
    switch (op) {
    case TokenKind::Plus:
      return is_float ? b.CreateFAdd(lhs, rhs) : b.CreateAdd(lhs, rhs);
    case TokenKind::Minus:
      return is_float ? b.CreateFSub(lhs, rhs) : b.CreateSub(lhs, rhs);
    case TokenKind::Star:
      return is_float ? b.CreateFMul(lhs, rhs) : b.CreateMul(lhs, rhs);
    case TokenKind::Slash:
      return is_float ? b.CreateFDiv(lhs, rhs) : b.CreateSDiv(lhs, rhs);
    case TokenKind::EqualEqual:
      return is_float ? b.CreateFCmpOEQ(lhs, rhs) : b.CreateICmpEQ(lhs, rhs);
    case TokenKind::BangEqual:
      return is_float ? b.CreateFCmpONE(lhs, rhs) : b.CreateICmpNE(lhs, rhs);
    case TokenKind::Less:
      return is_float ? b.CreateFCmpOLT(lhs, rhs) : b.CreateICmpSLT(lhs, rhs);
    case TokenKind::LessEqual:
      return is_float ? b.CreateFCmpOLE(lhs, rhs) : b.CreateICmpSLE(lhs, rhs);
    case TokenKind::Greater:
      return is_float ? b.CreateFCmpOGT(lhs, rhs) : b.CreateICmpSGT(lhs, rhs);
    case TokenKind::GreaterEqual:
      return is_float ? b.CreateFCmpOGE(lhs, rhs) : b.CreateICmpSGE(lhs, rhs);
    case TokenKind::AmpAmp: return b.CreateAnd(lhs, rhs);
    case TokenKind::PipePipe: return b.CreateOr(lhs, rhs);
    default: return nullptr;
    }
  }

  case NodeKind::Call: {
    // in shader bodies, calls are expected to be positional struct construction
    // (vec2/vec3/vec4) or shader intrinsics already handled above.
    // resolve callee name from Ident node.
    uint32_t callee_nid = nd.a;
    std::string callee_name;
    if (callee_nid && callee_nid < body.nodes.size()) {
      const BodyNode &cn = body.nodes[callee_nid];
      if (static_cast<NodeKind>(cn.kind) == NodeKind::Ident)
        callee_name = sym_name(cn.a);
    }
    uint32_t args_start = nd.b;
    uint32_t args_count = nd.c;

    // vec2/vec3/vec4 positional struct construction → SPIR-V vector
    if (callee_name == "vec2" || callee_name == "vec3" ||
        callee_name == "vec4") {
      uint32_t n = (callee_name == "vec2")   ? 2u
                   : (callee_name == "vec3") ? 3u
                                             : 4u;
      auto *f32 = llvm::Type::getFloatTy(llvm_ctx);
      auto *vec_ty = llvm::FixedVectorType::get(f32, n);
      llvm::Value *vec = llvm::UndefValue::get(vec_ty);
      for (uint32_t k = 0; k < n && k < args_count; ++k) {
        llvm::Value *elem = emit(body.list[args_start + k]);
        if (!elem) continue;
        // coerce double → float if needed
        if (elem->getType()->isDoubleTy()) elem = b.CreateFPTrunc(elem, f32);
        vec = b.CreateInsertElement(vec, elem, k);
      }
      return vec;
    }
    // method calls: self.shader_fn_name(args) → call the helper function
    if (callee_nid && callee_nid < body.nodes.size()) {
      const BodyNode &callee_nd = body.nodes[callee_nid];
      if (static_cast<NodeKind>(callee_nd.kind) == NodeKind::Field) {
        const BodyNode &base_nd = body.nodes[callee_nd.a];
        if (static_cast<NodeKind>(base_nd.kind) == NodeKind::Ident &&
            base_nd.a == self_sym_id) {
          const std::string &method = sym_name(callee_nd.b);
          auto fn_it = shader_fn_map.find(method);
          if (fn_it != shader_fn_map.end()) {
            llvm::Function *callee_fn = fn_it->second;
            std::vector<llvm::Value *> arg_vals;
            for (uint32_t k = 0; k < args_count; ++k) {
              llvm::Value *av = emit(body.list[args_start + k]);
              if (av) arg_vals.push_back(av);
            }
            return b.CreateCall(callee_fn, arg_vals);
          }
        }
      }
    }
    return nullptr;
  }

  case NodeKind::ArrayLit: {
    const ArrayLitPayload &al = body.array_lits[nd.a];
    // determine element type from first element (if available)
    llvm::Value *result = nullptr;
    for (uint32_t k = 0; k < al.values_count; ++k) {
      llvm::Value *elem = emit(body.list[al.values_start + k]);
      if (!elem) continue;
      if (!result) {
        auto *arr_ty = llvm::ArrayType::get(elem->getType(), al.values_count);
        result = llvm::UndefValue::get(arr_ty);
      }
      result = b.CreateInsertValue(result, elem, {k});
    }
    return result;
  }

  case NodeKind::Block: {
    uint32_t stmt_start = nd.b;
    uint32_t stmt_count = nd.c;
    for (uint32_t k = 0; k < stmt_count; ++k) emit(body.list[stmt_start + k]);
    return nullptr;
  }

  case NodeKind::ConstStmt: {
    // a = SymId, c = init ExprId; bind SSA value directly (no alloca)
    uint32_t sym_id = nd.a;
    if (nd.c != 0) {
      llvm::Value *v = emit(nd.c);
      if (v) sym_values[sym_id] = v;
    }
    return nullptr;
  }

  case NodeKind::VarStmt: {
    // a = SymId, c = init ExprId; emit alloca at function entry then store init
    uint32_t sym_id = nd.a;
    llvm::Value *init_val = (nd.c != 0) ? emit(nd.c) : nullptr;
    if (!init_val) return nullptr;
    // insert alloca before any other instructions in the entry block so
    // the SPIR-V backend sees it as OpVariable Function
    auto saved_ip = b.saveIP();
    b.SetInsertPointPastAllocas(fn);
    auto *alloca =
        b.CreateAlloca(init_val->getType(), nullptr, sym_name(sym_id));
    b.restoreIP(saved_ip);
    b.CreateStore(init_val, alloca);
    var_allocas[sym_id] = alloca;
    return nullptr;
  }

  case NodeKind::AssignStmt: {
    // a = lhs place, b = rhs expr, c = op TokenKind
    auto op = static_cast<TokenKind>(nd.c);
    llvm::Value *rhs = emit(nd.b);
    llvm::Value *lhs_ptr = emit(nd.a, /*is_lhs=*/true);
    if (!rhs || !lhs_ptr) return nullptr;
    if (op == TokenKind::Equal) {
      b.CreateStore(rhs, lhs_ptr);
      return nullptr;
    }
    // compound assignment: load current value, apply op, store back
    llvm::Value *cur = b.CreateLoad(rhs->getType(), lhs_ptr);
    bool is_float = rhs->getType()->isFloatingPointTy();
    llvm::Value *result = nullptr;
    switch (op) {
    case TokenKind::PlusEqual:
      result = is_float ? b.CreateFAdd(cur, rhs) : b.CreateAdd(cur, rhs);
      break;
    case TokenKind::MinusEqual:
      result = is_float ? b.CreateFSub(cur, rhs) : b.CreateSub(cur, rhs);
      break;
    case TokenKind::StarEqual:
      result = is_float ? b.CreateFMul(cur, rhs) : b.CreateMul(cur, rhs);
      break;
    case TokenKind::SlashEqual:
      result = is_float ? b.CreateFDiv(cur, rhs) : b.CreateSDiv(cur, rhs);
      break;
    default: break;
    }
    if (result) b.CreateStore(result, lhs_ptr);
    return nullptr;
  }

  case NodeKind::ExprStmt: emit(nd.a); return nullptr;

  case NodeKind::ReturnStmt:
    if (nd.a != 0) {
      llvm::Value *v = emit(nd.a);
      if (v) b.CreateRet(v);
    } else {
      b.CreateRetVoid();
    }
    return nullptr;

  case NodeKind::IfStmt: {
    llvm::Value *cond = emit(nd.a);
    if (!cond) return nullptr;
    auto *then_bb = llvm::BasicBlock::Create(llvm_ctx, "then", fn);
    auto *merge_bb = llvm::BasicBlock::Create(llvm_ctx, "merge", fn);
    auto *else_bb =
        (nd.c != 0) ? llvm::BasicBlock::Create(llvm_ctx, "else", fn) : merge_bb;
    b.CreateCondBr(cond, then_bb, else_bb);

    b.SetInsertPoint(then_bb);
    emit(nd.b);
    if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(merge_bb);

    if (nd.c != 0) {
      b.SetInsertPoint(else_bb);
      emit(nd.c);
      if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(merge_bb);
    }

    b.SetInsertPoint(merge_bb);
    return nullptr;
  }

  case NodeKind::ForStmt: {
    // ForPayload: init, cond (0 = unconditional), step, body
    const ForPayload &fp = body.fors[nd.a];
    auto *header_bb = llvm::BasicBlock::Create(llvm_ctx, "loop_header", fn);
    auto *body_bb = llvm::BasicBlock::Create(llvm_ctx, "loop_body", fn);
    auto *continue_bb = llvm::BasicBlock::Create(llvm_ctx, "loop_continue", fn);
    auto *merge_bb = llvm::BasicBlock::Create(llvm_ctx, "loop_merge", fn);

    if (fp.init) emit(fp.init);
    b.CreateBr(header_bb);

    b.SetInsertPoint(header_bb);
    auto *cond_val =
        fp.cond ? emit(fp.cond) : llvm::ConstantInt::getTrue(llvm_ctx);
    if (!cond_val) cond_val = llvm::ConstantInt::getTrue(llvm_ctx);
    b.CreateCondBr(cond_val, body_bb, merge_bb);

    b.SetInsertPoint(body_bb);
    emit(fp.body);
    if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(continue_bb);

    b.SetInsertPoint(continue_bb);
    if (fp.step) emit(fp.step);
    b.CreateBr(header_bb);

    b.SetInsertPoint(merge_bb);
    return nullptr;
  }

  case NodeKind::ForRange: {
    // a = loop-var SymId, b = IterCreate source NodeId, c = body Block NodeId
    // lowered to: i:=0; i<len; i+=1; body = elem at index i
    uint32_t loop_var_sym = nd.a;
    llvm::Value *src = emit(nd.b); // array value or slice struct
    if (!src) return nullptr;

    auto *src_ty = src->getType();
    llvm::Value *len_val = nullptr;
    llvm::Type *elem_ty = nullptr;

    if (auto *arr_ty = llvm::dyn_cast<llvm::ArrayType>(src_ty)) {
      len_val = llvm::ConstantInt::get(i32_ty, arr_ty->getNumElements());
      elem_ty = arr_ty->getElementType();
    } else {
      // slice: { ptr, len } — extract len (index 1)
      len_val = b.CreateExtractValue(src, {1});
      elem_ty =
          llvm::Type::getInt8Ty(llvm_ctx); // placeholder; refined by usage
    }

    // alloca for the loop counter and the loop variable
    auto saved_ip = b.saveIP();
    b.SetInsertPointPastAllocas(fn);
    auto *i_alloca = b.CreateAlloca(i32_ty, nullptr, "range_i");
    llvm::AllocaInst *var_alloca = nullptr;
    if (elem_ty) {
      var_alloca = b.CreateAlloca(elem_ty, nullptr, sym_name(loop_var_sym));
      var_allocas[loop_var_sym] = var_alloca;
    }
    b.restoreIP(saved_ip);

    b.CreateStore(llvm::ConstantInt::get(i32_ty, 0), i_alloca);

    // also store src in a temporary alloca so we can GEP into it
    auto src_saved_ip = b.saveIP();
    b.SetInsertPointPastAllocas(fn);
    auto *src_alloca = b.CreateAlloca(src_ty, nullptr, "range_src");
    b.restoreIP(src_saved_ip);
    b.CreateStore(src, src_alloca);

    auto *header_bb = llvm::BasicBlock::Create(llvm_ctx, "range_header", fn);
    auto *body_bb = llvm::BasicBlock::Create(llvm_ctx, "range_body", fn);
    auto *merge_bb = llvm::BasicBlock::Create(llvm_ctx, "range_merge", fn);

    b.CreateBr(header_bb);

    b.SetInsertPoint(header_bb);
    auto *i_val = b.CreateLoad(i32_ty, i_alloca, "i");
    auto *cond = b.CreateICmpSLT(i_val, len_val);
    b.CreateCondBr(cond, body_bb, merge_bb);

    b.SetInsertPoint(body_bb);
    // load elem[i] into loop variable
    if (var_alloca && llvm::isa<llvm::ArrayType>(src_ty)) {
      auto *elem_ptr = b.CreateGEP(src_ty, src_alloca,
                                   {llvm::ConstantInt::get(i32_ty, 0), i_val});
      auto *elem_val = b.CreateLoad(elem_ty, elem_ptr);
      b.CreateStore(elem_val, var_alloca);
    }
    emit(nd.c); // body block
    // increment i
    auto *i_next = b.CreateAdd(i_val, llvm::ConstantInt::get(i32_ty, 1));
    b.CreateStore(i_next, i_alloca);
    if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(header_bb);

    b.SetInsertPoint(merge_bb);
    return nullptr;
  }

    // these create GlobalVariable loads; the bindings must have been declared
    // by declare_descriptor_bindings before body emission.

  case NodeKind::ShaderDrawId: {
    // load gl_DrawID — Input GlobalVariable decorated with BuiltIn DrawIndex.
    // requires Capability DrawParameters.
    auto *gvar = mod.getGlobalVariable("draw_id");
    if (!gvar) {
      gvar = new llvm::GlobalVariable(
          mod, i32_ty, false, llvm::GlobalValue::ExternalLinkage, nullptr,
          "draw_id", nullptr, llvm::GlobalVariable::NotThreadLocal,
          SPIRV_AS_INPUT);
      spv_decoration(mod, gvar, {SPV_DECO_BUILTIN, SPV_BUILTIN_DRAW_INDEX});
    }
    return b.CreateLoad(i32_ty, gvar, "draw_id");
  }

  case NodeKind::ShaderTexture2d: {
    // @texture2d(idx) — load spirv.Image element from the textures_2d array
    auto *gvar = mod.getGlobalVariable("textures_2d");
    if (!gvar) return nullptr;
    llvm::Value *idx = emit(nd.a);
    if (!idx) return nullptr;
    auto *arr_ty = llvm::cast<llvm::ArrayType>(gvar->getValueType());
    auto *elem_ptr =
        b.CreateGEP(arr_ty, gvar, {llvm::ConstantInt::get(i32_ty, 0), idx});
    return b.CreateLoad(arr_ty->getElementType(), elem_ptr, "tex");
  }

  case NodeKind::ShaderSampler: {
    // @sampler(idx) — load spirv.Sampler element from the samplers array
    auto *gvar = mod.getGlobalVariable("samplers");
    if (!gvar) return nullptr;
    llvm::Value *idx = emit(nd.a);
    if (!idx) return nullptr;
    auto *arr_ty = llvm::cast<llvm::ArrayType>(gvar->getValueType());
    auto *elem_ptr =
        b.CreateGEP(arr_ty, gvar, {llvm::ConstantInt::get(i32_ty, 0), idx});
    return b.CreateLoad(arr_ty->getElementType(), elem_ptr, "samp");
  }

  case NodeKind::ShaderDrawPacket: {
    // @draw_packet(id) — load a draw_packet_t struct from the draw_packets SSBO.
    // result supports field access via Field emitter (ExtractValue by name).
    auto *gvar = mod.getGlobalVariable("draw_packets");
    if (!gvar) return nullptr;
    llvm::Value *id = emit(nd.a);
    if (!id) return nullptr;
    auto *arr_ty = llvm::cast<llvm::ArrayType>(gvar->getValueType());
    auto *elem_ptr = b.CreateGEP(arr_ty, gvar,
                                 {llvm::ConstantInt::get(i32_ty, 0), id}, "pkt_ptr");
    return b.CreateLoad(arr_ty->getElementType(), elem_ptr, "pkt");
  }

  case NodeKind::ShaderFrameRead: {
    // @frame_read<T>(offset) — byte-addressed read from frame_arena SSBO.
    // nd.b holds the SymId for T (sidecar translates TypeId → SymId at emit time).
    auto *gvar = mod.getGlobalVariable("frame_arena");
    if (!gvar) return nullptr;
    llvm::Value *offset = emit(nd.a);
    if (!offset) return nullptr;
    const std::string &type_name = sym_name(nd.b);
    llvm::Type *target_ty = llvm_type_for(llvm_ctx, type_name);
    if (!target_ty) {
      for (const auto &pod : sc.pods)
        if (pod.name == type_name) { target_ty = llvm_pod_type(llvm_ctx, pod); break; }
    }
    if (!target_ty) return nullptr;
    auto *byte_ptr = b.CreateGEP(llvm::Type::getInt8Ty(llvm_ctx), gvar, offset, "frame_ptr");
    return b.CreateLoad(target_ty, byte_ptr, "frame_val");
  }

  case NodeKind::ShaderSample: {
    // @sample(tex, samp, uv) — combine into SampledImage then sample with
    // implicit LOD. maps to OpSampledImage + OpImageSampleImplicitLod in
    // SPIR-V.
    llvm::Value *tex = emit(nd.a), *samp = emit(nd.b), *uv = emit(nd.c);
    if (!tex || !samp || !uv) return nullptr;
    auto *f32 = llvm::Type::getFloatTy(llvm_ctx);
    auto *vec4_ty = llvm::FixedVectorType::get(f32, 4);
    auto *si_ty = spirv_sampled_image_ty(llvm_ctx);
    auto si_fn = mod.getOrInsertFunction(
        "llvm.spv.sampled.image",
        llvm::FunctionType::get(si_ty, {tex->getType(), samp->getType()},
                                false));
    auto *si = b.CreateCall(si_fn, {tex, samp}, "sampled_img");
    auto sample_fn = mod.getOrInsertFunction(
        "llvm.spv.image.sample.implicit.lod",
        llvm::FunctionType::get(vec4_ty, {si_ty, uv->getType()}, false));
    return b.CreateCall(sample_fn, {si, uv}, "sample");
  }

  default: return nullptr;
  }
}

// declare and emit all @shader_fn helpers for the given shader type into mod.
// populates shader_fn_map on each EmitCtx that will use the helpers.
// must be called before emitting the entry point body.
static void
declare_shader_fns(llvm::Module &mod, const Sidecar &sc, size_t shader_idx,
                   const IOVarMap &io_var_map,
                   std::unordered_map<std::string, llvm::Function *> &out_map) {
  const std::string &shader_type = sc.shaders[shader_idx].name;
  for (const ShaderFnInfo &sfi : sc.shader_fns) {
    if (sfi.shader_type_name != shader_type) continue;

    // build param type list from serialized params
    std::vector<llvm::Type *> param_types;
    for (const ShaderFnParam &p : sfi.params) {
      llvm::Type *pt = llvm_type_for(mod.getContext(), p.type_name);
      if (!pt) {
        fprintf(stderr,
                "spirv_emit: unknown param type '%s' in @shader_fn %s\n",
                p.type_name.c_str(), sfi.method_name.c_str());
        continue;
      }
      param_types.push_back(pt);
    }

    // return type: for now all @shader_fn helpers return void; callers use
    // the void return and rely on side-effects via globals or future support
    // for explicit return types (not yet in the sidecar format).
    auto *ret_ty = llvm::Type::getVoidTy(mod.getContext());
    auto *fn_ty = llvm::FunctionType::get(ret_ty, param_types, false);
    auto *fn = llvm::Function::Create(fn_ty, llvm::Function::InternalLinkage,
                                      sfi.method_name, mod);

    // emit the body
    uint32_t self_sym_id = find_self_sym_id(sfi.body);

    EmitCtx ectx(mod.getContext(), mod, fn, sfi.body, io_var_map, sc,
                 shader_idx, self_sym_id);

    // bind params to sym_values (skip self; explicit params map by position)
    {
      auto arg_it = fn->arg_begin();
      for (const ShaderFnParam &p : sfi.params) {
        if (arg_it == fn->arg_end()) break;
        ectx.sym_values[p.sym_id] = &*arg_it;
        ++arg_it;
      }
    }

    if (sfi.body.body_root != 0 && !sfi.body.nodes.empty())
      ectx.emit(sfi.body.body_root);
    auto *cur_bb = ectx.b.GetInsertBlock();
    if (cur_bb && !cur_bb->getTerminator()) ectx.b.CreateRetVoid();

    out_map[sfi.method_name] = fn;
  }
}

int spirv_emit_stage(const Sidecar &sc, const StageInfo &stage,
                     const char *out_path) {
  size_t shader_idx = find_shader_idx(sc, stage);

  //  set up LLVM module targeting SPIR-V 1.5 / Vulkan 1.2
  llvm::LLVMContext ctx;
  llvm::Module mod("shader", ctx);
  llvm::Triple triple("spirv64-unknown-vulkan1.2");
  mod.setTargetTriple(triple);

  std::string err;
  const llvm::Target *tgt = llvm::TargetRegistry::lookupTarget(triple, err);
  if (!tgt) {
    fprintf(stderr, "spirv_emit_stage: %s\n", err.c_str());
    return -1;
  }
  llvm::TargetOptions target_opts;
  std::unique_ptr<llvm::TargetMachine> tm(
      tgt->createTargetMachine(triple, /*CPU=*/"", /*Features=*/"", target_opts,
                               /*RM=*/std::nullopt, /*CM=*/std::nullopt,
                               llvm::CodeGenOptLevel::Default));
  mod.setDataLayout(tm->createDataLayout());

  IOVarMap io_var_map;
  declare_io_vars(mod, sc, shader_idx, stage.stage_kind, io_var_map);
  declare_descriptor_bindings(mod, stage.body);

  std::unordered_map<std::string, llvm::Function *> shader_fn_map;
  declare_shader_fns(mod, sc, shader_idx, io_var_map, shader_fn_map);

  llvm::Function *entry_fn = nullptr;
  declare_entry_point(mod, &entry_fn, stage.stage_kind, io_var_map);

  if (stage.body.body_root == 0 || stage.body.nodes.empty()) {
    auto *bb = llvm::BasicBlock::Create(ctx, "entry", entry_fn);
    llvm::IRBuilder<> b(ctx);
    b.SetInsertPoint(bb);
    b.CreateRetVoid();
  } else {
    EmitCtx ectx(ctx, mod, entry_fn, stage.body, io_var_map, sc, shader_idx,
                 find_self_sym_id(stage.body));
    ectx.shader_fn_map = shader_fn_map;
    ectx.emit(stage.body.body_root);
    auto *cur_bb = ectx.b.GetInsertBlock();
    if (cur_bb && !cur_bb->getTerminator()) ectx.b.CreateRetVoid();
  }

  llvm::SmallVector<char, 65536> spv_buf;
  llvm::raw_svector_ostream spv_stream(spv_buf);
  llvm::legacy::PassManager pm;
  bool failed = tm->addPassesToEmitFile(pm, spv_stream, nullptr,
                                        llvm::CodeGenFileType::ObjectFile);
  if (failed) {
    fprintf(stderr, "spirv_emit_stage: addPassesToEmitFile failed\n");
    return -1;
  }
  pm.run(mod);

  FILE *fout = fopen(out_path, "wb");
  if (!fout) {
    fprintf(stderr, "spirv_emit_stage: cannot write %s\n", out_path);
    return -1;
  }
  fwrite(spv_buf.data(), 1, spv_buf.size(), fout);
  fclose(fout);
  return 0;
}

int spirv_dump_stage_ir(const Sidecar &sc, const StageInfo &stage) {
  size_t shader_idx = find_shader_idx(sc, stage);

  llvm::LLVMContext ctx;
  llvm::Module mod("shader", ctx);
  llvm::Triple triple("spirv64-unknown-vulkan1.2");
  mod.setTargetTriple(triple);

  std::string err;
  const llvm::Target *tgt = llvm::TargetRegistry::lookupTarget(triple, err);
  if (!tgt) {
    fprintf(stderr, "spirv_dump_stage_ir: %s\n", err.c_str());
    return -1;
  }
  llvm::TargetOptions target_opts;
  std::unique_ptr<llvm::TargetMachine> tm(
      tgt->createTargetMachine(triple, "", "", target_opts, std::nullopt,
                               std::nullopt, llvm::CodeGenOptLevel::Default));
  mod.setDataLayout(tm->createDataLayout());

  IOVarMap io_var_map;
  declare_io_vars(mod, sc, shader_idx, stage.stage_kind, io_var_map);
  declare_descriptor_bindings(mod, stage.body);

  std::unordered_map<std::string, llvm::Function *> shader_fn_map;
  declare_shader_fns(mod, sc, shader_idx, io_var_map, shader_fn_map);

  llvm::Function *entry_fn = nullptr;
  declare_entry_point(mod, &entry_fn, stage.stage_kind, io_var_map);

  if (stage.body.body_root == 0 || stage.body.nodes.empty()) {
    auto *bb = llvm::BasicBlock::Create(ctx, "entry", entry_fn);
    llvm::IRBuilder<> b(ctx);
    b.SetInsertPoint(bb);
    b.CreateRetVoid();
  } else {
    EmitCtx ectx(ctx, mod, entry_fn, stage.body, io_var_map, sc, shader_idx,
                 find_self_sym_id(stage.body));
    ectx.shader_fn_map = shader_fn_map;
    ectx.emit(stage.body.body_root);
    auto *cur_bb = ectx.b.GetInsertBlock();
    if (cur_bb && !cur_bb->getTerminator()) ectx.b.CreateRetVoid();
  }

  mod.print(llvm::outs(), nullptr);
  return 0;
}
