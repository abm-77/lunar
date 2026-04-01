// lower um.shader + arith + scf + memref + vector ops to MLIR spirv.* ops.
// one spirv.module is produced per stage func.func. helper funcs are cloned
// into every spirv.module that references them.
//
// custom um_shader.* ops are lowered via OpRewritePattern; standard dialect
// ops (arith, func, memref, cf, vector) use upstream conversion patterns.

#include <um/shader/shader_spirv_lower.h>
#include <um/shader/um_shader_dialect.h>

#include <mlir/Conversion/ArithToSPIRV/ArithToSPIRV.h>
#include <mlir/Conversion/ControlFlowToSPIRV/ControlFlowToSPIRV.h>
#include <mlir/Conversion/FuncToSPIRV/FuncToSPIRV.h>
#include <mlir/Conversion/MemRefToSPIRV/MemRefToSPIRV.h>
#include <mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h>
#include <mlir/Conversion/VectorToSPIRV/VectorToSPIRV.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/ControlFlow/IR/ControlFlowOps.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/Dialect/Vector/IR/VectorOps.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVDialect.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVTypes.h>
#include <mlir/Dialect/SPIRV/IR/TargetAndABI.h>
#include <mlir/Dialect/SPIRV/Transforms/SPIRVConversion.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/IRMapping.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Transforms/DialectConversion.h>
#include <mlir/Transforms/GreedyPatternRewriteDriver.h>

#include <compiler/frontend/module.h>
#include <compiler/sema/sema.h>
#include <common/interner.h>
#include <runtime/gfx/gfx_draw_packet.h>

#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>

namespace um::shader {


static mlir::Type mlir_type_for(mlir::MLIRContext &ctx, std::string_view name) {
  mlir::Builder b(&ctx);
  if (name == "f32")               return b.getF32Type();
  if (name == "f64")               return b.getF64Type();
  if (name == "i8"  || name == "u8")   return b.getIntegerType(8);
  if (name == "i16" || name == "u16")  return b.getIntegerType(16);
  if (name == "i32" || name == "u32")  return b.getI32Type();
  if (name == "i64" || name == "u64")  return b.getI64Type();
  if (name == "bool")              return b.getI1Type();
  auto f32 = b.getF32Type();
  if (name == "vec2") return mlir::VectorType::get({2}, f32);
  if (name == "vec3") return mlir::VectorType::get({3}, f32);
  if (name == "vec4") return mlir::VectorType::get({4}, f32);
  return {};
}


static mlir::spirv::StorageClass storage_class_for(ShaderFieldKind k) {
  switch (k) {
  case ShaderFieldKind::VsIn:
  case ShaderFieldKind::FsIn:
    return mlir::spirv::StorageClass::Input;
  case ShaderFieldKind::VsOut:
  case ShaderFieldKind::FsOut:
    return mlir::spirv::StorageClass::Output;
  default:
    return mlir::spirv::StorageClass::Private;
  }
}

static std::string find_pod_type_name(const LoadedModule &lm,
                                      Interner &interner,
                                      SymId shader_name, SymId field_name) {
  for (const Decl &d : lm.mod.decls) {
    if (!has(d.flags, DeclFlags::Shader) || d.name != shader_name) continue;
    if (!d.init || d.init >= lm.ir.nodes.kind.size()) continue;
    if (lm.ir.nodes.kind[d.init] != NodeKind::StructType) continue;
    u32 fs = lm.ir.nodes.b[d.init], fc = lm.ir.nodes.c[d.init];
    for (u32 fi = 0; fi < fc; ++fi) {
      SymId fname = lm.ir.nodes.list[fs + fi * 2];
      if (fname != field_name) continue;
      TypeId tid = lm.ir.nodes.list[fs + fi * 2 + 1];
      if (tid < lm.type_ast.kind.size() &&
          lm.type_ast.kind[tid] == TypeKind::Named)
        return std::string(interner.view(lm.type_ast.a[tid]));
    }
    break;
  }
  return {};
}

static std::string find_pod_field_type(const LoadedModule &lm,
                                       Interner &interner,
                                       SymId pod_name, SymId field_name) {
  for (const Decl &d : lm.mod.decls) {
    if (!has(d.flags, DeclFlags::ShaderPod) || d.name != pod_name) continue;
    if (!d.init || d.init >= lm.ir.nodes.kind.size()) continue;
    if (lm.ir.nodes.kind[d.init] != NodeKind::StructType) continue;
    u32 fs = lm.ir.nodes.b[d.init], fc = lm.ir.nodes.c[d.init];
    for (u32 fi = 0; fi < fc; ++fi) {
      SymId fname = lm.ir.nodes.list[fs + fi * 2];
      if (fname != field_name) continue;
      TypeId tid = lm.ir.nodes.list[fs + fi * 2 + 1];
      if (tid < lm.type_ast.kind.size() &&
          lm.type_ast.kind[tid] == TypeKind::Named)
        return std::string(interner.view(lm.type_ast.a[tid]));
    }
    break;
  }
  return {};
}


static mlir::spirv::TargetEnvAttr make_vulkan_target_env(mlir::MLIRContext &ctx) {
  mlir::spirv::Capability caps[] = {
      mlir::spirv::Capability::Shader,
      mlir::spirv::Capability::DrawParameters,
      mlir::spirv::Capability::RuntimeDescriptorArray,
      mlir::spirv::Capability::SampledImageArrayDynamicIndexing,
      mlir::spirv::Capability::StorageBufferArrayDynamicIndexing,
  };
  mlir::spirv::Extension exts[] = {
      mlir::spirv::Extension::SPV_KHR_storage_buffer_storage_class,
      mlir::spirv::Extension::SPV_EXT_descriptor_indexing,
  };
  auto triple = mlir::spirv::VerCapExtAttr::get(
      mlir::spirv::Version::V_1_3, caps, exts, &ctx);
  return mlir::spirv::TargetEnvAttr::get(
      triple, mlir::spirv::getDefaultResourceLimits(&ctx));
}


static mlir::spirv::GlobalVariableOp
declare_io_var(mlir::OpBuilder &b, mlir::Location loc,
               mlir::spirv::ModuleOp spirv_mod,
               std::string_view sym_name, mlir::Type field_type,
               mlir::spirv::StorageClass sc,
               std::optional<uint32_t> location,
               bool is_builtin_position) {
  mlir::OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(spirv_mod.getBody());
  auto ptr_ty = mlir::spirv::PointerType::get(field_type, sc);
  auto var = b.create<mlir::spirv::GlobalVariableOp>(loc, ptr_ty, sym_name,
                                                      mlir::FlatSymbolRefAttr{});
  if (is_builtin_position)
    var->setAttr("built_in",
        mlir::spirv::BuiltInAttr::get(b.getContext(), mlir::spirv::BuiltIn::Position));
  else if (location.has_value())
    var->setAttr("location", b.getI32IntegerAttr(*location));
  return var;
}

static mlir::spirv::GlobalVariableOp
declare_descriptor_var(mlir::OpBuilder &b, mlir::Location loc,
                       mlir::spirv::ModuleOp spirv_mod,
                       std::string_view sym_name,
                       mlir::Type elem_type,
                       mlir::spirv::StorageClass sc,
                       uint32_t binding) {
  mlir::OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(spirv_mod.getBody());
  auto arr_ty = mlir::spirv::RuntimeArrayType::get(elem_type);
  auto ptr_ty = mlir::spirv::PointerType::get(arr_ty, sc);
  auto var = b.create<mlir::spirv::GlobalVariableOp>(loc, ptr_ty, sym_name,
                                                      mlir::FlatSymbolRefAttr{});
  var->setAttr("descriptor_set", b.getI32IntegerAttr(0));
  var->setAttr("binding", b.getI32IntegerAttr(binding));
  return var;
}


struct SpirvLowerCtx {
  mlir::spirv::ModuleOp spirv_mod;
  // "field.sub" → gvar sym name
  std::unordered_map<std::string, std::string> input_vars;
  std::unordered_map<std::string, std::string> output_vars;
  std::optional<std::string> textures_2d_var;
  std::optional<std::string> samplers_var;
  std::optional<std::string> frame_arena_var;
  std::optional<std::string> draw_packets_var;
  std::optional<std::string> draw_index_var;

  static std::string io_key(llvm::StringRef f, llvm::StringRef s) {
    return (f + "." + s).str();
  }
};

// these use OpConversionPattern so they participate in the type conversion
// framework — operands are automatically remapped to converted types.

static mlir::Value addr_load(mlir::ConversionPatternRewriter &rw,
                             mlir::Location loc,
                             mlir::spirv::ModuleOp spirv_mod,
                             const std::string &var_name) {
  auto gvar = mlir::SymbolTable::lookupSymbolIn(spirv_mod, var_name);
  if (!gvar) return {};
  auto ptr_ty = mlir::cast<mlir::spirv::GlobalVariableOp>(gvar).getType();
  auto sym_ref = mlir::SymbolRefAttr::get(rw.getContext(), var_name);
  auto addr = rw.create<mlir::spirv::AddressOfOp>(loc, ptr_ty, sym_ref);
  return rw.create<mlir::spirv::LoadOp>(loc, addr.getPointer()).getValue();
}

static mlir::Value array_index_load(mlir::ConversionPatternRewriter &rw,
                                    mlir::Location loc,
                                    mlir::spirv::ModuleOp spirv_mod,
                                    const std::string &var_name,
                                    mlir::Value index,
                                    mlir::spirv::StorageClass sc) {
  auto gvar = mlir::SymbolTable::lookupSymbolIn(spirv_mod, var_name);
  if (!gvar) return {};
  auto arr_ptr_ty = mlir::cast<mlir::spirv::GlobalVariableOp>(gvar).getType();
  auto arr_ty = mlir::cast<mlir::spirv::PointerType>(arr_ptr_ty).getPointeeType();
  auto elem_ty = mlir::cast<mlir::spirv::RuntimeArrayType>(arr_ty).getElementType();
  auto elem_ptr_ty = mlir::spirv::PointerType::get(elem_ty, sc);
  auto sym_ref = mlir::SymbolRefAttr::get(rw.getContext(), var_name);
  auto arr_addr = rw.create<mlir::spirv::AddressOfOp>(loc, arr_ptr_ty, sym_ref);
  auto ptr = rw.create<mlir::spirv::AccessChainOp>(
      loc, elem_ptr_ty, arr_addr.getPointer(), mlir::ValueRange{index});
  return rw.create<mlir::spirv::LoadOp>(loc, ptr.getComponentPtr()).getValue();
}

struct LoadInputPattern : public mlir::OpConversionPattern<LoadInputOp> {
  SpirvLowerCtx &lctx;
  LoadInputPattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(LoadInputOp op, OpAdaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    auto key = SpirvLowerCtx::io_key(op.getFieldName(), op.getSubFieldName());
    auto it = lctx.input_vars.find(key);
    if (it == lctx.input_vars.end()) return mlir::failure();
    auto val = addr_load(rw, op.getLoc(), lctx.spirv_mod, it->second);
    if (!val) return mlir::failure();
    rw.replaceOp(op, val);
    return mlir::success();
  }
};

struct StoreOutputPattern : public mlir::OpConversionPattern<StoreOutputOp> {
  SpirvLowerCtx &lctx;
  StoreOutputPattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(StoreOutputOp op, OpAdaptor adaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    auto key = SpirvLowerCtx::io_key(op.getFieldName(), op.getSubFieldName());
    auto it = lctx.output_vars.find(key);
    if (it == lctx.output_vars.end()) return mlir::failure();
    auto gvar = mlir::SymbolTable::lookupSymbolIn(lctx.spirv_mod, it->second);
    if (!gvar) return mlir::failure();
    auto ptr_ty = mlir::cast<mlir::spirv::GlobalVariableOp>(gvar).getType();
    auto sym_ref = mlir::SymbolRefAttr::get(rw.getContext(), it->second);
    auto addr = rw.create<mlir::spirv::AddressOfOp>(op.getLoc(), ptr_ty, sym_ref);
    rw.create<mlir::spirv::StoreOp>(op.getLoc(), addr.getPointer(), adaptor.getValue());
    rw.eraseOp(op);
    return mlir::success();
  }
};

struct Texture2dPattern : public mlir::OpConversionPattern<Texture2dOp> {
  SpirvLowerCtx &lctx;
  Texture2dPattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(Texture2dOp op, OpAdaptor adaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    if (!lctx.textures_2d_var) return mlir::failure();
    auto val = array_index_load(rw, op.getLoc(), lctx.spirv_mod,
        *lctx.textures_2d_var, adaptor.getIndex(),
        mlir::spirv::StorageClass::UniformConstant);
    if (!val) return mlir::failure();
    rw.replaceOp(op, val);
    return mlir::success();
  }
};

struct SamplerPattern : public mlir::OpConversionPattern<SamplerOp> {
  SpirvLowerCtx &lctx;
  SamplerPattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(SamplerOp op, OpAdaptor adaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    if (!lctx.samplers_var) return mlir::failure();
    auto val = array_index_load(rw, op.getLoc(), lctx.spirv_mod,
        *lctx.samplers_var, adaptor.getIndex(),
        mlir::spirv::StorageClass::UniformConstant);
    if (!val) return mlir::failure();
    rw.replaceOp(op, val);
    return mlir::success();
  }
};

struct SamplePattern : public mlir::OpConversionPattern<SampleOp> {
  SpirvLowerCtx &lctx;
  SamplePattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(SampleOp op, OpAdaptor adaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    auto loc = op.getLoc();
    auto f32 = mlir::Float32Type::get(rw.getContext());
    auto vec4_ty = mlir::VectorType::get({4}, f32);
    // the texture operand is now a spirv.image type (after type conversion)
    auto img_ty = adaptor.getTexture().getType();
    auto sampled_img_ty = mlir::spirv::SampledImageType::get(img_ty);
    auto combined = rw.create<CombineSampledImageOp>(
        loc, sampled_img_ty, adaptor.getTexture(), adaptor.getSampler());
    auto sampled = rw.create<mlir::spirv::ImageSampleImplicitLodOp>(
        loc, vec4_ty, combined.getResult(), adaptor.getUv(),
        mlir::spirv::ImageOperandsAttr{}, mlir::ValueRange{});
    rw.replaceOp(op, sampled.getResult());
    return mlir::success();
  }
};

struct DrawIdPattern : public mlir::OpConversionPattern<DrawIdOp> {
  SpirvLowerCtx &lctx;
  DrawIdPattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(DrawIdOp op, OpAdaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    auto loc = op.getLoc();
    if (!lctx.draw_index_var) {
      mlir::OpBuilder::InsertionGuard guard(rw);
      rw.setInsertionPointToStart(lctx.spirv_mod.getBody());
      auto i32_ty = mlir::IntegerType::get(rw.getContext(), 32);
      auto ptr_ty = mlir::spirv::PointerType::get(i32_ty, mlir::spirv::StorageClass::Input);
      auto var = rw.create<mlir::spirv::GlobalVariableOp>(
          loc, ptr_ty, "__draw_index", mlir::FlatSymbolRefAttr{});
      var->setAttr("built_in",
          mlir::spirv::BuiltInAttr::get(rw.getContext(), mlir::spirv::BuiltIn::DrawIndex));
      lctx.draw_index_var = "__draw_index";
    }
    auto val = addr_load(rw, loc, lctx.spirv_mod, *lctx.draw_index_var);
    if (!val) return mlir::failure();
    rw.replaceOp(op, val);
    return mlir::success();
  }
};

struct DrawPacketPattern : public mlir::OpConversionPattern<DrawPacketOp> {
  SpirvLowerCtx &lctx;
  DrawPacketPattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(DrawPacketOp op, OpAdaptor adaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    if (!lctx.draw_packets_var) return mlir::failure();
    auto loc = op.getLoc();
    auto gvar = mlir::SymbolTable::lookupSymbolIn(lctx.spirv_mod, *lctx.draw_packets_var);
    if (!gvar) return mlir::failure();
    auto arr_ptr_ty = mlir::cast<mlir::spirv::GlobalVariableOp>(gvar).getType();
    auto arr_ty = mlir::cast<mlir::spirv::PointerType>(arr_ptr_ty).getPointeeType();
    auto pkt_ty = mlir::cast<mlir::spirv::RuntimeArrayType>(arr_ty).getElementType();
    auto pkt_ptr_ty = mlir::spirv::PointerType::get(pkt_ty, mlir::spirv::StorageClass::StorageBuffer);
    auto sym_ref = mlir::SymbolRefAttr::get(rw.getContext(), *lctx.draw_packets_var);
    auto arr_addr = rw.create<mlir::spirv::AddressOfOp>(loc, arr_ptr_ty, sym_ref);
    auto elem_ptr = rw.create<mlir::spirv::AccessChainOp>(
        loc, pkt_ptr_ty, arr_addr.getPointer(), mlir::ValueRange{adaptor.getDrawId()});
    rw.replaceOp(op, elem_ptr.getComponentPtr());
    return mlir::success();
  }
};

struct DrawPacketFieldPattern : public mlir::OpConversionPattern<DrawPacketFieldOp> {
  SpirvLowerCtx &lctx;
  DrawPacketFieldPattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(DrawPacketFieldOp op, OpAdaptor adaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    if (!lctx.draw_packets_var) return mlir::failure();
    int fidx = draw_packet_field_index(op.getFieldName().str().c_str());
    if (fidx < 0) return mlir::failure();
    auto loc = op.getLoc();
    auto i32_ty = mlir::IntegerType::get(rw.getContext(), 32);
    auto field_ty = draw_packet_field_is_u64(fidx)
        ? mlir::IntegerType::get(rw.getContext(), 64) : i32_ty;
    auto field_ptr_ty = mlir::spirv::PointerType::get(field_ty, mlir::spirv::StorageClass::StorageBuffer);
    auto idx_val = rw.create<mlir::arith::ConstantOp>(loc, rw.getI32IntegerAttr(fidx)).getResult();
    auto field_ptr = rw.create<mlir::spirv::AccessChainOp>(
        loc, field_ptr_ty, adaptor.getPacket(), mlir::ValueRange{idx_val});
    auto loaded = rw.create<mlir::spirv::LoadOp>(loc, field_ptr.getComponentPtr());
    mlir::Value result = loaded.getValue();
    if (draw_packet_field_is_u64(fidx) && op.getResult().getType() != field_ty)
      result = rw.create<mlir::arith::TruncIOp>(loc, i32_ty, result).getResult();
    rw.replaceOp(op, result);
    return mlir::success();
  }
};

struct FrameReadPattern : public mlir::OpConversionPattern<FrameReadOp> {
  SpirvLowerCtx &lctx;
  FrameReadPattern(const mlir::TypeConverter &tc, mlir::MLIRContext *ctx, SpirvLowerCtx &lctx)
      : OpConversionPattern(tc, ctx), lctx(lctx) {}
  mlir::LogicalResult matchAndRewrite(FrameReadOp op, OpAdaptor adaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    if (!lctx.frame_arena_var) return mlir::failure();
    auto loc = op.getLoc();
    auto &ctx = *rw.getContext();
    auto sym_ref = mlir::SymbolRefAttr::get(&ctx, *lctx.frame_arena_var);
    auto gvar = mlir::SymbolTable::lookupSymbolIn(lctx.spirv_mod, *lctx.frame_arena_var);
    if (!gvar) return mlir::failure();
    auto arr_ptr_ty = mlir::cast<mlir::spirv::GlobalVariableOp>(gvar).getType();
    auto i8_ptr_ty = mlir::spirv::PointerType::get(
        mlir::IntegerType::get(&ctx, 8), mlir::spirv::StorageClass::StorageBuffer);
    auto arr_addr = rw.create<mlir::spirv::AddressOfOp>(loc, arr_ptr_ty, sym_ref);
    auto byte_ptr = rw.create<mlir::spirv::AccessChainOp>(
        loc, i8_ptr_ty, arr_addr.getPointer(), mlir::ValueRange{adaptor.getOffset()});
    auto i32_ty = mlir::IntegerType::get(&ctx, 32);
    auto i32_ptr_ty = mlir::spirv::PointerType::get(i32_ty, mlir::spirv::StorageClass::StorageBuffer);
    auto cast_ptr = rw.create<mlir::spirv::BitcastOp>(loc, i32_ptr_ty, byte_ptr.getComponentPtr());
    auto raw = rw.create<mlir::spirv::LoadOp>(loc, cast_ptr.getResult());
    mlir::Value result = raw.getValue();
    auto req_ty = op.getElementType();
    if (req_ty != i32_ty)
      result = rw.create<mlir::spirv::BitcastOp>(loc, req_ty, result).getResult();
    rw.replaceOp(op, result);
    return mlir::success();
  }
};

// convert func.func → spirv.func (MLIR's FuncToSPIRV only handles return/call)
struct FuncOpPattern : public mlir::OpConversionPattern<mlir::func::FuncOp> {
  using OpConversionPattern::OpConversionPattern;

  mlir::LogicalResult matchAndRewrite(mlir::func::FuncOp funcOp,
      OpAdaptor adaptor,
      mlir::ConversionPatternRewriter &rw) const override {
    auto fnType = funcOp.getFunctionType();
    mlir::TypeConverter::SignatureConversion signatureConv(fnType.getNumInputs());
    auto *tc = getTypeConverter();
    for (auto [i, ty] : llvm::enumerate(fnType.getInputs())) {
      auto converted = tc->convertType(ty);
      if (!converted) return mlir::failure();
      signatureConv.addInputs(i, converted);
    }
    llvm::SmallVector<mlir::Type> result_types;
    if (tc->convertTypes(fnType.getResults(), result_types).failed())
      return mlir::failure();

    auto spvFnType = mlir::FunctionType::get(
        rw.getContext(), signatureConv.getConvertedTypes(), result_types);
    auto spvFunc = rw.create<mlir::spirv::FuncOp>(
        funcOp.getLoc(), funcOp.getName(), spvFnType);
    rw.inlineRegionBefore(funcOp.getBody(), spvFunc.getBody(), spvFunc.end());
    if (mlir::failed(rw.convertRegionTypes(&spvFunc.getBody(), *tc, &signatureConv)))
      return mlir::failure();
    rw.eraseOp(funcOp);
    return mlir::success();
  }
};

static void populateUmShaderToSPIRVPatterns(const mlir::TypeConverter &tc,
                                             mlir::RewritePatternSet &patterns,
                                             SpirvLowerCtx &lctx) {
  auto *ctx = patterns.getContext();
  patterns.add<LoadInputPattern>(tc, ctx, lctx);
  patterns.add<StoreOutputPattern>(tc, ctx, lctx);
  patterns.add<Texture2dPattern>(tc, ctx, lctx);
  patterns.add<SamplerPattern>(tc, ctx, lctx);
  patterns.add<SamplePattern>(tc, ctx, lctx);
  patterns.add<DrawIdPattern>(tc, ctx, lctx);
  patterns.add<DrawPacketPattern>(tc, ctx, lctx);
  patterns.add<DrawPacketFieldPattern>(tc, ctx, lctx);
  patterns.add<FrameReadPattern>(tc, ctx, lctx);
}


static int find_module_for_shader(std::string_view shader_type_name,
                                  std::span<const LoadedModule> modules,
                                  Interner &interner) {
  for (int mi = 0; mi < (int)modules.size(); ++mi)
    for (const ShaderStageInfo &si : modules[mi].mod.shader_stages)
      if (interner.view(si.shader_type) == shader_type_name)
        return mi;
  return -1;
}

static SpirvLowerCtx
build_lower_ctx(mlir::OpBuilder &b, mlir::Location loc,
                mlir::spirv::ModuleOp spirv_mod,
                mlir::func::FuncOp fn,
                std::string_view shader_type_name,
                std::string_view stage,
                std::span<const LoadedModule> modules,
                const SemaResult &sema,
                Interner &interner) {
  SpirvLowerCtx lctx;
  lctx.spirv_mod = spirv_mod;

  int mi = find_module_for_shader(shader_type_name, modules, interner);
  if (mi < 0) return lctx;
  const LoadedModule &lm = modules[mi];
  bool is_vertex = (stage == "vertex");

  SymId shader_sym = interner.intern(shader_type_name);

  // declare IO globals from @shader field annotations
  for (const ShaderFieldAnnot &sfa : lm.mod.shader_field_annots) {
    if (sfa.struct_name != shader_sym) continue;
    bool is_input = (sfa.kind == ShaderFieldKind::VsIn ||
                     sfa.kind == ShaderFieldKind::FsIn);
    bool is_output = (sfa.kind == ShaderFieldKind::VsOut ||
                      sfa.kind == ShaderFieldKind::FsOut);
    if (!is_input && !is_output) continue;
    if (is_vertex && (sfa.kind == ShaderFieldKind::FsIn ||
                      sfa.kind == ShaderFieldKind::FsOut)) continue;
    if (!is_vertex && (sfa.kind == ShaderFieldKind::VsIn ||
                       sfa.kind == ShaderFieldKind::VsOut)) continue;

    auto sc = storage_class_for(sfa.kind);
    std::string field_name(interner.view(sfa.field_name));
    std::string pod_type_name = find_pod_type_name(lm, interner,
                                                    sfa.struct_name, sfa.field_name);
    if (pod_type_name.empty()) continue;
    SymId pod_sym = interner.intern(pod_type_name);

    for (const IOFieldAnnot &ifa : lm.mod.io_field_annots) {
      if (ifa.struct_name != pod_sym) continue;
      std::string sub_name(interner.view(ifa.field_name));
      std::string type_name = find_pod_field_type(lm, interner,
                                                   pod_sym, ifa.field_name);
      if (type_name.empty()) continue;
      mlir::Type ft = mlir_type_for(*b.getContext(), type_name);
      if (!ft) continue;

      std::string var_sym = field_name + "_" + sub_name;
      bool is_pos = (ifa.kind == IOAnnotKind::BuiltinPosition);
      std::optional<uint32_t> loc_idx;
      if (ifa.kind == IOAnnotKind::Location)
        loc_idx = ifa.location_index;

      declare_io_var(b, loc, spirv_mod, var_sym, ft, sc, loc_idx, is_pos);

      auto key = SpirvLowerCtx::io_key(field_name, sub_name);
      if (is_input)
        lctx.input_vars[key] = var_sym;
      else
        lctx.output_vars[key] = var_sym;
    }
  }

  // scan for used descriptor ops and declare globals
  bool need_tex = false, need_samp = false, need_frame = false, need_pkt = false;
  fn.walk([&](mlir::Operation *op) {
    if (mlir::isa<Texture2dOp>(op)) need_tex = true;
    if (mlir::isa<SamplerOp>(op)) need_samp = true;
    if (mlir::isa<FrameReadOp>(op)) need_frame = true;
    if (mlir::isa<DrawPacketOp>(op)) need_pkt = true;
  });

  auto &ctx = *b.getContext();
  if (need_tex) {
    auto img_ty = mlir::spirv::ImageType::get(
        mlir::Float32Type::get(&ctx),
        mlir::spirv::Dim::Dim2D,
        mlir::spirv::ImageDepthInfo::DepthUnknown,
        mlir::spirv::ImageArrayedInfo::NonArrayed,
        mlir::spirv::ImageSamplingInfo::SingleSampled,
        mlir::spirv::ImageSamplerUseInfo::NeedSampler,
        mlir::spirv::ImageFormat::Unknown);
    declare_descriptor_var(b, loc, spirv_mod, "textures_2d", img_ty,
                           mlir::spirv::StorageClass::UniformConstant, 0);
    lctx.textures_2d_var = "textures_2d";
  }
  if (need_samp) {
    // MLIR's SPIR-V dialect lacks OpTypeSampler; use i32 as a placeholder.
    // the custom serialization pass emits the correct SPIR-V sampler type.
    auto samp_elem_ty = mlir::IntegerType::get(&ctx, 32);
    declare_descriptor_var(b, loc, spirv_mod, "samplers", samp_elem_ty,
                           mlir::spirv::StorageClass::UniformConstant, 1);
    lctx.samplers_var = "samplers";
  }
  if (need_frame) {
    auto i8_ty = mlir::IntegerType::get(&ctx, 8);
    declare_descriptor_var(b, loc, spirv_mod, "frame_arena", i8_ty,
                           mlir::spirv::StorageClass::StorageBuffer, 2);
    lctx.frame_arena_var = "frame_arena";
  }
  if (need_pkt) {
    // build draw_packet_t struct type from shared field definitions
    auto i64_ty = mlir::IntegerType::get(&ctx, 64);
    auto i32_ty = mlir::IntegerType::get(&ctx, 32);
    llvm::SmallVector<mlir::Type> fields;
    for (uint32_t i = 0; i < kDrawPacketFieldCount; ++i)
      fields.push_back(draw_packet_field_is_u64(i) ? i64_ty : i32_ty);
    auto pkt_ty = mlir::spirv::StructType::get(fields);
    declare_descriptor_var(b, loc, spirv_mod, "draw_packets", pkt_ty,
                           mlir::spirv::StorageClass::StorageBuffer, 3);
    lctx.draw_packets_var = "draw_packets";
  }

  return lctx;
}


bool run_spirv_lower(mlir::MLIRContext &ctx,
                     mlir::ModuleOp mlir_mod,
                     std::span<const LoadedModule> modules,
                     const SemaResult &sema,
                     Interner &interner) {
  ctx.loadDialect<mlir::spirv::SPIRVDialect,
                  mlir::cf::ControlFlowDialect>();

  // lower scf → cf first so we only need cf→spirv patterns
  {
    mlir::RewritePatternSet scf_patterns(&ctx);
    mlir::populateSCFToControlFlowConversionPatterns(scf_patterns);
    mlir::ConversionTarget scf_target(ctx);
    scf_target.addLegalDialect<mlir::cf::ControlFlowDialect,
                               mlir::arith::ArithDialect,
                               mlir::func::FuncDialect,
                               mlir::memref::MemRefDialect,
                               mlir::vector::VectorDialect>();
    scf_target.addLegalDialect<UmShaderDialect>();
    scf_target.addIllegalDialect<mlir::scf::SCFDialect>();
    if (mlir::applyPartialConversion(mlir_mod, scf_target,
                                      std::move(scf_patterns)).failed()) {
      fprintf(stderr, "shader_spirv_lower: scf-to-cf conversion failed\n");
      return false;
    }
  }

  auto target_env = make_vulkan_target_env(ctx);

  llvm::SmallVector<mlir::func::FuncOp> helper_fns;
  llvm::SmallVector<mlir::func::FuncOp> stage_fns;
  for (auto fn : mlir_mod.getOps<mlir::func::FuncOp>()) {
    if (fn->hasAttr("stage"))
      stage_fns.push_back(fn);
    else
      helper_fns.push_back(fn);
  }

  for (mlir::func::FuncOp stage_fn : stage_fns) {
    auto stage_attr = stage_fn->getAttrOfType<mlir::StringAttr>("stage");
    auto shader_type_attr = stage_fn->getAttrOfType<mlir::StringAttr>("shader_type");
    std::string stage = stage_attr.getValue().str();
    std::string shader_type_name = shader_type_attr
        ? shader_type_attr.getValue().str()
        : std::string{};
    std::string fn_name = stage_fn.getName().str();

    auto loc = stage_fn.getLoc();
    mlir::OpBuilder b(stage_fn);

    auto spirv_mod = b.create<mlir::spirv::ModuleOp>(
        loc, mlir::spirv::AddressingModel::Logical,
        mlir::spirv::MemoryModel::GLSL450);
    spirv_mod->setAttr(mlir::spirv::getTargetEnvAttrName(), target_env);
    spirv_mod.setVceTripleAttr(target_env.getTripleAttr());
    // propagate shader metadata for downstream serialization
    spirv_mod->setAttr("shader_type", b.getStringAttr(shader_type_name));
    spirv_mod->setAttr("stage", b.getStringAttr(stage));

    auto lctx = build_lower_ctx(b, loc, spirv_mod, stage_fn,
                                 shader_type_name, stage,
                                 modules, sema, interner);

    // clone needed helpers into the spirv.module
    {
      mlir::OpBuilder::InsertionGuard guard(b);
      b.setInsertionPointToEnd(spirv_mod.getBody());
      stage_fn.walk([&](mlir::func::CallOp call) {
        for (auto &hfn : helper_fns) {
          if (hfn.getName() == call.getCallee()) {
            mlir::IRMapping mapping;
            b.clone(*hfn.getOperation(), mapping);
          }
        }
      });
    }

    // move stage func into spirv.module
    stage_fn->moveBefore(spirv_mod.getBody(),
                          spirv_mod.getBody()->getTerminator()->getIterator());

    // strip custom attrs from func.func before conversion
    spirv_mod.walk([](mlir::func::FuncOp fn) {
      fn->removeAttr("shader_type");
      fn->removeAttr("stage");
    });

    // lower um_shader.* + arith + memref + cf + func + vector → spirv in one pass
    {
      auto target = mlir::SPIRVConversionTarget::get(target_env);
      target->addLegalOp<CombineSampledImageOp>();
      target->addIllegalDialect<mlir::func::FuncDialect,
                                mlir::arith::ArithDialect,
                                mlir::cf::ControlFlowDialect,
                                mlir::vector::VectorDialect,
                                mlir::memref::MemRefDialect>();
      target->addIllegalDialect<UmShaderDialect>();
      target->addLegalOp<CombineSampledImageOp>(); // survives to serialization
      mlir::SPIRVTypeConverter type_conv(target_env);
      // map our opaque types to their SPIR-V equivalents
      type_conv.addConversion([](TextureType t) -> mlir::Type {
        return mlir::spirv::ImageType::get(
            mlir::Float32Type::get(t.getContext()),
            mlir::spirv::Dim::Dim2D,
            mlir::spirv::ImageDepthInfo::DepthUnknown,
            mlir::spirv::ImageArrayedInfo::NonArrayed,
            mlir::spirv::ImageSamplingInfo::SingleSampled,
            mlir::spirv::ImageSamplerUseInfo::NeedSampler,
            mlir::spirv::ImageFormat::Unknown);
      });
      type_conv.addConversion([](SamplerType t) -> mlir::Type {
        // MLIR SPIR-V has no OpTypeSampler; use i32 as placeholder
        return mlir::IntegerType::get(t.getContext(), 32);
      });
      type_conv.addConversion([](DrawPacketType t) -> mlir::Type {
        // draw packet pointer becomes an i32 (SSBO index placeholder)
        return mlir::IntegerType::get(t.getContext(), 32);
      });

      mlir::RewritePatternSet patterns(&ctx);
      populateUmShaderToSPIRVPatterns(type_conv, patterns, lctx);
      mlir::arith::populateArithToSPIRVPatterns(type_conv, patterns);
      mlir::populateFuncToSPIRVPatterns(type_conv, patterns);
      patterns.add<FuncOpPattern>(type_conv, &ctx);
      mlir::populateMemRefToSPIRVPatterns(type_conv, patterns);
      mlir::cf::populateControlFlowToSPIRVPatterns(type_conv, patterns);
      mlir::populateVectorToSPIRVPatterns(type_conv, patterns);

      if (mlir::applyPartialConversion(spirv_mod, *target, std::move(patterns))
              .failed()) {
        spirv_mod->print(llvm::errs());
        llvm::errs() << "\n";
        fprintf(stderr, "shader_spirv_lower: conversion failed for %s\n",
                fn_name.c_str());
        return false;
      }
    }

    // ensure all GlobalVariables come before functions (serializer requires this)
    {
      llvm::SmallVector<mlir::Operation *> globals;
      spirv_mod.walk([&](mlir::spirv::GlobalVariableOp gv) {
        globals.push_back(gv.getOperation());
      });
      for (auto *gv : globals)
        gv->moveBefore(spirv_mod.getBody(), spirv_mod.getBody()->begin());
    }

    // find the converted spirv.func and add entry point + execution mode
    mlir::spirv::FuncOp spv_fn;
    spirv_mod.walk([&](mlir::spirv::FuncOp f) {
      if (f.getName() == fn_name) spv_fn = f;
    });
    if (!spv_fn) {
      fprintf(stderr, "shader_spirv_lower: spirv.func not found for %s\n",
              fn_name.c_str());
      return false;
    }

    mlir::OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToEnd(spirv_mod.getBody());

    llvm::SmallVector<mlir::Attribute> iface_refs;
    for (auto &[key, vname] : lctx.input_vars)
      iface_refs.push_back(mlir::SymbolRefAttr::get(&ctx, vname));
    for (auto &[key, vname] : lctx.output_vars)
      iface_refs.push_back(mlir::SymbolRefAttr::get(&ctx, vname));
    if (lctx.draw_index_var)
      iface_refs.push_back(mlir::SymbolRefAttr::get(&ctx, *lctx.draw_index_var));

    auto exec_model = (stage == "vertex")
                          ? mlir::spirv::ExecutionModel::Vertex
                          : mlir::spirv::ExecutionModel::Fragment;
    b.create<mlir::spirv::EntryPointOp>(
        loc, exec_model,
        llvm::StringRef(fn_name),
        mlir::ArrayAttr::get(&ctx, iface_refs));

    if (stage == "fragment") {
      b.create<mlir::spirv::ExecutionModeOp>(
          loc, llvm::StringRef(fn_name),
          mlir::spirv::ExecutionMode::OriginUpperLeft,
          mlir::ArrayAttr::get(&ctx, {}));
    }
  }

  for (auto &hfn : helper_fns)
    hfn.erase();

  return true;
}

} // namespace um::shader
